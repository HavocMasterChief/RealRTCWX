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
#define RD_GRAVITY      386.0f  // real gravity (1 unit ~ 1 inch)
#define RD_AIRDRAG      0.988f  // velocity kept per step
// "muscle tone": joints are pulled toward the death pose (relative to the
// pelvis) so limbs don't flop like rope; strong at first, fading to a light
// stiffness that stays
#define RD_MUSCLE_START 0.10f
#define RD_MUSCLE_END   0.0f    // fully limp once it fades (arms used to stay held out)
#define RD_MUSCLE_FADE  1.2f    // seconds
#define RD_HIT_WINDOW   1200    // ms: a hit this recent shapes the fall
// what bodies collide with: the world, and the player-clip brushes that metal
// walkways and grates are often made of (the grate itself isn't solid)
#define RD_MASK         ( MASK_SOLID | CONTENTS_PLAYERCLIP )

enum {
	J_PELVIS, J_CHEST, J_HEAD,
	J_LSHOULDER, J_LELBOW, J_LHAND,
	J_RSHOULDER, J_RELBOW, J_RHAND,
	J_LHIP, J_LKNEE, J_LFOOT,
	J_RHIP, J_RKNEE, J_RFOOT,
	J_NUM
};

// skeletons: the Character Studio biped (Bip01; the Priest's is Bip02) and
// the dog's quadruped rig, whose legs stand in for arms (front) and legs (back)
#define RD_RIG_HUMAN    0
#define RD_RIG_HUMAN2   1
#define RD_RIG_DOG      2
#define RD_RIG_XDOG     3       // the X-Shepherd (RealRTCW): a dog rig with anonymous "BoneNN" bones
#define RD_NUM_RIGS     4
#define RD_ISDOG( rig ) ( ( rig ) == RD_RIG_DOG || ( rig ) == RD_RIG_XDOG )
static const char *rdBoneNames[RD_NUM_RIGS][J_NUM] = {
	{
		"Bip01 Pelvis", "Bip01 Spine3", "Bip01 Head",
		"Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand",
		"Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand",
		"Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot",
		"Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot"
	},
	{
		"Bip02 Pelvis", "Bip02 Spine3", "Bip02 Head",
		"Bip02 L UpperArm", "Bip02 L Forearm", "Bip02 L Hand",
		"Bip02 R UpperArm", "Bip02 R Forearm", "Bip02 R Hand",
		"Bip02 L Thigh", "Bip02 L Calf", "Bip02 L Foot",
		"Bip02 R Thigh", "Bip02 R Calf", "Bip02 R Foot"
	},
	{
		"BipPelvis", "BipSpine4", "BipHead",
		"BipLFLeg2", "BipLFLeg3", "BipLFLeg5",
		"BipRFLeg2", "BipRFLeg3", "BipRFLeg5",
		"BipLBLeg1", "BipLBLeg2", "BipLBLeg4",
		"BipRBLeg1", "BipRBLeg2", "BipRBLeg4"
	},
	{
		// X-Shepherd (models/players/xdog): spine Bone02-05, head Bone08;
		// front legs Bone53.. (left) / Bone41.. (right) from the chest,
		// hind legs Bone29.. (left) / Bone17.. (right) from the spine
		// (sides from its tag_ffootleft / tag_footright etc.)
		"Bip01 Pelvis", "Bone05", "Bone08",
		"Bone54", "Bone55", "Bone57",
		"Bone42", "Bone43", "Bone45",
		"Bone29", "Bone30", "Bone32",
		"Bone17", "Bone18", "Bone20"
	}
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
	float scale;         // drawn size (Toy Soldiers skull: 0.5)
	int logMask;         // which timing lines have been logged (see CG_RagdollLogFall)
	vec3_t dogBend[4];   // dog: each leg's bend side, in the body frame of the death pose
	float dogSide[2];    // dog: same-side shoulder-hip lengths (the body can't twist)
	qboolean dogTwistLogged;
	int shoveTime;       // bullet shoves taken since then (capped)
	float shoved;
	float corpseDamage;  // force taken since death (Extreme: enough and it comes apart)
	int severedMask;     // body parts cut off (drawn collapsed into the stump), 1 << segment
	byte severRoot[RD_SEGS];    // the joint each cut part collapses into
	refEntity_t lastRef; // how it was last drawn (for a severed limb's copy)
	qboolean haveRef;
	int rig;             // RD_RIG_*
	qboolean sleeping;
	// burning: thrashing and rolling while the flames last (CG_RagdollBurn)
	qboolean burning;
	int burnLastSeen;       // last frame it was on fire
	float rollSign;
	int rollFlip, nextScorch;
	float phase[4];         // L leg, R leg, arms, head
	float data[RD_SEGS][12];
} ragdoll_t;

static ragdoll_t rdBodies[RD_MAX];
static byte rdDeclined[MAX_GENTITIES];   // dead bodies that keep their death animation
// Extreme gore, per body (cleared when the entity is alive again)
static byte rdHeadless[MAX_GENTITIES];   // head blown off
static byte rdGibbed[MAX_GENTITIES];     // shot to pieces after death
static byte rdBurn[MAX_GENTITIES];       // 1 charred (fire), 2 scorched (Tesla)
static int rdBurnStart[MAX_GENTITIES];   // when it caught fire (charring builds up from here)

qboolean CG_RagdollHeadless( int entityNum ) {
	return entityNum >= 0 && entityNum < MAX_GENTITIES && rdHeadless[entityNum];
}
qboolean CG_RagdollGibbed( int entityNum ) {
	return entityNum >= 0 && entityNum < MAX_GENTITIES && rdGibbed[entityNum];
}
int CG_RagdollBurnFlags( int entityNum ) {
	if ( entityNum < 0 || entityNum >= MAX_GENTITIES ) {
		return 0;
	}
	return rdBurn[entityNum] == 1 ? REFLAG_CHARRED : rdBurn[entityNum] == 2 ? REFLAG_SCORCHED : 0;
}
// how burnt (0..1): fire chars a body over about 6 s, not at once
float CG_RagdollCharLevel( int entityNum ) {
	float k;
	if ( entityNum < 0 || entityNum >= MAX_GENTITIES || rdBurn[entityNum] != 1 || !rdBurnStart[entityNum] ) {
		return 0;   // (0: the flags' full default)
	}
	k = 0.12f + ( cg.time - rdBurnStart[entityNum] ) / 6000.0f;
	return k > 1 ? 1 : k < 0.12f ? 0.12f : k;
}
static void CG_RagdollInitPairs( ragdoll_t *rd );
static void CG_RagdollAddWound( int entityNum, const vec3_t point, const vec3_t dir );
static void CG_RagdollLoadSaved( void );
static void CG_RagdollPose( ragdoll_t *rd );
static void CG_RagdollSever( ragdoll_t *rd, int joint, const vec3_t dir );
void CG_RagdollLimbsReset( void );
static void CG_RagdollCollapseSeg( ragdoll_t *rd, int s, const vec3_t at );
static float CG_SegSegDist( const vec3_t p1, const vec3_t q1, const vec3_t p2, const vec3_t q2,
							float *s, float *t, vec3_t c1, vec3_t c2 );

/*
The body part a shot's path goes through first: the capsule (rdCaps) the
line from 'point' - 'dir' * back to 'point' + 'dir' * ahead crosses nearest
its start, or -1. 'slack' widens the capsules (the server's hit box isn't
the model). The joint to cut at for a limb capsule is its rdCaps[].a.
*/
static int CG_RagdollRayPart( const ragdoll_t *rd, const vec3_t point, const vec3_t dir, float back, float ahead, float slack ) {
	vec3_t a, b, c1, c2;
	float bestS = 2.0f, sv, tv, d;
	int c, best = -1;
	VectorMA( point, -back, dir, a );
	VectorMA( point, ahead, dir, b );
	for ( c = 0; c < (int)( sizeof( rdCaps ) / sizeof( rdCaps[0] ) ); c++ ) {
		d = CG_SegSegDist( a, b, rd->p[rdCaps[c].a], rd->p[rdCaps[c].b], &sv, &tv, c1, c2 );
		if ( d < rdCaps[c].r + slack && sv < bestS ) {
			bestS = sv;
			best = c;
		}
	}
	return best;
}
static qboolean CG_GoreEnabled_Ragdoll( void ) {
	return cg_blood.integer && cg_bloodMode.integer != BLOODMODE_OFF;
}
static void CG_RagdollCleanStart( ragdoll_t *rd );
static void CG_RagdollKeepPose( const ragdoll_t *rd );
static void CG_RagdollDogInit( ragdoll_t *rd );

// ragdoll poses kept with savegames (save/<name>.rag)
#define RD_SAVE_MAGIC   0x31474152  // "RAG1"
typedef struct {
	int entityNum, aiChar, wound;
	int frame, oldframe, torsoFrame, oldTorsoFrame;
	float backlerp, torsoBacklerp;
	vec3_t axis[3], torsoAxis[3], origin;
	vec3_t p[J_NUM];
} rdSaved_t;
// every body whose pose is known, by entity: from a loaded save (until the
// body is seen and rebuilt), or a body whose ragdoll slot went to another.
// Saves write all of these as well as the live ragdolls, so a save made
// before you've looked at the bodies again (or with more bodies than
// slots) doesn't lose them.
static rdSaved_t rdKeep[MAX_GENTITIES];
static qboolean rdKeepValid[MAX_GENTITIES];
static char rdLastSave[256];     // the last g_lastSave we answered
static int rdStamp;

// the last bullet that hit each entity (where, which way, when)
static struct {
	int time;
	vec3_t point, dir;
	float force;        // weapon force of the hits landing together (a shotgun's pellets add up)
} rdHits[MAX_GENTITIES];

/*
===============
CG_RagdollWeaponForce

How hard one hit from 'shooter''s gun shoves a body, a pistol round being
1. Real bullets carry very little momentum; these are the weapons' real
momentum relative to each other (heavier and faster rounds more), and the
whole scale is exaggerated so it reads. A shotgun's ~4.5 is split over its
12 pellets, so it slams up close and barely moves anything far away.
===============
*/
static int rdShooter = -1;      // the shooter of the bullet being handled (CG_Bullet)

void CG_RagdollSetShooter( int shooter ) {
	rdShooter = shooter;
}

static float CG_RagdollWeaponForce( int shooter, float dist );

// the same, for the blood spray (cg_gore.c)
float CG_RagdollShotForce( int shooter, float dist ) {
	return CG_RagdollWeaponForce( shooter, dist );
}

// Extreme: only these take a corpse apart (machine guns, the Venom,
// shotguns - explosions too, see CG_RagdollExplosion)
static qboolean CG_RagdollHeavyShooter( int shooter ) {
	int w;
	if ( shooter < 0 || shooter >= MAX_GENTITIES ) {
		return qfalse;
	}
	if ( shooter == cg.snap->ps.clientNum ) {
		if ( cg.snap->ps.persistant[PERS_HWEAPON_USE] ) {
			return qtrue;       // a mounted MG42
		}
		w = cg.snap->ps.weapon;
	} else {
		if ( cg_entities[shooter].currentState.eFlags & EF_MG42_ACTIVE ) {
			return qtrue;
		}
		w = cg_entities[shooter].currentState.weapon;
	}
	switch ( w ) {
	case WP_BROWNING: case WP_MG42M: case WP_DUMMY_MG42: case WP_VENOM:
	case WP_M97: case WP_AUTO5: case WP_M30:
		return qtrue;
	default:
		return qfalse;
	}
}

static float CG_RagdollWeaponForce( int shooter, float dist ) {
	int w;
	float f, near, far, minF;

	if ( cg_xbKickHit ) {
		return 2.0f;
	}
	if ( shooter < 0 || shooter >= MAX_GENTITIES ) {
		return 1.0f;
	}
	w = ( shooter == cg.snap->ps.clientNum ) ? cg.snap->ps.weapon : cg_entities[shooter].currentState.weapon;
	near = 256; far = 1024; minF = 0.6f;      // handgun / SMG rounds lose a little with range
	switch ( w ) {
	case WP_LUGER: case WP_SILENCER: case WP_TT33: case WP_DUAL_TT33: case WP_HDM:
	case WP_MP40: case WP_STEN: case WP_PPSH: case WP_MP34:
		f = 1.0f; break;
	case WP_COLT: case WP_AKIMBO: case WP_THOMPSON: case WP_DELISLE: case WP_DELISLESCOPE:
		f = 1.4f; break;
	case WP_REVOLVER:
		f = 1.8f; break;
	case WP_MP44: case WP_FG42: case WP_FG42SCOPE: case WP_M1941: case WP_M1941SCOPE:
		f = 2.0f; near = 512; far = 2048; minF = 0.85f; break;
	case WP_G43: case WP_M1GARAND: case WP_GARAND:
		f = 3.0f; near = 1024; far = 4096; minF = 0.9f; break;
	case WP_MAUSER: case WP_MOSIN: case WP_SNIPERRIFLE: case WP_SNOOPERSCOPE: case WP_BAR: case WP_SNIPER:
		f = 3.2f; near = 1024; far = 4096; minF = 0.9f; break;
	case WP_BROWNING: case WP_MG42M: case WP_DUMMY_MG42:
		f = 3.5f; near = 1024; far = 4096; minF = 0.9f; break;
	case WP_M97: case WP_AUTO5: case WP_M30:
		f = 4.5f / 12.0f; near = 96; far = 768; minF = 0.2f; break;   // per pellet
	case WP_VENOM:
		f = 2.5f; break;
	case WP_TESLA: case WP_FLAMETHROWER:
		f = 0.15f; break;
	default:
		f = 1.0f; break;
	}
	if ( dist > near ) {
		float k = ( dist - near ) / ( far - near );
		if ( k > 1 ) {
			k = 1;
		}
		f *= 1.0f - ( 1.0f - minF ) * k;
	}
	return f;
}

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
	{
		float f = CG_RagdollWeaponForce( shooter, Distance( from, point ) );
		// hits landing together (a shotgun's pellets, a burst) add up
		if ( rdHits[entityNum].time && cg.time - rdHits[entityNum].time < 200 ) {
			rdHits[entityNum].force += f;
		} else {
			rdHits[entityNum].force = f;
		}
	}
	rdHits[entityNum].time = cg.time;
	VectorCopy( point, rdHits[entityNum].point );
	VectorCopy( dir, rdHits[entityNum].dir );
	if ( !cg_xbKickHit ) {
		CG_RagdollAddWound( entityNum, point, dir );
		// a rifle round goes through: an exit wound on the far side
		if ( CG_RagdollWeaponForce( shooter, 0 ) >= 2.5f ) {     // (one round, not a shotgun's pellets added up)
			vec3_t farSide, back;
			VectorMA( point, 30, dir, farSide );
			VectorNegate( dir, back );
			CG_RagdollAddWound( entityNum, farSide, back );
		}
	}
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
	CG_RagdollLimbsReset();
	memset( rdHeadless, 0, sizeof( rdHeadless ) );
	memset( rdGibbed, 0, sizeof( rdGibbed ) );
	memset( rdBurn, 0, sizeof( rdBurn ) );
	memset( rdBurnStart, 0, sizeof( rdBurnStart ) );
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

// armoured characters: heavy, so shots shove them about half as far
static float CG_RagdollMassScale( int aiChar ) {
	switch ( aiChar ) {
	case AICHAR_PROTOSOLDIER:
	case AICHAR_SUPERSOLDIER:
	case AICHAR_SUPERSOLDIER_LAB:
		return 0.45f;
	default:
		return 1.0f;
	}
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
		return qfalse;  // special deaths (bosses, lopers)
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
	int k;
	VectorSubtract( w, ref->origin, d );
	for ( k = 0; k < 3; k++ ) {
		// axes may be scaled (a shrunken character): undo it
		float l2 = DotProduct( ref->axis[k], ref->axis[k] );
		out[k] = DotProduct( d, ref->axis[k] ) / ( l2 > 0.0001f ? l2 : 1.0f );
	}
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

/*
===============
CG_RagdollHeadshotChunks

A killing shot to the head: a few tiny pieces fly out the way the bullet
went, bounce, leave small blood dots and fade (blood setting respected).
===============
*/
void CG_LaunchGib( centity_t *cent, vec3_t origin, vec3_t angles, vec3_t velocity, qhandle_t hModel, float sizeScale, int breakCount );

static void CG_RagdollHeadshotChunks( centity_t *cent, const vec3_t head, const vec3_t dir, int count ) {
	clientInfo_t *ci;
	int i, n, models[8], numModels = 0;

	if ( cg_bloodMode.integer == BLOODMODE_OFF || !CG_RagdollBleeds( cent->currentState.aiChar ) ) {
		return;
	}
	if ( CG_Skull( 1 << 7 ) ) {     // Confetti Blood
		CG_XbConfetti( head, 30, 220 );
		return;
	}
	ci = &cgs.clientinfo[cent->currentState.clientNum];
	for ( i = 0; i < MAX_GIB_MODELS && numModels < 8; i++ ) {
		if ( ci->gibModels[i] ) {
			models[numModels++] = i;
		}
	}
	if ( !numModels ) {
		return;
	}
	n = count ? count : 6 + ( rand() % 5 );
	for ( i = 0; i < n; i++ ) {
		vec3_t vel, ang, org;
		float sp = 90 + random() * 140;
		VectorCopy( head, org );
		org[0] += crandom() * 2;
		org[1] += crandom() * 2;
		org[2] += crandom() * 2;
		// mostly along the shot, spread out in a cone
		VectorScale( dir, sp, vel );
		vel[0] += crandom() * 70;
		vel[1] += crandom() * 70;
		vel[2] += 40 + random() * 70;
		VectorSet( ang, random() * 360, random() * 360, random() * 360 );
		CG_LaunchGib( cent, org, ang, vel, ci->gibModels[models[rand() % numModels]],
					  count ? 0.14f + random() * 0.2f : 0.1f + random() * 0.08f, 0 );
	}
}

static ragdoll_t *CG_RagdollStart( centity_t *cent, const refEntity_t *body ) {
	ragdoll_t *rd = NULL;
	orientation_t o;
	refEntity_t pose;
	vec3_t push, toBody, rest[J_NUM];
	int i, oldest = -1, slot, rigFound = 0;

	if ( body->nonNormalizedAxes ) {
		// a uniformly scaled character (Toy Soldiers) works; stretched ones don't
		float l0 = VectorLength( body->axis[0] ), l1 = VectorLength( body->axis[1] ), l2 = VectorLength( body->axis[2] );
		if ( fabs( l0 - l1 ) > 0.02f || fabs( l0 - l2 ) > 0.02f || l0 < 0.1f ) {
			CG_RagdollWhyNot( cent, body, "non-uniformly scaled model" );
			return NULL;
		}
	}
	pose = *body;
	pose.ragdoll = 0;
	pose.ragdollStamp = 0;

	// the joints, in model space, for the frozen pose (whichever skeleton
	// this model has)
	{
		int rig, miss = 0;
		for ( rig = 0; rig < RD_NUM_RIGS; rig++ ) {
			for ( i = 0; i < J_NUM; i++ ) {
				if ( trap_R_LerpBone( &o, &pose, rdBoneNames[rig][i] ) < 0 ) {
					if ( rig == 0 ) {
						miss = i;
					}
					break;
				}
				VectorCopy( o.origin, rest[i] );
			}
			if ( i == J_NUM ) {
				break;
			}
		}
		if ( rig == RD_NUM_RIGS ) {
			CG_RagdollWhyNot( cent, body, va( "no bone '%s'", rdBoneNames[0][miss] ) );
			return NULL;    // a skeleton we don't know: no ragdoll
		}
		rigFound = rig;
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
		CG_RagdollKeepPose( &rdBodies[oldest] );     // (its pose stays with the saves)
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
	rd->scale = body->nonNormalizedAxes ? VectorLength( body->axis[0] ) : 1.0f;
	rd->rig = rigFound;
	rd->shoveTime = 0;
	rd->shoved = 0;
	rd->corpseDamage = 0;
	rd->severedMask = 0;
	rd->lastRef = *body;
	rd->haveRef = qtrue;
	// Extreme: died burning, or to the Tesla gun
	if ( CG_BloodExtreme() ) {
		if ( CG_EntOnFire( cent ) ) {
			rdBurn[cent->currentState.number] = 1;
			rdBurnStart[cent->currentState.number] = cent->currentState.onFireStart > 0 && cent->currentState.onFireStart <= cg.time ?
													 cent->currentState.onFireStart : cg.time;
		} else if ( cent->pe.teslaDamagedTime && cent->pe.teslaDamagedTime > cg.time - 800 ) {
			rdBurn[cent->currentState.number] = 2;
		}
	}
	rd->logMask = 0;
	{
		static int logged;
		int ago = rdHits[rd->entityNum].time ? cg.time - rdHits[rd->entityNum].time : -1;
		if ( logged < 40 ) {
			logged++;
			CG_Printf( "ragdoll: body %i falls %i ms after the killing hit, force %.1f\n",
					   rd->entityNum, ago, ago >= 0 && ago < RD_HIT_WINDOW ? rdHits[rd->entityNum].force : 0.0f );
		}
	}
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
		float bestD = 1e9f, wf = rdHits[rd->entityNum].force;
		// the killing shot's weapon: a pistol mostly crumples them, a rifle
		// or a shotgun up close knocks them back
		if ( wf < 0.5f ) {
			wf = 0.5f;      // even a pistol moves the part it hit
		} else if ( wf > 5.0f ) {
			wf = 5.0f;
		}
		wf *= CG_RagdollMassScale( cg_entities[rd->entityNum].currentState.aiChar );
		for ( i = 0; i < J_NUM; i++ ) {
			float dd = DistanceSquared( rd->p[i], hp );
			if ( dd < bestD ) {
				bestD = dd;
				rd->wound = i;
			}
		}
		// Extreme: a heavy killing round into an arm or a leg takes it off -
		// the part its path goes through on the body as it fell, not just
		// the joint nearest the server's hit box
		if ( rdHits[rd->entityNum].force >= 3.0f ) {
			int part = CG_RagdollRayPart( rd, hp, hd, 48, 48, 2.0f );
			int cut = part >= 2 ? rdCaps[part].a : -1;
			if ( part < 0 && bestD < 10.0f * 10.0f && rd->wound != J_HEAD && rd->wound != J_CHEST && rd->wound != J_PELVIS ) {
				cut = rd->wound;
			}
			CG_Printf( "ragdoll: body %i killing shot force %.1f hit part %i\n", rd->entityNum, rdHits[rd->entityNum].force, part );
			if ( cut >= 0 ) {
				rd->haveRef = qtrue;
				rd->wound = cut;
				CG_RagdollSever( rd, cut, hd );
			}
		}
		if ( rd->wound == J_HEAD && bestD < 10.0f * 10.0f ) {
			// Extreme: a sniper round or a close shotgun blast takes the head off
			if ( CG_BloodExtreme() && rdHits[rd->entityNum].force >= 3.0f && !RD_ISDOG( rd->rig ) &&
				 CG_RagdollBleeds( cent->currentState.aiChar ) && !CG_Skull( 1 << 7 ) ) {
				rdHeadless[rd->entityNum] = 1;
				CG_RagdollHeadshotChunks( cent, rd->p[J_HEAD], hd, 22 );
				CG_GoreHeadBurst( rd->p[J_HEAD], hd, rd->entityNum );
				trap_S_StartSound( rd->p[J_HEAD], ENTITYNUM_WORLD, CHAN_AUTO, cgs.media.gibSound );
			} else {
				CG_RagdollHeadshotChunks( cent, rd->p[J_HEAD], hd, 0 );
			}
		}
		for ( i = 0; i < J_NUM; i++ ) {
			float d2 = DistanceSquared( rd->p[i], hp );
			// the part the killing shot hit leads (a foot shot kicks that
			// foot out, a head shot snaps the head back); the rest of the
			// body gets a small share, and feet that weren't hit grip
			float w = exp( -d2 / ( 2.0f * 9.0f * 9.0f ) );
			float share = ( i == J_LFOOT || i == J_RFOOT ) ? 0.0f : 8.0f;
			VectorScale( hd, ( share + 85 * w ) * wf * CG_XbThrowScale(), push );
			push[2] += ( 3 + 8 * w ) * sqrt( wf ) * CG_XbThrowScale();
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
			VectorScale( toBody, ( 52 + random() * 38 ) * k, push );
			push[2] = 17 * k;
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
	if ( RD_ISDOG( rd->rig ) ) {
		CG_RagdollDogInit( rd );
	}
	CG_RagdollCleanStart( rd );    // (after the bone lengths are taken from the pose)
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
			rd->pair[rd->numPairs].minDist = MIN( ( rdCaps[i].r + rdCaps[j].r ) * rd->scale, d * 0.9f );
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

/*
===============
Dog skeleton

A dog's spine runs along the ground and its hips sit close together, so
the biped's "pelvis frame" (spine up, hips sideways) flipped over when a
dog landed on its side and the back half twisted round. A dog's body frame
is the spine forward with the hips AND shoulders for the side, each leg
bends to the side its death pose bent it, and same-side shoulder-hip braces
keep the body from corkscrewing.
===============
*/
static const int rdDogLimbs[4][3] = {
	{ J_LSHOULDER, J_LELBOW, J_LHAND }, { J_RSHOULDER, J_RELBOW, J_RHAND },
	{ J_LHIP, J_LKNEE, J_LFOOT }, { J_RHIP, J_RKNEE, J_RFOOT }
};

static void CG_RagdollDogFrame( vec3_t j[J_NUM], vec3_t f[3] ) {
	vec3_t side, t;
	VectorSubtract( j[J_CHEST], j[J_PELVIS], f[0] );
	VectorNormalize( f[0] );
	VectorSubtract( j[J_RHIP], j[J_LHIP], side );
	VectorSubtract( j[J_RSHOULDER], j[J_LSHOULDER], t );
	VectorAdd( side, t, side );
	VectorMA( side, -DotProduct( side, f[0] ), f[0], f[1] );
	if ( VectorNormalize( f[1] ) < 0.001f ) {
		PerpendicularVector( f[1], f[0] );
	}
	CrossProduct( f[0], f[1], f[2] );
}

static void CG_RagdollDogInit( ragdoll_t *rd ) {
	vec3_t f[3], u, o, off;
	int i;
	CG_RagdollDogFrame( rd->rest, f );
	for ( i = 0; i < 4; i++ ) {
		float *a = rd->rest[rdDogLimbs[i][0]], *k = rd->rest[rdDogLimbs[i][1]], *b = rd->rest[rdDogLimbs[i][2]];
		VectorSubtract( b, a, u );
		VectorNormalize( u );
		VectorSubtract( k, a, o );
		VectorMA( o, -DotProduct( o, u ), u, off );
		if ( VectorNormalize( off ) < 0.001f ) {
			// straight in the death pose: front knees bend back, hind hocks forward... the
			// joint between them goes the usual way for a dog
			VectorScale( f[0], i < 2 ? -1.0f : 1.0f, off );
		}
		rd->dogBend[i][0] = DotProduct( off, f[0] );
		rd->dogBend[i][1] = DotProduct( off, f[1] );
		rd->dogBend[i][2] = DotProduct( off, f[2] );
		rd->hingeOn[i] = qtrue;
	}
	rd->dogSide[0] = Distance( rd->p[J_LSHOULDER], rd->p[J_LHIP] );
	rd->dogSide[1] = Distance( rd->p[J_RSHOULDER], rd->p[J_RHIP] );
	rd->dogTwistLogged = qfalse;
}

static void CG_RagdollHinge( ragdoll_t *rd, int a, int k, int b, const vec3_t bendDir, qboolean *on );

static void CG_RagdollDogLimits( ragdoll_t *rd ) {
	vec3_t f[3], bend, d;
	int i;
	// same-side braces (no corkscrew)
	for ( i = 0; i < 2; i++ ) {
		float *a = rd->p[i ? J_RSHOULDER : J_LSHOULDER], *b = rd->p[i ? J_RHIP : J_LHIP];
		float len, diff;
		VectorSubtract( b, a, d );
		len = VectorLength( d );
		if ( len < 0.001f ) {
			continue;
		}
		diff = ( len - rd->dogSide[i] ) / len * 0.35f;
		VectorMA( a, diff, d, a );
		VectorMA( b, -diff, d, b );
	}
	// legs bend only their own way
	CG_RagdollDogFrame( rd->p, f );
	for ( i = 0; i < 4; i++ ) {
		VectorScale( f[0], rd->dogBend[i][0], bend );
		VectorMA( bend, rd->dogBend[i][1], f[1], bend );
		VectorMA( bend, rd->dogBend[i][2], f[2], bend );
		CG_RagdollHinge( rd, rdDogLimbs[i][0], rdDogLimbs[i][1], rdDogLimbs[i][2], bend, &rd->hingeOn[i] );
	}
}

// how far the dog's back half has turned against its front half (degrees)
static float CG_RagdollDogTwist( ragdoll_t *rd ) {
	vec3_t sp, hips, sh;
	VectorSubtract( rd->p[J_CHEST], rd->p[J_PELVIS], sp );
	if ( VectorNormalize( sp ) < 0.001f ) {
		return 0;
	}
	VectorSubtract( rd->p[J_RHIP], rd->p[J_LHIP], hips );
	VectorSubtract( rd->p[J_RSHOULDER], rd->p[J_LSHOULDER], sh );
	VectorMA( hips, -DotProduct( hips, sp ), sp, hips );
	VectorMA( sh, -DotProduct( sh, sp ), sp, sh );
	if ( VectorNormalize( hips ) < 0.001f || VectorNormalize( sh ) < 0.001f ) {
		return 0;
	}
	return acos( DotProduct( hips, sh ) > 1 ? 1 : DotProduct( hips, sh ) < -1 ? -1 : DotProduct( hips, sh ) ) * 180.0f / M_PI;
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
/*
===============
CG_RagdollNeckLimit

The head stays within ~60 degrees of the spine's direction (it can loll,
not snap straight back).
===============
*/
static void CG_RagdollNeckLimit( ragdoll_t *rd ) {
	vec3_t up, h, perp;
	float len, c, maxC = 0.5f, along;
	if ( RD_ISDOG( rd->rig ) ) {
		return;
	}
	VectorSubtract( rd->p[J_CHEST], rd->p[J_PELVIS], up );
	if ( VectorNormalize( up ) < 0.5f ) {
		return;
	}
	VectorSubtract( rd->p[J_HEAD], rd->p[J_CHEST], h );
	len = VectorLength( h );
	if ( len < 0.5f ) {
		return;
	}
	c = DotProduct( h, up ) / len;
	if ( c >= maxC ) {
		return;
	}
	// rotate the neck back to the edge of the cone, keeping its sideways lean
	along = DotProduct( h, up );
	VectorMA( h, -along, up, perp );
	if ( VectorNormalize( perp ) < 0.001f ) {
		return;
	}
	VectorScale( up, len * maxC, h );
	VectorMA( h, len * 0.866f, perp, h );
	VectorAdd( rd->p[J_CHEST], h, rd->p[J_HEAD] );
}

/*
===============
CG_RagdollBoneTrace

Joints are balls, so a bone could pass through a table edge, a post or a
railing with a joint clear on each side, and stay hooked there. Each long
bone is traced end to end; where it crosses something solid, the end on
the shorter side is moved back across to the other side of that surface.
===============
*/
static const int rdBones[][2] = {
	{ J_PELVIS, J_CHEST }, { J_CHEST, J_HEAD },
	{ J_LSHOULDER, J_LELBOW }, { J_LELBOW, J_LHAND },
	{ J_RSHOULDER, J_RELBOW }, { J_RELBOW, J_RHAND },
	{ J_LHIP, J_LKNEE }, { J_LKNEE, J_LFOOT },
	{ J_RHIP, J_RKNEE }, { J_RKNEE, J_RFOOT }
};
#define RD_NUM_BONES ( sizeof( rdBones ) / sizeof( rdBones[0] ) )

// one bone; qtrue if it crossed something and was fixed
static qboolean CG_RagdollFixBone( ragdoll_t *rd, int a, int b, qboolean keepA ) {
	trace_t tr, back;
	int fixed, keep;
	vec3_t n, d;
	float dist;

	CG_Trace( &tr, rd->p[a], NULL, NULL, rd->p[b], rd->entityNum, RD_MASK );
	if ( tr.fraction >= 1.0f || tr.startsolid || tr.allsolid ) {
		return qfalse;      // clear (or a joint already inside: CG_RagdollCollide's job)
	}
	CG_Trace( &back, rd->p[b], NULL, NULL, rd->p[a], rd->entityNum, RD_MASK );
	if ( back.startsolid || back.allsolid ) {
		return qfalse;
	}
	// the end with more of the bone on its side stays
	if ( keepA || tr.fraction >= back.fraction ) {
		keep = a; fixed = b; VectorCopy( tr.plane.normal, n );
	} else {
		keep = b; fixed = a; VectorCopy( back.plane.normal, n );
		VectorCopy( back.endpos, tr.endpos );
	}
	(void)keep;
	// put the moved end back on the kept side of that surface
	VectorSubtract( rd->p[fixed], tr.endpos, d );
	dist = DotProduct( d, n );
	if ( dist < 1.0f ) {
		vec3_t to;
		trace_t t2;
		VectorMA( rd->p[fixed], 1.0f - dist, n, to );
		CG_Trace( &t2, rd->p[fixed], NULL, NULL, to, rd->entityNum, RD_MASK );
		if ( !t2.startsolid ) {
			VectorCopy( t2.endpos, rd->p[fixed] );
		} else {
			VectorCopy( to, rd->p[fixed] );
		}
		// and it loses its motion into that surface
		VectorSubtract( rd->p[fixed], rd->old[fixed], d );
		dist = DotProduct( d, n );
		if ( dist < 0 ) {
			VectorMA( rd->old[fixed], dist, n, rd->old[fixed] );
		}
		return qtrue;
	}
	return qfalse;
}

static void CG_RagdollBoneTrace( ragdoll_t *rd ) {
	int i;
	for ( i = 0; i < (int)RD_NUM_BONES; i++ ) {
		CG_RagdollFixBone( rd, rdBones[i][0], rdBones[i][1], qfalse );
	}
}

/*
===============
CG_RagdollCleanStart

The death animation ignores furniture, so the first pose can already have
an arm through a post or a chest through a tabletop. Walk the skeleton out
from the pelvis (which the game keeps clear) and fold back any joint whose
bone runs into something, with everything beyond it.
===============
*/
static void CG_RagdollCleanStart( ragdoll_t *rd ) {
	// parent -> child, in order from the pelvis out
	static const int chain[][2] = {
		{ J_PELVIS, J_CHEST }, { J_CHEST, J_HEAD },
		{ J_CHEST, J_LSHOULDER }, { J_LSHOULDER, J_LELBOW }, { J_LELBOW, J_LHAND },
		{ J_CHEST, J_RSHOULDER }, { J_RSHOULDER, J_RELBOW }, { J_RELBOW, J_RHAND },
		{ J_PELVIS, J_LHIP }, { J_LHIP, J_LKNEE }, { J_LKNEE, J_LFOOT },
		{ J_PELVIS, J_RHIP }, { J_RHIP, J_RKNEE }, { J_RKNEE, J_RFOOT }
	};
	static const int below[J_NUM][4] = {     // joints that move with each one
		{ -1 }, { J_HEAD, J_LSHOULDER, J_RSHOULDER, -1 }, { -1 },
		{ J_LELBOW, J_LHAND, -1 }, { J_LHAND, -1 }, { -1 },
		{ J_RELBOW, J_RHAND, -1 }, { J_RHAND, -1 }, { -1 },
		{ J_LKNEE, J_LFOOT, -1 }, { J_LFOOT, -1 }, { -1 },
		{ J_RKNEE, J_RFOOT, -1 }, { J_RFOOT, -1 }, { -1 }
	};
	int i, k, fixes = 0;
	for ( i = 0; i < (int)( sizeof( chain ) / sizeof( chain[0] ) ); i++ ) {
		int a = chain[i][0], b = chain[i][1];
		trace_t tr;
		vec3_t delta, to;
		CG_Trace( &tr, rd->p[a], NULL, NULL, rd->p[b], rd->entityNum, RD_MASK );
		if ( tr.fraction >= 1.0f || tr.startsolid ) {
			continue;
		}
		// just short of what it hit
		VectorSubtract( rd->p[b], rd->p[a], delta );
		VectorMA( rd->p[a], tr.fraction * 0.9f, delta, to );
		VectorSubtract( to, rd->p[b], delta );
		VectorAdd( rd->p[b], delta, rd->p[b] );
		VectorAdd( rd->old[b], delta, rd->old[b] );
		for ( k = 0; k < 4 && below[b][k] >= 0; k++ ) {
			VectorAdd( rd->p[below[b][k]], delta, rd->p[below[b][k]] );
			VectorAdd( rd->old[below[b][k]], delta, rd->old[below[b][k]] );
		}
		fixes++;
	}
	if ( fixes ) {
		CG_Printf( "ragdoll: body %i started with %i bone(s) in something; folded back\n", rd->entityNum, fixes );
	}
}

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
	if ( !RD_ISDOG( rd->rig ) ) {
		CG_RagdollJointLimits( rd );    // knees/elbows bend one way (a biped's)
	} else {
		CG_RagdollDogLimits( rd );
	}
	CG_RagdollNeckLimit( rd );
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
		float r = rdRadius[i] * rd->scale * ( ( RD_ISDOG( rd->rig ) && i >= J_LSHOULDER ) ? 0.6f : 1.0f );

		VectorSet( mins, -r, -r, -r );
		VectorSet( maxs, r, r, r );
		CG_Trace( &tr, start[i], mins, maxs, rd->p[i], rd->entityNum, RD_MASK );
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
			if ( ( i == J_PELVIS || i == J_CHEST || i == rd->wound || ( CG_BloodExtreme() && i == J_HEAD ) ) &&
				 ( tr.plane.normal[2] > 0.6f || CG_BloodExtreme() ) &&     // Extreme: down walls and stairs too
				 VectorLength( vt ) > 0.8f && cg.time - rd->startTime < 25000 &&
				 cg_blood.integer && cg_bloodMode.integer != BLOODMODE_OFF &&
				 CG_RagdollBleeds( cg_entities[rd->entityNum].currentState.aiChar ) &&
				 Distance( tr.endpos, rd->lastSmear ) > ( CG_BloodExtreme() ? 4.0f : 7.0f ) ) {
				VectorCopy( tr.endpos, rd->lastSmear );
				CG_ImpactMark( CG_BloodDot(), tr.endpos, tr.plane.normal, random() * 360,
							   1, 1, 1, 1, qfalse, CG_BloodExtreme() ? 7 + random() * 6 : 5 + random() * 4, qfalse,
							   CG_BloodExtreme() ? CG_BloodMarkTime() : -1 );
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
CG_RagdollCollideSegs

Joints are balls, so a body lying across a ledge used to hang from its
pelvis and chest with the torso passing through the edge between them. The
middle of each long part (torso, thighs, shins, upper arms) is traced as
well; where it hits, the whole part is held up there, so it pivots on the
edge and slides off it.
===============
*/
static const struct { int a, b; float r; } rdMidSegs[] = {
	{ J_PELVIS, J_CHEST, 3.0f },
	{ J_LHIP, J_LKNEE, 2.5f }, { J_RHIP, J_RKNEE, 2.5f },
	{ J_LKNEE, J_LFOOT, 2.0f }, { J_RKNEE, J_RFOOT, 2.0f },
	{ J_LSHOULDER, J_LELBOW, 2.0f }, { J_RSHOULDER, J_RELBOW, 2.0f }
};
#define RD_NUM_MIDSEGS ( sizeof( rdMidSegs ) / sizeof( rdMidSegs[0] ) )

// move a joint by 'fix', stopping at anything solid
static void CG_RagdollNudge( ragdoll_t *rd, int j, const vec3_t fix ) {
	trace_t tr;
	vec3_t to;
	VectorAdd( rd->p[j], fix, to );
	CG_Trace( &tr, rd->p[j], NULL, NULL, to, rd->entityNum, RD_MASK );
	if ( !tr.startsolid ) {
		VectorCopy( tr.endpos, rd->p[j] );
	}
}

static void CG_RagdollCollideSegs( ragdoll_t *rd, vec3_t before[J_NUM] ) {
	int i;
	for ( i = 0; i < (int)RD_NUM_MIDSEGS; i++ ) {
		int a = rdMidSegs[i].a, b = rdMidSegs[i].b;
		vec3_t m0, m1, mins, maxs, fix, d;
		float r = rdMidSegs[i].r * rd->scale, dn, da, db, va, vb, vm;
		trace_t tr;

		VectorAdd( before[a], before[b], m0 );
		VectorScale( m0, 0.5f, m0 );
		VectorAdd( rd->p[a], rd->p[b], m1 );
		VectorScale( m1, 0.5f, m1 );
		VectorSet( mins, -r, -r, -r );
		VectorSet( maxs, r, r, r );
		CG_Trace( &tr, m0, mins, maxs, m1, rd->entityNum, RD_MASK );
		if ( tr.allsolid || tr.startsolid || tr.fraction >= 1.0f ) {
			continue;
		}
		// only an edge under the middle with both ends clear of that surface
		// (a body across a ledge or a railing); anything else the joints
		// already handle
		VectorSubtract( rd->p[a], tr.endpos, d );
		da = DotProduct( d, tr.plane.normal );
		VectorSubtract( rd->p[b], tr.endpos, d );
		db = DotProduct( d, tr.plane.normal );
		if ( da < r || db < r ) {
			continue;
		}
		VectorSubtract( m1, tr.endpos, fix );
		dn = DotProduct( fix, tr.plane.normal );
		if ( dn >= 0 ) {
			continue;
		}
		// hold the middle up on the edge; the ends keep their own motion, so
		// the part tips over it and slides off
		VectorScale( tr.plane.normal, -dn + 0.05f, fix );
		CG_RagdollNudge( rd, a, fix );
		CG_RagdollNudge( rd, b, fix );
		// stop only the middle's motion into the surface (the part keeps
		// turning), with a touch of friction along it
		VectorSubtract( rd->p[a], rd->old[a], d );
		va = DotProduct( d, tr.plane.normal );
		VectorSubtract( rd->p[b], rd->old[b], d );
		vb = DotProduct( d, tr.plane.normal );
		vm = 0.5f * ( va + vb );
		if ( vm < 0 ) {
			VectorMA( rd->old[a], vm, tr.plane.normal, rd->old[a] );
			VectorMA( rd->old[b], vm, tr.plane.normal, rd->old[b] );
		}
		{
			vec3_t ta, tb;
			VectorSubtract( rd->p[a], rd->old[a], ta );
			VectorSubtract( rd->p[b], rd->old[b], tb );
			VectorMA( rd->p[a], -0.97f, ta, rd->old[a] );
			VectorMA( rd->p[b], -0.97f, tb, rd->old[b] );
		}
	}
}

/*
===============
CG_RagdollStep

One fixed physics step.
===============
*/
/*
===============
CG_RagdollBurnThrash

A body dying on fire doesn't just drop: while the flames last it rolls
back and forth on the ground, the knees pull up and kick, the arms claw at
the face and the head tosses, all fading to twitches as the fire dies down
(the server keeps it burning and screaming meanwhile; see g_active.c).
Driven joints are steered toward a target velocity, so it can't blow up.
===============
*/
#define RD_BURN_FADE 2500   // ms: the thrashing fades out over the fire's last part
static void CG_RagdollSteer( ragdoll_t *rd, int j, const vec3_t vt, float gain ) {
	vec3_t v;
	VectorSubtract( rd->p[j], rd->old[j], v );
	VectorScale( v, 1.0f / RD_STEP, v );
	v[0] += ( vt[0] - v[0] ) * gain;
	v[1] += ( vt[1] - v[1] ) * gain;
	v[2] += ( vt[2] - v[2] ) * gain;
	VectorMA( rd->p[j], -RD_STEP, v, rd->old[j] );
}

static void CG_RagdollBurnThrash( ragdoll_t *rd ) {
	centity_t *cent = &cg_entities[rd->entityNum];
	vec3_t axis, side, c, vt, r, up = { 0, 0, 1 };
	float k, w, left;
	int i, age;
	static const int torso[] = { J_PELVIS, J_CHEST, J_HEAD, J_LSHOULDER, J_RSHOULDER, J_LHIP, J_RHIP };

	if ( !rd->burning ) {
		return;
	}
	left = (float)( cent->currentState.onFireEnd - cg.time );
	k = left / RD_BURN_FADE;
	if ( k > 1 ) {
		k = 1;
	}
	age = cg.time - rd->startTime;
	if ( age < 250 ) {
		k *= age / 250.0f;      // the fall first
	}
	if ( k <= 0.02f ) {
		return;
	}
	VectorSubtract( rd->p[J_CHEST], rd->p[J_PELVIS], axis );
	if ( VectorNormalize( axis ) < 1 ) {
		return;
	}
	CrossProduct( axis, up, side );
	if ( VectorNormalize( side ) < 0.1f ) {
		VectorSet( side, 1, 0, 0 );
	}
	VectorAdd( rd->p[J_CHEST], rd->p[J_PELVIS], c );
	VectorScale( c, 0.5f, c );

	// roll one way, then back the other
	if ( cg.time >= rd->rollFlip || rd->rollFlip - cg.time > 2000 ) {
		rd->rollSign = rd->rollSign > 0 ? -1.0f : 1.0f;
		rd->rollFlip = cg.time + 650 + rand() % 550;
	}
	w = rd->rollSign * 5.5f * k;
	for ( i = 0; i < (int)( sizeof( torso ) / sizeof( torso[0] ) ); i++ ) {
		int j = torso[i];
		VectorSubtract( rd->p[j], c, r );
		VectorMA( r, -DotProduct( r, axis ), axis, r );     // off the long axis
		CrossProduct( axis, r, vt );
		VectorScale( vt, w, vt );
		VectorMA( vt, rd->rollSign * 55.0f * k, side, vt );    // and across the floor
		vt[2] -= 20.0f;     // (stays down)
		CG_RagdollSteer( rd, j, vt, 0.12f );
	}
	// legs: knees up to the chest and kicking out, out of step
	for ( i = 0; i < 2; i++ ) {
		int knee = i ? J_RKNEE : J_LKNEE, foot = i ? J_RFOOT : J_LFOOT;
		float s;
		rd->phase[i] += RD_STEP * ( 8.0f + 3.0f * i );
		s = sin( rd->phase[i] );
		VectorScale( axis, 110.0f * k * s, vt );
		VectorMA( vt, 40.0f * k * cos( rd->phase[i] * 1.3f ), side, vt );
		CG_RagdollSteer( rd, knee, vt, 0.16f );
		VectorScale( vt, 1.3f, vt );
		CG_RagdollSteer( rd, foot, vt, 0.16f );
	}
	// arms: hands to the face, clawing
	rd->phase[2] += RD_STEP * 9.0f;
	for ( i = 0; i < 2; i++ ) {
		int hand = i ? J_RHAND : J_LHAND, elbow = i ? J_RELBOW : J_LELBOW;
		VectorSubtract( rd->p[J_HEAD], rd->p[hand], vt );
		VectorNormalize( vt );
		VectorScale( vt, 140.0f * k * ( 0.35f + 0.65f * fabs( sin( rd->phase[2] + i * 1.7f ) ) ), vt );
		vt[0] += crandom() * 90.0f * k;
		vt[1] += crandom() * 90.0f * k;
		vt[2] += crandom() * 60.0f * k;
		CG_RagdollSteer( rd, hand, vt, 0.18f );
		VectorScale( vt, 0.6f, vt );
		CG_RagdollSteer( rd, elbow, vt, 0.12f );
	}
	// head tossing side to side
	rd->phase[3] += RD_STEP * 7.0f;
	VectorScale( side, 70.0f * k * sin( rd->phase[3] ), vt );
	CG_RagdollSteer( rd, J_HEAD, vt, 0.15f );

	// keep it awake while it burns
	rd->stillSteps = 0;
	rd->awakeSteps = 0;
	rd->jitter = 2.0f;
}

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
	CG_RagdollBurnThrash( rd );

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
			if ( i == J_LKNEE || i == J_LFOOT || i == J_RKNEE || i == J_RFOOT ) {
				continue;   // the knees give at once
			}
			VectorMA( rd->p[i], s, rel, rd->p[i] );
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
	// the middles of the long parts, for a body lying across a ledge
	// (the lift itself stops at walls; see CG_RagdollNudge)
	CG_RagdollCollideSegs( rd, orig );
	CG_RagdollBoneTrace( rd );
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
// draw one body part shrunk to (nearly) a point at 'at' (model space)
static void CG_RagdollCollapseSeg( ragdoll_t *rd, int s, const vec3_t at ) {
	float *d = rd->data[s];
	int i;
	for ( i = 0; i < 9; i++ ) {
		d[i] = ( i == 0 || i == 4 || i == 8 ) ? 0.001f : 0.0f;
	}
	for ( i = 0; i < 3; i++ ) {
		d[9 + i] = at[i] - 0.001f * rd->rest[rdSegs[s].anchor][i];
	}
}

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
		if ( RD_ISDOG( rd->rig ) && s == 0 ) {
			// dog: the hips alone are too close together to say which way
			// the back half faces; the shoulders steady it
			vec3_t t;
			VectorSubtract( rd->rest[J_RSHOULDER], rd->rest[J_LSHOULDER], t );
			VectorAdd( hr, t, hr );
			VectorSubtract( cur[J_RSHOULDER], cur[J_LSHOULDER], t );
			VectorAdd( hc, t, hc );
		}
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
		// a part that's been cut off shrinks into its stump
		if ( rd->severedMask & ( 1 << s ) ) {
			CG_RagdollCollapseSeg( rd, s, cur[rd->severRoot[s]] );
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
A corpse that has taken enough (corpseDamage: heavy fire, blasts) bursts
into gibs (any blood setting but off, as the original game's corpse gibs). qtrue if it did (the ragdoll is gone).
*/
static qboolean CG_RagdollGibCorpse( ragdoll_t *rd, const vec3_t dir ) {
	centity_t *bc;
	vec3_t org, gd;
	if ( !CG_GoreEnabled_Ragdoll() || CG_Skull( 1 << 7 ) || rd->corpseDamage < 18.0f || rd->entityNum >= MAX_GENTITIES || RD_ISDOG( rd->rig ) ||
		 !CG_RagdollBleeds( cg_entities[rd->entityNum].currentState.aiChar ) ) {
		return qfalse;
	}
	bc = &cg_entities[rd->entityNum];
	VectorCopy( rd->p[J_PELVIS], org );
	VectorCopy( dir, gd );
	trap_S_StartSound( org, ENTITYNUM_WORLD, CHAN_AUTO, cgs.media.gibSound );
	CG_GibPlayer( bc, org, gd );
	CG_GoreHeadBurst( rd->p[J_CHEST], dir, rd->entityNum );
	rdGibbed[rd->entityNum] = 1;
	rdKeepValid[rd->entityNum] = qfalse;   // (nothing left to keep)
	trap_R_SetRagdoll( rd->slot, NULL );
	rd->active = qfalse;
	CG_Printf( "ragdoll: body %i gibbed (corpse damage %.1f)\n", rd->entityNum, rd->corpseDamage );
	return qtrue;
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
	int i, c, bestCap;

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
	bestCap = CG_RagdollRayPart( best, hit, dir, 24, 24, 1.0f );
	CG_RagdollWake( best );
	{
		vec3_t vel;
		float f = CG_XbThrowScale();      // Ragdoll Launcher / Boot Camp skulls
		float wf = CG_RagdollWeaponForce( rdShooter, len ) * CG_RagdollMassScale( cg_entities[best->entityNum].currentState.aiChar );
		// a body takes only so much shoving in a short time, so automatic
		// fire jostles it rather than pushing it along the floor
		if ( cg.time - best->shoveTime > 250 ) {
			best->shoveTime = cg.time;
			best->shoved = 0;
		}
		if ( best->shoved + wf > 5.0f ) {
			wf = 5.0f - best->shoved;
		}
		if ( wf > 0 ) {
			// Extreme: a heavy round into a limb takes it off
			if ( wf >= 3.0f && bestCap >= 2 ) {
				CG_RagdollSever( best, rdCaps[bestCap].a, dir );
			}
			best->shoved += wf;
			// a body that keeps taking heavy fire comes apart
			if ( CG_RagdollHeavyShooter( rdShooter ) ) {
				best->corpseDamage += wf;
				if ( CG_RagdollGibCorpse( best, dir ) ) {
					return best->entityNum;
				}
			}
			VectorScale( dir, 55 * wf * f, vel );
			vel[2] += 7 * sqrt( wf ) * ( f > 1 ? f * 0.6f : 1 );
			if ( cg.time - best->startTime < 300 ) {
				// still falling from the killing shot: add to that fall
				// rather than a jolt of its own
				VectorScale( vel, 0.6f, vel );
				CG_RagdollKick( best, hit, vel, 18.0f, 0.3f );
			} else {
				CG_RagdollKick( best, hit, vel, 10.0f, f > 1 ? 0.5f : 0.12f );
			}
		}
	}
	if ( !cg_xbKickHit ) {     // a kick: no blood, no bullet hole
		if ( best->entityNum < MAX_CLIENTS ) {
			CG_Bleed( hit, best->entityNum );
		}
		CG_GoreSpray( hit, dir, CG_RagdollWeaponForce( rdShooter, len ), best->entityNum );
		if ( CG_RagdollWeaponForce( rdShooter, len ) >= 2.5f ) {
			vec3_t farSide, back;
			VectorMA( hit, 30, dir, farSide );
			VectorNegate( dir, back );
			CG_RagdollAddWound( best->entityNum, farSide, back );
		}
		CG_RagdollAddWound( best->entityNum, hit, dir );
	}
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
		CG_Trace( &tr, origin, NULL, NULL, rd->p[J_PELVIS], rd->entityNum, RD_MASK );
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
		// Extreme: right next to it, limbs come off
		// a blast right on it takes the body apart; further out it
		// adds up with heavy fire
		{
			float d = Distance( rd->p[J_PELVIS], origin );
			vec3_t up;
			VectorSubtract( rd->p[J_PELVIS], origin, up );
			VectorNormalize( up );
			if ( d < radius ) {
				rd->corpseDamage += 24.0f * ( 1.0f - d / radius ) * ( 1.0f - d / radius ) + ( d < 48 ? 18.0f : 0 );
			}
			if ( CG_RagdollGibCorpse( rd, up ) ) {
				continue;
			}
		}
		// (the limbs nearest the blast: one within 110 units, two within 60)
		if ( Distance( rd->p[J_PELVIS], origin ) < 110 ) {
			static const int roots[4] = { J_LSHOULDER, J_RSHOULDER, J_LHIP, J_RHIP };
			int k, n, take = Distance( rd->p[J_PELVIS], origin ) < 60 ? 2 : 1, used = 0;
			for ( n = 0; n < take; n++ ) {
				int pick = -1;
				float pd = 1e9f;
				vec3_t away;
				for ( k = 0; k < 4; k++ ) {
					float dd = Distance( rd->p[roots[k]], origin );
					if ( !( used & ( 1 << k ) ) && dd < pd ) {
						pd = dd;
						pick = k;
					}
				}
				if ( pick < 0 ) {
					break;
				}
				used |= 1 << pick;
				VectorSubtract( rd->p[roots[pick]], origin, away );
				VectorNormalize( away );
				CG_RagdollSever( rd, roots[pick], away );
			}
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
	CG_Trace( &tr, rd->p[J_PELVIS], NULL, NULL, down, rd->entityNum, RD_MASK );
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
int cg_perfMs[CGPERF_NUM];

void CG_RagdollPerfFrame( void ) {
	static int start, frames, bodies, moving, steps, ms, last, part[CGPERF_NUM];
	int now = trap_Milliseconds(), k;

	frames++;
	bodies += rdPerfBodies;
	moving += rdPerfMoving;
	steps += rdPerfSteps;
	ms += rdPerfMs;
	for ( k = 0; k < CGPERF_NUM; k++ ) {
		part[k] += cg_perfMs[k];
		cg_perfMs[k] = 0;
	}
	rdPerfBodies = rdPerfMoving = rdPerfSteps = rdPerfMs = rdFrameSteps = 0;
	last = now;
	if ( !start ) {
		start = now;
	}
	if ( now - start >= 2000 ) {
		float avg = (float)( now - start ) / frames;
		if ( avg > 22.0f ) {
			CG_Printf( "perf(cgame): %.0f FPS | bodies drawn %.1f, moving %.1f, %.1f sim steps, ragdoll %.1f ms a frame | "
					   "entities %.1f marks %.1f gore %.1f particles %.1f local %.1f ms | %i marks (%i drawn) %i drops %i limbs\n",
					   1000.0f / avg, (float)bodies / frames, (float)moving / frames, (float)steps / frames, (float)ms / frames,
					   (float)part[CGPERF_ENTS] / frames, (float)part[CGPERF_MARKS] / frames, (float)part[CGPERF_GORE] / frames,
					   (float)part[CGPERF_PARTICLES] / frames, (float)part[CGPERF_LOCALENTS] / frames,
					   CG_MarksActive(), cg_marksDrawn, CG_GoreDropsActive(), CG_RagdollLimbsActive() );
		}
		start = now;
		frames = bodies = moving = steps = ms = 0;
		memset( part, 0, sizeof( part ) );
	}
}

/*
Every frame for each body: is it burning (catching fire after death too),
the light of the flames, scorch marks where it rolls, smoke once it's out.
*/
static void CG_RagdollBurnFrame( ragdoll_t *rd, centity_t *cent ) {
	qboolean onFire = CG_EntOnFire( cent ) && ( rd->rig == RD_RIG_HUMAN || rd->rig == RD_RIG_HUMAN2 ) &&
					  CG_RagdollBleeds( cent->currentState.aiChar ) && cg_ragdoll.integer;
	int n = rd->entityNum;
	if ( onFire ) {
		if ( !rd->burning ) {
			rd->burning = qtrue;
			rd->rollSign = ( rand() & 1 ) ? 1.0f : -1.0f;
			rd->rollFlip = cg.time + 400 + rand() % 400;
			rd->phase[0] = random() * 6.28f;
			rd->phase[1] = random() * 6.28f;
			CG_Printf( "ragdoll: body %i burning (%i ms of fire left)\n", n, cent->currentState.onFireEnd - cg.time );
		}
		if ( CG_BloodExtreme() && rdBurn[n] != 1 ) {
			rdBurn[n] = 1;      // set alight after death
			rdBurnStart[n] = cg.time;
		}
		rd->burnLastSeen = cg.time;
		if ( rd->sleeping ) {
			CG_RagdollWake( rd );
		}
		{
			float k = ( cent->currentState.onFireEnd - cg.time ) / (float)RD_BURN_FADE;
			k = k > 1 ? 1 : k;
			trap_R_AddLightToScene( rd->p[J_CHEST], ( 140 + ( rand() & 63 ) ) * k + 40, 1.0f, 0.55f, 0.15f, 0 );
		}
		// the floor it rolls over gets scorched
		if ( cg.time >= rd->nextScorch || rd->nextScorch - cg.time > 1000 ) {
			trace_t tr;
			vec3_t end;
			rd->nextScorch = cg.time + 220;
			VectorCopy( rd->p[J_CHEST], end );
			end[2] -= 24;
			CG_Trace( &tr, rd->p[J_CHEST], NULL, NULL, end, n, MASK_SOLID );
			if ( tr.fraction < 1.0f && tr.plane.normal[2] > 0.5f && cgs.media.burnMarkShader ) {
				CG_ImpactMark( cgs.media.burnMarkShader, tr.endpos, tr.plane.normal, random() * 360,
							   1, 1, 1, 0.6f, qtrue, 10 + random() * 6, qfalse, 30000 );
			}
		}
	} else if ( rd->burning ) {
		rd->burning = qfalse;
	}
	// smoking for a few seconds after the flames go out
	if ( !onFire && rd->burnLastSeen && cg.time - rd->burnLastSeen < 5000 && cg.time >= rd->nextScorch ) {
		vec3_t vel = { 0, 0, 30 };
		float a = 1.0f - ( cg.time - rd->burnLastSeen ) / 5000.0f;
		rd->nextScorch = cg.time + 120;
		vel[0] = crandom() * 6;
		vel[1] = crandom() * 6;
		CG_SmokePuff( rd->p[J_CHEST], vel, 14 + random() * 8, 0.25f, 0.25f, 0.25f, 0.5f * a, 1800, cg.time, 0, 0,
					  cgs.media.smokePuffShader );
	}
}

void CG_RagdollBody( centity_t *cent, refEntity_t *body ) {
	ragdoll_t *rd;
	float dt;

	// bullet wounds drawn on the skin (alive or dead)
	if ( cent->currentState.number != cg.snap->ps.clientNum && CG_RagdollBleeds( cent->currentState.aiChar ) ) {
		body->reFlags |= REFLAG_WOUNDS;
		if ( CG_BloodExtreme() ) {
			body->reFlags |= REFLAG_WOUNDS_SOAK;    // wounds soak out through the clothes
		}
		if ( cg_bloodMode.integer == BLOODMODE_GREEN ) {
			body->reFlags |= REFLAG_WOUNDS_GREEN;
		}
	} else if ( cent->currentState.number != cg.snap->ps.clientNum && CG_RagdollDryWounds( cent->currentState.aiChar ) ) {
		body->reFlags |= REFLAG_WOUNDS | REFLAG_WOUNDS_DUST;
	}
	body->reFlags |= CG_RagdollBurnFlags( cent->currentState.number );    // Extreme: burnt / scorched
	body->charLevel = CG_RagdollCharLevel( cent->currentState.number );

	rd = CG_RagdollFind( cent->currentState.number );
	if ( ( cent->currentState.eFlags & EF_DEAD ) && cg_ragdoll.integer && !CG_RagdollEligible( cent ) &&
		 cent->currentState.number != cg.snap->ps.clientNum ) {
		// (log the skeleton of the characters left out too, once per model:
		// to map them later)
		CG_RagdollWhyNot( cent, body, "left out (special death)" );
	}
	if ( !( cent->currentState.eFlags & EF_DEAD ) || !CG_RagdollEligible( cent ) ) {
		if ( rd ) {     // revived / respawned / turned off
			trap_R_SetRagdoll( rd->slot, NULL );
			rd->active = qfalse;
		}
		rdDeclined[cent->currentState.number] = 0;
		if ( !( cent->currentState.eFlags & EF_DEAD ) ) {
			rdKeepValid[cent->currentState.number] = qfalse;   // alive again: no pose to keep
			rdHeadless[cent->currentState.number] = 0;
			rdGibbed[cent->currentState.number] = 0;
			rdBurn[cent->currentState.number] = 0;
			rdBurnStart[cent->currentState.number] = 0;
		}
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
		int en = cent->currentState.number;
		if ( en >= 0 && en < MAX_GENTITIES && rdKeepValid[en] && rdKeep[en].aiChar == cent->currentState.aiChar ) {
			sv = &rdKeep[en];   // (kept: the live ragdoll is written from now on)
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
	CG_RagdollBurnFrame( rd, cent );

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
		if ( RD_ISDOG( rd->rig ) && !rd->dogTwistLogged ) {
			float tw = CG_RagdollDogTwist( rd );
			if ( tw > 90 ) {
				rd->dogTwistLogged = qtrue;
				CG_Printf( "ragdoll: dog %i twisted %.0f degrees front to back\n", rd->entityNum, tw );
			}
		}
		// how fast it's moving early in the fall (tuning the death push)
		{
			static const int at[3] = { 100, 500, 1000 };
			static int logged;
			int k, age = cg.time - rd->startTime;
			for ( k = 0; k < 3; k++ ) {
				if ( age >= at[k] && !( rd->logMask & ( 1 << k ) ) ) {
					rd->logMask |= 1 << k;
					if ( logged < 120 ) {
						logged++;
						CG_Printf( "ragdoll: body %i at %i ms: pelvis %.0f, head %.0f units/s\n", rd->entityNum, at[k],
								   Distance( rd->p[J_PELVIS], rd->old[J_PELVIS] ) / RD_STEP,
								   Distance( rd->p[J_HEAD], rd->old[J_HEAD] ) / RD_STEP );
					}
				}
			}
		}
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
	rd->lastRef = *body;
	rd->haveRef = qtrue;
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

// a body's pose into rdKeep (what saves write)
static void CG_RagdollKeepPose( const ragdoll_t *rd ) {
	rdSaved_t *sv;
	if ( !rd->active || rd->entityNum < 0 || rd->entityNum >= MAX_GENTITIES ) {
		return;
	}
	sv = &rdKeep[rd->entityNum];
	memset( sv, 0, sizeof( *sv ) );
	sv->entityNum = rd->entityNum;
	sv->aiChar = cg_entities[rd->entityNum].currentState.aiChar;
	sv->wound = rd->wound;
	sv->frame = rd->pose.frame;
	sv->oldframe = rd->pose.oldframe;
	sv->torsoFrame = rd->pose.torsoFrame;
	sv->oldTorsoFrame = rd->pose.oldTorsoFrame;
	sv->backlerp = rd->pose.backlerp;
	sv->torsoBacklerp = rd->pose.torsoBacklerp;
	memcpy( sv->axis, rd->pose.axis, sizeof( sv->axis ) );
	memcpy( sv->torsoAxis, rd->pose.torsoAxis, sizeof( sv->torsoAxis ) );
	VectorCopy( rd->pose.origin, sv->origin );
	memcpy( sv->p, rd->p, sizeof( sv->p ) );
	rdKeepValid[rd->entityNum] = qtrue;
}

static void CG_RagdollWriteSave( const char *username ) {
	static ragdoll_t *live[MAX_GENTITIES];
	char path[MAX_QPATH], file[MAX_QPATH];
	fileHandle_t f;
	int i, n = 0, magic = RD_SAVE_MAGIC;

	Com_sprintf( file, sizeof( file ), "save/%s", username );
	CG_RagdollSaveName( file, path, sizeof( path ) );
	if ( trap_FS_FOpenFile( path, &f, FS_WRITE ) < 0 ) {
		return;
	}
	memset( live, 0, sizeof( live ) );
	for ( i = 0; i < RD_MAX; i++ ) {
		if ( rdBodies[i].active && rdBodies[i].entityNum >= 0 && rdBodies[i].entityNum < MAX_GENTITIES ) {
			live[rdBodies[i].entityNum] = &rdBodies[i];
		}
	}
	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		if ( live[i] || ( rdKeepValid[i] && ( cg_entities[i].currentState.eFlags & EF_DEAD ) ) ) {
			n++;
		}
	}
	trap_FS_Write( &magic, sizeof( magic ), f );
	trap_FS_Write( cgs.mapname, MAX_QPATH, f );
	trap_FS_Write( &n, sizeof( n ), f );
	for ( i = 0; i < MAX_GENTITIES; i++ ) {
		if ( live[i] ) {
			CG_RagdollKeepPose( live[i] );  // (and up to date for the next save)
		} else if ( !rdKeepValid[i] || !( cg_entities[i].currentState.eFlags & EF_DEAD ) ) {
			continue;
		}
		trap_FS_Write( &rdKeep[i], sizeof( rdSaved_t ), f );
	}
	trap_FS_FCloseFile( f );
	CG_Printf( "ragdoll: kept %i body poses with save '%s'\n", n, username );
}

static void CG_RagdollLoadSaved( void ) {
	char name[MAX_QPATH], path[MAX_QPATH], map[MAX_QPATH];
	fileHandle_t f;
	int len, magic = 0, n = 0, i;

	memset( rdKeepValid, 0, sizeof( rdKeepValid ) );
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
	if ( magic != RD_SAVE_MAGIC || Q_stricmp( map, cgs.mapname ) || n < 0 || n > MAX_GENTITIES ||
		 len < (int)( sizeof( int ) * 2 + MAX_QPATH + n * sizeof( rdSaved_t ) ) ) {
		trap_FS_FCloseFile( f );
		return;
	}
	for ( i = 0; i < n; i++ ) {
		rdSaved_t sv;
		trap_FS_Read( &sv, sizeof( sv ), f );
		if ( sv.entityNum >= 0 && sv.entityNum < MAX_GENTITIES ) {
			rdKeep[sv.entityNum] = sv;
			rdKeepValid[sv.entityNum] = qtrue;
		}
	}
	trap_FS_FCloseFile( f );
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


/*
===========================================================================
Dismemberment (Extreme)

A very hard hit on an arm or a leg (a rifle round, a close shotgun blast,
an explosion right next to it) cuts it off. The body draws that part shrunk
into the stump; the limb is a copy of the body with only that part showing,
simulated as a short chain of joints that falls, bounces and comes to rest.
===========================================================================
*/
#define RD_MAX_LIMBS    12      // (no more than REF_LIMB_ENTITIES)
#define RD_LIMB_SLOT0   ( RD_MAX + 1 )      // renderer slots after the bodies'

typedef struct {
	qboolean active;
	int startTime;
	ragdoll_t r;            // copy of the body; only the chain joints move
	refEntity_t ref;
	int keepMask;           // segments drawn
	int chain[3], n;        // joints simulated, root first
	float len[2];
	vec3_t root0, others0[J_NUM];
	int still;
	qboolean asleep;
} rdLimb_t;

static rdLimb_t rdLimbsActive[RD_MAX_LIMBS];

int CG_RagdollLimbsActive( void ) {
	int i, n = 0;
	for ( i = 0; i < RD_MAX_LIMBS; i++ ) {
		n += rdLimbsActive[i].active;
	}
	return n;
}

void CG_RagdollLimbsReset( void ) {
	int i;
	for ( i = 0; i < RD_MAX_LIMBS; i++ ) {
		if ( rdLimbsActive[i].active ) {
			trap_R_SetRagdoll( RD_LIMB_SLOT0 + i, NULL );
		}
	}
	memset( rdLimbsActive, 0, sizeof( rdLimbsActive ) );
}

// which part a hit at 'joint' cuts off; qfalse if it isn't a limb
static qboolean CG_RagdollCutFor( int joint, int *mask, int chain[3], int *n ) {
	switch ( joint ) {
	case J_LSHOULDER: *mask = ( 1 << 3 ) | ( 1 << 4 ); chain[0] = J_LSHOULDER; chain[1] = J_LELBOW; chain[2] = J_LHAND; *n = 3; return qtrue;
	case J_LELBOW: case J_LHAND: *mask = 1 << 4; chain[0] = J_LELBOW; chain[1] = J_LHAND; *n = 2; return qtrue;
	case J_RSHOULDER: *mask = ( 1 << 5 ) | ( 1 << 6 ); chain[0] = J_RSHOULDER; chain[1] = J_RELBOW; chain[2] = J_RHAND; *n = 3; return qtrue;
	case J_RELBOW: case J_RHAND: *mask = 1 << 6; chain[0] = J_RELBOW; chain[1] = J_RHAND; *n = 2; return qtrue;
	case J_LHIP: *mask = ( 1 << 7 ) | ( 1 << 8 ); chain[0] = J_LHIP; chain[1] = J_LKNEE; chain[2] = J_LFOOT; *n = 3; return qtrue;
	case J_LKNEE: case J_LFOOT: *mask = 1 << 8; chain[0] = J_LKNEE; chain[1] = J_LFOOT; *n = 2; return qtrue;
	case J_RHIP: *mask = ( 1 << 9 ) | ( 1 << 10 ); chain[0] = J_RHIP; chain[1] = J_RKNEE; chain[2] = J_RFOOT; *n = 3; return qtrue;
	case J_RKNEE: case J_RFOOT: *mask = 1 << 10; chain[0] = J_RKNEE; chain[1] = J_RFOOT; *n = 2; return qtrue;
	default: return qfalse;
	}
}

// cut off the part a hit at 'joint' takes, flying along 'dir'
static void CG_RagdollSever( ragdoll_t *rd, int joint, const vec3_t dir ) {
	int mask, chain[3], n, i, s, slot, oldest = 0;
	rdLimb_t *l = NULL;

	if ( !CG_BloodExtreme() || RD_ISDOG( rd->rig ) || !rd->haveRef || CG_Skull( 1 << 7 ) ||
		 !CG_RagdollBleeds( cg_entities[rd->entityNum].currentState.aiChar ) ||
		 !CG_RagdollCutFor( joint, &mask, chain, &n ) || ( rd->severedMask & mask ) ) {
		return;
	}
	for ( i = 0; i < RD_MAX_LIMBS; i++ ) {
		if ( !rdLimbsActive[i].active ) {
			l = &rdLimbsActive[i];
			break;
		}
		if ( rdLimbsActive[i].startTime < rdLimbsActive[oldest].startTime ) {
			oldest = i;
		}
	}
	if ( !l ) {
		l = &rdLimbsActive[oldest];
	}
	slot = RD_LIMB_SLOT0 + (int)( l - rdLimbsActive );
	memset( l, 0, sizeof( *l ) );
	l->active = qtrue;
	l->startTime = cg.time;
	l->r = *rd;
	l->r.slot = slot;
	l->r.severedMask = 0;
	l->keepMask = mask;
	l->n = n;
	for ( i = 0; i < n; i++ ) {
		l->chain[i] = chain[i];
	}
	for ( i = 0; i + 1 < n; i++ ) {
		l->len[i] = Distance( rd->p[chain[i]], rd->p[chain[i + 1]] );
	}
	VectorCopy( rd->p[chain[0]], l->root0 );
	memcpy( l->others0, rd->p, sizeof( l->others0 ) );
	// thrown along the shot, tumbling a little
	for ( i = 0; i < n; i++ ) {
		vec3_t v;
		VectorScale( dir, 160 + random() * 80 + 40 * i, v );
		v[2] += 90 + random() * 60;
		v[0] += crandom() * 30 * i;
		v[1] += crandom() * 30 * i;
		VectorMA( l->r.p[chain[i]], -RD_STEP, v, l->r.old[chain[i]] );
	}
	l->ref = rd->lastRef;
	// its own entity number: its own wound list, apart from the body's (the
	// body's wounds were found on the whole body and don't fit the limb)
	l->ref.entityNum = REF_LIMB_ENTITY0 + (int)( l - rdLimbsActive );
	l->ref.reFlags &= ~( REFLAG_ZOMBIEFX | REFLAG_ZOMBIEFX2 );
	trap_R_AddWound( l->ref.entityNum, NULL, NULL );

	// the body: that part shrinks into the stump, which bleeds
	for ( s = 0; s < RD_SEGS; s++ ) {
		if ( mask & ( 1 << s ) ) {
			rd->severRoot[s] = (byte)chain[0];
		}
	}
	rd->severedMask |= mask;
	rd->wound = chain[0];       // (the squirt comes from here now)
	{
		// a raw wound on both cut ends: the body's looking in from the limb's
		// side, the limb's looking in from the body's
		vec3_t in;
		VectorSubtract( rd->p[chain[0]], rd->p[chain[1]], in );
		VectorNormalize( in );
		CG_RagdollAddWound( rd->entityNum, rd->p[chain[0]], in );
		VectorNegate( in, in );
		trap_R_AddWound( l->ref.entityNum, rd->p[chain[0]], in );
	}
	CG_GoreHeadBurst( rd->p[chain[0]], dir, rd->entityNum );
	trap_S_StartSound( rd->p[chain[0]], ENTITYNUM_WORLD, CHAN_AUTO, cgs.media.gibSound );
	CG_RagdollWake( rd );
	CG_Printf( "ragdoll: body %i lost a limb (joint %i)\n", rd->entityNum, joint );
}

static void CG_RagdollLimbStep( rdLimb_t *l ) {
	int i, it;
	float moved = 0;
	vec3_t start[3], shift;
	for ( i = 0; i < l->n; i++ ) {
		float *p = l->r.p[l->chain[i]], *o = l->r.old[l->chain[i]];
		vec3_t v;
		VectorCopy( p, start[i] );
		VectorSubtract( p, o, v );
		VectorScale( v, RD_AIRDRAG, v );
		VectorCopy( p, o );
		VectorAdd( p, v, p );
		p[2] -= RD_GRAVITY * CG_XbGravityScale() * RD_STEP * RD_STEP;
	}
	for ( it = 0; it < 4; it++ ) {
		for ( i = 0; i + 1 < l->n; i++ ) {
			float *a = l->r.p[l->chain[i]], *b = l->r.p[l->chain[i + 1]];
			vec3_t d;
			float len = Distance( a, b ), diff;
			if ( len < 0.001f ) {
				continue;
			}
			VectorSubtract( b, a, d );
			diff = ( len - l->len[i] ) / len * 0.5f;
			VectorMA( a, diff, d, a );
			VectorMA( b, -diff, d, b );
		}
	}
	for ( i = 0; i < l->n; i++ ) {
		float *p = l->r.p[l->chain[i]], *o = l->r.old[l->chain[i]];
		trace_t tr;
		vec3_t mins = { -2, -2, -2 }, maxs = { 2, 2, 2 };
		CG_Trace( &tr, start[i], mins, maxs, p, l->r.entityNum, RD_MASK );
		if ( tr.allsolid ) {
			VectorCopy( start[i], p );
			VectorCopy( start[i], o );
		} else if ( tr.fraction < 1.0f ) {
			vec3_t v, vt;
			float dn;
			VectorSubtract( p, o, v );
			VectorMA( tr.endpos, 0.05f, tr.plane.normal, p );
			dn = DotProduct( v, tr.plane.normal );
			VectorMA( v, -dn, tr.plane.normal, vt );
			VectorMA( p, -0.6f, vt, o );        // grips and rolls to a stop
			if ( dn < -2.0f && CG_GoreEnabled_Ragdoll() ) {
				CG_ImpactMark( CG_BloodDot(), tr.endpos, tr.plane.normal, random() * 360,
							   1, 1, 1, 1, qtrue, 4 + random() * 4, qfalse, CG_BloodMarkTime() );
			}
		}
		moved += Distance( p, start[i] );
	}
	// the rest of the skeleton rides along with the cut end (only so the
	// limb's frames stay well defined; those parts aren't drawn)
	VectorSubtract( l->r.p[l->chain[0]], l->root0, shift );
	for ( i = 0; i < J_NUM; i++ ) {
		int k, inChain = 0;
		for ( k = 0; k < l->n; k++ ) {
			inChain |= ( l->chain[k] == i );
		}
		if ( !inChain ) {
			VectorAdd( l->others0[i], shift, l->r.p[i] );
			VectorCopy( l->r.p[i], l->r.old[i] );
		}
	}
	l->r.steps++;
	if ( moved < 0.05f * l->n ) {
		if ( ++l->still > 45 ) {
			l->asleep = qtrue;
		}
	} else {
		l->still = 0;
	}
}

void CG_RagdollAddLimbs( void ) {
	int i, s;
	for ( i = 0; i < RD_MAX_LIMBS; i++ ) {
		rdLimb_t *l = &rdLimbsActive[i];
		vec3_t root;
		if ( !l->active ) {
			continue;
		}
		if ( cg.time - l->startTime > 180000 || !CG_BloodExtreme() ) {
			trap_R_SetRagdoll( l->r.slot, NULL );
			l->active = qfalse;
			continue;
		}
		if ( !l->asleep ) {
			int steps = 0;
			l->r.accum += cg.frametime * 0.001f;
			if ( l->r.accum > 0.1f ) {
				l->r.accum = 0.1f;
			}
			while ( l->r.accum >= RD_STEP && steps < 3 ) {
				CG_RagdollLimbStep( l );
				l->r.accum -= RD_STEP;
				steps++;
			}
			CG_RagdollPose( &l->r );
			CG_RagdollToModel( &l->r.pose, l->r.p[l->chain[0]], root );
			for ( s = 0; s < RD_SEGS; s++ ) {
				if ( !( l->keepMask & ( 1 << s ) ) ) {
					CG_RagdollCollapseSeg( &l->r, s, root );
				}
			}
		}
		trap_R_SetRagdoll( l->r.slot, &l->r.data[0][0] );
		l->ref.ragdoll = l->r.slot;
		l->ref.ragdollStamp = ++rdStamp;
		VectorCopy( l->r.p[l->chain[0]], l->ref.lightingOrigin );
		l->ref.renderfx |= RF_LIGHTING_ORIGIN;
		trap_R_AddRefEntityToScene( &l->ref );
	}
}
