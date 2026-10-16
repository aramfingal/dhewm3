/*
mgl.cpp - the OpenGL entry points dhewm3 uses, implemented on Metal.

dhewm3 calls OpenGL through qgl* function pointers that are looked up by name. With
r_backend metal, MGL_GetProcAddress supplies them: every function here keeps the GL state it
is given, and draw calls turn that state into a Metal pipeline, uniforms and bindings.
See docs/metal/DESIGN.md.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

#include <SDL_opengl.h>

#include <Metal/Metal.hpp>

#include "metal_host.h"
#include "metal_device.h"
#include "mgl_local.h"

#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif
#ifndef GL_TEXTURE_RECTANGLE_ARB
#define GL_TEXTURE_RECTANGLE_ARB 0x84F5
#endif

namespace {

const int MGL_MAX_UNITS = 16;		// image units; fixed-function texturing uses the first 8
const int MGL_MAX_ATTRIBS = 16;
const int MGL_MATRIX_STACK = 16;
const NS::UInteger MGL_UNIFORM_VERTEX_SLOT = 30;
const NS::UInteger MGL_UNIFORM_FRAGMENT_SLOT = 0;

// ============================================================================
// objects
// ============================================================================

struct MglTexture {
	MTL::Texture *tex;
	int target;				// MT_2D, MT_CUBE, MT_3D
	int width, height, depth;
	int levels;				// allocated
	int maxLevelDefined;
	GLenum internalFormat;
	MTL::PixelFormat pixelFormat;
	uint32_t swizzle;
	uint8_t formatClass;	// MF_*
	bool isDepth;
	// sampler parameters
	GLenum minFilter, magFilter, wrapS, wrapT, wrapR;
	float anisotropy, lodBias;
	int maxLevel;
	int border;				// 0 transparent black, 1 opaque black, 2 opaque white
	MTL::SamplerState *sampler;	// NULL: parameters changed
	// depth copies made by replay (CopySceneDepth): one storage per copy of a frame
	std::vector<MTL::Texture *> copyStorage;
	uint64_t copyFrame;
	int copyCount;

	MglTexture() : tex( NULL ), target( MT_2D ), width( 0 ), height( 0 ), depth( 0 ), levels( 0 ), maxLevelDefined( 0 ),
		internalFormat( 0 ), pixelFormat( MTL::PixelFormatInvalid ), swizzle( 0 ), formatClass( MF_RGBA ), isDepth( false ),
		minFilter( GL_NEAREST_MIPMAP_LINEAR ), magFilter( GL_LINEAR ), wrapS( GL_REPEAT ), wrapT( GL_REPEAT ), wrapR( GL_REPEAT ),
		anisotropy( 1.0f ), lodBias( 0.0f ), maxLevel( 1000 ), border( 0 ), sampler( NULL ), copyFrame( ~0ull ), copyCount( 0 ) {}
};

struct MglBuffer {
	MTL::Buffer *buf;
	size_t size;
	uint64_t lastUsedFrame;		// frame serial of the last draw that used it, ~0 = never
	MglBuffer() : buf( NULL ), size( 0 ), lastUsedFrame( ~0ull ) {}
};

struct ClientArray {
	bool enabled;
	int size;
	GLenum type;
	bool normalized;
	int stride;
	const void *pointer;		// address, or offset into `buffer`
	GLuint buffer;
	// immediate mode: data already in frame memory
	MTL::Buffer *directBuffer;
	size_t directOffset;
};

struct TexUnit {
	bool enabled[4];			// by MT_2D, MT_CUBE, MT_3D
	GLuint bound[4];
	int envMode;				// ME_*
	uint8_t combineRGB, combineA;
	uint8_t srcRGB[3], opRGB[3], srcA[3], opA[3];
	int rgbScale, alphaScale;
	float envColor[4];
	bool genEnabled[4];
	uint8_t genMode[4];			// MG_*
	float objPlane[4][4];
	float eyePlane[4][4];
	float matrix[MGL_MATRIX_STACK][16];
	int matrixTop;
	bool matrixIdentity;
};

struct StencilFace {
	GLenum sfail, zfail, zpass;
};

struct GLState {
	bool blend, depthTest, stencilTest, cullFace, scissorTest, alphaTest;
	bool polyOffsetFill, polyOffsetLine, vertexProgram, fragmentProgram;
	GLenum blendSrc, blendDst;
	GLenum depthFunc;
	bool depthMask;
	bool colorMask[4];
	GLenum alphaFunc;
	float alphaRef;
	GLenum cullFaceMode;
	GLenum polygonMode;
	float polyOffsetFactor, polyOffsetUnits;
	GLenum stencilFunc;
	int stencilRef;
	unsigned stencilReadMask, stencilWriteMask;
	StencilFace stencil[2];		// front, back
	int clearStencil;
	float clearDepth;
	float clearColor[4];
	int viewport[4];
	float depthRange[2];
	int scissor[4];

	GLenum matrixMode;
	float modelview[MGL_MATRIX_STACK][16];
	int modelviewTop;
	float projection[MGL_MATRIX_STACK][16];
	int projectionTop;
	float mvp[16];
	bool mvpDirty;

	float color[4];
	float normal[4];
	float texCoord[8][4];

	int activeTexture, clientActiveTexture;
	ClientArray vertexArray, colorArray, normalArray;
	ClientArray texCoordArray[8];
	ClientArray attribArray[MGL_MAX_ATTRIBS];
	GLuint arrayBuffer, elementBuffer;

	GLuint boundProgram[2];		// vertex, fragment
	float env[2][256][4];
	TexUnit unit[MGL_MAX_UNITS];

	int packAlignment, unpackAlignment, unpackRowLength;
	GLenum error;
};

GLState gl;
bool initialized = false;

std::unordered_map<GLuint, MglTexture *> textures;
GLuint nextTextureName = 1;
std::unordered_map<GLuint, MglBuffer *> buffers;
GLuint nextBufferName = 1;
std::unordered_map<GLuint, MglArbProgram> programs;
GLuint nextProgramName = 100000;
uint32_t programVersionCounter = 1;
std::string programErrorString;
int programErrorPos = -1;

std::vector<MglPair> pairs;
std::unordered_map<std::string, int> pairIndex;
bool shaderDirty = true;
int currentPair = -1;
bool rasterDirty = true;

int debugSkipPair = -1;				// r_metalSkipPair, read once per frame
MglTexture *fallbackTexture[4];		// by MT_*
int clearProgram = -1;

std::string extensionsString;
std::string rendererString;

// immediate mode
struct ImmVertex {
	float pos[4];
	float col[4];
	float tc[4];
};
std::vector<ImmVertex> immVerts;
GLenum immMode = 0;
bool immActive = false;

std::unordered_map<std::string, bool> stubsReported;

// ============================================================================
// helpers
// ============================================================================

void SetError( GLenum e ) {
	if ( gl.error == GL_NO_ERROR ) {
		gl.error = e;
	}
}

void MatIdentity( float *m ) {
	memset( m, 0, 16 * sizeof( float ) );
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b (column-major, as in OpenGL)
void MatMul( const float *a, const float *b, float *out ) {
	float r[16];
	for ( int c = 0; c < 4; c++ ) {
		for ( int row = 0; row < 4; row++ ) {
			r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] +
							 a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
		}
	}
	memcpy( out, r, sizeof( r ) );
}

bool MatIsIdentity( const float *m ) {
	static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	return memcmp( m, id, sizeof( id ) ) == 0;
}

bool MatInverse( const float *m, float *out ) {
	double inv[16], det;
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if ( det == 0.0 ) {
		MatIdentity( out );
		return false;
	}
	det = 1.0 / det;
	for ( int i = 0; i < 16; i++ ) {
		out[i] = (float)( inv[i] * det );
	}
	return true;
}

float *CurrentMatrix( void ) {
	if ( gl.matrixMode == GL_PROJECTION ) {
		return gl.projection[gl.projectionTop];
	}
	if ( gl.matrixMode == GL_TEXTURE ) {
		TexUnit &u = gl.unit[gl.activeTexture];
		return u.matrix[u.matrixTop];
	}
	return gl.modelview[gl.modelviewTop];
}

void MatrixChanged( void ) {
	if ( gl.matrixMode == GL_TEXTURE ) {
		TexUnit &u = gl.unit[gl.activeTexture];
		const bool identity = MatIsIdentity( u.matrix[u.matrixTop] );
		if ( identity != u.matrixIdentity ) {
			u.matrixIdentity = identity;
			shaderDirty = true;
		}
	} else {
		gl.mvpDirty = true;
	}
}

const float *Mvp( void ) {
	if ( gl.mvpDirty ) {
		MatMul( gl.projection[gl.projectionTop], gl.modelview[gl.modelviewTop], gl.mvp );
		gl.mvpDirty = false;
	}
	return gl.mvp;
}

int TargetIndex( GLenum target ) {
	switch ( target ) {
	case GL_TEXTURE_2D:
	case GL_TEXTURE_RECTANGLE_ARB:
		return MT_2D;
	case GL_TEXTURE_3D:
		return MT_3D;
	case GL_TEXTURE_CUBE_MAP:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_X: case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Y: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
		return MT_CUBE;
	}
	return MT_NONE;
}

MglTexture *TextureByName( GLuint name ) {
	if ( name == 0 ) {
		return NULL;
	}
	auto it = textures.find( name );
	return it != textures.end() ? it->second : NULL;
}

MglTexture *BoundTexture( GLenum target ) {
	const int ti = TargetIndex( target );
	if ( ti == MT_NONE ) {
		return NULL;
	}
	return TextureByName( gl.unit[gl.activeTexture].bound[ti] );
}

MglBuffer *BufferByName( GLuint name ) {
	if ( name == 0 ) {
		return NULL;
	}
	auto it = buffers.find( name );
	return it != buffers.end() ? it->second : NULL;
}

// ============================================================================
// textures
// ============================================================================

#define SWZ( r, g, b, a ) ( (uint32_t)(r) | ( (uint32_t)(g) << 8 ) | ( (uint32_t)(b) << 16 ) | ( (uint32_t)(a) << 24 ) )

struct FormatInfo {
	MTL::PixelFormat pixelFormat;
	uint32_t swizzle;
	uint8_t formatClass;
	bool compressed;
	int blockBytes;
	bool depth;
};

// What a GL internal format becomes. `compressedData` says whether the caller supplies
// compressed blocks (glCompressedTexImage2D); a compressed internal format given
// uncompressed pixels is stored uncompressed.
FormatInfo FormatForInternal( GLenum internalFormat, bool compressedData, GLenum externalFormat ) {
	const uint32_t R = (uint32_t)MTL::TextureSwizzleRed, G = (uint32_t)MTL::TextureSwizzleGreen;
	const uint32_t Z = (uint32_t)MTL::TextureSwizzleZero, O = (uint32_t)MTL::TextureSwizzleOne;
	const uint32_t B = (uint32_t)MTL::TextureSwizzleBlue;
	FormatInfo f = { MTL::PixelFormatRGBA8Unorm, 0, MF_RGBA, false, 0, false };
	const bool bgra = ( externalFormat == GL_BGRA || externalFormat == GL_BGR );
	if ( bgra ) {
		f.pixelFormat = MTL::PixelFormatBGRA8Unorm;
	}
	switch ( internalFormat ) {
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
		f.formatClass = MF_RGB;
		f.swizzle = SWZ( R, G, B, O );
		if ( compressedData ) { f.pixelFormat = MTL::PixelFormatBC1_RGBA; f.compressed = true; f.blockBytes = 8; }
		break;
	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		if ( compressedData ) { f.pixelFormat = MTL::PixelFormatBC1_RGBA; f.compressed = true; f.blockBytes = 8; }
		break;
	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		if ( compressedData ) { f.pixelFormat = MTL::PixelFormatBC2_RGBA; f.compressed = true; f.blockBytes = 16; }
		break;
	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		if ( compressedData ) { f.pixelFormat = MTL::PixelFormatBC3_RGBA; f.compressed = true; f.blockBytes = 16; }
		break;
	case GL_COMPRESSED_RGBA_BPTC_UNORM:
		if ( compressedData ) { f.pixelFormat = MTL::PixelFormatBC7_RGBAUnorm; f.compressed = true; f.blockBytes = 16; }
		break;
	case GL_ALPHA: case GL_ALPHA4: case GL_ALPHA8:
		f.pixelFormat = MTL::PixelFormatR8Unorm;
		f.swizzle = SWZ( Z, Z, Z, R );
		f.formatClass = MF_ALPHA;
		break;
	case 1: case GL_LUMINANCE: case GL_LUMINANCE4: case GL_LUMINANCE8:
		f.pixelFormat = MTL::PixelFormatR8Unorm;
		f.swizzle = SWZ( R, R, R, O );
		f.formatClass = MF_LUMINANCE;
		break;
	case GL_INTENSITY: case GL_INTENSITY4: case GL_INTENSITY8:
		f.pixelFormat = MTL::PixelFormatR8Unorm;
		f.swizzle = SWZ( R, R, R, R );
		f.formatClass = MF_INTENSITY;
		break;
	case 2: case GL_LUMINANCE_ALPHA: case GL_LUMINANCE4_ALPHA4: case GL_LUMINANCE8_ALPHA8:
		f.pixelFormat = MTL::PixelFormatRG8Unorm;
		f.swizzle = SWZ( R, R, R, G );
		f.formatClass = MF_LUMINANCE_ALPHA;
		break;
	case 3: case GL_RGB: case GL_RGB4: case GL_RGB5: case GL_RGB8: case GL_R3_G3_B2: case GL_COMPRESSED_RGB:
		f.swizzle = SWZ( R, G, B, O );
		f.formatClass = MF_RGB;
		break;
	case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32:
		f.pixelFormat = MTL::PixelFormatR32Float;
		f.swizzle = SWZ( R, R, R, O );
		f.formatClass = MF_LUMINANCE;
		f.depth = true;
		break;
	default:
		break;
	}
	return f;
}

int MipLevelCount( int w, int h, int d ) {
	int levels = 1;
	int m = std::max( w, std::max( h, d ) );
	while ( m > 1 ) {
		m >>= 1;
		levels++;
	}
	return levels;
}

void ReleaseTextureStorage( MglTexture *t ) {
	for ( MTL::Texture *storage : t->copyStorage ) {
		if ( storage != t->tex ) {
			GLMetal_ReleaseTexture( storage );
		}
	}
	t->copyStorage.clear();
	if ( t->tex ) {
		GLMetal_ReleaseTexture( t->tex );
		t->tex = NULL;
	}
}

// Makes sure the texture object has storage of this shape; keeps the storage it has when
// it already matches.
bool EnsureTextureStorage( MglTexture *t, int targetIndex, const FormatInfo &f, GLenum internalFormat, int w, int h, int d ) {
	if ( t->tex && t->target == targetIndex && t->width == w && t->height == h && t->depth == d &&
		 t->pixelFormat == f.pixelFormat && t->swizzle == f.swizzle ) {
		t->internalFormat = internalFormat;
		return true;
	}
	ReleaseTextureStorage( t );
	const MTL::TextureType type = ( targetIndex == MT_CUBE ) ? MTL::TextureTypeCube :
								  ( targetIndex == MT_3D ) ? MTL::TextureType3D : MTL::TextureType2D;
	const int levels = MipLevelCount( w, h, targetIndex == MT_3D ? d : 1 );
	t->tex = GLMetal_NewTexture( type, f.pixelFormat, w, h, d, levels, false, f.swizzle );
	if ( t->formatClass != f.formatClass || t->target != targetIndex ) {
		shaderDirty = true;
	}
	t->target = targetIndex;
	t->width = w;
	t->height = h;
	t->depth = d;
	t->levels = levels;
	t->maxLevelDefined = 0;
	t->internalFormat = internalFormat;
	t->pixelFormat = f.pixelFormat;
	t->swizzle = f.swizzle;
	t->formatClass = f.formatClass;
	t->isDepth = f.depth;
	t->sampler = NULL;
	return t->tex != NULL;
}

int SourceComponents( GLenum format ) {
	switch ( format ) {
	case GL_RGBA: case GL_BGRA: return 4;
	case GL_RGB: case GL_BGR: return 3;
	case GL_LUMINANCE_ALPHA: return 2;
	case GL_ALPHA: case GL_LUMINANCE: case GL_INTENSITY: case GL_RED: case GL_COLOR_INDEX: case GL_DEPTH_COMPONENT: return 1;
	}
	return 4;
}

// Converts client pixels to the texture's pixel format. Returns `pixels` itself when the
// layout already matches, otherwise `storage`.
const uint8_t *ConvertPixels( const void *pixels, GLenum format, GLenum type, int w, int h, int d,
							  MTL::PixelFormat dst, uint8_t formatClass, std::vector<uint8_t> &storage, int *bytesPerPixel ) {
	const int dstBpp = ( dst == MTL::PixelFormatR8Unorm ) ? 1 : ( dst == MTL::PixelFormatRG8Unorm ) ? 2 : 4;
	*bytesPerPixel = dstBpp;
	if ( type != GL_UNSIGNED_BYTE ) {
		if ( dst == MTL::PixelFormatR32Float && type == GL_FLOAT ) {
			return (const uint8_t *)pixels;
		}
		return NULL;
	}
	const int srcBpp = SourceComponents( format );
	const size_t n = (size_t)w * (size_t)h * (size_t)std::max( d, 1 );
	const uint8_t *src = (const uint8_t *)pixels;
	// client rows are padded to gl.unpackAlignment; dhewm3 only uploads 4 byte pixels or
	// tightly packed data, so rows are taken as tightly packed
	if ( dstBpp == 4 && srcBpp == 4 ) {
		// GL_RGBA into RGBA8, GL_BGRA into BGRA8: no conversion
		if ( ( dst == MTL::PixelFormatRGBA8Unorm && format == GL_RGBA ) || ( dst == MTL::PixelFormatBGRA8Unorm && format == GL_BGRA ) ) {
			return src;
		}
	}
	storage.resize( n * (size_t)dstBpp );
	uint8_t *out = storage.data();
	for ( size_t i = 0; i < n; i++ ) {
		uint8_t r = 0, g = 0, b = 0, a = 255;
		const uint8_t *s = src + i * (size_t)srcBpp;
		switch ( format ) {
		case GL_RGBA: r = s[0]; g = s[1]; b = s[2]; a = s[3]; break;
		case GL_BGRA: b = s[0]; g = s[1]; r = s[2]; a = s[3]; break;
		case GL_RGB: r = s[0]; g = s[1]; b = s[2]; break;
		case GL_BGR: b = s[0]; g = s[1]; r = s[2]; break;
		case GL_LUMINANCE_ALPHA: r = g = b = s[0]; a = s[1]; break;
		case GL_ALPHA: a = s[0]; break;
		case GL_INTENSITY: r = g = b = a = s[0]; break;
		default: r = g = b = s[0]; break;
		}
		uint8_t *o = out + i * (size_t)dstBpp;
		if ( dstBpp == 1 ) {
			o[0] = ( formatClass == MF_ALPHA ) ? a : r;
		} else if ( dstBpp == 2 ) {
			o[0] = r;
			o[1] = a;
		} else if ( dst == MTL::PixelFormatBGRA8Unorm ) {
			o[0] = b; o[1] = g; o[2] = r; o[3] = a;
		} else {
			o[0] = r; o[1] = g; o[2] = b; o[3] = a;
		}
	}
	return out;
}

int CubeFace( GLenum target ) {
	if ( target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ) {
		return (int)( target - GL_TEXTURE_CUBE_MAP_POSITIVE_X );
	}
	return 0;
}

void UploadLevel( MglTexture *t, int face, int level, int x, int y, int z, int w, int h, int d,
				  GLenum format, GLenum type, const void *pixels ) {
	if ( !t->tex || !pixels || level >= t->levels || w <= 0 || h <= 0 ) {
		return;
	}
	std::vector<uint8_t> storage;
	std::vector<uint8_t> packed;
	if ( gl.unpackRowLength > w && type == GL_UNSIGNED_BYTE ) {
		// the client's rows are longer than the region (GL_UNPACK_ROW_LENGTH): pack them
		const size_t srcBpp = (size_t)SourceComponents( format );
		packed.resize( (size_t)w * (size_t)h * srcBpp );
		for ( int row = 0; row < h; row++ ) {
			memcpy( packed.data() + (size_t)row * (size_t)w * srcBpp,
					(const uint8_t *)pixels + (size_t)row * (size_t)gl.unpackRowLength * srcBpp, (size_t)w * srcBpp );
		}
		pixels = packed.data();
	}
	int bpp = 4;
	const uint8_t *data = ConvertPixels( pixels, format, type, w, h, d, t->pixelFormat, t->formatClass, storage, &bpp );
	if ( !data ) {
		MetalHost_Warning( "[Metal] texture upload: unsupported pixel type 0x%x", type );
		return;
	}
	GLMetal_UploadTextureRegion( t->tex, face, level, x, y, z, w, h, std::max( d, 1 ), data, w * bpp, w * h * bpp );
	t->maxLevelDefined = std::max( t->maxLevelDefined, level );
	if ( level > 0 ) {
		t->sampler = NULL;
	}
}

void TexImage( GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, GLsizei d,
			   GLenum format, GLenum type, const GLvoid *pixels ) {
	MglTexture *t = BoundTexture( target );
	const int ti = TargetIndex( target );
	if ( !t || ti == MT_NONE || w <= 0 || h <= 0 ) {
		return;
	}
	if ( level == 0 ) {
		const FormatInfo f = FormatForInternal( (GLenum)internalFormat, false, format );
		if ( !EnsureTextureStorage( t, ti, f, (GLenum)internalFormat, w, h, ti == MT_3D ? d : 1 ) ) {
			return;
		}
	}
	UploadLevel( t, CubeFace( target ), level, 0, 0, 0, w, h, ti == MT_3D ? d : 1, format, type, pixels );
	if ( level > t->maxLevelDefined && level < t->levels ) {
		t->maxLevelDefined = level;
		t->sampler = NULL;
	}
}

MetalSamplerKey SamplerKeyFor( const MglTexture *t ) {
	MetalSamplerKey k;
	memset( &k, 0, sizeof( k ) );
	bool mip = true;
	bool mipLinear = false;
	bool minLinear = false;
	switch ( t->minFilter ) {
	case GL_NEAREST: mip = false; break;
	case GL_LINEAR: mip = false; minLinear = true; break;
	case GL_NEAREST_MIPMAP_NEAREST: break;
	case GL_LINEAR_MIPMAP_NEAREST: minLinear = true; break;
	case GL_NEAREST_MIPMAP_LINEAR: mipLinear = true; break;
	default: minLinear = true; mipLinear = true; break;
	}
	const int maxLod = std::min( std::min( t->maxLevelDefined, t->levels - 1 ), t->maxLevel );
	if ( maxLod <= 0 ) {
		mip = false;
	}
	k.minFilter = (uint8_t)( minLinear ? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest );
	k.magFilter = (uint8_t)( t->magFilter == GL_NEAREST ? MTL::SamplerMinMagFilterNearest : MTL::SamplerMinMagFilterLinear );
	k.mipFilter = (uint8_t)( !mip ? MTL::SamplerMipFilterNotMipmapped : ( mipLinear ? MTL::SamplerMipFilterLinear : MTL::SamplerMipFilterNearest ) );
	auto wrap = []( GLenum w ) -> uint8_t {
		switch ( w ) {
		case GL_REPEAT: return (uint8_t)MTL::SamplerAddressModeRepeat;
		case GL_CLAMP_TO_BORDER: return (uint8_t)MTL::SamplerAddressModeClampToBorderColor;
		case GL_MIRRORED_REPEAT: return (uint8_t)MTL::SamplerAddressModeMirrorRepeat;
		default: return (uint8_t)MTL::SamplerAddressModeClampToEdge;
		}
	};
	k.addressS = wrap( t->wrapS );
	k.addressT = wrap( t->wrapT );
	k.addressR = wrap( t->wrapR );
	if ( t->target == MT_CUBE ) {
		k.addressS = k.addressT = k.addressR = (uint8_t)MTL::SamplerAddressModeClampToEdge;
	}
	// anisotropic filtering applies to linearly filtered, mipmapped textures
	k.anisotropy = (uint8_t)( ( mip && minLinear ) ? std::max( 1, std::min( 16, (int)t->anisotropy ) ) : 1 );
	k.maxLod = (uint8_t)std::max( 0, std::min( 31, maxLod ) );
	k.lodBias8 = (int8_t)std::max( -127.0f, std::min( 127.0f, t->lodBias * 8.0f ) );
	k.border = (uint8_t)t->border;
	return k;
}

MglTexture *NewFallbackTexture( int targetIndex ) {
	MglTexture *t = new MglTexture();
	const FormatInfo f = FormatForInternal( GL_RGBA8, false, GL_RGBA );
	EnsureTextureStorage( t, targetIndex, f, GL_RGBA8, 1, 1, 1 );
	const uint8_t black[4] = { 0, 0, 0, 255 };
	const int faces = ( targetIndex == MT_CUBE ) ? 6 : 1;
	for ( int i = 0; i < faces; i++ ) {
		GLMetal_UploadTextureRegion( t->tex, i, 0, 0, 0, 0, 1, 1, 1, black, 4, 4 );
	}
	t->minFilter = GL_NEAREST;
	t->magFilter = GL_NEAREST;
	return t;
}

// ============================================================================
// buffers
// ============================================================================

// A buffer the GPU may still read (frames in flight) must not be written: give the object
// fresh storage with the same contents.
void MakeBufferWritable( MglBuffer *b ) {
	if ( !b->buf || b->lastUsedFrame == ~0ull ) {
		return;
	}
	if ( b->lastUsedFrame + 4 < GLMetal_FrameSerial() ) {
		return;
	}
	MTL::Buffer *fresh = GLMetal_Device()->newBuffer( (NS::UInteger)b->size, MTL::ResourceStorageModeShared );
	if ( !fresh ) {
		return;
	}
	memcpy( fresh->contents(), b->buf->contents(), b->size );
	GLMetal_ReleaseBuffer( b->buf );
	b->buf = fresh;
	b->lastUsedFrame = ~0ull;
}

// ============================================================================
// shader pairs
// ============================================================================

const MglArbProgram *ActiveProgram( int stage ) {
	if ( stage == 0 ? !gl.vertexProgram : !gl.fragmentProgram ) {
		return NULL;
	}
	auto it = programs.find( gl.boundProgram[stage] );
	if ( it == programs.end() || !it->second.valid || it->second.isFragment != ( stage == 1 ) ) {
		return NULL;
	}
	return &it->second;
}

const ClientArray *ArrayForAttrib( int index ) {
	if ( gl.attribArray[index].enabled ) {
		return &gl.attribArray[index];
	}
	if ( index == 0 ) {
		return gl.vertexArray.enabled ? &gl.vertexArray : NULL;
	}
	if ( index == 2 ) {
		return gl.normalArray.enabled ? &gl.normalArray : NULL;
	}
	if ( index == 3 ) {
		return gl.colorArray.enabled ? &gl.colorArray : NULL;
	}
	if ( index >= 8 ) {
		return gl.texCoordArray[index - 8].enabled ? &gl.texCoordArray[index - 8] : NULL;
	}
	return NULL;
}

int EnabledTarget( const TexUnit &u ) {
	// OpenGL: cube map over 3D over 2D
	if ( u.enabled[MT_CUBE] ) return MT_CUBE;
	if ( u.enabled[MT_3D] ) return MT_3D;
	if ( u.enabled[MT_2D] ) return MT_2D;
	return MT_NONE;
}

void BuildPairKey( MglPairKey &key ) {
	memset( &key, 0, sizeof( key ) );
	key.vp = ActiveProgram( 0 );
	key.fp = ActiveProgram( 1 );
	if ( key.vp ) {
		key.vpId = gl.boundProgram[0];
		key.vpVersion = key.vp->version;
	}
	if ( key.fp ) {
		key.fpId = gl.boundProgram[1];
		key.fpVersion = key.fp->version;
	}
	if ( gl.alphaTest ) {
		key.alphaFunc = (uint8_t)( MA_NEVER + ( gl.alphaFunc - GL_NEVER ) );
	}
	uint32_t reads = key.fp ? key.fp->varyings : 0;
	for ( int i = 0; i < 8; i++ ) {
		const TexUnit &u = gl.unit[i];
		MglFFUnit &k = key.unit[i];
		if ( !key.fp ) {
			const int target = EnabledTarget( u );
			if ( target != MT_NONE ) {
				const MglTexture *t = TextureByName( u.bound[target] );
				if ( !t || !t->tex ) {
					t = fallbackTexture[target];
				}
				k.target = (uint8_t)target;
				k.formatClass = t->formatClass;
				k.envMode = (uint8_t)u.envMode;
				if ( u.envMode == ME_COMBINE ) {
					k.combineRGB = u.combineRGB;
					k.combineA = u.combineA;
					for ( int j = 0; j < 3; j++ ) {
						k.srcRGB[j] = u.srcRGB[j];
						k.opRGB[j] = u.opRGB[j];
						k.srcA[j] = u.srcA[j];
						k.opA[j] = u.opA[j];
					}
					k.rgbScale = (uint8_t)u.rgbScale;
					k.alphaScale = (uint8_t)u.alphaScale;
				}
				reads |= MV_TEXCOORD0 << i;
			}
		}
		// the vertex stage only generates coordinates the fragment stage reads
		if ( !key.vp && ( reads & ( MV_TEXCOORD0 << i ) ) ) {
			for ( int c = 0; c < 4; c++ ) {
				if ( u.genEnabled[c] ) {
					k.genMask |= 1 << c;
					k.genModes |= u.genMode[c] << ( 2 * c );
				}
			}
			k.texMatrix = u.matrixIdentity ? 0 : 1;
		}
	}
	uint32_t arrays = 0;
	const uint32_t needed = MglShader_VertexAttribs( key );
	for ( int i = 0; i < MGL_MAX_ATTRIBS; i++ ) {
		if ( ( needed & ( 1u << i ) ) && ArrayForAttrib( i ) ) {
			arrays |= 1u << i;
		}
	}
	key.arrayMask = arrays;
}

int LookupPair( void ) {
	MglPairKey key;
	BuildPairKey( key );

	MglPairKey bytes = key;
	bytes.vp = NULL;
	bytes.fp = NULL;
	const std::string k( (const char *)&bytes, sizeof( bytes ) );
	auto it = pairIndex.find( k );
	if ( it != pairIndex.end() ) {
		return it->second;
	}

	MglPair pair;
	std::string msl;
	MglShader_Generate( key, pair, msl );
	const int debug = MetalHost_CvarInt( "r_metalDebug" );
	if ( debug >= 1 ) {
		MetalHost_Printf( "[Metal] compiling shader pair %d: %s (%d uniforms)\n", (int)pairs.size(), pair.name.c_str(), (int)pair.uniforms.size() );
	}
	if ( debug >= 2 ) {
		char path[256];
		snprintf( path, sizeof( path ), "/tmp/mgl_pair_%03d_%s.metal", (int)pairs.size(), pair.name.c_str() );
		FILE *f = fopen( path, "w" );
		if ( f ) {
			fputs( msl.c_str(), f );
			fclose( f );
		}
	}
	MTL::Library *lib = GLMetal_LoadShaderLibraryFromSource( msl.c_str(), pair.name.c_str() );
	if ( lib ) {
		MTL::Function *vs = lib->newFunction( NS::String::string( "mgl_vs", NS::UTF8StringEncoding ) );
		MTL::Function *fs = lib->newFunction( NS::String::string( "mgl_fs", NS::UTF8StringEncoding ) );
		pair.programId = GLMetal_RegisterProgram( pair.name.c_str(), vs, fs );
		if ( vs ) vs->release();
		if ( fs ) fs->release();
		lib->release();
	} else {
		// keep the source of a pair that does not compile, to look at
		char path[256];
		snprintf( path, sizeof( path ), "/tmp/mgl_failed_%s.metal", pair.name.c_str() );
		FILE *f = fopen( path, "w" );
		if ( f ) {
			fputs( msl.c_str(), f );
			fclose( f );
			MetalHost_Warning( "[Metal] MSL of the failed pair written to %s", path );
		}
	}
	pairs.push_back( pair );
	const int index = (int)pairs.size() - 1;
	pairIndex[k] = index;
	return index;
}

// ============================================================================
// uniforms
// ============================================================================

void MatrixFor( int which, float *out ) {
	if ( which == MM_MVP ) {
		memcpy( out, Mvp(), 16 * sizeof( float ) );
	} else if ( which == MM_MODELVIEW ) {
		memcpy( out, gl.modelview[gl.modelviewTop], 16 * sizeof( float ) );
	} else if ( which == MM_PROJECTION ) {
		memcpy( out, gl.projection[gl.projectionTop], 16 * sizeof( float ) );
	} else {
		const TexUnit &u = gl.unit[ std::min( which - MM_TEXTURE0, MGL_MAX_UNITS - 1 ) ];
		memcpy( out, u.matrix[u.matrixTop], 16 * sizeof( float ) );
	}
}

void FillUniform( const MglUniform &u, float *out ) {
	switch ( u.kind ) {
	case MU_ENV:
		memcpy( out, gl.env[u.a & 1][u.b & 255], 4 * sizeof( float ) );
		break;
	case MU_LOCAL: {
		auto it = programs.find( gl.boundProgram[u.a & 1] );
		if ( it != programs.end() ) {
			memcpy( out, it->second.local[u.b & 31], 4 * sizeof( float ) );
		} else {
			out[0] = out[1] = out[2] = out[3] = 0.0f;
		}
		break;
	}
	case MU_MATRIX_ROW: {
		const int row = u.b & 3;
		const int xform = ( u.b >> 2 ) & 3;
		if ( xform == MX_NONE || xform == MX_TRANSPOSE ) {
			// the common cases without a copy
			const float *m;
			float tmp[16];
			if ( u.a == MM_MVP ) m = Mvp();
			else if ( u.a == MM_MODELVIEW ) m = gl.modelview[gl.modelviewTop];
			else if ( u.a == MM_PROJECTION ) m = gl.projection[gl.projectionTop];
			else { MatrixFor( u.a, tmp ); m = tmp; }
			if ( xform == MX_NONE ) {
				out[0] = m[row]; out[1] = m[4 + row]; out[2] = m[8 + row]; out[3] = m[12 + row];
			} else {
				memcpy( out, m + row * 4, 4 * sizeof( float ) );
			}
		} else {
			float m[16], inv[16];
			MatrixFor( u.a, m );
			MatInverse( m, inv );
			if ( xform == MX_INVERSE ) {
				out[0] = inv[row]; out[1] = inv[4 + row]; out[2] = inv[8 + row]; out[3] = inv[12 + row];
			} else {
				memcpy( out, inv + row * 4, 4 * sizeof( float ) );
			}
		}
		break;
	}
	case MU_TEXGEN_OBJECT:
		memcpy( out, gl.unit[u.a & 15].objPlane[u.b & 3], 4 * sizeof( float ) );
		break;
	case MU_TEXGEN_EYE:
		memcpy( out, gl.unit[u.a & 15].eyePlane[u.b & 3], 4 * sizeof( float ) );
		break;
	case MU_CURRENT_ATTRIB:
		if ( u.a == 3 ) {
			memcpy( out, gl.color, 4 * sizeof( float ) );
		} else if ( u.a == 2 ) {
			memcpy( out, gl.normal, 4 * sizeof( float ) );
		} else if ( u.a >= 8 ) {
			memcpy( out, gl.texCoord[u.a - 8], 4 * sizeof( float ) );
		} else {
			out[0] = out[1] = out[2] = 0.0f;
			out[3] = 1.0f;
		}
		break;
	case MU_ALPHA_REF:
		out[0] = gl.alphaRef;
		out[1] = out[2] = out[3] = 0.0f;
		break;
	case MU_ENV_COLOR:
		memcpy( out, gl.unit[u.a & 15].envColor, 4 * sizeof( float ) );
		break;
	case MU_TEX_SIZE: {
		const MglTexture *t = TextureByName( gl.unit[u.a & 15].bound[MT_2D] );
		out[0] = ( t && t->width ) ? 1.0f / t->width : 1.0f;
		out[1] = ( t && t->height ) ? 1.0f / t->height : 1.0f;
		out[2] = out[3] = 0.0f;
		break;
	}
	default:
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		break;
	}
}

// ============================================================================
// raster state
// ============================================================================

MTL::BlendFactor BlendFactorFor( GLenum f ) {
	switch ( f ) {
	case GL_ZERO: return MTL::BlendFactorZero;
	case GL_ONE: return MTL::BlendFactorOne;
	case GL_SRC_COLOR: return MTL::BlendFactorSourceColor;
	case GL_ONE_MINUS_SRC_COLOR: return MTL::BlendFactorOneMinusSourceColor;
	case GL_SRC_ALPHA: return MTL::BlendFactorSourceAlpha;
	case GL_ONE_MINUS_SRC_ALPHA: return MTL::BlendFactorOneMinusSourceAlpha;
	case GL_DST_COLOR: return MTL::BlendFactorDestinationColor;
	case GL_ONE_MINUS_DST_COLOR: return MTL::BlendFactorOneMinusDestinationColor;
	case GL_DST_ALPHA: return MTL::BlendFactorDestinationAlpha;
	case GL_ONE_MINUS_DST_ALPHA: return MTL::BlendFactorOneMinusDestinationAlpha;
	case GL_SRC_ALPHA_SATURATE: return MTL::BlendFactorSourceAlphaSaturated;
	}
	return MTL::BlendFactorOne;
}

uint32_t CurrentBlendKey( void ) {
	const uint32_t mask = ( gl.colorMask[0] ? (uint32_t)MTL::ColorWriteMaskRed : 0 ) | ( gl.colorMask[1] ? (uint32_t)MTL::ColorWriteMaskGreen : 0 ) |
						  ( gl.colorMask[2] ? (uint32_t)MTL::ColorWriteMaskBlue : 0 ) | ( gl.colorMask[3] ? (uint32_t)MTL::ColorWriteMaskAlpha : 0 );
	if ( !gl.blend ) {
		return GLMetal_BlendKey( MTL::BlendFactorOne, MTL::BlendFactorZero, false, mask );
	}
	return GLMetal_BlendKey( BlendFactorFor( gl.blendSrc ), BlendFactorFor( gl.blendDst ), true, mask );
}

uint8_t StencilOpFor( GLenum op ) {
	switch ( op ) {
	case GL_ZERO: return (uint8_t)MTL::StencilOperationZero;
	case GL_REPLACE: return (uint8_t)MTL::StencilOperationReplace;
	case GL_INCR: return (uint8_t)MTL::StencilOperationIncrementClamp;
	case GL_DECR: return (uint8_t)MTL::StencilOperationDecrementClamp;
	case GL_INVERT: return (uint8_t)MTL::StencilOperationInvert;
	case GL_INCR_WRAP: return (uint8_t)MTL::StencilOperationIncrementWrap;
	case GL_DECR_WRAP: return (uint8_t)MTL::StencilOperationDecrementWrap;
	}
	return (uint8_t)MTL::StencilOperationKeep;
}

void ScissorRect( int *x, int *y, int *w, int *h ) {
	if ( gl.scissorTest ) {
		*x = gl.scissor[0]; *y = gl.scissor[1]; *w = gl.scissor[2]; *h = gl.scissor[3];
	} else {
		*x = 0; *y = 0;
		GLMetal_GetSceneSize( w, h );
	}
}

void ApplyRasterState( void ) {
	if ( !rasterDirty ) {
		return;
	}
	rasterDirty = false;
	GLMetal_SetViewport( gl.viewport[0], gl.viewport[1], gl.viewport[2], gl.viewport[3], gl.depthRange[0], gl.depthRange[1] );
	int sx, sy, sw, sh;
	ScissorRect( &sx, &sy, &sw, &sh );
	GLMetal_SetScissor( sx, sy, sw, sh );

	MTL::CullMode cull = MTL::CullModeNone;
	if ( gl.cullFace ) {
		cull = ( gl.cullFaceMode == GL_FRONT ) ? MTL::CullModeFront : MTL::CullModeBack;
	}
	GLMetal_SetCullMode( cull );
	GLMetal_SetFillMode( gl.polygonMode == GL_LINE ? MTL::TriangleFillModeLines : MTL::TriangleFillModeFill );

	const bool offset = ( gl.polygonMode == GL_LINE ) ? gl.polyOffsetLine : gl.polyOffsetFill;
	GLMetal_SetDepthBias( offset ? gl.polyOffsetUnits : 0.0f, offset ? gl.polyOffsetFactor : 0.0f, 0.0f );

	MetalDepthStencilKey ds;
	memset( &ds, 0, sizeof( ds ) );
	// OpenGL: with the depth test off, the depth buffer is not written either
	ds.depthFunc = (uint8_t)( gl.depthTest ? ( gl.depthFunc - GL_NEVER ) : (int)MTL::CompareFunctionAlways );
	ds.depthWrite = ( gl.depthTest && gl.depthMask ) ? 1 : 0;
	if ( gl.stencilTest ) {
		ds.stencilEnable = 1;
		ds.readMask = (uint8_t)gl.stencilReadMask;
		ds.writeMask = (uint8_t)gl.stencilWriteMask;
		for ( int i = 0; i < 2; i++ ) {
			MetalStencilFace &f = ( i == 0 ) ? ds.front : ds.back;
			f.func = (uint8_t)( gl.stencilFunc - GL_NEVER );
			f.sfail = StencilOpFor( gl.stencil[i].sfail );
			f.zfail = StencilOpFor( gl.stencil[i].zfail );
			f.zpass = StencilOpFor( gl.stencil[i].zpass );
		}
	}
	GLMetal_SetDepthStencil( ds, (uint32_t)( gl.stencilRef & 0xff ) );
}

// ============================================================================
// drawing
// ============================================================================

MTL::VertexFormat VertexFormatFor( const ClientArray &a ) {
	if ( a.type == GL_FLOAT ) {
		switch ( a.size ) {
		case 1: return MTL::VertexFormatFloat;
		case 2: return MTL::VertexFormatFloat2;
		case 3: return MTL::VertexFormatFloat3;
		default: return MTL::VertexFormatFloat4;
		}
	}
	if ( a.type == GL_UNSIGNED_BYTE ) {
		return a.normalized ? MTL::VertexFormatUChar4Normalized : MTL::VertexFormatUChar4;
	}
	if ( a.type == GL_SHORT ) {
		if ( a.size == 2 ) return a.normalized ? MTL::VertexFormatShort2Normalized : MTL::VertexFormatShort2;
		return a.normalized ? MTL::VertexFormatShort4Normalized : MTL::VertexFormatShort4;
	}
	return MTL::VertexFormatInvalid;
}

int ElementSize( const ClientArray &a ) {
	if ( a.type == GL_FLOAT ) return a.size * 4;
	if ( a.type == GL_UNSIGNED_BYTE ) return 4;
	if ( a.type == GL_SHORT ) return ( a.size == 2 ) ? 4 : 8;
	return 16;
}

struct VertexGroup {
	MTL::Buffer *buffer;	// NULL: client memory
	MglBuffer *object;
	GLuint name;
	const uint8_t *base;	// lowest address (client memory) or offset (buffer object)
	uint32_t stride;
	uint32_t extent;		// bytes of one vertex that are used, for client memory copies
};

// Binds the vertex arrays the pair reads and fills the layout. `vertexCount` is the number
// of vertices client-memory arrays must provide.
bool BindVertexArrays( const MglPair &pair, int vertexCount, MetalVertexLayout &layout ) {
	memset( &layout, 0, sizeof( layout ) );
	VertexGroup groups[MGL_MAX_ATTRIBS];
	int numGroups = 0;
	int groupOf[MGL_MAX_ATTRIBS];
	const ClientArray *arrays[MGL_MAX_ATTRIBS];

	for ( int i = 0; i < MGL_MAX_ATTRIBS; i++ ) {
		groupOf[i] = -1;
		arrays[i] = NULL;
		if ( !( pair.attribMask & ( 1u << i ) ) ) {
			continue;
		}
		const ClientArray *a = ArrayForAttrib( i );
		if ( !a ) {
			return false;	// the array was switched off after the pair was chosen
		}
		arrays[i] = a;
		const uint32_t stride = a->stride ? (uint32_t)a->stride : (uint32_t)ElementSize( *a );
		const uint8_t *ptr = a->directBuffer ? (const uint8_t *)a->directOffset : (const uint8_t *)a->pointer;
		int g;
		for ( g = 0; g < numGroups; g++ ) {
			VertexGroup &vg = groups[g];
			if ( vg.name != a->buffer || vg.stride != stride || vg.buffer != a->directBuffer ) {
				continue;
			}
			// interleaved with the group if it starts within one stride of it
			const ptrdiff_t d = ptr - vg.base;
			if ( d >= 0 && (uint32_t)d + (uint32_t)ElementSize( *a ) <= stride ) {
				vg.extent = std::max( vg.extent, (uint32_t)d + (uint32_t)ElementSize( *a ) );
				break;
			}
			if ( d < 0 && (uint32_t)( -d ) + vg.extent <= stride ) {
				vg.extent += (uint32_t)( -d );
				vg.base = ptr;
				break;
			}
		}
		if ( g == numGroups ) {
			VertexGroup &vg = groups[numGroups++];
			vg.buffer = a->directBuffer;
			vg.object = NULL;
			vg.name = a->buffer;
			vg.base = ptr;
			vg.stride = stride;
			vg.extent = (uint32_t)ElementSize( *a );
		}
		groupOf[i] = g;
	}

	for ( int g = 0; g < numGroups; g++ ) {
		VertexGroup &vg = groups[g];
		MTL::Buffer *buffer = vg.buffer;
		NS::UInteger offset = (NS::UInteger)(size_t)vg.base;
		if ( !buffer ) {
			if ( vg.name ) {
				MglBuffer *b = BufferByName( vg.name );
				if ( !b || !b->buf ) {
					return false;
				}
				b->lastUsedFrame = GLMetal_FrameSerial();
				buffer = b->buf;
			} else {
				// client memory: copy what the draw reads into frame memory
				if ( !vg.base || vertexCount <= 0 ) {
					return false;
				}
				const size_t bytes = (size_t)( vertexCount - 1 ) * vg.stride + vg.extent;
				void *dst = GLMetal_FrameAlloc( bytes, 16, &buffer, &offset );
				if ( !dst ) {
					return false;
				}
				memcpy( dst, vg.base, bytes );
			}
		}
		GLMetal_SetVertexBuffer( buffer, offset, (NS::UInteger)g );
		layout.buffers[g].slot = (uint8_t)g;
		layout.buffers[g].stride = vg.stride;
	}
	layout.numBuffers = numGroups;

	for ( int i = 0; i < MGL_MAX_ATTRIBS; i++ ) {
		if ( groupOf[i] < 0 ) {
			continue;
		}
		const ClientArray *a = arrays[i];
		const uint8_t *ptr = a->directBuffer ? (const uint8_t *)a->directOffset : (const uint8_t *)a->pointer;
		MetalVertexAttrib &va = layout.attribs[layout.numAttribs++];
		va.index = (uint8_t)i;
		va.format = (uint8_t)VertexFormatFor( *a );
		va.buffer = (uint8_t)groupOf[i];
		va.offset = (uint32_t)( ptr - groups[groupOf[i]].base );
	}
	return true;
}

void BindTextures( const MglPair &pair ) {
	for ( int i = 0; i < MGL_MAX_UNITS; i++ ) {
		if ( !( pair.texMask & ( 1u << i ) ) ) {
			continue;
		}
		int target = pair.texTarget[i];
		if ( target == MT_RECT ) {
			target = MT_2D;
		}
		MglTexture *t = TextureByName( gl.unit[i].bound[target] );
		if ( !t || !t->tex || t->target != target ) {
			t = fallbackTexture[target];
		}
		if ( !t->sampler ) {
			t->sampler = GLMetal_GetSampler( SamplerKeyFor( t ) );
		}
		GLMetal_SetFragmentTexture( t->tex, t->sampler, (NS::UInteger)i );
	}
}

// Everything a draw needs except the draw call itself. Returns the encoder or NULL.
MTL::RenderCommandEncoder *PrepareDraw( int vertexCount ) {
	if ( shaderDirty || currentPair < 0 ) {
		currentPair = LookupPair();
		shaderDirty = false;
	}
	const MglPair &pair = pairs[currentPair];
	if ( pair.programId < 0 || currentPair == debugSkipPair ) {
		return NULL;
	}
	MTL::RenderCommandEncoder *enc = GLMetal_CurrentRenderEncoder();
	if ( !enc ) {
		return NULL;
	}
	// a new encoder has none of the state
	static uint32_t lastGeneration = 0;
	if ( lastGeneration != GLMetal_EncoderGeneration() ) {
		lastGeneration = GLMetal_EncoderGeneration();
		rasterDirty = true;
	}
	ApplyRasterState();

	MetalVertexLayout layout;
	if ( !BindVertexArrays( pair, vertexCount, layout ) ) {
		return NULL;
	}
	MTL::RenderPipelineState *pso = GLMetal_GetPipeline( pair.programId, layout, GLMetal_HashVertexLayout( layout ), CurrentBlendKey(), false );
	if ( !pso ) {
		return NULL;
	}
	GLMetal_SetRenderPipelineState( pso );

	float uniforms[256 * 4];
	const int n = std::min( (int)pair.uniforms.size(), 256 );
	for ( int i = 0; i < n; i++ ) {
		FillUniform( pair.uniforms[i], uniforms + i * 4 );
	}
	if ( n == 0 ) {
		uniforms[0] = uniforms[1] = uniforms[2] = uniforms[3] = 0.0f;
	}
	GLMetal_SetSharedUniforms( uniforms, (size_t)std::max( n, 1 ) * 16, MGL_UNIFORM_VERTEX_SLOT, MGL_UNIFORM_FRAGMENT_SLOT );

	BindTextures( pair );
	return enc;
}

bool CullsEverything( void ) {
	return gl.cullFace && gl.cullFaceMode == GL_FRONT_AND_BACK;
}

void DrawIndexed( GLsizei count, GLenum type, const GLvoid *indices ) {
	if ( count <= 0 || CullsEverything() ) {
		return;
	}
	const size_t indexSize = ( type == GL_UNSIGNED_SHORT ) ? 2 : 4;
	if ( type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT ) {
		return;
	}
	MTL::Buffer *indexBuffer = NULL;
	NS::UInteger indexOffset = 0;
	const uint8_t *cpuIndices = NULL;
	if ( gl.elementBuffer ) {
		MglBuffer *b = BufferByName( gl.elementBuffer );
		if ( !b || !b->buf ) {
			return;
		}
		b->lastUsedFrame = GLMetal_FrameSerial();
		indexBuffer = b->buf;
		indexOffset = (NS::UInteger)(size_t)indices;
		cpuIndices = (const uint8_t *)b->buf->contents() + indexOffset;
	} else {
		if ( !indices ) {
			return;
		}
		void *dst = GLMetal_FrameAlloc( (size_t)count * indexSize, 4, &indexBuffer, &indexOffset );
		if ( !dst ) {
			return;
		}
		memcpy( dst, indices, (size_t)count * indexSize );
		cpuIndices = (const uint8_t *)indices;
	}

	// arrays in client memory are copied up to the highest index used
	int vertexCount = 0;
	bool clientArrays = false;
	if ( shaderDirty || currentPair < 0 ) {
		currentPair = LookupPair();
		shaderDirty = false;
	}
	const MglPair &pair = pairs[currentPair];
	for ( int i = 0; i < MGL_MAX_ATTRIBS; i++ ) {
		if ( pair.attribMask & ( 1u << i ) ) {
			const ClientArray *a = ArrayForAttrib( i );
			if ( a && !a->buffer && !a->directBuffer ) {
				clientArrays = true;
			}
		}
	}
	if ( clientArrays ) {
		unsigned maxIndex = 0;
		if ( indexSize == 4 ) {
			const uint32_t *p = (const uint32_t *)cpuIndices;
			for ( GLsizei i = 0; i < count; i++ ) maxIndex = std::max( maxIndex, (unsigned)p[i] );
		} else {
			const uint16_t *p = (const uint16_t *)cpuIndices;
			for ( GLsizei i = 0; i < count; i++ ) maxIndex = std::max( maxIndex, (unsigned)p[i] );
		}
		vertexCount = (int)maxIndex + 1;
	}

	MTL::RenderCommandEncoder *enc = PrepareDraw( vertexCount );
	if ( !enc ) {
		return;
	}
	GLMetal_DrawIndexed( (NS::UInteger)count, indexSize == 2 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32, indexBuffer, indexOffset );
}

// ---- clears -------------------------------------------------------------------------------

const char *kClearMSL =
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct U { float4 v[1]; };\n"
	"struct VIn { float4 a0 [[attribute(0)]]; };\n"
	"struct V2F { float4 position [[position]]; };\n"
	"vertex V2F mgl_vs( VIn in [[stage_in]] ) { V2F o; o.position = in.a0; return o; }\n"
	"fragment float4 mgl_fs( V2F in [[stage_in]], constant U &u [[buffer(0)]] ) { return u.v[0]; }\n";

// glClear honours the scissor and the write masks, which a render pass load action cannot:
// draw a rectangle instead.
void ClearBuffers( GLbitfield mask ) {
	if ( clearProgram < 0 ) {
		return;
	}
	MTL::RenderCommandEncoder *enc = GLMetal_CurrentRenderEncoder();
	if ( !enc ) {
		return;
	}
	int width, height;
	GLMetal_GetSceneSize( &width, &height );
	GLMetal_SetViewport( 0, 0, width, height, 0.0f, 1.0f );
	int sx, sy, sw, sh;
	ScissorRect( &sx, &sy, &sw, &sh );
	GLMetal_SetScissor( sx, sy, sw, sh );
	GLMetal_SetCullMode( MTL::CullModeNone );
	GLMetal_SetFillMode( MTL::TriangleFillModeFill );
	GLMetal_SetDepthBias( 0.0f, 0.0f, 0.0f );

	MetalDepthStencilKey ds;
	memset( &ds, 0, sizeof( ds ) );
	ds.depthFunc = (uint8_t)MTL::CompareFunctionAlways;
	ds.depthWrite = ( ( mask & GL_DEPTH_BUFFER_BIT ) && gl.depthMask ) ? 1 : 0;
	if ( mask & GL_STENCIL_BUFFER_BIT ) {
		ds.stencilEnable = 1;
		ds.readMask = 0xff;
		ds.writeMask = (uint8_t)gl.stencilWriteMask;
		ds.front.func = ds.back.func = (uint8_t)MTL::CompareFunctionAlways;
		ds.front.sfail = ds.front.zfail = ds.front.zpass = (uint8_t)MTL::StencilOperationReplace;
		ds.back = ds.front;
	}
	GLMetal_SetDepthStencil( ds, (uint32_t)( gl.clearStencil & 0xff ) );

	uint32_t writeMask = 0;
	if ( mask & GL_COLOR_BUFFER_BIT ) {
		writeMask = ( gl.colorMask[0] ? (uint32_t)MTL::ColorWriteMaskRed : 0 ) | ( gl.colorMask[1] ? (uint32_t)MTL::ColorWriteMaskGreen : 0 ) |
					( gl.colorMask[2] ? (uint32_t)MTL::ColorWriteMaskBlue : 0 ) | ( gl.colorMask[3] ? (uint32_t)MTL::ColorWriteMaskAlpha : 0 );
	}

	MTL::Buffer *buffer = NULL;
	NS::UInteger offset = 0;
	float *v = (float *)GLMetal_FrameAlloc( 4 * 4 * sizeof( float ), 16, &buffer, &offset );
	if ( !v ) {
		return;
	}
	const float z = gl.clearDepth;
	const float quad[16] = { -1, -1, z, 1,   1, -1, z, 1,   -1, 1, z, 1,   1, 1, z, 1 };
	memcpy( v, quad, sizeof( quad ) );

	MetalVertexLayout layout;
	memset( &layout, 0, sizeof( layout ) );
	layout.numAttribs = 1;
	layout.attribs[0].index = 0;
	layout.attribs[0].format = (uint8_t)MTL::VertexFormatFloat4;
	layout.numBuffers = 1;
	layout.buffers[0].stride = 16;
	MTL::RenderPipelineState *pso = GLMetal_GetPipeline( clearProgram, layout, GLMetal_HashVertexLayout( layout ),
														 GLMetal_BlendKey( MTL::BlendFactorOne, MTL::BlendFactorZero, false, writeMask ), false );
	if ( pso ) {
		GLMetal_SetRenderPipelineState( pso );
		GLMetal_SetVertexBuffer( buffer, offset, 0 );
		GLMetal_SetSharedUniforms( gl.clearColor, 16, MGL_UNIFORM_VERTEX_SLOT, MGL_UNIFORM_FRAGMENT_SLOT );
		if ( ds.depthWrite && sx <= 0 && sy <= 0 && sx + sw >= width && sy + sh >= height ) {
			GLMetal_DepthBufferCleared();
		}
		GLMetal_Draw( MTL::PrimitiveTypeTriangleStrip, (NS::UInteger)0, (NS::UInteger)4 );
	}
	rasterDirty = true;
}

// ---- immediate mode -----------------------------------------------------------------------

void ImmFlush( void ) {
	const size_t n = immVerts.size();
	if ( n == 0 ) {
		return;
	}
	std::vector<ImmVertex> out;
	MTL::PrimitiveType prim = MTL::PrimitiveTypeTriangle;
	switch ( immMode ) {
	case GL_POINTS:
		prim = MTL::PrimitiveTypePoint;
		out = immVerts;
		break;
	case GL_LINES:
		prim = MTL::PrimitiveTypeLine;
		out.assign( immVerts.begin(), immVerts.begin() + ( n & ~(size_t)1 ) );
		break;
	case GL_LINE_STRIP:
		prim = MTL::PrimitiveTypeLineStrip;
		out = immVerts;
		break;
	case GL_LINE_LOOP:
		prim = MTL::PrimitiveTypeLineStrip;
		out = immVerts;
		out.push_back( immVerts[0] );
		break;
	case GL_TRIANGLES:
		out.assign( immVerts.begin(), immVerts.begin() + ( n - n % 3 ) );
		break;
	case GL_TRIANGLE_STRIP:
		prim = MTL::PrimitiveTypeTriangleStrip;
		out = immVerts;
		break;
	case GL_QUADS:
		for ( size_t i = 0; i + 3 < n; i += 4 ) {
			out.push_back( immVerts[i] ); out.push_back( immVerts[i + 1] ); out.push_back( immVerts[i + 2] );
			out.push_back( immVerts[i] ); out.push_back( immVerts[i + 2] ); out.push_back( immVerts[i + 3] );
		}
		break;
	case GL_QUAD_STRIP:
		for ( size_t i = 0; i + 3 < n; i += 2 ) {
			out.push_back( immVerts[i] ); out.push_back( immVerts[i + 1] ); out.push_back( immVerts[i + 3] );
			out.push_back( immVerts[i] ); out.push_back( immVerts[i + 3] ); out.push_back( immVerts[i + 2] );
		}
		break;
	default:	// GL_TRIANGLE_FAN, GL_POLYGON
		for ( size_t i = 1; i + 1 < n; i++ ) {
			out.push_back( immVerts[0] ); out.push_back( immVerts[i] ); out.push_back( immVerts[i + 1] );
		}
		break;
	}
	immVerts.clear();
	if ( out.empty() ) {
		return;
	}
	const bool triangles = ( prim == MTL::PrimitiveTypeTriangle || prim == MTL::PrimitiveTypeTriangleStrip );
	if ( triangles && CullsEverything() ) {
		return;
	}

	MTL::Buffer *buffer = NULL;
	NS::UInteger offset = 0;
	void *dst = GLMetal_FrameAlloc( out.size() * sizeof( ImmVertex ), 16, &buffer, &offset );
	if ( !dst ) {
		return;
	}
	memcpy( dst, out.data(), out.size() * sizeof( ImmVertex ) );

	// draw through the normal path with the client arrays pointing at the copy
	const ClientArray savedVertex = gl.vertexArray, savedColor = gl.colorArray, savedTc = gl.texCoordArray[0];
	const bool saved0 = gl.attribArray[0].enabled, saved3 = gl.attribArray[3].enabled, saved8 = gl.attribArray[8].enabled;
	ClientArray a;
	memset( &a, 0, sizeof( a ) );
	a.enabled = true;
	a.size = 4;
	a.type = GL_FLOAT;
	a.stride = sizeof( ImmVertex );
	a.directBuffer = buffer;
	a.directOffset = offset + offsetof( ImmVertex, pos );
	gl.vertexArray = a;
	a.directOffset = offset + offsetof( ImmVertex, col );
	gl.colorArray = a;
	a.directOffset = offset + offsetof( ImmVertex, tc );
	gl.texCoordArray[0] = a;
	gl.attribArray[0].enabled = gl.attribArray[3].enabled = gl.attribArray[8].enabled = false;
	shaderDirty = true;

	MTL::RenderCommandEncoder *enc = PrepareDraw( (int)out.size() );
	if ( enc ) {
		GLMetal_Draw( prim, (NS::UInteger)0, (NS::UInteger)out.size() );
	}

	gl.vertexArray = savedVertex;
	gl.colorArray = savedColor;
	gl.texCoordArray[0] = savedTc;
	gl.attribArray[0].enabled = saved0;
	gl.attribArray[3].enabled = saved3;
	gl.attribArray[8].enabled = saved8;
	shaderDirty = true;
}

void ImmVertex4( float x, float y, float z, float w ) {
	if ( !immActive ) {
		return;
	}
	ImmVertex v;
	v.pos[0] = x; v.pos[1] = y; v.pos[2] = z; v.pos[3] = w;
	memcpy( v.col, gl.color, sizeof( v.col ) );
	memcpy( v.tc, gl.texCoord[0], sizeof( v.tc ) );
	immVerts.push_back( v );
}

// reads element `index` of an enabled client array as floats
bool FetchArrayElement( const ClientArray &a, int index, float out[4] ) {
	out[0] = out[1] = out[2] = 0.0f;
	out[3] = 1.0f;
	if ( !a.enabled ) {
		return false;
	}
	const uint8_t *base = (const uint8_t *)a.pointer;
	if ( a.buffer ) {
		MglBuffer *b = BufferByName( a.buffer );
		if ( !b || !b->buf ) {
			return false;
		}
		base = (const uint8_t *)b->buf->contents() + (size_t)a.pointer;
	}
	if ( !base ) {
		return false;
	}
	const int stride = a.stride ? a.stride : ElementSize( a );
	const uint8_t *p = base + (size_t)index * (size_t)stride;
	if ( a.type == GL_FLOAT ) {
		memcpy( out, p, (size_t)std::min( a.size, 4 ) * sizeof( float ) );
	} else if ( a.type == GL_UNSIGNED_BYTE ) {
		for ( int i = 0; i < std::min( a.size, 4 ); i++ ) {
			out[i] = a.normalized ? p[i] / 255.0f : (float)p[i];
		}
	}
	return true;
}

// ============================================================================
// GL entry points
// ============================================================================

void SetCap( GLenum cap, bool on ) {
	TexUnit &u = gl.unit[gl.activeTexture];
	switch ( cap ) {
	case GL_BLEND: gl.blend = on; break;
	case GL_DEPTH_TEST: gl.depthTest = on; rasterDirty = true; break;
	case GL_STENCIL_TEST: gl.stencilTest = on; rasterDirty = true; break;
	case GL_CULL_FACE: gl.cullFace = on; rasterDirty = true; break;
	case GL_SCISSOR_TEST: gl.scissorTest = on; rasterDirty = true; break;
	case GL_ALPHA_TEST: if ( gl.alphaTest != on ) { gl.alphaTest = on; shaderDirty = true; } break;
	case GL_POLYGON_OFFSET_FILL: gl.polyOffsetFill = on; rasterDirty = true; break;
	case GL_POLYGON_OFFSET_LINE: gl.polyOffsetLine = on; rasterDirty = true; break;
	case GL_VERTEX_PROGRAM_ARB: if ( gl.vertexProgram != on ) { gl.vertexProgram = on; shaderDirty = true; } break;
	case GL_FRAGMENT_PROGRAM_ARB: if ( gl.fragmentProgram != on ) { gl.fragmentProgram = on; shaderDirty = true; } break;
	case GL_TEXTURE_2D: if ( u.enabled[MT_2D] != on ) { u.enabled[MT_2D] = on; shaderDirty = true; } break;
	case GL_TEXTURE_3D: if ( u.enabled[MT_3D] != on ) { u.enabled[MT_3D] = on; shaderDirty = true; } break;
	case GL_TEXTURE_CUBE_MAP: if ( u.enabled[MT_CUBE] != on ) { u.enabled[MT_CUBE] = on; shaderDirty = true; } break;
	case GL_TEXTURE_GEN_S: case GL_TEXTURE_GEN_T: case GL_TEXTURE_GEN_R: case GL_TEXTURE_GEN_Q: {
		const int c = (int)( cap - GL_TEXTURE_GEN_S );
		if ( u.genEnabled[c] != on ) { u.genEnabled[c] = on; shaderDirty = true; }
		break;
	}
	default:
		break;	// lighting, line stipple, depth bounds, ...: nothing to do
	}
}

void APIENTRY mgl_Enable( GLenum cap ) { SetCap( cap, true ); }
void APIENTRY mgl_Disable( GLenum cap ) { SetCap( cap, false ); }

GLboolean APIENTRY mgl_IsEnabled( GLenum cap ) {
	const TexUnit &u = gl.unit[gl.activeTexture];
	switch ( cap ) {
	case GL_BLEND: return gl.blend;
	case GL_DEPTH_TEST: return gl.depthTest;
	case GL_STENCIL_TEST: return gl.stencilTest;
	case GL_CULL_FACE: return gl.cullFace;
	case GL_SCISSOR_TEST: return gl.scissorTest;
	case GL_ALPHA_TEST: return gl.alphaTest;
	case GL_TEXTURE_2D: return u.enabled[MT_2D];
	case GL_TEXTURE_3D: return u.enabled[MT_3D];
	case GL_TEXTURE_CUBE_MAP: return u.enabled[MT_CUBE];
	case GL_VERTEX_PROGRAM_ARB: return gl.vertexProgram;
	case GL_FRAGMENT_PROGRAM_ARB: return gl.fragmentProgram;
	}
	return GL_FALSE;
}

void APIENTRY mgl_BlendFunc( GLenum s, GLenum d ) { gl.blendSrc = s; gl.blendDst = d; }
void APIENTRY mgl_BlendEquation( GLenum ) {}
void APIENTRY mgl_DepthFunc( GLenum f ) { gl.depthFunc = f; rasterDirty = true; }
void APIENTRY mgl_DepthMask( GLboolean m ) { gl.depthMask = m != 0; rasterDirty = true; }
void APIENTRY mgl_ColorMask( GLboolean r, GLboolean g, GLboolean b, GLboolean a ) {
	gl.colorMask[0] = r != 0; gl.colorMask[1] = g != 0; gl.colorMask[2] = b != 0; gl.colorMask[3] = a != 0;
}
void APIENTRY mgl_AlphaFunc( GLenum f, GLclampf ref ) {
	if ( gl.alphaFunc != f ) { gl.alphaFunc = f; shaderDirty = true; }
	gl.alphaRef = ref;
}
void APIENTRY mgl_CullFace( GLenum m ) { gl.cullFaceMode = m; rasterDirty = true; }
void APIENTRY mgl_FrontFace( GLenum ) {}
void APIENTRY mgl_PolygonMode( GLenum, GLenum mode ) { gl.polygonMode = mode; rasterDirty = true; }
void APIENTRY mgl_PolygonOffset( GLfloat factor, GLfloat units ) { gl.polyOffsetFactor = factor; gl.polyOffsetUnits = units; rasterDirty = true; }
void APIENTRY mgl_StencilFunc( GLenum f, GLint ref, GLuint mask ) { gl.stencilFunc = f; gl.stencilRef = ref; gl.stencilReadMask = mask; rasterDirty = true; }
void APIENTRY mgl_StencilMask( GLuint mask ) { gl.stencilWriteMask = mask; rasterDirty = true; }
void APIENTRY mgl_StencilOp( GLenum sfail, GLenum zfail, GLenum zpass ) {
	for ( int i = 0; i < 2; i++ ) { gl.stencil[i].sfail = sfail; gl.stencil[i].zfail = zfail; gl.stencil[i].zpass = zpass; }
	rasterDirty = true;
}
void APIENTRY mgl_StencilOpSeparate( GLenum face, GLenum sfail, GLenum zfail, GLenum zpass ) {
	for ( int i = 0; i < 2; i++ ) {
		if ( face == GL_FRONT_AND_BACK || ( i == 0 && face == GL_FRONT ) || ( i == 1 && face == GL_BACK ) ) {
			gl.stencil[i].sfail = sfail; gl.stencil[i].zfail = zfail; gl.stencil[i].zpass = zpass;
		}
	}
	rasterDirty = true;
}
void APIENTRY mgl_Viewport( GLint x, GLint y, GLsizei w, GLsizei h ) { gl.viewport[0] = x; gl.viewport[1] = y; gl.viewport[2] = w; gl.viewport[3] = h; rasterDirty = true; }
void APIENTRY mgl_Scissor( GLint x, GLint y, GLsizei w, GLsizei h ) { gl.scissor[0] = x; gl.scissor[1] = y; gl.scissor[2] = w; gl.scissor[3] = h; rasterDirty = true; }
void APIENTRY mgl_DepthRange( GLclampd n, GLclampd f ) { gl.depthRange[0] = (float)n; gl.depthRange[1] = (float)f; rasterDirty = true; }
void APIENTRY mgl_ClearColor( GLclampf r, GLclampf g, GLclampf b, GLclampf a ) { gl.clearColor[0] = r; gl.clearColor[1] = g; gl.clearColor[2] = b; gl.clearColor[3] = a; }
void APIENTRY mgl_ClearDepth( GLclampd d ) { gl.clearDepth = (float)d; }
void APIENTRY mgl_ClearStencil( GLint s ) { gl.clearStencil = s; }
void APIENTRY mgl_Clear( GLbitfield mask ) { ClearBuffers( mask ); }
void APIENTRY mgl_Nop1( GLenum ) {}
void APIENTRY mgl_Nop2( GLenum, GLenum ) {}
void APIENTRY mgl_NopF( GLfloat ) {}
void APIENTRY mgl_Nop0( void ) {}
void APIENTRY mgl_Finish( void ) { GLMetal_FlushAndWait(); }
void APIENTRY mgl_PixelStorei( GLenum pname, GLint param ) {
	if ( pname == GL_PACK_ALIGNMENT ) gl.packAlignment = param;
	else if ( pname == GL_UNPACK_ALIGNMENT ) gl.unpackAlignment = param;
	else if ( pname == GL_UNPACK_ROW_LENGTH ) gl.unpackRowLength = param;
}

// glPushAttrib / glPopAttrib save and restore the whole state, whatever the mask: the
// callers (Dear ImGui, debug tools) bracket a block of drawing and want it undone.
std::vector<GLState> attribStack;
void APIENTRY mgl_PushAttrib( GLbitfield ) {
	if ( attribStack.size() < 8 ) {
		attribStack.push_back( gl );
	}
}
void APIENTRY mgl_PopAttrib( void ) {
	if ( attribStack.empty() ) {
		return;
	}
	const GLenum error = gl.error;
	gl = attribStack.back();
	attribStack.pop_back();
	gl.error = error;
	gl.mvpDirty = true;
	shaderDirty = true;
	rasterDirty = true;
}

GLenum APIENTRY mgl_GetError( void ) {
	const GLenum e = gl.error;
	gl.error = GL_NO_ERROR;
	return e;
}

const GLubyte *APIENTRY mgl_GetString( GLenum name ) {
	switch ( name ) {
	case GL_VENDOR: return (const GLubyte *)"Apple";
	case GL_RENDERER: return (const GLubyte *)rendererString.c_str();
	case GL_VERSION: return (const GLubyte *)"2.0 dhewm3 Metal";
	case GL_EXTENSIONS: return (const GLubyte *)extensionsString.c_str();
	case GL_PROGRAM_ERROR_STRING_ARB: return (const GLubyte *)programErrorString.c_str();
	}
	return (const GLubyte *)"";
}

void APIENTRY mgl_GetIntegerv( GLenum pname, GLint *params ) {
	switch ( pname ) {
	case GL_MAX_TEXTURE_SIZE: params[0] = 16384; break;
	case GL_MAX_TEXTURE_UNITS_ARB: params[0] = 8; break;
	case GL_MAX_TEXTURE_COORDS_ARB: params[0] = 8; break;
	case GL_MAX_TEXTURE_IMAGE_UNITS_ARB: params[0] = 8; break;
	case GL_PROGRAM_ERROR_POSITION_ARB: params[0] = programErrorPos; break;
	case GL_ARRAY_BUFFER_BINDING_ARB: params[0] = (GLint)gl.arrayBuffer; break;
	case GL_ELEMENT_ARRAY_BUFFER_BINDING_ARB: params[0] = (GLint)gl.elementBuffer; break;
	case GL_READ_BUFFER: case GL_DRAW_BUFFER: params[0] = GL_BACK; break;
	case GL_VIEWPORT: memcpy( params, gl.viewport, sizeof( gl.viewport ) ); break;
	case GL_SCISSOR_BOX: memcpy( params, gl.scissor, sizeof( gl.scissor ) ); break;
	case GL_TEXTURE_BINDING_2D: params[0] = (GLint)gl.unit[gl.activeTexture].bound[MT_2D]; break;
	case GL_STENCIL_BITS: params[0] = 8; break;
	case GL_DEPTH_BITS: params[0] = 24; break;
	case GL_MATRIX_MODE: params[0] = (GLint)gl.matrixMode; break;
	case GL_POLYGON_MODE: params[0] = params[1] = (GLint)gl.polygonMode; break;
	case GL_SHADE_MODEL: params[0] = GL_SMOOTH; break;
	default: params[0] = 0; break;
	}
}

void APIENTRY mgl_GetFloatv( GLenum pname, GLfloat *params ) {
	switch ( pname ) {
	case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: params[0] = 16.0f; break;
	case GL_MODELVIEW_MATRIX: memcpy( params, gl.modelview[gl.modelviewTop], 16 * sizeof( float ) ); break;
	case GL_PROJECTION_MATRIX: memcpy( params, gl.projection[gl.projectionTop], 16 * sizeof( float ) ); break;
	case GL_CURRENT_COLOR: memcpy( params, gl.color, 4 * sizeof( float ) ); break;
	default: params[0] = 0.0f; break;
	}
}

// ---- matrices -----------------------------------------------------------------------------

void APIENTRY mgl_MatrixMode( GLenum mode ) { gl.matrixMode = mode; }
void APIENTRY mgl_LoadIdentity( void ) { MatIdentity( CurrentMatrix() ); MatrixChanged(); }
void APIENTRY mgl_LoadMatrixf( const GLfloat *m ) { memcpy( CurrentMatrix(), m, 16 * sizeof( float ) ); MatrixChanged(); }
void APIENTRY mgl_MultMatrixf( const GLfloat *m ) { float *c = CurrentMatrix(); MatMul( c, m, c ); MatrixChanged(); }
void APIENTRY mgl_Ortho( GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f ) {
	float m[16];
	MatIdentity( m );
	m[0] = (float)( 2.0 / ( r - l ) );
	m[5] = (float)( 2.0 / ( t - b ) );
	m[10] = (float)( -2.0 / ( f - n ) );
	m[12] = (float)( -( r + l ) / ( r - l ) );
	m[13] = (float)( -( t + b ) / ( t - b ) );
	m[14] = (float)( -( f + n ) / ( f - n ) );
	mgl_MultMatrixf( m );
}
void APIENTRY mgl_Translatef( GLfloat x, GLfloat y, GLfloat z ) {
	float m[16];
	MatIdentity( m );
	m[12] = x; m[13] = y; m[14] = z;
	mgl_MultMatrixf( m );
}
void APIENTRY mgl_Scalef( GLfloat x, GLfloat y, GLfloat z ) {
	float m[16];
	MatIdentity( m );
	m[0] = x; m[5] = y; m[10] = z;
	mgl_MultMatrixf( m );
}
void APIENTRY mgl_Rotatef( GLfloat angle, GLfloat x, GLfloat y, GLfloat z ) {
	const float len = sqrtf( x * x + y * y + z * z );
	if ( len == 0.0f ) {
		return;
	}
	x /= len; y /= len; z /= len;
	const float rad = angle * 3.14159265358979323846f / 180.0f;
	const float c = cosf( rad ), s = sinf( rad ), t = 1.0f - c;
	float m[16];
	MatIdentity( m );
	m[0] = x * x * t + c;     m[4] = x * y * t - z * s; m[8] = x * z * t + y * s;
	m[1] = y * x * t + z * s; m[5] = y * y * t + c;     m[9] = y * z * t - x * s;
	m[2] = z * x * t - y * s; m[6] = z * y * t + x * s; m[10] = z * z * t + c;
	mgl_MultMatrixf( m );
}
void APIENTRY mgl_PushMatrix( void ) {
	if ( gl.matrixMode == GL_PROJECTION ) {
		if ( gl.projectionTop < MGL_MATRIX_STACK - 1 ) {
			memcpy( gl.projection[gl.projectionTop + 1], gl.projection[gl.projectionTop], 16 * sizeof( float ) );
			gl.projectionTop++;
		}
	} else if ( gl.matrixMode == GL_TEXTURE ) {
		TexUnit &u = gl.unit[gl.activeTexture];
		if ( u.matrixTop < MGL_MATRIX_STACK - 1 ) {
			memcpy( u.matrix[u.matrixTop + 1], u.matrix[u.matrixTop], 16 * sizeof( float ) );
			u.matrixTop++;
		}
	} else if ( gl.modelviewTop < MGL_MATRIX_STACK - 1 ) {
		memcpy( gl.modelview[gl.modelviewTop + 1], gl.modelview[gl.modelviewTop], 16 * sizeof( float ) );
		gl.modelviewTop++;
	}
}
void APIENTRY mgl_PopMatrix( void ) {
	if ( gl.matrixMode == GL_PROJECTION ) {
		if ( gl.projectionTop > 0 ) gl.projectionTop--;
	} else if ( gl.matrixMode == GL_TEXTURE ) {
		TexUnit &u = gl.unit[gl.activeTexture];
		if ( u.matrixTop > 0 ) u.matrixTop--;
	} else if ( gl.modelviewTop > 0 ) {
		gl.modelviewTop--;
	}
	MatrixChanged();
}

// ---- current values and immediate mode ------------------------------------------------------

void APIENTRY mgl_Color4f( GLfloat r, GLfloat g, GLfloat b, GLfloat a ) { gl.color[0] = r; gl.color[1] = g; gl.color[2] = b; gl.color[3] = a; }
void APIENTRY mgl_Color3f( GLfloat r, GLfloat g, GLfloat b ) { mgl_Color4f( r, g, b, 1.0f ); }
void APIENTRY mgl_Color3fv( const GLfloat *v ) { mgl_Color4f( v[0], v[1], v[2], 1.0f ); }
void APIENTRY mgl_Color4fv( const GLfloat *v ) { mgl_Color4f( v[0], v[1], v[2], v[3] ); }
void APIENTRY mgl_Color4ubv( const GLubyte *v ) { mgl_Color4f( v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f ); }
void APIENTRY mgl_Color3ubv( const GLubyte *v ) { mgl_Color4f( v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, 1.0f ); }
void APIENTRY mgl_Color4ub( GLubyte r, GLubyte g, GLubyte b, GLubyte a ) { mgl_Color4f( r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f ); }
void APIENTRY mgl_Normal3f( GLfloat x, GLfloat y, GLfloat z ) { gl.normal[0] = x; gl.normal[1] = y; gl.normal[2] = z; }
void APIENTRY mgl_Normal3fv( const GLfloat *v ) { mgl_Normal3f( v[0], v[1], v[2] ); }
void APIENTRY mgl_TexCoord2f( GLfloat s, GLfloat t ) { float *c = gl.texCoord[0]; c[0] = s; c[1] = t; c[2] = 0.0f; c[3] = 1.0f; }
void APIENTRY mgl_TexCoord2fv( const GLfloat *v ) { mgl_TexCoord2f( v[0], v[1] ); }
void APIENTRY mgl_MultiTexCoord2fARB( GLenum unit, GLfloat s, GLfloat t ) {
	const int i = (int)( unit - GL_TEXTURE0 );
	if ( i >= 0 && i < 8 ) { float *c = gl.texCoord[i]; c[0] = s; c[1] = t; c[2] = 0.0f; c[3] = 1.0f; }
}
void APIENTRY mgl_MultiTexCoord2fvARB( GLenum unit, GLfloat *v ) { mgl_MultiTexCoord2fARB( unit, v[0], v[1] ); }

void APIENTRY mgl_Begin( GLenum mode ) { immMode = mode; immActive = true; immVerts.clear(); }
void APIENTRY mgl_End( void ) { if ( immActive ) { immActive = false; ImmFlush(); } }
void APIENTRY mgl_Vertex2f( GLfloat x, GLfloat y ) { ImmVertex4( x, y, 0.0f, 1.0f ); }
void APIENTRY mgl_Vertex2fv( const GLfloat *v ) { ImmVertex4( v[0], v[1], 0.0f, 1.0f ); }
void APIENTRY mgl_Vertex3f( GLfloat x, GLfloat y, GLfloat z ) { ImmVertex4( x, y, z, 1.0f ); }
void APIENTRY mgl_Vertex3fv( const GLfloat *v ) { ImmVertex4( v[0], v[1], v[2], 1.0f ); }
void APIENTRY mgl_Vertex4f( GLfloat x, GLfloat y, GLfloat z, GLfloat w ) { ImmVertex4( x, y, z, w ); }
void APIENTRY mgl_Vertex4fv( const GLfloat *v ) { ImmVertex4( v[0], v[1], v[2], v[3] ); }
void APIENTRY mgl_ArrayElement( GLint i ) {
	float v[4];
	if ( FetchArrayElement( gl.colorArray, i, v ) ) memcpy( gl.color, v, sizeof( v ) );
	if ( FetchArrayElement( gl.texCoordArray[0], i, v ) ) memcpy( gl.texCoord[0], v, sizeof( v ) );
	if ( FetchArrayElement( gl.vertexArray, i, v ) ) ImmVertex4( v[0], v[1], v[2], v[3] );
}

// ---- client arrays --------------------------------------------------------------------------

ClientArray *ArrayForClientState( GLenum array ) {
	switch ( array ) {
	case GL_VERTEX_ARRAY: return &gl.vertexArray;
	case GL_COLOR_ARRAY: return &gl.colorArray;
	case GL_NORMAL_ARRAY: return &gl.normalArray;
	case GL_TEXTURE_COORD_ARRAY: return &gl.texCoordArray[gl.clientActiveTexture];
	}
	return NULL;
}
void SetClientState( GLenum array, bool on ) {
	ClientArray *a = ArrayForClientState( array );
	if ( a && a->enabled != on ) {
		a->enabled = on;
		shaderDirty = true;
	}
}
void APIENTRY mgl_EnableClientState( GLenum array ) { SetClientState( array, true ); }
void APIENTRY mgl_DisableClientState( GLenum array ) { SetClientState( array, false ); }

void SetPointer( ClientArray &a, int size, GLenum type, bool normalized, GLsizei stride, const GLvoid *pointer ) {
	a.size = size;
	a.type = type;
	a.normalized = normalized;
	a.stride = stride;
	a.pointer = pointer;
	a.buffer = gl.arrayBuffer;
	a.directBuffer = NULL;
	a.directOffset = 0;
}
void APIENTRY mgl_VertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *p ) { SetPointer( gl.vertexArray, size, type, false, stride, p ); }
void APIENTRY mgl_ColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *p ) { SetPointer( gl.colorArray, size, type, true, stride, p ); }
void APIENTRY mgl_NormalPointer( GLenum type, GLsizei stride, const GLvoid *p ) { SetPointer( gl.normalArray, 3, type, true, stride, p ); }
void APIENTRY mgl_TexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *p ) { SetPointer( gl.texCoordArray[gl.clientActiveTexture], size, type, false, stride, p ); }
void APIENTRY mgl_VertexAttribPointerARB( GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const GLvoid *p ) {
	if ( index < (GLuint)MGL_MAX_ATTRIBS ) SetPointer( gl.attribArray[index], size, type, normalized != 0, stride, p );
}
void APIENTRY mgl_EnableVertexAttribArrayARB( GLuint index ) {
	if ( index < (GLuint)MGL_MAX_ATTRIBS && !gl.attribArray[index].enabled ) { gl.attribArray[index].enabled = true; shaderDirty = true; }
}
void APIENTRY mgl_DisableVertexAttribArrayARB( GLuint index ) {
	if ( index < (GLuint)MGL_MAX_ATTRIBS && gl.attribArray[index].enabled ) { gl.attribArray[index].enabled = false; shaderDirty = true; }
}
void APIENTRY mgl_ActiveTextureARB( GLenum texture ) {
	const int i = (int)( texture - GL_TEXTURE0 );
	if ( i >= 0 && i < MGL_MAX_UNITS ) gl.activeTexture = i;
}
void APIENTRY mgl_ClientActiveTextureARB( GLenum texture ) {
	const int i = (int)( texture - GL_TEXTURE0 );
	if ( i >= 0 && i < 8 ) gl.clientActiveTexture = i;
}

void APIENTRY mgl_DrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices ) {
	if ( mode != GL_TRIANGLES ) {
		return;
	}
	DrawIndexed( count, type, indices );
}

void APIENTRY mgl_DrawArrays( GLenum mode, GLint first, GLsizei count ) {
	if ( count <= 0 || ( mode == GL_TRIANGLES && CullsEverything() ) ) {
		return;
	}
	MTL::PrimitiveType prim;
	switch ( mode ) {
	case GL_TRIANGLES: prim = MTL::PrimitiveTypeTriangle; break;
	case GL_TRIANGLE_STRIP: prim = MTL::PrimitiveTypeTriangleStrip; break;
	case GL_LINES: prim = MTL::PrimitiveTypeLine; break;
	case GL_LINE_STRIP: prim = MTL::PrimitiveTypeLineStrip; break;
	case GL_POINTS: prim = MTL::PrimitiveTypePoint; break;
	default: return;
	}
	MTL::RenderCommandEncoder *enc = PrepareDraw( first + count );
	if ( enc ) {
		GLMetal_Draw( prim, (NS::UInteger)first, (NS::UInteger)count );
	}
}

// ---- textures -------------------------------------------------------------------------------

void APIENTRY mgl_GenTextures( GLsizei n, GLuint *names ) {
	for ( GLsizei i = 0; i < n; i++ ) {
		names[i] = nextTextureName++;
	}
}

void APIENTRY mgl_BindTexture( GLenum target, GLuint name ) {
	const int ti = TargetIndex( target );
	if ( ti == MT_NONE ) {
		return;
	}
	TexUnit &u = gl.unit[gl.activeTexture];
	if ( u.bound[ti] == name ) {
		return;
	}
	MglTexture *t = NULL;
	if ( name != 0 ) {
		t = TextureByName( name );
		if ( !t ) {
			t = new MglTexture();
			t->target = ti;
			textures[name] = t;
			if ( name >= nextTextureName ) {
				nextTextureName = name + 1;
			}
		}
	}
	// the fixed-function fragment stage depends on the texture's base format
	if ( gl.activeTexture < 8 && u.enabled[ti] && !gl.fragmentProgram ) {
		const MglTexture *old = TextureByName( u.bound[ti] );
		const int oldClass = ( old && old->tex ) ? old->formatClass : MF_RGBA;
		const int newClass = ( t && t->tex ) ? t->formatClass : MF_RGBA;
		if ( oldClass != newClass ) {
			shaderDirty = true;
		}
	}
	u.bound[ti] = name;
}

void APIENTRY mgl_DeleteTextures( GLsizei n, const GLuint *names ) {
	for ( GLsizei i = 0; i < n; i++ ) {
		auto it = textures.find( names[i] );
		if ( it == textures.end() ) {
			continue;
		}
		for ( int u = 0; u < MGL_MAX_UNITS; u++ ) {
			for ( int k = 0; k < 4; k++ ) {
				if ( gl.unit[u].bound[k] == names[i] ) {
					gl.unit[u].bound[k] = 0;
				}
			}
		}
		ReleaseTextureStorage( it->second );
		delete it->second;
		textures.erase( it );
		shaderDirty = true;
	}
}

void APIENTRY mgl_TexImage2D( GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type, const GLvoid *pixels ) {
	TexImage( target, level, internalFormat, w, h, 1, format, type, pixels );
}

void APIENTRY mgl_TexImage3D( GLenum target, GLint level, GLint internalFormat, GLsizei w, GLsizei h, GLsizei d, GLint, GLenum format, GLenum type, const GLvoid *pixels ) {
	TexImage( target, level, internalFormat, w, h, d, format, type, pixels );
}

void APIENTRY mgl_TexSubImage2D( GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *pixels ) {
	MglTexture *t = BoundTexture( target );
	if ( !t || !t->tex ) {
		return;
	}
	UploadLevel( t, CubeFace( target ), level, x, y, 0, w, h, 1, format, type, pixels );
}

void APIENTRY mgl_CompressedTexImage2DARB( GLenum target, GLint level, GLenum internalFormat, GLsizei w, GLsizei h, GLint, GLsizei imageSize, const GLvoid *data ) {
	MglTexture *t = BoundTexture( target );
	const int ti = TargetIndex( target );
	if ( !t || ti == MT_NONE || w <= 0 || h <= 0 ) {
		return;
	}
	const FormatInfo f = FormatForInternal( internalFormat, true, GL_RGBA );
	if ( !f.compressed ) {
		MetalHost_Warning( "[Metal] unsupported compressed texture format 0x%x", internalFormat );
		return;
	}
	if ( level == 0 && !EnsureTextureStorage( t, ti, f, internalFormat, w, h, 1 ) ) {
		return;
	}
	if ( !t->tex || !data || level >= t->levels ) {
		return;
	}
	const int blocksWide = ( w + 3 ) / 4, blocksHigh = ( h + 3 ) / 4;
	const int bytesPerRow = blocksWide * f.blockBytes;
	const int bytes = bytesPerRow * blocksHigh;
	if ( imageSize < bytes ) {
		return;
	}
	GLMetal_UploadTextureRegion( t->tex, CubeFace( target ), level, 0, 0, 0, w, h, 1, data, bytesPerRow, bytes );
	if ( level > t->maxLevelDefined ) {
		t->maxLevelDefined = level;
		t->sampler = NULL;
	}
}

// Copies from the framebuffer need a texture in the framebuffer's own pixel format.
bool EnsureCopyTarget( MglTexture *t, bool depth, int w, int h ) {
	const uint32_t R = (uint32_t)MTL::TextureSwizzleRed, G = (uint32_t)MTL::TextureSwizzleGreen;
	const uint32_t B = (uint32_t)MTL::TextureSwizzleBlue, O = (uint32_t)MTL::TextureSwizzleOne;
	FormatInfo f;
	memset( &f, 0, sizeof( f ) );
	if ( depth ) {
		f.pixelFormat = MTL::PixelFormatR32Float;
		f.swizzle = SWZ( R, R, R, O );
		f.formatClass = MF_LUMINANCE;
		f.depth = true;
	} else {
		f.pixelFormat = GLMetal_SceneColorFormat();
		f.swizzle = SWZ( R, G, B, O );
		f.formatClass = MF_RGB;
	}
	return EnsureTextureStorage( t, MT_2D, f, depth ? GL_DEPTH_COMPONENT24 : GL_RGB8, w, h, 1 );
}

// A depth copy made by replay runs before everything else of the frame, so a second copy in
// a frame (another view) must not overwrite what the draws of the first one sample: the
// texture object gets another storage for it.
void CopySceneDepth( MglTexture *t, int x, int y, int w, int h, int xoff, int yoff ) {
	if ( t->copyFrame != GLMetal_FrameSerial() ) {
		t->copyFrame = GLMetal_FrameSerial();
		t->copyCount = 0;
	}
	const int k = t->copyCount++;
	bool replay = GLMetal_CanReplayDepth() && k < 8;
	if ( replay ) {
		if ( t->copyStorage.empty() ) {
			t->copyStorage.push_back( t->tex );
		}
		while ( replay && (int)t->copyStorage.size() <= k ) {
			MTL::Texture *storage = GLMetal_NewTexture( MTL::TextureType2D, t->pixelFormat, t->width, t->height, 1, t->levels, false, t->swizzle );
			if ( storage ) {
				t->copyStorage.push_back( storage );
			} else {
				replay = false;
			}
		}
		if ( replay ) {
			t->tex = t->copyStorage[k];
		}
	}
	GLMetal_CopySceneToTexture( true, x, y, w, h, t->tex, xoff, yoff, replay );
}

void APIENTRY mgl_CopyTexImage2D( GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei w, GLsizei h, GLint ) {
	MglTexture *t = BoundTexture( target );
	if ( !t || level != 0 || w <= 0 || h <= 0 ) {
		return;
	}
	const bool depth = ( internalFormat == GL_DEPTH_COMPONENT || internalFormat == GL_DEPTH_COMPONENT16 ||
						 internalFormat == GL_DEPTH_COMPONENT24 || internalFormat == GL_DEPTH_COMPONENT32 );
	if ( !EnsureCopyTarget( t, depth, w, h ) ) {
		return;
	}
	if ( depth ) {
		CopySceneDepth( t, x, y, w, h, 0, 0 );
	} else {
		GLMetal_CopySceneToTexture( false, x, y, w, h, t->tex, 0, 0 );
	}
}

void APIENTRY mgl_CopyTexSubImage2D( GLenum target, GLint level, GLint xoff, GLint yoff, GLint x, GLint y, GLsizei w, GLsizei h ) {
	MglTexture *t = BoundTexture( target );
	if ( !t || !t->tex || level != 0 ) {
		return;
	}
	if ( !EnsureCopyTarget( t, t->isDepth, t->width, t->height ) ) {
		return;
	}
	if ( t->isDepth ) {
		CopySceneDepth( t, x, y, w, h, xoff, yoff );
	} else {
		GLMetal_CopySceneToTexture( false, x, y, w, h, t->tex, xoff, yoff );
	}
}

void TexParameter( GLenum target, GLenum pname, float value ) {
	MglTexture *t = BoundTexture( target );
	if ( !t ) {
		return;
	}
	switch ( pname ) {
	case GL_TEXTURE_MIN_FILTER: t->minFilter = (GLenum)value; break;
	case GL_TEXTURE_MAG_FILTER: t->magFilter = (GLenum)value; break;
	case GL_TEXTURE_WRAP_S: t->wrapS = (GLenum)value; break;
	case GL_TEXTURE_WRAP_T: t->wrapT = (GLenum)value; break;
	case GL_TEXTURE_WRAP_R: t->wrapR = (GLenum)value; break;
	case GL_TEXTURE_MAX_ANISOTROPY_EXT: t->anisotropy = value; break;
	case GL_TEXTURE_LOD_BIAS: t->lodBias = value; break;
	case GL_TEXTURE_MAX_LEVEL: t->maxLevel = (int)value; break;
	default: return;
	}
	t->sampler = NULL;
}
void APIENTRY mgl_TexParameterf( GLenum target, GLenum pname, GLfloat value ) { TexParameter( target, pname, value ); }
void APIENTRY mgl_TexParameteri( GLenum target, GLenum pname, GLint value ) { TexParameter( target, pname, (float)value ); }
void APIENTRY mgl_TexParameterfv( GLenum target, GLenum pname, const GLfloat *v ) {
	if ( pname == GL_TEXTURE_BORDER_COLOR ) {
		MglTexture *t = BoundTexture( target );
		if ( t ) {
			t->border = ( v[3] == 0.0f ) ? 0 : ( ( v[0] + v[1] + v[2] ) > 1.5f ? 2 : 1 );
			t->sampler = NULL;
		}
		return;
	}
	TexParameter( target, pname, v[0] );
}

// ---- texture environment and texgen ----------------------------------------------------------

uint8_t CombineFuncFor( GLint v ) {
	switch ( v ) {
	case GL_REPLACE: return MC_REPLACE;
	case GL_ADD: return MC_ADD;
	case GL_ADD_SIGNED: return MC_ADD_SIGNED;
	case GL_INTERPOLATE: return MC_INTERPOLATE;
	case GL_SUBTRACT: return MC_SUBTRACT;
	case GL_DOT3_RGB: return MC_DOT3_RGB;
	case GL_DOT3_RGBA: return MC_DOT3_RGBA;
	}
	return MC_MODULATE;
}
uint8_t CombineSrcFor( GLint v ) {
	switch ( v ) {
	case GL_CONSTANT: return MS_CONSTANT;
	case GL_PRIMARY_COLOR: return MS_PRIMARY;
	case GL_PREVIOUS: return MS_PREVIOUS;
	}
	return MS_TEXTURE;
}
uint8_t CombineOpFor( GLint v ) {
	switch ( v ) {
	case GL_ONE_MINUS_SRC_COLOR: return MO_ONE_MINUS_SRC_COLOR;
	case GL_SRC_ALPHA: return MO_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA: return MO_ONE_MINUS_SRC_ALPHA;
	}
	return MO_SRC_COLOR;
}

void APIENTRY mgl_TexEnvi( GLenum target, GLenum pname, GLint v ) {
	if ( target != GL_TEXTURE_ENV ) {
		return;
	}
	TexUnit &u = gl.unit[gl.activeTexture];
	switch ( pname ) {
	case GL_TEXTURE_ENV_MODE:
		switch ( v ) {
		case GL_REPLACE: u.envMode = ME_REPLACE; break;
		case GL_DECAL: u.envMode = ME_DECAL; break;
		case GL_ADD: u.envMode = ME_ADD; break;
		case GL_BLEND: u.envMode = ME_BLEND; break;
		case GL_COMBINE: u.envMode = ME_COMBINE; break;
		default: u.envMode = ME_MODULATE; break;
		}
		break;
	case GL_COMBINE_RGB: u.combineRGB = CombineFuncFor( v ); break;
	case GL_COMBINE_ALPHA: u.combineA = CombineFuncFor( v ); break;
	case GL_SOURCE0_RGB: case GL_SOURCE1_RGB: case GL_SOURCE2_RGB: u.srcRGB[pname - GL_SOURCE0_RGB] = CombineSrcFor( v ); break;
	case GL_SOURCE0_ALPHA: case GL_SOURCE1_ALPHA: case GL_SOURCE2_ALPHA: u.srcA[pname - GL_SOURCE0_ALPHA] = CombineSrcFor( v ); break;
	case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB: u.opRGB[pname - GL_OPERAND0_RGB] = CombineOpFor( v ); break;
	case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA: u.opA[pname - GL_OPERAND0_ALPHA] = CombineOpFor( v ); break;
	case GL_RGB_SCALE: u.rgbScale = v; break;
	case GL_ALPHA_SCALE: u.alphaScale = v; break;
	default: return;
	}
	shaderDirty = true;
}
void APIENTRY mgl_TexEnvf( GLenum target, GLenum pname, GLfloat v ) { mgl_TexEnvi( target, pname, (GLint)v ); }
void APIENTRY mgl_TexEnvfv( GLenum target, GLenum pname, const GLfloat *v ) {
	if ( target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_COLOR ) {
		// OpenGL clamps the environment colour to [0,1] when it is set
		for ( int i = 0; i < 4; i++ ) {
			gl.unit[gl.activeTexture].envColor[i] = std::max( 0.0f, std::min( 1.0f, v[i] ) );
		}
	} else {
		mgl_TexEnvi( target, pname, (GLint)v[0] );
	}
}
void APIENTRY mgl_GetTexEnviv( GLenum, GLenum pname, GLint *params ) {
	static const GLint modes[] = { GL_MODULATE, GL_REPLACE, GL_DECAL, GL_ADD, GL_BLEND, GL_COMBINE };
	params[0] = ( pname == GL_TEXTURE_ENV_MODE ) ? modes[gl.unit[gl.activeTexture].envMode] : 0;
}

void APIENTRY mgl_TexGenf( GLenum coord, GLenum pname, GLfloat v ) {
	const int c = (int)( coord - GL_S );
	if ( c < 0 || c > 3 || pname != GL_TEXTURE_GEN_MODE ) {
		return;
	}
	uint8_t mode = MG_OBJECT_LINEAR;
	switch ( (GLenum)v ) {
	case GL_EYE_LINEAR: mode = MG_EYE_LINEAR; break;
	case GL_REFLECTION_MAP: mode = MG_REFLECTION_MAP; break;
	case GL_NORMAL_MAP: mode = MG_NORMAL_MAP; break;
	}
	TexUnit &u = gl.unit[gl.activeTexture];
	if ( u.genMode[c] != mode ) {
		u.genMode[c] = mode;
		shaderDirty = true;
	}
}
void APIENTRY mgl_TexGeni( GLenum coord, GLenum pname, GLint v ) { mgl_TexGenf( coord, pname, (GLfloat)v ); }
void APIENTRY mgl_TexGenfv( GLenum coord, GLenum pname, const GLfloat *v ) {
	const int c = (int)( coord - GL_S );
	if ( c < 0 || c > 3 ) {
		return;
	}
	TexUnit &u = gl.unit[gl.activeTexture];
	if ( pname == GL_OBJECT_PLANE ) {
		memcpy( u.objPlane[c], v, 4 * sizeof( float ) );
	} else if ( pname == GL_EYE_PLANE ) {
		// OpenGL stores an eye plane multiplied by the inverse of the modelview matrix
		float inv[16];
		MatInverse( gl.modelview[gl.modelviewTop], inv );
		for ( int i = 0; i < 4; i++ ) {
			u.eyePlane[c][i] = v[0] * inv[i * 4 + 0] + v[1] * inv[i * 4 + 1] + v[2] * inv[i * 4 + 2] + v[3] * inv[i * 4 + 3];
		}
	} else {
		mgl_TexGenf( coord, pname, v[0] );
	}
}

// ---- buffer objects -------------------------------------------------------------------------

GLuint *BufferBinding( GLenum target ) {
	return ( target == GL_ELEMENT_ARRAY_BUFFER ) ? &gl.elementBuffer : &gl.arrayBuffer;
}
void APIENTRY mgl_GenBuffersARB( GLsizei n, GLuint *names ) {
	for ( GLsizei i = 0; i < n; i++ ) {
		names[i] = nextBufferName++;
		buffers[names[i]] = new MglBuffer();
	}
}
void APIENTRY mgl_BindBufferARB( GLenum target, GLuint name ) {
	*BufferBinding( target ) = name;
	if ( name && buffers.find( name ) == buffers.end() ) {
		buffers[name] = new MglBuffer();
		if ( name >= nextBufferName ) nextBufferName = name + 1;
	}
}
void APIENTRY mgl_DeleteBuffersARB( GLsizei n, const GLuint *names ) {
	for ( GLsizei i = 0; i < n; i++ ) {
		auto it = buffers.find( names[i] );
		if ( it == buffers.end() ) continue;
		GLMetal_ReleaseBuffer( it->second->buf );
		delete it->second;
		buffers.erase( it );
		if ( gl.arrayBuffer == names[i] ) gl.arrayBuffer = 0;
		if ( gl.elementBuffer == names[i] ) gl.elementBuffer = 0;
	}
}
GLboolean APIENTRY mgl_IsBufferARB( GLuint name ) { return BufferByName( name ) != NULL; }
void APIENTRY mgl_BufferDataARB( GLenum target, GLsizeiptrARB size, const GLvoid *data, GLenum ) {
	MglBuffer *b = BufferByName( *BufferBinding( target ) );
	if ( !b ) {
		return;
	}
	// new storage every time: frames in flight may still read the old one
	GLMetal_ReleaseBuffer( b->buf );
	b->buf = NULL;
	b->size = 0;
	b->lastUsedFrame = ~0ull;
	if ( size <= 0 ) {
		return;
	}
	b->buf = data ? GLMetal_Device()->newBuffer( data, (NS::UInteger)size, MTL::ResourceStorageModeShared )
				  : GLMetal_Device()->newBuffer( (NS::UInteger)size, MTL::ResourceStorageModeShared );
	b->size = b->buf ? (size_t)size : 0;
}
void APIENTRY mgl_BufferSubDataARB( GLenum target, GLintptrARB offset, GLsizeiptrARB size, const GLvoid *data ) {
	MglBuffer *b = BufferByName( *BufferBinding( target ) );
	if ( !b || !b->buf || !data || offset < 0 || (size_t)offset + (size_t)size > b->size ) {
		return;
	}
	MakeBufferWritable( b );
	memcpy( (uint8_t *)b->buf->contents() + offset, data, (size_t)size );
}
void APIENTRY mgl_GetBufferSubDataARB( GLenum target, GLintptrARB offset, GLsizeiptrARB size, GLvoid *data ) {
	MglBuffer *b = BufferByName( *BufferBinding( target ) );
	if ( b && b->buf && (size_t)offset + (size_t)size <= b->size ) {
		memcpy( data, (const uint8_t *)b->buf->contents() + offset, (size_t)size );
	}
}
GLvoid *APIENTRY mgl_MapBufferARB( GLenum target, GLenum ) {
	MglBuffer *b = BufferByName( *BufferBinding( target ) );
	if ( !b || !b->buf ) {
		return NULL;
	}
	MakeBufferWritable( b );
	return b->buf->contents();
}
GLboolean APIENTRY mgl_UnmapBufferARB( GLenum ) { return GL_TRUE; }
void APIENTRY mgl_GetBufferParameterivARB( GLenum target, GLenum pname, GLint *params ) {
	MglBuffer *b = BufferByName( *BufferBinding( target ) );
	params[0] = ( b && pname == GL_BUFFER_SIZE ) ? (GLint)b->size : 0;
}
void APIENTRY mgl_GetBufferPointervARB( GLenum target, GLenum, GLvoid **params ) {
	MglBuffer *b = BufferByName( *BufferBinding( target ) );
	params[0] = ( b && b->buf ) ? b->buf->contents() : NULL;
}

// ---- ARB programs ---------------------------------------------------------------------------

int StageFor( GLenum target ) { return ( target == GL_FRAGMENT_PROGRAM_ARB ) ? 1 : 0; }

void APIENTRY mgl_GenProgramsARB( GLsizei n, GLuint *names ) {
	for ( GLsizei i = 0; i < n; i++ ) {
		names[i] = nextProgramName++;
	}
}
void APIENTRY mgl_BindProgramARB( GLenum target, GLuint name ) {
	const int stage = StageFor( target );
	if ( gl.boundProgram[stage] != name ) {
		gl.boundProgram[stage] = name;
		shaderDirty = true;
	}
}
void APIENTRY mgl_ProgramStringARB( GLenum target, GLenum, GLsizei len, const GLvoid *string ) {
	const int stage = StageFor( target );
	MglArbProgram &prog = programs[gl.boundProgram[stage]];
	prog.version = programVersionCounter++;
	shaderDirty = true;
	programErrorPos = -1;
	programErrorString.clear();
	if ( !MglArb_Translate( (const char *)string, (int)len, stage == 1, prog ) ) {
		programErrorPos = prog.errorPos;
		programErrorString = prog.error;
		MetalHost_Warning( "[Metal] ARB %s program %u not translated: %s", stage ? "fragment" : "vertex", gl.boundProgram[stage], prog.error.c_str() );
		SetError( GL_INVALID_OPERATION );
	}
}
void APIENTRY mgl_ProgramEnvParameter4fvARB( GLenum target, GLuint index, const GLfloat *v ) {
	if ( index < 256 ) {
		memcpy( gl.env[StageFor( target )][index], v, 4 * sizeof( float ) );
	}
}
void APIENTRY mgl_ProgramLocalParameter4fvARB( GLenum target, GLuint index, const GLfloat *v ) {
	if ( index < 32 ) {
		memcpy( programs[gl.boundProgram[StageFor( target )]].local[index], v, 4 * sizeof( float ) );
	}
}

// ---- reading back ---------------------------------------------------------------------------

void APIENTRY mgl_ReadPixels( GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *data ) {
	if ( w <= 0 || h <= 0 || !data ) {
		return;
	}
	// clip to the scene; what lies outside is left untouched, as in OpenGL
	int sw, sh;
	GLMetal_GetSceneSize( &sw, &sh );
	if ( x < 0 || y < 0 || x + w > sw || y + h > sh ) {
		return;
	}
	if ( format == GL_STENCIL_INDEX && type == GL_UNSIGNED_BYTE ) {
		GLMetal_ReadScenePixels( METAL_READ_STENCIL, x, y, w, h, data );
		return;
	}
	if ( format == GL_DEPTH_COMPONENT && type == GL_FLOAT ) {
		GLMetal_ReadScenePixels( METAL_READ_DEPTH, x, y, w, h, data );
		return;
	}
	if ( type != GL_UNSIGNED_BYTE || ( format != GL_RGB && format != GL_RGBA ) ) {
		return;
	}
	std::vector<uint8_t> rgba( (size_t)w * (size_t)h * 4 );
	if ( !GLMetal_ReadScenePixels( METAL_READ_COLOR, x, y, w, h, rgba.data() ) ) {
		return;
	}
	if ( format == GL_RGBA ) {
		memcpy( data, rgba.data(), rgba.size() );
		return;
	}
	const int align = std::max( gl.packAlignment, 1 );
	const size_t rowBytes = ( (size_t)w * 3 + (size_t)align - 1 ) / (size_t)align * (size_t)align;
	uint8_t *out = (uint8_t *)data;
	for ( int row = 0; row < h; row++ ) {
		const uint8_t *s = rgba.data() + (size_t)row * (size_t)w * 4;
		uint8_t *d = out + (size_t)row * rowBytes;
		for ( int i = 0; i < w; i++ ) {
			d[i * 3 + 0] = s[i * 4 + 0];
			d[i * 3 + 1] = s[i * 4 + 1];
			d[i * 3 + 2] = s[i * 4 + 2];
		}
	}
}

// ============================================================================
// function table
// ============================================================================

void StubCalled( const char *name ) {
	if ( !stubsReported[name] ) {
		stubsReported[name] = true;
		MetalHost_Printf( "[Metal] note: %s is not implemented by the Metal backend\n", name );
	}
}

// one stub for every function of the GL 1.1 table, used where nothing is implemented
#define QGLPROC( name, rettype, args ) void stub_##name( void ) { StubCalled( #name ); }
#include "renderer/qgl_proc.h"
#undef QGLPROC

typedef void ( *GenericProc )( void );
struct ProcEntry {
	const char *name;
	GenericProc proc;
};

#define P( glname, fn ) { #glname, (GenericProc)fn }
const ProcEntry implemented[] = {
	P( glEnable, mgl_Enable ), P( glDisable, mgl_Disable ), P( glIsEnabled, mgl_IsEnabled ),
	P( glBlendFunc, mgl_BlendFunc ), P( glBlendEquation, mgl_BlendEquation ), P( glDepthFunc, mgl_DepthFunc ),
	P( glDepthMask, mgl_DepthMask ), P( glColorMask, mgl_ColorMask ), P( glAlphaFunc, mgl_AlphaFunc ),
	P( glCullFace, mgl_CullFace ), P( glFrontFace, mgl_FrontFace ), P( glPolygonMode, mgl_PolygonMode ),
	P( glPolygonOffset, mgl_PolygonOffset ), P( glStencilFunc, mgl_StencilFunc ), P( glStencilMask, mgl_StencilMask ),
	P( glStencilOp, mgl_StencilOp ), P( glStencilOpSeparate, mgl_StencilOpSeparate ),
	P( glViewport, mgl_Viewport ), P( glScissor, mgl_Scissor ), P( glDepthRange, mgl_DepthRange ),
	P( glClearColor, mgl_ClearColor ), P( glClearDepth, mgl_ClearDepth ), P( glClearStencil, mgl_ClearStencil ),
	P( glClear, mgl_Clear ), P( glDrawBuffer, mgl_Nop1 ), P( glReadBuffer, mgl_Nop1 ), P( glShadeModel, mgl_Nop1 ),
	P( glHint, mgl_Nop2 ), P( glLineWidth, mgl_NopF ), P( glPointSize, mgl_NopF ), P( glFlush, mgl_Nop0 ),
	P( glFinish, mgl_Finish ), P( glPixelStorei, mgl_PixelStorei ), P( glPushAttrib, mgl_PushAttrib ),
	P( glPopAttrib, mgl_PopAttrib ), P( glGetError, mgl_GetError ), P( glGetString, mgl_GetString ),
	P( glGetIntegerv, mgl_GetIntegerv ), P( glGetFloatv, mgl_GetFloatv ),
	P( glMatrixMode, mgl_MatrixMode ), P( glLoadIdentity, mgl_LoadIdentity ), P( glLoadMatrixf, mgl_LoadMatrixf ),
	P( glMultMatrixf, mgl_MultMatrixf ), P( glOrtho, mgl_Ortho ), P( glTranslatef, mgl_Translatef ),
	P( glScalef, mgl_Scalef ), P( glRotatef, mgl_Rotatef ), P( glPushMatrix, mgl_PushMatrix ), P( glPopMatrix, mgl_PopMatrix ),
	P( glColor3f, mgl_Color3f ), P( glColor3fv, mgl_Color3fv ), P( glColor4f, mgl_Color4f ), P( glColor4fv, mgl_Color4fv ),
	P( glColor4ubv, mgl_Color4ubv ), P( glColor3ubv, mgl_Color3ubv ), P( glColor4ub, mgl_Color4ub ),
	P( glNormal3f, mgl_Normal3f ), P( glNormal3fv, mgl_Normal3fv ),
	P( glTexCoord2f, mgl_TexCoord2f ), P( glTexCoord2fv, mgl_TexCoord2fv ),
	P( glMultiTexCoord2fARB, mgl_MultiTexCoord2fARB ), P( glMultiTexCoord2fvARB, mgl_MultiTexCoord2fvARB ),
	P( glBegin, mgl_Begin ), P( glEnd, mgl_End ), P( glVertex2f, mgl_Vertex2f ), P( glVertex2fv, mgl_Vertex2fv ),
	P( glVertex3f, mgl_Vertex3f ), P( glVertex3fv, mgl_Vertex3fv ), P( glVertex4f, mgl_Vertex4f ), P( glVertex4fv, mgl_Vertex4fv ),
	P( glArrayElement, mgl_ArrayElement ),
	P( glEnableClientState, mgl_EnableClientState ), P( glDisableClientState, mgl_DisableClientState ),
	P( glVertexPointer, mgl_VertexPointer ), P( glColorPointer, mgl_ColorPointer ), P( glNormalPointer, mgl_NormalPointer ),
	P( glTexCoordPointer, mgl_TexCoordPointer ), P( glVertexAttribPointerARB, mgl_VertexAttribPointerARB ),
	P( glEnableVertexAttribArrayARB, mgl_EnableVertexAttribArrayARB ), P( glDisableVertexAttribArrayARB, mgl_DisableVertexAttribArrayARB ),
	P( glActiveTextureARB, mgl_ActiveTextureARB ), P( glClientActiveTextureARB, mgl_ClientActiveTextureARB ),
	P( glDrawElements, mgl_DrawElements ), P( glDrawArrays, mgl_DrawArrays ),
	P( glGenTextures, mgl_GenTextures ), P( glBindTexture, mgl_BindTexture ), P( glDeleteTextures, mgl_DeleteTextures ),
	P( glTexImage2D, mgl_TexImage2D ), P( glTexImage3D, mgl_TexImage3D ), P( glTexSubImage2D, mgl_TexSubImage2D ),
	P( glCompressedTexImage2DARB, mgl_CompressedTexImage2DARB ),
	P( glCopyTexImage2D, mgl_CopyTexImage2D ), P( glCopyTexSubImage2D, mgl_CopyTexSubImage2D ),
	P( glTexParameterf, mgl_TexParameterf ), P( glTexParameteri, mgl_TexParameteri ), P( glTexParameterfv, mgl_TexParameterfv ),
	P( glTexEnvi, mgl_TexEnvi ), P( glTexEnvf, mgl_TexEnvf ), P( glTexEnvfv, mgl_TexEnvfv ), P( glGetTexEnviv, mgl_GetTexEnviv ),
	P( glTexGenf, mgl_TexGenf ), P( glTexGeni, mgl_TexGeni ), P( glTexGenfv, mgl_TexGenfv ),
	P( glGenBuffersARB, mgl_GenBuffersARB ), P( glBindBufferARB, mgl_BindBufferARB ), P( glDeleteBuffersARB, mgl_DeleteBuffersARB ),
	P( glIsBufferARB, mgl_IsBufferARB ), P( glBufferDataARB, mgl_BufferDataARB ), P( glBufferSubDataARB, mgl_BufferSubDataARB ),
	P( glGetBufferSubDataARB, mgl_GetBufferSubDataARB ), P( glMapBufferARB, mgl_MapBufferARB ), P( glUnmapBufferARB, mgl_UnmapBufferARB ),
	P( glGetBufferParameterivARB, mgl_GetBufferParameterivARB ), P( glGetBufferPointervARB, mgl_GetBufferPointervARB ),
	P( glGenProgramsARB, mgl_GenProgramsARB ), P( glBindProgramARB, mgl_BindProgramARB ), P( glProgramStringARB, mgl_ProgramStringARB ),
	P( glProgramEnvParameter4fvARB, mgl_ProgramEnvParameter4fvARB ), P( glProgramLocalParameter4fvARB, mgl_ProgramLocalParameter4fvARB ),
	P( glReadPixels, mgl_ReadPixels ),
};
#undef P

#define QGLPROC( name, rettype, args ) { #name, (GenericProc)stub_##name },
const ProcEntry stubs[] = {
#include "renderer/qgl_proc.h"
};
#undef QGLPROC

void ResetState( void ) {
	memset( &gl, 0, sizeof( gl ) );
	gl.blendSrc = GL_ONE;
	gl.blendDst = GL_ZERO;
	gl.depthFunc = GL_LESS;
	gl.depthMask = true;
	gl.colorMask[0] = gl.colorMask[1] = gl.colorMask[2] = gl.colorMask[3] = true;
	gl.alphaFunc = GL_ALWAYS;
	gl.cullFaceMode = GL_BACK;
	gl.polygonMode = GL_FILL;
	gl.stencilFunc = GL_ALWAYS;
	gl.stencilReadMask = 0xff;
	gl.stencilWriteMask = 0xff;
	for ( int i = 0; i < 2; i++ ) {
		gl.stencil[i].sfail = gl.stencil[i].zfail = gl.stencil[i].zpass = GL_KEEP;
	}
	gl.clearDepth = 1.0f;
	gl.depthRange[1] = 1.0f;
	GLMetal_GetSceneSize( &gl.viewport[2], &gl.viewport[3] );
	GLMetal_GetSceneSize( &gl.scissor[2], &gl.scissor[3] );
	gl.matrixMode = GL_MODELVIEW;
	MatIdentity( gl.modelview[0] );
	MatIdentity( gl.projection[0] );
	gl.mvpDirty = true;
	gl.color[0] = gl.color[1] = gl.color[2] = gl.color[3] = 1.0f;
	gl.normal[2] = 1.0f;
	gl.normal[3] = 0.0f;
	for ( int i = 0; i < 8; i++ ) {
		gl.texCoord[i][3] = 1.0f;
	}
	for ( int i = 0; i < MGL_MAX_UNITS; i++ ) {
		TexUnit &u = gl.unit[i];
		u.envMode = ME_MODULATE;
		u.combineRGB = u.combineA = MC_MODULATE;
		u.srcRGB[0] = u.srcA[0] = MS_TEXTURE;
		u.srcRGB[1] = u.srcA[1] = MS_PREVIOUS;
		u.srcRGB[2] = u.srcA[2] = MS_CONSTANT;
		u.opRGB[0] = u.opRGB[1] = MO_SRC_COLOR;
		u.opRGB[2] = MO_SRC_ALPHA;
		u.opA[0] = u.opA[1] = u.opA[2] = MO_SRC_ALPHA;
		u.rgbScale = u.alphaScale = 1;
		for ( int c = 0; c < 4; c++ ) {
			u.genMode[c] = MG_EYE_LINEAR;
		}
		u.objPlane[0][0] = u.eyePlane[0][0] = 1.0f;
		u.objPlane[1][1] = u.eyePlane[1][1] = 1.0f;
		MatIdentity( u.matrix[0] );
		u.matrixIdentity = true;
	}
	gl.packAlignment = gl.unpackAlignment = 4;
	gl.vertexArray.size = 4;
	gl.vertexArray.type = GL_FLOAT;
	shaderDirty = true;
	rasterDirty = true;
	currentPair = -1;
}

} // namespace

// ============================================================================
// interface to sys/glimp.cpp
// ============================================================================

extern "C" int MGL_Init( void *caMetalLayer, int width, int height, int multiSamples ) {
	if ( !GLMetal_DeviceInit( caMetalLayer, width, height, multiSamples ) ) {
		GLMetal_Shutdown();
		return 0;
	}
	ResetState();

	rendererString = std::string( "Metal: " ) + GLMetal_Device()->name()->utf8String();
	extensionsString = "GL_ARB_multitexture GL_ARB_texture_env_combine GL_ARB_texture_cube_map GL_ARB_texture_env_dot3 "
					   "GL_ARB_texture_env_add GL_ARB_texture_non_power_of_two GL_EXT_texture_filter_anisotropic "
					   "GL_EXT_texture3D GL_EXT_stencil_wrap GL_ARB_vertex_buffer_object GL_ARB_vertex_program "
					   "GL_ARB_fragment_program GL_EXT_texture_lod";
	if ( GLMetal_Device()->supportsBCTextureCompression() ) {
		extensionsString += " GL_ARB_texture_compression GL_EXT_texture_compression_s3tc GL_ARB_texture_compression_bptc";
	}

	fallbackTexture[MT_NONE] = NULL;
	fallbackTexture[MT_2D] = NewFallbackTexture( MT_2D );
	fallbackTexture[MT_CUBE] = NewFallbackTexture( MT_CUBE );
	fallbackTexture[MT_3D] = NewFallbackTexture( MT_3D );

	MTL::Library *lib = GLMetal_LoadShaderLibraryFromSource( kClearMSL, "clear" );
	if ( lib ) {
		MTL::Function *vs = lib->newFunction( NS::String::string( "mgl_vs", NS::UTF8StringEncoding ) );
		MTL::Function *fs = lib->newFunction( NS::String::string( "mgl_fs", NS::UTF8StringEncoding ) );
		clearProgram = GLMetal_RegisterProgram( "clear", vs, fs );
		if ( vs ) vs->release();
		if ( fs ) fs->release();
		lib->release();
	}
	initialized = true;
	return GLMetal_SampleCount();
}

extern "C" void MGL_Shutdown( void ) {
	if ( !initialized ) {
		return;
	}
	initialized = false;
	for ( auto &kv : textures ) {
		ReleaseTextureStorage( kv.second );
		delete kv.second;
	}
	textures.clear();
	for ( int i = 0; i < 4; i++ ) {
		if ( fallbackTexture[i] ) {
			ReleaseTextureStorage( fallbackTexture[i] );
			delete fallbackTexture[i];
			fallbackTexture[i] = NULL;
		}
	}
	for ( auto &kv : buffers ) {
		GLMetal_ReleaseBuffer( kv.second->buf );
		delete kv.second;
	}
	buffers.clear();
	programs.clear();
	pairs.clear();
	pairIndex.clear();
	currentPair = -1;
	clearProgram = -1;
	immVerts.clear();
	GLMetal_Shutdown();
}

extern "C" float MGL_BeginParticleLayer( void ) {
	if ( gl.polygonMode != GL_FILL || gl.alphaTest || ( gl.depthTest && gl.depthFunc != GL_ALWAYS ) ||
		 ( gl.stencilTest && gl.stencilFunc != GL_ALWAYS ) ) {
		return 0.0f;
	}
	return (float)GLMetal_BeginBlendLayer( CurrentBlendKey() );
}

extern "C" void MGL_EndParticleLayer( void ) {
	GLMetal_EndBlendLayer();
}

extern "C" void MGL_SwapBuffers( void ) {
	if ( initialized ) {
		GLMetal_Present();
		debugSkipPair = MetalHost_CvarInt( "r_metalSkipPair" );
	}
}

extern "C" void MGL_Resize( int width, int height ) {
	if ( initialized ) {
		GLMetal_Resize( width, height );
		rasterDirty = true;
	}
}

extern "C" void ( *MGL_GetProcAddress( const char *name ) )( void ) {
	for ( size_t i = 0; i < sizeof( implemented ) / sizeof( implemented[0] ); i++ ) {
		if ( strcmp( implemented[i].name, name ) == 0 ) {
			return implemented[i].proc;
		}
	}
	for ( size_t i = 0; i < sizeof( stubs ) / sizeof( stubs[0] ); i++ ) {
		if ( strcmp( stubs[i].name, name ) == 0 ) {
			return stubs[i].proc;
		}
	}
	return NULL;
}
