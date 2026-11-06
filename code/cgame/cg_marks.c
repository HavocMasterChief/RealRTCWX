/*
===========================================================================

Return to Castle Wolfenstein single player GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company. 

This file is part of the Return to Castle Wolfenstein single player GPL Source Code (RTCW SP Source Code).  

RTCW SP Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTCW SP Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTCW SP Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the RTCW SP Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the RTCW SP Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

// cg_marks.c -- wall marks

#include "cg_local.h"

/*
===================================================================

MARK POLYS

===================================================================
*/


markPoly_t cg_activeMarkPolys;          // double linked list
markPoly_t  *cg_freeMarkPolys;          // single linked list
markPoly_t cg_markPolys[MAX_MARK_POLYS];
static int cg_numActiveMarks;
int CG_MarksActive( void ) {
	return cg_numActiveMarks;
}

/*
===================
CG_InitMarkPolys

This is called at startup and for tournement restarts
===================
*/
void    CG_InitMarkPolys( void ) {
	int i;
	markPoly_t *trav, *lasttrav;

	memset( cg_markPolys, 0, sizeof( cg_markPolys ) );

	cg_activeMarkPolys.nextMark = &cg_activeMarkPolys;
	cg_activeMarkPolys.prevMark = &cg_activeMarkPolys;
	cg_freeMarkPolys = cg_markPolys;
	cg_numActiveMarks = 0;
	for ( i = 0, trav = cg_markPolys + 1, lasttrav = cg_markPolys ; i < MAX_MARK_POLYS - 1 ; i++, trav++ ) {
		lasttrav->nextMark = trav;
		lasttrav = trav;
	}
}


/*
==================
CG_FreeMarkPoly
==================
*/
void CG_FreeMarkPoly( markPoly_t *le ) {
	if ( !le->prevMark || !le->nextMark ) {
		CG_Error( "CG_FreeLocalEntity: not active" );
	}

	// remove from the doubly linked active list
	cg_numActiveMarks--;
	le->prevMark->nextMark = le->nextMark;
	le->nextMark->prevMark = le->prevMark;

	// the free list is only singly linked
	le->nextMark = cg_freeMarkPolys;
	cg_freeMarkPolys = le;
}

/*
===================
CG_AllocMark

Will allways succeed, even if it requires freeing an old active mark
===================
*/
/*
===================
CG_FreeOldestMark

Used by the gore frame-rate governor.
===================
*/
qboolean CG_IsBloodMark( qhandle_t shader ) {
	int i;
	for ( i = 0; i < 5; i++ ) {
		if ( shader == cgs.media.bloodDotShaders[i] || shader == cgs.media.bloodDotShadersGreen[i] ) {
			return qtrue;
		}
	}
	return qfalse;
}

/*
===================
CG_ClearBloodMarks

Blood switched off: remove the blood decals already on the walls.
===================
*/
void CG_ClearBloodMarks( void ) {
	markPoly_t *mp, *next;

	if ( !cg_activeMarkPolys.nextMark ) {
		return;
	}
	for ( mp = cg_activeMarkPolys.nextMark; mp != &cg_activeMarkPolys; mp = next ) {
		next = mp->nextMark;
		if ( CG_IsBloodMark( mp->markShader ) ) {
			CG_FreeMarkPoly( mp );
		}
	}
}

void CG_FreeOldestMark( void ) {
	if ( cg_activeMarkPolys.prevMark && cg_activeMarkPolys.prevMark != &cg_activeMarkPolys ) {
		CG_FreeMarkPoly( cg_activeMarkPolys.prevMark );
	}
}

markPoly_t  *CG_AllocMark( int endTime ) {
	markPoly_t  *le; //, *trav, *lastTrav;
	int time;

	// RealRTCWX: Max Blood caps how many decals stay (oldest go first)
	{
		static const int cap[4] = { 2048, 4096, 8192, MAX_MARK_POLYS };
		int a = cg_bloodAmount.integer < 0 ? 0 : cg_bloodAmount.integer > 3 ? 3 : cg_bloodAmount.integer;
		while ( cg_numActiveMarks >= cap[a] && cg_activeMarkPolys.prevMark != &cg_activeMarkPolys ) {
			CG_FreeMarkPoly( cg_activeMarkPolys.prevMark );
		}
	}
	if ( !cg_freeMarkPolys ) {
		// no free entities, so free the one at the end of the chain
		// remove the oldest active entity
		time = cg_activeMarkPolys.prevMark->time;
		while ( cg_activeMarkPolys.prevMark && time == cg_activeMarkPolys.prevMark->time ) {
			CG_FreeMarkPoly( cg_activeMarkPolys.prevMark );
		}
	}

	le = cg_freeMarkPolys;
	cg_freeMarkPolys = cg_freeMarkPolys->nextMark;

	memset( le, 0, sizeof( *le ) );

	// Ridah, TODO: sort this, so the list is always sorted by longest duration -> shortest duration,
	// this way the shortest duration mark will always get overwritten first
	//for (trav = cg_activeMarkPolys.nextMark; (trav->duration + trav->time > endTime) && (trav != cg_activeMarkPolys.prevMark) ; lastTrav = trav, trav++ ) {
	// Respect the FOR loop
	//}

	// link into the active list
	cg_numActiveMarks++;
	le->nextMark = cg_activeMarkPolys.nextMark;
	le->prevMark = &cg_activeMarkPolys;
	cg_activeMarkPolys.nextMark->prevMark = le;
	cg_activeMarkPolys.nextMark = le;
	return le;
}



/*
=================
CG_ImpactMark

origin should be a point within a unit of the plane
dir should be the plane normal

temporary marks will not be stored or randomly oriented, but immediately
passed to the renderer.
=================
*/
// Ridah, increased this since we leave them around for longer
#define MAX_MARK_FRAGMENTS  1024 // RealRTCW was 384
#define MAX_MARK_POINTS     2048 // RealRTCW was 1024
//#define	MAX_MARK_FRAGMENTS	128
//#define	MAX_MARK_POINTS		384

// these are ignored now for the most part
//#define	MARK_TOTAL_TIME		20000	// (SA) made this a cvar: cg_markTime  (we could cap the time or remove marks quicker if too long a time starts to cause new marks to not appear)
#define MARK_FADE_TIME      30000

void CG_ImpactMark( qhandle_t markShader, const vec3_t origin, const vec3_t dir,
					float orientation, float red, float green, float blue, float alpha,
					qboolean alphaFade, float radius, qboolean temporary, int duration ) {
	vec3_t axis[3];
	float texCoordScale;
	vec3_t originalPoints[4];
	byte colors[4];
	int i, j;
	int numFragments;
	markFragment_t markFragments[MAX_MARK_FRAGMENTS], *mf;
	vec5_t markPoints[MAX_MARK_POINTS];             // Ridah, made it vec5_t so it includes S/T
	vec3_t projection;
	int multMaxFragments = 1;

	if ( !cg_markTime.integer || !markShader ) {
		return;
	}

	// blood off: no blood decals from any code path
	if ( CG_IsBloodMark( markShader ) && ( !cg_blood.integer || cg_bloodMode.integer == BLOODMODE_OFF ) ) {
		return;
	}

	if ( radius <= 0 ) {
		// just ignore it, don't error out
		return;
//		CG_Error( "CG_ImpactMark called with <= 0 radius" );
	}

	// Ridah, if no duration, use the default
	if ( duration < 0 ) {
		if ( duration == -2 ) {
			multMaxFragments = -1;  // use original mapping
		}

//		duration = MARK_TOTAL_TIME;
		duration = cg_markTime.integer;
	}

	// create the texture axis
	VectorNormalize2( dir, axis[0] );
	PerpendicularVector( axis[1], axis[0] );
	RotatePointAroundVector( axis[2], axis[0], axis[1], orientation );
	CrossProduct( axis[0], axis[2], axis[1] );

	texCoordScale = 0.5 * 1.0 / radius;

	// create the full polygon
	for ( i = 0 ; i < 3 ; i++ ) {
		originalPoints[0][i] = origin[i] - radius * axis[1][i] - radius * axis[2][i];
		originalPoints[1][i] = origin[i] + radius * axis[1][i] - radius * axis[2][i];
		originalPoints[2][i] = origin[i] + radius * axis[1][i] + radius * axis[2][i];
		originalPoints[3][i] = origin[i] - radius * axis[1][i] + radius * axis[2][i];
	}

	// get the fragments
	//VectorScale( dir, -20, projection );
	VectorScale( dir, radius * 2, projection );
	{
		int o = ( (int)orientation % 360 + 360 ) % 360;
		if ( CG_IsBloodMark( markShader ) ) {
			trace_t tr;
			vec3_t a, b;
			o |= 0x20000;   // blood: rain-wet "nomarks" ground takes it too (tr_marks.c)
			// on such ground it's thinned by the water and washes away
			VectorMA( origin, 2, dir, a );
			VectorMA( origin, -16, dir, b );
			CG_Trace( &tr, a, NULL, NULL, b, ENTITYNUM_NONE, MASK_SOLID );
			if ( tr.fraction < 1.0f && ( tr.surfaceFlags & SURF_NOMARKS ) ) {
				alpha *= 0.55f;
				if ( duration <= 0 || duration > 60000 ) {
					duration = 60000;
				}
			}
		}
		numFragments = trap_CM_MarkFragments( o, (void *)originalPoints,
											  projection, MAX_MARK_POINTS, (float *)&markPoints[0],
											  MAX_MARK_FRAGMENTS * multMaxFragments, markFragments );
	}

	colors[0] = red * 255;
	colors[1] = green * 255;
	colors[2] = blue * 255;
	colors[3] = alpha * 255;

	for ( i = 0, mf = markFragments ; i < numFragments ; i++, mf++ ) {
		polyVert_t  *v;
		polyVert_t verts[MAX_VERTS_ON_POLY];
		markPoly_t  *mark;
		qboolean hasST;

		// we have an upper limit on the complexity of polygons
		// that we store persistantly
		if ( mf->numPoints > MAX_VERTS_ON_POLY ) {
			mf->numPoints = MAX_VERTS_ON_POLY;
		}
		if ( mf->numPoints < 0 ) {
			hasST = qtrue;
			mf->numPoints *= -1;
		} else {
			hasST = qfalse;
		}
		for ( j = 0, v = verts ; j < mf->numPoints ; j++, v++ ) {
			vec3_t delta;

			VectorCopy( markPoints[mf->firstPoint + j], v->xyz );

			if ( !hasST ) {
				VectorSubtract( v->xyz, origin, delta );
				v->st[0] = 0.5 + DotProduct( delta, axis[1] ) * texCoordScale;
				v->st[1] = 0.5 + DotProduct( delta, axis[2] ) * texCoordScale;
			} else {
				v->st[0] = markPoints[mf->firstPoint + j][3];
				v->st[1] = markPoints[mf->firstPoint + j][4];
			}

			*(int *)v->modulate = *(int *)colors;
		}

		// if it is a temporary (shadow) mark, add it immediately and forget about it
		if ( temporary ) {
			trap_R_AddPolyToScene( markShader, mf->numPoints, verts );
			continue;
		}

		// otherwise save it persistantly
		mark = CG_AllocMark( cg.time + duration );
		mark->time = cg.time;
		mark->alphaFade = alphaFade;
		mark->markShader = markShader;
		mark->poly.numVerts = mf->numPoints;
		mark->color[0] = red;
		mark->color[1] = green;
		mark->color[2] = blue;
		mark->color[3] = alpha;
		mark->duration = duration;
		memcpy( mark->verts, verts, mf->numPoints * sizeof( verts[0] ) );
		{
			int k;
			float r2 = 0;
			VectorClear( mark->center );
			for ( k = 0; k < mf->numPoints; k++ ) {
				VectorAdd( mark->center, verts[k].xyz, mark->center );
			}
			VectorScale( mark->center, 1.0f / ( mf->numPoints ? mf->numPoints : 1 ), mark->center );
			for ( k = 0; k < mf->numPoints; k++ ) {
				float d = DistanceSquared( mark->center, verts[k].xyz );
				if ( d > r2 ) {
					r2 = d;
				}
			}
			mark->radius = sqrt( r2 ) + 1.0f;
		}
	}
}


int cg_marksDrawn;      // (the perf line)
static vec3_t cullOrg, cullAxis[3];
static float cullTanX, cullTanY;
static qboolean cullOn;

static void CG_MarkCullSetup( void ) {
	cullOn = cg.refdef.fov_x > 1 && cg.refdef.fov_x < 179 && cg.refdef.fov_y > 1 && cg.refdef.fov_y < 179;
	VectorCopy( cg.refdef.vieworg, cullOrg );
	AxisCopy( cg.refdef.viewaxis, cullAxis );
	cullTanX = tan( cg.refdef.fov_x * ( M_PI / 360.0f ) );
	cullTanY = tan( cg.refdef.fov_y * ( M_PI / 360.0f ) );
}

// outside the view (with its whole bounding sphere), or very far away
static qboolean CG_MarkCulled( const markPoly_t *mp ) {
	vec3_t d;
	float f, s, u, r = mp->radius;
	if ( !cullOn || r <= 0 ) {
		return qfalse;  // (an old mark with no sphere)
	}
	VectorSubtract( mp->center, cullOrg, d );
	f = DotProduct( d, cullAxis[0] );
	if ( f < -r || f > 6000.0f + r ) {
		return qtrue;   // behind, or beyond anything you'd make out
	}
	s = DotProduct( d, cullAxis[1] );
	u = DotProduct( d, cullAxis[2] );
	// sphere against the side planes (with the sphere's size along them)
	if ( fabs( s ) - r * 1.5f > ( f > 0 ? f : 0 ) * cullTanX ) {
		return qtrue;
	}
	if ( fabs( u ) - r * 1.5f > ( f > 0 ? f : 0 ) * cullTanY ) {
		return qtrue;
	}
	return qfalse;
}

/*
===============
CG_AddMarks
===============
*/

void CG_AddMarks( void ) {
	int drawn = 0;
	int j;
	markPoly_t  *mp, *next;
	int t;
	int fade;

	if ( !cg_markTime.integer ) {
		return;
	}
	CG_MarkCullSetup();

	mp = cg_activeMarkPolys.nextMark;
	for ( ; mp != &cg_activeMarkPolys ; mp = next ) {
		// grab next now, so if the local entity is freed we
		// still have it
		next = mp->nextMark;

		// see if it is time to completely remove it
		if ( cg.time > mp->time + mp->duration ) {
			CG_FreeMarkPoly( mp );
			continue;
		}

		// RealRTCWX: only what can be seen gets drawn (the original sent
		// every mark on the map to the renderer every frame; with Max Blood
		// that's thousands, and it cost the frame rate)
		if ( CG_MarkCulled( mp ) ) {
			continue;
		}
		drawn++;

		// fade out the energy bursts
		if ( mp->markShader == cgs.media.energyMarkShader ) {

			fade = 450 - 450 * ( ( cg.time - mp->time ) / 3000.0 );
			if ( fade < 255 ) {
				if ( fade < 0 ) {
					fade = 0;
				}
				if ( mp->verts[0].modulate[0] != 0 ) {
					for ( j = 0 ; j < mp->poly.numVerts ; j++ ) {
						mp->verts[j].modulate[0] = mp->color[0] * fade;
						mp->verts[j].modulate[1] = mp->color[1] * fade;
						mp->verts[j].modulate[2] = mp->color[2] * fade;
					}
				}
			}
		}

		// fade in the zombie spirit marks
		if ( mp->markShader == cgs.media.zombieSpiritWallShader ) {

			fade = 255 * ( ( cg.time - mp->time ) / 2000.0 );
			if ( fade < 255 ) {
				if ( fade < 0 ) {
					fade = 0;
				}
				if ( mp->verts[0].modulate[0] != 0 ) {
					for ( j = 0 ; j < mp->poly.numVerts ; j++ ) {
						mp->verts[j].modulate[0] = mp->color[0] * fade;
						mp->verts[j].modulate[1] = mp->color[1] * fade;
						mp->verts[j].modulate[2] = mp->color[2] * fade;
					}
				}
			}
		}

		// fade all marks out with time
		t = mp->time + mp->duration - cg.time;
		if ( t < (float)mp->duration / 2.0 ) {
			fade = (int)( 255.0 * (float)t / ( (float)mp->duration / 2.0 ) );
			if ( mp->alphaFade ) {
				for ( j = 0 ; j < mp->poly.numVerts ; j++ ) {
					mp->verts[j].modulate[3] = fade;
				}
			} else {
				for ( j = 0 ; j < mp->poly.numVerts ; j++ ) {
					mp->verts[j].modulate[0] = mp->color[0] * fade;
					mp->verts[j].modulate[1] = mp->color[1] * fade;
					mp->verts[j].modulate[2] = mp->color[2] * fade;
				}
			}
		}

		trap_R_AddPolyToScene( mp->markShader, mp->poly.numVerts, mp->verts );
	}
	cg_marksDrawn = drawn;
}

