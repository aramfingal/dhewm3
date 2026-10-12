/*
mgl_local.h - shared declarations of the GL-on-Metal layer (mgl*.cpp).
*/
#ifndef MGL_LOCAL_H
#define MGL_LOCAL_H

#include <stdint.h>
#include <string>
#include <vector>

// ---- uniforms ---------------------------------------------------------------------------
// A shader pair reads its uniforms from one block of float4 slots. Each slot has a source:

enum MglUniformKind {
	MU_ENV,				// a = 0 vertex / 1 fragment, b = index: glProgramEnvParameter4fvARB
	MU_LOCAL,			// a = 0 vertex / 1 fragment, b = index: glProgramLocalParameter4fvARB
	MU_MATRIX_ROW,		// a = matrix (MM_*), b = row | transform << 2 (MX_*)
	MU_TEXGEN_OBJECT,	// a = unit, b = coordinate (0 s .. 3 q): object plane
	MU_TEXGEN_EYE,		// a = unit, b = coordinate: eye plane
	MU_CURRENT_ATTRIB,	// a = attribute index: value used when its array is disabled
	MU_ALPHA_REF,		// x = alpha test reference
	MU_ENV_COLOR,		// a = unit: GL_TEXTURE_ENV_COLOR
	MU_TEX_SIZE,		// a = unit: 1/width, 1/height of the bound rectangle texture
};

enum { MM_MVP, MM_MODELVIEW, MM_PROJECTION, MM_TEXTURE0 };	// MM_TEXTURE0 + unit
enum { MX_NONE, MX_INVERSE, MX_TRANSPOSE, MX_INVTRANS };

struct MglUniform {
	uint8_t kind;
	uint8_t a;
	uint16_t b;
	bool operator==( const MglUniform &o ) const { return kind == o.kind && a == o.a && b == o.b; }
};

// ---- varyings ---------------------------------------------------------------------------

enum {
	MV_COLOR0 = 1 << 0,
	MV_COLOR1 = 1 << 1,
	MV_FOG = 1 << 2,
	MV_TEXCOORD0 = 1 << 8,	// MV_TEXCOORD0 << unit
};

enum { MT_NONE, MT_2D, MT_CUBE, MT_3D, MT_RECT };

// ---- one translated ARB program ----------------------------------------------------------
// The body is MSL statements over these float4 variables:
//   vertex:   a0..a15 (attributes), o_pos, o_col0, o_col1, o_fog, o_tc0..o_tc7
//   fragment: f_pos, f_col0, f_col1, f_fog, f_tc0..f_tc7, r_col, r_depth; textures t<n>/s<n>
// and "@U<n>@" for the n-th entry of `uniforms`.

struct MglArbProgram {
	bool valid;
	bool isFragment;
	bool positionInvariant;
	bool writesDepth;
	bool readsFragCoord;
	std::string error;
	int errorPos;
	std::string body;
	std::vector<MglUniform> uniforms;
	uint32_t attribsRead;		// vertex: bit per attribute index
	uint32_t varyings;			// vertex: written, fragment: read
	uint32_t texMask;			// fragment: image units sampled
	uint8_t texTarget[16];		// MT_*
	uint32_t version;			// changes whenever the program text changes
	float local[32][4];

	MglArbProgram() : valid( false ), isFragment( false ), positionInvariant( false ), writesDepth( false ),
		readsFragCoord( false ), errorPos( -1 ), attribsRead( 0 ), varyings( 0 ), texMask( 0 ), version( 0 ) {
		for ( int i = 0; i < 16; i++ ) texTarget[i] = MT_NONE;
		for ( int i = 0; i < 32; i++ ) { local[i][0] = local[i][1] = local[i][2] = local[i][3] = 0.0f; }
	}
};

// Translates ARB_vertex_program / ARB_fragment_program assembly. Returns false and sets
// out.error / out.errorPos (byte offset into text) when the text cannot be translated.
bool MglArb_Translate( const char *text, int length, bool fragment, MglArbProgram &out );

// ---- shader pairs -------------------------------------------------------------------------

// texture base formats: they decide what a fixed-function texture environment does
enum { MF_RGBA, MF_RGB, MF_ALPHA, MF_LUMINANCE, MF_LUMINANCE_ALPHA, MF_INTENSITY };
enum { ME_MODULATE, ME_REPLACE, ME_DECAL, ME_ADD, ME_BLEND, ME_COMBINE };
enum { MC_REPLACE, MC_MODULATE, MC_ADD, MC_ADD_SIGNED, MC_INTERPOLATE, MC_SUBTRACT, MC_DOT3_RGB, MC_DOT3_RGBA };
enum { MS_TEXTURE, MS_CONSTANT, MS_PRIMARY, MS_PREVIOUS };
enum { MO_SRC_COLOR, MO_ONE_MINUS_SRC_COLOR, MO_SRC_ALPHA, MO_ONE_MINUS_SRC_ALPHA };
enum { MG_OBJECT_LINEAR, MG_EYE_LINEAR, MG_REFLECTION_MAP, MG_NORMAL_MAP };
// alpha test: 0 = off, otherwise 1 + (GL func - GL_NEVER)
enum { MA_OFF, MA_NEVER, MA_LESS, MA_EQUAL, MA_LEQUAL, MA_GREATER, MA_NOTEQUAL, MA_GEQUAL, MA_ALWAYS };

// Fixed-function state of one texture unit. Plain bytes: the struct is hashed and compared
// with memcmp, so it must be zero-filled before use.
struct MglFFUnit {
	uint8_t target;			// fragment: MT_*, MT_NONE when texturing is off for the unit
	uint8_t formatClass;	// fragment: MF_*
	uint8_t envMode;		// fragment: ME_*
	uint8_t combineRGB, combineA;			// MC_*
	uint8_t srcRGB[3], opRGB[3];			// MS_*, MO_*
	uint8_t srcA[3], opA[3];
	uint8_t rgbScale, alphaScale;			// 1, 2 or 4
	uint8_t genMask;		// vertex: bit per coordinate (s t r q) with texgen on
	uint8_t genModes;		// vertex: MG_* << ( 2 * coordinate )
	uint8_t texMatrix;		// vertex: the texture matrix is not identity
};

struct MglPairKey {
	const MglArbProgram *vp;	// NULL: fixed-function vertex stage
	const MglArbProgram *fp;	// NULL: fixed-function fragment stage
	uint32_t vpId, vpVersion;
	uint32_t fpId, fpVersion;
	uint32_t arrayMask;			// attributes that come from enabled arrays
	uint8_t alphaFunc;			// MA_*
	uint8_t pad[3];
	MglFFUnit unit[8];
};

struct MglPair {
	int programId;				// device program id, -1 when the pair failed to build
	std::vector<MglUniform> uniforms;
	uint32_t attribMask;		// attributes the vertex function reads from arrays
	uint32_t texMask;			// image units the fragment function samples
	uint8_t texTarget[16];		// MT_*
	std::string name;
};

// Generates the MSL for a pair. Fills everything in `out` except programId.
uint32_t MglShader_FragmentVaryings( const MglPairKey &key );
uint32_t MglShader_VertexAttribs( const MglPairKey &key );
void MglShader_Generate( const MglPairKey &key, MglPair &out, std::string &msl );

#endif
