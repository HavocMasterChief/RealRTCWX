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

/*
=================
UWP_ExceptionLogger

Vectored handler: sees crashes before WinRT or the C runtime can swallow
them, and appends where it happened to LocalState\crash_info.txt. It only
logs and lets the exception continue, so behaviour is otherwise unchanged.
=================
*/
static LONG CALLBACK UWP_ExceptionLogger( EXCEPTION_POINTERS *ep ) {
	static volatile LONG logged = 0;
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	void *addr = ep->ExceptionRecord->ExceptionAddress;
	char path[ MAX_PATH ], module[ MAX_PATH ] = "?";
	HMODULE mod = NULL;
	FILE *f;

	if ( code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
		 code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
		 code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000409 /* fast fail / GS */ ) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if ( InterlockedIncrement( &logged ) > 8 ) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)addr, &mod ) && mod ) {
		const char *base;
		GetModuleFileNameA( mod, module, sizeof( module ) );
		base = strrchr( module, '\\' );
		if ( base ) {
			memmove( module, base + 1, strlen( base + 1 ) + 1 );
		}
	}

	snprintf( path, sizeof( path ), "%s\\crash_info.txt", uwp_localDir );
	f = fopen( path, "a" );
	if ( f ) {
		fprintf( f, "exception 0x%08lX in %s+0x%llX (thread %lu)",
			(unsigned long)code, module,
			(unsigned long long)( (char *)addr - (char *)mod ), GetCurrentThreadId() );
		if ( code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 ) {
			fprintf( f, ", %s address 0x%llX",
				ep->ExceptionRecord->ExceptionInformation[ 0 ] == 0 ? "reading" :
				ep->ExceptionRecord->ExceptionInformation[ 0 ] == 1 ? "writing" : "executing",
				(unsigned long long)ep->ExceptionRecord->ExceptionInformation[ 1 ] );
		}
		fprintf( f, "\n" );
		fclose( f );
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

/*
=================
UWP_CaptureStderr

Mesa reports fatal problems on stderr and then aborts, which leaves no
trace on Xbox. Mesa uses the shared UCRT (ucrtbase.dll), not our static
CRT, so point *that* CRT's stdout/stderr at LocalState\\stderr.txt.
=================
*/
static void UWP_CaptureStderr( void ) {
	typedef int ( __cdecl *open_osfhandle_t )( intptr_t, int );
	typedef int ( __cdecl *dup2_t )( int, int );
	typedef int ( __cdecl *putenv_t )( const char * );
	char path[ MAX_PATH ];
	HMODULE ucrt;
	HANDLE h;

	snprintf( path, sizeof( path ), "%s\\stderr.txt", uwp_localDir );
	h = CreateFileA( path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	if ( h == INVALID_HANDLE_VALUE ) {
		return;
	}
	SetStdHandle( STD_ERROR_HANDLE, h );
	SetStdHandle( STD_OUTPUT_HANDLE, h );

	ucrt = GetModuleHandleA( "ucrtbase.dll" );
	if ( ucrt ) {
		open_osfhandle_t p_open = (open_osfhandle_t)GetProcAddress( ucrt, "_open_osfhandle" );
		dup2_t p_dup2 = (dup2_t)GetProcAddress( ucrt, "_dup2" );
		putenv_t p_putenv = (putenv_t)GetProcAddress( ucrt, "_putenv" );
		HANDLE h2 = INVALID_HANDLE_VALUE;

		if ( p_open && p_dup2 ) {
			int fd = p_open( (intptr_t)h, 0 );
			if ( fd >= 0 ) {
				p_dup2( fd, 2 );
			}
			if ( DuplicateHandle( GetCurrentProcess(), h, GetCurrentProcess(), &h2, 0, FALSE, DUPLICATE_SAME_ACCESS ) ) {
				fd = p_open( (intptr_t)h2, 0 );
				if ( fd >= 0 ) {
					p_dup2( fd, 1 );
				}
			}
		}
		if ( p_putenv ) {
			// Ask Mesa to explain errors instead of failing quietly.
			p_putenv( "MESA_DEBUG=1" );
		}
	}
	{
		const char *msg = ucrt ? "stderr captured (ucrtbase)\r\n" : "stderr: ucrtbase.dll not loaded yet\r\n";
		DWORD w;
		WriteFile( h, msg, (DWORD)strlen( msg ), &w, NULL );
	}
}

static void UWP_AtExit( void ) {
	char path[ MAX_PATH ];
	FILE *f;

	snprintf( path, sizeof( path ), "%s\\crash_info.txt", uwp_localDir );
	f = fopen( path, "a" );
	if ( f ) {
		fprintf( f, "process exited through exit() (not a crash)\n" );
		fclose( f );
	}
}

static int SDLCALL UWP_SDLMain( int argc, char **argv ) {
	char exePath[ MAX_PATH ];
	char *args[ 32 ];
	int n = 0;

	(void)argc;
	(void)argv;

	UWP_CopyPath( uwp_installDir, sizeof( uwp_installDir ), SDL_GetWinRTFSPath( UWP_PATH_INSTALLED_LOCATION ) );
	UWP_CopyPath( uwp_localDir, sizeof( uwp_localDir ), SDL_GetWinRTFSPath( UWP_PATH_LOCAL_FOLDER ) );

	AddVectoredExceptionHandler( 1, UWP_ExceptionLogger );
	UWP_CaptureStderr();
	atexit( UWP_AtExit );

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
	// The render-to-texture path (bloom etc.) crashes in FBO_Init on Mesa's
	// D3D12 driver; render straight to the window until that's sorted out.
	args[ n++ ] = "+set"; args[ n++ ] = "r_fbo"; args[ n++ ] = "0";
	// Flush every console line to LocalState/main/qconsole.log so a crash
	// still leaves a usable log to pull from Device Portal.
	args[ n++ ] = "+set"; args[ n++ ] = "logfile"; args[ n++ ] = "2";
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
