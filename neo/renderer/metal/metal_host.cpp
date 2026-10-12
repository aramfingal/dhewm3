/*
metal_host.cpp - engine side of the Metal renderer: cvars and printing for the Metal files.
*/

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/CVarSystem.h"

#include "renderer/metal/metal_host.h"

idCVar r_backend( "r_backend", "opengl", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INIT,
	"rendering backend: opengl or metal (macOS). Read when the renderer starts: set it on the command line or use vid_restart" );
idCVar r_metalFrameStats( "r_metalFrameStats", "0", CVAR_RENDERER | CVAR_INTEGER,
	"every N frames, print where the frame time went (Metal backend)" );
idCVar r_metalNoPresent( "r_metalNoPresent", "0", CVAR_RENDERER | CVAR_BOOL,
	"render but never present to the window (benchmarks, Metal backend)" );
idCVar r_metalPresentHz( "r_metalPresentHz", "-1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER,
	"with r_swapInterval 0, presents per second: -1 = display refresh rate, 0 = every frame, N = at most N (Metal backend)" );
idCVar r_metalHidden( "r_metalHidden", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_INIT,
	"never show the window and never present (automated runs, Metal backend)" );
idCVar r_metalDebug( "r_metalDebug", "0", CVAR_RENDERER | CVAR_INTEGER,
	"Metal backend debugging: 1 = print every shader pair that is compiled, 2 = also dump generated MSL to the savepath" );

extern "C" void MetalHost_Printf( const char *fmt, ... ) {
	va_list argptr;
	va_start( argptr, fmt );
	common->VPrintf( fmt, argptr );
	va_end( argptr );
}

extern "C" void MetalHost_Warning( const char *fmt, ... ) {
	char msg[4096];
	va_list argptr;
	va_start( argptr, fmt );
	idStr::vsnPrintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );
	common->Warning( "%s", msg );
}

extern "C" void MetalHost_FatalError( const char *fmt, ... ) {
	char msg[4096];
	va_list argptr;
	va_start( argptr, fmt );
	idStr::vsnPrintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );
	common->FatalError( "%s", msg );
}

extern "C" int MetalHost_CvarInt( const char *name ) {
	return cvarSystem->GetCVarInteger( name );
}

extern "C" float MetalHost_CvarFloat( const char *name ) {
	return cvarSystem->GetCVarFloat( name );
}
