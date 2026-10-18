/*
metal_device.cpp - Metal device layer.

Derived from the ioq3 Metal renderer's device layer (aramfingal/ioq3
code/renderermetal/tr_metal_device.cpp). See metal_device.h and
docs/metal/DEVICE_LAYER_CHANGES.md.

Reference counting: metal-cpp objects from new... or alloc are owned (+1); everything else is
autoreleased and is retained here when it is kept across calls.
*/

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <unordered_map>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <string.h>
#include <limits.h>

#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <CoreGraphics/CGGeometry.h>
#include <CoreGraphics/CGDirectDisplay.h>

#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <QuartzCore/CAMetalLayer.hpp>

#include "metal_host.h"
#include "metal_device.h"

// ============================================================================
// Device context
// ============================================================================

static MTL::Device                *s_device = nullptr;
static MTL::CommandQueue          *s_queue = nullptr;
static CA::MetalLayer             *s_layer = nullptr;
static const MTL::PixelFormat      s_colorPixelFormat = MTL::PixelFormatBGRA8Unorm;
static const MTL::PixelFormat      s_depthPixelFormat = MTL::PixelFormatDepth32Float_Stencil8;

// The scene is rendered into these and copied (flipped) to the drawable when presenting.
static MTL::Texture               *s_sceneColor = nullptr;
static MTL::Texture               *s_sceneDepth = nullptr;
// With multisampling the pass renders into these and resolves into the two above whenever
// it ends, so copies, readback and present always see single-sample textures.
static MTL::Texture               *s_msaaColor = nullptr;
static MTL::Texture               *s_msaaDepth = nullptr;
static int                         s_sampleCount = 1;
static int                         s_width = 0;
static int                         s_height = 0;

// Per-frame state
static CA::MetalDrawable          *s_drawable = nullptr;
static MTL::CommandBuffer         *s_cmdBuffer = nullptr;
static MTL::RenderCommandEncoder  *s_encoder = nullptr;
static bool                        s_frameBegun = false;
static bool                        s_frameCleared = false;	// the scene pass has been opened once this frame
// r_metalPostProcessSingleSample: the main view's _currentRender copy ends multisampling for the
// rest of the frame (see GLMetal_EndMultisamplingAtNextCopy)
static bool                        s_endMsaaAtNextCopy = false;
static bool                        s_msaaEnded = false;
static const uint32_t              PIPELINE_SINGLE_SAMPLE = 1u << 24;	// in a pipeline key's blendKey
static bool                        s_frameHasWork = false;	// the frame command buffer has encoded work
static bool                        s_firstFrame = true;
static uint64_t                    s_frameSerial = 0;
static uint64_t                    s_lastPresentNs = 0;

static dispatch_semaphore_t        s_inFlightSemaphore = nullptr;
static bool                        s_inFlightFramePendingSignal = false;
static const long                  s_maxFramesInFlight = 3;
static int                         s_appliedSwapInterval = INT_MIN;

static MTL::RenderPipelineState   *s_presentPipeline = nullptr;
static MTL::SamplerState          *s_presentSampler = nullptr;

// Readback
static MTL::Texture               *s_captureTexture = nullptr;
static MTL::Buffer                *s_captureBuffer = nullptr;	// depth / stencil readback and depth copies
static size_t                      s_captureBufferSize = 0;

static inline void GLMetal_HashCombine( size_t &seed, size_t value ) {
	seed ^= value + (size_t)0x9e3779b97f4a7c15ull + ( seed << 6 ) + ( seed >> 2 );
}

// ---------------------------------------------------------------------------
// Encoder-local state cache: what has actually been set on s_encoder since it was opened.
// Lets the per-draw path skip redundant Objective-C encoder calls.
// ---------------------------------------------------------------------------
static const int METAL_MAX_TEXTURE_BINDINGS = 16;
static const int METAL_MAX_BUFFER_SLOTS = 31;
struct MetalEncoderStateCache {
	MTL::RenderPipelineState *pipeline;
	MTL::DepthStencilState *depthStencil;
	uint32_t stencilReference;
	bool stencilReferenceValid;
	int cullMode;					// -1 = unknown
	int fillMode;
	bool viewportValid;
	MTL::Viewport viewport;
	bool scissorValid;
	MTL::ScissorRect scissor;
	bool depthBiasValid;
	float depthBias, slopeScale, depthBiasClamp;
	MTL::Texture *fragmentTexture[METAL_MAX_TEXTURE_BINDINGS];
	MTL::SamplerState *fragmentSampler[METAL_MAX_TEXTURE_BINDINGS];
	MTL::Buffer *vertexBuffer[METAL_MAX_BUFFER_SLOTS];
	NS::UInteger vertexOffset[METAL_MAX_BUFFER_SLOTS];
	MTL::Buffer *fragmentBuffer[METAL_MAX_BUFFER_SLOTS];
	NS::UInteger fragmentOffset[METAL_MAX_BUFFER_SLOTS];
};
static MetalEncoderStateCache s_encState;
static uint32_t s_encoderGeneration = 0;
uint32_t GLMetal_EncoderGeneration( void ) { return s_encoderGeneration; }

// State the host has asked for; applied to every encoder that is opened.
static struct {
	MTL::Viewport viewport;
	MTL::ScissorRect scissor;
	MTL::CullMode cullMode;
	MTL::TriangleFillMode fillMode;
	float depthBias, slopeScale, depthBiasClamp;
	MTL::DepthStencilState *depthStencil;
	uint32_t stencilRef;
} s_want;

// ---------------------------------------------------------------------------
// Caches
// ---------------------------------------------------------------------------
struct MetalProgramRecord {
	std::string name;
	MTL::Function *vert;
	MTL::Function *frag;
};
static std::vector<MetalProgramRecord> s_programs;

struct PipelineCacheKey {
	int programId;
	uint32_t blendKey;
	uint64_t layoutHash;
	bool operator==( const PipelineCacheKey &o ) const {
		return programId == o.programId && blendKey == o.blendKey && layoutHash == o.layoutHash;
	}
};
struct PipelineCacheKeyHash {
	size_t operator()( const PipelineCacheKey &key ) const {
		size_t h = std::hash<int>{}( key.programId );
		GLMetal_HashCombine( h, std::hash<uint32_t>{}( key.blendKey ) );
		GLMetal_HashCombine( h, std::hash<uint64_t>{}( key.layoutHash ) );
		return h;
	}
};
// nullptr = creation failed (not retried)
static std::unordered_map<PipelineCacheKey, MTL::RenderPipelineState *, PipelineCacheKeyHash> s_pipelineCache;
static PipelineCacheKey s_lastPipelineKey = { -1, 0, 0 };
static MTL::RenderPipelineState *s_lastPipeline = nullptr;

// depth capture by replay (see GLMetal_RecordDepthDraw)
static bool s_depthRecordValid = false;		// this frame's depth-writing draws are all in s_depthDraws
static bool s_depthCurWrite = false;		// the depth/stencil state last set writes depth
static PipelineCacheKey s_curPipelineKey = { -1, 0, 0 };	// of the last GLMetal_GetPipeline
static MetalVertexLayout s_curPipelineLayout;

static std::unordered_map<uint64_t, MTL::DepthStencilState *> s_depthStencilCache;
static uint64_t s_lastDepthStencilKey = ~0ull;
static MTL::DepthStencilState *s_lastDepthStencilState = nullptr;

static std::unordered_map<uint64_t, MTL::SamplerState *> s_samplerCache;

// ---------------------------------------------------------------------------
// Per-frame transient memory arenas (vertex/index/uniform streaming).
// One arena per in-flight frame slot; the in-flight semaphore guarantees the GPU has finished
// with an arena before GLMetal_BeginFrame hands it out again.
// ---------------------------------------------------------------------------
struct MetalFrameArenaChunk {
	MTL::Buffer *buffer;
	uint8_t *cpu;
	size_t size;
	size_t used;
};
struct MetalFrameArena {
	std::vector<MetalFrameArenaChunk> chunks;
	size_t current;
};
static const size_t kFrameArenaChunkSize = 4u * 1024u * 1024u;
static MetalFrameArena s_frameArenas[s_maxFramesInFlight];
static int s_frameArenaIndex = 0;

// ---------------------------------------------------------------------------
// Texture upload batching. While no frame has encoded work, texture contents are staged
// through shared buffers and copied by one blit command buffer that is committed ahead of
// the frame command buffer (same queue => ordered). Once the frame has work, uploads are
// encoded into the frame command buffer itself so they stay ordered with the draws.
// ---------------------------------------------------------------------------
static MTL::CommandBuffer *s_uploadCmdBuffer = nullptr;
static MTL::BlitCommandEncoder *s_uploadBlit = nullptr;
static size_t s_uploadPendingBytes = 0;
static const size_t kUploadFlushThreshold = 64u * 1024u * 1024u;

static const char *MetalErrorString( NS::Error *error ) {
	if ( !error ) {
		return "unknown error";
	}
	NS::String *desc = error->localizedDescription();
	if ( !desc ) {
		return "unknown error";
	}
	return desc->utf8String();
}

static uint64_t GLMetal_NowNs( void ) {
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch() ).count();
}

MTL::Device *GLMetal_Device( void ) { return s_device; }
MTL::PixelFormat GLMetal_SceneColorFormat( void ) { return s_colorPixelFormat; }
MTL::PixelFormat GLMetal_SceneDepthFormat( void ) { return s_depthPixelFormat; }
MTL::Texture *GLMetal_SceneColorTexture( void ) { return s_sceneColor; }
MTL::Texture *GLMetal_SceneDepthTexture( void ) { return s_sceneDepth; }
uint64_t GLMetal_FrameSerial( void ) { return s_frameSerial; }

void GLMetal_GetSceneSize( int *width, int *height ) {
	if ( width ) {
		*width = s_width;
	}
	if ( height ) {
		*height = s_height;
	}
}

// ============================================================================
// Encoder state
// ============================================================================

// Called every time s_encoder is (re)created: nothing has been set on the new encoder yet.
static void GLMetal_OnEncoderOpened( void ) {
	memset( &s_encState, 0, sizeof( s_encState ) );
	s_encState.cullMode = -1;
	s_encState.fillMode = -1;
	s_encoderGeneration++;
	if ( !s_encoder ) {
		return;
	}
	// The scene is rendered upside down (see docs/metal/DESIGN.md), which turns GL's
	// counter-clockwise front faces into clockwise ones.
	s_encoder->setFrontFacingWinding( MTL::WindingClockwise );

	s_encoder->setViewport( s_want.viewport );
	s_encState.viewport = s_want.viewport;
	s_encState.viewportValid = true;
	s_encoder->setScissorRect( s_want.scissor );
	s_encState.scissor = s_want.scissor;
	s_encState.scissorValid = true;
	s_encoder->setCullMode( s_want.cullMode );
	s_encState.cullMode = (int)s_want.cullMode;
	s_encoder->setTriangleFillMode( s_want.fillMode );
	s_encState.fillMode = (int)s_want.fillMode;
	s_encoder->setDepthBias( s_want.depthBias, s_want.slopeScale, s_want.depthBiasClamp );
	s_encState.depthBias = s_want.depthBias;
	s_encState.slopeScale = s_want.slopeScale;
	s_encState.depthBiasClamp = s_want.depthBiasClamp;
	s_encState.depthBiasValid = true;
	if ( s_want.depthStencil ) {
		s_encoder->setDepthStencilState( s_want.depthStencil );
		s_encState.depthStencil = s_want.depthStencil;
	}
	s_encoder->setStencilReferenceValue( s_want.stencilRef );
	s_encState.stencilReference = s_want.stencilRef;
	s_encState.stencilReferenceValid = true;
}

void GLMetal_SetRenderPipelineState( MTL::RenderPipelineState *pso ) {
	if ( !s_encoder || !pso || s_encState.pipeline == pso ) {
		return;
	}
	s_encoder->setRenderPipelineState( pso );
	s_encState.pipeline = pso;
}

void GLMetal_SetVertexBuffer( MTL::Buffer *buffer, NS::UInteger offset, NS::UInteger slot ) {
	if ( !s_encoder || !buffer ) {
		return;
	}
	if ( slot >= (NS::UInteger)METAL_MAX_BUFFER_SLOTS ) {
		s_encoder->setVertexBuffer( buffer, offset, slot );
		return;
	}
	if ( s_encState.vertexBuffer[slot] == buffer ) {
		if ( s_encState.vertexOffset[slot] != offset ) {
			s_encoder->setVertexBufferOffset( offset, slot );
			s_encState.vertexOffset[slot] = offset;
		}
		return;
	}
	s_encoder->setVertexBuffer( buffer, offset, slot );
	s_encState.vertexBuffer[slot] = buffer;
	s_encState.vertexOffset[slot] = offset;
}

void GLMetal_SetFragmentBuffer( MTL::Buffer *buffer, NS::UInteger offset, NS::UInteger slot ) {
	if ( !s_encoder || !buffer ) {
		return;
	}
	if ( slot >= (NS::UInteger)METAL_MAX_BUFFER_SLOTS ) {
		s_encoder->setFragmentBuffer( buffer, offset, slot );
		return;
	}
	if ( s_encState.fragmentBuffer[slot] == buffer ) {
		if ( s_encState.fragmentOffset[slot] != offset ) {
			s_encoder->setFragmentBufferOffset( offset, slot );
			s_encState.fragmentOffset[slot] = offset;
		}
		return;
	}
	s_encoder->setFragmentBuffer( buffer, offset, slot );
	s_encState.fragmentBuffer[slot] = buffer;
	s_encState.fragmentOffset[slot] = offset;
}

void GLMetal_SetFragmentTexture( MTL::Texture *texture, MTL::SamplerState *sampler, NS::UInteger slot ) {
	if ( !s_encoder || slot >= (NS::UInteger)METAL_MAX_TEXTURE_BINDINGS ) {
		return;
	}
	if ( texture && s_encState.fragmentTexture[slot] != texture ) {
		s_encoder->setFragmentTexture( texture, slot );
		s_encState.fragmentTexture[slot] = texture;
	}
	if ( sampler && s_encState.fragmentSampler[slot] != sampler ) {
		s_encoder->setFragmentSamplerState( sampler, slot );
		s_encState.fragmentSampler[slot] = sampler;
	}
}

void GLMetal_SetViewport( int x, int y, int w, int h, float znear, float zfar ) {
	MTL::Viewport vp;
	vp.originX = (double)x;
	vp.originY = (double)y;
	vp.width = (double)w;
	vp.height = (double)h;
	vp.znear = (double)znear;
	vp.zfar = (double)zfar;
	s_want.viewport = vp;
	if ( s_encoder && ( !s_encState.viewportValid || memcmp( &s_encState.viewport, &vp, sizeof( vp ) ) != 0 ) ) {
		s_encoder->setViewport( vp );
		s_encState.viewport = vp;
		s_encState.viewportValid = true;
	}
}

void GLMetal_SetScissor( int x, int y, int w, int h ) {
	// Metal rejects a scissor rectangle that leaves the render target
	int x0 = std::max( x, 0 ), y0 = std::max( y, 0 );
	int x1 = std::min( x + w, s_width ), y1 = std::min( y + h, s_height );
	MTL::ScissorRect sr;
	if ( x1 <= x0 || y1 <= y0 ) {
		sr.x = 0; sr.y = 0; sr.width = 0; sr.height = 0;
	} else {
		sr.x = (NS::UInteger)x0;
		sr.y = (NS::UInteger)y0;
		sr.width = (NS::UInteger)( x1 - x0 );
		sr.height = (NS::UInteger)( y1 - y0 );
	}
	s_want.scissor = sr;
	if ( s_encoder && ( !s_encState.scissorValid || memcmp( &s_encState.scissor, &sr, sizeof( sr ) ) != 0 ) ) {
		s_encoder->setScissorRect( sr );
		s_encState.scissor = sr;
		s_encState.scissorValid = true;
	}
}

void GLMetal_SetCullMode( MTL::CullMode mode ) {
	s_want.cullMode = mode;
	if ( s_encoder && s_encState.cullMode != (int)mode ) {
		s_encoder->setCullMode( mode );
		s_encState.cullMode = (int)mode;
	}
}

void GLMetal_SetFillMode( MTL::TriangleFillMode mode ) {
	s_want.fillMode = mode;
	if ( s_encoder && s_encState.fillMode != (int)mode ) {
		s_encoder->setTriangleFillMode( mode );
		s_encState.fillMode = (int)mode;
	}
}

void GLMetal_SetDepthBias( float depthBias, float slopeScale, float clamp ) {
	s_want.depthBias = depthBias;
	s_want.slopeScale = slopeScale;
	s_want.depthBiasClamp = clamp;
	if ( !s_encoder ) {
		return;
	}
	if ( s_encState.depthBiasValid && s_encState.depthBias == depthBias &&
		 s_encState.slopeScale == slopeScale && s_encState.depthBiasClamp == clamp ) {
		return;
	}
	s_encoder->setDepthBias( depthBias, slopeScale, clamp );
	s_encState.depthBias = depthBias;
	s_encState.slopeScale = slopeScale;
	s_encState.depthBiasClamp = clamp;
	s_encState.depthBiasValid = true;
}

static uint64_t GLMetal_PackDepthStencilKey( const MetalDepthStencilKey &k ) {
	uint64_t v = 0;
	v |= (uint64_t)( k.depthFunc & 7 );
	v |= (uint64_t)( k.depthWrite & 1 ) << 3;
	v |= (uint64_t)( k.stencilEnable & 1 ) << 4;
	if ( k.stencilEnable ) {
		v |= (uint64_t)k.readMask << 8;
		v |= (uint64_t)k.writeMask << 16;
		v |= (uint64_t)( k.front.func & 7 ) << 24;
		v |= (uint64_t)( k.front.sfail & 7 ) << 27;
		v |= (uint64_t)( k.front.zfail & 7 ) << 30;
		v |= (uint64_t)( k.front.zpass & 7 ) << 33;
		v |= (uint64_t)( k.back.func & 7 ) << 36;
		v |= (uint64_t)( k.back.sfail & 7 ) << 39;
		v |= (uint64_t)( k.back.zfail & 7 ) << 42;
		v |= (uint64_t)( k.back.zpass & 7 ) << 45;
	}
	return v;
}

static MTL::DepthStencilState *GLMetal_GetOrCreateDepthStencilState( const MetalDepthStencilKey &k ) {
	const uint64_t key = GLMetal_PackDepthStencilKey( k );
	if ( key == s_lastDepthStencilKey && s_lastDepthStencilState ) {
		return s_lastDepthStencilState;
	}
	auto it = s_depthStencilCache.find( key );
	MTL::DepthStencilState *state = ( it != s_depthStencilCache.end() ) ? it->second : nullptr;
	if ( !state ) {
		MTL::DepthStencilDescriptor *desc = MTL::DepthStencilDescriptor::alloc()->init();
		desc->setDepthWriteEnabled( k.depthWrite != 0 );
		desc->setDepthCompareFunction( (MTL::CompareFunction)k.depthFunc );
		if ( k.stencilEnable ) {
			const MetalStencilFace *faces[2] = { &k.front, &k.back };
			for ( int i = 0; i < 2; i++ ) {
				MTL::StencilDescriptor *sd = MTL::StencilDescriptor::alloc()->init();
				sd->setStencilCompareFunction( (MTL::CompareFunction)faces[i]->func );
				sd->setStencilFailureOperation( (MTL::StencilOperation)faces[i]->sfail );
				sd->setDepthFailureOperation( (MTL::StencilOperation)faces[i]->zfail );
				sd->setDepthStencilPassOperation( (MTL::StencilOperation)faces[i]->zpass );
				sd->setReadMask( k.readMask );
				sd->setWriteMask( k.writeMask );
				if ( i == 0 ) {
					desc->setFrontFaceStencil( sd );
				} else {
					desc->setBackFaceStencil( sd );
				}
				sd->release();
			}
		}
		state = s_device->newDepthStencilState( desc );
		desc->release();
		if ( !state ) {
			MetalHost_Warning( "[Metal] failed to create depth-stencil state (0x%016llx)", (unsigned long long)key );
			return nullptr;
		}
		s_depthStencilCache.emplace( key, state );
	}
	s_lastDepthStencilKey = key;
	s_lastDepthStencilState = state;
	return state;
}

void GLMetal_SetDepthStencil( const MetalDepthStencilKey &key, uint32_t stencilRef ) {
	MTL::DepthStencilState *state = GLMetal_GetOrCreateDepthStencilState( key );
	if ( !state ) {
		return;
	}
	s_want.depthStencil = state;
	s_want.stencilRef = stencilRef;
	s_depthCurWrite = ( key.depthWrite != 0 );
	if ( !s_encoder ) {
		return;
	}
	if ( s_encState.depthStencil != state ) {
		s_encoder->setDepthStencilState( state );
		s_encState.depthStencil = state;
	}
	if ( !s_encState.stencilReferenceValid || s_encState.stencilReference != stencilRef ) {
		s_encoder->setStencilReferenceValue( stencilRef );
		s_encState.stencilReference = stencilRef;
		s_encState.stencilReferenceValid = true;
	}
}

// ============================================================================
// Frame arenas
// ============================================================================

static void GLMetal_ResetFrameArena( MetalFrameArena &arena ) {
	for ( auto &chunk : arena.chunks ) {
		chunk.used = 0;
	}
	arena.current = 0;
}

static void GLMetal_ReleaseFrameArenas( void ) {
	for ( auto &arena : s_frameArenas ) {
		for ( auto &chunk : arena.chunks ) {
			if ( chunk.buffer ) {
				chunk.buffer->release();
			}
		}
		arena.chunks.clear();
		arena.current = 0;
	}
	s_frameArenaIndex = 0;
}

static void *GLMetal_ArenaAlloc( MetalFrameArena &arena, size_t size, size_t alignment, MTL::Buffer **outBuffer, NS::UInteger *outOffset ) {
	if ( alignment == 0 ) {
		alignment = 16;
	}
	// Find room in the current chunk or any later (already reset) chunk.
	while ( arena.current < arena.chunks.size() ) {
		MetalFrameArenaChunk &chunk = arena.chunks[arena.current];
		const size_t offset = ( chunk.used + alignment - 1 ) & ~( alignment - 1 );
		if ( offset + size <= chunk.size ) {
			chunk.used = offset + size;
			*outBuffer = chunk.buffer;
			*outOffset = (NS::UInteger)offset;
			return chunk.cpu + offset;
		}
		arena.current++;
	}

	if ( !s_device ) {
		return nullptr;
	}
	MetalFrameArenaChunk chunk;
	chunk.size = std::max( kFrameArenaChunkSize, size );
	chunk.buffer = s_device->newBuffer( (NS::UInteger)chunk.size, MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeWriteCombined );
	if ( !chunk.buffer ) {
		MetalHost_Warning( "[Metal] frame arena: failed to allocate %zu-byte chunk", chunk.size );
		return nullptr;
	}
	chunk.cpu = (uint8_t *)chunk.buffer->contents();
	chunk.used = size;
	arena.chunks.push_back( chunk );
	arena.current = arena.chunks.size() - 1;
	*outBuffer = chunk.buffer;
	*outOffset = 0;
	return chunk.cpu;
}

void *GLMetal_FrameAlloc( size_t size, size_t alignment, MTL::Buffer **outBuffer, NS::UInteger *outOffset ) {
	if ( !outBuffer || !outOffset || size == 0 ) {
		return nullptr;
	}
	if ( !s_frameBegun ) {
		GLMetal_BeginFrame();
	}
	if ( !s_frameBegun ) {
		return nullptr;
	}
	return GLMetal_ArenaAlloc( s_frameArenas[s_frameArenaIndex], size, alignment, outBuffer, outOffset );
}

// Constant-address-space buffer offsets must be 256-byte aligned on macOS GPUs.
static const size_t kUniformAlignment = 256;

void GLMetal_SetSharedUniforms( const void *bytes, size_t size, NS::UInteger vertexSlot, NS::UInteger fragmentSlot ) {
	if ( !s_encoder || !bytes || size == 0 ) {
		return;
	}
	MTL::Buffer *buffer = nullptr;
	NS::UInteger offset = 0;
	void *dst = GLMetal_FrameAlloc( size, kUniformAlignment, &buffer, &offset );
	if ( !dst ) {
		return;
	}
	memcpy( dst, bytes, size );
	GLMetal_SetVertexBuffer( buffer, offset, vertexSlot );
	GLMetal_SetFragmentBuffer( buffer, offset, fragmentSlot );
}

// ============================================================================
// Shaders and pipelines
// ============================================================================

// GPU time of every command buffer, summed as they complete (r_metalFrameStats)
static std::atomic<uint64_t> s_gpuNs( 0 );
static void GLMetal_TrackGPUTime( MTL::CommandBuffer *cmd ) {
	cmd->addCompletedHandler( []( MTL::CommandBuffer *done ) {
		const double t = done->GPUEndTime() - done->GPUStartTime();
		if ( t > 0.0 && t < 1.0 ) {
			s_gpuNs += (uint64_t)( t * 1e9 );
		}
	} );
}

// Time spent compiling shaders and creating pipelines in the current frame (r_metalDebug 1
// prints it), to find the hitches of first use.
static double s_frameCompileSeconds = 0.0, s_framePipelineSeconds = 0.0;
static int s_frameCompiles = 0, s_framePipelines = 0;
static double GLMetal_Seconds( void ) {
	return std::chrono::duration<double>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

// From any thread: no printing, no statistics. NULL if it does not compile.
MTL::Library *GLMetal_CompileLibraryAnyThread( const char *mslSource ) {
	if ( !s_device || !mslSource || !mslSource[0] ) {
		return nullptr;
	}
	NS::AutoreleasePool *pool = NS::AutoreleasePool::alloc()->init();
	MTL::CompileOptions *options = MTL::CompileOptions::alloc()->init();
	options->setPreserveInvariance( true );
	NS::Error *error = nullptr;
	MTL::Library *library = s_device->newLibrary( NS::String::string( mslSource, NS::UTF8StringEncoding ), options, &error );
	options->release();
	pool->release();
	return library;
}

MTL::Library *GLMetal_LoadShaderLibraryFromSource( const char *mslSource, const char *label ) {
	if ( !s_device || !mslSource || !mslSource[0] ) {
		MetalHost_Warning( "[Metal] GLMetal_LoadShaderLibraryFromSource: invalid input or device not ready" );
		return nullptr;
	}
	NS::String *source = NS::String::string( mslSource, NS::UTF8StringEncoding );
	if ( !source ) {
		return nullptr;
	}
	MTL::CompileOptions *options = MTL::CompileOptions::alloc()->init();
	// Honour [[invariant]] on vertex position outputs, so every pass over a surface produces
	// bit-identical depth (the lighting passes test depth for equality with the depth fill).
	options->setPreserveInvariance( true );

	NS::Error *error = nullptr;
	const double t0 = GLMetal_Seconds();
	MTL::Library *library = s_device->newLibrary( source, options, &error );
	s_frameCompileSeconds += GLMetal_Seconds() - t0;
	s_frameCompiles++;
	options->release();

	if ( !library ) {
		MetalHost_Warning( "[Metal] MSL compile failed (%s): %s", ( label && label[0] ) ? label : "unnamed", MetalErrorString( error ) );
		return nullptr;
	}
	return library;
}

int GLMetal_RegisterProgram( const char *name, MTL::Function *vertexFunction, MTL::Function *fragmentFunction ) {
	if ( !vertexFunction || !fragmentFunction ) {
		return -1;
	}
	MetalProgramRecord rec;
	rec.name = ( name && name[0] ) ? name : "unnamed";
	rec.vert = vertexFunction;
	rec.frag = fragmentFunction;
	rec.vert->retain();
	rec.frag->retain();
	s_programs.push_back( rec );
	return (int)s_programs.size() - 1;
}

uint64_t GLMetal_HashVertexLayout( const MetalVertexLayout &layout ) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&h]( uint64_t v ) { h ^= v; h *= 1099511628211ull; };
	mix( (uint64_t)layout.numAttribs );
	for ( int i = 0; i < layout.numAttribs; i++ ) {
		const MetalVertexAttrib &a = layout.attribs[i];
		mix( ( (uint64_t)a.index ) | ( (uint64_t)a.format << 8 ) | ( (uint64_t)a.buffer << 16 ) | ( (uint64_t)a.offset << 24 ) );
	}
	mix( (uint64_t)layout.numBuffers );
	for ( int i = 0; i < layout.numBuffers; i++ ) {
		mix( ( (uint64_t)layout.buffers[i].slot ) | ( (uint64_t)layout.buffers[i].constant << 8 ) | ( (uint64_t)layout.buffers[i].stride << 16 ) );
	}
	return h;
}

static void ConfigureBlendState( MTL::RenderPipelineColorAttachmentDescriptor *colorAttachment, uint32_t blendKey ) {
	const MTL::BlendFactor src = (MTL::BlendFactor)( blendKey & 0xff );
	const MTL::BlendFactor dst = (MTL::BlendFactor)( ( blendKey >> 8 ) & 0xff );
	const bool enabled = ( blendKey & 0x10000u ) != 0 && !( src == MTL::BlendFactorOne && dst == MTL::BlendFactorZero );
	colorAttachment->setWriteMask( (MTL::ColorWriteMask)( ( blendKey >> 17 ) & 15u ) );
	colorAttachment->setBlendingEnabled( enabled );
	if ( enabled ) {
		colorAttachment->setRgbBlendOperation( MTL::BlendOperationAdd );
		colorAttachment->setAlphaBlendOperation( MTL::BlendOperationAdd );
		colorAttachment->setSourceRGBBlendFactor( src );
		colorAttachment->setSourceAlphaBlendFactor( src );
		colorAttachment->setDestinationRGBBlendFactor( dst );
		colorAttachment->setDestinationAlphaBlendFactor( dst );
	}
}

// PIPELINE_DEPTH_ONLY: the variant for the single-sample depth replay pass (no colour target).
// PIPELINE_LAYER: the variant for a blend layer target (see GLMetal_BeginBlendLayer).
enum { PIPELINE_SCENE, PIPELINE_DEPTH_ONLY, PIPELINE_LAYER };
static MTL::RenderPipelineState *GLMetal_CreatePipeline( int programId, const MetalVertexLayout &layout, uint32_t blendKey, int mode ) {
	const MetalProgramRecord &rec = s_programs[programId];
	MTL::VertexDescriptor *vertexDesc = MTL::VertexDescriptor::alloc()->init();
	for ( int i = 0; i < layout.numAttribs; i++ ) {
		const MetalVertexAttrib &a = layout.attribs[i];
		MTL::VertexAttributeDescriptor *ad = vertexDesc->attributes()->object( a.index );
		ad->setFormat( (MTL::VertexFormat)a.format );
		ad->setOffset( a.offset );
		ad->setBufferIndex( a.buffer );
	}
	for ( int i = 0; i < layout.numBuffers; i++ ) {
		MTL::VertexBufferLayoutDescriptor *ld = vertexDesc->layouts()->object( layout.buffers[i].slot );
		if ( layout.buffers[i].constant ) {
			ld->setStepFunction( MTL::VertexStepFunctionConstant );
			ld->setStepRate( 0 );
			ld->setStride( std::max<uint32_t>( layout.buffers[i].stride, 16 ) );
		} else {
			ld->setStepFunction( MTL::VertexStepFunctionPerVertex );
			ld->setStepRate( 1 );
			ld->setStride( layout.buffers[i].stride );
		}
	}

	MTL::RenderPipelineDescriptor *pipelineDesc = MTL::RenderPipelineDescriptor::alloc()->init();
	pipelineDesc->setVertexFunction( rec.vert );
	pipelineDesc->setFragmentFunction( rec.frag );
	pipelineDesc->setVertexDescriptor( vertexDesc );
	pipelineDesc->setLabel( NS::String::string( rec.name.c_str(), NS::UTF8StringEncoding ) );
	if ( mode != PIPELINE_LAYER ) {
		pipelineDesc->setDepthAttachmentPixelFormat( s_depthPixelFormat );
		pipelineDesc->setStencilAttachmentPixelFormat( s_depthPixelFormat );
	}
	if ( mode == PIPELINE_DEPTH_ONLY ) {
		pipelineDesc->setRasterSampleCount( 1 );
	} else if ( mode == PIPELINE_LAYER ) {
		// rgb collects colour as the blend says; alpha collects how much of the background is left
		const MTL::BlendFactor src = (MTL::BlendFactor)( blendKey & 0xff );
		const MTL::BlendFactor dst = (MTL::BlendFactor)( ( blendKey >> 8 ) & 0xff );
		MTL::RenderPipelineColorAttachmentDescriptor *colorAttachment = pipelineDesc->colorAttachments()->object( 0 );
		colorAttachment->setPixelFormat( MTL::PixelFormatRGBA16Float );
		pipelineDesc->setRasterSampleCount( 1 );
		colorAttachment->setBlendingEnabled( true );
		colorAttachment->setRgbBlendOperation( MTL::BlendOperationAdd );
		colorAttachment->setAlphaBlendOperation( MTL::BlendOperationAdd );
		colorAttachment->setSourceRGBBlendFactor( src );
		colorAttachment->setDestinationRGBBlendFactor( dst );
		colorAttachment->setSourceAlphaBlendFactor( MTL::BlendFactorZero );
		colorAttachment->setDestinationAlphaBlendFactor( dst == MTL::BlendFactorOneMinusSourceAlpha ? MTL::BlendFactorOneMinusSourceAlpha : MTL::BlendFactorOne );
	} else {
		MTL::RenderPipelineColorAttachmentDescriptor *colorAttachment = pipelineDesc->colorAttachments()->object( 0 );
		colorAttachment->setPixelFormat( s_colorPixelFormat );
		pipelineDesc->setRasterSampleCount( ( blendKey & PIPELINE_SINGLE_SAMPLE ) ? 1 : (NS::UInteger)s_sampleCount );
		ConfigureBlendState( colorAttachment, blendKey );
	}

	NS::Error *error = nullptr;
	const double t0 = GLMetal_Seconds();
	MTL::RenderPipelineState *pipeline = s_device->newRenderPipelineState( pipelineDesc, &error );
	s_framePipelineSeconds += GLMetal_Seconds() - t0;
	s_framePipelines++;
	pipelineDesc->release();
	vertexDesc->release();
	if ( !pipeline ) {
		MetalHost_Warning( "[Metal] pipeline creation failed (%s, blend 0x%x%s): %s", rec.name.c_str(), blendKey,
						   mode == PIPELINE_DEPTH_ONLY ? ", depth only" : mode == PIPELINE_LAYER ? ", layer" : "", MetalErrorString( error ) );
	}
	return pipeline;
}

MTL::RenderPipelineState *GLMetal_GetPipeline( int programId, const MetalVertexLayout &layout, uint64_t layoutHash,
											   uint32_t blendKey, bool lines ) {
	(void)lines;
	if ( programId < 0 || programId >= (int)s_programs.size() ) {
		return nullptr;
	}
	if ( s_msaaEnded ) {
		blendKey |= PIPELINE_SINGLE_SAMPLE;
	}
	PipelineCacheKey key = { programId, blendKey, layoutHash };
	if ( !( key == s_curPipelineKey ) ) {
		// what a variant of the pipeline about to be bound is made from
		s_curPipelineKey = key;
		s_curPipelineLayout = layout;
	}
	if ( s_lastPipeline && key == s_lastPipelineKey ) {
		return s_lastPipeline;
	}
	auto it = s_pipelineCache.find( key );
	if ( it != s_pipelineCache.end() ) {
		if ( it->second ) {
			s_lastPipelineKey = key;
			s_lastPipeline = it->second;
		}
		return it->second;
	}

	MTL::RenderPipelineState *pipeline = GLMetal_CreatePipeline( programId, layout, blendKey, PIPELINE_SCENE );
	s_pipelineCache.emplace( key, pipeline );
	if ( pipeline ) {
		s_lastPipelineKey = key;
		s_lastPipeline = pipeline;
	}
	return pipeline;
}

// ============================================================================
// Depth capture by replay
//
// Copying the depth buffer to a texture in the middle of a frame (soft particles do, right
// after the depth fill) means ending the scene pass and opening it again. With multisampling
// that stores and loads every sample of every attachment, and blending onto loaded samples
// is several times slower than onto samples that never left the tile (measured: translucent
// fill 1.6 ms against 4.7 ms at 2560x1440, 8 samples). So with multisampling the depth-writing
// draws of a frame are recorded, and a depth copy draws them again into a single-sample depth
// target, in a command buffer of its own that is committed at once and so runs before the
// frame's. The scene pass is not touched.
// ============================================================================

static const int METAL_DEPTH_DRAW_TEXTURES = 8;
static const int METAL_DEPTH_DRAW_BUFFERS = 12;
struct MetalDepthDraw {
	MTL::RenderPipelineState *pipeline;		// depth-only variant
	MTL::DepthStencilState *depthStencil;
	uint32_t stencilRef;
	MTL::Viewport viewport;
	MTL::ScissorRect scissor;
	float depthBias, slopeScale, depthBiasClamp;
	int8_t cullMode, fillMode;
	uint8_t indexed, numVertexBuffers, numTextures;
	uint8_t vertexSlot[METAL_DEPTH_DRAW_BUFFERS];
	MTL::Buffer *vertexBuffer[METAL_DEPTH_DRAW_BUFFERS];
	NS::UInteger vertexOffset[METAL_DEPTH_DRAW_BUFFERS];
	MTL::Buffer *fragmentBuffer;			// slot 0
	NS::UInteger fragmentOffset;
	MTL::Texture *texture[METAL_DEPTH_DRAW_TEXTURES];
	MTL::SamplerState *sampler[METAL_DEPTH_DRAW_TEXTURES];
	MTL::PrimitiveType primitive;
	MTL::IndexType indexType;
	MTL::Buffer *indexBuffer;
	NS::UInteger first, count;				// first: index buffer offset when indexed
};
static std::vector<MetalDepthDraw> s_depthDraws;
static std::unordered_map<PipelineCacheKey, MTL::RenderPipelineState *, PipelineCacheKeyHash> s_depthPipelineCache;
static MTL::Texture *s_replayDepth = nullptr;
static uint64_t s_depthCopyFrame = ~0ull;	// the last frame that copied depth

bool GLMetal_CanReplayDepth( void ) {
	return s_depthRecordValid;
}

// A clear of the whole depth buffer is about to be drawn: nothing before it matters.
void GLMetal_DepthBufferCleared( void ) {
	s_depthDraws.clear();
}

// Copies what the draw that is about to be made needs from the encoder state.
static bool GLMetal_CaptureDraw( MetalDepthDraw &d, bool indexed, MTL::PrimitiveType primitive, MTL::IndexType indexType,
								 MTL::Buffer *indexBuffer, NS::UInteger first, NS::UInteger count ) {
	d.numVertexBuffers = 0;
	for ( int i = 0; i < METAL_MAX_BUFFER_SLOTS; i++ ) {
		if ( !s_encState.vertexBuffer[i] ) {
			continue;
		}
		if ( d.numVertexBuffers == METAL_DEPTH_DRAW_BUFFERS ) {
			return false;
		}
		d.vertexSlot[d.numVertexBuffers] = (uint8_t)i;
		d.vertexBuffer[d.numVertexBuffers] = s_encState.vertexBuffer[i];
		d.vertexOffset[d.numVertexBuffers] = s_encState.vertexOffset[i];
		d.numVertexBuffers++;
	}
	if ( !s_encState.depthStencil ) {
		return false;
	}
	d.depthStencil = s_encState.depthStencil;
	d.stencilRef = s_encState.stencilReference;
	d.viewport = s_encState.viewport;
	d.scissor = s_encState.scissor;
	d.depthBias = s_encState.depthBias;
	d.slopeScale = s_encState.slopeScale;
	d.depthBiasClamp = s_encState.depthBiasClamp;
	d.cullMode = (int8_t)s_encState.cullMode;
	d.fillMode = (int8_t)s_encState.fillMode;
	d.fragmentBuffer = s_encState.fragmentBuffer[0];
	d.fragmentOffset = s_encState.fragmentOffset[0];
	d.numTextures = 0;
	for ( int i = 0; i < METAL_DEPTH_DRAW_TEXTURES; i++ ) {
		d.texture[i] = s_encState.fragmentTexture[i];
		d.sampler[i] = s_encState.fragmentSampler[i];
		if ( d.texture[i] || d.sampler[i] ) {
			d.numTextures = (uint8_t)( i + 1 );
		}
	}
	d.indexed = indexed ? 1 : 0;
	d.primitive = primitive;
	d.indexType = indexType;
	d.indexBuffer = indexBuffer;
	d.first = first;
	d.count = count;
	return true;
}

static void GLMetal_RecordDepthDraw( bool indexed, MTL::PrimitiveType primitive, MTL::IndexType indexType,
									 MTL::Buffer *indexBuffer, NS::UInteger first, NS::UInteger count ) {
	if ( !s_depthRecordValid || !s_depthCurWrite || !s_encState.pipeline ) {
		return;
	}
	MetalDepthDraw d;
	PipelineCacheKey key = { s_curPipelineKey.programId, 0, s_curPipelineKey.layoutHash };
	auto it = s_depthPipelineCache.find( key );
	if ( it != s_depthPipelineCache.end() ) {
		d.pipeline = it->second;
	} else {
		d.pipeline = GLMetal_CreatePipeline( key.programId, s_curPipelineLayout, 0, PIPELINE_DEPTH_ONLY );
		s_depthPipelineCache.emplace( key, d.pipeline );
	}
	if ( !d.pipeline || !GLMetal_CaptureDraw( d, indexed, primitive, indexType, indexBuffer, first, count ) ) {
		// cannot be drawn again: copies fall back to ending the pass
		s_depthRecordValid = false;
		s_depthDraws.clear();
		return;
	}
	s_depthDraws.push_back( d );
}

// Draws recorded draws with another encoder. `scale` divides viewports and scissors (a
// smaller target); `depth`: the target has a depth and stencil attachment.
static void GLMetal_ReplayDraws( MTL::RenderCommandEncoder *enc, const std::vector<MetalDepthDraw> &draws, int scale, bool depth,
								 int targetWidth, int targetHeight ) {
	enc->setFrontFacingWinding( MTL::WindingClockwise );
	const MetalDepthDraw *last = nullptr;
	for ( const MetalDepthDraw &d : draws ) {
		if ( !last || last->pipeline != d.pipeline ) {
			enc->setRenderPipelineState( d.pipeline );
		}
		if ( depth ) {
			if ( !last || last->depthStencil != d.depthStencil ) {
				enc->setDepthStencilState( d.depthStencil );
			}
			if ( !last || last->stencilRef != d.stencilRef ) {
				enc->setStencilReferenceValue( d.stencilRef );
			}
			if ( !last || last->depthBias != d.depthBias || last->slopeScale != d.slopeScale || last->depthBiasClamp != d.depthBiasClamp ) {
				enc->setDepthBias( d.depthBias, d.slopeScale, d.depthBiasClamp );
			}
		}
		if ( !last || memcmp( &last->viewport, &d.viewport, sizeof( d.viewport ) ) != 0 ) {
			MTL::Viewport vp = d.viewport;
			vp.originX /= scale;
			vp.originY /= scale;
			vp.width /= scale;
			vp.height /= scale;
			enc->setViewport( vp );
		}
		if ( !last || memcmp( &last->scissor, &d.scissor, sizeof( d.scissor ) ) != 0 ) {
			MTL::ScissorRect sr = d.scissor;
			if ( scale > 1 && sr.width > 0 && sr.height > 0 ) {
				const NS::UInteger x1 = std::min<NS::UInteger>( ( sr.x + sr.width + scale - 1 ) / scale, (NS::UInteger)targetWidth );
				const NS::UInteger y1 = std::min<NS::UInteger>( ( sr.y + sr.height + scale - 1 ) / scale, (NS::UInteger)targetHeight );
				sr.x /= scale;
				sr.y /= scale;
				sr.width = x1 > sr.x ? x1 - sr.x : 0;
				sr.height = y1 > sr.y ? y1 - sr.y : 0;
			}
			enc->setScissorRect( sr );
		}
		if ( !last || last->cullMode != d.cullMode ) {
			enc->setCullMode( (MTL::CullMode)d.cullMode );
		}
		if ( !last || last->fillMode != d.fillMode ) {
			enc->setTriangleFillMode( (MTL::TriangleFillMode)d.fillMode );
		}
		for ( int i = 0; i < d.numVertexBuffers; i++ ) {
			if ( last && i < last->numVertexBuffers && last->vertexSlot[i] == d.vertexSlot[i] && last->vertexBuffer[i] == d.vertexBuffer[i] ) {
				if ( last->vertexOffset[i] != d.vertexOffset[i] ) {
					enc->setVertexBufferOffset( d.vertexOffset[i], d.vertexSlot[i] );
				}
			} else {
				enc->setVertexBuffer( d.vertexBuffer[i], d.vertexOffset[i], d.vertexSlot[i] );
			}
		}
		if ( d.fragmentBuffer ) {
			if ( last && last->fragmentBuffer == d.fragmentBuffer ) {
				if ( last->fragmentOffset != d.fragmentOffset ) {
					enc->setFragmentBufferOffset( d.fragmentOffset, 0 );
				}
			} else {
				enc->setFragmentBuffer( d.fragmentBuffer, d.fragmentOffset, 0 );
			}
		}
		for ( int i = 0; i < d.numTextures; i++ ) {
			if ( d.texture[i] && ( !last || last->texture[i] != d.texture[i] ) ) {
				enc->setFragmentTexture( d.texture[i], (NS::UInteger)i );
			}
			if ( d.sampler[i] && ( !last || last->sampler[i] != d.sampler[i] ) ) {
				enc->setFragmentSamplerState( d.sampler[i], (NS::UInteger)i );
			}
		}
		if ( d.indexed ) {
			enc->drawIndexedPrimitives( d.primitive, d.count, d.indexType, d.indexBuffer, d.first );
		} else {
			enc->drawPrimitives( d.primitive, d.first, d.count );
		}
		last = &d;
	}
}

// Draws the recorded depth writes into s_replayDepth with `cmd`.
static bool GLMetal_ReplayDepthDraws( MTL::CommandBuffer *cmd ) {
	MTL::RenderPassDescriptor *desc = MTL::RenderPassDescriptor::renderPassDescriptor();
	desc->depthAttachment()->setTexture( s_replayDepth );
	desc->depthAttachment()->setLoadAction( MTL::LoadActionClear );
	desc->depthAttachment()->setClearDepth( 1.0 );
	desc->depthAttachment()->setStoreAction( MTL::StoreActionStore );
	desc->stencilAttachment()->setTexture( s_replayDepth );
	desc->stencilAttachment()->setLoadAction( MTL::LoadActionClear );
	desc->stencilAttachment()->setClearStencil( 0 );
	desc->stencilAttachment()->setStoreAction( MTL::StoreActionDontCare );
	MTL::RenderCommandEncoder *enc = cmd->renderCommandEncoder( desc );
	if ( !enc ) {
		return false;
	}
	GLMetal_ReplayDraws( enc, s_depthDraws, 1, true, s_width, s_height );
	enc->endEncoding();
	return true;
}

// ============================================================================
// Blend layer (r_metalParticleLayer)
//
// Steam and smoke are many large blended quads on top of each other, and filling them is what
// a frame with a lot of them costs: in the scene target every layer is blended into every
// sample. Soft particles are not depth tested (they fade against the depth texture) and have
// no hard edges, so they do not need the scene's samples or even its resolution. Between
// GLMetal_BeginBlendLayer and GLMetal_EndBlendLayer draws are not made but recorded. Before
// the next draw into the scene they are drawn into a single-sample target of 1/scale the
// size, which starts as (0,0,0,1) and collects colour in rgb and what is left of the
// background in alpha, in a command buffer that runs before the frame's; one rectangle then
// blends that target over the scene (colour + scene * alpha).
// ============================================================================

static void GLMetal_FlushUploads( bool wait );

static const int METAL_MAX_LAYER_TARGETS = 4;	// groups of layer draws per frame
static std::vector<MetalDepthDraw> s_layerDraws;
static std::unordered_map<PipelineCacheKey, MTL::RenderPipelineState *, PipelineCacheKeyHash> s_layerPipelineCache;
static MTL::Texture *s_layerTargets[METAL_MAX_LAYER_TARGETS];
static int s_layerTargetsUsed = 0;
static int s_layerTargetScale = 0, s_layerWidth = 0, s_layerHeight = 0;
static int s_layerScale = 0;				// r_metalParticleLayer of this frame
static bool s_layerActive = false;
static bool s_layerFlushing = false;
static uint64_t s_depthReplayFrame = ~0ull;	// the last frame whose depth copy was made by replay
static MTL::RenderPipelineState *s_layerCompositePipeline = nullptr;
static MTL::RenderPipelineState *s_layerCompositeDepthPipeline = nullptr;

// The scene depth the layer is upsampled against: the last depth copy of the frame (the
// texture the soft particles read), the part of the scene it covers and where that is in it.
struct MetalLayerDepth {
	MTL::Texture *tex;
	int x, y, width, height;	// scene pixels
	int offsetX, offsetY;		// texel = scene pixel + offset
};
static MetalLayerDepth s_lastDepthCopy;		// tex null: none this frame
static MetalLayerDepth s_layerGroupDepth;	// of the group being collected

static const char *kLayerCompositeMSL =
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct LayerOut { float4 position [[position]]; float2 uv; };\n"
	"vertex LayerOut layer_vs( uint vid [[vertex_id]], constant float4 &uvScale [[buffer(0)]] ) {\n"
	"	LayerOut o;\n"
	"	float2 p = float2( float( ( vid << 1 ) & 2 ), float( vid & 2 ) );\n"
	"	o.position = float4( p * 2.0 - 1.0, 0.0, 1.0 );\n"
	// same rows as the scene target: no flip
	"	o.uv = float2( p.x, 1.0 - p.y ) * uvScale.xy;\n"
	"	return o;\n"
	"}\n"
	"fragment float4 layer_fs( LayerOut in [[stage_in]], texture2d<float> layer [[texture(0)]], sampler smp [[sampler(0)]] ) {\n"
	"	return layer.sample( smp, in.uv );\n"
	"}\n"
	// Depth-aware upsample: of the four layer texels around the pixel, only those drawn at
	// about the pixel's own depth are blended, so steam behind a nearer edge does not bleed
	// onto it. Depth d is 1 - near / z (infinite far plane), so (1 - d) ratios are distance ratios.
	"struct LayerDepthParams { int4 rect; int4 misc; };\n"	// rect: scene pixels with depth; misc: texel offset, scale
	"fragment float4 layer_depth_fs( LayerOut in [[stage_in]], texture2d<float> layer [[texture(0)]], texture2d<float> depth [[texture(1)]],\n"
	"								 sampler smp [[sampler(0)]], constant LayerDepthParams &p [[buffer(0)]] ) {\n"
	"	const int2 px = int2( in.position.xy );\n"
	"	const int2 rmin = p.rect.xy, rmax = p.rect.xy + p.rect.zw - 1;\n"
	"	const float4 plain = layer.sample( smp, in.uv );\n"
	// colour is never negative and alpha never above 1, so an empty filtered sample means four empty texels
	"	if ( any( px < rmin ) || any( px > rmax ) || ( plain.a > 0.999 && max3( plain.r, plain.g, plain.b ) < 0.001 ) ) {\n"
	"		return plain;\n"
	"	}\n"
	"	const int s = p.misc.z;\n"
	"	const int2 off = p.misc.xy;\n"
	"	const float a = max( 1.0 - depth.read( uint2( px + off ) ).r, 1e-7 );\n"
	"	const float2 f = ( float2( px ) + 0.5 ) / float( s ) - 0.5;\n"
	"	const int2 i0 = int2( floor( f ) );\n"
	"	const float2 t = f - float2( i0 );\n"
	"	const int2 lmax = int2( layer.get_width(), layer.get_height() ) - 1;\n"
	"	int2 li[4];\n"
	"	float rd[4];\n"
	"	bool edge = false;\n"
	"	for ( int k = 0; k < 4; k++ ) {\n"
	"		li[k] = clamp( i0 + int2( k & 1, k >> 1 ), int2( 0 ), lmax );\n"
	"		const int2 dp = clamp( li[k] * s + s / 2, rmin, rmax );\n"
	"		const float b = max( 1.0 - depth.read( uint2( dp + off ) ).r, 1e-7 );\n"
	"		rd[k] = max( a, b ) / min( a, b ) - 1.0;\n"
	"		edge = edge || rd[k] >= 0.1;\n"
	"	}\n"
	"	if ( !edge ) {\n"
	"		return plain;\n"
	"	}\n"
	"	float4 sum = 0.0, nearest = 0.0;\n"
	"	float wsum = 0.0, best = 1e30;\n"
	"	for ( int k = 0; k < 4; k++ ) {\n"
	"		const float4 c = layer.read( uint2( li[k] ) );\n"
	"		const float w = ( ( k & 1 ) ? t.x : 1.0 - t.x ) * ( ( k >> 1 ) ? t.y : 1.0 - t.y );\n"
	"		if ( rd[k] < 0.1 ) {\n"
	"			sum += c * w;\n"
	"			wsum += w;\n"
	"		}\n"
	"		if ( rd[k] < best ) {\n"
	"			best = rd[k];\n"
	"			nearest = c;\n"
	"		}\n"
	"	}\n"
	"	return wsum > 1e-4 ? sum / wsum : nearest;\n"
	"}\n";

static void GLMetal_ReleaseLayerTargets( void ) {
	for ( int i = 0; i < METAL_MAX_LAYER_TARGETS; i++ ) {
		if ( s_layerTargets[i] ) {
			s_layerTargets[i]->release();
			s_layerTargets[i] = nullptr;
		}
	}
	s_layerTargetScale = 0;
}

static MTL::RenderPipelineState *GLMetal_NewLayerCompositePipeline( MTL::Library *lib, const char *fragmentName ) {
	MTL::Function *vs = lib->newFunction( NS::String::string( "layer_vs", NS::UTF8StringEncoding ) );
	MTL::Function *fs = lib->newFunction( NS::String::string( fragmentName, NS::UTF8StringEncoding ) );
	MTL::RenderPipelineDescriptor *desc = MTL::RenderPipelineDescriptor::alloc()->init();
	desc->setVertexFunction( vs );
	desc->setFragmentFunction( fs );
	MTL::RenderPipelineColorAttachmentDescriptor *ca = desc->colorAttachments()->object( 0 );
	ca->setPixelFormat( s_colorPixelFormat );
	ca->setBlendingEnabled( true );
	ca->setRgbBlendOperation( MTL::BlendOperationAdd );
	ca->setAlphaBlendOperation( MTL::BlendOperationAdd );
	ca->setSourceRGBBlendFactor( MTL::BlendFactorOne );
	ca->setSourceAlphaBlendFactor( MTL::BlendFactorOne );
	ca->setDestinationRGBBlendFactor( MTL::BlendFactorSourceAlpha );
	ca->setDestinationAlphaBlendFactor( MTL::BlendFactorSourceAlpha );
	ca->setWriteMask( MTL::ColorWriteMaskRed | MTL::ColorWriteMaskGreen | MTL::ColorWriteMaskBlue );
	desc->setDepthAttachmentPixelFormat( s_depthPixelFormat );
	desc->setStencilAttachmentPixelFormat( s_depthPixelFormat );
	desc->setRasterSampleCount( (NS::UInteger)s_sampleCount );
	NS::Error *error = nullptr;
	MTL::RenderPipelineState *pipeline = s_device->newRenderPipelineState( desc, &error );
	desc->release();
	if ( vs ) {
		vs->release();
	}
	if ( fs ) {
		fs->release();
	}
	if ( !pipeline ) {
		MetalHost_Warning( "[Metal] layer composite pipeline (%s) creation failed: %s", fragmentName, MetalErrorString( error ) );
	}
	return pipeline;
}

static bool GLMetal_EnsureLayerCompositePipeline( void ) {
	if ( s_layerCompositePipeline ) {
		return true;
	}
	MTL::Library *lib = GLMetal_LoadShaderLibraryFromSource( kLayerCompositeMSL, "layer composite" );
	if ( !lib ) {
		return false;
	}
	s_layerCompositePipeline = GLMetal_NewLayerCompositePipeline( lib, "layer_fs" );
	s_layerCompositeDepthPipeline = GLMetal_NewLayerCompositePipeline( lib, "layer_depth_fs" );
	lib->release();
	return s_layerCompositePipeline != nullptr;
}

// Returns the scale of the layer (1, 2, 4) if the draws that follow, with this blend state,
// will be collected in a layer, or 0: then they are drawn as usual.
int GLMetal_BeginBlendLayer( uint32_t blendKey ) {
	if ( s_layerScale <= 0 || s_layerActive || !s_encoder || s_depthReplayFrame != s_frameSerial || s_msaaEnded ) {
		return 0;
	}
	// blends that only add to the background or cover it by the source alpha
	const MTL::BlendFactor src = (MTL::BlendFactor)( blendKey & 0xff );
	const MTL::BlendFactor dst = (MTL::BlendFactor)( ( blendKey >> 8 ) & 0xff );
	const uint32_t rgb = (uint32_t)MTL::ColorWriteMaskRed | (uint32_t)MTL::ColorWriteMaskGreen | (uint32_t)MTL::ColorWriteMaskBlue;
	if ( !( blendKey & 0x10000u ) || ( ( blendKey >> 17 ) & rgb ) != rgb ) {
		return 0;
	}
	if ( !( ( src == MTL::BlendFactorSourceAlpha || src == MTL::BlendFactorOne ) &&
			( dst == MTL::BlendFactorOne || dst == MTL::BlendFactorOneMinusSourceAlpha ) ) ) {
		return 0;
	}
	if ( s_layerTargetScale != s_layerScale ) {
		GLMetal_ReleaseLayerTargets();
		s_layerTargetScale = s_layerScale;
		s_layerWidth = ( s_width + s_layerScale - 1 ) / s_layerScale;
		s_layerHeight = ( s_height + s_layerScale - 1 ) / s_layerScale;
	}
	if ( s_layerDraws.empty() ) {
		// a new group needs a target of its own
		if ( s_layerTargetsUsed >= METAL_MAX_LAYER_TARGETS || !GLMetal_EnsureLayerCompositePipeline() || !s_presentSampler ) {
			return 0;
		}
		if ( !s_layerTargets[s_layerTargetsUsed] ) {
			s_layerTargets[s_layerTargetsUsed] = GLMetal_NewTexture( MTL::TextureType2D, MTL::PixelFormatRGBA16Float, s_layerWidth, s_layerHeight, 1, 1, true, 0 );
			if ( !s_layerTargets[s_layerTargetsUsed] ) {
				return 0;
			}
		}
		s_layerGroupDepth = s_lastDepthCopy;
	}
	s_layerActive = true;
	return s_layerScale;
}

void GLMetal_EndBlendLayer( void ) {
	s_layerActive = false;
}

// A draw while the layer is active. False: it could not be recorded and must be drawn.
static bool GLMetal_RecordLayerDraw( bool indexed, MTL::PrimitiveType primitive, MTL::IndexType indexType,
									 MTL::Buffer *indexBuffer, NS::UInteger first, NS::UInteger count ) {
	if ( !s_encState.pipeline ) {
		return false;
	}
	MetalDepthDraw d;
	auto it = s_layerPipelineCache.find( s_curPipelineKey );
	if ( it != s_layerPipelineCache.end() ) {
		d.pipeline = it->second;
	} else {
		d.pipeline = GLMetal_CreatePipeline( s_curPipelineKey.programId, s_curPipelineLayout, s_curPipelineKey.blendKey, PIPELINE_LAYER );
		s_layerPipelineCache.emplace( s_curPipelineKey, d.pipeline );
	}
	if ( !d.pipeline || !GLMetal_CaptureDraw( d, indexed, primitive, indexType, indexBuffer, first, count ) ) {
		return false;
	}
	s_layerDraws.push_back( d );
	return true;
}

// Draws the collected layer and blends it over the scene. Called before anything else is
// drawn into the scene and before the scene pass ends.
static void GLMetal_FlushBlendLayer( void ) {
	if ( s_layerDraws.empty() || s_layerFlushing || !s_encoder ) {
		return;
	}
	s_layerFlushing = true;
	MTL::Texture *target = s_layerTargets[s_layerTargetsUsed++];

	GLMetal_FlushUploads( false );
	MTL::CommandBuffer *pre = s_queue->commandBuffer();
	MTL::RenderPassDescriptor *desc = MTL::RenderPassDescriptor::renderPassDescriptor();
	desc->colorAttachments()->object( 0 )->setTexture( target );
	desc->colorAttachments()->object( 0 )->setLoadAction( MTL::LoadActionClear );
	desc->colorAttachments()->object( 0 )->setClearColor( MTL::ClearColor::Make( 0.0, 0.0, 0.0, 1.0 ) );
	desc->colorAttachments()->object( 0 )->setStoreAction( MTL::StoreActionStore );
	MTL::RenderCommandEncoder *enc = pre ? pre->renderCommandEncoder( desc ) : nullptr;
	if ( enc ) {
		GLMetal_ReplayDraws( enc, s_layerDraws, s_layerTargetScale, false, s_layerWidth, s_layerHeight );
		enc->endEncoding();
		GLMetal_TrackGPUTime( pre );
		pre->commit();

		MetalDepthStencilKey ds;
		memset( &ds, 0, sizeof( ds ) );
		ds.depthFunc = (uint8_t)MTL::CompareFunctionAlways;
		MTL::DepthStencilState *noDepth = GLMetal_GetOrCreateDepthStencilState( ds );
		MTL::Viewport vp = { 0.0, 0.0, (double)s_width, (double)s_height, 0.0, 1.0 };
		MTL::ScissorRect sr = { 0, 0, (NS::UInteger)s_width, (NS::UInteger)s_height };
		// the part of the target the scene covers
		const float uvScale[4] = { (float)s_width / ( (float)s_layerWidth * s_layerTargetScale ),
								   (float)s_height / ( (float)s_layerHeight * s_layerTargetScale ), 0.0f, 0.0f };
		const MetalLayerDepth &ld = s_layerGroupDepth;
		const bool depthAware = s_layerTargetScale > 1 && ld.tex && s_layerCompositeDepthPipeline;
		s_encoder->setRenderPipelineState( depthAware ? s_layerCompositeDepthPipeline : s_layerCompositePipeline );
		if ( depthAware ) {
			const int32_t params[8] = { ld.x, ld.y, ld.width, ld.height, ld.offsetX, ld.offsetY, s_layerTargetScale, 0 };
			s_encoder->setFragmentBytes( params, sizeof( params ), 0 );
			s_encoder->setFragmentTexture( ld.tex, 1 );
		}
		if ( noDepth ) {
			s_encoder->setDepthStencilState( noDepth );
		}
		s_encoder->setViewport( vp );
		s_encoder->setScissorRect( sr );
		s_encoder->setCullMode( MTL::CullModeNone );
		s_encoder->setTriangleFillMode( MTL::TriangleFillModeFill );
		s_encoder->setDepthBias( 0.0f, 0.0f, 0.0f );
		s_encoder->setVertexBytes( uvScale, sizeof( uvScale ), 0 );
		s_encoder->setFragmentTexture( target, 0 );
		s_encoder->setFragmentSamplerState( s_presentSampler, 0 );
		s_encoder->drawPrimitives( MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3 );
		// nothing the encoder state cache knows is true any more
		GLMetal_OnEncoderOpened();
	}
	s_layerDraws.clear();
	s_layerFlushing = false;
}

void GLMetal_DrawIndexed( NS::UInteger count, MTL::IndexType indexType, MTL::Buffer *indexBuffer, NS::UInteger indexOffset ) {
	if ( !s_encoder ) {
		return;
	}
	if ( s_layerActive && GLMetal_RecordLayerDraw( true, MTL::PrimitiveTypeTriangle, indexType, indexBuffer, indexOffset, count ) ) {
		return;
	}
	s_encoder->drawIndexedPrimitives( MTL::PrimitiveTypeTriangle, count, indexType, indexBuffer, indexOffset );
	GLMetal_RecordDepthDraw( true, MTL::PrimitiveTypeTriangle, indexType, indexBuffer, indexOffset, count );
}

void GLMetal_Draw( MTL::PrimitiveType primitive, NS::UInteger first, NS::UInteger count ) {
	if ( !s_encoder ) {
		return;
	}
	if ( s_layerActive && GLMetal_RecordLayerDraw( false, primitive, MTL::IndexTypeUInt16, nullptr, first, count ) ) {
		return;
	}
	s_encoder->drawPrimitives( primitive, first, count );
	GLMetal_RecordDepthDraw( false, primitive, MTL::IndexTypeUInt16, nullptr, first, count );
}

// ============================================================================
// Samplers and textures
// ============================================================================

MTL::SamplerState *GLMetal_GetSampler( const MetalSamplerKey &k ) {
	uint64_t key = 0;
	key |= (uint64_t)( k.minFilter & 1 );
	key |= (uint64_t)( k.magFilter & 1 ) << 1;
	key |= (uint64_t)( k.mipFilter & 3 ) << 2;
	key |= (uint64_t)( k.addressS & 7 ) << 4;
	key |= (uint64_t)( k.addressT & 7 ) << 7;
	key |= (uint64_t)( k.addressR & 7 ) << 10;
	key |= (uint64_t)( k.anisotropy & 31 ) << 13;
	key |= (uint64_t)( k.maxLod ) << 18;
	key |= (uint64_t)( (uint8_t)k.lodBias8 ) << 26;
	key |= (uint64_t)( k.border & 3 ) << 34;
	auto it = s_samplerCache.find( key );
	if ( it != s_samplerCache.end() ) {
		return it->second;
	}
	if ( !s_device ) {
		return nullptr;
	}
	MTL::SamplerDescriptor *desc = MTL::SamplerDescriptor::alloc()->init();
	desc->setSAddressMode( (MTL::SamplerAddressMode)k.addressS );
	desc->setTAddressMode( (MTL::SamplerAddressMode)k.addressT );
	desc->setRAddressMode( (MTL::SamplerAddressMode)k.addressR );
	desc->setMinFilter( (MTL::SamplerMinMagFilter)k.minFilter );
	desc->setMagFilter( (MTL::SamplerMinMagFilter)k.magFilter );
	desc->setMipFilter( (MTL::SamplerMipFilter)k.mipFilter );
	desc->setMaxAnisotropy( std::max<NS::UInteger>( 1, std::min<NS::UInteger>( 16, k.anisotropy ) ) );
	desc->setLodMinClamp( 0.0f );
	desc->setLodMaxClamp( (float)k.maxLod );
	desc->setBorderColor( k.border == 2 ? MTL::SamplerBorderColorOpaqueWhite : ( k.border == 1 ? MTL::SamplerBorderColorOpaqueBlack : MTL::SamplerBorderColorTransparentBlack ) );
	// lodBias is not in the metal-cpp headers for every SDK; it is settable on macOS 11+
	if ( k.lodBias8 != 0 ) {
		static SEL setLodBias = sel_registerName( "setLodBias:" );
		typedef void ( *FloatSetter )( id, SEL, float );
		( (FloatSetter)objc_msgSend )( (id)desc, setLodBias, (float)k.lodBias8 / 8.0f );
	}
	MTL::SamplerState *sampler = s_device->newSamplerState( desc );
	desc->release();
	if ( !sampler ) {
		MetalHost_Warning( "[Metal] sampler creation failed" );
		return nullptr;
	}
	s_samplerCache.emplace( key, sampler );
	return sampler;
}

static MTL::Texture *GLMetal_NewMultisampleTarget( MTL::PixelFormat format, int width, int height, int samples ) {
	MTL::TextureDescriptor *desc = MTL::TextureDescriptor::alloc()->init();
	desc->setTextureType( MTL::TextureType2DMultisample );
	desc->setPixelFormat( format );
	desc->setWidth( (NS::UInteger)width );
	desc->setHeight( (NS::UInteger)height );
	desc->setSampleCount( (NS::UInteger)samples );
	desc->setUsage( MTL::TextureUsageRenderTarget );
	desc->setStorageMode( MTL::StorageModePrivate );
	MTL::Texture *texture = s_device->newTexture( desc );
	desc->release();
	return texture;
}

MTL::Texture *GLMetal_NewTexture( MTL::TextureType type, MTL::PixelFormat format, int width, int height, int depth,
								  int mipLevels, bool renderTarget, uint32_t swizzle ) {
	if ( !s_device || width <= 0 || height <= 0 ) {
		return nullptr;
	}
	MTL::TextureDescriptor *desc = MTL::TextureDescriptor::alloc()->init();
	desc->setTextureType( type );
	desc->setPixelFormat( format );
	desc->setWidth( (NS::UInteger)width );
	desc->setHeight( (NS::UInteger)height );
	desc->setDepth( type == MTL::TextureType3D ? (NS::UInteger)std::max( depth, 1 ) : 1 );
	desc->setMipmapLevelCount( (NS::UInteger)std::max( mipLevels, 1 ) );
	desc->setUsage( renderTarget ? ( MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget ) : MTL::TextureUsageShaderRead );
	desc->setStorageMode( MTL::StorageModePrivate );
	if ( swizzle ) {
		MTL::TextureSwizzleChannels ch;
		ch.red = (MTL::TextureSwizzle)( swizzle & 0xff );
		ch.green = (MTL::TextureSwizzle)( ( swizzle >> 8 ) & 0xff );
		ch.blue = (MTL::TextureSwizzle)( ( swizzle >> 16 ) & 0xff );
		ch.alpha = (MTL::TextureSwizzle)( ( swizzle >> 24 ) & 0xff );
		desc->setSwizzle( ch );
	}
	MTL::Texture *texture = s_device->newTexture( desc );
	desc->release();
	if ( !texture ) {
		MetalHost_Warning( "[Metal] failed to create %dx%d texture (format %d)", width, height, (int)format );
	}
	return texture;
}

void GLMetal_ReleaseTexture( MTL::Texture *texture ) {
	if ( !texture ) {
		return;
	}
	// a later texture may get the same address: forget it in the bind filter
	for ( int i = 0; i < METAL_MAX_TEXTURE_BINDINGS; i++ ) {
		if ( s_encState.fragmentTexture[i] == texture ) {
			s_encState.fragmentTexture[i] = nullptr;
		}
	}
	// command buffers retain what they use, so frames in flight keep it alive
	texture->release();
}

void GLMetal_ReleaseBuffer( MTL::Buffer *buffer ) {
	if ( !buffer ) {
		return;
	}
	for ( int i = 0; i < METAL_MAX_BUFFER_SLOTS; i++ ) {
		if ( s_encState.vertexBuffer[i] == buffer ) {
			s_encState.vertexBuffer[i] = nullptr;
		}
		if ( s_encState.fragmentBuffer[i] == buffer ) {
			s_encState.fragmentBuffer[i] = nullptr;
		}
	}
	buffer->release();
}

// Commit the pending upload command buffer. Must run before any frame command buffer that
// samples the uploaded textures is committed (same queue => executes in commit order).
static void GLMetal_FlushUploads( bool wait ) {
	if ( !s_uploadCmdBuffer ) {
		return;
	}
	if ( s_uploadBlit ) {
		s_uploadBlit->endEncoding();
		s_uploadBlit->release();
		s_uploadBlit = nullptr;
	}
	s_uploadCmdBuffer->commit();
	if ( wait ) {
		s_uploadCmdBuffer->waitUntilCompleted();
	}
	s_uploadCmdBuffer->release();
	s_uploadCmdBuffer = nullptr;
	s_uploadPendingBytes = 0;
}

static MTL::BlitCommandEncoder *GLMetal_GetUploadBlitEncoder( void ) {
	if ( s_uploadBlit ) {
		return s_uploadBlit;
	}
	if ( !s_queue ) {
		return nullptr;
	}
	s_uploadCmdBuffer = s_queue->commandBuffer();
	if ( !s_uploadCmdBuffer ) {
		return nullptr;
	}
	s_uploadCmdBuffer->retain();
	s_uploadBlit = s_uploadCmdBuffer->blitCommandEncoder();
	if ( !s_uploadBlit ) {
		s_uploadCmdBuffer->release();
		s_uploadCmdBuffer = nullptr;
		return nullptr;
	}
	s_uploadBlit->retain();
	return s_uploadBlit;
}

// The pass descriptor leaves the store actions open (GLMetal_EnsureEncoder). A pass that ends
// in the middle of a frame keeps everything, because the pass that follows loads it. The last
// pass of a frame only has to leave the colour image: storing the samples of all three
// attachments is a large part of the cost of a multisampled frame.
// frameEnd: nothing is drawn into this pass's attachments again this frame.
// endMultisampling: the samples are resolved and dropped; the rest of the frame is drawn into
// the single-sample targets (GLMetal_EndMultisamplingAtNextCopy).
static void GLMetal_EndEncoderIfNeeded( bool frameEnd = false, bool endMultisampling = false ) {
	if ( !s_encoder ) {
		return;
	}
	s_layerActive = false;
	GLMetal_FlushBlendLayer();
	const bool msaa = ( s_sampleCount > 1 && !s_msaaEnded );
	if ( msaa && endMultisampling ) {
		s_encoder->setColorStoreAction( MTL::StoreActionMultisampleResolve, 0 );
		s_encoder->setDepthStoreAction( MTL::StoreActionMultisampleResolve );
		s_encoder->setStencilStoreAction( MTL::StoreActionMultisampleResolve );
		s_msaaEnded = true;
	} else if ( frameEnd ) {
		s_encoder->setColorStoreAction( msaa ? MTL::StoreActionMultisampleResolve : MTL::StoreActionStore, 0 );
		s_encoder->setDepthStoreAction( MTL::StoreActionDontCare );
		s_encoder->setStencilStoreAction( MTL::StoreActionDontCare );
	} else {
		const MTL::StoreAction store = msaa ? MTL::StoreActionStoreAndMultisampleResolve : MTL::StoreActionStore;
		s_encoder->setColorStoreAction( store, 0 );
		s_encoder->setDepthStoreAction( store );
		s_encoder->setStencilStoreAction( store );
	}
	s_encoder->endEncoding();
	s_encoder->release();
	s_encoder = nullptr;
}

MTL::CommandBuffer *GLMetal_CommandBufferForBlit( void ) {
	if ( !s_frameBegun ) {
		GLMetal_BeginFrame();
	}
	GLMetal_EndEncoderIfNeeded();
	s_frameHasWork = true;
	return s_cmdBuffer;
}

void GLMetal_UploadTextureRegion( MTL::Texture *texture, int slice, int level, int x, int y, int z,
								  int width, int height, int depth,
								  const void *bytes, int bytesPerRow, int bytesPerImage ) {
	if ( !texture || !bytes || !s_device || bytesPerImage <= 0 ) {
		return;
	}
	const size_t total = (size_t)bytesPerImage * (size_t)std::max( depth, 1 );
	MTL::Buffer *staging = s_device->newBuffer( bytes, (NS::UInteger)total, MTL::ResourceStorageModeShared );
	if ( !staging ) {
		MetalHost_Warning( "[Metal] texture upload: staging buffer allocation failed (%zu bytes)", total );
		return;
	}
	MTL::BlitCommandEncoder *blit;
	bool ownBlit = false;
	if ( s_frameBegun && s_frameHasWork && s_cmdBuffer ) {
		// the texture may already have been drawn with this frame: keep the upload in order
		GLMetal_EndEncoderIfNeeded();
		blit = s_cmdBuffer->blitCommandEncoder();
		ownBlit = true;
	} else {
		blit = GLMetal_GetUploadBlitEncoder();
	}
	if ( blit ) {
		blit->copyFromBuffer( staging, 0, (NS::UInteger)bytesPerRow, (NS::UInteger)bytesPerImage,
							  MTL::Size::Make( (NS::UInteger)width, (NS::UInteger)height, (NS::UInteger)std::max( depth, 1 ) ),
							  texture, (NS::UInteger)slice, (NS::UInteger)level,
							  MTL::Origin::Make( (NS::UInteger)x, (NS::UInteger)y, (NS::UInteger)z ) );
		if ( ownBlit ) {
			blit->endEncoding();
		} else {
			s_uploadPendingBytes += total;
			if ( s_uploadPendingBytes > kUploadFlushThreshold ) {
				GLMetal_FlushUploads( false );
			}
		}
	}
	staging->release();	// the command buffer retains it until the copy has executed
}

static bool GLMetal_EnsureCaptureBuffer( size_t size ) {
	if ( s_captureBuffer && s_captureBufferSize >= size ) {
		return true;
	}
	if ( s_captureBuffer ) {
		s_captureBuffer->release();
	}
	s_captureBuffer = s_device->newBuffer( (NS::UInteger)size, MTL::ResourceStorageModeShared );
	s_captureBufferSize = s_captureBuffer ? size : 0;
	return s_captureBuffer != nullptr;
}

static bool GLMetal_ClipToScene( int &srcX, int &srcY, int &width, int &height, int &dstX, int &dstY ) {
	if ( srcX < 0 ) { width += srcX; dstX -= srcX; srcX = 0; }
	if ( srcY < 0 ) { height += srcY; dstY -= srcY; srcY = 0; }
	if ( srcX + width > s_width ) { width = s_width - srcX; }
	if ( srcY + height > s_height ) { height = s_height - srcY; }
	return width > 0 && height > 0;
}

// The next colour copy is the main view's _currentRender copy: after it only post-process
// surfaces (heat haze, glass distortion) and 2D are drawn. With multisampling a copy ends the
// scene pass, and opening it again loads every sample of colour, depth and stencil, which with
// 8 samples costs more than the surfaces drawn after it. So the samples are resolved and
// dropped there, and the rest of the frame is drawn without multisampling: post-process
// surfaces get no antialiased edges, which their soft masks mostly hide.
void GLMetal_EndMultisamplingAtNextCopy( void ) {
	s_endMsaaAtNextCopy = s_sampleCount > 1 && !s_msaaEnded && MetalHost_CvarInt( "r_metalPostProcessSingleSample" ) != 0;
}

void GLMetal_CopySceneToTexture( bool depth, int srcX, int srcY, int width, int height,
								 MTL::Texture *dst, int dstX, int dstY, bool replay ) {
	if ( !dst || !s_sceneColor || !GLMetal_ClipToScene( srcX, srcY, width, height, dstX, dstY ) ) {
		return;
	}
	if ( dstX + width > (int)dst->width() ) {
		width = (int)dst->width() - dstX;
	}
	if ( dstY + height > (int)dst->height() ) {
		height = (int)dst->height() - dstY;
	}
	if ( width <= 0 || height <= 0 || dstX < 0 || dstY < 0 ) {
		return;
	}
	const MTL::Origin srcOrigin = MTL::Origin::Make( (NS::UInteger)srcX, (NS::UInteger)srcY, 0 );
	const MTL::Size size = MTL::Size::Make( (NS::UInteger)width, (NS::UInteger)height, 1 );
	const MTL::Origin dstOrigin = MTL::Origin::Make( (NS::UInteger)dstX, (NS::UInteger)dstY, 0 );
	if ( depth ) {
		s_depthCopyFrame = s_frameSerial;
		s_lastDepthCopy = { dst, srcX, srcY, width, height, dstX - srcX, dstY - srcY };
	}
	if ( depth && replay && GLMetal_CanReplayDepth() && s_replayDepth && s_queue ) {
		// draw the depth buffer again instead of ending the scene pass to read it
		const size_t bytes = (size_t)width * (size_t)height * 4;
		if ( GLMetal_EnsureCaptureBuffer( (size_t)s_width * (size_t)s_height * 4 ) && bytes <= s_captureBufferSize ) {
			GLMetal_FlushUploads( false );
			MTL::CommandBuffer *pre = s_queue->commandBuffer();
			if ( pre && GLMetal_ReplayDepthDraws( pre ) ) {
				MTL::BlitCommandEncoder *blit = pre->blitCommandEncoder();
				if ( blit ) {
					blit->copyFromTexture( s_replayDepth, 0, 0, srcOrigin, size, s_captureBuffer, 0,
										   (NS::UInteger)width * 4, (NS::UInteger)bytes, MTL::BlitOptionDepthFromDepthStencil );
					blit->copyFromBuffer( s_captureBuffer, 0, (NS::UInteger)width * 4, (NS::UInteger)bytes, size, dst, 0, 0, dstOrigin );
					blit->endEncoding();
				}
				GLMetal_TrackGPUTime( pre );
				pre->commit();
				s_depthReplayFrame = s_frameSerial;
				return;
			}
		}
	}
	if ( !depth ) {
		const bool endMsaa = s_endMsaaAtNextCopy;
		s_endMsaaAtNextCopy = false;
		if ( endMsaa && s_encoder ) {
			GLMetal_EndEncoderIfNeeded( false, true );
		}
	}
	MTL::CommandBuffer *cmd = GLMetal_CommandBufferForBlit();
	if ( !cmd ) {
		return;
	}
	MTL::BlitCommandEncoder *blit = cmd->blitCommandEncoder();
	if ( !blit ) {
		return;
	}
	if ( !depth ) {
		blit->copyFromTexture( s_sceneColor, 0, 0, srcOrigin, size, dst, 0, 0, dstOrigin );
	} else {
		// depth plane -> buffer -> R32Float texture (a blit cannot convert formats)
		const size_t bytes = (size_t)width * (size_t)height * 4;
		if ( GLMetal_EnsureCaptureBuffer( (size_t)s_width * (size_t)s_height * 4 ) && bytes <= s_captureBufferSize ) {
			blit->copyFromTexture( s_sceneDepth, 0, 0, srcOrigin, size, s_captureBuffer, 0,
								   (NS::UInteger)width * 4, (NS::UInteger)bytes, MTL::BlitOptionDepthFromDepthStencil );
			blit->copyFromBuffer( s_captureBuffer, 0, (NS::UInteger)width * 4, (NS::UInteger)bytes, size, dst, 0, 0, dstOrigin );
		}
	}
	blit->endEncoding();
}

void GLMetal_FlushAndWait( void ) {
	if ( !s_frameBegun || !s_cmdBuffer ) {
		GLMetal_FlushUploads( true );
		return;
	}
	GLMetal_EndEncoderIfNeeded();
	GLMetal_FlushUploads( false );
	// the recorded depth draws use buffers and textures this command buffer kept alive
	s_depthRecordValid = false;
	s_depthDraws.clear();
	GLMetal_TrackGPUTime( s_cmdBuffer );
	s_cmdBuffer->commit();
	s_cmdBuffer->waitUntilCompleted();
	if ( s_cmdBuffer->status() == MTL::CommandBufferStatusError ) {
		MetalHost_Warning( "[Metal] command buffer failed: %s", MetalErrorString( s_cmdBuffer->error() ) );
	}
	s_cmdBuffer->release();
	// the frame goes on in a new command buffer
	s_cmdBuffer = s_queue->commandBuffer();
	if ( s_cmdBuffer ) {
		s_cmdBuffer->retain();
	}
	s_frameHasWork = false;
}

bool GLMetal_ReadScenePixels( int what, int x, int y, int width, int height, void *dest ) {
	if ( !dest || width <= 0 || height <= 0 || !s_sceneColor ) {
		return false;
	}
	if ( x < 0 || y < 0 || x + width > s_width || y + height > s_height ) {
		MetalHost_Warning( "[Metal] GLMetal_ReadScenePixels rect %d,%d %dx%d outside the %dx%d scene", x, y, width, height, s_width, s_height );
		return false;
	}
	const size_t pixelBytes = ( what == METAL_READ_STENCIL ) ? 1 : 4;
	const size_t bytes = (size_t)width * (size_t)height * pixelBytes;
	if ( !GLMetal_EnsureCaptureBuffer( std::max( bytes, (size_t)s_width * (size_t)s_height * 4 ) ) ) {
		return false;
	}
	MTL::CommandBuffer *cmd = GLMetal_CommandBufferForBlit();
	if ( !cmd ) {
		return false;
	}
	MTL::BlitCommandEncoder *blit = cmd->blitCommandEncoder();
	if ( !blit ) {
		return false;
	}
	const MTL::Origin origin = MTL::Origin::Make( (NS::UInteger)x, (NS::UInteger)y, 0 );
	const MTL::Size size = MTL::Size::Make( (NS::UInteger)width, (NS::UInteger)height, 1 );
	if ( what == METAL_READ_COLOR ) {
		blit->copyFromTexture( s_sceneColor, 0, 0, origin, size, s_captureBuffer, 0, (NS::UInteger)width * 4, (NS::UInteger)bytes );
	} else if ( what == METAL_READ_DEPTH ) {
		blit->copyFromTexture( s_sceneDepth, 0, 0, origin, size, s_captureBuffer, 0, (NS::UInteger)width * 4, (NS::UInteger)bytes,
							   MTL::BlitOptionDepthFromDepthStencil );
	} else {
		blit->copyFromTexture( s_sceneDepth, 0, 0, origin, size, s_captureBuffer, 0, (NS::UInteger)width, (NS::UInteger)bytes,
							   MTL::BlitOptionStencilFromDepthStencil );
	}
	blit->endEncoding();
	GLMetal_FlushAndWait();

	const unsigned char *src = (const unsigned char *)s_captureBuffer->contents();
	if ( what == METAL_READ_COLOR ) {
		// BGRA -> RGBA
		unsigned char *out = (unsigned char *)dest;
		const size_t n = (size_t)width * (size_t)height;
		for ( size_t i = 0; i < n; i++ ) {
			out[i * 4 + 0] = src[i * 4 + 2];
			out[i * 4 + 1] = src[i * 4 + 1];
			out[i * 4 + 2] = src[i * 4 + 0];
			out[i * 4 + 3] = src[i * 4 + 3];
		}
	} else {
		memcpy( dest, src, bytes );
	}
	return true;
}

// ============================================================================
// Render pass
// ============================================================================

static bool GLMetal_EnsureSceneTargets( int width, int height ) {
	if ( s_sceneColor && s_width == width && s_height == height ) {
		return true;
	}
	if ( width <= 0 || height <= 0 ) {
		return false;
	}
	if ( s_sceneColor ) {
		s_sceneColor->release();
		s_sceneColor = nullptr;
	}
	if ( s_sceneDepth ) {
		s_sceneDepth->release();
		s_sceneDepth = nullptr;
	}
	if ( s_replayDepth ) {
		s_replayDepth->release();
		s_replayDepth = nullptr;
	}
	GLMetal_ReleaseLayerTargets();
	if ( s_msaaColor ) {
		s_msaaColor->release();
		s_msaaColor = nullptr;
	}
	if ( s_msaaDepth ) {
		s_msaaDepth->release();
		s_msaaDepth = nullptr;
	}
	if ( s_sampleCount > 1 ) {
		s_msaaColor = GLMetal_NewMultisampleTarget( s_colorPixelFormat, width, height, s_sampleCount );
		s_msaaDepth = GLMetal_NewMultisampleTarget( s_depthPixelFormat, width, height, s_sampleCount );
		if ( !s_msaaColor || !s_msaaDepth ) {
			MetalHost_Warning( "[Metal] failed to create %dx multisample targets", s_sampleCount );
			return false;
		}
	}
	s_replayDepth = GLMetal_NewTexture( MTL::TextureType2D, s_depthPixelFormat, width, height, 1, 1, true, 0 );
	s_sceneColor = GLMetal_NewTexture( MTL::TextureType2D, s_colorPixelFormat, width, height, 1, 1, true, 0 );
	s_sceneDepth = GLMetal_NewTexture( MTL::TextureType2D, s_depthPixelFormat, width, height, 1, 1, true, 0 );
	s_width = width;
	s_height = height;
	return s_sceneColor && s_sceneDepth;
}

static bool GLMetal_EnsureEncoder( void ) {
	if ( s_encoder ) {
		return true;
	}
	if ( !s_frameBegun ) {
		GLMetal_BeginFrame();
	}
	if ( !s_cmdBuffer || !s_sceneColor || !s_sceneDepth ) {
		return false;
	}
	// the first pass of a frame clears; a pass reopened after a blit loads what is there
	const MTL::LoadAction load = s_frameCleared ? MTL::LoadActionLoad : MTL::LoadActionClear;
	MTL::RenderPassDescriptor *desc = MTL::RenderPassDescriptor::renderPassDescriptor();
	const bool msaa = ( s_sampleCount > 1 && !s_msaaEnded );
	const MTL::StoreAction store = MTL::StoreActionUnknown;	// chosen when the pass ends
	desc->colorAttachments()->object( 0 )->setTexture( msaa ? s_msaaColor : s_sceneColor );
	desc->colorAttachments()->object( 0 )->setLoadAction( load );
	desc->colorAttachments()->object( 0 )->setClearColor( MTL::ClearColor::Make( 0.0, 0.0, 0.0, 1.0 ) );
	desc->colorAttachments()->object( 0 )->setStoreAction( store );
	desc->depthAttachment()->setTexture( msaa ? s_msaaDepth : s_sceneDepth );
	desc->depthAttachment()->setLoadAction( load );
	desc->depthAttachment()->setClearDepth( 1.0 );
	desc->depthAttachment()->setStoreAction( store );
	desc->stencilAttachment()->setTexture( msaa ? s_msaaDepth : s_sceneDepth );
	desc->stencilAttachment()->setLoadAction( load );
	desc->stencilAttachment()->setClearStencil( 0 );
	desc->stencilAttachment()->setStoreAction( store );
	if ( msaa ) {
		// the single-sample textures are what copies, readback and present use
		desc->colorAttachments()->object( 0 )->setResolveTexture( s_sceneColor );
		desc->depthAttachment()->setResolveTexture( s_sceneDepth );
		desc->depthAttachment()->setDepthResolveFilter( MTL::MultisampleDepthResolveFilterSample0 );
		desc->stencilAttachment()->setResolveTexture( s_sceneDepth );
		desc->stencilAttachment()->setStencilResolveFilter( MTL::MultisampleStencilResolveFilterSample0 );
	}

	s_encoder = s_cmdBuffer->renderCommandEncoder( desc );
	if ( !s_encoder ) {
		return false;
	}
	s_encoder->retain();
	s_frameCleared = true;
	s_frameHasWork = true;
	GLMetal_OnEncoderOpened();
	return true;
}

MTL::RenderCommandEncoder *GLMetal_CurrentRenderEncoder( void ) {
	if ( !GLMetal_EnsureEncoder() ) {
		return nullptr;
	}
	if ( !s_layerActive && !s_layerDraws.empty() ) {
		// something else is about to be drawn: the layer collected so far goes first
		GLMetal_FlushBlendLayer();
	}
	return s_encoder;
}

MTL::RenderCommandEncoder *GLMetal_OpenRenderEncoder( void ) {
	return s_encoder;
}

// ============================================================================
// Frame pacing (r_metalFrameStats, r_metalNoPresent, r_metalPresentHz, r_metalHidden)
// ============================================================================

// r_metalFrameStats N: every N frames print where frame time went: blocked waiting for a
// free in-flight frame slot (the GPU is behind), blocked in nextDrawable (the window
// system is pacing presentation), and everything else. "backend" is the CPU time from
// GLMetal_BeginFrame to the end of GLMetal_Present with both waits excluded.
static struct {
	uint64_t lastFrameStart;	// ns
	uint64_t beginNs;
	uint64_t frameNs, slotWaitNs, drawableWaitNs, backendNs;
	uint64_t waitNsAtFrameStart;
	int      frames;
	int      presented;
	int      interval;
} s_frameStats;

// Refresh rate of the main display, re-read about once a second.
static double GLMetal_DisplayRefreshRate( void ) {
	static double rate = 0.0;
	static uint64_t lastQueryNs = 0;
	const uint64_t now = GLMetal_NowNs();

	if ( rate <= 0.0 || now - lastQueryNs > 1000000000ull ) {
		lastQueryNs = now;
		rate = 0.0;
		CGDisplayModeRef mode = CGDisplayCopyDisplayMode( CGMainDisplayID() );
		if ( mode ) {
			rate = CGDisplayModeGetRefreshRate( mode );
			CGDisplayModeRelease( mode );
		}
		if ( rate < 1.0 ) {
			rate = 60.0;
		}
	}
	return rate;
}

static void GLMetal_FrameStatsBeginFrame( void ) {
	s_frameStats.interval = MetalHost_CvarInt( "r_metalFrameStats" );
	if ( s_frameStats.interval <= 0 ) {
		s_frameStats.lastFrameStart = 0;
		s_frameStats.frames = 0;
		s_frameStats.frameNs = s_frameStats.slotWaitNs = s_frameStats.drawableWaitNs = s_frameStats.backendNs = 0;
		return;
	}
	const uint64_t now = GLMetal_NowNs();
	if ( s_frameStats.lastFrameStart ) {
		s_frameStats.frameNs += now - s_frameStats.lastFrameStart;
		s_frameStats.frames++;
	}
	s_frameStats.lastFrameStart = now;
	if ( s_frameStats.frames >= s_frameStats.interval ) {
		const double n = (double)s_frameStats.frames * 1e6;	// ns -> ms per frame
		const double frameMs = s_frameStats.frameNs / n;
		const double slotMs = s_frameStats.slotWaitNs / n;
		const double drawableMs = s_frameStats.drawableWaitNs / n;
		MetalHost_Printf( "[Metal] frame stats (%d frames, %d presented): %.3f ms/frame = %.3f waiting for GPU slot + %.3f waiting for drawable + %.3f other; backend %.3f ms; gpu %.3f ms\n",
						  s_frameStats.frames, s_frameStats.presented, frameMs, slotMs, drawableMs, frameMs - slotMs - drawableMs,
						  s_frameStats.backendNs / n, s_gpuNs.exchange( 0 ) / n );
		s_frameStats.frames = 0;
		s_frameStats.presented = 0;
		s_frameStats.frameNs = s_frameStats.slotWaitNs = s_frameStats.drawableWaitNs = s_frameStats.backendNs = 0;
	}
	s_frameStats.waitNsAtFrameStart = s_frameStats.slotWaitNs + s_frameStats.drawableWaitNs;
	s_frameStats.beginNs = GLMetal_NowNs();
}

static void GLMetal_AcquireInFlightFrameSlot( void ) {
	if ( !s_inFlightSemaphore || s_inFlightFramePendingSignal ) {
		return;
	}
	const uint64_t waitStart = GLMetal_NowNs();
	dispatch_semaphore_wait( s_inFlightSemaphore, DISPATCH_TIME_FOREVER );
	s_frameStats.slotWaitNs += GLMetal_NowNs() - waitStart;
	s_inFlightFramePendingSignal = true;
}

static void GLMetal_ReleaseInFlightFrameSlot( void ) {
	if ( !s_inFlightSemaphore || !s_inFlightFramePendingSignal ) {
		return;
	}
	dispatch_semaphore_signal( s_inFlightSemaphore );
	s_inFlightFramePendingSignal = false;
}

static void GLMetal_UpdateDisplaySyncSetting( bool force ) {
	if ( !s_layer ) {
		return;
	}
	const int swapInterval = MetalHost_CvarInt( "r_swapInterval" );
	if ( !force && s_appliedSwapInterval == swapInterval ) {
		return;
	}
	const bool displaySyncEnabled = ( swapInterval != 0 );
	// CA::MetalLayer::displaySyncEnabled is not in the metal-cpp headers
	static SEL setDisplaySync = sel_registerName( "setDisplaySyncEnabled:" );
	typedef void ( *BoolSetter )( id, SEL, bool );
	( (BoolSetter)objc_msgSend )( (id)s_layer, setDisplaySync, displaySyncEnabled );
	MetalHost_Printf( "[Metal] r_swapInterval %d -> displaySyncEnabled %s\n", swapInterval, displaySyncEnabled ? "ON" : "OFF" );
	s_appliedSwapInterval = swapInterval;
}

// A frame is either presented (the scene is drawn into a drawable of the layer) or only
// rendered into the scene target.
//
// nextDrawable blocks until the window system hands a drawable back, which it does at about
// the display's refresh rate: presenting every frame caps the frame rate there. A display
// cannot show more frames than its refresh rate anyway, so with vsync off (r_swapInterval 0)
// only one frame per refresh interval is presented.
//   r_metalPresentHz -1: presents per second = the display's refresh rate (default)
//   r_metalPresentHz  0: present every frame
//   r_metalPresentHz  N: at most N presents per second
// r_metalNoPresent 1 never presents; r_metalHidden 1 (no visible window) implies it.
static bool GLMetal_ShouldPresent( void ) {
	if ( MetalHost_CvarInt( "r_metalNoPresent" ) || MetalHost_CvarInt( "r_metalHidden" ) ) {
		return false;
	}
	const int presentHz = MetalHost_CvarInt( "r_metalPresentHz" );
	if ( MetalHost_CvarInt( "r_swapInterval" ) == 0 && presentHz != 0 ) {
		double hz = (double)presentHz;
		if ( hz < 0.0 ) {
			hz = GLMetal_DisplayRefreshRate();
		}
		const uint64_t now = GLMetal_NowNs();
		// a little under one refresh interval, so that no refresh goes without a new frame
		const uint64_t interval = (uint64_t)( 0.9 * 1e9 / hz );
		// s_lastPresentNs is the time the last drawable was handed out (GLMetal_Present).
		// Measuring from before the wait for it would let that wait use up the interval,
		// and every frame would present and block again: a cap at the refresh rate.
		if ( s_lastPresentNs && now - s_lastPresentNs < interval ) {
			return false;
		}
	}
	return true;
}

// ============================================================================
// Device lifecycle
// ============================================================================

static const char *kPresentMSL =
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct PresentOut { float4 position [[position]]; float2 uv; };\n"
	"vertex PresentOut present_vs( uint vid [[vertex_id]] ) {\n"
	"	PresentOut o;\n"
	"	float2 p = float2( float( ( vid << 1 ) & 2 ), float( vid & 2 ) );\n"
	"	o.position = float4( p * 2.0 - 1.0, 0.0, 1.0 );\n"
	// the scene is stored upside down: row 0 is the bottom of the picture
	"	o.uv = float2( p.x, p.y );\n"
	"	return o;\n"
	"}\n"
	"fragment float4 present_fs( PresentOut in [[stage_in]], texture2d<float> scene [[texture(0)]], sampler smp [[sampler(0)]] ) {\n"
	"	return float4( scene.sample( smp, in.uv ).rgb, 1.0 );\n"
	"}\n";

static bool GLMetal_CreatePresentPipeline( void ) {
	MTL::Library *lib = GLMetal_LoadShaderLibraryFromSource( kPresentMSL, "present" );
	if ( !lib ) {
		return false;
	}
	MTL::Function *vs = lib->newFunction( NS::String::string( "present_vs", NS::UTF8StringEncoding ) );
	MTL::Function *fs = lib->newFunction( NS::String::string( "present_fs", NS::UTF8StringEncoding ) );
	MTL::RenderPipelineDescriptor *desc = MTL::RenderPipelineDescriptor::alloc()->init();
	desc->setVertexFunction( vs );
	desc->setFragmentFunction( fs );
	desc->colorAttachments()->object( 0 )->setPixelFormat( s_colorPixelFormat );
	NS::Error *error = nullptr;
	s_presentPipeline = s_device->newRenderPipelineState( desc, &error );
	desc->release();
	if ( vs ) {
		vs->release();
	}
	if ( fs ) {
		fs->release();
	}
	lib->release();
	if ( !s_presentPipeline ) {
		MetalHost_Warning( "[Metal] present pipeline creation failed: %s", MetalErrorString( error ) );
		return false;
	}
	MTL::SamplerDescriptor *sd = MTL::SamplerDescriptor::alloc()->init();
	sd->setMinFilter( MTL::SamplerMinMagFilterLinear );
	sd->setMagFilter( MTL::SamplerMinMagFilterLinear );
	sd->setSAddressMode( MTL::SamplerAddressModeClampToEdge );
	sd->setTAddressMode( MTL::SamplerAddressModeClampToEdge );
	s_presentSampler = s_device->newSamplerState( sd );
	sd->release();
	return s_presentSampler != nullptr;
}

int GLMetal_SampleCount( void ) { return s_sampleCount; }

bool GLMetal_DeviceInit( void *layerPtr, int w, int h, int samples ) {
	s_device = MTL::CreateSystemDefaultDevice();
	if ( !s_device ) {
		MetalHost_Warning( "[Metal] MTL::CreateSystemDefaultDevice() failed" );
		return false;
	}
	// the highest supported sample count that does not exceed the request
	s_sampleCount = 1;
	for ( int n = std::min( samples, 8 ); n > 1; n-- ) {
		if ( s_device->supportsTextureSampleCount( (NS::UInteger)n ) ) {
			s_sampleCount = n;
			break;
		}
	}
	s_layer = reinterpret_cast<CA::MetalLayer *>( layerPtr );
	s_layer->setDevice( s_device );
	s_layer->setPixelFormat( s_colorPixelFormat );
	s_layer->setDrawableSize( CGSizeMake( (CGFloat)w, (CGFloat)h ) );
	s_appliedSwapInterval = INT_MIN;
	GLMetal_UpdateDisplaySyncSetting( true );

	s_queue = s_device->newCommandQueue();
	if ( !s_queue ) {
		MetalHost_Warning( "[Metal] newCommandQueue failed" );
		return false;
	}
	s_firstFrame = true;
	s_frameBegun = false;
	s_inFlightFramePendingSignal = false;
	if ( !s_inFlightSemaphore ) {
		s_inFlightSemaphore = dispatch_semaphore_create( s_maxFramesInFlight );
	}
	memset( &s_want, 0, sizeof( s_want ) );
	s_want.viewport.width = w;
	s_want.viewport.height = h;
	s_want.viewport.zfar = 1.0;
	s_want.scissor.width = (NS::UInteger)w;
	s_want.scissor.height = (NS::UInteger)h;
	s_want.cullMode = MTL::CullModeNone;
	s_want.fillMode = MTL::TriangleFillModeFill;

	MetalHost_Printf( "[Metal] device: %s\n", s_device->name()->utf8String() );
	MetalHost_Printf( "[Metal] drawable size: %dx%d, %d sample(s) per pixel\n", w, h, s_sampleCount );

	if ( !GLMetal_EnsureSceneTargets( w, h ) || !GLMetal_CreatePresentPipeline() ) {
		return false;
	}
	// now rather than at the first soft particle: compiling it can take a tenth of a second
	GLMetal_EnsureLayerCompositePipeline();
	return true;
}

void GLMetal_Resize( int w, int h ) {
	if ( !s_layer || w <= 0 || h <= 0 || ( w == s_width && h == s_height ) ) {
		return;
	}
	if ( s_frameBegun ) {
		GLMetal_FlushAndWait();
		GLMetal_EndEncoderIfNeeded();
	}
	s_layer->setDrawableSize( CGSizeMake( (CGFloat)w, (CGFloat)h ) );
	GLMetal_EnsureSceneTargets( w, h );
	s_frameCleared = false;
}

bool GLMetal_BeginFrame( void ) {
	if ( ( s_frameCompiles || s_framePipelines ) && MetalHost_CvarInt( "r_metalDebug" ) >= 1 ) {
		MetalHost_Printf( "[Metal] frame %llu: %d shader compiles %.1f ms, %d pipelines %.1f ms\n", (unsigned long long)s_frameSerial,
						  s_frameCompiles, s_frameCompileSeconds * 1000.0, s_framePipelines, s_framePipelineSeconds * 1000.0 );
	}
	s_frameCompileSeconds = s_framePipelineSeconds = 0.0;
	s_frameCompiles = s_framePipelines = 0;
	if ( s_frameBegun ) {
		return true;
	}
	if ( !s_layer || !s_queue ) {
		return false;
	}
	GLMetal_FrameStatsBeginFrame();
	GLMetal_AcquireInFlightFrameSlot();
	GLMetal_UpdateDisplaySyncSetting( false );

	// The in-flight semaphore guarantees the frame that last used this arena slot has completed.
	s_frameArenaIndex = ( s_frameArenaIndex + 1 ) % (int)( sizeof( s_frameArenas ) / sizeof( s_frameArenas[0] ) );
	GLMetal_ResetFrameArena( s_frameArenas[s_frameArenaIndex] );

	// Submit textures created since the last frame now so the GPU can start on them early.
	GLMetal_FlushUploads( false );

	s_cmdBuffer = s_queue->commandBuffer();
	if ( !s_cmdBuffer ) {
		MetalHost_Warning( "[Metal] commandBuffer() returned null, skipping frame" );
		GLMetal_ReleaseInFlightFrameSlot();
		return false;
	}
	s_cmdBuffer->retain();
	s_frameCleared = false;
	s_frameHasWork = false;
	s_frameBegun = true;
	// record depth writes while depth copies are being asked for
	s_depthDraws.clear();
	s_depthRecordValid = ( s_replayDepth && s_depthCopyFrame + 2 >= s_frameSerial && s_depthCopyFrame <= s_frameSerial
						   && !MetalHost_CvarInt( "r_metalNoDepthReplay" ) );
	s_layerScale = MetalHost_CvarInt( "r_metalParticleLayer" );
	if ( s_layerScale != 1 && s_layerScale != 2 && s_layerScale != 4 ) {
		s_layerScale = 0;
	}
	s_layerTargetsUsed = 0;
	s_layerActive = false;
	s_msaaEnded = false;
	s_endMsaaAtNextCopy = false;
	s_lastDepthCopy.tex = nullptr;
	return true;
}

void GLMetal_Present( void ) {
	if ( !s_frameBegun ) {
		GLMetal_BeginFrame();
	}
	if ( !s_cmdBuffer ) {
		GLMetal_ReleaseInFlightFrameSlot();
		s_frameBegun = false;
		return;
	}
	GLMetal_EndEncoderIfNeeded( true );

	const bool present = GLMetal_ShouldPresent() && s_presentPipeline;
	// With vsync, nextDrawable waits for the display. The frame is committed before that wait
	// so the GPU works on it meanwhile (and does not idle into a lower clock); a second
	// command buffer copies it to the drawable. Commit order on the queue keeps them in order.
	if ( present && MetalHost_CvarInt( "r_metalEarlyCommit" ) ) {
		MTL::CommandBuffer *copyBuffer = s_queue->commandBuffer();
		if ( copyBuffer ) {
			GLMetal_FlushUploads( false );
			GLMetal_TrackGPUTime( s_cmdBuffer );
			s_cmdBuffer->commit();
			s_cmdBuffer->release();
			copyBuffer->retain();
			s_cmdBuffer = copyBuffer;
		}
	}

	if ( present ) {
		const uint64_t waitStart = GLMetal_NowNs();
		s_drawable = s_layer->nextDrawable();
		s_lastPresentNs = GLMetal_NowNs();
		s_frameStats.drawableWaitNs += s_lastPresentNs - waitStart;
		if ( !s_drawable || !s_drawable->texture() ) {
			MetalHost_Warning( "[Metal] nextDrawable() returned null" );
			s_drawable = nullptr;
		} else {
			s_drawable->retain();
			s_frameStats.presented++;
			MTL::RenderPassDescriptor *desc = MTL::RenderPassDescriptor::renderPassDescriptor();
			desc->colorAttachments()->object( 0 )->setTexture( s_drawable->texture() );
			desc->colorAttachments()->object( 0 )->setLoadAction( MTL::LoadActionDontCare );
			desc->colorAttachments()->object( 0 )->setStoreAction( MTL::StoreActionStore );
			MTL::RenderCommandEncoder *enc = s_cmdBuffer->renderCommandEncoder( desc );
			if ( enc ) {
				enc->setRenderPipelineState( s_presentPipeline );
				enc->setFragmentTexture( s_sceneColor, 0 );
				enc->setFragmentSamplerState( s_presentSampler, 0 );
				enc->drawPrimitives( MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3 );
				enc->endEncoding();
			}
		}
	}

	// Texture uploads recorded before this frame had work must execute before it.
	GLMetal_FlushUploads( false );

	MTL::CommandBuffer *cmdBuffer = s_cmdBuffer;
	s_frameSerial++;

	dispatch_semaphore_t inFlightSemaphore = s_inFlightSemaphore;	// by value
	cmdBuffer->addCompletedHandler( [inFlightSemaphore]( MTL::CommandBuffer * ) {
		if ( inFlightSemaphore ) {
			dispatch_semaphore_signal( inFlightSemaphore );
		}
	} );
	s_inFlightFramePendingSignal = false;

	if ( s_drawable ) {
		cmdBuffer->presentDrawable( s_drawable );
	}
	GLMetal_TrackGPUTime( cmdBuffer );
	cmdBuffer->commit();

	if ( s_firstFrame ) {
		MetalHost_Printf( "[Metal] first frame %dx%d\n", s_width, s_height );
		s_firstFrame = false;
	}

	s_cmdBuffer = nullptr;
	cmdBuffer->release();
	if ( s_drawable ) {
		s_drawable->release();
		s_drawable = nullptr;
	}
	if ( s_frameStats.interval > 0 && s_frameStats.lastFrameStart ) {
		const uint64_t waited = s_frameStats.slotWaitNs + s_frameStats.drawableWaitNs - s_frameStats.waitNsAtFrameStart;
		s_frameStats.backendNs += GLMetal_NowNs() - s_frameStats.lastFrameStart - waited;
	}
	s_frameBegun = false;
}

void GLMetal_Shutdown( void ) {
	GLMetal_EndEncoderIfNeeded();
	if ( s_cmdBuffer ) {
		GLMetal_ReleaseInFlightFrameSlot();
		s_cmdBuffer->commit();
		s_cmdBuffer->release();
		s_cmdBuffer = nullptr;
	} else {
		GLMetal_ReleaseInFlightFrameSlot();
	}
	s_frameBegun = false;

	// Drain the GPU before tearing anything down. Frames presented earlier still have
	// in-flight command buffers whose completion handlers signal s_inFlightSemaphore.
	// A committed empty command buffer on the same serial queue completes only after every
	// prior frame, so waiting on it guarantees the GPU is idle.
	GLMetal_FlushUploads( true );
	if ( s_queue ) {
		MTL::CommandBuffer *drain = s_queue->commandBuffer();
		if ( drain ) {
			drain->commit();
			drain->waitUntilCompleted();
		}
	}
	if ( s_drawable ) {
		s_drawable->release();
		s_drawable = nullptr;
	}
	for ( auto &kv : s_pipelineCache ) {
		if ( kv.second ) {
			kv.second->release();
		}
	}
	s_pipelineCache.clear();
	for ( auto &kv : s_depthPipelineCache ) {
		if ( kv.second ) {
			kv.second->release();
		}
	}
	s_depthPipelineCache.clear();
	for ( auto &kv : s_layerPipelineCache ) {
		if ( kv.second ) {
			kv.second->release();
		}
	}
	s_layerPipelineCache.clear();
	if ( s_layerCompositePipeline ) {
		s_layerCompositePipeline->release();
		s_layerCompositePipeline = nullptr;
	}
	if ( s_layerCompositeDepthPipeline ) {
		s_layerCompositeDepthPipeline->release();
		s_layerCompositeDepthPipeline = nullptr;
	}
	s_lastDepthCopy.tex = nullptr;
	s_layerGroupDepth.tex = nullptr;
	GLMetal_ReleaseLayerTargets();
	s_depthDraws.clear();
	s_layerDraws.clear();
	s_depthRecordValid = false;
	s_layerActive = false;
	s_curPipelineKey = { -1, 0, 0 };
	s_depthCopyFrame = ~0ull;
	s_depthReplayFrame = ~0ull;
	s_lastPipeline = nullptr;
	s_lastPipelineKey = { -1, 0, 0 };
	for ( auto &rec : s_programs ) {
		rec.vert->release();
		rec.frag->release();
	}
	s_programs.clear();
	for ( auto &kv : s_depthStencilCache ) {
		kv.second->release();
	}
	s_depthStencilCache.clear();
	s_lastDepthStencilState = nullptr;
	s_lastDepthStencilKey = ~0ull;
	for ( auto &kv : s_samplerCache ) {
		kv.second->release();
	}
	s_samplerCache.clear();
	GLMetal_ReleaseFrameArenas();
	if ( s_presentPipeline ) { s_presentPipeline->release(); s_presentPipeline = nullptr; }
	if ( s_presentSampler ) { s_presentSampler->release(); s_presentSampler = nullptr; }
	if ( s_captureTexture ) { s_captureTexture->release(); s_captureTexture = nullptr; }
	if ( s_captureBuffer ) { s_captureBuffer->release(); s_captureBuffer = nullptr; s_captureBufferSize = 0; }
	if ( s_msaaColor ) { s_msaaColor->release(); s_msaaColor = nullptr; }
	if ( s_msaaDepth ) { s_msaaDepth->release(); s_msaaDepth = nullptr; }
	if ( s_sceneColor ) { s_sceneColor->release(); s_sceneColor = nullptr; }
	if ( s_sceneDepth ) { s_sceneDepth->release(); s_sceneDepth = nullptr; }
	s_width = s_height = 0;
	memset( &s_want, 0, sizeof( s_want ) );
	memset( &s_encState, 0, sizeof( s_encState ) );
	if ( s_queue ) { s_queue->release(); s_queue = nullptr; }
	// the semaphore is kept: its count is back at s_maxFramesInFlight and the next init reuses it
	s_layer = nullptr;
	if ( s_device ) { s_device->release(); s_device = nullptr; }
	s_lastPresentNs = 0;
}
