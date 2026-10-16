/*
metal_host.h - what the Metal files need from the engine.

The Metal files (metal_device.cpp, mgl*.cpp) are C++17 and include metal-cpp; the engine is
C++11 and its headers are kept out of them. metal_host.cpp implements these with idlib.
*/
#ifndef METAL_HOST_H
#define METAL_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

void	MetalHost_Printf( const char *fmt, ... ) __attribute__(( format( printf, 1, 2 ) ));
void	MetalHost_Warning( const char *fmt, ... ) __attribute__(( format( printf, 1, 2 ) ));
void	MetalHost_FatalError( const char *fmt, ... ) __attribute__(( format( printf, 1, 2 ) ));
int		MetalHost_CvarInt( const char *name );
float	MetalHost_CvarFloat( const char *name );

// what glimp.cpp calls (declared again in sys/glimp.cpp)
// returns the samples per pixel in use (>= 1), or 0 on failure
int		MGL_Init( void *caMetalLayer, int width, int height, int multiSamples );
void	MGL_Shutdown( void );
void	MGL_SwapBuffers( void );
void	MGL_Resize( int width, int height );
void	( *MGL_GetProcAddress( const char *name ) )( void );

#ifdef __cplusplus
}
#endif

#endif
