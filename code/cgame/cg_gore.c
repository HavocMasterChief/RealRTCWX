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

/*
===============
CG_GoreLog

Writes to gore_log.txt (in LocalState\\main on Xbox) so effects that
don't show up can be diagnosed without spamming the screen.
===============
*/
static fileHandle_t goLogFile;
static int goLogLines;

static void QDECL CG_GoreLog( const char *fmt, ... ) {
	va_list ap;
	char buf[512];
	int len;

	if ( goLogLines > 4000 || !CG_XbDebugLog() ) {
		return;     // enough (or not logging: xb_debugLog 0)
	}
	if ( !goLogFile ) {
		trap_FS_FOpenFile( "gore_log.txt", &goLogFile, FS_APPEND_SYNC );   // synced: survives a crash
		if ( !goLogFile ) {
			return;
		}
	}
	len = Com_sprintf( buf, sizeof( buf ), "%8i ", cg.time );
	va_start( ap, fmt );
	Q_vsnprintf( buf + len, sizeof( buf ) - len, fmt, ap );
	va_end( ap );
	trap_FS_Write( buf, strlen( buf ), goLogFile );
	goLogLines++;
}

#define GORE_MAX_POOLS          24
#define GORE_MAX_DROPS          256
static int goDropLimit = GORE_MAX_DROPS;     // blood droplets in the air at once (the governor lowers it)

// wounded and still alive: where they were hit (relative to them), until when
static struct {
	int until, next;
	vec3_t ofs;
	int arteryUntil, arteryNext;   // Extreme: a neck hit spurts while they're still on their feet
} goBleeders[MAX_CLIENTS];
static struct {
	int next;
	vec3_t last;
	int squirtStart, squirtUntil, squirtNext;   // Extreme: the wound pumps for a few seconds after death
	qboolean seenAlive;     // (a body that was already dead when the level loaded doesn't)
	int arteryTime;         // last hit in the neck (Extreme: a long arterial squirt)
	int knifeTime;          // last knife hit (a throat-slit spray, a quick pool)
} goBodyDrips[MAX_GENTITIES];
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
	float light;            // level light at the pool, 0..1
	int entityNum;          // surface it's on (moving objects like the tram)
	vec3_t entOrigin;
} gorePool_t;

typedef struct {
	qboolean active;
	int startTime;
	int nextDrip;
	vec3_t origin;
	int entityNum;          // ceiling it's on (moving objects like the tram)
	vec3_t entOrigin;
	int life;               // ms it keeps dripping
	int body;               // 1 + the body whose wound it follows (0: none)
} goreDrip_t;

#define GORE_RUN_MAX_POINTS     96
#define GORE_RUN_MAX_FRAGS      12

typedef struct {
	qboolean active;
	int startTime;
	int runTime;            // ms to reach full length
	vec3_t start;
	vec3_t end;             // where it stops (on the floor if 'pools')
	float length;
	qboolean pools;         // ends on a floor: start a small pool there
	qboolean pooled;
	// projected onto the wall once at full length; revealed top-down over time
	int numFragments;
	int fragFirst[GORE_RUN_MAX_FRAGS];
	int fragCount[GORE_RUN_MAX_FRAGS];
	vec3_t xyz[GORE_RUN_MAX_POINTS];
	float st[GORE_RUN_MAX_POINTS][2];
	float light;
	int entityNum;
	vec3_t entOrigin;
	vec3_t wallNormal;
} goreRun_t;

static gorePool_t goPools[GORE_MAX_POOLS];
static goreDrip_t goDrips[GORE_MAX_DRIPS];
static goreRun_t goRuns[GORE_MAX_RUNS];

static int goDeadTime[MAX_GENTITIES];   // when we first saw each entity dead (0 = alive)
static qboolean goPooled[MAX_GENTITIES];

static qhandle_t goPoolShader[2], goPoolShaderGreen[2];
static qhandle_t goRunShader, goRunShaderGreen, goFootShader, goFootShaderGreen;
static qboolean goRegistered;

#define GORE_MAX_FEET           48
typedef struct goreFoot_s goreFoot_t;
#define GORE_FOOT_MAX_POINTS    40
#define GORE_FOOT_MAX_FRAGS     6
#define GORE_FOOT_STEPS         14
#define GORE_FOOT_STRIDE        30.0f

struct goreFoot_s {
	qboolean active;
	int startTime;
	float light, strength;
	int entityNum;
	vec3_t entOrigin;
	int numFragments;
	int fragFirst[GORE_FOOT_MAX_FRAGS];
	int fragCount[GORE_FOOT_MAX_FRAGS];
	vec3_t xyz[GORE_FOOT_MAX_POINTS];
	float st[GORE_FOOT_MAX_POINTS][2];
};

static goreFoot_t goFeet[GORE_MAX_FEET];
static int goFootSteps;             // bloody steps left
static qboolean goFootLeft;
static vec3_t goFootLast;


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
	goFootShader = trap_R_RegisterShader( "gore/footprint" );
	goFootShaderGreen = trap_R_RegisterShader( "gore/footprint#green" );
	goRegistered = qtrue;
}

/*
===============
CG_GoreReset

Called on level load; everything client-side starts clean.
===============
*/
static void CG_GoreScreenReset( void );
static void CG_GoreFeetReset( void );
static void CG_GoreDropsReset( void );

void CG_GoreReset( void ) {
	CG_GoreScreenReset();
	CG_GoreFeetReset();
	memset( goPools, 0, sizeof( goPools ) );
	memset( goDrips, 0, sizeof( goDrips ) );
	memset( goRuns, 0, sizeof( goRuns ) );
	memset( goDeadTime, 0, sizeof( goDeadTime ) );
	memset( goPooled, 0, sizeof( goPooled ) );
	CG_GoreDropsReset();
	goRegistered = qfalse;
}

static qboolean CG_GoreEnabled( void ) {
	return ( cg_blood.integer && cg_bloodMode.integer != BLOODMODE_OFF ) ? qtrue : qfalse;
}

/*
===============
Moving surfaces

Blood that lands on a moving object (the tram, lifts, doors) belongs to
that object: remember where it was, and offset the effect by how far it
has moved since.
===============
*/
// invisible walls (clip / nodraw brushes, like the ones keeping you off a
// ledge) and sky are solid to traces but nothing to draw blood on
static qboolean CG_GoreVisible( const trace_t *tr ) {
	return !( tr->surfaceFlags & ( SURF_NODRAW | SURF_SKY ) );
}

static void CG_GoreAttach( int entityNum, int *outEnt, vec3_t outOrigin ) {
	if ( entityNum >= 0 && entityNum < MAX_GENTITIES && entityNum != ENTITYNUM_WORLD ) {
		*outEnt = entityNum;
		VectorCopy( cg_entities[entityNum].lerpOrigin, outOrigin );
	} else {
		*outEnt = ENTITYNUM_WORLD;
		VectorClear( outOrigin );
	}
}

static void CG_GoreAttachOffset( int entityNum, const vec3_t origin0, vec3_t out ) {
	if ( entityNum >= 0 && entityNum < MAX_GENTITIES && entityNum != ENTITYNUM_WORLD ) {
		VectorSubtract( cg_entities[entityNum].lerpOrigin, origin0, out );
	} else {
		VectorClear( out );
	}
}

// One quad as a fallback "fragment" when the surface isn't world geometry
// (decal projection only works on the world).
static int CG_GoreQuadFragment( vec3_t quad[4], const vec3_t lift, vec5_t *points, markFragment_t *frag ) {
	int i;
	for ( i = 0; i < 4; i++ ) {
		VectorAdd( quad[i], lift, points[i] );
	}
	frag->firstPoint = 0;
	frag->numPoints = 4;
	return 1;
}

/*
===============
CG_GoreLightAt

How lit a spot is (0.12..1), from the level's light grid, so blood is
as dark as the surface it's on.
===============
*/
static float CG_GoreLightAt( const vec3_t point ) {
	vec3_t p, ambient, directed, dir;
	float l;

	VectorCopy( point, p );
	p[2] += 8;      // just above the surface
	if ( !trap_R_LightForPoint( p, ambient, directed, dir ) ) {
		return 0.45f;
	}
	l = ( ( ambient[0] + ambient[1] + ambient[2] ) + 0.5f * ( directed[0] + directed[1] + directed[2] ) ) / ( 3.0f * 255.0f );
	if ( l > 1.0f ) {
		l = 1.0f;
	}
	l = l * sqrt( l );      // darker in the shadows
	if ( l < 0.08f ) {
		l = 0.08f;
	}
	return l;
}

// blood colour at a given light level, as vertex colours
static void CG_GoreColor( float light, float alpha, byte *out ) {
	// the blood filters the surface under it (see zz_xbox_gore.shader), so
	// the level's light already applies; grey only sets how strong it is
	(void)light;
	if ( alpha < 0 ) {
		alpha = 0;
	} else if ( alpha > 1 ) {
		alpha = 1;
	}
	out[0] = out[1] = out[2] = (byte)( 255 * alpha );
	out[3] = 255;
	if ( CG_Skull( 1 << 7 ) ) {
		CG_XbPaintModulate( light, out );     // Confetti Blood: paint colours
	}
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
static void CG_GoreSpawnPool( const vec3_t origin, float radius, int growTime, int entityNum ) {
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

	numFrags = 0;
	if ( entityNum == ENTITYNUM_WORLD ) {
		numFrags = trap_CM_MarkFragments( 0x20000 /* blood */, (void *)points, projection, GORE_POOL_MAX_POINTS,
										  (float *)&markPoints[0], GORE_POOL_MAX_FRAGS, frags );
	}
	if ( numFrags <= 0 ) {
		// a moving object or a surface the projection missed: a flat quad
		static const vec3_t lift = { 0, 0, 0.25f };
		numFrags = CG_GoreQuadFragment( points, lift, markPoints, frags );
		CG_GoreLog( "pool: using a flat quad (entity %i)\n", entityNum );
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
		// floors only: skip pieces that landed on walls, steps or legs of
		// furniture (anything not level with the pool)
		{
			int q;
			qboolean flat = qtrue;
			for ( q = 0; q < n; q++ ) {
				if ( fabs( markPoints[mf->firstPoint + q][2] - origin[2] ) > 2.5f ) {
					flat = qfalse;
					break;
				}
			}
			if ( !flat ) {
				continue;
			}
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

	if ( !p->numFragments ) {
		p->active = qfalse;
		CG_GoreLog( "pool: no flat floor at (%.0f %.0f %.0f)\n", origin[0], origin[1], origin[2] );
		return;
	}
	p->active = qtrue;
	p->startTime = cg.time;
	p->growTime = growTime;
	p->radius = radius;
	p->variant = rand() & 1;
	p->light = CG_GoreLightAt( origin );
	CG_GoreAttach( entityNum, &p->entityNum, p->entOrigin );
	CG_GoreLog( "pool: spawned at (%.0f %.0f %.0f) r=%.0f frags=%i light=%.2f\n", origin[0], origin[1], origin[2], radius, p->numFragments, p->light );
	VectorCopy( origin, p->origin );
}

/*
===============
CG_GorePoolAt

Finds the floor under 'point' and starts a pool there.
===============
*/
/*
===============
CG_GoreWaterBlood

Blood in water: slow red clouds that drift up and spread instead of a pool.
===============
*/
void CG_GoreWaterBlood( const vec3_t point, int puffs ) {
	int i;
	if ( !CG_GoreEnabled() ) {
		return;
	}
	for ( i = 0; i < puffs; i++ ) {
		vec3_t p, vel;
		VectorCopy( point, p );
		p[0] += crandom() * 4;
		p[1] += crandom() * 4;
		VectorSet( vel, crandom() * 4, crandom() * 4, 5 + random() * 8 );
		CG_SmokePuff( p, vel, 6 + random() * 6, 1, 1, 1, 0.55f, 3500 + random() * 2000,
					  cg.time + i * 180, 300, 0,
					  CG_BloodShader( cgs.media.bloodCloudShader, cgs.media.bloodCloudShaderGreen ) );
	}
}

static void CG_GorePoolAt( const vec3_t point, float radius, int growTime ) {
	trace_t tr;
	vec3_t start, end;

	if ( CG_PointContents( point, -1 ) & CONTENTS_WATER ) {
		CG_GoreWaterBlood( point, 10 );     // a body in water: clouds, no pool
		CG_GoreLog( "pool: under water at (%.0f %.0f %.0f), clouds instead\n", point[0], point[1], point[2] );
		return;
	}

	VectorCopy( point, start );
	start[2] += 8;
	VectorCopy( point, end );
	end[2] -= 96;
	CG_Trace( &tr, start, NULL, NULL, end, -1, MASK_SOLID );
	if ( tr.fraction >= 1.0f || tr.startsolid || tr.plane.normal[2] < 0.7f || !CG_GoreVisible( &tr ) ) {
		CG_GoreLog( "pool: no floor below (%.0f %.0f %.0f) frac=%.2f startsolid=%i nz=%.2f\n",
					point[0], point[1], point[2], tr.fraction, tr.startsolid, tr.plane.normal[2] );
		return;
	}
	if ( CG_BloodExtreme() ) {
		radius *= 1.35f;
	}
	CG_GoreSpawnPool( tr.endpos, radius, growTime, tr.entityNum );

	// Extreme: where the pool reaches a step's edge, blood drips over it
	if ( CG_BloodExtreme() && tr.entityNum == ENTITYNUM_WORLD ) {
		int k, found = 0;
		for ( k = 0; k < 8 && found < 2; k++ ) {
			float a = k * ( M_PI / 4 ) + random() * 0.3f;
			vec3_t edge, below;
			trace_t t2;
			edge[0] = tr.endpos[0] + cos( a ) * radius * 1.05f;
			edge[1] = tr.endpos[1] + sin( a ) * radius * 1.05f;
			edge[2] = tr.endpos[2] + 4;
			VectorCopy( edge, below );
			below[2] -= 64;
			CG_Trace( &t2, edge, NULL, NULL, below, -1, MASK_SOLID );
			if ( t2.startsolid || t2.fraction >= 1.0f ) {
				continue;
			}
			if ( tr.endpos[2] - t2.endpos[2] > 6 ) {
				// the floor drops away here: drip off the edge, a pool below
				vec3_t lip;
				VectorSubtract( edge, tr.endpos, lip );
				VectorNormalize( lip );
				VectorMA( tr.endpos, radius * 0.95f, lip, lip );
				lip[2] = tr.endpos[2] - 1;
				CG_GoreAddDrip( lip, growTime + 8000 );
				CG_GoreSpawnPool( t2.endpos, radius * 0.5f, growTime + 4000, t2.entityNum );
				found++;
			}
		}
	}
}

/*
===============
CG_GoreStartRun

A splat on a wall starts a run of blood down it. The path is walked in
small steps so the run stops at the wall's edge (it never hangs in the air)
or at the floor, where it pools. The run is then projected onto the wall
like a decal, so it follows the real surface.
===============
*/
static void CG_GoreStartRun( const vec3_t point, const vec3_t normal ) {
	goreRun_t *r;
	trace_t tr;
	vec3_t down = { 0, 0, -1 }, rdown, side, prev, cur, a, b, delta;
	vec3_t quad[4], projection, top;
	markFragment_t frags[GORE_RUN_MAX_FRAGS], *mf;
	vec5_t markPoints[GORE_RUN_MAX_POINTS];
	float d, maxLen, len, width;
	int i, j, n, numFrags, outPoints = 0;
	qboolean pools = qfalse;
	int wallEnt;

	// not right next to another run
	for ( i = 0; i < GORE_MAX_RUNS; i++ ) {
		if ( goRuns[i].active && Distance( goRuns[i].start, point ) < 28 ) {
			return;
		}
	}
	d = DotProduct( down, normal );
	VectorMA( down, -d, normal, rdown );
	if ( VectorNormalize( rdown ) < 0.1f ) {
		return;
	}
	CrossProduct( normal, rdown, side );

	// what is the splat on? (the world, or a moving object like the tram)
	VectorMA( point, 1.0f, normal, a );
	VectorMA( point, -4.0f, normal, b );
	CG_Trace( &tr, a, NULL, NULL, b, -1, MASK_SOLID );
	if ( tr.fraction >= 1.0f ) {
		CG_GoreLog( "run: no wall behind (%.0f %.0f %.0f)\n", point[0], point[1], point[2] );
		return;
	}
	if ( !CG_GoreVisible( &tr ) ) {
		CG_GoreLog( "run: invisible wall at (%.0f %.0f %.0f)\n", point[0], point[1], point[2] );
		return;
	}
	wallEnt = tr.entityNum;

	maxLen = 32 + random() * 72;
	VectorMA( point, 1.0f, normal, prev );
	len = 0;
	while ( len < maxLen ) {
		VectorMA( prev, 3.0f, rdown, cur );
		// anything in the way (usually the floor)?
		CG_Trace( &tr, prev, NULL, NULL, cur, -1, MASK_SOLID );
		if ( tr.fraction < 1.0f || tr.startsolid ) {
			if ( !tr.startsolid && tr.plane.normal[2] > 0.7f ) {
				pools = qtrue;
				len += 3.0f * tr.fraction;
			}
			break;
		}
		// is the wall still there, facing the same way?
		VectorMA( cur, 1.0f, normal, a );
		VectorMA( cur, -4.0f, normal, b );
		CG_Trace( &tr, a, NULL, NULL, b, -1, MASK_SOLID );
		if ( tr.fraction >= 1.0f || tr.entityNum != wallEnt || !CG_GoreVisible( &tr ) ||
			 DotProduct( tr.plane.normal, normal ) < 0.85f ) {
			break;
		}
		VectorCopy( cur, prev );
		len += 3.0f;
	}
	if ( len < 10 ) {
		CG_GoreLog( "run: too short (%.0f) at (%.0f %.0f %.0f)\n", len, point[0], point[1], point[2] );
		return;
	}

	width = 2.5f + random() * 2.0f;
	VectorMA( point, 2.0f, normal, top );
	VectorMA( top, -2.0f, rdown, top );     // start a little above the splat
	len += 2.0f;
	for ( i = 0; i < 3; i++ ) {
		quad[0][i] = top[i] - width * side[i];
		quad[1][i] = top[i] + width * side[i];
		quad[2][i] = top[i] + width * side[i] + len * rdown[i];
		quad[3][i] = top[i] - width * side[i] + len * rdown[i];
	}
	VectorScale( normal, -6.0f, projection );
	numFrags = 0;
	if ( wallEnt == ENTITYNUM_WORLD ) {
		numFrags = trap_CM_MarkFragments( 0x20000 /* blood */, (void *)quad, projection, GORE_RUN_MAX_POINTS,
										  (float *)&markPoints[0], GORE_RUN_MAX_FRAGS, frags );
	}
	if ( numFrags <= 0 ) {
		// a moving object: a flat strip on the wall (the walk above already
		// checked the wall is there for the whole length)
		vec3_t lift;
		VectorScale( normal, -1.85f, lift );
		numFrags = CG_GoreQuadFragment( quad, lift, markPoints, frags );
		CG_GoreLog( "run: flat strip (entity %i)\n", wallEnt );
	}

	GORE_ALLOC( goRuns, GORE_MAX_RUNS, r );
	for ( i = 0, mf = frags; i < numFrags; i++, mf++ ) {
		n = mf->numPoints < 0 ? -mf->numPoints : mf->numPoints;
		if ( n > MAX_VERTS_ON_POLY ) {
			n = MAX_VERTS_ON_POLY;
		}
		if ( outPoints + n > GORE_RUN_MAX_POINTS ) {
			break;
		}
		r->fragFirst[r->numFragments] = outPoints;
		r->fragCount[r->numFragments] = n;
		for ( j = 0; j < n; j++ ) {
			VectorCopy( markPoints[mf->firstPoint + j], r->xyz[outPoints] );
			VectorMA( r->xyz[outPoints], 0.1f, normal, r->xyz[outPoints] );
			VectorSubtract( r->xyz[outPoints], top, delta );
			r->st[outPoints][0] = 0.5f + DotProduct( delta, side ) / ( 2.0f * width );
			r->st[outPoints][1] = DotProduct( delta, rdown ) / len;
			outPoints++;
		}
		r->numFragments++;
	}
	if ( !r->numFragments ) {
		return;
	}
	r->active = qtrue;
	r->startTime = cg.time;
	r->length = len;
	r->pools = pools;
	VectorCopy( point, r->start );
	VectorMA( top, len, rdown, r->end );
	VectorMA( r->end, 3.0f, normal, r->end );   // out from the wall, over the floor
	// creeps down: 10-20 s for the full length
	r->runTime = 10000 + rand() % 10000;
	r->light = CG_GoreLightAt( point );
	CG_GoreAttach( wallEnt, &r->entityNum, r->entOrigin );
	VectorCopy( normal, r->wallNormal );
	CG_GoreLog( "run: spawned at (%.0f %.0f %.0f) len=%.0f frags=%i pools=%i light=%.2f\n", point[0], point[1], point[2], len, r->numFragments, pools, r->light );
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
		{
			trace_t tr;
			vec3_t a, b;
			VectorMA( point, 2.0f, normal, a );
			VectorMA( point, -4.0f, normal, b );
			CG_Trace( &tr, a, NULL, NULL, b, -1, MASK_SOLID );
			if ( tr.fraction >= 1.0f || !CG_GoreVisible( &tr ) ) {
				return;     // no real ceiling there (sky, clip)
			}
			// a high ceiling: the drops would seem to come out of nowhere
			VectorMA( point, 160.0f, normal, b );   // (the normal points down)
			CG_Trace( &tr, a, NULL, NULL, b, -1, MASK_SOLID );
			if ( tr.fraction >= 1.0f ) {
				return;
			}
		}
		// don't stack drip points on top of each other
		for ( i = 0; i < GORE_MAX_DRIPS; i++ ) {
			if ( goDrips[i].active && Distance( goDrips[i].origin, point ) < 12 ) {
				return;
			}
		}
		GORE_ALLOC( goDrips, GORE_MAX_DRIPS, d );
		d->active = qtrue;
		d->life = 0;
		d->startTime = cg.time;
		d->nextDrip = cg.time + 300 + rand() % 700;
		VectorMA( point, 1.0f, normal, d->origin );
		{
			trace_t tr;
			vec3_t a, b;
			VectorMA( point, 2.0f, normal, a );
			VectorMA( point, -4.0f, normal, b );
			CG_Trace( &tr, a, NULL, NULL, b, -1, MASK_SOLID );
			CG_GoreAttach( tr.fraction < 1.0f ? tr.entityNum : ENTITYNUM_WORLD, &d->entityNum, d->entOrigin );
		}
	} else if ( fabs( normal[2] ) < 0.35f && random() < 0.15f ) {
		CG_GoreStartRun( point, normal );
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
	vec3_t ofs;

	CG_GoreAttachOffset( p->entityNum, p->entOrigin, ofs );

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
			VectorAdd( p->xyz[first + j], ofs, verts[j].xyz );
			verts[j].st[0] = 0.5f + ( p->st[first + j][0] - 0.5f ) * inv;
			verts[j].st[1] = 0.5f + ( p->st[first + j][1] - 0.5f ) * inv;
			CG_GoreColor( p->light, 0.92f, verts[j].modulate );
		}
		trap_R_AddPolyToScene( shader, n, verts );
	}
}

static void CG_GoreDrawRun( goreRun_t *r, qhandle_t shader ) {
	polyVert_t verts[MAX_VERTS_ON_POLY];
	float frac, inv;
	int i, j;
	vec3_t ofs;

	CG_GoreAttachOffset( r->entityNum, r->entOrigin, ofs );

	frac = (float)( cg.time - r->startTime ) / (float)r->runTime;
	if ( frac > 1.0f ) {
		frac = 1.0f;
	}
	// fast at first, then slowing as it thins out
	frac = 1.0f - ( 1.0f - frac ) * ( 1.0f - frac );
	if ( frac < 0.06f ) {
		frac = 0.06f;
	}
	// stretch the drip texture over the part that has run so far; the
	// texture's last rows are empty, so everything below the tip is clear
	inv = 1.0f / frac;
	for ( i = 0; i < r->numFragments; i++ ) {
		int first = r->fragFirst[i], n = r->fragCount[i];
		for ( j = 0; j < n; j++ ) {
			VectorAdd( r->xyz[first + j], ofs, verts[j].xyz );
			verts[j].st[0] = r->st[first + j][0];
			verts[j].st[1] = r->st[first + j][1] * inv;
			CG_GoreColor( r->light, 0.9f, verts[j].modulate );
		}
		trap_R_AddPolyToScene( shader, n, verts );
	}

	if ( frac >= 0.98f && r->pools && !r->pooled ) {
		vec3_t end;
		float rad = 4.5f + random() * 3.0f;
		r->pooled = qtrue;
		// a small circle on the floor, just out from the foot of the wall
		VectorMA( r->end, rad * 0.7f, r->wallNormal, end );
		VectorAdd( end, ofs, end );
		CG_GorePoolAt( end, rad, 7000 );
	}
}

// a free drip point (a wound hanging over a drop)
static goreDrip_t *CG_GoreNewDrip( const vec3_t origin, int life ) {
	goreDrip_t *d;
	GORE_ALLOC( goDrips, GORE_MAX_DRIPS, d );
	memset( d, 0, sizeof( *d ) );
	d->active = qtrue;
	d->startTime = cg.time;
	d->nextDrip = cg.time + 200;
	d->life = life;
	VectorCopy( origin, d->origin );
	d->origin[2] -= 3;
	d->entityNum = ENTITYNUM_WORLD;
	return d;
}

void CG_GoreAddDrip( const vec3_t origin, int life ) {
	CG_GoreNewDrip( origin, life );
}

// a drip from a body's wound: it moves with the body and stops once the
// wound is no longer hanging over a drop (or the body is gone)
static void CG_GoreAddBodyDrip( int bodyNum, const vec3_t origin, int life ) {
	CG_GoreNewDrip( origin, life )->body = bodyNum + 1;
}

static void CG_GoreUpdateDrip( goreDrip_t *d ) {
	int age = cg.time - d->startTime;
	vec3_t down = { 0, 0, -1 };

	if ( age > ( d->life ? d->life : 30000 ) ) {
		d->active = qfalse;
		return;
	}
	if ( d->body && cg.time >= d->nextDrip ) {
		int bodyNum = d->body - 1;
		vec3_t w, below;
		trace_t tr;
		if ( !CG_RagdollWound( bodyNum, w ) || !( cg_entities[bodyNum].currentState.eFlags & EF_DEAD ) ) {
			d->active = qfalse;     // gone, or not that body any more
			return;
		}
		// still over a drop?
		VectorCopy( w, below );
		below[2] -= 256;
		CG_Trace( &tr, w, NULL, NULL, below, bodyNum, MASK_SOLID );
		if ( tr.fraction * 256 < 12 || tr.fraction >= 1.0f ) {
			d->active = qfalse;
			return;
		}
		VectorCopy( w, d->origin );
		d->origin[2] -= 3;
	}
	if ( cg.time >= d->nextDrip ) {
		// one falling drop that leaves a small splat where it lands
		vec3_t from, ofs;
		CG_GoreAttachOffset( d->entityNum, d->entOrigin, ofs );
		VectorAdd( d->origin, ofs, from );
		CG_AddBloodTrails( from, down, 20, 3000, 1, 0.0f );
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
		// wait for the body to fall before the pool starts (a ragdoll takes
		// a little longer to come to rest)
		{
			vec3_t wound;
			qboolean rag = CG_RagdollWound( num, wound );
			int wait = ( cg.time - goBodyDrips[num].knifeTime < 3000 ) ? 400 : ( rag ? 2000 : 1200 );
			if ( goPooled[num] || cg.time - goDeadTime[num] < wait || !CG_GoreBleeds( s ) ) {
				continue;
			}
			goPooled[num] = qtrue;
			cent = &cg_entities[num];
			CG_GoreLog( "body %i dead (aiChar %i, ragdoll %i): starting pool\n", num, s->aiChar, rag );
			if ( rag ) {
				// the wound hanging in the air (over a ledge or railing):
				// it drips onto whatever is below, and the pool forms there
				trace_t tr;
				vec3_t below;
				VectorCopy( wound, below );
				below[2] -= 256;
				CG_Trace( &tr, wound, NULL, NULL, below, num, MASK_SOLID );
				if ( tr.fraction * 256 > 16 && tr.fraction < 1.0f ) {
					CG_GoreAddBodyDrip( num, wound, 45000 );
					CG_GoreLog( "body %i: wound hangs %.0f above the floor, dripping\n", num, tr.fraction * 256 );
					VectorCopy( tr.endpos, wound );
					wound[2] += 4;
				}
			}
			CG_GorePoolAt( rag ? wound : cent->lerpOrigin, ( CG_BloodExtreme() ? 40 : 30 ) + random() * 12,
						   CG_BloodExtreme() ? 15000 : 20000 );
		}
	}
}

/*
===============
CG_GoreGovernor

Keeps everything until the frame rate drops under 40 (and 10 below what
the scene normally runs at) for two seconds, then removes the oldest effects
(and blood decals) a few at a time until it recovers. If trimming doesn't
bring the frame rate back, the blood isn't what's slow: it stands down for
30 s instead of wiping everything. The droplet budget grows back as soon as
the frame rate is fine again.
===============
*/
static int goDropPriority;      // a cut or a head burst: may use the whole droplet pool

static void CG_GoreGovernor( void ) {
	static float avgMs = 0, baseline = 0, fpsAtFirstTrim = 0;
	static int lowSince = 0, lastTrim = 0, lastDecay = 0, lastGrow = 0, trims = 0, standDownUntil = 0;
	float target, fps, limit;
	int i, oldest;

	static int lastReal;
	int now = trap_Milliseconds(), ms = now - lastReal;

	lastReal = now;
	// real frame time (the game clock can step unevenly); pauses, loads and
	// hitches don't count
	if ( ms <= 0 || ms > 250 || cg.frametime <= 0 ) {
		return;
	}
	avgMs = avgMs ? avgMs * 0.95f + ms * 0.05f : (float)ms;
	fps = 1000.0f / avgMs;

	{
		char buf[32];
		trap_Cvar_VariableStringBuffer( "com_maxfps", buf, sizeof( buf ) );
		target = atof( buf );
	}
	if ( target <= 0 ) {
		target = 60;
	}
	// what this scene normally runs at: follows the frame rate up at once,
	// and only drifts down slowly (1 FPS every 5 s)
	if ( fps > baseline ) {
		baseline = fps;
	} else if ( cg.time - lastDecay > 5000 || lastDecay > cg.time ) {
		baseline -= 1.0f;
		lastDecay = cg.time;
	}
	if ( baseline > target ) {
		baseline = target;
	}
	limit = baseline - 10.0f;
	if ( limit > 40.0f ) {
		limit = 40.0f;      // 40+ is fine on this machine: leave the blood alone
	}

	if ( fps >= limit ) {
		lowSince = 0;
		trims = 0;
		// running fine: the droplet budget grows back
		if ( goDropLimit < GORE_MAX_DROPS && ( cg.time - lastGrow > 500 || lastGrow > cg.time ) ) {
			lastGrow = cg.time;
			goDropLimit += 16;
			if ( goDropLimit > GORE_MAX_DROPS ) {
				goDropLimit = GORE_MAX_DROPS;
			}
		}
		return;
	}
	if ( standDownUntil > cg.time && standDownUntil - cg.time <= 30000 ) {
		return;
	}
	if ( !lowSince || lowSince > cg.time ) {
		lowSince = cg.time;
		return;
	}
	// a short dip (an explosion, a big fight starting) isn't enough
	if ( cg.time - lowSince < 2000 || ( cg.time - lastTrim < 200 && lastTrim <= cg.time ) ) {
		return;
	}
	lastTrim = cg.time;
	if ( !trims ) {
		fpsAtFirstTrim = fps;
	}
	if ( ++trims > 15 ) {
		if ( fps < fpsAtFirstTrim + 3.0f ) {
			CG_GoreLog( "governor: fps %.1f after %i trims (was %.1f) - the blood isn't the cause, standing down for 30 s\n",
						fps, trims - 1, fpsAtFirstTrim );
			standDownUntil = cg.time + 30000;
			trims = 0;
			return;
		}
		trims = 1;
		fpsAtFirstTrim = fps;
	}
	CG_GoreLog( "governor: fps %.1f below %.1f (baseline %.1f) - trimming\n", fps, limit, baseline );

	// fewer blood droplets first, then drips, runs, pools, decals
	if ( goDropLimit > 48 ) {
		goDropLimit = goDropLimit * 7 / 10;
		return;
	}
	for ( oldest = -1, i = 0; i < GORE_MAX_DRIPS; i++ ) {
		if ( goDrips[i].active && ( oldest < 0 || goDrips[i].startTime < goDrips[oldest].startTime ) ) {
			oldest = i;
		}
	}
	if ( oldest >= 0 ) {
		goDrips[oldest].active = qfalse;
		return;
	}
	for ( oldest = -1, i = 0; i < GORE_MAX_FEET; i++ ) {
		if ( goFeet[i].active && ( oldest < 0 || goFeet[i].startTime < goFeet[oldest].startTime ) ) {
			oldest = i;
		}
	}
	if ( oldest >= 0 ) {
		goFeet[oldest].active = qfalse;
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
	if ( cg_bloodAmount.integer < 3 ) {     // Max Blood "Unlimited": keep every decal
		for ( i = 0; i < 8; i++ ) {
			CG_FreeOldestMark();
		}
	}
}

/*
===========================================================================
Bloody footprints

Walk through a pool and you leave footprints for a while; each step is a
little fainter until the blood runs out.
===========================================================================
*/
static void CG_GoreFeetReset( void ) {
	memset( goFeet, 0, sizeof( goFeet ) );
	goFootSteps = 0;
}

static void CG_GoreSpawnFoot( const vec3_t point, const vec3_t normal, const vec3_t forward, float strength, qboolean left, int entityNum ) {
	goreFoot_t *f;
	vec3_t fwd, right, quad[4], projection, delta;
	markFragment_t frags[GORE_FOOT_MAX_FRAGS], *mf;
	vec5_t markPoints[GORE_FOOT_MAX_POINTS];
	const float halfLen = 6.5f, halfWid = 3.25f;
	int i, j, n, numFrags, outPoints = 0;
	float d;

	// forward along the floor
	d = DotProduct( forward, normal );
	VectorMA( forward, -d, normal, fwd );
	if ( VectorNormalize( fwd ) < 0.1f ) {
		return;
	}
	CrossProduct( fwd, normal, right );
	for ( i = 0; i < 3; i++ ) {
		quad[0][i] = point[i] + fwd[i] * halfLen - right[i] * halfWid + normal[i] * 2;
		quad[1][i] = point[i] + fwd[i] * halfLen + right[i] * halfWid + normal[i] * 2;
		quad[2][i] = point[i] - fwd[i] * halfLen + right[i] * halfWid + normal[i] * 2;
		quad[3][i] = point[i] - fwd[i] * halfLen - right[i] * halfWid + normal[i] * 2;
	}
	VectorScale( normal, -8.0f, projection );
	numFrags = 0;
	if ( entityNum == ENTITYNUM_WORLD ) {
		numFrags = trap_CM_MarkFragments( 0x20000 /* blood */, (void *)quad, projection, GORE_FOOT_MAX_POINTS,
										  (float *)&markPoints[0], GORE_FOOT_MAX_FRAGS, frags );
	}
	if ( numFrags <= 0 ) {
		vec3_t lift;
		VectorScale( normal, -1.85f, lift );
		numFrags = CG_GoreQuadFragment( quad, lift, markPoints, frags );
	}
	GORE_ALLOC( goFeet, GORE_MAX_FEET, f );
	for ( i = 0, mf = frags; i < numFrags; i++, mf++ ) {
		n = mf->numPoints < 0 ? -mf->numPoints : mf->numPoints;
		if ( n > MAX_VERTS_ON_POLY ) {
			n = MAX_VERTS_ON_POLY;
		}
		if ( outPoints + n > GORE_FOOT_MAX_POINTS ) {
			break;
		}
		f->fragFirst[f->numFragments] = outPoints;
		f->fragCount[f->numFragments] = n;
		for ( j = 0; j < n; j++ ) {
			VectorCopy( markPoints[mf->firstPoint + j], f->xyz[outPoints] );
			VectorMA( f->xyz[outPoints], 0.12f, normal, f->xyz[outPoints] );
			VectorSubtract( f->xyz[outPoints], point, delta );
			// toe at the top of the texture; mirror for the left foot
			f->st[outPoints][0] = 0.5f + ( left ? -1.0f : 1.0f ) * DotProduct( delta, right ) / ( 2.0f * halfWid );
			f->st[outPoints][1] = 0.5f - DotProduct( delta, fwd ) / ( 2.0f * halfLen );
			outPoints++;
		}
		f->numFragments++;
	}
	if ( !f->numFragments ) {
		return;
	}
	f->active = qtrue;
	f->startTime = cg.time;
	f->strength = strength;
	f->light = CG_GoreLightAt( point );
	CG_GoreAttach( entityNum, &f->entityNum, f->entOrigin );
}

// did the player step in a pool? lay a print every stride while bloody
static void CG_GoreUpdateFeet( void ) {
	playerState_t *ps = &cg.predictedPlayerState;
	vec3_t start, end, forward, delta;
	trace_t tr;
	int i;

	if ( !cg.snap || ps->stats[STAT_HEALTH] <= 0 || ps->groundEntityNum == ENTITYNUM_NONE ) {
		return;
	}
	for ( i = 0; i < GORE_MAX_POOLS; i++ ) {
		gorePool_t *p = &goPools[i];
		float g, dx, dy;
		if ( !p->active || fabs( ps->origin[2] - 24 - p->origin[2] ) > 32 ) {
			continue;
		}
		g = (float)( cg.time - p->startTime ) / (float)p->growTime;
		if ( g > 1.0f ) {
			g = 1.0f;
		}
		g = 1.0f - ( 1.0f - g ) * ( 1.0f - g );
		{
			vec3_t ofs;
			CG_GoreAttachOffset( p->entityNum, p->entOrigin, ofs );
			dx = ps->origin[0] - ( p->origin[0] + ofs[0] );
			dy = ps->origin[1] - ( p->origin[1] + ofs[1] );
		}
		if ( g > 0.25f && dx * dx + dy * dy < ( p->radius * g * 0.7f ) * ( p->radius * g * 0.7f ) ) {
			if ( goFootSteps <= 0 ) {
				VectorCopy( ps->origin, goFootLast );
			}
			goFootSteps = GORE_FOOT_STEPS;
			break;
		}
	}
	if ( goFootSteps <= 0 ) {
		return;
	}
	VectorSubtract( ps->origin, goFootLast, delta );
	delta[2] = 0;
	if ( VectorLength( delta ) < GORE_FOOT_STRIDE ) {
		return;
	}
	VectorCopy( ps->origin, goFootLast );
	VectorNormalize2( delta, forward );

	// where the foot lands: a little to the side of the walk line
	goFootLeft = !goFootLeft;
	{
		vec3_t right = { forward[1], -forward[0], 0 };
		VectorMA( ps->origin, goFootLeft ? -4.0f : 4.0f, right, start );
	}
	VectorCopy( start, end );
	end[2] -= 64;
	CG_Trace( &tr, start, NULL, NULL, end, ps->clientNum, MASK_SOLID );
	if ( tr.fraction < 1.0f && !tr.startsolid && tr.plane.normal[2] > 0.7f && CG_GoreVisible( &tr ) ) {
		CG_GoreSpawnFoot( tr.endpos, tr.plane.normal, forward,
						  (float)goFootSteps / GORE_FOOT_STEPS, goFootLeft, tr.entityNum );
	}
	goFootSteps--;
}

static void CG_GoreDrawFoot( goreFoot_t *f ) {
	polyVert_t verts[MAX_VERTS_ON_POLY];
	int i, j;
	vec3_t ofs;

	CG_GoreAttachOffset( f->entityNum, f->entOrigin, ofs );

	for ( i = 0; i < f->numFragments; i++ ) {
		int first = f->fragFirst[i], n = f->fragCount[i];
		for ( j = 0; j < n; j++ ) {
			VectorAdd( f->xyz[first + j], ofs, verts[j].xyz );
			verts[j].st[0] = f->st[first + j][0];
			verts[j].st[1] = f->st[first + j][1];
			CG_GoreColor( f->light, 0.25f + 0.65f * f->strength, verts[j].modulate );
		}
		trap_R_AddPolyToScene( cg_bloodMode.integer == BLOODMODE_GREEN ? goFootShaderGreen : goFootShader, n, verts );
	}
}

/*
===============
CG_AddGore

Once per frame, alongside the regular marks.
===============
*/
/*
===========================================================================
Blood droplets

A hit sprays real droplets: most out the back along the bullet (exit
wound), a few back toward the shooter. Each flies with gravity and a little
drag and is drawn as a short streak; where it lands it leaves a splat sized
by how big and fast it was, smeared along its path on a glancing hit
(Extreme: ceiling hits start drips, heavy wall hits start runs). Wounded
characters dribble a few as they move, and so do ragdolls still falling.
===========================================================================
*/


typedef struct {
	qboolean active;
	int startTime;
	vec3_t pos, vel;
	float size;
} goreDrop_t;

static goreDrop_t goDrops[GORE_MAX_DROPS];
static int goDropMarksThisFrame, goDropMarkFrame;



int CG_GoreDropsActive( void ) {
	int i, n = 0;
	for ( i = 0; i < GORE_MAX_DROPS; i++ ) {
		n += goDrops[i].active;
	}
	return n;
}

static void CG_GoreDropsReset( void ) {
	memset( goDrops, 0, sizeof( goDrops ) );
	memset( goBleeders, 0, sizeof( goBleeders ) );
	memset( goBodyDrips, 0, sizeof( goBodyDrips ) );
	goDropLimit = GORE_MAX_DROPS;
}

static void CG_GoreNewDrop( const vec3_t pos, const vec3_t vel, float size ) {
	goreDrop_t *d;
	int i, n = 0;
	for ( i = 0; i < GORE_MAX_DROPS; i++ ) {
		n += goDrops[i].active;
	}
	if ( n >= ( goDropPriority ? GORE_MAX_DROPS : goDropLimit ) ) {
		return;
	}
	GORE_ALLOC( goDrops, GORE_MAX_DROPS, d );
	d->active = qtrue;
	d->startTime = cg.time;
	VectorCopy( pos, d->pos );
	VectorCopy( vel, d->vel );
	d->size = size;
}

/*
===============
CG_GoreSpray

A bullet hit flesh at 'point' going along 'dir'. 'force' is the weapon's
(a pistol round 1, a rifle ~3, one shotgun pellet ~0.4).
===============
*/
/*
===============
CG_GoreExtremeHit

Extreme extras for one bullet hit: a red mist at the wound, blood on
anyone standing in the spray, and a neck hit opening an artery.
===============
*/
static void CG_GoreExtremeHit( const vec3_t entry, const vec3_t exitPt, const vec3_t dir, float force, int entityNum ) {
	qboolean green = ( cg_bloodMode.integer == BLOODMODE_GREEN );
	int i;

	// mist, bigger for heavier rounds
	{
		vec3_t vel;
		VectorScale( dir, 25, vel );
		vel[2] += 6;
		CG_SmokePuff( exitPt, vel, 5 + 3.5f * ( force > 4 ? 4 : force ), 1, 1, 1, 0.55f, 500 + 150 * force, cg.time, 0, 0,
					  green ? cgs.media.bloodCloudShaderGreen : cgs.media.bloodCloudShader );
	}
	// whoever stands just past them in the spray gets some of it
	if ( cg.snap ) {
		for ( i = 0; i < cg.snap->numEntities; i++ ) {
			entityState_t *es = &cg.snap->entities[i];
			vec3_t c, to;
			float d;
			if ( es->eType != ET_PLAYER || es->number == entityNum || es->number == cg.snap->ps.clientNum ||
				 es->number >= MAX_GENTITIES ) {
				continue;
			}
			VectorCopy( cg_entities[es->number].lerpOrigin, c );
			c[2] = exitPt[2] < c[2] ? c[2] : exitPt[2] > c[2] + 60 ? c[2] + 60 : exitPt[2];
			VectorSubtract( c, exitPt, to );
			d = VectorNormalize( to );
			if ( d > 72 || DotProduct( to, dir ) < 0.4f ) {
				continue;
			}
			{
				vec3_t from, splash;
				VectorMA( c, -20, to, from );
				VectorScale( to, 2.0f, splash );   // (length 2: a stain, no hole)
				trap_R_AddWound( es->number, from, splash );
			}
		}
	}
	// a neck hit opens an artery
	if ( entityNum >= 0 && entityNum < MAX_CLIENTS && cg_entities[entityNum].currentState.aiChar && !cg_reloading.integer ) {
		vec3_t head, torso, legs, neck;
		CG_GetBleedOrigin( head, torso, legs, entityNum );
		VectorSubtract( torso, head, neck );
		VectorMA( head, 0.3f, neck, neck );
		if ( Distance( entry, neck ) < 7.5f ) {
			goBodyDrips[entityNum].arteryTime = cg.time;
			if ( !( cg_entities[entityNum].currentState.eFlags & EF_DEAD ) ) {
				VectorSubtract( neck, cg_entities[entityNum].lerpOrigin, goBleeders[entityNum].ofs );
				goBleeders[entityNum].arteryUntil = cg.time + 3500;
				goBleeders[entityNum].arteryNext = cg.time + 100;
			}
		}
	}
}

/*
===============
CG_GoreKnife

A knife hit: a throat-slit style spray across the blade's path, an artery
on Extreme, and the pool under the body starts sooner.
===============
*/
void CG_GoreKnife( const vec3_t point, int entityNum ) {
	vec3_t fwd, side;
	int i, n;
	if ( !CG_GoreEnabled() || CG_Skull( 1 << 7 ) || entityNum < 0 || entityNum >= MAX_GENTITIES ||
		 entityNum == cg.snap->ps.clientNum || !CG_GoreBleeds( &cg_entities[entityNum].currentState ) ) {
		return;
	}
	VectorSubtract( point, cg.refdef.vieworg, fwd );
	fwd[2] = 0;
	if ( VectorNormalize( fwd ) < 0.01f ) {
		VectorSet( fwd, 1, 0, 0 );
	}
	VectorSet( side, -fwd[1], fwd[0], 0 );
	if ( rand() & 1 ) {
		VectorNegate( side, side );
	}
	n = CG_BloodExtreme() ? 18 : 7;
	for ( i = 0; i < n; i++ ) {
		vec3_t v;
		float t = random();
		VectorScale( side, 0.6f + t, v );
		VectorMA( v, 0.3f + 0.4f * random(), fwd, v );
		v[2] = 0.15f + random() * 0.35f;
		VectorNormalize( v );
		VectorScale( v, 140 + random() * 160, v );
		CG_GoreNewDrop( point, v, 1.0f + random() * 0.9f );
	}
	goBodyDrips[entityNum].knifeTime = cg.time;
	if ( CG_BloodExtreme() ) {
		goBodyDrips[entityNum].arteryTime = cg.time;
		if ( entityNum < MAX_CLIENTS && !( cg_entities[entityNum].currentState.eFlags & EF_DEAD ) ) {
			VectorSubtract( point, cg_entities[entityNum].lerpOrigin, goBleeders[entityNum].ofs );
			goBleeders[entityNum].arteryUntil = cg.time + 3000;
			goBleeders[entityNum].arteryNext = cg.time + 150;
		}
	}
}

// droplets thrown off something wet hitting a surface (a gib landing)
void CG_GoreSplash( const vec3_t point, const vec3_t normal, int count ) {
	int i;
	if ( !CG_GoreEnabled() || CG_Skull( 1 << 7 ) ) {
		return;
	}
	for ( i = 0; i < count; i++ ) {
		vec3_t v, p;
		VectorScale( normal, 0.6f + random() * 0.5f, v );
		v[0] += crandom() * 0.8f;
		v[1] += crandom() * 0.8f;
		v[2] += crandom() * 0.4f + 0.2f;
		VectorNormalize( v );
		VectorScale( v, 70 + random() * 110, v );
		VectorMA( point, 1.5f, normal, p );
		CG_GoreNewDrop( p, v, 0.7f + random() * 0.8f );
	}
}

/*
===============
CG_GoreHeadBurst

Extreme: a head blown off (or a body shot apart): a big burst of blood
along the shot, a cloud, and the stump pumps for several seconds.
===============
*/
void CG_GoreHeadBurst( const vec3_t point, const vec3_t dir, int entityNum ) {
	qboolean green = ( cg_bloodMode.integer == BLOODMODE_GREEN );
	int i;
	if ( !CG_GoreEnabled() || CG_Skull( 1 << 7 ) ) {
		return;
	}
	goDropPriority = 1;     // the big moments always get their blood
	for ( i = 0; i < 36; i++ ) {
		vec3_t v;
		VectorCopy( dir, v );
		v[0] += crandom() * 0.6f;
		v[1] += crandom() * 0.6f;
		v[2] += crandom() * 0.5f + 0.25f;
		VectorNormalize( v );
		VectorScale( v, 180 + random() * 280, v );
		CG_GoreNewDrop( point, v, 1.2f + random() * 1.4f );
	}
	goDropPriority = 0;
	{
		vec3_t vel;
		VectorScale( dir, 30, vel );
		CG_SmokePuff( point, vel, 22, 1, 1, 1, 0.7f, 1100, cg.time, 0, 0,
					  green ? cgs.media.bloodCloudShaderGreen : cgs.media.bloodCloudShader );
	}
	CG_GoreScreenSplat( point, 3 );
	if ( entityNum >= 0 && entityNum < MAX_GENTITIES ) {
		goBodyDrips[entityNum].arteryTime = cg.time;    // the stump pumps (see CG_GoreUpdateBleeders)
	}
}

void CG_GoreSpray( const vec3_t point0, const vec3_t dir, float force, int entityNum ) {
	qboolean extreme = CG_BloodExtreme();
	const float *point = point0;
	int i, n, back;
	float k;

	if ( !CG_GoreEnabled() || entityNum == cg.snap->ps.clientNum ) {
		return;     // (you being hit: the screen effects cover it)
	}
	if ( entityNum >= 0 && entityNum < MAX_GENTITIES && !CG_GoreBleeds( &cg_entities[entityNum].currentState ) ) {
		return;     // armour, zombies (they have their own dust), bosses
	}
	if ( CG_Skull( 1 << 7 ) ) {
		CG_XbConfettiSpray( point, dir, force );     // Confetti Blood: confetti papers the walls instead
		return;
	}
	if ( force < 0.2f ) {
		force = 0.2f;
	}
	k = pow( force, 0.7f );
	n = (int)( ( extreme ? 14.0f : 6.0f ) * k + random() * 2 );
	if ( n < ( extreme ? 5 : 2 ) ) {
		n = extreme ? 5 : 2;
	}
	if ( n > ( extreme ? 40 : 16 ) ) {
		n = extreme ? 40 : 16;
	}
	// a rifle round goes clean through: the spray starts from the exit
	// wound on the far side (cg_ragdoll.c puts a hole there too)
	{
		vec3_t exitPt;
		VectorMA( point, force >= 2.5f ? 10.0f : 0.0f, dir, exitPt );
		if ( extreme ) {
			CG_GoreExtremeHit( point, exitPt, dir, force, entityNum );
		}
		point = exitPt;
	}
	// exit spray: a tight, fast jet along the bullet, whichever way it was
	// going (shot upward, it paints the ceiling); air drag slows it so a
	// pistol's lands a few feet out and a rifle's further
	for ( i = 0; i < n; i++ ) {
		vec3_t v, p;
		float sp = 300 + 120 * force + random() * 150, spread = 0.08f + 0.1f * random();
		VectorCopy( dir, v );
		v[0] += crandom() * spread;
		v[1] += crandom() * spread;
		v[2] += crandom() * spread;
		VectorNormalize( v );
		VectorScale( v, sp, v );
		VectorMA( point, 2 + random() * 3, dir, p );
		CG_GoreNewDrop( p, v, ( extreme ? 1.3f : 1.0f ) * ( 0.8f + random() * 1.3f ) );
	}
	// back-spatter: a fine burst toward the shooter
	back = extreme ? 3 + rand() % 3 : 1 + rand() % 2;
	for ( i = 0; i < back; i++ ) {
		vec3_t v;
		VectorScale( dir, -1, v );
		v[0] += crandom() * 0.5f;
		v[1] += crandom() * 0.5f;
		v[2] += crandom() * 0.4f + 0.2f;
		VectorNormalize( v );
		VectorScale( v, 50 + random() * 70, v );
		CG_GoreNewDrop( point, v, 0.5f + random() * 0.5f );
	}
	// a live character keeps dribbling from the wound for a while
	if ( entityNum >= 0 && entityNum < MAX_CLIENTS && entityNum != cg.snap->ps.clientNum &&
		 !( cg_entities[entityNum].currentState.eFlags & EF_DEAD ) ) {
		VectorSubtract( point, cg_entities[entityNum].lerpOrigin, goBleeders[entityNum].ofs );
		goBleeders[entityNum].until = cg.time + 4000 + (int)( 2000 * force );
	}
}

// draw one in flight: a short streak along its motion
static void CG_GoreDrawDrop( goreDrop_t *d, qhandle_t shader ) {
	polyVert_t v[4];
	vec3_t along, side, toEye, tail;
	float len = VectorLength( d->vel ) * 0.012f, w = d->size * 0.55f;
	int i;
	if ( len < 0.8f ) {
		len = 0.8f;
	} else if ( len > 7 ) {
		len = 7;
	}
	VectorNormalize2( d->vel, along );
	VectorSubtract( cg.refdef.vieworg, d->pos, toEye );
	CrossProduct( along, toEye, side );
	if ( VectorNormalize( side ) < 0.001f ) {
		return;
	}
	VectorMA( d->pos, -len, along, tail );
	VectorMA( d->pos, w, side, v[0].xyz );
	VectorMA( d->pos, -w, side, v[1].xyz );
	VectorMA( tail, -w, side, v[2].xyz );
	VectorMA( tail, w, side, v[3].xyz );
	v[0].st[0] = 1; v[0].st[1] = 0;
	v[1].st[0] = 0; v[1].st[1] = 0;
	v[2].st[0] = 0; v[2].st[1] = 1;
	v[3].st[0] = 1; v[3].st[1] = 1;
	for ( i = 0; i < 4; i++ ) {
		v[i].modulate[0] = 255; v[i].modulate[1] = 255; v[i].modulate[2] = 255; v[i].modulate[3] = 230;
	}
	trap_R_AddPolyToScene( shader, 4, v );
}

// it hit something
static void CG_GoreDropLand( goreDrop_t *d, const trace_t *tr ) {
	float speed = VectorLength( d->vel ), r, into;
	vec3_t dirN, tang;

	if ( goDropMarkFrame != cg.clientFrame ) {
		goDropMarkFrame = cg.clientFrame;
		goDropMarksThisFrame = 0;
	}
	if ( ++goDropMarksThisFrame > 16 || !CG_GoreVisible( tr ) ) {
		return;     // (a big spray lands over a few frames anyway)
	}
	r = d->size * ( 1.1f + speed / 320.0f );
	if ( CG_BloodExtreme() ) {
		r *= 1.35f;
	}
	if ( r < 1.2f ) {
		r = 1.2f;
	} else if ( r > 9 ) {
		r = 9;
	}
	CG_ImpactMark( CG_BloodDot(), tr->endpos, tr->plane.normal, random() * 360,
				   1, 1, 1, 1, qtrue, r, qfalse, CG_BloodMarkTime() );
	// a fast, glancing hit smears along the way it was going
	VectorNormalize2( d->vel, dirN );
	into = -DotProduct( dirN, tr->plane.normal );
	if ( speed > 140 && into < 0.75f ) {
		int k, steps = ( into < 0.4f ) ? 3 : 2;
		VectorMA( dirN, into, tr->plane.normal, tang );
		if ( VectorNormalize( tang ) > 0.01f ) {
			for ( k = 1; k <= steps; k++ ) {
				vec3_t p;
				float rk = r * ( 1.0f - 0.22f * k );
				VectorMA( tr->endpos, r * 0.9f * k, tang, p );
				CG_ImpactMark( CG_BloodDot(), p, tr->plane.normal, random() * 360,
							   1, 1, 1, 1, qtrue, rk, qfalse, CG_BloodMarkTime() );
			}
		}
	}
	// Extreme: ceilings drip, heavier wall hits can start a run
	if ( d->size > 1.4f && ( rand() % 3 ) == 0 ) {
		CG_GoreSurfaceHit( tr->endpos, tr->plane.normal );
	}
}

static void CG_GoreUpdateDrops( void ) {
	float dt = cg.frametime * 0.001f;
	qhandle_t shader = ( cg_bloodMode.integer == BLOODMODE_GREEN ) ? cgs.media.bloodTrailShaderGreen : cgs.media.bloodTrailShader;
	int i;

	if ( dt <= 0 ) {
		dt = 0;
	} else if ( dt > 0.05f ) {
		dt = 0.05f;
	}
	for ( i = 0; i < GORE_MAX_DROPS; i++ ) {
		goreDrop_t *d = &goDrops[i];
		vec3_t to;
		trace_t tr;
		if ( !d->active ) {
			continue;
		}
		if ( cg.time - d->startTime > 4000 ) {
			d->active = qfalse;
			continue;
		}
		if ( dt > 0 ) {
			d->vel[2] -= 386.0f * dt;
			VectorScale( d->vel, exp( -3.5f * dt ), d->vel );     // a fine spray loses speed fast
			VectorMA( d->pos, dt, d->vel, to );
			CG_Trace( &tr, d->pos, NULL, NULL, to, -1, ( MASK_SHOT & ~CONTENTS_BODY ) | CONTENTS_PLAYERCLIP );
			if ( tr.startsolid ) {
				d->active = qfalse;
				continue;
			}
			if ( tr.fraction < 1.0f ) {
				if ( !( tr.surfaceFlags & SURF_SKY ) ) {
					CG_GoreDropLand( d, &tr );
				}
				d->active = qfalse;
				continue;
			}
			if ( CG_PointContents( to, -1 ) & CONTENTS_WATER ) {
				if ( ( rand() & 3 ) == 0 ) {
					CG_GoreWaterBlood( to, 1 );
				}
				d->active = qfalse;
				continue;
			}
			VectorCopy( to, d->pos );
		}
		CG_GoreDrawDrop( d, shader );
	}
}

// the wounded dribble as they go; so do ragdolls still falling
static void CG_GoreUpdateBleeders( void ) {
	int i;
	if ( !cg.snap ) {
		return;
	}
	for ( i = 0; i < cg.snap->numEntities; i++ ) {
		entityState_t *es = &cg.snap->entities[i];
		int num = es->number;
		vec3_t w, v;
		if ( es->eType != ET_PLAYER || num < 0 || num >= MAX_GENTITIES ) {
			continue;
		}
		if ( !( es->eFlags & EF_DEAD ) ) {
			goBodyDrips[num].seenAlive = qtrue;
			goBodyDrips[num].squirtStart = goBodyDrips[num].squirtUntil = 0;
			// Extreme: an opened artery spurts while they're still standing
			if ( num < MAX_CLIENTS && goBleeders[num].arteryUntil > cg.time && cg.time >= goBleeders[num].arteryNext ) {
				vec3_t out;
				int k;
				goBleeders[num].arteryNext = cg.time + 260 + rand() % 80;
				VectorAdd( cg_entities[num].lerpOrigin, goBleeders[num].ofs, w );
				VectorCopy( goBleeders[num].ofs, out );
				out[2] = 0;
				if ( VectorNormalize( out ) < 0.01f ) {
					VectorSet( out, 1, 0, 0 );
				}
				out[2] = 0.5f;
				for ( k = 0; k < 5; k++ ) {
					VectorCopy( out, v );
					v[0] += crandom() * 0.2f;
					v[1] += crandom() * 0.2f;
					v[2] += crandom() * 0.15f;
					VectorNormalize( v );
					VectorScale( v, 170 + random() * 110, v );
					CG_GoreNewDrop( w, v, 1.0f + random() * 0.8f );
				}
			}
			if ( num < MAX_CLIENTS && goBleeders[num].until > cg.time && cg.time >= goBleeders[num].next ) {
				goBleeders[num].next = cg.time + 180 + rand() % 220;
				VectorAdd( cg_entities[num].lerpOrigin, goBleeders[num].ofs, w );
				VectorSet( v, crandom() * 15, crandom() * 15, -20 );
				CG_GoreNewDrop( w, v, 0.7f + random() * 0.6f );
			}
			continue;
		}
		if ( num < MAX_CLIENTS ) {
			goBleeders[num].until = 0;
		}
		// Extreme: sometimes the wound keeps pumping for a few seconds after
		// death, in pulses that weaken as the heart stops
		if ( !goBodyDrips[num].squirtStart ) {
			goBodyDrips[num].squirtStart = cg.time;
			qboolean artery = cg.time - goBodyDrips[num].arteryTime < 1500;
			if ( goBodyDrips[num].seenAlive && CG_BloodExtreme() && CG_GoreBleeds( es ) && ( artery || random() < 0.4f ) ) {
				goBodyDrips[num].squirtUntil = cg.time + ( artery ? 5000 + rand() % 2000 : 2500 + rand() % 3000 );
				goBodyDrips[num].squirtNext = cg.time + 250;
			}
		}
		if ( goBodyDrips[num].squirtUntil > cg.time && cg.time >= goBodyDrips[num].squirtNext ) {
			vec3_t out, pel;
			float life = (float)( goBodyDrips[num].squirtUntil - cg.time ) /
						 (float)( goBodyDrips[num].squirtUntil - goBodyDrips[num].squirtStart );
			int k, cnt = 2 + (int)( 4 * life );
			goBodyDrips[num].squirtNext = cg.time + 300 + (int)( 250 * ( 1.0f - life ) );   // the pulse slows
			if ( !CG_RagdollWound( num, w ) ) {
				VectorCopy( cg_entities[num].lerpOrigin, w );
				w[2] += 8;
			}
			// out of the body at the wound (away from its middle), a bit upward
			if ( CG_RagdollPelvis( num, pel ) ) {
				VectorSubtract( w, pel, out );
			} else {
				VectorSubtract( w, cg_entities[num].lerpOrigin, out );
			}
			out[2] += 4;
			if ( VectorNormalize( out ) < 0.01f ) {
				VectorSet( out, 0, 0, 1 );
			}
			for ( k = 0; k < cnt; k++ ) {
				VectorCopy( out, v );
				v[0] += crandom() * 0.25f;
				v[1] += crandom() * 0.25f;
				v[2] += 0.3f + crandom() * 0.2f;
				VectorNormalize( v );
				VectorScale( v, ( 90 + random() * 90 ) * ( 0.4f + 0.6f * life ), v );
				CG_GoreNewDrop( w, v, 0.9f + random() * 0.8f );
			}
		}
		// a ragdoll still moving: drops from its wound
		if ( cg.time >= goBodyDrips[num].next && CG_RagdollWound( num, w ) && CG_GoreBleeds( es ) ) {
			goBodyDrips[num].next = cg.time + 120;
			if ( Distance( w, goBodyDrips[num].last ) > 3.0f && Distance( w, goBodyDrips[num].last ) < 200.0f ) {
				VectorSet( v, crandom() * 10, crandom() * 10, -10 );
				CG_GoreNewDrop( w, v, 0.8f + random() * 0.7f );
			}
			VectorCopy( w, goBodyDrips[num].last );
		}
	}
}

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
	CG_GoreUpdateBleeders();
	CG_GoreUpdateDrops();
	CG_GoreUpdateFeet();
	for ( i = 0; i < GORE_MAX_FEET; i++ ) {
		if ( goFeet[i].active ) {
			CG_GoreDrawFoot( &goFeet[i] );
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

static void CG_GoreScreenReset( void ) {
	memset( goScreen, 0, sizeof( goScreen ) );
	goScreenRegistered = qfalse;
}

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
		{
			// on the side of the screen it came from
			vec3_t right, up;
			float sx, sy;
			AngleVectors( cg.refdefViewAngles, NULL, right, up );
			sx = DotProduct( delta, right ) / DotProduct( delta, forward );   // -1..1-ish across the view
			sy = DotProduct( delta, up ) / DotProduct( delta, forward );
			s->x = 0.5f + 0.45f * ( sx > 1 ? 1 : sx < -1 ? -1 : sx ) + crandom() * 0.18f;
			s->y = 0.45f - 0.4f * ( sy > 1 ? 1 : sy < -1 ? -1 : sy ) + crandom() * 0.18f;
			s->x = s->x < 0.08f ? 0.08f : s->x > 0.92f ? 0.92f : s->x;
			s->y = s->y < 0.06f ? 0.06f : s->y > 0.85f ? 0.85f : s->y;
		}
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
	{
		// match the light where you're standing (smoothed)
		static float light = 1.0f;
		float target = CG_GoreLightAt( cg.refdef.vieworg );
		light += ( target - light ) * 0.1f;
		color[0] = color[1] = color[2] = 0.45f + 0.35f * light;
	}

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

// full control: body motors for ms, right/left trigger motors for trigMs
void CG_PadRumbleEx( float low, float high, int ms, float rightTrig, int trigMs, float leftTrig ) {
	if ( rightTrig > 1 ) rightTrig = 1;
	if ( leftTrig > 1 ) leftTrig = 1;
	if ( low > 1 ) low = 1;
	if ( high > 1 ) high = 1;
	if ( ms > goRumbleUntil - cg.time ) {
		goRumbleUntil = cg.time + ms;
		goRumbleStrength = low > high ? low : high;
	}
	goRumbleSerial++;
	trap_Cvar_Set( "xbox_rumble", va( "%.2f %.2f %d %.2f %d %d %.2f", low, high, ms, rightTrig, goRumbleSerial,
									  trigMs, leftTrig ) );
}

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
