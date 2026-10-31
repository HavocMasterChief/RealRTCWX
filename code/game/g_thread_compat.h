/*
 * Minimal pthread_create()/sleep() for the two places in game code that use
 * them (delayed removal of the no-target flag). MinGW/Linux/macOS use the real
 * POSIX headers; MSVC (desktop and Xbox UWP) gets a tiny CRT-based shim.
 */
#ifndef G_THREAD_COMPAT_H
#define G_THREAD_COMPAT_H

#ifndef _MSC_VER
#include <pthread.h>
#include <unistd.h>
#else
#include <stdlib.h>
#include <process.h>

typedef uintptr_t pthread_t;

typedef struct {
	void *( *fn )( void * );
	void *arg;
} g_thread_start_t;

static unsigned __stdcall g_thread_trampoline( void *p ) {
	g_thread_start_t s = *(g_thread_start_t *)p;
	free( p );
	s.fn( s.arg );
	return 0;
}

static int pthread_create( pthread_t *t, const void *attr, void *( *fn )( void * ), void *arg ) {
	g_thread_start_t *s = (g_thread_start_t *)malloc( sizeof( *s ) );
	uintptr_t h;
	(void)attr;
	if ( !s ) {
		return -1;
	}
	s->fn = fn;
	s->arg = arg;
	h = _beginthreadex( NULL, 0, g_thread_trampoline, s, 0, NULL );
	if ( !h ) {
		free( s );
		return -1;
	}
	*t = h;
	return 0;
}

__declspec( dllimport ) void __stdcall Sleep( unsigned long ms );
#define sleep( secs ) Sleep( (unsigned long)( secs ) * 1000UL )
#endif

#endif
