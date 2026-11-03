/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_arb.c -- ARB assembly vertex/fragment program helpers backing the \r_fbo offscreen-rendering effects, since this renderer has no GLSL pipeline; adapted from Quake3e's code/renderer/tr_arb.c

#include "tr_local.h"

static GLuint current_vp = 0;
static GLuint current_fp = 0;


/*
==============
ARB_CompileProgram

Compiles a single ARB assembly vertex or fragment program from source text.
`target` is GL_VERTEX_PROGRAM_ARB or GL_FRAGMENT_PROGRAM_ARB, `program` is a
name previously allocated via qglGenProgramsARB(). Returns qfalse (and logs
the driver's error string) on a compile failure.
==============
*/
qboolean ARB_CompileProgram( GLenum target, const char *text, GLuint program )
{
	GLint errorPos;
	GLenum errCode;

	// drain errors pending from earlier in this init pass so they aren't misattributed below
	while ( qglGetError() != GL_NO_ERROR ) {}

	qglBindProgramARB( target, program );
	qglProgramStringARB( target, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen( text ), text );
	qglGetIntegerv( GL_PROGRAM_ERROR_POSITION_ARB, &errorPos );

	if ( ( errCode = qglGetError() ) != GL_NO_ERROR || errorPos != -1 )
	{
		ri.Printf( PRINT_WARNING, "%s program compile error (0x%x, offset %i): %s\n%s\n",
			( target == GL_FRAGMENT_PROGRAM_ARB ) ? "fragment" : "vertex",
			errCode, errorPos, qglGetString( GL_PROGRAM_ERROR_STRING_ARB ), text );
		qglBindProgramARB( target, 0 );
		return qfalse;
	}

	return qtrue;
}


/*
==============
ARB_ProgramEnable

Binds the given vertex/fragment program pair (either may be 0 to leave that
stage's fixed-function pipeline active instead).
==============
*/
void ARB_ProgramEnable( GLuint vp, GLuint fp )
{
	if ( current_vp != vp ) {
		current_vp = vp;
		if ( current_vp ) {
			qglEnable( GL_VERTEX_PROGRAM_ARB );
			qglBindProgramARB( GL_VERTEX_PROGRAM_ARB, current_vp );
		} else {
			qglDisable( GL_VERTEX_PROGRAM_ARB );
		}
	}

	if ( current_fp != fp ) {
		current_fp = fp;
		if ( current_fp ) {
			qglEnable( GL_FRAGMENT_PROGRAM_ARB );
			qglBindProgramARB( GL_FRAGMENT_PROGRAM_ARB, current_fp );
		} else {
			qglDisable( GL_FRAGMENT_PROGRAM_ARB );
		}
	}
}


/*
==============
ARB_ProgramDisable

Returns both program stages to the fixed-function pipeline.
==============
*/
void ARB_ProgramDisable( void )
{
	ARB_ProgramEnable( 0, 0 );
}


/*
==============================================================================

FBO POST-PROCESS EFFECTS

Applied while blitting tr.mainFbo to the real backbuffer at the end of each
frame (see RB_SwapBuffers in tr_backend.c). This renderer has no GLSL
pipeline, so gamma correction is an ARB assembly fragment program instead.

==============================================================================
*/

typedef enum {
	GAMMA_FRAGMENT,
	NUM_ARB_PROGRAMS
} arbProgramNum;

static GLuint arbPrograms[ NUM_ARB_PROGRAMS ];
static qboolean arbProgramsReady = qfalse;

// applies pow(color, invGamma), the software equivalent of s_gammatable[] since hw gamma is unavailable; program.local[0].x = tr.invGamma
static const char *gammaFP =
	"!!ARBfp1.0\n"
	"TEMP tex;\n"
	"TEX tex, fragment.texcoord[0], texture[0], 2D;\n"
	"POW result.color.x, tex.x, program.local[0].x;\n"
	"POW result.color.y, tex.y, program.local[0].x;\n"
	"POW result.color.z, tex.z, program.local[0].x;\n"
	"MOV result.color.w, tex.w;\n"
	"END\n";


/*
==============
GL_ProgramAvailable
==============
*/
qboolean GL_ProgramAvailable( void )
{
	return arbProgramsReady;
}


/*
==============
ARB_InitPrograms

Compiles the ARB programs used by the \r_fbo postprocess pass. Safe to call
whenever glRefConfig.arbPrograms is false (a no-op) -- FBO_PostProcess() falls
back to a plain blit in that case.
==============
*/
void ARB_InitPrograms( void )
{
	arbProgramsReady = qfalse;

	if ( !glRefConfig.arbPrograms ) {
		return;
	}

	qglGenProgramsARB( NUM_ARB_PROGRAMS, arbPrograms );

	if ( !ARB_CompileProgram( GL_FRAGMENT_PROGRAM_ARB, gammaFP, arbPrograms[ GAMMA_FRAGMENT ] ) ) {
		qglDeleteProgramsARB( NUM_ARB_PROGRAMS, arbPrograms );
		Com_Memset( arbPrograms, 0, sizeof( arbPrograms ) );
		return;
	}

	arbProgramsReady = qtrue;
}


/*
==============
ARB_ShutdownPrograms
==============
*/
void ARB_ShutdownPrograms( void )
{
	if ( arbProgramsReady ) {
		qglDeleteProgramsARB( NUM_ARB_PROGRAMS, arbPrograms );
	}
	Com_Memset( arbPrograms, 0, sizeof( arbPrograms ) );
	arbProgramsReady = qfalse;

	// these named programs are gone; don't let a later re-init's recycled IDs match the stale cache
	current_vp = 0;
	current_fp = 0;
}


/*
==============
FBO_PostProcess

Blits tr.mainFbo to the real backbuffer, applying gamma correction along the
way when possible. Falls back to a plain hardware blit (no gamma correction,
matching this renderer's pre-\r_fbo behavior) if the ARB program didn't compile.
==============
*/
void FBO_PostProcess( void )
{
	if ( !arbProgramsReady ) {
		FBO_FastBlit( tr.mainFbo, NULL, GL_COLOR_BUFFER_BIT, GL_NEAREST );
		return;
	}

	FBO_Bind( NULL );

	RB_SetGL2D();

	// override RB_SetGL2D()'s alpha blend so this composite fully replaces the backbuffer
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );

	GL_SelectTexture( 0 );
	GL_Bind( (image_t *)tr.mainFbo->colorImage );

	ARB_ProgramEnable( 0, arbPrograms[ GAMMA_FRAGMENT ] );
	qglProgramLocalParameter4fARB( GL_FRAGMENT_PROGRAM_ARB, 0, tr.invGamma, tr.invGamma, tr.invGamma, 1.0f );

	// flip texcoords vertically: the FBO texture is bottom-left origin, RB_SetGL2D() isn't
	qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	qglBegin( GL_QUADS );
		qglTexCoord2f( 0.0f, 1.0f ); qglVertex2f( 0.0f, 0.0f );
		qglTexCoord2f( 1.0f, 1.0f ); qglVertex2f( (float)glConfig.vidWidth, 0.0f );
		qglTexCoord2f( 1.0f, 0.0f ); qglVertex2f( (float)glConfig.vidWidth, (float)glConfig.vidHeight );
		qglTexCoord2f( 0.0f, 0.0f ); qglVertex2f( 0.0f, (float)glConfig.vidHeight );
	qglEnd();

	ARB_ProgramDisable();
}


/*
==============================================================================

RealRTCWX Newsreel skull (g_xbskulls bit 12): the finished 3D picture is
copied to a texture and drawn back through a fragment program that turns it
black and white with a sepia tone and a flickering exposure. Runs before the
HUD (with bloom, see RB_ExecuteRenderCommands), so the HUD keeps its colour;
cgame draws the grain, scratches and vignette on top.

==============================================================================
*/
#ifdef RTCW_UWP
static const char *newsreelFP =
	"!!ARBfp1.0\n"
	"PARAM lumw = { 0.299, 0.587, 0.114, 0 };\n"
	"PARAM tint = { 1.08, 0.96, 0.78, 1 };\n"
	"TEMP c, l;\n"
	"TEX c, fragment.texcoord[0], texture[0], 2D;\n"
	"DP3 l.x, c, lumw;\n"
	"MAD_SAT l.x, l.x, program.local[0].x, program.local[0].y;\n"
	"MUL result.color.xyz, tint, l.xxxx;\n"
	"MOV result.color.w, 1;\n"
	"END\n";

void R_XbNewsreel( void ) {
	extern cvar_t *r_xbSkulls;
	static GLuint prog;
	static int state;           // 0 not tried, 1 ready, -1 failed
	static image_t *screenImg;
	static int doneFrame = -1;
	static int gen;
	float tw, th, contrast, bright;

	if ( !r_xbSkulls || !( r_xbSkulls->integer & ( 1 << 12 ) ) ) {
		return;
	}
	if ( !backEnd.doneSurfaces || doneFrame == tr.frameCount ) {
		return;
	}
	doneFrame = tr.frameCount;

	// the image (and maybe the GL context) die with the renderer
	if ( gen != tr_xbRendererGen ) {
		gen = tr_xbRendererGen;
		state = 0;
		prog = 0;
		screenImg = NULL;
	}
	if ( state == 0 ) {
		byte *data;
		state = -1;
		if ( !glRefConfig.arbPrograms ) {
			ri.Printf( PRINT_ALL, "newsreel: no ARB fragment programs\n" );
			return;
		}
		qglGenProgramsARB( 1, &prog );
		if ( !ARB_CompileProgram( GL_FRAGMENT_PROGRAM_ARB, newsreelFP, prog ) ) {
			return;
		}
		data = ri.Hunk_AllocateTempMemory( 2048 * 2048 * 4 );
		Com_Memset( data, 0, 2048 * 2048 * 4 );
		screenImg = R_CreateImage( "*xbnewsreel", data, 2048, 2048, IMGTYPE_COLORALPHA, IMGFLAG_NONE, 0 );
		ri.Hunk_FreeTempMemory( data );
		state = 1;
		ri.Printf( PRINT_ALL, "newsreel: post-process ready\n" );
	}
	if ( state < 0 || glConfig.vidWidth > 2048 || glConfig.vidHeight > 2048 ) {
		return;
	}

	if ( !backEnd.projection2D ) {
		RB_SetGL2D();
	}
	GL_SelectTexture( 0 );
	GL_Bind( screenImg );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, glConfig.vidWidth, glConfig.vidHeight );

	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
	// old film: a bit more contrast, and the exposure flickers
	contrast = 1.25f;
	bright = -0.08f + 0.05f * ( ( rand() & 255 ) / 255.0f ) - ( ( rand() & 31 ) == 0 ? 0.08f : 0 );
	ARB_ProgramEnable( 0, prog );
	qglProgramLocalParameter4fARB( GL_FRAGMENT_PROGRAM_ARB, 0, contrast, bright, 0, 0 );

	tw = glConfig.vidWidth / 2048.0f;
	th = glConfig.vidHeight / 2048.0f;
	qglColor4f( 1, 1, 1, 1 );
	qglBegin( GL_QUADS );
		// the copy is bottom-up, the 2D projection top-down
		qglTexCoord2f( 0, th ); qglVertex2f( 0, 0 );
		qglTexCoord2f( tw, th ); qglVertex2f( glConfig.vidWidth, 0 );
		qglTexCoord2f( tw, 0 ); qglVertex2f( glConfig.vidWidth, glConfig.vidHeight );
		qglTexCoord2f( 0, 0 ); qglVertex2f( 0, glConfig.vidHeight );
	qglEnd();
	ARB_ProgramDisable();
}
#endif
