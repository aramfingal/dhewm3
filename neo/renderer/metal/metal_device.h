/*
metal_device.h - Metal device layer: device, drawable and frame pacing, per-frame transient
memory, render pass and encoder state, pipeline / depth-stencil / sampler caches, texture
uploads, readback and MSL compilation.

Derived from the ioq3 Metal renderer's device layer (aramfingal/ioq3
code/renderermetal/tr_metal_device.cpp). docs/metal/DEVICE_LAYER_CHANGES.md lists what was
kept, what changed and why, so the two can later become one shared library.
*/
#ifndef METAL_DEVICE_H
#define METAL_DEVICE_H

#include <stdint.h>
#include <stddef.h>

#include <Metal/Metal.hpp>

// ---- device and frame lifecycle -------------------------------------------------------

// layerPtr is a CAMetalLayer*
// `samples`: requested samples per pixel (0 or 1: no multisampling)
bool GLMetal_DeviceInit( void *layerPtr, int w, int h, int samples );
int GLMetal_SampleCount( void );
void GLMetal_Shutdown( void );
void GLMetal_Resize( int w, int h );

// Begins a frame if none is in progress. Returns true when a frame is in progress afterwards.
bool GLMetal_BeginFrame( void );
// Ends the scene pass, draws the scene into the drawable (unless presenting is off) and commits.
void GLMetal_Present( void );
// Commits everything encoded so far and waits for the GPU; the frame continues afterwards.
void GLMetal_FlushAndWait( void );
// Number of frames presented so far.
uint64_t GLMetal_FrameSerial( void );

MTL::Device *GLMetal_Device( void );
MTL::PixelFormat GLMetal_SceneColorFormat( void );
MTL::PixelFormat GLMetal_SceneDepthFormat( void );
void GLMetal_GetSceneSize( int *width, int *height );

// ---- per-frame transient memory --------------------------------------------------------

// Returns a CPU pointer to `size` bytes that stay valid until the frame has been presented,
// and the buffer/offset to bind. nullptr if no frame could be begun.
void *GLMetal_FrameAlloc( size_t size, size_t alignment, MTL::Buffer **outBuffer, NS::UInteger *outOffset );

// ---- render pass -------------------------------------------------------------------------

// The scene pass encoder, opened if necessary. nullptr if no frame could be begun.
MTL::RenderCommandEncoder *GLMetal_CurrentRenderEncoder( void );
// Changes whenever a new encoder is opened (everything bound to the old one is gone).
uint32_t GLMetal_EncoderGeneration( void );
// The open encoder or nullptr: starts no pass.
MTL::RenderCommandEncoder *GLMetal_OpenRenderEncoder( void );
// The frame's command buffer with the scene pass ended, for blit work that must be ordered
// with the draws around it. The next draw reopens the pass and loads the attachments.
MTL::CommandBuffer *GLMetal_CommandBufferForBlit( void );
MTL::Texture *GLMetal_SceneColorTexture( void );
MTL::Texture *GLMetal_SceneDepthTexture( void );

// ---- encoder state, filtered against what the encoder already has ----------------------

void GLMetal_SetRenderPipelineState( MTL::RenderPipelineState *pso );
void GLMetal_SetVertexBuffer( MTL::Buffer *buffer, NS::UInteger offset, NS::UInteger slot );
void GLMetal_SetFragmentBuffer( MTL::Buffer *buffer, NS::UInteger offset, NS::UInteger slot );
// Copies bytes into frame memory and binds the range at both stages.
void GLMetal_SetSharedUniforms( const void *bytes, size_t size, NS::UInteger vertexSlot, NS::UInteger fragmentSlot );
void GLMetal_SetFragmentTexture( MTL::Texture *texture, MTL::SamplerState *sampler, NS::UInteger slot );
// Rectangles are in target pixels with the origin at row 0 of the target.
void GLMetal_SetViewport( int x, int y, int w, int h, float znear, float zfar );
void GLMetal_SetScissor( int x, int y, int w, int h );
void GLMetal_SetCullMode( MTL::CullMode mode );
void GLMetal_SetFillMode( MTL::TriangleFillMode mode );
void GLMetal_SetDepthBias( float depthBias, float slopeScale, float clamp );

struct MetalStencilFace {
	uint8_t func;		// MTL::CompareFunction
	uint8_t sfail;		// MTL::StencilOperation
	uint8_t zfail;
	uint8_t zpass;
};
struct MetalDepthStencilKey {
	uint8_t depthFunc;	// MTL::CompareFunction
	uint8_t depthWrite;
	uint8_t stencilEnable;
	uint8_t readMask;
	uint8_t writeMask;
	MetalStencilFace front;
	MetalStencilFace back;
};
void GLMetal_SetDepthStencil( const MetalDepthStencilKey &key, uint32_t stencilRef );

// ---- pipelines ---------------------------------------------------------------------------

struct MetalVertexAttrib {
	uint8_t index;		// attribute index in the shader
	uint8_t format;		// MTL::VertexFormat
	uint8_t buffer;		// buffer slot
	uint8_t pad;
	uint32_t offset;
};
struct MetalVertexLayout {
	int numAttribs;
	MetalVertexAttrib attribs[16];
	int numBuffers;
	struct {
		uint8_t slot;
		uint8_t constant;	// step function: one value for all vertices
		uint16_t pad;
		uint32_t stride;
	} buffers[16];
};
uint64_t GLMetal_HashVertexLayout( const MetalVertexLayout &layout );

// blend key: src factor | dst factor << 8 | enabled << 16 | colour write mask << 17
static inline uint32_t GLMetal_BlendKey( MTL::BlendFactor src, MTL::BlendFactor dst, bool enabled, uint32_t writeMask ) {
	return (uint32_t)src | ( (uint32_t)dst << 8 ) | ( enabled ? 0x10000u : 0u ) | ( ( writeMask & 15u ) << 17 );
}

// Registers a shader pair; retains both functions. Returns an id >= 0.
int GLMetal_RegisterProgram( const char *name, MTL::Function *vertexFunction, MTL::Function *fragmentFunction );
// The pipeline for a registered program drawing into the scene target.
MTL::RenderPipelineState *GLMetal_GetPipeline( int programId, const MetalVertexLayout &layout, uint64_t layoutHash,
											   uint32_t blendKey, bool lines );

// ---- shaders -------------------------------------------------------------------------------

MTL::Library *GLMetal_CompileLibraryAnyThread( const char *mslSource );
MTL::Library *GLMetal_LoadShaderLibraryFromSource( const char *mslSource, const char *label );

// ---- textures and samplers -----------------------------------------------------------------

struct MetalSamplerKey {
	uint8_t minFilter;	// MTL::SamplerMinMagFilter
	uint8_t magFilter;
	uint8_t mipFilter;	// MTL::SamplerMipFilter
	uint8_t addressS;	// MTL::SamplerAddressMode
	uint8_t addressT;
	uint8_t addressR;
	uint8_t anisotropy;
	uint8_t maxLod;		// highest mip level that may be sampled
	int8_t lodBias8;	// lod bias in 1/8 steps
	uint8_t border;		// border colour: 0 transparent black, 1 opaque black, 2 opaque white
};
MTL::SamplerState *GLMetal_GetSampler( const MetalSamplerKey &key );

// swizzle: four MTL::TextureSwizzle values (r | g << 8 | b << 16 | a << 24), 0 for none
MTL::Texture *GLMetal_NewTexture( MTL::TextureType type, MTL::PixelFormat format, int width, int height, int depth,
								  int mipLevels, bool renderTarget, uint32_t swizzle );
// Copies pixel data into a texture region. Ordered with the draws issued before and after it.
void GLMetal_UploadTextureRegion( MTL::Texture *texture, int slice, int level, int x, int y, int z,
								  int width, int height, int depth,
								  const void *bytes, int bytesPerRow, int bytesPerImage );
// Releases a texture once no frame in flight can still use it.
void GLMetal_ReleaseTexture( MTL::Texture *texture );
void GLMetal_ReleaseBuffer( MTL::Buffer *buffer );

// Copies a rectangle of the scene colour (or depth) target into a texture of the same format.
// `replay`: a depth copy may be made by drawing the frame's depth writes again in a command
// buffer that runs before the frame's (GLMetal_CanReplayDepth), which leaves the scene pass
// open. The caller must then pass a texture nothing drawn earlier in this frame samples.
void GLMetal_CopySceneToTexture( bool depth, int srcX, int srcY, int width, int height,
								 MTL::Texture *dst, int dstX, int dstY, bool replay = false );
bool GLMetal_CanReplayDepth( void );
// Blend layer: see metal_device.cpp. Begin returns the scale of the layer, or 0 for none.
int GLMetal_BeginBlendLayer( uint32_t blendKey );
void GLMetal_EndMultisamplingAtNextCopy( void );
// Shadow maps (r_shadowMaps): see metal_device.cpp. Acquire returns a map no other light of
// this frame uses, or -1; while a layer is begun, draws go into it.
int GLMetal_ShadowMapAcquire( int size, int layers );
void GLMetal_ShadowMapBeginLayer( int map, int layer );
void GLMetal_ShadowMapEndLayer( void );
bool GLMetal_ShadowMapLayerActive( void );
MTL::Texture *GLMetal_ShadowMapTexture( int map );
MTL::SamplerState *GLMetal_ShadowSampler( void );
void GLMetal_EndBlendLayer( void );
// A clear of the whole depth buffer is about to be drawn.
void GLMetal_DepthBufferCleared( void );
// Draw calls on the scene pass's encoder.
void GLMetal_DrawIndexed( NS::UInteger count, MTL::IndexType indexType, MTL::Buffer *indexBuffer, NS::UInteger indexOffset );
void GLMetal_Draw( MTL::PrimitiveType primitive, NS::UInteger first, NS::UInteger count );
// Reads back scene colour as RGBA8 (row 0 = row 0 of the target), depth as float, or stencil as bytes.
enum { METAL_READ_COLOR, METAL_READ_DEPTH, METAL_READ_STENCIL };
bool GLMetal_ReadScenePixels( int what, int x, int y, int width, int height, void *dest );

#endif // METAL_DEVICE_H
