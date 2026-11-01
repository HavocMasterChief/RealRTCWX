/*
===========================================================================
cg_ragdoll.c -- ragdoll deaths (RealRTCWX)

When a (human) character dies, its skeleton stops following the death
animation and becomes a jointed body that falls, tumbles and collides with
the world.

  - 15 joints (pelvis, chest, head, shoulders, elbows, hands, hips, knees,
    feet) are read from the skeleton at the moment of death (Bip01 bones,
    shared by all RTCW characters) and simulated as Verlet particles held
    together by distance constraints, with a few braces to keep the torso
    rigid and loose limits so limbs don't fold through the body.
  - The body is split into 11 rigid parts (pelvis, chest, head, upper/lower
    arms, thighs, calves). Each frame every part's rotation and position is
    worked out from its joints and sent to the renderer, which moves every
    bone of that part with it (see R_ApplyRagdoll in tr_animation.c). Heads,
    helmets and gear are attached to tags, so they follow.
  - The death animation pose at the moment of death is frozen and used as
    the "rest" pose the parts are rotated from.
  - Purely visual: the server still has the body where it thinks it is.

Bodies that settle stop simulating. cg_ragdoll 0 turns ragdolls off.

This file is part of RealRTCW and is licensed under the GPL v3.
===========================================================================
*/

#include "cg_local.h"

#define RD_MAX          16      // renderer slots (RD_MAX_SLOTS in tr_animation.c)
#define RD_SEGS         11
#define RD_STEP         ( 1.0f / 60.0f )
#define RD_ITER         8
#define RD_GRAVITY      800.0f

enum {
	J_PELVIS, J_CHEST, J_HEAD,
	J_LSHOULDER, J_LELBOW, J_LHAND,
	J_RSHOULDER, J_RELBOW, J_RHAND,
	J_LHIP, J_LKNEE, J_LFOOT,
	J_RHIP, J_RKNEE, J_RFOOT,
	J_NUM
};

static const char *rdBoneNames[J_NUM] = {
	"Bip01 Pelvis", "Bip01 Spine3", "Bip01 Head",
	"Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand",
	"Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand",
	"Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot",
	"Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot"
};

static const float rdRadius[J_NUM] = {
	5, 5, 5,
	3, 2.5f, 2.5f,
	3, 2.5f, 2.5f,
	3.5f, 3, 3,
	3.5f, 3, 3
};

// rigid links (bones), then braces that keep the torso a solid box
static const int rdSticks[][2] = {
	{ J_PELVIS, J_CHEST }, { J_CHEST, J_HEAD },
	{ J_CHEST, J_LSHOULDER }, { J_CHEST, J_RSHOULDER }, { J_LSHOULDER, J_RSHOULDER },
	{ J_LSHOULDER, J_LELBOW }, { J_LELBOW, J_LHAND },
	{ J_RSHOULDER, J_RELBOW }, { J_RELBOW, J_RHAND },
	{ J_PELVIS, J_LHIP }, { J_PELVIS, J_RHIP }, { J_LHIP, J_RHIP },
	{ J_LHIP, J_LKNEE }, { J_LKNEE, J_LFOOT },
	{ J_RHIP, J_RKNEE }, { J_RKNEE, J_RFOOT },
	// braces
	{ J_PELVIS, J_LSHOULDER }, { J_PELVIS, J_RSHOULDER },
	{ J_CHEST, J_LHIP }, { J_CHEST, J_RHIP },
	{ J_LSHOULDER, J_RHIP }, { J_RSHOULDER, J_LHIP },
	{ J_HEAD, J_LSHOULDER }, { J_HEAD, J_RSHOULDER }
};
#define RD_NUM_STICKS ( sizeof( rdSticks ) / sizeof( rdSticks[0] ) )

// loose joint limits: these pairs may get closer than their rest distance,
// but not closer than this fraction of it (no limbs folding into the body)
static const struct { int a, b; float frac; } rdLimits[] = {
	{ J_LSHOULDER, J_LHAND, 0.35f }, { J_RSHOULDER, J_RHAND, 0.35f },
	{ J_LHIP, J_LFOOT, 0.55f }, { J_RHIP, J_RFOOT, 0.55f },
	{ J_LHAND, J_PELVIS, 0.25f }, { J_RHAND, J_PELVIS, 0.25f },
	{ J_LFOOT, J_RFOOT, 0.2f }, { J_LKNEE, J_RKNEE, 0.3f },
	{ J_HEAD, J_PELVIS, 0.85f }, { J_LFOOT, J_CHEST, 0.6f }, { J_RFOOT, J_CHEST, 0.6f }
};
#define RD_NUM_LIMITS ( sizeof( rdLimits ) / sizeof( rdLimits[0] ) )

// body parts: anchor joint, primary axis (from -> to), secondary helper (from -> to)
static const struct { int anchor, p0, p1, s0, s1; } rdSegs[RD_SEGS] = {
	{ J_PELVIS,    J_PELVIS,    J_CHEST,  J_LHIP,      J_RHIP },       // 0 pelvis
	{ J_CHEST,     J_PELVIS,    J_CHEST,  J_LSHOULDER, J_RSHOULDER },  // 1 chest
	{ J_HEAD,      J_CHEST,     J_HEAD,   J_LSHOULDER, J_RSHOULDER },  // 2 head
	{ J_LSHOULDER, J_LSHOULDER, J_LELBOW, J_LSHOULDER, J_RSHOULDER },  // 3 L upper arm
	{ J_LELBOW,    J_LELBOW,    J_LHAND,  J_LSHOULDER, J_RSHOULDER },  // 4 L forearm
	{ J_RSHOULDER, J_RSHOULDER, J_RELBOW, J_LSHOULDER, J_RSHOULDER },  // 5 R upper arm
	{ J_RELBOW,    J_RELBOW,    J_RHAND,  J_LSHOULDER, J_RSHOULDER },  // 6 R forearm
	{ J_LHIP,      J_LHIP,      J_LKNEE,  J_LHIP,      J_RHIP },       // 7 L thigh
	{ J_LKNEE,     J_LKNEE,     J_LFOOT,  J_LHIP,      J_RHIP },       // 8 L calf
	{ J_RHIP,      J_RHIP,      J_RKNEE,  J_LHIP,      J_RHIP },       // 9 R thigh
	{ J_RKNEE,     J_RKNEE,     J_RFOOT,  J_LHIP,      J_RHIP }        // 10 R calf
};

typedef struct {
	qboolean active;
	int entityNum;
	int slot;               // renderer slot 1..RD_MAX
	int startTime, lastSeen;
	qhandle_t hModel;
	refEntity_t pose;       // frozen death-animation pose + placement
	vec3_t rest[J_NUM];     // joints in model space, frozen pose
	vec3_t p[J_NUM], old[J_NUM];   // simulation, world space
	float stick[RD_NUM_STICKS];
	float limit[RD_NUM_LIMITS];
	float accum;
	int stillSteps;
	qboolean sleeping;
	float data[RD_SEGS][12];
} ragdoll_t;

static ragdoll_t rdBodies[RD_MAX];
static int rdStamp;

/*
===============
CG_RagdollReset
===============
*/
void CG_RagdollReset( void ) {
	int i;
	for ( i = 0; i < RD_MAX; i++ ) {
		if ( rdBodies[i].active ) {
			trap_R_SetRagdoll( rdBodies[i].slot, NULL );
		}
	}
	memset( rdBodies, 0, sizeof( rdBodies ) );
}

static ragdoll_t *CG_RagdollFind( int entityNum ) {
	int i;
	for ( i = 0; i < RD_MAX; i++ ) {
		if ( rdBodies[i].active && rdBodies[i].entityNum == entityNum ) {
			return &rdBodies[i];
		}
	}
	return NULL;
}

qboolean CG_RagdollPelvis( int entityNum, vec3_t out ) {
	ragdoll_t *rd = CG_RagdollFind( entityNum );
	if ( !rd ) {
		return qfalse;
	}
	VectorCopy( rd->p[J_PELVIS], out );
	return qtrue;
}

static qboolean CG_RagdollEligible( centity_t *cent ) {
	if ( !cg_ragdoll.integer || cent->currentState.number == cg.snap->ps.clientNum ) {
		return qfalse;
	}
	switch ( cent->currentState.aiChar ) {
	case AICHAR_ZOMBIE:
	case AICHAR_ZOMBIE_SURV:
	case AICHAR_ZOMBIE_FLAME:
	case AICHAR_ZOMBIE_GHOST:
	case AICHAR_WARZOMBIE:
	case AICHAR_LOPER:
	case AICHAR_LOPER_SPECIAL:
	case AICHAR_HELGA:
	case AICHAR_HEINRICH:
	case AICHAR_PROTOSOLDIER:
	case AICHAR_SUPERSOLDIER:
	case AICHAR_SUPERSOLDIER_LAB:
		return qfalse;  // special deaths (burning, dissolving, bosses)
	default:
		return qtrue;
	}
}

static void CG_RagdollToWorld( const refEntity_t *ref, const vec3_t m, vec3_t out ) {
	int i;
	for ( i = 0; i < 3; i++ ) {
		out[i] = ref->origin[i] + m[0] * ref->axis[0][i] + m[1] * ref->axis[1][i] + m[2] * ref->axis[2][i];
	}
}

static void CG_RagdollToModel( const refEntity_t *ref, const vec3_t w, vec3_t out ) {
	vec3_t d;
	VectorSubtract( w, ref->origin, d );
	out[0] = DotProduct( d, ref->axis[0] );
	out[1] = DotProduct( d, ref->axis[1] );
	out[2] = DotProduct( d, ref->axis[2] );
}

/*
===============
CG_RagdollStart

Freeze the current pose and read the joints from it.
===============
*/
static ragdoll_t *CG_RagdollStart( centity_t *cent, const refEntity_t *body ) {
	ragdoll_t *rd = NULL;
	orientation_t o;
	refEntity_t pose;
	vec3_t push, toBody;
	int i, oldest = 0, slot;

	if ( body->nonNormalizedAxes ) {
		return NULL;    // scaled characters: the model space math assumes plain axes
	}
	pose = *body;
	pose.ragdoll = 0;
	pose.ragdollStamp = 0;

	// take a free body, or the oldest one
	for ( i = 0; i < RD_MAX; i++ ) {
		if ( !rdBodies[i].active ) {
			rd = &rdBodies[i];
			break;
		}
		if ( rdBodies[i].startTime < rdBodies[oldest].startTime ) {
			oldest = i;
		}
	}
	if ( !rd ) {
		rd = &rdBodies[oldest];
	}
	slot = (int)( rd - rdBodies ) + 1;

	// the joints, in model space, for the frozen pose
	for ( i = 0; i < J_NUM; i++ ) {
		if ( trap_R_LerpBone( &o, &pose, rdBoneNames[i] ) < 0 ) {
			return NULL;    // not a Bip01 skeleton (dogs, creatures): no ragdoll
		}
		VectorCopy( o.origin, rd->rest[i] );
	}

	memset( rd->data, 0, sizeof( rd->data ) );
	rd->active = qtrue;
	rd->entityNum = cent->currentState.number;
	rd->slot = slot;
	rd->startTime = rd->lastSeen = cg.time;
	rd->hModel = body->hModel;
	rd->pose = pose;
	rd->accum = 0;
	rd->stillSteps = 0;
	rd->sleeping = qfalse;

	// a push away from the player (most deaths come from you), plus the
	// body's own movement; upper body more, so it topples rather than slides
	VectorSubtract( cent->lerpOrigin, cg.refdef.vieworg, toBody );
	toBody[2] = 0;
	if ( VectorNormalize( toBody ) < 1 ) {
		VectorSet( toBody, 1, 0, 0 );
	}
	for ( i = 0; i < J_NUM; i++ ) {
		float k;
		CG_RagdollToWorld( &pose, rd->rest[i], rd->p[i] );
		switch ( i ) {
		case J_CHEST: case J_HEAD: case J_LSHOULDER: case J_RSHOULDER:
		case J_LELBOW: case J_RELBOW: case J_LHAND: case J_RHAND:
			k = 1.0f;
			break;
		case J_PELVIS: case J_LHIP: case J_RHIP:
			k = 0.45f;
			break;
		default:
			k = 0.1f;
			break;
		}
		VectorScale( toBody, ( 90 + random() * 70 ) * k, push );
		push[2] = 30 * k;
		VectorAdd( push, cent->currentState.pos.trDelta, push );
		VectorMA( rd->p[i], -RD_STEP, push, rd->old[i] );
	}
	for ( i = 0; i < (int)RD_NUM_STICKS; i++ ) {
		rd->stick[i] = Distance( rd->p[rdSticks[i][0]], rd->p[rdSticks[i][1]] );
	}
	for ( i = 0; i < (int)RD_NUM_LIMITS; i++ ) {
		rd->limit[i] = rdLimits[i].frac * Distance( rd->p[rdLimits[i].a], rd->p[rdLimits[i].b] );
	}
	return rd;
}

/*
===============
CG_RagdollStep

One fixed physics step.
===============
*/
static void CG_RagdollStep( ragdoll_t *rd ) {
	vec3_t start[J_NUM], v, d;
	float moved = 0;
	int i, it;

	// integrate
	for ( i = 0; i < J_NUM; i++ ) {
		VectorCopy( rd->p[i], start[i] );
		VectorSubtract( rd->p[i], rd->old[i], v );
		VectorScale( v, 0.995f, v );
		VectorCopy( rd->p[i], rd->old[i] );
		VectorAdd( rd->p[i], v, rd->p[i] );
		rd->p[i][2] -= RD_GRAVITY * RD_STEP * RD_STEP;
	}

	// constraints
	for ( it = 0; it < RD_ITER; it++ ) {
		for ( i = 0; i < (int)RD_NUM_STICKS; i++ ) {
			float *a = rd->p[rdSticks[i][0]], *b = rd->p[rdSticks[i][1]];
			float len, diff;
			VectorSubtract( b, a, d );
			len = VectorLength( d );
			if ( len < 0.001f ) {
				continue;
			}
			diff = ( len - rd->stick[i] ) / len * 0.5f;
			VectorMA( a, diff, d, a );
			VectorMA( b, -diff, d, b );
		}
		for ( i = 0; i < (int)RD_NUM_LIMITS; i++ ) {
			float *a = rd->p[rdLimits[i].a], *b = rd->p[rdLimits[i].b];
			float len, diff;
			VectorSubtract( b, a, d );
			len = VectorLength( d );
			if ( len < 0.001f || len >= rd->limit[i] ) {
				continue;
			}
			diff = ( len - rd->limit[i] ) / len * 0.5f;
			VectorMA( a, diff, d, a );
			VectorMA( b, -diff, d, b );
		}
	}

	// collide with the world
	for ( i = 0; i < J_NUM; i++ ) {
		trace_t tr;
		vec3_t mins, maxs;
		float r = rdRadius[i];

		VectorSet( mins, -r, -r, -r );
		VectorSet( maxs, r, r, r );
		CG_Trace( &tr, start[i], mins, maxs, rd->p[i], rd->entityNum, MASK_SOLID );
		if ( tr.allsolid ) {
			VectorCopy( start[i], rd->p[i] );
			VectorCopy( start[i], rd->old[i] );
		} else if ( tr.fraction < 1.0f ) {
			vec3_t vel, vn, vt;
			float dn;
			VectorSubtract( rd->p[i], start[i], vel );
			VectorMA( tr.endpos, 0.05f, tr.plane.normal, rd->p[i] );
			// friction: keep a bit of the sliding motion, none into the surface
			dn = DotProduct( vel, tr.plane.normal );
			VectorScale( tr.plane.normal, dn, vn );
			VectorSubtract( vel, vn, vt );
			VectorMA( rd->p[i], -0.55f, vt, rd->old[i] );
		}
		moved += Distance( rd->p[i], start[i] );
	}

	// settled?
	if ( moved < 0.15f ) {
		if ( ++rd->stillSteps > 45 ) {
			rd->sleeping = qtrue;
		}
	} else {
		rd->stillSteps = 0;
	}
}

// an orthonormal frame (axes as rows) from a primary and a helper direction
static void CG_RagdollFrame( const vec3_t prim, const vec3_t helper, const vec3_t helper2, vec3_t f[3] ) {
	vec3_t h;
	VectorNormalize2( prim, f[0] );
	CrossProduct( f[0], helper, f[2] );
	if ( VectorNormalize( f[2] ) < 0.05f * VectorLength( helper ) ) {
		CrossProduct( f[0], helper2, f[2] );
		VectorNormalize( f[2] );
	}
	CrossProduct( f[2], f[0], h );
	VectorCopy( h, f[1] );
}

/*
===============
CG_RagdollPose

Turn the joints into a rotation + translation per body part, in model space.
===============
*/
static void CG_RagdollPose( ragdoll_t *rd ) {
	vec3_t cur[J_NUM];
	int s, i, j, k;

	for ( i = 0; i < J_NUM; i++ ) {
		CG_RagdollToModel( &rd->pose, rd->p[i], cur[i] );
	}
	for ( s = 0; s < RD_SEGS; s++ ) {
		vec3_t pr, hr, h2r, pc, hc, h2c, fr[3], fc[3], ra;
		float *d = rd->data[s];
		float R[3][3];

		VectorSubtract( rd->rest[rdSegs[s].p1], rd->rest[rdSegs[s].p0], pr );
		VectorSubtract( rd->rest[rdSegs[s].s1], rd->rest[rdSegs[s].s0], hr );
		VectorSubtract( rd->rest[J_CHEST], rd->rest[J_PELVIS], h2r );
		VectorSubtract( cur[rdSegs[s].p1], cur[rdSegs[s].p0], pc );
		VectorSubtract( cur[rdSegs[s].s1], cur[rdSegs[s].s0], hc );
		VectorSubtract( cur[J_CHEST], cur[J_PELVIS], h2c );
		CG_RagdollFrame( pr, hr, h2r, fr );
		CG_RagdollFrame( pc, hc, h2c, fc );

		// R = Fcur * Frest^T  (frames with the axes as columns)
		for ( i = 0; i < 3; i++ ) {
			for ( j = 0; j < 3; j++ ) {
				R[i][j] = 0;
				for ( k = 0; k < 3; k++ ) {
					R[i][j] += fc[k][i] * fr[k][j];
				}
				d[i * 3 + j] = R[i][j];
			}
		}
		// t = anchor_now - R * anchor_rest
		for ( i = 0; i < 3; i++ ) {
			ra[i] = R[i][0] * rd->rest[rdSegs[s].anchor][0] + R[i][1] * rd->rest[rdSegs[s].anchor][1] +
					R[i][2] * rd->rest[rdSegs[s].anchor][2];
			d[9 + i] = cur[rdSegs[s].anchor][i] - ra[i];
		}
	}
}

/*
===============
CG_RagdollBody

Called for every skeletal character just before it's added to the scene.
For dead (eligible) characters it runs the ragdoll and replaces the pose.
===============
*/
void CG_RagdollBody( centity_t *cent, refEntity_t *body ) {
	ragdoll_t *rd;
	float dt;

	rd = CG_RagdollFind( cent->currentState.number );
	if ( !( cent->currentState.eFlags & EF_DEAD ) || !CG_RagdollEligible( cent ) ) {
		if ( rd ) {     // revived / respawned / turned off
			trap_R_SetRagdoll( rd->slot, NULL );
			rd->active = qfalse;
		}
		return;
	}
	if ( rd && rd->hModel != body->hModel ) {
		rd->active = qfalse;
		rd = NULL;
	}
	if ( !rd ) {
		rd = CG_RagdollStart( cent, body );
		if ( !rd ) {
			return;
		}
	}
	rd->lastSeen = cg.time;

	// simulate (fixed steps, at most a few per frame)
	if ( !rd->sleeping ) {
		dt = cg.frametime * 0.001f;
		if ( dt > 0.1f ) {
			dt = 0.1f;
		}
		rd->accum += dt;
		while ( rd->accum >= RD_STEP && !rd->sleeping ) {
			CG_RagdollStep( rd );
			rd->accum -= RD_STEP;
		}
		CG_RagdollPose( rd );
	}
	trap_R_SetRagdoll( rd->slot, &rd->data[0][0] );

	// draw the frozen pose, bent by the ragdoll
	body->frame = rd->pose.frame;
	body->oldframe = rd->pose.oldframe;
	body->backlerp = rd->pose.backlerp;
	body->torsoFrame = rd->pose.torsoFrame;
	body->oldTorsoFrame = rd->pose.oldTorsoFrame;
	body->torsoBacklerp = rd->pose.torsoBacklerp;
	memcpy( body->axis, rd->pose.axis, sizeof( body->axis ) );
	memcpy( body->torsoAxis, rd->pose.torsoAxis, sizeof( body->torsoAxis ) );
	VectorCopy( rd->pose.origin, body->origin );
	VectorCopy( rd->pose.origin, body->oldorigin );
	VectorCopy( rd->p[J_PELVIS], body->lightingOrigin );
	body->renderfx |= RF_LIGHTING_ORIGIN;
	body->ragdoll = rd->slot;
	body->ragdollStamp = ++rdStamp;
}
