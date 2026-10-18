/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

/*
Shadow maps (r_shadowMaps, Metal backend only)

Instead of counting shadow volumes in the stencil buffer, the shadow casters of a light are
drawn into a depth map seen from the light, and the light's interactions are multiplied by
how much of the light the map lets through. The cost of a shadow no longer grows with the
screen area its volume covers, or with the number of samples per pixel.

The casters are the shadow volumes the front end makes anyway: their caps are the triangles
of the casters that face away from the light (the precomputed volumes of the world have the
nearest such triangle), and shadow.vp sends the extruded vertices to infinity, which seen
from the light is the far plane. So the map holds, for every direction from the light, the
nearest back face of a caster, and lit faces do not shadow themselves.

A light whose origin is inside its volume (point lights) gets six maps, one per axis
direction; any other (projected lights, and parallel lights, which are point lights 100000
units away) one map that covers the volume.

As with stencil shadows, surfaces with a noselfshadow material are shadowed by the other
casters only: they get a map without the casters of their group (localShadows).
*/

#include "sys/platform.h"
#include "renderer/VertexCache.h"

#include "renderer/tr_local.h"

// the parameters of the lookup, as renderer/metal/mgl_local.h has them (MGL_SHADOW_*)
enum { MGL_SHADOW_EYE_TO_WORLD = 0, MGL_SHADOW_PARAMS = 3, MGL_SHADOW_ORIGIN = 4, MGL_SHADOW_FLAGS = 5,
	   MGL_SHADOW_LAYER0 = 6, MGL_SHADOW_SLOTS = 6 + 4 * 6 };

idCVar r_shadowMaps( "r_shadowMaps", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
	"Metal backend: shadow maps instead of stencil shadow volumes" );
idCVar r_shadowMapSize( "r_shadowMapSize", "1024", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER,
	"size of the shadow map of a light that covers the screen (point lights: of each of its six maps)", 128, 4096 );
idCVar r_shadowMapSoftness( "r_shadowMapSoftness", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
	"shadow edge filter radius in shadow map texels, 0 = hard edges", 0.0f, 8.0f );
idCVar r_shadowMapBias( "r_shadowMapBias", "0", CVAR_RENDERER | CVAR_FLOAT,
	"depth bias of the shadow map lookup" );
idCVar r_shadowMapPolygonFactor( "r_shadowMapPolygonFactor", "0", CVAR_RENDERER | CVAR_FLOAT,
	"slope-scaled depth bias when drawing shadow maps" );
idCVar r_shadowMapDebug( "r_shadowMapDebug", "0", CVAR_RENDERER | CVAR_INTEGER,
	"1 = print the shadow map setup of each light, 2 = show how much light the maps let through, 3 = map coordinates, 4 = map depth" );
idCVar r_shadowMapPolygonOffset( "r_shadowMapPolygonOffset", "0", CVAR_RENDERER | CVAR_FLOAT,
	"constant depth bias when drawing shadow maps" );

bool RB_ShadowMapsActive( void ) {
	return r_shadowMaps.GetBool() && r_shadows.GetBool() && GLimp_HasShadowMaps();
}

bool R_UseOptimizedShadows( void ) {
	return r_useOptimizedShadows.GetBool() && !( r_shadowMaps.GetBool() && GLimp_HasShadowMaps() );
}

/*
==================
R_ShadowMapsFrame

Interactions made for one kind of shadows have the wrong shadow volumes for the other
(R_UseOptimizedShadows): when r_shadowMaps changes, they are all made again.
==================
*/
void R_ShadowMapsFrame( void ) {
	if ( r_shadowMaps.IsModified() ) {
		r_shadowMaps.ClearModified();
		if ( GLimp_HasShadowMaps() ) {
			R_FreeDerivedData();
			R_ReCreateWorldReferences();
		}
	}
}

// column-major 4x4 (OpenGL layout): out = a * b
static void SM_Mul( const float a[16], const float b[16], float out[16] ) {
	for ( int c = 0; c < 4; c++ ) {
		for ( int r = 0; r < 4; r++ ) {
			out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1]
						   + a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
		}
	}
}

// world -> eye of a camera at `origin` looking along `fwd` (OpenGL: the eye looks down -z)
static void SM_View( const idVec3 &origin, const idVec3 &fwd, const idVec3 &upHint, float out[16] ) {
	idVec3 right = fwd.Cross( upHint );
	right.Normalize();
	const idVec3 up = right.Cross( fwd );
	const idVec3 axes[3] = { right, up, -fwd };
	for ( int r = 0; r < 3; r++ ) {
		out[0 * 4 + r] = axes[r].x;
		out[1 * 4 + r] = axes[r].y;
		out[2 * 4 + r] = axes[r].z;
		out[3 * 4 + r] = -( axes[r] * origin );
	}
	out[3] = out[7] = out[11] = 0.0f;
	out[15] = 1.0f;
}

static void SM_Perspective( float tanX, float tanY, float zNear, float zFar, float out[16] ) {
	memset( out, 0, 16 * sizeof( float ) );
	out[0] = 1.0f / tanX;
	out[5] = 1.0f / tanY;
	out[10] = -( zFar + zNear ) / ( zFar - zNear );
	out[11] = -1.0f;
	out[14] = -2.0f * zFar * zNear / ( zFar - zNear );
}

struct smLight_t {
	int		layers;			// 1, or 6 for a point light
	int		size;
	float	view[6][16];	// world -> eye
	float	proj[6][16];
	float	clip[6][16];	// world -> clip
};

// The projection(s) that cover the light's volume as seen from its origin.
static bool SM_SetupLight( const viewLight_t *vLight, smLight_t &sl ) {
	const idRenderLightLocal *light = vLight->lightDef;
	const srfTriangles_t *tri = light->frustumTris;
	if ( !tri || tri->numVerts <= 0 ) {
		return false;
	}
	const idVec3 &origin = vLight->globalLightOrigin;
	const float softness = r_shadowMapSoftness.GetFloat();

	// a smaller map for a light that covers less of the screen
	const idScreenRect &sr = vLight->scissorRect;
	const idScreenRect &vr = backEnd.viewDef->viewport;
	const float cover = Max( ( sr.x2 - sr.x1 + 1 ) / (float)Max( 1, vr.x2 - vr.x1 + 1 ),
							 ( sr.y2 - sr.y1 + 1 ) / (float)Max( 1, vr.y2 - vr.y1 + 1 ) );
	int size = idMath::CeilPowerOfTwo( idMath::ClampInt( 128, 4096, r_shadowMapSize.GetInteger() ) );
	if ( cover < 0.25f ) {
		size /= 4;
	} else if ( cover < 0.5f ) {
		size /= 2;
	}

	idBounds bounds = tri->bounds;
	bounds.ExpandSelf( 1.0f );
	if ( bounds.ContainsPoint( origin ) ) {
		// six 90 degree views, a little wider so the filter does not reach past an edge
		size = Max( 64, size / 2 );
		sl.layers = 6;
		sl.size = size;
		float zFar = 1.0f;
		for ( int i = 0; i < 8; i++ ) {
			const idVec3 corner( bounds[( i >> 0 ) & 1][0], bounds[( i >> 1 ) & 1][1], bounds[( i >> 2 ) & 1][2] );
			zFar = Max( zFar, ( corner - origin ).Length() );
		}
		const float zNear = Max( 1.0f, zFar / 8192.0f );
		const float t = 1.0f + 2.0f * ( softness + 2.0f ) / size;
		static const idVec3 fwd[6] = { idVec3( 1, 0, 0 ), idVec3( -1, 0, 0 ), idVec3( 0, 1, 0 ),
									   idVec3( 0, -1, 0 ), idVec3( 0, 0, 1 ), idVec3( 0, 0, -1 ) };
		static const idVec3 up[6] = { idVec3( 0, 0, 1 ), idVec3( 0, 0, 1 ), idVec3( 0, 0, 1 ),
									  idVec3( 0, 0, 1 ), idVec3( 1, 0, 0 ), idVec3( 1, 0, 0 ) };
		for ( int f = 0; f < 6; f++ ) {
			SM_View( origin, fwd[f], up[f], sl.view[f] );
			SM_Perspective( t, t, zNear, zFar, sl.proj[f] );
			SM_Mul( sl.proj[f], sl.view[f], sl.clip[f] );
		}
		return true;
	}

	// one view from the origin towards the volume, just wide enough for its corners
	idVec3 fwd = bounds.GetCenter() - origin;
	fwd.Normalize();
	const idVec3 upHint = ( idMath::Fabs( fwd.z ) < 0.9f ) ? idVec3( 0, 0, 1 ) : idVec3( 1, 0, 0 );
	SM_View( origin, fwd, upHint, sl.view[0] );
	float tanX = 0.0f, tanY = 0.0f, zMin = idMath::INFINITY, zMax = 0.0f;
	for ( int i = 0; i < tri->numVerts; i++ ) {
		const idVec3 &p = tri->verts[i].xyz;
		const float *v = sl.view[0];
		const float ex = v[0] * p.x + v[4] * p.y + v[8] * p.z + v[12];
		const float ey = v[1] * p.x + v[5] * p.y + v[9] * p.z + v[13];
		const float ez = -( v[2] * p.x + v[6] * p.y + v[10] * p.z + v[14] );
		if ( ez < 1.0f ) {
			return false;	// the volume reaches behind the origin
		}
		tanX = Max( tanX, idMath::Fabs( ex / ez ) );
		tanY = Max( tanY, idMath::Fabs( ey / ez ) );
		zMin = Min( zMin, ez );
		zMax = Max( zMax, ez );
	}
	const float pad = 1.0f + 2.0f * ( softness + 2.0f ) / size;
	sl.layers = 1;
	sl.size = size;
	// casters between the origin and the volume shadow it too
	SM_Perspective( Max( tanX, 1e-4f ) * pad, Max( tanY, 1e-4f ) * pad, Max( 1.0f, zMin * 0.25f ), zMax * 1.01f, sl.proj[0] );
	SM_Mul( sl.proj[0], sl.view[0], sl.clip[0] );
	return true;
}

// True when no part of `bounds` (in the space of `model`) is inside the view of `clip`.
static bool SM_Culled( const idBounds &bounds, const float model[16], const float clip[16] ) {
	float mvp[16];
	SM_Mul( clip, model, mvp );
	int outside[6] = { 0, 0, 0, 0, 0, 0 };
	for ( int i = 0; i < 8; i++ ) {
		const float x = bounds[( i >> 0 ) & 1][0], y = bounds[( i >> 1 ) & 1][1], z = bounds[( i >> 2 ) & 1][2];
		float c[4];
		for ( int r = 0; r < 4; r++ ) {
			c[r] = mvp[r] * x + mvp[4 + r] * y + mvp[8 + r] * z + mvp[12 + r];
		}
		outside[0] += c[0] < -c[3];
		outside[1] += c[0] > c[3];
		outside[2] += c[1] < -c[3];
		outside[3] += c[1] > c[3];
		outside[4] += c[2] < -c[3];
		outside[5] += c[3] <= 0.0f;	// behind the light
	}
	for ( int p = 0; p < 6; p++ ) {
		if ( outside[p] == 8 ) {
			return true;
		}
	}
	return false;
}

static void SM_DrawCasters( const drawSurf_t *surfs, const float view[16], const float clip[16] ) {
	for ( const drawSurf_t *surf = surfs; surf; surf = surf->nextOnLight ) {
		const srfTriangles_t *tri = surf->geo;
		if ( !tri->shadowCache ) {
			continue;
		}
		// the shadow volume's bounds are those of the caster: its far end is at infinity
		if ( SM_Culled( tri->bounds, surf->space->modelMatrix, clip ) ) {
			continue;
		}
		float modelView[16];
		SM_Mul( view, surf->space->modelMatrix, modelView );
		qglLoadMatrixf( modelView );

		// shadow.vp extrudes the vertexes with w = 0 away from the light, to infinity
		idVec4 localLight;
		R_GlobalPointToLocal( surf->space->modelMatrix, backEnd.vLight->globalLightOrigin, localLight.ToVec3() );
		localLight.w = 0.0f;
		qglProgramEnvParameter4fvARB( GL_VERTEX_PROGRAM_ARB, PP_LIGHT_ORIGIN, localLight.ToFloatPtr() );

		qglVertexPointer( 4, GL_FLOAT, sizeof( shadowCache_t ), vertexCache.Position( tri->shadowCache ) );
		// all of it: the caps are what the map needs
		RB_DrawShadowElementsWithCounters( tri, tri->numIndexes );
	}
}

// A map with the casters of both lists, or -1.
static int SM_DrawMap( const smLight_t &sl, const drawSurf_t *a, const drawSurf_t *b ) {
	const int map = GLimp_ShadowMapAcquire( sl.size, sl.layers );
	if ( map < 0 ) {
		return -1;
	}
	for ( int layer = 0; layer < sl.layers; layer++ ) {
		GLimp_ShadowMapBeginLayer( map, layer );
		qglMatrixMode( GL_PROJECTION );
		qglLoadMatrixf( sl.proj[layer] );
		qglMatrixMode( GL_MODELVIEW );
		SM_DrawCasters( a, sl.view[layer], sl.clip[layer] );
		SM_DrawCasters( b, sl.view[layer], sl.clip[layer] );
		GLimp_ShadowMapEndLayer();
	}
	return map;
}

/*
==================
RB_ShadowMapDrawInteractions

Draws the interactions of backEnd.vLight with shadow maps instead of stencil shadows, using
`drawInteractions` (RB_ARB2_CreateDrawInteractions) for each group. Returns false, having
drawn nothing, when the light cannot have shadow maps: the caller then uses stencil shadows.
==================
*/
bool RB_ShadowMapDrawInteractions( void ( *drawInteractions )( const drawSurf_t *surf ) ) {
	const viewLight_t *vLight = backEnd.vLight;
	smLight_t sl;
	if ( !SM_SetupLight( vLight, sl ) ) {
		return false;
	}

	// what the stencil shadow pass sets up
	globalImages->BindNull();
	qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
	qglEnable( GL_VERTEX_PROGRAM_ARB );
	qglBindProgramARB( GL_VERTEX_PROGRAM_ARB, VPROG_STENCIL_SHADOW );
	GL_State( GLS_DEPTHFUNC_LESS | GLS_COLORMASK | GLS_ALPHAMASK );
	GL_Cull( CT_TWO_SIDED );
	qglDisable( GL_STENCIL_TEST );
	qglPolygonOffset( r_shadowMapPolygonFactor.GetFloat(), r_shadowMapPolygonOffset.GetFloat() );
	qglEnable( GL_POLYGON_OFFSET_FILL );
	// the viewport and scissor of a map are set by GLimp_ShadowMapBeginLayer

	// global interactions are shadowed by every caster, noselfshadow ones only by the others
	int mapAll = -1, mapGlobal = -1;
	if ( vLight->globalInteractions ) {
		mapAll = SM_DrawMap( sl, vLight->globalShadows, vLight->localShadows );
	}
	if ( vLight->localInteractions && vLight->globalShadows ) {
		mapGlobal = vLight->localShadows ? SM_DrawMap( sl, vLight->globalShadows, NULL ) : mapAll;
		if ( mapGlobal < 0 ) {
			mapGlobal = SM_DrawMap( sl, vLight->globalShadows, NULL );
		}
	}

	// back to the view
	qglDisable( GL_POLYGON_OFFSET_FILL );
	GL_Cull( CT_FRONT_SIDED );
	qglEnable( GL_STENCIL_TEST );
	qglStencilFunc( GL_ALWAYS, 128, 255 );
	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	qglDisable( GL_VERTEX_PROGRAM_ARB );
	qglMatrixMode( GL_PROJECTION );
	qglLoadMatrixf( backEnd.viewDef->projectionMatrix );
	qglMatrixMode( GL_MODELVIEW );
	backEnd.currentSpace = NULL;
	const viewDef_t *viewDef = backEnd.viewDef;

	// the lookup's parameters (MGL_SHADOW_*)
	float slots[MGL_SHADOW_SLOTS][4];
	memset( slots, 0, sizeof( slots ) );
	const float *w = viewDef->worldSpace.modelViewMatrix;	// world -> eye, orthonormal
	for ( int i = 0; i < 3; i++ ) {
		float *row = slots[MGL_SHADOW_EYE_TO_WORLD + i];
		row[0] = w[i * 4 + 0];
		row[1] = w[i * 4 + 1];
		row[2] = w[i * 4 + 2];
		row[3] = -( w[i * 4 + 0] * w[12] + w[i * 4 + 1] * w[13] + w[i * 4 + 2] * w[14] );
	}
	slots[MGL_SHADOW_PARAMS][0] = (float)sl.layers;
	slots[MGL_SHADOW_PARAMS][1] = r_shadowMapSoftness.GetFloat();
	slots[MGL_SHADOW_PARAMS][2] = 1.0f / sl.size;
	slots[MGL_SHADOW_PARAMS][3] = r_shadowMapBias.GetFloat();
	slots[MGL_SHADOW_ORIGIN][0] = vLight->globalLightOrigin.x;
	slots[MGL_SHADOW_ORIGIN][1] = vLight->globalLightOrigin.y;
	slots[MGL_SHADOW_ORIGIN][2] = vLight->globalLightOrigin.z;
	slots[MGL_SHADOW_ORIGIN][3] = (float)Max( 1, r_shadowMapDebug.GetInteger() );
	for ( int layer = 0; layer < sl.layers; layer++ ) {
		for ( int r = 0; r < 4; r++ ) {
			float *row = slots[MGL_SHADOW_LAYER0 + 4 * layer + r];
			for ( int c = 0; c < 4; c++ ) {
				row[c] = sl.clip[layer][c * 4 + r];
			}
		}
	}

	if ( r_shadowMapDebug.GetInteger() == 1 ) {
		common->Printf( "shadow map: light %p layers %d size %d origin (%s) shadows %s%s interactions %s%s maps %d %d\n",
			(void *)vLight->lightDef, sl.layers, sl.size, vLight->globalLightOrigin.ToString( 0 ),
			vLight->globalShadows ? "global " : "", vLight->localShadows ? "local" : "",
			vLight->globalInteractions ? "global " : "", vLight->localInteractions ? "local" : "", mapAll, mapGlobal );
		// where a point 64 units in front of the view lands in each layer
		const idVec3 p = viewDef->renderView.vieworg + viewDef->renderView.viewaxis[0] * 64.0f;
		for ( int layer = 0; layer < sl.layers; layer++ ) {
			float c[4];
			for ( int r = 0; r < 4; r++ ) {
				const float *row = slots[MGL_SHADOW_LAYER0 + 4 * layer + r];
				c[r] = row[0] * p.x + row[1] * p.y + row[2] * p.z + row[3];
			}
			common->Printf( "  layer %d: (%s) at %.3f %.3f depth %.5f\n", layer, p.ToString( 0 ), c[0] / c[3], c[1] / c[3], c[2] / c[3] );
		}
	}

	if ( vLight->localInteractions ) {
		// noselfshadow: not in shadow for facing away from the light either
		slots[MGL_SHADOW_FLAGS][0] = 0.0f;
		GLimp_ShadowSample( mapGlobal, &slots[0][0] );
		drawInteractions( vLight->localInteractions );
	}
	if ( vLight->globalInteractions ) {
		slots[MGL_SHADOW_FLAGS][0] = 1.0f;
		GLimp_ShadowSample( mapAll, &slots[0][0] );
		drawInteractions( vLight->globalInteractions );
	}
	GLimp_ShadowSample( -1, NULL );
	return true;
}
