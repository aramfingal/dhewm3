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

Frame interpolation.

The game runs at a fixed rate (60 tics per second) and tells the render world where
everything is once per tic. To draw more frames than that, a frame drawn between two tics
shows the world in between: entities, lights and the view are placed on the way from where
they were after the previous tic to where they are after the latest one, by the fraction
of a tic that has passed (alpha). What is on screen is therefore up to one tic old, and the
simulation itself is untouched.

Looking around must not feel a tic late, so when the player controls the view
(directLook) its direction is that of the latest tic plus the mouse movement since then,
and what is attached to the view (the first person weapon and its lights) follows it.

The session drives this (framework/Session.cpp): InterpBeginTic before every game tic,
InterpSetFrame before every frame is drawn.

*/

#include "sys/platform.h"
#include "framework/Session.h"

#include "idlib/math/Quat.h"
#include "idlib/geometry/JointTransform.h"
#include "renderer/RenderWorld_local.h"

#include "renderer/tr_local.h"

static ID_INLINE idVec3 Lerp( const idVec3 &from, const idVec3 &to, float alpha ) {
	return from + ( to - from ) * alpha;
}

idCVar r_interpolateLog( "r_interpolateLog", "0", CVAR_RENDERER | CVAR_BOOL,
	"frame interpolation: print time, alpha and the view position and direction of the first scene of every frame (tests)" );
// what the last frame interpolated, for r_interpolateLog
static int	c_interpEntities, c_interpLights, c_interpSkeletons;

idCVar r_interpolateMaxDistance( "r_interpolateMaxDistance", "128", CVAR_RENDERER | CVAR_FLOAT,
	"frame interpolation: something that moves farther than this in one game tic has been teleported and is not interpolated" );

/*
===================
R_InterpLerpAxis
===================
*/
static idMat3 R_InterpLerpAxis( const idMat3 &from, const idMat3 &to, float alpha ) {
	if ( from == to ) {
		return to;
	}
	idQuat q;
	q.Slerp( from.ToQuat(), to.ToQuat(), alpha );
	return q.ToMat3();
}

/*
===================
R_InterpSetEntityTransform

Moves an entity def without telling anything else about it apart.
===================
*/
static void R_InterpSetEntityTransform( idRenderEntityLocal *def, const idVec3 &origin, const idMat3 &axis ) {
	if ( def->parms.origin == origin && def->parms.axis == axis ) {
		return;
	}

	// a model that was instantiated for this pose (a skinned mesh) does not depend on
	// where the entity is: keep it, only the interactions and area references go
	idRenderModel *dynamicModel = def->dynamicModel;
	R_FreeEntityDefDerivedData( def, true, true );
	def->dynamicModel = dynamicModel;

	def->parms.origin = origin;
	def->parms.axis = axis;
	R_AxisToModelMatrix( def->parms.axis, def->parms.origin, def->modelMatrix );
	def->lastModifiedFrameNum = tr.frameCount;
	R_CreateEntityRefs( def );

	def->interp.applied = ( origin != def->interp.gameOrigin || axis != def->interp.gameAxis );
}

/*
===================
R_InterpSetLightTransform
===================
*/
static void R_InterpSetLightTransform( idRenderLightLocal *light, const idVec3 &origin, const idMat3 &axis ) {
	if ( light->parms.origin == origin && light->parms.axis == axis ) {
		return;
	}

	light->lightHasMoved = true;
	R_FreeLightDefDerivedData( light );

	light->parms.origin = origin;
	light->parms.axis = axis;
	light->parms.prelightModel = NULL;
	light->lastModifiedFrameNum = tr.frameCount;

	R_DeriveLightData( light );
	R_CreateLightRefs( light );
	R_CreateLightDefFogPortals( light );

	light->interp.applied = ( origin != light->interp.gameOrigin || axis != light->interp.gameAxis );
}

/*
===================
R_InterpJoints

Called when the dynamic model of an entity is needed. The game poses a skeleton once per
game tic (at the latest when the entity comes into view, through its callback), in memory
of its own. This keeps the pose of the latest tic and of the one before, and returns the
pose in between for this frame, or NULL when the game's pose is the one to use. `rebuild`
says that a model made earlier no longer shows the right pose.
===================
*/
const idJointMat *R_InterpJoints( idRenderEntityLocal *def, bool &rebuild ) {
	rebuild = false;

	const idRenderWorldLocal *world = def->world;
	const int numJoints = def->parms.numJoints;
	if ( !world || !def->parms.joints || numJoints <= 0 || session->readDemo ) {
		return NULL;
	}

	idJointMat *from = def->interpJoints;
	idJointMat *to = from + numJoints;
	idJointMat *out = to + numJoints;

	if ( def->interpJointsFrame == tr.frameCount && def->interpNumJoints == numJoints ) {
		// asked before in this frame (another view, a light): the same answer
		return def->interpJointsApplied ? out : NULL;
	}
	def->interpJointsFrame = tr.frameCount;

	if ( def->interpNumJoints != numJoints ) {
		if ( def->interpJoints ) {
			Mem_Free16( def->interpJoints );
		}
		def->interpJoints = (idJointMat *)Mem_Alloc16( 3 * numJoints * sizeof( idJointMat ) );
		def->interpNumJoints = numJoints;
		def->interpJointsTic = -1;
		def->interpJointsMoving = false;
		from = def->interpJoints;
		to = from + numJoints;
		out = to + numJoints;
	}

	const size_t bytes = numJoints * sizeof( idJointMat );
	const int tic = world->interp.ticCount;
	if ( def->interpJointsTic != tic ) {
		// the first time since a game tic: the game's pose is the pose of that tic
		const bool haveFrom = ( def->interpJointsTic == tic - 1 );
		if ( haveFrom ) {
			memcpy( from, to, bytes );
		}
		memcpy( to, def->parms.joints, bytes );
		def->interpJointsTic = tic;
		def->interpJointsMoving = haveFrom && memcmp( from, to, bytes ) != 0;
	}

	const bool wasApplied = def->interpJointsApplied;
	const float alpha = world->interp.alpha;
	if ( !world->interp.active || alpha >= 1.0f || !def->interpJointsMoving ) {
		def->interpJointsApplied = false;
		rebuild = wasApplied;	// back to the game's pose
		return NULL;
	}

	const float *a = from->ToFloatPtr();
	const float *b = to->ToFloatPtr();
	float *o = out->ToFloatPtr();
	const int count = numJoints * 12;
	for ( int i = 0; i < count; i++ ) {
		o[i] = a[i] + ( b[i] - a[i] ) * alpha;
	}
	def->interpJointsApplied = true;
	c_interpSkeletons++;
	rebuild = true;		// another pose in every frame
	return out;
}

/*
===================
R_InterpNote

The game sets a new transform: remember where it came from.
===================
*/
static void R_InterpNote( frameInterp_t &interp, const idVec3 &origin, const idMat3 &axis, int ticCount ) {
	if ( interp.valid && ( origin != interp.gameOrigin || axis != interp.gameAxis ) ) {
		if ( ( origin - interp.gameOrigin ).LengthSqr() > Square( r_interpolateMaxDistance.GetFloat() ) ) {
			// teleported
			interp.tic = -1;
		} else if ( interp.tic != ticCount ) {
			// the first move of this tic: where the game had it before is where it was
			// after the previous tic
			interp.fromOrigin = interp.gameOrigin;
			interp.fromAxis = interp.gameAxis;
			interp.tic = ticCount;
		}
	} else if ( !interp.valid ) {
		interp.tic = -1;
	}
	interp.gameOrigin = origin;
	interp.gameAxis = axis;
	interp.valid = true;
}

void idRenderWorldLocal::InterpNoteEntityUpdate( idRenderEntityLocal *def, const renderEntity_t *re ) {
	R_InterpNote( def->interp, re->origin, re->axis, interp.ticCount );
}

void idRenderWorldLocal::InterpNoteLightUpdate( idRenderLightLocal *light, const renderLight_t *rlight ) {
	R_InterpNote( light->interp, rlight->origin, rlight->axis, interp.ticCount );
}

/*
===================
idRenderWorldLocal::InterpBeginTic

A game tic is about to run.
===================
*/
void idRenderWorldLocal::InterpBeginTic() {
	// the views drawn after the tic that is ending are the views to come from
	if ( interp.viewsTic == interp.ticCount ) {
		interp.numPrevViews = interp.numViews;
		memcpy( interp.prevViews, interp.views, sizeof( interp.prevViews ) );
		interp.prevViewsTic = interp.viewsTic;
	}
	interp.ticCount++;
}

/*
===================
idRenderWorldLocal::InterpSetFrame

A frame is about to be drawn. alpha is the part of a tic that has passed since the latest
game tic; lookPitch / lookYaw is how far the input has turned since then.
===================
*/
void idRenderWorldLocal::InterpSetFrame( bool enabled, float alpha, bool directLook, float lookPitch, float lookYaw, float ticLookPitch, float ticLookYaw ) {
	// without a frame drawn after the previous tic there is nothing to come from for the
	// view (the frame rate is below the tic rate): show the latest tic as it is
	if ( !enabled || interp.prevViewsTic != interp.ticCount - 1 || alpha >= 1.0f ) {
		alpha = 1.0f;
	}
	if ( alpha < 0.0f ) {
		alpha = 0.0f;
	}
	interp.alpha = alpha;
	interp.directLook = enabled && directLook;
	interp.lookPitch = lookPitch;
	interp.lookYaw = lookYaw;
	interp.ticLookPitch = ticLookPitch;
	interp.ticLookYaw = ticLookYaw;
	interp.active = ( alpha < 1.0f ) || interp.directLook;
	interp.sceneIndex = 0;

	const bool lerp = ( alpha < 1.0f );

	for ( int i = 0; i < entityDefs.Num(); i++ ) {
		idRenderEntityLocal *def = entityDefs[i];
		if ( !def || !def->interp.valid ) {
			continue;
		}
		const bool moving = lerp && def->interp.tic == interp.ticCount;
		if ( !moving && !def->interp.applied ) {
			continue;
		}
		if ( interp.directLook && def->parms.allowSurfaceInViewID != 0 ) {
			// attached to a view: placed when that view is drawn
			continue;
		}
		if ( moving ) {
			c_interpEntities++;
			R_InterpSetEntityTransform( def, Lerp( def->interp.fromOrigin, def->interp.gameOrigin, alpha ),
										R_InterpLerpAxis( def->interp.fromAxis, def->interp.gameAxis, alpha ) );
		} else {
			R_InterpSetEntityTransform( def, def->interp.gameOrigin, def->interp.gameAxis );
		}
	}

	for ( int i = 0; i < lightDefs.Num(); i++ ) {
		idRenderLightLocal *light = lightDefs[i];
		if ( !light || !light->interp.valid ) {
			continue;
		}
		const bool moving = lerp && light->interp.tic == interp.ticCount;
		if ( !moving && !light->interp.applied ) {
			continue;
		}
		if ( interp.directLook && light->parms.allowLightInViewID != 0 ) {
			continue;
		}
		if ( moving ) {
			c_interpLights++;
			R_InterpSetLightTransform( light, Lerp( light->interp.fromOrigin, light->interp.gameOrigin, alpha ),
									   R_InterpLerpAxis( light->interp.fromAxis, light->interp.gameAxis, alpha ) );
		} else {
			R_InterpSetLightTransform( light, light->interp.gameOrigin, light->interp.gameAxis );
		}
	}
}

/*
===================
R_InterpAttached

Where something attached to the view is: its place relative to the view goes from what it
was after the previous tic to what it is after the latest one, and the view is where this
frame has it.
===================
*/
static void R_InterpAttached( const frameInterp_t &in, int ticCount, float alpha,
							  const renderView_t &prevView, const renderView_t &curView, const renderView_t &newView,
							  idVec3 &origin, idMat3 &axis ) {
	const bool moved = ( in.tic == ticCount );
	const idVec3 &fromOrigin = moved ? in.fromOrigin : in.gameOrigin;
	const idMat3 &fromAxis = moved ? in.fromAxis : in.gameAxis;

	const idMat3 prevInv = prevView.viewaxis.Transpose();
	const idMat3 curInv = curView.viewaxis.Transpose();

	const idVec3 relFrom = ( fromOrigin - prevView.vieworg ) * prevInv;
	const idVec3 relTo = ( in.gameOrigin - curView.vieworg ) * curInv;
	const idMat3 relAxisFrom = fromAxis * prevInv;
	const idMat3 relAxisTo = in.gameAxis * curInv;

	const idVec3 rel = Lerp( relFrom, relTo, alpha );
	const idMat3 relAxis = R_InterpLerpAxis( relAxisFrom, relAxisTo, alpha );

	origin = newView.vieworg + rel * newView.viewaxis;
	axis = relAxis * newView.viewaxis;
}

/*
===================
idRenderWorldLocal::InterpView

Called for every scene the game draws. Returns the view to draw it with.
===================
*/
const renderView_t *idRenderWorldLocal::InterpView( const renderView_t *gameView, renderView_t &storage ) {
	const int index = interp.sceneIndex++;

	// the first frame after a tic tells us the game's views of that tic
	if ( interp.viewsTic != interp.ticCount ) {
		interp.viewsTic = interp.ticCount;
		interp.numViews = 0;
	}
	if ( index == interp.numViews && index < MAX_INTERP_VIEWS ) {
		interp.views[interp.numViews++] = *gameView;
	}

	if ( !interp.active || index >= MAX_INTERP_VIEWS ) {
		return gameView;
	}

	// the same scene after the previous tic
	const renderView_t *prev = NULL;
	if ( index < interp.numPrevViews && interp.prevViewsTic == interp.ticCount - 1 ) {
		const renderView_t *candidate = &interp.prevViews[index];
		if ( candidate->viewID == gameView->viewID
			 && ( candidate->vieworg - gameView->vieworg ).LengthSqr() <= Square( r_interpolateMaxDistance.GetFloat() ) ) {
			prev = candidate;
		}
	}

	const float alpha = prev ? interp.alpha : 1.0f;
	if ( !prev ) {
		prev = gameView;
	}

	storage = *gameView;
	storage.vieworg = Lerp( prev->vieworg, gameView->vieworg, alpha );
	storage.time = prev->time + (int)( alpha * ( gameView->time - prev->time ) );
	// cinematic cameras zoom
	storage.fov_x = prev->fov_x + alpha * ( gameView->fov_x - prev->fov_x );
	storage.fov_y = prev->fov_y + alpha * ( gameView->fov_y - prev->fov_y );

	// only the player's own view (the one things are attached to) follows the input directly
	const bool direct = interp.directLook && gameView->viewID != 0;
	if ( direct ) {
		// The game's view direction is the input plus what the game adds: weapon kick, bob,
		// shake. The input is taken as it is now; what the game adds is interpolated
		// between the two tics like everything else, so it does not step once per tic.
		//   direction = input now + lerp( game - input at the previous tic, at the latest )
		idAngles angles = gameView->viewaxis.ToAngles();
		if ( alpha < 1.0f ) {
			const idAngles prevAngles = prev->viewaxis.ToAngles();
			idAngles added;		// ( game - input ) at the previous tic minus the same at the latest
			added.pitch = idMath::AngleNormalize180( prevAngles.pitch - angles.pitch + interp.ticLookPitch );
			added.yaw = idMath::AngleNormalize180( prevAngles.yaw - angles.yaw + interp.ticLookYaw );
			added.roll = idMath::AngleNormalize180( prevAngles.roll - angles.roll );
			// more than a kick: the game turned the view itself (a teleporter): no blending
			if ( idMath::Fabs( added.pitch ) < 15.0f && idMath::Fabs( added.yaw ) < 15.0f && idMath::Fabs( added.roll ) < 15.0f ) {
				angles += added * ( 1.0f - alpha );
			}
		}
		angles.yaw += interp.lookYaw;
		angles.pitch = idMath::ClampFloat( -89.0f, 89.0f, angles.pitch + interp.lookPitch );
		storage.viewaxis = angles.ToMat3();
	} else {
		storage.viewaxis = R_InterpLerpAxis( prev->viewaxis, gameView->viewaxis, alpha );
	}

	if ( direct ) {
		// what is only drawn in this view and is near it (the first person weapon, its muzzle
		// flash and flashlight) is attached to it. "Near" keeps a portal sky scene, which has
		// the player's view id but is drawn from somewhere else, from pulling the weapon along.
		const float maxDistSqr = Square( 256.0f );
		idVec3 origin;
		idMat3 axis;

		for ( int i = 0; i < entityDefs.Num(); i++ ) {
			idRenderEntityLocal *def = entityDefs[i];
			if ( !def || !def->interp.valid || def->parms.allowSurfaceInViewID != gameView->viewID ) {
				continue;
			}
			if ( ( def->interp.gameOrigin - gameView->vieworg ).LengthSqr() > maxDistSqr ) {
				if ( def->interp.applied ) {
					R_InterpSetEntityTransform( def, def->interp.gameOrigin, def->interp.gameAxis );
				}
				continue;
			}
			R_InterpAttached( def->interp, interp.ticCount, alpha, *prev, *gameView, storage, origin, axis );
			R_InterpSetEntityTransform( def, origin, axis );
		}

		for ( int i = 0; i < lightDefs.Num(); i++ ) {
			idRenderLightLocal *light = lightDefs[i];
			if ( !light || !light->interp.valid || light->parms.allowLightInViewID != gameView->viewID ) {
				continue;
			}
			if ( ( light->interp.gameOrigin - gameView->vieworg ).LengthSqr() > maxDistSqr ) {
				if ( light->interp.applied ) {
					R_InterpSetLightTransform( light, light->interp.gameOrigin, light->interp.gameAxis );
				}
				continue;
			}
			R_InterpAttached( light->interp, interp.ticCount, alpha, *prev, *gameView, storage, origin, axis );
			R_InterpSetLightTransform( light, origin, axis );
		}
	}

	if ( index == 0 && r_interpolateLog.GetBool() ) {
		// the counts of skeletons are those of the frame before: they are posed while it is drawn
		common->Printf( "interp: %.3f %.4f %d %.3f %.3f %.3f %.5f %.5f %.5f %d %d %d\n", Sys_MillisecondsPrecise(), alpha, interp.ticCount,
						storage.vieworg.x, storage.vieworg.y, storage.vieworg.z,
						storage.viewaxis[0].x, storage.viewaxis[0].y, storage.viewaxis[0].z,
						c_interpEntities, c_interpLights, c_interpSkeletons );
		c_interpEntities = c_interpLights = c_interpSkeletons = 0;
	}

	return &storage;
}

/*
===================
R_FrameInterpolation_BeginTic / R_FrameInterpolation_SetFrame

What the session calls.
===================
*/
void R_FrameInterpolation_BeginTic( idRenderWorld *world ) {
	if ( world ) {
		static_cast<idRenderWorldLocal *>( world )->InterpBeginTic();
	}
}

void R_FrameInterpolation_SetFrame( idRenderWorld *world, bool enabled, float alpha, bool directLook,
									float lookPitch, float lookYaw, float ticLookPitch, float ticLookYaw ) {
	if ( world ) {
		static_cast<idRenderWorldLocal *>( world )->InterpSetFrame( enabled, alpha, directLook, lookPitch, lookYaw, ticLookPitch, ticLookYaw );
	}
}
