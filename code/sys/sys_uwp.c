/*
===========================================================================
Xbox (UWP / Dev Mode) platform glue for RealRTCW.

Built only when RTCW_UWP is defined. The engine itself is compiled as a
normal Win32 program; this file supplies the pieces a packaged UWP app needs:

  - WinMain hands control to SDL3-uwp's SDL_RunApp(), which creates the
    CoreWindow and then calls back into Sys_Main() on the game thread.
  - fs_basepath is the package's install folder (read-only: exe, SDL3.dll,
    Mesa's opengl32.dll, and the game DLLs under main/).
  - fs_homepath is the app's LocalState folder (writable). This is where the
    player copies the game data (main/*.pk3) through Device Portal, and where
    configs and saves go.
  - DLLs inside the package must be loaded with LoadPackagedLibrary(), which
    only accepts paths relative to the package root, so absolute paths into
    the install folder are rewritten before loading.

This file is part of RealRTCW and is licensed under the GPL v3, like the
rest of the engine.
===========================================================================
*/

#ifdef RTCW_UWP

#include <windows.h>
#include <string.h>
#include <stdio.h>

#include "SDL3/SDL.h"
#include "SDL3/SDL_loadso.h"

// These live in SDL_system.h but are only declared there when SDL itself is
// being compiled for WinRT. The engine is compiled as a desktop program, so
// declare the two we need here. Values match SDL_WinRT_Path in SDL3-uwp.
#define UWP_PATH_INSTALLED_LOCATION 0
#define UWP_PATH_LOCAL_FOLDER       1
extern SDL_DECLSPEC const char * SDLCALL SDL_GetWinRTFSPath( int pathType );

int Sys_Main( int argc, char **argv );

static char uwp_installDir[ MAX_PATH ];
static char uwp_localDir[ MAX_PATH ];

static void UWP_CopyPath( char *dst, size_t size, const char *src ) {
	size_t len;

	if ( !src ) {
		dst[ 0 ] = '\0';
		return;
	}
	strncpy( dst, src, size - 1 );
	dst[ size - 1 ] = '\0';

	// strip any trailing separator so the engine's own joins look normal
	len = strlen( dst );
	while ( len > 3 && ( dst[ len - 1 ] == '\\' || dst[ len - 1 ] == '/' ) ) {
		dst[ --len ] = '\0';
	}
}

const char *Sys_UWP_InstallDir( void ) {
	return uwp_installDir;
}

const char *Sys_UWP_LocalDir( void ) {
	return uwp_localDir;
}

/*
=================
Sys_UWP_LoadLibrary

Engine paths are absolute. Packaged DLLs must be loaded relative to the
package root, so translate when the path points inside the install folder.
=================
*/
void *Sys_UWP_LoadLibrary( const char *path ) {
	void *handle = NULL;
	size_t n = strlen( uwp_installDir );

	if ( n && !_strnicmp( path, uwp_installDir, n ) && ( path[ n ] == '\\' || path[ n ] == '/' ) ) {
		char rel[ MAX_PATH ];
		char *p;

		strncpy( rel, path + n + 1, sizeof( rel ) - 1 );
		rel[ sizeof( rel ) - 1 ] = '\0';
		for ( p = rel; *p; p++ ) {
			if ( *p == '/' ) {
				*p = '\\';
			}
		}
		handle = SDL_LoadObject( rel );
	}

	if ( !handle ) {
		handle = SDL_LoadObject( path );
	}
	if ( !handle ) {
		// Dev Mode may still allow a plain LoadLibrary for files outside
		// the package (e.g. a mod's DLLs in LocalState). Worth a try.
		handle = (void *)LoadLibraryA( path );
	}
	return handle;
}

static int SDLCALL UWP_SDLMain( int argc, char **argv ) {
	char exePath[ MAX_PATH ];
	char *args[ 32 ];
	int n = 0;

	(void)argc;
	(void)argv;

	UWP_CopyPath( uwp_installDir, sizeof( uwp_installDir ), SDL_GetWinRTFSPath( UWP_PATH_INSTALLED_LOCATION ) );
	UWP_CopyPath( uwp_localDir, sizeof( uwp_localDir ), SDL_GetWinRTFSPath( UWP_PATH_LOCAL_FOLDER ) );

	// Sys_Main derives the binary path from argv[0]'s directory.
	snprintf( exePath, sizeof( exePath ), "%s\\RealRTCW.x64.exe", uwp_installDir );

	args[ n++ ] = exePath;
	args[ n++ ] = "+set"; args[ n++ ] = "fs_basepath"; args[ n++ ] = uwp_installDir;
	args[ n++ ] = "+set"; args[ n++ ] = "fs_homepath"; args[ n++ ] = uwp_localDir;
	// Console defaults: native-resolution fullscreen, controller on,
	// SDL audio (OpenAL Soft's desktop backend isn't available here).
	args[ n++ ] = "+set"; args[ n++ ] = "r_fullscreen"; args[ n++ ] = "1";
	args[ n++ ] = "+set"; args[ n++ ] = "r_mode"; args[ n++ ] = "-2";
	args[ n++ ] = "+set"; args[ n++ ] = "in_joystick"; args[ n++ ] = "1";
	args[ n++ ] = "+set"; args[ n++ ] = "s_useOpenAL"; args[ n++ ] = "0";
	args[ n ] = NULL;

	return Sys_Main( n, args );
}

int WINAPI WinMain( HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int show ) {
	(void)hInst;
	(void)hPrev;
	(void)cmdLine;
	(void)show;

	// On WinRT, controllers only arrive through Windows.Gaming.Input, and
	// SDL's WGI driver stays off unless asked for.
	SDL_SetHint( SDL_HINT_JOYSTICK_WGI, "1" );

	return SDL_RunApp( 0, NULL, UWP_SDLMain, NULL );
}

#endif // RTCW_UWP
