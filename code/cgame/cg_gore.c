/*
===========================================================================
cg_gore.c -- persistent blood effects (RealRTCWX)

  - Death pools: a pool of blood slowly spreads under every body
    (Default, Green and Extreme blood modes).
  - Extreme only:
      * ceiling drips: blood that lands on a ceiling drips for a while,
        each drop leaving a small splat on the floor
      * wall runs: some splats on walls run down the wall and pool where
        they meet the floor
  - A frame-rate governor: effects stay until the frame rate falls 10 FPS
    below the target (com_maxfps); then the oldest effects and blood
    decals are removed until it recovers.

Pools and runs are drawn as polygons every frame. A pool is projected onto
the world once at full size; while it "grows", its texture coordinates are
scaled around the centre so more of the (soft-edged, clamped) pool texture
is revealed each frame. That costs a handful of polygons, not new decals.

This file is part of RealRTCW and is licensed under the GPL v3.
===========================================================================
*/

#include "cg_local.h"

#define GORE_MAX_POOLS          24
#define GORE_MAX_DRIPS          16
#define GORE_MAX_RUNS           24
#define GORE_POOL_MAX_POINTS    384
#define GORE_POOL_MAX_FRAGS     48

typedef struct {
	qboolean active;
	int startTime;
	int growTime;           // ms to reach full size
	float radius;
	vec3_t origin;
	int numFragments;
	int fragFirst[GORE_POOL_MAX_FRAGS];
	int fragCount[GORE_POOL_MAX_FRAGS];
	vec3_t xyz[GORE_POOL_MAX_POINTS];
	float st[GORE_POOL_MAX_POINTS][2];  // texture coords at full size
	int variant;
} gorePool_t;

typedef struct {
	qboolean active;
	int startTime;
	int nextDrip;
	vec3_t origin;
} goreDrip_t;

typedef struct {
	qboolean active;
	int startTime;
	int runTime;            // ms to reach full length
	vec3_t start;
	vec3_t down;            // along the wall, downward
	vec3_t side;            // along the wall, horizontal
	vec3_t normal;
	float length;
	float width;
	qboolean pools;         // ends on a floor: start a small pool there
	qboolean pooled;
	vec3_t end;
} goreRun_t;

static gorePool_t goPools[GORE_MAX_POOLS];
static goreDrip_t goDrips[GORE_MAX_DRIPS];
static goreRun_t goRuns[GORE_MAX_RUNS];

static int goDeadTime[MAX_GENTITIES];   // when we first saw each entity dead (0 = alive)
static qboolean goPooled[MAX_GENTITIES];

static qhandle_t goPoolShader[2], goPoolShaderGreen[2];
static qhandle_t goRunShader, goRunShaderGreen;
static qboolean goRegistered;

/*
===============
CG_GoreRegister
===============
*/
static void CG_GoreRegister( void ) {
	if ( goRegistered ) {
		return;
	}
	goPoolShader[0] = trap_R_RegisterShader( "gore/bloodpool1" );
	goPoolShader[1] = trap_R_RegisterShader( "gore/bloodpool2" );
	goPoolShaderGreen[0] = trap_R_RegisterShader( "gore/bloodpool1#green" );
	goPoolShaderGreen[1] = trap_R_RegisterShader( "gore/bloodpool2#green" );
	goRunShader = trap_R_RegisterShader( "gore/bloodrun" );
	goRunShaderGreen = trap_R_RegisterShader( "gore/bloodrun#green" );
	goRegistered = qtrue;
}

/*
===============
CG_GoreReset

Called on level load; everything client-side starts clean.
===============
*/
void CG_GoreReset( void ) {
	memset( goPools, 0, sizeof( goPools ) );
	memset( goDrips, 0, sizeof( goDrips ) );
	memset( goRuns, 0, sizeof( goRuns ) );
	memset( goDeadTime, 0, sizeof( goDeadTime ) );
	memset( goPooled, 0, sizeof( goPooled ) );
	goRegistered = qfalse;
}

static qboolean CG_GoreEnabled( void ) {
	return ( cg_blood.integer && cg_bloodMode.integer != BLOODMODE_OFF ) ? qtrue : qfalse;
}

/*
===============
Slot allocation: reuse a free slot, else replace the oldest
===============
*/
#define GORE_ALLOC( array, max, result ) do { \
		int _i, _old = 0; \
		result = NULL; \
		for ( _i = 0; _i < max; _i++ ) { \
			if ( !array[_i].active ) { result = &array[_i]; break; } \
			if ( array[_i].startTime < array[_old].startTime ) { _old = _i; } \
		} \
		if ( !result ) { result = &array[_old]; } \
		memset( result, 0, sizeof( *result ) ); \
} while ( 0 )

/*
===============
CG_GoreSpawnPool

Projects a pool onto the surface below 'origin' at full size.
===============
*/
static void CG_GoreSpawnPool( const vec3_t origin, float radius, int growTime ) {
	gorePool_t *p;
	vec3_t axis[3], points[4], projection, delta;
	markFragment_t frags[GORE_POOL_MAX_FRAGS], *mf;
	vec5_t markPoints[GORE_POOL_MAX_POINTS];
	int i, j, n, numFrags, outPoints = 0;
	float texScale;
	static const vec3_t up = { 0, 0, 1 };

	GORE_ALLOC( goPools, GORE_MAX_POOLS, p );

	VectorCopy( up, axis[0] );
	PerpendicularVector( axis[1], axis[0] );
	RotatePointAroundVector( axis[2], axis[0], axis[1], random() * 360 );
	CrossProduct( axis[0], axis[2], axis[1] );

	for ( i = 0; i < 3; i++ ) {
		points[0][i] = origin[i] - radius * axis[1][i] - radius * axis[2][i];
		points[1][i] = origin[i] + radius * axis[1][i] - radius * axis[2][i];
		points[2][i] = origin[i] + radius * axis[1][i] + radius * axis[2][i];
		points[3][i] = origin[i] - radius * axis[1][i] + radius * axis[2][i];
	}
	VectorScale( up, -24, projection );

	numFrags = trap_CM_MarkFragments( 0, (void *)points, projection, GORE_POOL_MAX_POINTS,
									  (float *)&markPoints[0], GORE_POOL_MAX_FRAGS, frags );
	if ( numFrags <= 0 ) {
		p->active = qfalse;
		return;
	}

	texScale = 0.5f / radius;
	for ( i = 0, mf = frags; i < numFrags; i++, mf++ ) {
		n = mf->numPoints < 0 ? -mf->numPoints : mf->numPoints;
		if ( n > MAX_VERTS_ON_POLY ) {
			n = MAX_VERTS_ON_POLY;
		}
		if ( outPoints + n > GORE_POOL_MAX_POINTS ) {
			break;
		}
		p->fragFirst[p->numFragments] = outPoints;
		p->fragCount[p->numFragments] = n;
		for ( j = 0; j < n; j++ ) {
			VectorCopy( markPoints[mf->firstPoint + j], p->xyz[outPoints] );
			// lift slightly off the surface to avoid z-fighting
			p->xyz[outPoints][2] += 0.15f;
			VectorSubtract( p->xyz[outPoints], origin, delta );
			p->st[outPoints][0] = 0.5f + DotProduct( delta, axis[1] ) * texScale;
			p->st[outPoints][1] = 0.5f + DotProduct( delta, axis[2] ) * texScale;
			outPoints++;
		}
		p->numFragments++;
	}

	p->active = qtrue;
	p->startTime = cg.time;
	p->growTime = growTime;
	p->radius = radius;
	p->variant = rand() & 1;
	VectorCopy( origin, p->origin );
}

/*
===============
CG_GorePoolAt

Finds the floor under 'point' and starts a pool there.
===============
*/
static void CG_GorePoolAt( const vec3_t point, float radius, int growTime ) {
	trace_t tr;
	vec3_t start, end;

	VectorCopy( point, start );
	start[2] += 8;
	VectorCopy( point, end );
	end[2] -= 96;
	CG_Trace( &tr, start, NULL, NULL, end, -1, MASK_SOLID );
	if ( tr.fraction >= 1.0f || tr.startsolid || tr.plane.normal[2] < 0.7f ) {
		return;
	}
	CG_GoreSpawnPool( tr.endpos, radius, growTime );
}

/*
===============
CG_GoreSurfaceHit

Called (Extreme only) where blood lands. Ceilings start drips; some wall
splats start runs.
===============
*/
void CG_GoreSurfaceHit( const vec3_t point, const vec3_t normal ) {
	if ( !CG_BloodExtreme() || !cg_blood.integer ) {
		return;
	}

	if ( normal[2] < -0.6f ) {
		goreDrip_t *d;
		int i;
		// don't stack drip points on top of each other
		for ( i = 0; i < GORE_MAX_DRIPS; i++ ) {
			if ( goDrips[i].active && Distance( goDrips[i].origin, point ) < 12 ) {
				return;
			}
		}
		GORE_ALLOC( goDrips, GORE_MAX_DRIPS, d );
		d->active = qtrue;
		d->startTime = cg.time;
		d->nextDrip = cg.time + 300 + rand() % 700;
		VectorMA( point, 1.0f, normal, d->origin );
	} else if ( fabs( normal[2] ) < 0.35f && random() < 0.35f ) {
		goreRun_t *r;
		trace_t tr;
		vec3_t down = { 0, 0, -1 }, from, to;
		float d;

		GORE_ALLOC( goRuns, GORE_MAX_RUNS, r );
		VectorCopy( normal, r->normal );
		d = DotProduct( down, normal );
		VectorMA( down, -d, normal, r->down );
		if ( VectorNormalize( r->down ) < 0.1f ) {
			r->active = qfalse;
			return;
		}
		CrossProduct( r->normal, r->down, r->side );
		VectorMA( point, 0.3f, normal, r->start );

		// how far can it run before something stops it?
		r->length = 24 + random() * 56;
		VectorMA( r->start, 1.0f, normal, from );
		VectorMA( from, r->length, r->down, to );
		CG_Trace( &tr, from, NULL, NULL, to, -1, MASK_SOLID );
		r->length *= tr.fraction;
		if ( r->length < 6 ) {
			r->active = qfalse;
			return;
		}
		r->pools = ( tr.fraction < 1.0f && tr.plane.normal[2] > 0.7f ) ? qtrue : qfalse;
		VectorCopy( tr.endpos, r->end );
		r->width = 3 + random() * 3;
		r->runTime = (int)( r->length * ( 60 + random() * 60 ) );   // ~1 unit per 60-120 ms
		r->startTime = cg.time;
		r->active = qtrue;
	}
}

/*
===============
Drawing helpers
===============
*/
static void CG_GoreDrawPool( gorePool_t *p, qhandle_t shader ) {
	polyVert_t verts[MAX_VERTS_ON_POLY];
	float growth, inv;
	int i, j;

	growth = (float)( cg.time - p->startTime ) / (float)p->growTime;
	if ( growth > 1.0f ) {
		growth = 1.0f;
	}
	// ease out: spreads quickly at first, then slows
	growth = 1.0f - ( 1.0f - growth ) * ( 1.0f - growth );
	if ( growth < 0.04f ) {
		growth = 0.04f;
	}
	inv = 1.0f / growth;

	for ( i = 0; i < p->numFragments; i++ ) {
		int first = p->fragFirst[i], n = p->fragCount[i];
		for ( j = 0; j < n; j++ ) {
			VectorCopy( p->xyz[first + j], verts[j].xyz );
			verts[j].st[0] = 0.5f + ( p->st[first + j][0] - 0.5f ) * inv;
			verts[j].st[1] = 0.5f + ( p->st[first + j][1] - 0.5f ) * inv;
			verts[j].modulate[0] = verts[j].modulate[1] = verts[j].modulate[2] = 215;
			verts[j].modulate[3] = 255;
		}
		trap_R_AddPolyToScene( shader, n, verts );
	}
}

static void CG_GoreDrawRun( goreRun_t *r, qhandle_t shader ) {
	polyVert_t verts[4];
	float frac, len;
	vec3_t bottom;
	int i;

	frac = (float)( cg.time - r->startTime ) / (float)r->runTime;
	if ( frac > 1.0f ) {
		frac = 1.0f;
	}
	len = 4 + ( r->length - 4 ) * frac;
	VectorMA( r->start, len, r->down, bottom );

	VectorMA( r->start, -r->width, r->side, verts[0].xyz );
	VectorMA( r->start, r->width, r->side, verts[1].xyz );
	VectorMA( bottom, r->width, r->side, verts[2].xyz );
	VectorMA( bottom, -r->width, r->side, verts[3].xyz );
	verts[0].st[0] = 0; verts[0].st[1] = 0;
	verts[1].st[0] = 1; verts[1].st[1] = 0;
	verts[2].st[0] = 1; verts[2].st[1] = 1;
	verts[3].st[0] = 0; verts[3].st[1] = 1;
	for ( i = 0; i < 4; i++ ) {
		verts[i].modulate[0] = verts[i].modulate[1] = verts[i].modulate[2] = 215;
		verts[i].modulate[3] = 255;
	}
	trap_R_AddPolyToScene( shader, 4, verts );

	if ( frac >= 1.0f && r->pools && !r->pooled ) {
		r->pooled = qtrue;
		CG_GoreSpawnPool( r->end, 6 + random() * 8, 6000 );
	}
}

static void CG_GoreUpdateDrip( goreDrip_t *d ) {
	int age = cg.time - d->startTime;
	vec3_t down = { 0, 0, -1 };

	if ( age > 30000 ) {
		d->active = qfalse;
		return;
	}
	if ( cg.time >= d->nextDrip ) {
		// one falling drop that leaves a small splat where it lands
		CG_AddBloodTrails( d->origin, down, 20, 3000, 1, 0.0f );
		// drips slow down as the blood dries up
		d->nextDrip = cg.time + 400 + age / 15 + rand() % 600;
	}
}

/*
===============
CG_GoreCheckBodies

Starts a pool under every body shortly after it dies.
===============
*/
static qboolean CG_GoreBleeds( const entityState_t *s ) {
	switch ( s->aiChar ) {
	case AICHAR_ZOMBIE:
	case AICHAR_ZOMBIE_SURV:
	case AICHAR_ZOMBIE_FLAME:
	case AICHAR_ZOMBIE_GHOST:
	case AICHAR_PROTOSOLDIER:
	case AICHAR_SUPERSOLDIER:
	case AICHAR_SUPERSOLDIER_LAB:
	case AICHAR_HELGA:
	case AICHAR_HEINRICH:
		return qfalse;
	default:
		return qtrue;
	}
}

static void CG_GoreCheckBodies( void ) {
	int i;

	if ( !cg.snap ) {
		return;
	}
	for ( i = 0; i < cg.snap->numEntities; i++ ) {
		entityState_t *s = &cg.snap->entities[i];
		int num = s->number;
		centity_t *cent;

		if ( num < 0 || num >= MAX_GENTITIES || s->eType != ET_PLAYER ) {
			continue;
		}
		if ( !( s->eFlags & EF_DEAD ) ) {
			goDeadTime[num] = 0;
			goPooled[num] = qfalse;
			continue;
		}
		if ( !goDeadTime[num] ) {
			goDeadTime[num] = cg.time;
		}
		// wait for the body to fall before the pool starts
		if ( goPooled[num] || cg.time - goDeadTime[num] < 1200 || !CG_GoreBleeds( s ) ) {
			continue;
		}
		goPooled[num] = qtrue;
		cent = &cg_entities[num];
		CG_GorePoolAt( cent->lerpOrigin, ( CG_BloodExtreme() ? 40 : 30 ) + random() * 12,
					   CG_BloodExtreme() ? 15000 : 20000 );
	}
}

/*
===============
CG_GoreGovernor

Keeps everything until the frame rate drops 10 below com_maxfps for about a
second, then removes the oldest effects (and blood decals) a few at a time
until it recovers.
===============
*/
static void CG_GoreGovernor( void ) {
	static float avgMs = 0;
	static int lowSince = 0, lastTrim = 0;
	float target, fps;
	int i, oldest;

	if ( cg.frametime <= 0 || cg.frametime > 250 ) {
		return;     // pauses, loads and hitches don't count
	}
	avgMs = avgMs ? avgMs * 0.95f + cg.frametime * 0.05f : (float)cg.frametime;

	{
		char buf[32];
		trap_Cvar_VariableStringBuffer( "com_maxfps", buf, sizeof( buf ) );
		target = atof( buf );
	}
	if ( target <= 0 ) {
		target = 60;
	}
	fps = 1000.0f / avgMs;
	if ( fps >= target - 10 ) {
		lowSince = 0;
		return;
	}
	if ( !lowSince ) {
		lowSince = cg.time;
		return;
	}
	if ( cg.time - lowSince < 1000 || cg.time - lastTrim < 200 ) {
		return;
	}
	lastTrim = cg.time;

	// drips first, then runs, then pools, then decals
	for ( oldest = -1, i = 0; i < GORE_MAX_DRIPS; i++ ) {
		if ( goDrips[i].active && ( oldest < 0 || goDrips[i].startTime < goDrips[oldest].startTime ) ) {
			oldest = i;
		}
	}
	if ( oldest >= 0 ) {
		goDrips[oldest].active = qfalse;
		return;
	}
	for ( oldest = -1, i = 0; i < GORE_MAX_RUNS; i++ ) {
		if ( goRuns[i].active && ( oldest < 0 || goRuns[i].startTime < goRuns[oldest].startTime ) ) {
			oldest = i;
		}
	}
	if ( oldest >= 0 ) {
		goRuns[oldest].active = qfalse;
		return;
	}
	for ( oldest = -1, i = 0; i < GORE_MAX_POOLS; i++ ) {
		if ( goPools[i].active && ( oldest < 0 || goPools[i].startTime < goPools[oldest].startTime ) ) {
			oldest = i;
		}
	}
	if ( oldest >= 0 ) {
		goPools[oldest].active = qfalse;
		return;
	}
	for ( i = 0; i < 8; i++ ) {
		CG_FreeOldestMark();
	}
}

/*
===============
CG_AddGore

Once per frame, alongside the regular marks.
===============
*/
void CG_AddGore( void ) {
	int i;
	qboolean green = ( cg_bloodMode.integer == BLOODMODE_GREEN );

	if ( !CG_GoreEnabled() ) {
		return;
	}
	CG_GoreRegister();
	CG_GoreCheckBodies();
	CG_GoreGovernor();

	for ( i = 0; i < GORE_MAX_POOLS; i++ ) {
		if ( goPools[i].active ) {
			gorePool_t *p = &goPools[i];
			CG_GoreDrawPool( p, green ? goPoolShaderGreen[p->variant] : goPoolShader[p->variant] );
		}
	}
	for ( i = 0; i < GORE_MAX_RUNS; i++ ) {
		if ( goRuns[i].active ) {
			CG_GoreDrawRun( &goRuns[i], green ? goRunShaderGreen : goRunShader );
		}
	}
	for ( i = 0; i < GORE_MAX_DRIPS; i++ ) {
		if ( goDrips[i].active ) {
			CG_GoreUpdateDrip( &goDrips[i] );
		}
	}
}

/*
===========================================================================
Extreme: close-range screen splatter

When a target is shot or gibbed close to the player and in front of them,
blood hits the "lens": a few splats appear on screen, slide down a little
and fade over a couple of seconds. Red only (Extreme is always red).
===========================================================================
*/

#define GORE_MAX_SCREEN         8
#define GORE_SCREEN_RANGE       140.0f
#define GORE_SCREEN_LIFE        2600

typedef struct {
	qboolean active;
	int startTime;
	float x, y, size;       // in 0..1 screen fractions (size relative to height)
	float slide;            // how far it slides down over its life (fraction of height)
	int variant;
	qboolean flip;
} goreScreen_t;

static goreScreen_t goScreen[GORE_MAX_SCREEN];
static qhandle_t goScreenShader[3];
static qboolean goScreenRegistered;
static int goScreenLast;

static void CG_GoreScreenRegister( void ) {
	if ( goScreenRegistered ) {
		return;
	}
	goScreenShader[0] = trap_R_RegisterShaderNoMip( "gore/screensplat1" );
	goScreenShader[1] = trap_R_RegisterShaderNoMip( "gore/screensplat2" );
	goScreenShader[2] = trap_R_RegisterShaderNoMip( "gore/screensplat3" );
	goScreenRegistered = qtrue;
}

/*
===============
CG_GoreScreenSplat

'count' splats if 'origin' is close and in front of the view.
===============
*/
void CG_GoreScreenSplat( const vec3_t origin, int count ) {
	vec3_t delta, forward;
	float dist;
	int i;

	if ( !CG_BloodExtreme() || !cg_blood.integer || cg.renderingThirdPerson ) {
		return;
	}
	VectorSubtract( origin, cg.refdef.vieworg, delta );
	dist = VectorNormalize( delta );
	if ( dist > GORE_SCREEN_RANGE ) {
		return;
	}
	AngleVectors( cg.refdefViewAngles, forward, NULL, NULL );
	if ( DotProduct( delta, forward ) < 0.35f ) {
		return;
	}
	// rapid fire shouldn't paint the whole screen at once
	if ( count < 3 && cg.time - goScreenLast < 250 && goScreenLast <= cg.time ) {
		return;
	}
	goScreenLast = cg.time;

	// closer = bigger splats
	for ( i = 0; i < count; i++ ) {
		goreScreen_t *s;
		GORE_ALLOC( goScreen, GORE_MAX_SCREEN, s );
		s->active = qtrue;
		s->startTime = cg.time;
		s->x = 0.12f + random() * 0.76f;
		s->y = 0.08f + random() * 0.70f;
		s->size = ( 0.18f + random() * 0.22f ) * ( 1.3f - dist / GORE_SCREEN_RANGE * 0.6f );
		s->slide = 0.02f + random() * 0.06f;
		s->variant = rand() % 3;
		s->flip = ( rand() & 1 ) ? qtrue : qfalse;
	}
}

/*
===============
CG_DrawGoreScreen

Called from CG_Draw2D, under the HUD.
===============
*/
void CG_DrawGoreScreen( void ) {
	int i;
	float vw = cgs.glconfig.vidWidth, vh = cgs.glconfig.vidHeight;
	vec4_t color = { 1, 1, 1, 1 };

	if ( !CG_BloodExtreme() ) {
		return;
	}
	CG_GoreScreenRegister();

	for ( i = 0; i < GORE_MAX_SCREEN; i++ ) {
		goreScreen_t *s = &goScreen[i];
		float t, w, h, x, y;

		if ( !s->active ) {
			continue;
		}
		if ( s->startTime > cg.time ) {     // level restart / time went back
			s->active = qfalse;
			continue;
		}
		t = (float)( cg.time - s->startTime ) / GORE_SCREEN_LIFE;
		if ( t >= 1.0f ) {
			s->active = qfalse;
			continue;
		}
		// full strength for the first 40%, then fade out
		color[3] = ( t < 0.4f ) ? 0.9f : 0.9f * ( 1.0f - ( t - 0.4f ) / 0.6f );
		h = s->size * vh;
		w = h;
		x = s->x * vw - w * 0.5f;
		y = ( s->y + s->slide * t ) * vh - h * 0.5f;
		trap_R_SetColor( color );
		if ( s->flip ) {
			trap_R_DrawStretchPic( x, y, w, h, 1, 0, 0, 1, goScreenShader[s->variant] );
		} else {
			trap_R_DrawStretchPic( x, y, w, h, 0, 0, 1, 1, goScreenShader[s->variant] );
		}
	}
	trap_R_SetColor( NULL );
}

/*
===========================================================================
Controller rumble

cgame can't talk to the controller, so it posts requests through the
xbox_rumble cvar ("low high ms trigger serial"); the Xbox client reads it
each frame and drives the gamepad (see CL_CheckRumble). On PC the cvar is
simply unused.
===========================================================================
*/
static int goRumbleSerial;
static int goRumbleUntil;
static float goRumbleStrength;

void CG_PadRumble( float low, float high, int ms, float trigger ) {
	float strength = low > high ? low : high;

	if ( low <= 0 && high <= 0 && trigger <= 0 ) {
		return;
	}
	// don't let a weak effect cut a strong one short
	if ( cg.time < goRumbleUntil && strength < goRumbleStrength * 0.6f && goRumbleUntil >= cg.time ) {
		return;
	}
	if ( low > 1 ) low = 1;
	if ( high > 1 ) high = 1;
	if ( trigger > 1 ) trigger = 1;
	goRumbleUntil = cg.time + ms;
	goRumbleStrength = strength;
	goRumbleSerial++;
	trap_Cvar_Set( "xbox_rumble", va( "%.2f %.2f %d %.2f %d", low, high, ms, trigger, goRumbleSerial ) );
}
