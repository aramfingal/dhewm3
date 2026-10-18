/*
mgl_shader.cpp - generates the MSL for one vertex/fragment pair.

Each stage is either the fixed-function pipeline in a given configuration or a translated
ARB program (mgl_arb.cpp). Both stages of a pair are generated together, so the vertex
function writes exactly the varyings the fragment function reads, and both read one uniform
block that holds only the values this pair uses.

Vertex buffers use slots 0..n, the uniform block is vertex buffer 30 and fragment buffer 0.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mgl_local.h"

uint32_t MglShader_FragmentVaryings( const MglPairKey &key ) {
	if ( key.fp ) {
		return key.fp->varyings;
	}
	uint32_t v = MV_COLOR0;
	for ( int i = 0; i < 8; i++ ) {
		if ( key.unit[i].target != MT_NONE ) {
			v |= MV_TEXCOORD0 << i;
		}
	}
	return v;
}

// Attributes the vertex function reads (from arrays or, when an array is off, from the
// current value).
uint32_t MglShader_VertexAttribs( const MglPairKey &key ) {
	if ( key.vp ) {
		// a shadowed pair also needs the position for the shadow map lookup
		return key.vp->attribsRead | ( ( key.vp->positionInvariant || key.shadow ) ? 1u : 0u );
	}
	const uint32_t reads = MglShader_FragmentVaryings( key );
	uint32_t a = 1u;
	if ( reads & MV_COLOR0 ) {
		a |= 1u << 3;
	}
	for ( int i = 0; i < 8; i++ ) {
		if ( !( reads & ( MV_TEXCOORD0 << i ) ) ) {
			continue;
		}
		const MglFFUnit &u = key.unit[i];
		if ( ( u.genMask & 15 ) != 15 ) {
			a |= 1u << ( 8 + i );
		}
		for ( int c = 0; c < 4; c++ ) {
			const int mode = ( u.genModes >> ( 2 * c ) ) & 3;
			if ( ( u.genMask & ( 1 << c ) ) && ( mode == MG_REFLECTION_MAP || mode == MG_NORMAL_MAP ) ) {
				a |= 1u << 2;
			}
		}
	}
	return a;
}

namespace {

struct Builder {
	MglPair &pair;
	explicit Builder( MglPair &p ) : pair( p ) {}

	std::string U( int kind, int a, int b ) {
		MglUniform u;
		u.kind = (uint8_t)kind;
		u.a = (uint8_t)a;
		u.b = (uint16_t)b;
		size_t i;
		for ( i = 0; i < pair.uniforms.size(); i++ ) {
			if ( pair.uniforms[i] == u ) break;
		}
		if ( i == pair.uniforms.size() ) {
			pair.uniforms.push_back( u );
		}
		char buf[32];
		snprintf( buf, sizeof( buf ), "u.v[%d]", (int)i );
		return buf;
	}

	// replaces the "@U<n>@" references of a translated program with slots of this pair
	std::string Resolve( const MglArbProgram &prog ) {
		std::string out;
		const std::string &body = prog.body;
		size_t i = 0;
		while ( i < body.size() ) {
			if ( body[i] == '@' && i + 1 < body.size() && body[i + 1] == 'U' ) {
				size_t j = body.find( '@', i + 2 );
				const int n = atoi( body.substr( i + 2, j - i - 2 ).c_str() );
				const MglUniform &u = prog.uniforms[n];
				out += U( u.kind, u.a, u.b );
				i = j + 1;
			} else {
				out += body[i++];
			}
		}
		return out;
	}
};

std::string Num( int n ) {
	char buf[16];
	snprintf( buf, sizeof( buf ), "%d", n );
	return buf;
}

const char *kCoordNames = "xyzw";

void VertexStage( const MglPairKey &key, Builder &b, uint32_t fsReads, std::string &msl ) {
	const uint32_t attribs = MglShader_VertexAttribs( key );
	b.pair.attribMask = attribs & key.arrayMask;

	std::string body;
	if ( b.pair.attribMask ) {
		msl += "struct VIn {\n";
		for ( int i = 0; i < 16; i++ ) {
			if ( b.pair.attribMask & ( 1u << i ) ) {
				msl += "\tfloat4 a" + Num( i ) + " [[attribute(" + Num( i ) + ")]];\n";
			}
		}
		msl += "};\n";
	}
	for ( int i = 0; i < 16; i++ ) {
		if ( !( attribs & ( 1u << i ) ) ) {
			continue;
		}
		if ( b.pair.attribMask & ( 1u << i ) ) {
			body += "\tfloat4 a" + Num( i ) + " = in.a" + Num( i ) + ";\n";
		} else {
			body += "\tfloat4 a" + Num( i ) + " = " + b.U( MU_CURRENT_ATTRIB, i, 0 ) + ";\n";
		}
	}
	body += "\tfloat4 o_pos = float4(0.0, 0.0, 0.0, 1.0);\n"
			// a vertex program that does not write result.color leaves it undefined; Apple's
			// OpenGL gives black (r_showShadows relies on nothing better), so do the same
			"\tfloat4 o_col0 = float4(0.0, 0.0, 0.0, 1.0);\n"
			"\tfloat4 o_col1 = float4(0.0, 0.0, 0.0, 1.0);\n"
			"\tfloat4 o_fog = float4(0.0, 0.0, 0.0, 1.0);\n"
			"\tfloat4 o_psize = float4(1.0);\n";
	for ( int i = 0; i < 8; i++ ) {
		body += "\tfloat4 o_tc" + Num( i ) + " = float4(0.0, 0.0, 0.0, 1.0);\n";
	}

	// the same expression in every pair, so that depth is identical from pass to pass
	const bool ffPosition = !key.vp || key.vp->positionInvariant;
	if ( ffPosition ) {
		body += "\to_pos = " + b.U( MU_MATRIX_ROW, MM_MVP, 0 | ( MX_TRANSPOSE << 2 ) ) + " * a0.x + "
				+ b.U( MU_MATRIX_ROW, MM_MVP, 1 | ( MX_TRANSPOSE << 2 ) ) + " * a0.y + "
				+ b.U( MU_MATRIX_ROW, MM_MVP, 2 | ( MX_TRANSPOSE << 2 ) ) + " * a0.z + "
				+ b.U( MU_MATRIX_ROW, MM_MVP, 3 | ( MX_TRANSPOSE << 2 ) ) + " * a0.w;\n";
	}

	if ( key.vp ) {
		body += b.Resolve( *key.vp );
	} else {
		if ( fsReads & MV_COLOR0 ) {
			body += "\to_col0 = a3;\n";
		}
		bool haveReflection = false;
		for ( int i = 0; i < 8; i++ ) {
			const MglFFUnit &u = key.unit[i];
			bool eye = false;
			for ( int c = 0; c < 4; c++ ) {
				eye = eye || ( ( u.genMask & ( 1 << c ) ) && ( ( u.genModes >> ( 2 * c ) ) & 3 ) == MG_EYE_LINEAR );
			}
			if ( eye && ( fsReads & ( MV_TEXCOORD0 << i ) ) ) {
				body += "\tfloat4 mgl_eye = float4( dot( " + b.U( MU_MATRIX_ROW, MM_MODELVIEW, 0 ) + ", a0 ), dot( "
					+ b.U( MU_MATRIX_ROW, MM_MODELVIEW, 1 ) + ", a0 ), dot( " + b.U( MU_MATRIX_ROW, MM_MODELVIEW, 2 ) + ", a0 ), dot( "
					+ b.U( MU_MATRIX_ROW, MM_MODELVIEW, 3 ) + ", a0 ) );\n";
				break;
			}
		}
		for ( int i = 0; i < 8; i++ ) {
			if ( !( fsReads & ( MV_TEXCOORD0 << i ) ) ) {
				continue;
			}
			const MglFFUnit &u = key.unit[i];
			const std::string tc = "tc" + Num( i );
			if ( attribs & ( 1u << ( 8 + i ) ) ) {
				body += "\tfloat4 " + tc + " = a" + Num( 8 + i ) + ";\n";
			} else {
				body += "\tfloat4 " + tc + " = float4(0.0, 0.0, 0.0, 1.0);\n";
			}
			for ( int c = 0; c < 4; c++ ) {
				if ( !( u.genMask & ( 1 << c ) ) ) {
					continue;
				}
				const int mode = ( u.genModes >> ( 2 * c ) ) & 3;
				if ( mode == MG_OBJECT_LINEAR ) {
					body += "\t" + tc + "." + kCoordNames[c] + " = dot( " + b.U( MU_TEXGEN_OBJECT, i, c ) + ", a0 );\n";
				} else if ( mode == MG_EYE_LINEAR ) {
					body += "\t" + tc + "." + kCoordNames[c] + " = dot( " + b.U( MU_TEXGEN_EYE, i, c ) + ", mgl_eye );\n";
				} else if ( c < 3 ) {
					if ( !haveReflection ) {
						haveReflection = true;
						const std::string decl =
							"\tfloat3 mgl_eyeP = float3( dot( " + b.U( MU_MATRIX_ROW, MM_MODELVIEW, 0 ) + ", a0 ), dot( "
							+ b.U( MU_MATRIX_ROW, MM_MODELVIEW, 1 ) + ", a0 ), dot( " + b.U( MU_MATRIX_ROW, MM_MODELVIEW, 2 ) + ", a0 ) );\n"
							"\tfloat3 mgl_eyeN = normalize( float3( dot( " + b.U( MU_MATRIX_ROW, MM_MODELVIEW, 0 | ( MX_INVTRANS << 2 ) ) + ".xyz, a2.xyz ), dot( "
							+ b.U( MU_MATRIX_ROW, MM_MODELVIEW, 1 | ( MX_INVTRANS << 2 ) ) + ".xyz, a2.xyz ), dot( "
							+ b.U( MU_MATRIX_ROW, MM_MODELVIEW, 2 | ( MX_INVTRANS << 2 ) ) + ".xyz, a2.xyz ) ) );\n"
							"\tfloat3 mgl_eyeU = normalize( mgl_eyeP );\n"
							"\tfloat3 mgl_refl = mgl_eyeU - 2.0 * mgl_eyeN * dot( mgl_eyeN, mgl_eyeU );\n";
						body += decl;
					}
					body += "\t" + tc + "." + kCoordNames[c] + ( mode == MG_REFLECTION_MAP ? " = mgl_refl." : " = mgl_eyeN." ) + kCoordNames[c] + ";\n";
				}
			}
			if ( u.texMatrix ) {
				body += "\t" + tc + " = float4( dot( " + b.U( MU_MATRIX_ROW, MM_TEXTURE0 + i, 0 ) + ", " + tc + " ), dot( "
						+ b.U( MU_MATRIX_ROW, MM_TEXTURE0 + i, 1 ) + ", " + tc + " ), dot( "
						+ b.U( MU_MATRIX_ROW, MM_TEXTURE0 + i, 2 ) + ", " + tc + " ), dot( "
						+ b.U( MU_MATRIX_ROW, MM_TEXTURE0 + i, 3 ) + ", " + tc + " ) );\n";
			}
			body += "\to_tc" + Num( i ) + " = " + tc + ";\n";
		}
	}

	// GL clip space to Metal's; the image is rendered upside down (docs/metal/DESIGN.md)
	body += "\tV2F o;\n\to.position = float4( o_pos.x, -o_pos.y, ( o_pos.z + o_pos.w ) * 0.5, o_pos.w );\n";
	if ( key.shadow ) {
		// eye space position, for the shadow map lookup in the fragment function
		body += "\to.shw = float4( dot( " + b.U( MU_MATRIX_ROW, MM_MODELVIEW, 0 ) + ", a0 ), dot( "
				+ b.U( MU_MATRIX_ROW, MM_MODELVIEW, 1 ) + ", a0 ), dot( " + b.U( MU_MATRIX_ROW, MM_MODELVIEW, 2 ) + ", a0 ), 1.0 );\n";
	}
	// OpenGL clamps vertex colours to [0,1] before they are interpolated, whether they come
	// from the fixed-function pipeline or a vertex program (materials do use colours above 1)
	if ( fsReads & MV_COLOR0 ) body += "\to.col0 = saturate( o_col0 );\n";
	if ( fsReads & MV_COLOR1 ) body += "\to.col1 = saturate( o_col1 );\n";
	if ( fsReads & MV_FOG ) body += "\to.fog = o_fog;\n";
	for ( int i = 0; i < 8; i++ ) {
		if ( fsReads & ( MV_TEXCOORD0 << i ) ) body += "\to.tc" + Num( i ) + " = o_tc" + Num( i ) + ";\n";
	}
	body += "\treturn o;\n";

	msl += "struct V2F {\n\tfloat4 position [[position, invariant]];\n";
	if ( key.shadow ) msl += "\tfloat4 shw;\n";
	if ( fsReads & MV_COLOR0 ) msl += "\tfloat4 col0;\n";
	if ( fsReads & MV_COLOR1 ) msl += "\tfloat4 col1;\n";
	if ( fsReads & MV_FOG ) msl += "\tfloat4 fog;\n";
	for ( int i = 0; i < 8; i++ ) {
		if ( fsReads & ( MV_TEXCOORD0 << i ) ) msl += "\tfloat4 tc" + Num( i ) + ";\n";
	}
	msl += "};\n";
	msl += std::string( "vertex V2F mgl_vs( " ) + ( b.pair.attribMask ? "VIn in [[stage_in]], " : "" ) + "constant U &u [[buffer(30)]] ) {\n" + body + "}\n";
}

std::string CombineArg( Builder &b, int unit, int src, int op, bool alpha ) {
	std::string x;
	switch ( src ) {
	case MS_TEXTURE: x = "tx"; break;
	case MS_CONSTANT: x = b.U( MU_ENV_COLOR, unit, 0 ); break;
	case MS_PRIMARY: x = "f_col0"; break;
	default: x = "c"; break;
	}
	if ( alpha ) {
		return ( op == MO_ONE_MINUS_SRC_ALPHA || op == MO_ONE_MINUS_SRC_COLOR ) ? "( 1.0 - " + x + ".a )" : x + ".a";
	}
	switch ( op ) {
	case MO_SRC_COLOR: return x + ".rgb";
	case MO_ONE_MINUS_SRC_COLOR: return "( 1.0 - " + x + ".rgb )";
	case MO_SRC_ALPHA: return "float3( " + x + ".a )";
	default: return "float3( 1.0 - " + x + ".a )";
	}
}

std::string CombineFunc( int func, const std::string a[3], bool alpha ) {
	switch ( func ) {
	case MC_REPLACE: return a[0];
	case MC_MODULATE: return a[0] + " * " + a[1];
	case MC_ADD: return a[0] + " + " + a[1];
	case MC_ADD_SIGNED: return a[0] + " + " + a[1] + " - 0.5";
	case MC_INTERPOLATE: return a[0] + " * " + a[2] + " + " + a[1] + " * ( 1.0 - " + a[2] + " )";
	case MC_SUBTRACT: return a[0] + " - " + a[1];
	default:
		if ( alpha ) return a[0] + " * " + a[1];
		return "float3( 4.0 * dot( " + a[0] + " - 0.5, " + a[1] + " - 0.5 ) )";
	}
}

// The texture environment functions of the fixed-function pipeline (OpenGL 1.3, table 3.22):
// what a texture contributes depends on its base format.
void FixedFunctionUnit( const MglFFUnit &u, int i, Builder &b, std::string &body ) {
	const std::string t = "t" + Num( i ), s = "s" + Num( i ), tc = "f_tc" + Num( i );
	std::string sample;
	if ( u.target == MT_CUBE ) sample = t + ".sample( " + s + ", " + tc + ".xyz )";
	else if ( u.target == MT_3D ) sample = t + ".sample( " + s + ", " + tc + ".xyz / " + tc + ".w )";
	else sample = t + ".sample( " + s + ", " + tc + ".xy / " + tc + ".w )";
	body += "\t{\n\t\tfloat4 tx = " + sample + ";\n";

	const int f = u.formatClass;
	const bool hasColor = ( f != MF_ALPHA );
	const bool hasAlpha = ( f == MF_RGBA || f == MF_ALPHA || f == MF_LUMINANCE_ALPHA || f == MF_INTENSITY );
	switch ( u.envMode ) {
	case ME_REPLACE:
		if ( hasColor ) body += "\t\tc.rgb = tx.rgb;\n";
		if ( hasAlpha ) body += "\t\tc.a = tx.a;\n";
		break;
	case ME_DECAL:
		if ( f == MF_RGB ) body += "\t\tc.rgb = tx.rgb;\n";
		else if ( f == MF_RGBA ) body += "\t\tc.rgb = mix( c.rgb, tx.rgb, tx.a );\n";
		break;
	case ME_ADD:
		if ( hasColor ) body += "\t\tc.rgb = c.rgb + tx.rgb;\n";
		if ( f == MF_INTENSITY ) body += "\t\tc.a = c.a + tx.a;\n";
		else if ( hasAlpha ) body += "\t\tc.a = c.a * tx.a;\n";
		break;
	case ME_BLEND: {
		const std::string cc = b.U( MU_ENV_COLOR, i, 0 );
		if ( hasColor ) body += "\t\tc.rgb = mix( c.rgb, " + cc + ".rgb, tx.rgb );\n";
		if ( f == MF_INTENSITY ) body += "\t\tc.a = mix( c.a, " + cc + ".a, tx.a );\n";
		else if ( hasAlpha ) body += "\t\tc.a = c.a * tx.a;\n";
		break;
	}
	case ME_COMBINE: {
		std::string rgb[3], a[3];
		for ( int k = 0; k < 3; k++ ) {
			rgb[k] = CombineArg( b, i, u.srcRGB[k], u.opRGB[k], false );
			a[k] = CombineArg( b, i, u.srcA[k], u.opA[k], true );
		}
		body += "\t\tfloat3 crgb = ( " + CombineFunc( u.combineRGB, rgb, false ) + " ) * " + Num( u.rgbScale ? u.rgbScale : 1 ) + ".0;\n";
		if ( u.combineRGB == MC_DOT3_RGBA ) {
			body += "\t\tfloat ca = crgb.r;\n";
		} else {
			body += "\t\tfloat ca = ( " + CombineFunc( u.combineA, a, true ) + " ) * " + Num( u.alphaScale ? u.alphaScale : 1 ) + ".0;\n";
		}
		body += "\t\tc = float4( crgb, ca );\n";
		break;
	}
	default:	// ME_MODULATE
		if ( hasColor ) body += "\t\tc.rgb = c.rgb * tx.rgb;\n";
		if ( hasAlpha ) body += "\t\tc.a = c.a * tx.a;\n";
		break;
	}
	body += "\t\tc = saturate( c );\n\t}\n";
}

void FragmentStage( const MglPairKey &key, Builder &b, uint32_t fsReads, std::string &msl ) {
	std::string body;
	body += "\tfloat4 f_pos = in.position;\n";
	body += std::string( "\tfloat4 f_col0 = " ) + ( ( fsReads & MV_COLOR0 ) ? "in.col0" : "float4(1.0)" ) + ";\n";
	body += std::string( "\tfloat4 f_col1 = " ) + ( ( fsReads & MV_COLOR1 ) ? "in.col1" : "float4(0.0, 0.0, 0.0, 1.0)" ) + ";\n";
	body += std::string( "\tfloat4 f_fog = " ) + ( ( fsReads & MV_FOG ) ? "in.fog" : "float4(0.0, 0.0, 0.0, 1.0)" ) + ";\n";
	for ( int i = 0; i < 8; i++ ) {
		if ( fsReads & ( MV_TEXCOORD0 << i ) ) {
			body += "\tfloat4 f_tc" + Num( i ) + " = in.tc" + Num( i ) + ";\n";
		}
	}
	body += "\tfloat4 r_col = float4(0.0, 0.0, 0.0, 1.0);\n\tfloat4 r_depth = f_pos;\n";
	body += "\t(void)f_pos; (void)f_col0; (void)f_col1; (void)f_fog;\n";

	bool writesDepth = false;
	if ( key.fp ) {
		body += b.Resolve( *key.fp );
		b.pair.texMask = key.fp->texMask;
		for ( int i = 0; i < 16; i++ ) {
			b.pair.texTarget[i] = key.fp->texTarget[i];
		}
		writesDepth = key.fp->writesDepth;
	} else {
		body += "\tfloat4 c = f_col0;\n";
		for ( int i = 0; i < 8; i++ ) {
			const MglFFUnit &u = key.unit[i];
			if ( u.target == MT_NONE ) {
				continue;
			}
			b.pair.texMask |= 1u << i;
			b.pair.texTarget[i] = u.target;
			FixedFunctionUnit( u, i, b, body );
		}
		body += "\tr_col = c;\n";
	}

	if ( key.shadow ) {
		// the slots are requested in order and no other stage uses MU_SHADOW, so they are adjacent
		std::string first;
		for ( int i = 0; i < MGL_SHADOW_SLOTS; i++ ) {
			const std::string slot = b.U( MU_SHADOW, 0, i );
			if ( i == 0 ) first = slot;
		}
		body += "\tif ( ( &" + first + " )[4].w > 1.5 ) r_col = mgl_shadow_debug( in.shw, &" + first + ", mgl_sm, mgl_ss );\n"
				"\telse r_col.rgb *= mgl_shadow( in.shw, &" + first + ", mgl_sm, mgl_ss );\n";
	}

	if ( key.alphaFunc != MA_OFF && key.alphaFunc != MA_ALWAYS ) {
		static const char *ops[] = { "", "", "<", "==", "<=", ">", "!=", ">=", "" };
		if ( key.alphaFunc == MA_NEVER ) {
			body += "\tdiscard_fragment();\n";
		} else {
			body += std::string( "\tif ( !( r_col.a " ) + ops[key.alphaFunc] + " " + b.U( MU_ALPHA_REF, 0, 0 ) + ".x ) ) discard_fragment();\n";
		}
	}
	body += "\tFOut o;\n\to.color = r_col;\n";
	if ( writesDepth ) {
		body += "\to.depth = r_depth.z;\n";
	}
	body += "\treturn o;\n";

	msl += std::string( "struct FOut {\n\tfloat4 color [[color(0)]];\n" ) + ( writesDepth ? "\tfloat depth [[depth(any)]];\n" : "" ) + "};\n";
	msl += "fragment FOut mgl_fs( V2F in [[stage_in]], constant U &u [[buffer(0)]]";
	for ( int i = 0; i < 16; i++ ) {
		if ( !( b.pair.texMask & ( 1u << i ) ) ) {
			continue;
		}
		const char *type = "texture2d<float>";
		if ( b.pair.texTarget[i] == MT_CUBE ) type = "texturecube<float>";
		else if ( b.pair.texTarget[i] == MT_3D ) type = "texture3d<float>";
		msl += std::string( ", " ) + type + " t" + Num( i ) + " [[texture(" + Num( i ) + ")]], sampler s" + Num( i ) + " [[sampler(" + Num( i ) + ")]]";
	}
	if ( key.shadow ) {
		msl += ", depth2d_array<float> mgl_sm [[texture(" + Num( MGL_SHADOW_TEXTURE_UNIT ) + ")]], sampler mgl_ss [[sampler(" + Num( MGL_SHADOW_TEXTURE_UNIT ) + ")]]";
	}
	msl += " ) {\n" + body + "}\n";
}

} // namespace

// How much of the light reaches a point (0..1), from the shadow map of the light
// (draw_shadowmap.cpp). `e` is the eye space position, `sv` the MGL_SHADOW_* slots.
// A point light has six layers, one per axis direction; other lights one.
static const char *kShadowMSL =
	"static inline float mgl_shadow( float4 e, constant float4 *sv, depth2d_array<float> sm, sampler ss ) {\n"
	"\tfloat4 w = float4( dot( sv[0], e ), dot( sv[1], e ), dot( sv[2], e ), 1.0 );\n"
	"\tuint layer = 0;\n"
	"\tif ( sv[3].x > 1.5 ) {\n"
	"\t\tfloat3 d = w.xyz - sv[4].xyz;\n"
	"\t\tfloat3 a = abs( d );\n"
	"\t\tif ( a.x >= a.y && a.x >= a.z ) layer = d.x >= 0.0 ? 0 : 1;\n"
	"\t\telse if ( a.y >= a.z ) layer = d.y >= 0.0 ? 2 : 3;\n"
	"\t\telse layer = d.z >= 0.0 ? 4 : 5;\n"
	"\t}\n"
	// As with stencil shadows, whose caps are the triangles that face away from the light, a
	// surface facing away from the light is in shadow, whatever its normal map says. The flat
	// normal comes from the position's derivatives, turned towards the viewer (sv[0..2].w is
	// the eye's position).
	"\tfloat3 n = cross( dfdx( w.xyz ), dfdy( w.xyz ) );\n"
	"\tif ( dot( n, float3( sv[0].w, sv[1].w, sv[2].w ) - w.xyz ) < 0.0 ) n = -n;\n"
	"\tif ( sv[5].x > 0.5 && dot( n, sv[4].xyz - w.xyz ) <= 0.0 ) return 0.0;\n"
	"\tconstant float4 *m = sv + 6 + 4 * layer;\n"
	"\tfloat4 c = float4( dot( m[0], w ), dot( m[1], w ), dot( m[2], w ), dot( m[3], w ) );\n"
	"\tif ( c.w <= 0.0 ) return 1.0;\n"
	"\tfloat3 p = c.xyz / c.w;\n"
	"\tif ( abs( p.x ) > 1.0 || abs( p.y ) > 1.0 || p.z > 1.0 ) return 1.0;\n"
	// the shadow pass's vertex functions negate y and map z from [-1,1] to [0,1] as every other
	// pass does, so OpenGL's y = 1 is the last row of the map and z/w = 1 the depth 1
	"\tfloat2 uv = p.xy * 0.5 + 0.5;\n"
	"\tfloat ref = p.z * 0.5 + 0.5 - sv[3].w;\n"
	"\tfloat r = sv[3].y * sv[3].z;\n"
	"\tif ( r <= 0.0 ) return sm.sample_compare( ss, uv, layer, ref );\n"
	// 3x3 comparisons with bilinear filtering: a smooth edge about 2r wide
	"\tfloat lit = 0.0;\n"
	"\tfor ( int y = -1; y <= 1; y++ ) {\n"
	"\t\tfor ( int x = -1; x <= 1; x++ ) {\n"
	"\t\t\tlit += sm.sample_compare( ss, uv + float2( x, y ) * r, layer, ref );\n"
	"\t\t}\n"
	"\t}\n"
	"\treturn lit * ( 1.0 / 9.0 );\n"
	"}\n"
	// r_shadowMapDebug 2: the shadow factor, 3: map coordinates and reference depth, 4: map depth
	"static inline float4 mgl_shadow_debug( float4 e, constant float4 *sv, depth2d_array<float> sm, sampler ss ) {\n"
	"\tif ( sv[4].w < 2.5 ) { float s = mgl_shadow( e, sv, sm, ss ); return float4( s, s, s, 1.0 ) * 0.25; }\n"
	"\tfloat4 w = float4( dot( sv[0], e ), dot( sv[1], e ), dot( sv[2], e ), 1.0 );\n"
	"\tuint layer = 0;\n"
	"\tif ( sv[3].x > 1.5 ) {\n"
	"\t\tfloat3 d = w.xyz - sv[4].xyz; float3 a = abs( d );\n"
	"\t\tif ( a.x >= a.y && a.x >= a.z ) layer = d.x >= 0.0 ? 0 : 1;\n"
	"\t\telse if ( a.y >= a.z ) layer = d.y >= 0.0 ? 2 : 3;\n"
	"\t\telse layer = d.z >= 0.0 ? 4 : 5;\n"
	"\t}\n"
	"\tconstant float4 *m = sv + 6 + 4 * layer;\n"
	"\tfloat4 c = float4( dot( m[0], w ), dot( m[1], w ), dot( m[2], w ), dot( m[3], w ) );\n"
	"\tfloat3 p = c.xyz / c.w;\n"
	"\tfloat2 uv = p.xy * 0.5 + 0.5;\n"
	"\tif ( sv[4].w < 3.5 ) return float4( uv, p.z * 0.5 + 0.5, 1.0 ) * 0.25;\n"
	"\tuint2 t = uint2( saturate( uv ) * float2( sm.get_width() - 1, sm.get_height() - 1 ) );\n"
	"\tfloat z = sm.read( t, layer );\n"
	"\treturn float4( z, fract( z * 64.0 ), fract( z * 4096.0 ), 1.0 );\n"
	"}\n";

void MglShader_Generate( const MglPairKey &key, MglPair &out, std::string &msl ) {
	out.programId = -1;
	out.uniforms.clear();
	out.attribMask = 0;
	out.texMask = 0;
	memset( out.texTarget, 0, sizeof( out.texTarget ) );
	out.shadow = key.shadow;
	out.name = std::string( key.shadow ? "sh_" : "" ) + ( key.vp ? "vp" + Num( (int)key.vpId ) : std::string( "ff" ) ) + "+" + ( key.fp ? "fp" + Num( (int)key.fpId ) : std::string( "ff" ) );

	Builder b( out );
	const uint32_t fsReads = MglShader_FragmentVaryings( key );
	std::string vs, fs;
	VertexStage( key, b, fsReads, vs );
	FragmentStage( key, b, fsReads, fs );

	msl = "#include <metal_stdlib>\nusing namespace metal;\n"
		  "static inline float4 mgl_lit( float4 v ) {\n"
		  "\tfloat d = max( v.x, 0.0 );\n"
		  "\tfloat s = ( v.x > 0.0 ) ? pow( max( v.y, 0.0 ), clamp( v.w, -128.0, 128.0 ) ) : 0.0;\n"
		  "\treturn float4( 1.0, d, s, 1.0 );\n}\n";
	if ( key.shadow ) {
		msl += kShadowMSL;
	}
	msl += "struct U { float4 v[" + Num( out.uniforms.empty() ? 1 : (int)out.uniforms.size() ) + "]; };\n";
	msl += vs;
	msl += fs;
}
