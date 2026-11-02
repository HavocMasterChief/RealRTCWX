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

#define RD_MAX          128     // bodies kept in their ragdoll pose (RD_MAX_SLOTS in tr_animation.c)
#define RD_MAX_SIM      32      // of those, how many may still be moving; past that the oldest
								// moving one is locked in the pose it has
#define RD_SEGS         11
#define RD_STEP         ( 1.0f / 60.0f )
#define RD_ITER         8
#define RD_GRAVITY      386.0f  // real gravity: 1 unit ~ 1 inch (the game's 800 is for jumping)
#define RD_AIRDRAG      0.988f  // velocity kept per step
// "muscle tone": joints are pulled toward the death pose (relative to the
// pelvis) so limbs don't flop like rope; strong at first, fading to a light
// stiffness that stays
#define RD_MUSCLE_START 0.10f
#define RD_MUSCLE_END   0.0f    // fully limp once it fades (arms used to stay held out)
#define RD_MUSCLE_FADE  1.2f    // seconds
#define RD_HIT_WINDOW   1200    // ms: a hit this recent shapes the fall

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
	{ J_LHIP, J_LFOOT, 0.7f }, { J_RHIP, J_RFOOT, 0.7f },       // knees can't fold flat
	{ J_LKNEE, J_CHEST, 0.75f }, { J_RKNEE, J_CHEST, 0.75f },   // thighs can't fold into the chest
	{ J_LFOOT, J_PELVIS, 0.6f }, { J_RFOOT, J_PELVIS, 0.6f },   // no sitting on the heels
	{ J_LHAND, J_PELVIS, 0.25f }, { J_RHAND, J_PELVIS, 0.25f },
	{ J_LFOOT, J_RFOOT, 0.2f }, { J_LKNEE, J_RKNEE, 0.3f },
	{ J_HEAD, J_PELVIS, 0.85f }, { J_LFOOT, J_CHEST, 0.6f }, { J_RFOOT, J_CHEST, 0.6f }
};
#define RD_NUM_LIMITS ( sizeof( rdLimits ) / sizeof( rdLimits[0] ) )

// capsules for self-collision (limbs can't pass through each other or the
// torso); radius in units
static const struct { int a, b; float r; } rdCaps[] = {
	{ J_PELVIS, J_CHEST, 7.0f },        // 0 torso
	{ J_HEAD, J_HEAD, 5.0f },           // 1 head
	{ J_LSHOULDER, J_LELBOW, 2.8f },    // 2
	{ J_LELBOW, J_LHAND, 2.5f },        // 3
	{ J_RSHOULDER, J_RELBOW, 2.8f },    // 4
	{ J_RELBOW, J_RHAND, 2.5f },        // 5
	{ J_LHIP, J_LKNEE, 3.6f },          // 6
	{ J_LKNEE, J_LFOOT, 3.0f },         // 7
	{ J_RHIP, J_RKNEE, 3.6f },          // 8
	{ J_RKNEE, J_RFOOT, 3.0f }          // 9
};
#define RD_NUM_CAPS ( sizeof( rdCaps ) / sizeof( rdCaps[0] ) )
#define RD_MAX_PAIRS 48

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
	int wound;              // joint nearest the killing shot (blood comes from here)
	int groundEnt;          // mover it's lying on (-1: none), carried along with it
	vec3_t groundOrg;
	int lastThud;
	vec3_t lastSmear;
	qhandle_t hModel;
	refEntity_t pose;       // frozen death-animation pose + placement
	vec3_t rest[J_NUM];     // joints in model space, frozen pose
	vec3_t p[J_NUM], old[J_NUM];   // simulation, world space
	float stick[RD_NUM_STICKS];
	float limit[RD_NUM_LIMITS];
	qboolean hingeOn[4];    // L knee, R knee, L elbow, R elbow
	vec3_t limbNRest[4];    // bend-plane normal of each limb (L arm, R arm, L leg, R leg), model space
	vec3_t limbN[4];        // same, tracked as the body moves (keeps the twist when a limb straightens)
	int numPairs;
	struct { short a, b; float minDist; } pair[RD_MAX_PAIRS];   // capsule pairs that collide
	float accum;
	int steps;
	vec3_t restFrame[3];    // pelvis frame of the frozen pose (model space)
	int stillSteps;
	int awakeSteps;      // steps since it last woke (jittering bodies get put to sleep)
	float jitter;        // recent movement, smoothed
	qboolean sleeping;
	float data[RD_SEGS][12];
} ragdoll_t;

static ragdoll_t rdBodies[RD_MAX];
static byte rdDeclined[MAX_GENTITIES];   // dead bodies that keep their death animation
static void CG_RagdollInitPairs( ragdoll_t *rd );
static void CG_RagdollAddWound( int entityNum, const vec3_t point, const vec3_t dir );
static void CG_RagdollLoadSaved( void );
static void CG_RagdollPose( ragdoll_t *rd );

// ragdoll poses kept with savegames (save/<name>.rag)
#define RD_SAVE_MAGIC   0x31474152  // "RAG1"
typedef struct {
	int entityNum, aiChar, wound;
	int frame, oldframe, torsoFrame, oldTorsoFrame;
	float backlerp, torsoBacklerp;
	vec3_t axis[3], torsoAxis[3], origin;
	vec3_t p[J_NUM];
} rdSaved_t;
static rdSaved_t rdSaved[RD_MAX];
static char rdLastSave[256];     // the last g_lastSave we answered
static int rdNumSaved;
static qboolean rdSavedUsed[RD_MAX];
static int rdStamp;

// the last bullet that hit each entity (where, which way, when)
static struct {
	int time;
	vec3_t point, dir;
} rdHits[MAX_GENTITIES];

/*
===============
CG_RagdollNoteHit

Called for every bullet that hits flesh, so a body that dies from it can
be thrown the way the shot went, from where it struck.
===============
*/
void CG_RagdollNoteHit( int entityNum, const vec3_t point, int shooter ) {
	vec3_t from, dir;
	if ( entityNum < 0 || entityNum >= MAX_GENTITIES ) {
		return;
	}
	if ( shooter == cg.snap->ps.clientNum ) {
		VectorCopy( cg.refdef.vieworg, from );
	} else if ( shooter >= 0 && shooter < MAX_GENTITIES ) {
		VectorCopy( cg_entities[shooter].lerpOrigin, from );
		from[2] += 40;  // roughly the gun's height
	} else {
		return;
	}
	VectorSubtract( point, from, dir );
	if ( VectorNormalize( dir ) < 1 ) {
		return;
	}
	rdHits[entityNum].time = cg.time;
	VectorCopy( point, rdHits[entityNum].point );
	VectorCopy( dir, rdHits[entityNum].dir );
	CG_RagdollAddWound( entityNum, point, dir );
}

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
	memset( rdHits, 0, sizeof( rdHits ) );
	memset( rdDeclined, 0, sizeof( rdDeclined ) );
	trap_R_AddWound( -1, NULL, NULL );
	CG_RagdollLoadSaved();
	// a save announced before this level started isn't ours to answer
	// (it would overwrite that save's poses with this level's empty set)
	trap_Cvar_VariableStringBuffer( "g_lastSave", rdLastSave, sizeof( rdLastSave ) );
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

// where a ragdolled body bleeds from: the joint the killing shot hit
qboolean CG_RagdollWound( int entityNum, vec3_t out ) {
	ragdoll_t *rd = CG_RagdollFind( entityNum );
	if ( !rd ) {
		return qfalse;
	}
	VectorCopy( rd->p[rd->wound], out );
	return qtrue;
}

qboolean CG_RagdollPelvis( int entityNum, vec3_t out ) {
	ragdoll_t *rd = CG_RagdollFind( entityNum );
	if ( !rd ) {
		return qfalse;
	}
	VectorCopy( rd->p[J_PELVIS], out );
	return qtrue;
}

// characters that bleed (and get bullet wounds drawn on them)
static qboolean CG_RagdollBleeds( int aiChar ) {
	switch ( aiChar ) {
	case AICHAR_ZOMBIE:
	case AICHAR_ZOMBIE_SURV:
	case AICHAR_ZOMBIE_FLAME:
	case AICHAR_ZOMBIE_GHOST:
	case AICHAR_WARZOMBIE:
	case AICHAR_LOPER:
	case AICHAR_LOPER_SPECIAL:
	case AICHAR_PROTOSOLDIER:
	case AICHAR_SUPERSOLDIER:
	case AICHAR_SUPERSOLDIER_LAB:
		return qfalse;
	default:
		return qtrue;
	}
}

// zombies: bullet holes, but dry (no blood soaking the skin)
static qboolean CG_RagdollDryWounds( int aiChar ) {
	switch ( aiChar ) {
	case AICHAR_ZOMBIE:
	case AICHAR_ZOMBIE_SURV:
	case AICHAR_ZOMBIE_FLAME:
	case AICHAR_ZOMBIE_GHOST:
	case AICHAR_WARZOMBIE:
		return qtrue;
	default:
		return qfalse;
	}
}

static void CG_RagdollAddWound( int entityNum, const vec3_t point, const vec3_t dir ) {
	int ai;
	if ( entityNum < 0 || entityNum >= MAX_CLIENTS || entityNum == cg.snap->ps.clientNum ||
		 !cg_blood.integer || cg_bloodMode.integer == BLOODMODE_OFF ) {
		return;
	}
	ai = cg_entities[entityNum].currentState.aiChar;
	if ( !CG_RagdollBleeds( ai ) && !CG_RagdollDryWounds( ai ) ) {
		return;
	}
	trap_R_AddWound( entityNum, point, dir );
}

static qboolean CG_RagdollEligible( centity_t *cent ) {
	if ( !cg_ragdoll.integer || cent->currentState.number == cg.snap->ps.clientNum ) {
		return qfalse;
	}
	switch ( cent->currentState.aiChar ) {
	case AICHAR_LOPER:
	case AICHAR_LOPER_SPECIAL:
	case AICHAR_HELGA:
	case AICHAR_HEINRICH:
	case AICHAR_PROTOSOLDIER:
	case AICHAR_SUPERSOLDIER:
	case AICHAR_SUPERSOLDIER_LAB:
		return qfalse;  // special deaths (bosses, lopers, armoured soldiers)
	default:
		return qtrue;
	}
}

static void CG_RagdollFrame( const vec3_t prim, const vec3_t helper, const vec3_t helper2, vec3_t f[3] );

// the pelvis frame (axes as rows) from joint positions
static void CG_RagdollPelvisFrame( vec3_t j[J_NUM], vec3_t f[3] ) {
	vec3_t prim, side, fwd;
	VectorSubtract( j[J_CHEST], j[J_PELVIS], prim );
	VectorSubtract( j[J_RHIP], j[J_LHIP], side );
	VectorSubtract( j[J_RSHOULDER], j[J_LSHOULDER], fwd );
	CG_RagdollFrame( prim, side, fwd, f );
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

// limbs: root, hinge, end joint, and the body parts that roll with them
static const struct { int a, k, b, seg0, seg1, side0, side1; } rdLimbs[4] = {
	{ J_LSHOULDER, J_LELBOW, J_LHAND, 3, 4, J_LSHOULDER, J_RSHOULDER },
	{ J_RSHOULDER, J_RELBOW, J_RHAND, 5, 6, J_LSHOULDER, J_RSHOULDER },
	{ J_LHIP, J_LKNEE, J_LFOOT, 7, 8, J_LHIP, J_RHIP },
	{ J_RHIP, J_RKNEE, J_RFOOT, 9, 10, J_LHIP, J_RHIP }
};

// a limb's bend-plane normal; when it's (nearly) straight there's no plane,
// so keep the previous normal, squared up to the upper segment
static void CG_RagdollLimbNormal( vec3_t j[J_NUM], int limb, vec3_t n ) {
	vec3_t u1, u2, c;
	float l1, l2, len;
	VectorSubtract( j[rdLimbs[limb].k], j[rdLimbs[limb].a], u1 );
	VectorSubtract( j[rdLimbs[limb].b], j[rdLimbs[limb].k], u2 );
	l1 = VectorNormalize( u1 );
	l2 = VectorNormalize( u2 );
	CrossProduct( u1, u2, c );
	len = VectorLength( c );
	if ( l1 > 0.01f && l2 > 0.01f && len > 0.12f ) {
		VectorScale( c, 1.0f / len, c );
		if ( DotProduct( c, n ) < 0 && VectorLengthSquared( n ) > 0.5f ) {
			VectorNegate( c, c );
		}
		VectorCopy( c, n );
		return;
	}
	if ( VectorLengthSquared( n ) < 0.5f ) {
		// no history: use the shoulder/hip line
		vec3_t side;
		VectorSubtract( j[rdLimbs[limb].side1], j[rdLimbs[limb].side0], side );
		CrossProduct( u1, side, n );
		CrossProduct( n, u1, n );   // perpendicular to the limb, in the side's direction
	}
	VectorMA( n, -DotProduct( n, u1 ), u1, n );
	if ( VectorNormalize( n ) < 0.01f ) {
		VectorSet( n, 0, 0, 1 );
	}
}

/*
===============
CG_RagdollStart

Freeze the current pose and read the joints from it.
===============
*/
// log (once per model) why a character can't have a ragdoll, so new
// skeletons (the dog) can be mapped. The renderer lists the model's bones.
static void CG_RagdollWhyNot( centity_t *cent, const refEntity_t *body, const char *why ) {
	static qhandle_t seen[32];
	static int numSeen;
	int i;
	for ( i = 0; i < numSeen; i++ ) {
		if ( seen[i] == body->hModel ) {
			return;
		}
	}
	if ( numSeen < 32 ) {
		seen[numSeen++] = body->hModel;
	}
	CG_Printf( "ragdoll: aiChar %i (model %i) gets no ragdoll: %s\n", cent->currentState.aiChar, body->hModel, why );
	{
		orientation_t o;
		trap_R_LerpBone( &o, body, "?list" );     // prints the skeleton
	}
}

static ragdoll_t *CG_RagdollStart( centity_t *cent, const refEntity_t *body ) {
	ragdoll_t *rd = NULL;
	orientation_t o;
	refEntity_t pose;
	vec3_t push, toBody, rest[J_NUM];
	int i, oldest = -1, slot;

	if ( body->nonNormalizedAxes ) {
		CG_RagdollWhyNot( cent, body, "scaled model" );
		return NULL;    // scaled characters: the model space math assumes plain axes
	}
	pose = *body;
	pose.ragdoll = 0;
	pose.ragdollStamp = 0;

	// the joints, in model space, for the frozen pose
	for ( i = 0; i < J_NUM; i++ ) {
		if ( trap_R_LerpBone( &o, &pose, rdBoneNames[i] ) < 0 ) {
			CG_RagdollWhyNot( cent, body, va( "no bone '%s'", rdBoneNames[i] ) );
			return NULL;    // not a Bip01 skeleton (dogs, creatures): no ragdoll
		}
		VectorCopy( o.origin, rest[i] );
	}

	// take a free body, else one whose corpse hasn't been in view for a
	// while. Never one that's still being drawn: stealing those made every
	// corpse in a crowded room restart its ragdoll each frame (flicker,
	// bodies snapping back to the death animation)
	for ( i = 0; i < RD_MAX; i++ ) {
		if ( !rdBodies[i].active ) {
			rd = &rdBodies[i];
			break;
		}
		if ( oldest < 0 || rdBodies[i].lastSeen < rdBodies[oldest].lastSeen ) {
			oldest = i;
		}
	}
	if ( !rd ) {
		if ( oldest < 0 || cg.time - rdBodies[oldest].lastSeen < 1000 ) {
			return NULL;    // all in use: this one keeps its death animation
		}
		trap_R_SetRagdoll( rdBodies[oldest].slot, NULL );
		rd = &rdBodies[oldest];
	}
	slot = (int)( rd - rdBodies ) + 1;

	// too many still moving: lock the oldest moving body where it is
	{
		int moving = 0, first = -1;
		for ( i = 0; i < RD_MAX; i++ ) {
			if ( &rdBodies[i] == rd || !rdBodies[i].active || rdBodies[i].sleeping ) {
				continue;
			}
			moving++;
			if ( first < 0 || rdBodies[i].startTime < rdBodies[first].startTime ) {
				first = i;
			}
		}
		if ( moving >= RD_MAX_SIM && first >= 0 ) {
			rdBodies[first].sleeping = qtrue;
		}
	}
	for ( i = 0; i < J_NUM; i++ ) {
		VectorCopy( rest[i], rd->rest[i] );
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
	rd->awakeSteps = 0;
	rd->jitter = 1.0f;
	rd->sleeping = qfalse;
	rd->groundEnt = -1;
	rd->lastThud = 0;
	rd->wound = J_PELVIS;
	VectorSet( rd->lastSmear, 99999, 99999, 99999 );

	rd->steps = 0;

	for ( i = 0; i < J_NUM; i++ ) {
		CG_RagdollToWorld( &pose, rd->rest[i], rd->p[i] );
	}
	CG_RagdollPelvisFrame( rd->rest, rd->restFrame );

	if ( cg.time - rdHits[rd->entityNum].time < RD_HIT_WINDOW && rdHits[rd->entityNum].time ) {
		// shot: the joints near the wound take the hit along the bullet's
		// path, the rest of the body follows a little (a head shot snaps
		// the head back, a leg shot takes the legs out)
		const float *hp = rdHits[rd->entityNum].point, *hd = rdHits[rd->entityNum].dir;
		float bestD = 1e9f;
		for ( i = 0; i < J_NUM; i++ ) {
			float dd = DistanceSquared( rd->p[i], hp );
			if ( dd < bestD ) {
				bestD = dd;
				rd->wound = i;
			}
		}
		for ( i = 0; i < J_NUM; i++ ) {
			float d2 = DistanceSquared( rd->p[i], hp );
			float w = exp( -d2 / ( 2.0f * 14.0f * 14.0f ) );
			VectorScale( hd, ( 45 + 190 * w ) * CG_XbThrowScale(), push );
			push[2] += ( 20 + 25 * w ) * CG_XbThrowScale();
			VectorAdd( push, cent->currentState.pos.trDelta, push );
			VectorMA( rd->p[i], -RD_STEP, push, rd->old[i] );
		}
	} else {
		// no recent bullet (explosion, melee...): a push away from the
		// player, upper body more so it topples rather than slides
		VectorSubtract( cent->lerpOrigin, cg.refdef.vieworg, toBody );
		toBody[2] = 0;
		if ( VectorNormalize( toBody ) < 1 ) {
			VectorSet( toBody, 1, 0, 0 );
		}
		for ( i = 0; i < J_NUM; i++ ) {
			float k;
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
			VectorScale( toBody, ( 70 + random() * 50 ) * k, push );
			push[2] = 25 * k;
			VectorAdd( push, cent->currentState.pos.trDelta, push );
			VectorMA( rd->p[i], -RD_STEP, push, rd->old[i] );
		}
	}
	for ( i = 0; i < (int)RD_NUM_STICKS; i++ ) {
		rd->stick[i] = Distance( rd->p[rdSticks[i][0]], rd->p[rdSticks[i][1]] );
	}
	for ( i = 0; i < (int)RD_NUM_LIMITS; i++ ) {
		rd->limit[i] = rdLimits[i].frac * Distance( rd->p[rdLimits[i].a], rd->p[rdLimits[i].b] );
	}
	CG_RagdollInitPairs( rd );
	memset( rd->hingeOn, 0, sizeof( rd->hingeOn ) );
	for ( i = 0; i < 4; i++ ) {
		VectorClear( rd->limbNRest[i] );
		CG_RagdollLimbNormal( rd->rest, i, rd->limbNRest[i] );
		VectorCopy( rd->limbNRest[i], rd->limbN[i] );
	}
	return rd;
}

// closest points between segments p1-q1 and p2-q2: fractions s, t along them
static float CG_SegSegDist( const vec3_t p1, const vec3_t q1, const vec3_t p2, const vec3_t q2,
							float *s, float *t, vec3_t c1, vec3_t c2 ) {
	vec3_t d1, d2, r, diff;
	float a, e, f, b, c, denom;
	VectorSubtract( q1, p1, d1 );
	VectorSubtract( q2, p2, d2 );
	VectorSubtract( p1, p2, r );
	a = DotProduct( d1, d1 );
	e = DotProduct( d2, d2 );
	f = DotProduct( d2, r );
	if ( a < 1e-4f && e < 1e-4f ) {
		*s = *t = 0;
	} else if ( a < 1e-4f ) {
		*s = 0;
		*t = Com_Clamp( 0, 1, f / e );
	} else {
		c = DotProduct( d1, r );
		if ( e < 1e-4f ) {
			*t = 0;
			*s = Com_Clamp( 0, 1, -c / a );
		} else {
			b = DotProduct( d1, d2 );
			denom = a * e - b * b;
			*s = denom > 1e-6f ? Com_Clamp( 0, 1, ( b * f - c * e ) / denom ) : 0;
			*t = ( b * *s + f ) / e;
			if ( *t < 0 ) {
				*t = 0;
				*s = Com_Clamp( 0, 1, -c / a );
			} else if ( *t > 1 ) {
				*t = 1;
				*s = Com_Clamp( 0, 1, ( b - c ) / a );
			}
		}
	}
	VectorMA( p1, *s, d1, c1 );
	VectorMA( p2, *t, d2, c2 );
	VectorSubtract( c1, c2, diff );
	return VectorLength( diff );
}

// pick the capsule pairs that can collide: not sharing a joint, not the
// limbs' roots against the torso; the allowed gap never exceeds what the
// death pose already has (so a pose that starts tucked doesn't explode)
static void CG_RagdollInitPairs( ragdoll_t *rd ) {
	int i, j;
	float s, t, d;
	vec3_t c1, c2;
	rd->numPairs = 0;
	for ( i = 0; i < (int)RD_NUM_CAPS; i++ ) {
		for ( j = i + 1; j < (int)RD_NUM_CAPS; j++ ) {
			if ( rdCaps[i].a == rdCaps[j].a || rdCaps[i].a == rdCaps[j].b ||
				 rdCaps[i].b == rdCaps[j].a || rdCaps[i].b == rdCaps[j].b ) {
				continue;
			}
			if ( i == 0 && ( j == 2 || j == 4 || j == 6 || j == 8 ) ) {
				continue;   // upper arms and thighs are attached to the torso
			}
			if ( i == 1 && ( j == 2 || j == 4 ) ) {
				continue;   // head vs upper arms: shoulders sit right under it
			}
			if ( rd->numPairs >= RD_MAX_PAIRS ) {
				return;
			}
			d = CG_SegSegDist( rd->p[rdCaps[i].a], rd->p[rdCaps[i].b],
							   rd->p[rdCaps[j].a], rd->p[rdCaps[j].b], &s, &t, c1, c2 );
			rd->pair[rd->numPairs].a = i;
			rd->pair[rd->numPairs].b = j;
			rd->pair[rd->numPairs].minDist = MIN( rdCaps[i].r + rdCaps[j].r, d * 0.9f );
			rd->numPairs++;
		}
	}
}

// push overlapping capsules apart, spreading the move over their end joints
static void CG_RagdollSelfCollide( ragdoll_t *rd ) {
	int n;
	for ( n = 0; n < rd->numPairs; n++ ) {
		int ci = rd->pair[n].a, cj = rd->pair[n].b;
		float *a1 = rd->p[rdCaps[ci].a], *b1 = rd->p[rdCaps[ci].b];
		float *a2 = rd->p[rdCaps[cj].a], *b2 = rd->p[rdCaps[cj].b];
		float s, t, d, pen, w1a, w1b, w2a, w2b, sum;
		vec3_t c1, c2, dir;
		d = CG_SegSegDist( a1, b1, a2, b2, &s, &t, c1, c2 );
		pen = rd->pair[n].minDist - d;
		if ( pen <= 0 ) {
			continue;
		}
		if ( d > 0.001f ) {
			VectorSubtract( c1, c2, dir );
			VectorScale( dir, 1.0f / d, dir );
		} else {
			VectorSet( dir, 0, 0, 1 );
		}
		// weights per end joint (a degenerate capsule has a == b)
		w1a = 1 - s; w1b = s; w2a = 1 - t; w2b = t;
		sum = w1a * w1a + w1b * w1b + w2a * w2a + w2b * w2b;
		if ( sum < 1e-4f ) {
			continue;
		}
		pen /= sum;
		if ( a1 == b1 ) {
			VectorMA( a1, pen, dir, a1 );
		} else {
			VectorMA( a1, pen * w1a, dir, a1 );
			VectorMA( b1, pen * w1b, dir, b1 );
		}
		if ( a2 == b2 ) {
			VectorMA( a2, -pen, dir, a2 );
		} else {
			VectorMA( a2, -pen * w2a, dir, a2 );
			VectorMA( b2, -pen * w2b, dir, b2 );
		}
	}
}

// hinge: joint k (knee/elbow) between a and b may only bend toward
// bendDir (knees forward, elbows back); a reversed bend is pushed straight.
// A hinge the death pose already has reversed is left alone until it
// first bends the right way (no violent snap on the first frame).
static void CG_RagdollHinge( ragdoll_t *rd, int a, int k, int b, const vec3_t bendDir, qboolean *on ) {
	float *pa = rd->p[a], *pk = rd->p[k], *pb = rd->p[b];
	vec3_t u, o, off, bp;
	float len, c, corr;
	VectorSubtract( pb, pa, u );
	len = VectorNormalize( u );
	if ( len < 0.01f ) {
		return;
	}
	VectorSubtract( pk, pa, o );
	VectorMA( o, -DotProduct( o, u ), u, off );     // knee's offset from the a-b line
	VectorMA( bendDir, -DotProduct( bendDir, u ), u, bp );
	if ( VectorNormalize( bp ) < 0.15f ) {
		return;     // limb points along bendDir: no meaningful bend side
	}
	c = DotProduct( off, bp );
	if ( c >= 0.25f ) {
		*on = qtrue;
		return;
	}
	if ( !*on ) {
		return;
	}
	corr = 0.25f - c;
	VectorMA( pk, corr * ( 2.0f / 3.0f ), bp, pk );
	VectorMA( pa, -corr / 3.0f, bp, pa );
	VectorMA( pb, -corr / 3.0f, bp, pb );
}

static void CG_RagdollJointLimits( ragdoll_t *rd ) {
	vec3_t up, right, fwd, chestUp, shoulders, chestFwd, back;
	// Quake axes: forward = up x right
	VectorSubtract( rd->p[J_CHEST], rd->p[J_PELVIS], up );
	VectorSubtract( rd->p[J_RHIP], rd->p[J_LHIP], right );
	CrossProduct( up, right, fwd );
	if ( VectorNormalize( fwd ) > 0.01f ) {
		CG_RagdollHinge( rd, J_LHIP, J_LKNEE, J_LFOOT, fwd, &rd->hingeOn[0] );
		CG_RagdollHinge( rd, J_RHIP, J_RKNEE, J_RFOOT, fwd, &rd->hingeOn[1] );
	}
	VectorSubtract( rd->p[J_HEAD], rd->p[J_CHEST], chestUp );
	VectorSubtract( rd->p[J_RSHOULDER], rd->p[J_LSHOULDER], shoulders );
	CrossProduct( chestUp, shoulders, chestFwd );
	if ( VectorNormalize( chestFwd ) > 0.01f ) {
		VectorNegate( chestFwd, back );
		CG_RagdollHinge( rd, J_LSHOULDER, J_LELBOW, J_LHAND, back, &rd->hingeOn[2] );
		CG_RagdollHinge( rd, J_RSHOULDER, J_RELBOW, J_RHAND, back, &rd->hingeOn[3] );
	}
}

// one pass over every joint constraint
static void CG_RagdollConstrain( ragdoll_t *rd ) {
	vec3_t d;
	int i;

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
	CG_RagdollJointLimits( rd );
	CG_RagdollSelfCollide( rd );
}

// move each joint from where it was this step to where it wants to be,
// stopping at the world; sliding keeps most of its motion (bodies slide
// and tumble off steps and edges instead of sticking to them)
static void CG_RagdollCollide( ragdoll_t *rd, vec3_t start[J_NUM] ) {
	int i;
	for ( i = 0; i < J_NUM; i++ ) {
		trace_t tr;
		vec3_t mins, maxs;
		float r = rdRadius[i];

		VectorSet( mins, -r, -r, -r );
		VectorSet( maxs, r, r, r );
		CG_Trace( &tr, start[i], mins, maxs, rd->p[i], rd->entityNum, MASK_SOLID );
		if ( tr.allsolid ) {
			// started inside (a step corner): stay put this step, but keep
			// half the motion so the constraints can work it loose
			vec3_t vel;
			VectorSubtract( rd->p[i], start[i], vel );
			VectorCopy( start[i], rd->p[i] );
			VectorMA( start[i], -0.5f, vel, rd->old[i] );
		} else if ( tr.fraction < 1.0f ) {
			vec3_t vel, vn, vt;
			float dn, keep;
			VectorSubtract( rd->p[i], start[i], vel );
			VectorMA( tr.endpos, 0.05f, tr.plane.normal, rd->p[i] );
			dn = DotProduct( vel, tr.plane.normal );
			VectorScale( tr.plane.normal, dn, vn );
			VectorSubtract( vel, vn, vt );
			// floors grip, edges / step risers / slopes let it slide
			keep = tr.plane.normal[2] > 0.9f ? 0.7f : 0.92f;
			// a bleeding body sliding along the floor smears blood behind it
			if ( ( i == J_PELVIS || i == J_CHEST || i == rd->wound ) && tr.plane.normal[2] > 0.6f &&
				 VectorLength( vt ) > 0.8f && cg.time - rd->startTime < 25000 &&
				 cg_blood.integer && cg_bloodMode.integer != BLOODMODE_OFF &&
				 CG_RagdollBleeds( cg_entities[rd->entityNum].currentState.aiChar ) &&
				 Distance( tr.endpos, rd->lastSmear ) > 7.0f ) {
				VectorCopy( tr.endpos, rd->lastSmear );
				CG_ImpactMark( CG_BloodDot(), tr.endpos, tr.plane.normal, random() * 360,
							   1, 1, 1, 1, qfalse, 5 + random() * 4, qfalse, -1 );
			}
			// a heavy part landing hard: thud
			if ( ( i == J_PELVIS || i == J_CHEST || i == J_HEAD ) && -dn > 3.0f &&
				 cg.time - rd->lastThud > 250 ) {
				int vol = (int)( ( -dn - 3.0f ) * 40.0f + 60.0f );
				rd->lastThud = cg.time;
				trap_S_StartSoundVControl( tr.endpos, ENTITYNUM_WORLD, CHAN_AUTO, cgs.media.landSound,
										   vol > 255 ? 255 : vol );
			}
			VectorMA( rd->p[i], -keep, vt, rd->old[i] );
			VectorCopy( rd->p[i], start[i] );   // the second pass starts from here
		} else {
			VectorCopy( rd->p[i], start[i] );
		}
	}
}

/*
===============
CG_RagdollStep

One fixed physics step.
===============
*/
static void CG_RagdollStep( ragdoll_t *rd ) {
	vec3_t start[J_NUM], orig[J_NUM], v;
	float moved = 0;
	int i, it;

	// integrate
	for ( i = 0; i < J_NUM; i++ ) {
		VectorCopy( rd->p[i], start[i] );
		VectorSubtract( rd->p[i], rd->old[i], v );
		VectorScale( v, RD_AIRDRAG, v );
		VectorCopy( rd->p[i], rd->old[i] );
		VectorAdd( rd->p[i], v, rd->p[i] );
		rd->p[i][2] -= RD_GRAVITY * CG_XbGravityScale() * RD_STEP * RD_STEP;     // Moon Gravity skull
	}

	// muscle tone: pull each joint toward where the death pose puts it,
	// relative to the pelvis as it is now
	{
		float age = rd->steps * RD_STEP, s, f;
		vec3_t cur[3], local, target, rel;
		f = 1.0f - age / RD_MUSCLE_FADE;
		if ( f < 0 ) {
			f = 0;
		}
		s = RD_MUSCLE_END + ( RD_MUSCLE_START - RD_MUSCLE_END ) * f;
		CG_RagdollPelvisFrame( rd->p, cur );
		for ( i = 0; i < J_NUM; i++ ) {
			if ( i == J_PELVIS ) {
				continue;
			}
			VectorSubtract( rd->rest[i], rd->rest[J_PELVIS], rel );
			local[0] = DotProduct( rel, rd->restFrame[0] );
			local[1] = DotProduct( rel, rd->restFrame[1] );
			local[2] = DotProduct( rel, rd->restFrame[2] );
			VectorCopy( rd->p[J_PELVIS], target );
			VectorMA( target, local[0], cur[0], target );
			VectorMA( target, local[1], cur[1], target );
			VectorMA( target, local[2], cur[2], target );
			VectorSubtract( target, rd->p[i], rel );
			// legs give out first: knees buckle, the body crumples down
			// instead of toppling stiff as a plank
			VectorMA( rd->p[i], ( i == J_LKNEE || i == J_LFOOT || i == J_RKNEE || i == J_RFOOT ) ? s * 0.5f : s,
					  rel, rd->p[i] );
		}
		rd->steps++;
	}

	// constraints
	for ( it = 0; it < RD_ITER; it++ ) {
		CG_RagdollConstrain( rd );
	}

	// collide with the world, then let the joints settle against what they
	// hit and collide again (so constraints can't drag a joint into a step)
	memcpy( orig, start, sizeof( orig ) );
	CG_RagdollCollide( rd, start );
	for ( it = 0; it < 2; it++ ) {
		CG_RagdollConstrain( rd );
	}
	CG_RagdollCollide( rd, start );
	for ( i = 0; i < J_NUM; i++ ) {
		moved += Distance( rd->p[i], orig[i] );
	}

	// settled? (a little longer than before, so a body balanced on an
	// edge gets the chance to tip off it)
	if ( moved < 0.12f ) {
		if ( ++rd->stillSteps > 75 ) {
			rd->sleeping = qtrue;
		}
	} else {
		rd->stillSteps = 0;
	}
	// a body that never quite settles (wedged, or twitching between a
	// limit and a wall) kept simulating forever and cost frame rate: after
	// 6 s awake, if it's only shuffling in place, it goes to sleep
	rd->jitter = rd->jitter * 0.95f + moved * 0.05f;
	if ( ++rd->awakeSteps > 360 && rd->jitter < 1.5f ) {
		rd->sleeping = qtrue;
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
	// track each limb's twist through its bend plane
	for ( i = 0; i < 4; i++ ) {
		CG_RagdollLimbNormal( cur, i, rd->limbN[i] );
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
		for ( i = 0; i < 4; i++ ) {
			if ( s == rdLimbs[i].seg0 || s == rdLimbs[i].seg1 ) {
				// limb part: roll follows the limb's bend plane
				VectorCopy( rd->limbNRest[i], hr );
				VectorCopy( rd->limbN[i], hc );
			}
		}
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

// wake a body (it may have been locked or settled); keeps the moving cap
static void CG_RagdollWake( ragdoll_t *rd ) {
	int i, moving = 0, first = -1;
	if ( !rd->sleeping ) {
		rd->stillSteps = 0;
		return;
	}
	for ( i = 0; i < RD_MAX; i++ ) {
		if ( &rdBodies[i] == rd || !rdBodies[i].active || rdBodies[i].sleeping ) {
			continue;
		}
		moving++;
		if ( first < 0 || rdBodies[i].startTime < rdBodies[first].startTime ) {
			first = i;
		}
	}
	if ( moving >= RD_MAX_SIM && first >= 0 ) {
		rdBodies[first].sleeping = qtrue;
	}
	rd->sleeping = qfalse;
	rd->stillSteps = 0;
	rd->awakeSteps = 0;
	rd->jitter = 1.0f;
	rd->accum = 0;
}

// give joints near 'point' a kick (units/s), falling off over 'spread'
static void CG_RagdollKick( ragdoll_t *rd, const vec3_t point, const vec3_t vel, float spread, float minShare ) {
	int i;
	for ( i = 0; i < J_NUM; i++ ) {
		float d2 = DistanceSquared( rd->p[i], point );
		float w = minShare + ( 1.0f - minShare ) * exp( -d2 / ( 2.0f * spread * spread ) );
		VectorMA( rd->old[i], -RD_STEP * w, vel, rd->old[i] );
	}
}

/*
===============
CG_RagdollBullet

Every bullet fired (anyone's): if its path crosses a ragdoll, the part it
hits is knocked along the shot and bleeds. Done here rather than on the
server, which only knows the box where the body died, not where it is now.
===============
*/
int CG_RagdollBullet( const vec3_t start, const vec3_t end ) {
	vec3_t dir, far, c1, c2, hit;
	ragdoll_t *best = NULL;
	float bestS = 2.0f, len;
	int i, c;

	VectorSubtract( end, start, dir );
	len = VectorNormalize( dir );
	if ( len < 1 ) {
		return -1;
	}
	VectorMA( end, 24, dir, far );  // the server's hit box can stop the bullet just short of the body
	for ( i = 0; i < RD_MAX; i++ ) {
		ragdoll_t *rd = &rdBodies[i];
		float s, t, d;
		if ( !rd->active ) {
			continue;
		}
		// quick reject: pelvis far from the line
		d = CG_SegSegDist( start, far, rd->p[J_PELVIS], rd->p[J_PELVIS], &s, &t, c1, c2 );
		if ( d > 64 ) {
			continue;
		}
		for ( c = 0; c < (int)RD_NUM_CAPS; c++ ) {
			d = CG_SegSegDist( start, far, rd->p[rdCaps[c].a], rd->p[rdCaps[c].b], &s, &t, c1, c2 );
			if ( d < rdCaps[c].r + 1.0f && s < bestS ) {
				bestS = s;
				best = rd;
				VectorCopy( c1, hit );
			}
		}
	}
	if ( !best ) {
		return -1;
	}
	CG_RagdollWake( best );
	{
		vec3_t vel;
		float f = CG_XbThrowScale();      // Ragdoll Launcher / Boot Camp skulls
		VectorScale( dir, 230 * f, vel );
		vel[2] += 25 * ( f > 1 ? f * 0.6f : 1 );
		CG_RagdollKick( best, hit, vel, 10.0f, f > 1 ? 0.5f : 0.12f );
	}
	if ( best->entityNum < MAX_CLIENTS ) {
		CG_Bleed( hit, best->entityNum );
	}
	CG_RagdollAddWound( best->entityNum, hit, dir );
	if ( CG_PointContents( hit, -1 ) & CONTENTS_WATER ) {
		CG_GoreWaterBlood( hit, 2 );
	}
	return best->entityNum;
}

/*
===============
CG_RagdollExplosion

Throws every body in reach of a blast that isn't behind a wall.
'strength' is the kick (units/s) right at the centre.
===============
*/
void CG_RagdollExplosion( const vec3_t origin, float radius, float strength ) {
	int i, j;
	strength *= CG_XbThrowScale();      // Ragdoll Launcher skull
	for ( i = 0; i < RD_MAX; i++ ) {
		ragdoll_t *rd = &rdBodies[i];
		trace_t tr;
		if ( !rd->active || Distance( rd->p[J_PELVIS], origin ) > radius + 40 ) {
			continue;
		}
		CG_Trace( &tr, origin, NULL, NULL, rd->p[J_PELVIS], rd->entityNum, MASK_SOLID );
		if ( tr.fraction < 1.0f ) {
			continue;   // sheltered
		}
		CG_RagdollWake( rd );
		for ( j = 0; j < J_NUM; j++ ) {
			vec3_t away;
			float d, f;
			VectorSubtract( rd->p[j], origin, away );
			d = VectorNormalize( away );
			if ( d >= radius ) {
				continue;
			}
			f = 1.0f - d / radius;
			f = strength * f * f;
			away[2] += 0.6f;    // blasts lift
			VectorMA( rd->old[j], -RD_STEP * f, away, rd->old[j] );
		}
	}
}

// the player walking into a body nudges the parts in the way
static void CG_RagdollPlayerContact( ragdoll_t *rd ) {
	const playerState_t *ps = &cg.predictedPlayerState;
	vec3_t mins = { -16, -16, -24 }, maxs = { 16, 16, 40 };
	float speed;
	int i;

	if ( ps->pm_type == PM_DEAD || Distance( ps->origin, rd->p[J_PELVIS] ) > 96 ) {
		return;
	}
	speed = sqrt( ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1] );
	if ( speed < 90 ) {
		return;     // standing, creeping or walking slowly over a body doesn't move it
	}
	for ( i = 0; i < J_NUM; i++ ) {
		vec3_t rel, out;
		float r, ox, oy, push;
		// the heavy core (pelvis, spine, chest) stays put; only limbs and head get nudged
		if ( i == J_PELVIS || i == J_CHEST ) {
			continue;
		}
		r = rdRadius[i] * 0.5f;
		VectorSubtract( rd->p[i], ps->origin, rel );
		if ( rel[0] < mins[0] - r || rel[0] > maxs[0] + r || rel[1] < mins[1] - r || rel[1] > maxs[1] + r ||
			 rel[2] < mins[2] - r || rel[2] > maxs[2] + r ) {
			continue;
		}
		ox = rel[0] > 0 ? ( maxs[0] + r - rel[0] ) : ( mins[0] - r - rel[0] );
		oy = rel[1] > 0 ? ( maxs[1] + r - rel[1] ) : ( mins[1] - r - rel[1] );
		VectorClear( out );
		if ( fabs( ox ) < fabs( oy ) ) {
			out[0] = ox;
		} else {
			out[1] = oy;
		}
		// a gentle shove: at most a couple of units a frame, no momentum handed over
		push = VectorLength( out );
		if ( push > 2.0f ) {
			VectorScale( out, 2.0f / push, out );
		}
		CG_RagdollWake( rd );
		VectorAdd( rd->p[i], out, rd->p[i] );
		VectorAdd( rd->old[i], out, rd->old[i] );
		VectorMA( rd->old[i], -RD_STEP * 0.08f, ps->velocity, rd->old[i] );
		rd->old[i][2] = rd->p[i][2];   // never lift it
	}
}

// a body lying on a mover (the tram, a lift) rides along with it
static void CG_RagdollRide( ragdoll_t *rd ) {
	trace_t tr;
	vec3_t down, delta;
	int i;

	VectorCopy( rd->p[J_PELVIS], down );
	down[2] -= 24;
	CG_Trace( &tr, rd->p[J_PELVIS], NULL, NULL, down, rd->entityNum, MASK_SOLID );
	if ( tr.fraction < 1.0f && tr.entityNum != ENTITYNUM_WORLD && tr.entityNum < MAX_GENTITIES &&
		 ( cg_entities[tr.entityNum].currentState.eType == ET_MOVER ||
		   cg_entities[tr.entityNum].currentState.eType == ET_MOVERSCALED ) ) {
		if ( rd->groundEnt != tr.entityNum ) {
			rd->groundEnt = tr.entityNum;
			VectorCopy( cg_entities[tr.entityNum].lerpOrigin, rd->groundOrg );
			return;
		}
	} else if ( tr.fraction < 1.0f ) {
		rd->groundEnt = -1;     // on the world now
		return;
	}
	if ( rd->groundEnt < 0 ) {
		return;
	}
	VectorSubtract( cg_entities[rd->groundEnt].lerpOrigin, rd->groundOrg, delta );
	VectorCopy( cg_entities[rd->groundEnt].lerpOrigin, rd->groundOrg );
	if ( VectorLengthSquared( delta ) < 1e-4f ) {
		return;
	}
	for ( i = 0; i < J_NUM; i++ ) {
		VectorAdd( rd->p[i], delta, rd->p[i] );
		VectorAdd( rd->old[i], delta, rd->old[i] );
	}
	VectorAdd( rd->pose.origin, delta, rd->pose.origin );   // model space stays the same
	if ( rd->sleeping ) {
		CG_RagdollPose( rd );
	}
}

/*
===============
CG_RagdollBody

Called for every skeletal character just before it's added to the scene.
For dead (eligible) characters it runs the ragdoll and replaces the pose.
===============
*/
// RealRTCWX: frame-rate watch (see CG_RagdollPerfFrame)
static int rdPerfBodies, rdPerfMoving, rdPerfSteps, rdPerfMs, rdFrameSteps;

// every frame: if the game ran under 45 FPS over the last 2 s, log what
// the bodies cost
void CG_RagdollPerfFrame( void ) {
	static int start, frames, bodies, moving, steps, ms, last;
	int now = trap_Milliseconds();

	frames++;
	bodies += rdPerfBodies;
	moving += rdPerfMoving;
	steps += rdPerfSteps;
	ms += rdPerfMs;
	rdPerfBodies = rdPerfMoving = rdPerfSteps = rdPerfMs = rdFrameSteps = 0;
	last = now;
	if ( !start ) {
		start = now;
	}
	if ( now - start >= 2000 ) {
		float avg = (float)( now - start ) / frames;
		if ( avg > 22.0f ) {
			CG_Printf( "perf(cgame): %.0f FPS | bodies drawn %.1f, moving %.1f, %.1f sim steps, ragdoll %.1f ms a frame\n",
					   1000.0f / avg, (float)bodies / frames, (float)moving / frames, (float)steps / frames, (float)ms / frames );
		}
		start = now;
		frames = bodies = moving = steps = ms = 0;
	}
}

void CG_RagdollBody( centity_t *cent, refEntity_t *body ) {
	ragdoll_t *rd;
	float dt;

	// bullet wounds drawn on the skin (alive or dead)
	if ( cent->currentState.number != cg.snap->ps.clientNum && CG_RagdollBleeds( cent->currentState.aiChar ) ) {
		body->reFlags |= REFLAG_WOUNDS;
		if ( cg_bloodMode.integer == BLOODMODE_GREEN ) {
			body->reFlags |= REFLAG_WOUNDS_GREEN;
		}
	} else if ( cent->currentState.number != cg.snap->ps.clientNum && CG_RagdollDryWounds( cent->currentState.aiChar ) ) {
		body->reFlags |= REFLAG_WOUNDS | REFLAG_WOUNDS_DUST;
	}

	rd = CG_RagdollFind( cent->currentState.number );
	if ( !( cent->currentState.eFlags & EF_DEAD ) || !CG_RagdollEligible( cent ) ) {
		if ( rd ) {     // revived / respawned / turned off
			trap_R_SetRagdoll( rd->slot, NULL );
			rd->active = qfalse;
		}
		rdDeclined[cent->currentState.number] = 0;
		return;
	}
	if ( rdDeclined[cent->currentState.number] ) {
		return;
	}
	if ( rd && rd->hModel != body->hModel ) {
		trap_R_SetRagdoll( rd->slot, NULL );
		rd->active = qfalse;
		rd = NULL;
	}
	if ( !rd ) {
		rdSaved_t *sv = NULL;
		int k;
		for ( k = 0; k < rdNumSaved; k++ ) {
			if ( !rdSavedUsed[k] && rdSaved[k].entityNum == cent->currentState.number &&
				 rdSaved[k].aiChar == cent->currentState.aiChar ) {
				sv = &rdSaved[k];
				rdSavedUsed[k] = qtrue;
				break;
			}
		}
		if ( sv ) {
			// a body from a loaded save: rebuild it from its saved pose,
			// then put the joints back where they were
			refEntity_t saved = *body;
			saved.frame = sv->frame;
			saved.oldframe = sv->oldframe;
			saved.torsoFrame = sv->torsoFrame;
			saved.oldTorsoFrame = sv->oldTorsoFrame;
			saved.backlerp = sv->backlerp;
			saved.torsoBacklerp = sv->torsoBacklerp;
			memcpy( saved.axis, sv->axis, sizeof( saved.axis ) );
			memcpy( saved.torsoAxis, sv->torsoAxis, sizeof( saved.torsoAxis ) );
			VectorCopy( sv->origin, saved.origin );
			rd = CG_RagdollStart( cent, &saved );
			if ( rd ) {
				memcpy( rd->p, sv->p, sizeof( rd->p ) );
				memcpy( rd->old, sv->p, sizeof( rd->old ) );
				rd->wound = sv->wound;
				rd->sleeping = qtrue;
				rd->steps = 1000;   // muscles long relaxed
				CG_RagdollPose( rd );
			}
		} else {
			rd = CG_RagdollStart( cent, body );
		}
		if ( !rd ) {
			// decided once per death: retrying every frame would restart
			// the body (and cost bone lookups) over and over
			rdDeclined[cent->currentState.number] = 1;
			return;
		}
	}
	rd->lastSeen = cg.time;
	rdPerfBodies++;

	CG_RagdollRide( rd );
	CG_RagdollPlayerContact( rd );

	// simulate (fixed steps, at most a few per frame)
	if ( !rd->sleeping ) {
		dt = cg.frametime * 0.001f;
		if ( dt > 0.1f ) {
			dt = 0.1f;
		}
		rd->accum += dt;
		rdPerfMoving++;
		{
			// at most 2 steps a body per frame, and a budget for all of them:
			// when the frame rate drops, bodies fall a little slower instead
			// of each frame costing more and dropping it further
			int t0 = trap_Milliseconds(), n = 0;
			while ( rd->accum >= RD_STEP && !rd->sleeping && n < 2 && rdFrameSteps < 40 ) {
				CG_RagdollStep( rd );
				rd->accum -= RD_STEP;
				rdPerfSteps++;
				rdFrameSteps++;
				n++;
			}
			if ( rd->accum > RD_STEP ) {
				rd->accum = RD_STEP;
			}
			rdPerfMs += trap_Milliseconds() - t0;
		}
		// blown up (bad numbers, or flung far out of the level): give up
		// on this one rather than drawing garbage
		{
			int i;
			for ( i = 0; i < J_NUM; i++ ) {
				if ( IS_NAN( rd->p[i][0] ) || IS_NAN( rd->p[i][1] ) || IS_NAN( rd->p[i][2] ) ||
					 Distance( rd->p[i], rd->p[J_PELVIS] ) > 120 ||
					 Distance( rd->p[J_PELVIS], rd->pose.origin ) > 8192 ) {
					break;
				}
			}
			if ( i < J_NUM ) {
				CG_Printf( "ragdoll: body %i blew up, back to its death animation\n", rd->entityNum );
				trap_R_SetRagdoll( rd->slot, NULL );
				rd->active = qfalse;
				rdDeclined[cent->currentState.number] = 1;
				return;
			}
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

/*
===============
Ragdolls in savegames

The game saves entities, not cgame's ragdolls, so each save gets a small
file of its own next to it (save/<name>.rag) with every body's final pose.
The game announces each save in g_lastSave ("name levelTime"); a loaded
save is in savegame_filename when cgame starts.
===============
*/
static void CG_RagdollSaveName( const char *in, char *out, int outSize ) {
	char *dot;
	Q_strncpyz( out, in ? in : "", outSize );
	dot = strrchr( out, '.' );
	if ( dot && !Q_stricmp( dot, ".svg" ) ) {
		*dot = 0;
	}
	Q_strcat( out, outSize, ".rag" );
}

static void CG_RagdollWriteSave( const char *username ) {
	char path[MAX_QPATH], file[MAX_QPATH];
	fileHandle_t f;
	int i, n = 0, magic = RD_SAVE_MAGIC;

	Com_sprintf( file, sizeof( file ), "save/%s", username );
	CG_RagdollSaveName( file, path, sizeof( path ) );
	if ( trap_FS_FOpenFile( path, &f, FS_WRITE ) < 0 ) {
		return;
	}
	for ( i = 0; i < RD_MAX; i++ ) {
		if ( rdBodies[i].active ) {
			n++;
		}
	}
	trap_FS_Write( &magic, sizeof( magic ), f );
	trap_FS_Write( cgs.mapname, MAX_QPATH, f );
	trap_FS_Write( &n, sizeof( n ), f );
	for ( i = 0; i < RD_MAX; i++ ) {
		ragdoll_t *rd = &rdBodies[i];
		rdSaved_t sv;
		if ( !rd->active ) {
			continue;
		}
		memset( &sv, 0, sizeof( sv ) );
		sv.entityNum = rd->entityNum;
		sv.aiChar = cg_entities[rd->entityNum].currentState.aiChar;
		sv.wound = rd->wound;
		sv.frame = rd->pose.frame;
		sv.oldframe = rd->pose.oldframe;
		sv.torsoFrame = rd->pose.torsoFrame;
		sv.oldTorsoFrame = rd->pose.oldTorsoFrame;
		sv.backlerp = rd->pose.backlerp;
		sv.torsoBacklerp = rd->pose.torsoBacklerp;
		memcpy( sv.axis, rd->pose.axis, sizeof( sv.axis ) );
		memcpy( sv.torsoAxis, rd->pose.torsoAxis, sizeof( sv.torsoAxis ) );
		VectorCopy( rd->pose.origin, sv.origin );
		memcpy( sv.p, rd->p, sizeof( sv.p ) );
		trap_FS_Write( &sv, sizeof( sv ), f );
	}
	trap_FS_FCloseFile( f );
	CG_Printf( "ragdoll: kept %i body poses with save '%s'\n", n, username );
}

static void CG_RagdollLoadSaved( void ) {
	char name[MAX_QPATH], path[MAX_QPATH], map[MAX_QPATH];
	fileHandle_t f;
	int len, magic = 0, n = 0, i;

	rdNumSaved = 0;
	memset( rdSavedUsed, 0, sizeof( rdSavedUsed ) );
	trap_Cvar_VariableStringBuffer( "savegame_filename", name, sizeof( name ) );
	if ( !name[0] ) {
		return;
	}
	CG_RagdollSaveName( name, path, sizeof( path ) );
	len = trap_FS_FOpenFile( path, &f, FS_READ );
	if ( len < (int)( sizeof( int ) * 2 + MAX_QPATH ) ) {
		if ( len >= 0 ) {
			trap_FS_FCloseFile( f );
		}
		return;
	}
	trap_FS_Read( &magic, sizeof( magic ), f );
	trap_FS_Read( map, MAX_QPATH, f );
	trap_FS_Read( &n, sizeof( n ), f );
	map[MAX_QPATH - 1] = 0;
	if ( magic != RD_SAVE_MAGIC || Q_stricmp( map, cgs.mapname ) || n < 0 || n > RD_MAX ||
		 len < (int)( sizeof( int ) * 2 + MAX_QPATH + n * sizeof( rdSaved_t ) ) ) {
		trap_FS_FCloseFile( f );
		return;
	}
	for ( i = 0; i < n; i++ ) {
		trap_FS_Read( &rdSaved[i], sizeof( rdSaved_t ), f );
	}
	trap_FS_FCloseFile( f );
	rdNumSaved = n;
	CG_Printf( "ragdoll: restoring %i body poses from '%s'\n", n, path );
}

// each frame (and right away when the client is about to suspend)
void CG_RagdollCheckSave( void ) {
	char *last = rdLastSave;
	char cur[256];
	char *s, *tok;

	trap_Cvar_VariableStringBuffer( "g_lastSave", cur, sizeof( cur ) );
	if ( !cur[0] || !strcmp( cur, last ) ) {
		return;
	}
	Q_strncpyz( last, cur, sizeof( rdLastSave ) );
	CG_AchSaved();      // "game saved" indicator
	// "levelTime name [name...]"
	s = cur;
	tok = COM_Parse( &s );     // the time
	while ( 1 ) {
		tok = COM_Parse( &s );
		if ( !tok[0] ) {
			break;
		}
		CG_RagdollWriteSave( tok );
	}
}
