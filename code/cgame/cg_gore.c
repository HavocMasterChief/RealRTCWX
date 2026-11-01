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

	if ( goLogLines > 4000 ) {
		return;     // enough
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
} goreRun_t;

static gorePool_t goPools[GORE_MAX_POOLS];
static goreDrip_t goDrips[GORE_MAX_DRIPS];
static goreRun_t goRuns[GORE_MAX_RUNS];

static int goDeadTime[MAX_GENTITIES];   // when we first saw each entity dead (0 = alive)
static qboolean goPooled[MAX_GENTITIES];

static qhandle_t goPoolShader[2], goPoolShaderGreen[2];
static qhandle_t goRunShader, goRunShaderGreen, goFootShader;
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
	goPoolShaderGreen[0] = goPoolShader[0];     // green is a vertex colour now
	goPoolShaderGreen[1] = goPoolShader[1];
	goRunShader = trap_R_RegisterShader( "gore/bloodrun" );
	goRunShaderGreen = goRunShader;
	goFootShader = trap_R_RegisterShader( "gore/footprint" );
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

void CG_GoreReset( void ) {
	CG_GoreScreenReset();
	CG_GoreFeetReset();
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
Moving surfaces

Blood that lands on a moving object (the tram, lifts, doors) belongs to
that object: remember where it was, and offset the effect by how far it
has moved since.
===============
*/
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
	// about the darkness of the game's own blood decals
	float k = 0.55f + 0.45f * light;    // mostly constant, a little darker in shadow
	if ( cg_bloodMode.integer == BLOODMODE_GREEN ) {
		out[0] = (byte)( 25 * k );
		out[1] = (byte)( 95 * k );
		out[2] = (byte)( 12 * k );
	} else {
		out[0] = (byte)( 105 * k );
		out[1] = (byte)( 8 * k );
		out[2] = (byte)( 8 * k );
	}
	out[3] = (byte)( 255 * alpha );
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
		numFrags = trap_CM_MarkFragments( 0, (void *)points, projection, GORE_POOL_MAX_POINTS,
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
static void CG_GorePoolAt( const vec3_t point, float radius, int growTime ) {
	trace_t tr;
	vec3_t start, end;

	VectorCopy( point, start );
	start[2] += 8;
	VectorCopy( point, end );
	end[2] -= 96;
	CG_Trace( &tr, start, NULL, NULL, end, -1, MASK_SOLID );
	if ( tr.fraction >= 1.0f || tr.startsolid || tr.plane.normal[2] < 0.7f ) {
		CG_GoreLog( "pool: no floor below (%.0f %.0f %.0f) frac=%.2f startsolid=%i nz=%.2f\n",
					point[0], point[1], point[2], tr.fraction, tr.startsolid, tr.plane.normal[2] );
		return;
	}
	CG_GoreSpawnPool( tr.endpos, radius, growTime, tr.entityNum );
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
		if ( tr.fraction >= 1.0f || tr.entityNum != wallEnt ||
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
		numFrags = trap_CM_MarkFragments( 0, (void *)quad, projection, GORE_RUN_MAX_POINTS,
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
		r->pooled = qtrue;
		VectorAdd( r->end, ofs, end );
		CG_GorePoolAt( end, 9 + random() * 7, 9000 );
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
		// wait for the body to fall before the pool starts (a ragdoll takes
		// a little longer to come to rest)
		{
			vec3_t pelvis;
			qboolean rag = CG_RagdollPelvis( num, pelvis );
			if ( goPooled[num] || cg.time - goDeadTime[num] < ( rag ? 2000 : 1200 ) || !CG_GoreBleeds( s ) ) {
				continue;
			}
			goPooled[num] = qtrue;
			cent = &cg_entities[num];
			CG_GoreLog( "body %i dead (aiChar %i, ragdoll %i): starting pool\n", num, s->aiChar, rag );
			CG_GorePoolAt( rag ? pelvis : cent->lerpOrigin, ( CG_BloodExtreme() ? 40 : 30 ) + random() * 12,
						   CG_BloodExtreme() ? 15000 : 20000 );
		}
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
	static float avgMs = 0, baseline = 0;
	static int lowSince = 0, lastTrim = 0, lastDecay = 0;
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

	if ( fps >= limit ) {
		lowSince = 0;
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
	CG_GoreLog( "governor: fps %.1f below %.1f (baseline %.1f) - trimming\n", fps, limit, baseline );

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
	for ( i = 0; i < 8; i++ ) {
		CG_FreeOldestMark();
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
		numFrags = trap_CM_MarkFragments( 0, (void *)quad, projection, GORE_FOOT_MAX_POINTS,
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
	if ( tr.fraction < 1.0f && !tr.startsolid && tr.plane.normal[2] > 0.7f ) {
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
		trap_R_AddPolyToScene( goFootShader, n, verts );
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
