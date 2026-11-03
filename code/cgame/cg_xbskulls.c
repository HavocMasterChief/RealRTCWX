/*
===========================================================================
RealRTCWX (Xbox): Skulls, the cgame side

The bits are in g_xbskulls (see ../game/bg_xbskulls.h); this reads them once
a frame and does the parts that are only seen or felt on this side:
confetti, the drunk camera, the newsreel film look, blind HUD, paint colours
for blood, how hard bodies get thrown, corpses that blow up (asked of the
server, which does the damage), and the weapon switches of Gun Game.
===========================================================================
*/

#include "cg_local.h"
#include "../game/bg_xbskulls.h"

int cg_xbSkulls;

qboolean CG_Skull( int bit ) {
	return ( cg_xbSkulls & bit ) ? qtrue : qfalse;
}

/*
===============
Confetti

A small particle system of its own: coloured paper squares that tumble,
flutter down and fade. Party Pooper (headshot kills) and Confetti Blood.
===============
*/
#define CONFETTI_MAX 600

typedef struct {
	qboolean active;
	vec3_t pos, vel, axis[2];
	float spin, size;
	byte color[3];
	int start, life;
} confetti_t;

static confetti_t cfParts[CONFETTI_MAX];
static int cfNext;
static qhandle_t cfShader;
static sfxHandle_t cfHorn, cfProjector;

static const byte cfColors[][3] = {
	{ 255, 60, 60 }, { 255, 210, 40 }, { 60, 200, 255 }, { 90, 230, 90 },
	{ 255, 110, 220 }, { 255, 150, 40 }, { 170, 110, 255 }, { 255, 255, 255 }
};

void CG_XbConfetti( const vec3_t origin, int count, float speed ) {
	int i, k;
	for ( i = 0; i < count; i++ ) {
		confetti_t *c = &cfParts[cfNext];
		cfNext = ( cfNext + 1 ) % CONFETTI_MAX;
		memset( c, 0, sizeof( *c ) );
		c->active = qtrue;
		VectorCopy( origin, c->pos );
		for ( k = 0; k < 3; k++ ) {
			c->vel[k] = crandom() * speed;
		}
		c->vel[2] = fabs( c->vel[2] ) * 0.8f + speed * 0.6f;
		c->axis[0][0] = crandom(); c->axis[0][1] = crandom(); c->axis[0][2] = crandom();
		VectorNormalize( c->axis[0] );
		PerpendicularVector( c->axis[1], c->axis[0] );
		c->spin = crandom() * 12;
		c->size = 0.7f + random() * 0.6f;
		memcpy( c->color, cfColors[rand() % 8], 3 );
		c->start = cg.time;
		c->life = 2500 + rand() % 2000;
	}
}

static void CG_XbConfettiDraw( void ) {
	int i, k;
	float dt = cg.frametime * 0.001f;
	if ( dt > 0.1f ) {
		dt = 0.1f;
	}
	for ( i = 0; i < CONFETTI_MAX; i++ ) {
		confetti_t *c = &cfParts[i];
		polyVert_t v[4];
		vec3_t a, b, n;
		float age, alpha, s, co, si;
		trace_t tr;
		vec3_t next;

		if ( !c->active ) {
			continue;
		}
		age = cg.time - c->start;
		if ( age > c->life || age < 0 ) {
			c->active = qfalse;
			continue;
		}
		// paper: heavy drag, a slow flutter down, sways
		if ( dt > 0 && !( c->spin == 0 && VectorLengthSquared( c->vel ) < 0.01f ) ) {   // settled pieces just lie there
			VectorScale( c->vel, 1.0f - 2.8f * dt, c->vel );
			c->vel[2] -= ( CG_Skull( SKULL_MOON ) ? 60 : 180 ) * dt;
			if ( c->vel[2] < -70 ) {
				c->vel[2] = -70;
			}
			c->vel[0] += sin( cg.time * 0.004f + i ) * 40 * dt;
			VectorMA( c->pos, dt, c->vel, next );
			CG_Trace( &tr, c->pos, NULL, NULL, next, -1, MASK_SOLID );
			if ( tr.fraction < 1.0f ) {
				VectorMA( tr.endpos, 0.3f, tr.plane.normal, c->pos );
				VectorClear( c->vel );
				c->spin = 0;
			} else {
				VectorCopy( next, c->pos );
			}
		}
		alpha = 1.0f - age / c->life;
		alpha = alpha > 0.3f ? 1.0f : alpha / 0.3f;
		s = c->size;
		co = cos( c->spin * age * 0.001f );
		si = sin( c->spin * age * 0.001f );
		CrossProduct( c->axis[0], c->axis[1], n );
		VectorScale( c->axis[0], co * s, a );
		VectorMA( a, si * s, n, a );
		VectorScale( c->axis[1], s, b );
		for ( k = 0; k < 4; k++ ) {
			float sx = ( k == 0 || k == 3 ) ? -1 : 1, sy = ( k < 2 ) ? -1 : 1;
			VectorMA( c->pos, sx, a, v[k].xyz );
			VectorMA( v[k].xyz, sy, b, v[k].xyz );
			v[k].st[0] = sx > 0 ? 1 : 0;
			v[k].st[1] = sy > 0 ? 1 : 0;
			v[k].modulate[0] = c->color[0];
			v[k].modulate[1] = c->color[1];
			v[k].modulate[2] = c->color[2];
			v[k].modulate[3] = (byte)( 255 * alpha );
		}
		trap_R_AddPolyToScene( cfShader, 4, v );
	}
}

/*
===============
Corpse hits: Kaboom (a corpse shot a few times blows up) and Explosive
Rounds on corpses. The server does the blast and its damage.
===============
*/
static int xbCorpseHits[MAX_GENTITIES];
static int xbLastBlast;

void CG_XbSkullCorpseHit( int entityNum, const vec3_t point ) {
	if ( entityNum < 0 || entityNum >= MAX_GENTITIES ) {
		return;
	}
	if ( CG_Skull( SKULL_KABOOM ) && ++xbCorpseHits[entityNum] >= 3 ) {
		xbCorpseHits[entityNum] = 0;
		trap_SendClientCommand( va( "xbblast %i %i %i 1", (int)point[0], (int)point[1], (int)point[2] ) );
		xbLastBlast = cg.time;
		return;
	}
	if ( CG_Skull( SKULL_EXPLOSIVE ) && ( cg.time - xbLastBlast > 60 || cg.time < xbLastBlast ) ) {
		trap_SendClientCommand( va( "xbblast %i %i %i 0", (int)point[0], (int)point[1], (int)point[2] ) );
		xbLastBlast = cg.time;
	}
}

/*
===============
How hard hits and blasts throw bodies (Ragdoll Launcher, Boot Camp kicks)
===============
*/
qboolean cg_xbKickHit;      // set around a kick's CG_Bullet (cg_event.c)

float CG_XbThrowScale( void ) {
	float f = CG_Skull( SKULL_LAUNCHER ) ? 5.0f : 1.0f;
	if ( cg_xbKickHit && CG_Skull( SKULL_BOOTCAMP ) ) {
		f *= 6.0f;
	}
	return f;
}

float CG_XbGravityScale( void ) {
	return CG_Skull( SKULL_MOON ) ? 0.3f : 1.0f;
}

/*
===============
Paint colours (Confetti Blood): the gore is drawn as an absorption filter,
so the colour that's left is the one a channel *isn't* taken from.
===============
*/
void CG_XbPaintModulate( float seed, byte *rgba ) {
	static const byte keep[4][3] = {
		{ 255, 255, 0 },    // blue
		{ 255, 0, 255 },    // green
		{ 0, 0, 255 },      // yellow
		{ 0, 255, 0 }       // magenta
	};
	int k = ( (int)( seed * 7919.0f ) & 0x7fffffff ) % 4, i;
	for ( i = 0; i < 3; i++ ) {
		rgba[i] = (byte)( rgba[i] * keep[k][i] / 255 );
	}
}

/*
===============
Drunk: a slow sway and roll
===============
*/
void CG_XbSkullsViewAngles( vec3_t angles ) {
	float t = cg.time * 0.001f;
	if ( !CG_Skull( SKULL_DRUNK ) ) {
		return;
	}
	angles[ROLL] += sin( t * 0.9f ) * 7.0f + sin( t * 2.3f ) * 2.0f;
	angles[PITCH] += sin( t * 1.3f + 1 ) * 3.0f;
	angles[YAW] += sin( t * 0.7f + 2 ) * 4.0f;
}

/*
===============
Newsreel: grain, scratches and a vignette over the picture and a projector
rattling away; the renderer makes the picture black and white with a sepia
tone and flickering exposure (R_XbNewsreel).
===============
*/
static qhandle_t nrGrain, nrScratch, nrVignette;

// (black and white is the renderer's: R_XbNewsreel in tr_arb.c)
static void CG_XbNewsreelCheck( void ) {
	// builds before 63 switched r_greyscale for this; put it back once
	char saved[16];
	trap_Cvar_VariableStringBuffer( "xb_greyscaleSaved", saved, sizeof( saved ) );
	if ( saved[0] ) {
		trap_Cvar_Set( "r_greyscale", saved );
		trap_Cvar_Set( "xb_greyscaleSaved", "" );
	}
}

static void CG_XbNewsreelDraw( void ) {
	float flick, x, y;
	int i;
	vec4_t c;

	if ( !CG_Skull( SKULL_NEWSREEL ) ) {
		return;
	}
	(void)flick;
	// grain: the noise texture at a random offset every frame
	Vector4Set( c, 1, 1, 1, 0.55f );
	trap_R_SetColor( c );
	x = random();
	y = random();
	trap_R_DrawStretchPic( 0, 0, cgs.glconfig.vidWidth, cgs.glconfig.vidHeight, x, y, x + 3, y + 2, nrGrain );
	// a scratch or two that jump around
	for ( i = 0; i < 2; i++ ) {
		if ( rand() % 3 ) {
			continue;
		}
		Vector4Set( c, 1, 1, 1, 0.35f + 0.4f * random() );
		trap_R_SetColor( c );
		CG_DrawPic( random() * 640, 0, 4, 480, nrScratch );
	}
	trap_R_SetColor( NULL );
	CG_DrawPic( -200, 0, 1040, 480, nrVignette );
}

/*
===============
Per frame
===============
*/
void CG_XbSkullsFrame( void ) {
	char buf[32];

	trap_Cvar_VariableStringBuffer( "g_xbskulls", buf, sizeof( buf ) );
	cg_xbSkulls = atoi( buf );
	if ( !cfShader ) {
		cfShader = trap_R_RegisterShader( "xbox/confetti" );
		nrGrain = trap_R_RegisterShader( "xbox/newsreel_grain" );
		nrScratch = trap_R_RegisterShader( "xbox/newsreel_scratch" );
		nrVignette = trap_R_RegisterShader( "xbox/newsreel_vignette" );
		cfHorn = trap_S_RegisterSound( "sound/xbox/party_horn.wav" );
		cfProjector = trap_S_RegisterSound( "sound/xbox/projector.wav" );
	}
	CG_XbNewsreelCheck();
	if ( CG_Skull( SKULL_NEWSREEL ) && cg.snap && !cg_paused.integer ) {
		trap_S_AddLoopingSound( cg.snap->ps.clientNum, cg.refdef.vieworg, vec3_origin, 0, cfProjector, 70 );
	}
}

// with the 3D scene (CG_DrawActiveFrame)
void CG_XbSkullsAddToScene( void ) {
	CG_XbConfettiDraw();
}

// over the picture, before the HUD (CG_Draw2D)
void CG_XbSkullsDraw2D( void ) {
	CG_XbNewsreelDraw();
}

/*
===============
Server commands: "xbparty x y z" (headshot kill, Party Pooper),
"xbgun <weapon>" (Gun Game)
===============
*/
void CG_XbParty_f( void ) {
	vec3_t o;
	o[0] = atof( CG_Argv( 1 ) );
	o[1] = atof( CG_Argv( 2 ) );
	o[2] = atof( CG_Argv( 3 ) );
	CG_XbConfetti( o, 90, 260 );
	trap_S_StartSound( o, ENTITYNUM_WORLD, CHAN_AUTO, cfHorn );
}

void CG_XbGun_f( void ) {
	int w = atoi( CG_Argv( 1 ) );
	if ( w > WP_NONE && w < WP_NUM_WEAPONS ) {
		cg.weaponSelect = w;
		cg.weaponSelectTime = cg.time;
	}
}
