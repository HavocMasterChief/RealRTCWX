/*
===========================================================================
RealRTCWX (Xbox): Skulls, the game (server) side

Bits in g_xbskulls (bg_xbskulls.h), set from the Cheats menu. The hooks are
small calls from the code they change (g_combat.c, g_weapon.c, g_cmds.c,
ai_cast_fight.c, ai_cast_funcs.c); what each skull does lives here.
===========================================================================
*/

#include "g_local.h"
#include "bg_xbskulls.h"

extern void AICast_AudibleEvent( int srcnum, vec3_t pos, float range );

int G_Skulls( void ) {
	return trap_Cvar_VariableIntegerValue( "g_xbskulls" );
}

qboolean G_Skull( int bit ) {
	return ( G_Skulls() & bit ) ? qtrue : qfalse;
}

// the human player (not an AI), as attacker
static qboolean G_SkullIsPlayer( gentity_t *ent ) {
	return ent && ent->client && !( ent->r.svFlags & SVF_CASTAI ) ? qtrue : qfalse;
}

/*
===============
Mythic: enemies take half damage from you (double health)
===============
*/
int G_SkullDamage( gentity_t *targ, gentity_t *attacker, int damage ) {
	if ( targ && targ->aiCharacter && G_SkullIsPlayer( attacker ) && G_Skull( SKULL_MYTHIC ) ) {
		damage = ( damage + 1 ) / 2;
	}
	return damage;
}

/*
===============
Kills by the player: Vampire, Party Pooper, Bullet Time, Gun Game
===============
*/
static int xbSlowUntil;     // real time (ms) Bullet Time ends

static const int xbGunPool[] = {
	WP_LUGER, WP_COLT, WP_TT33, WP_REVOLVER, WP_MP40, WP_THOMPSON, WP_STEN, WP_PPSH,
	WP_MP34, WP_MAUSER, WP_GARAND, WP_MOSIN, WP_M1GARAND, WP_G43, WP_M1941, WP_MP44,
	WP_FG42, WP_BAR, WP_M97, WP_AUTO5, WP_BROWNING, WP_MG42M, WP_PANZERFAUST,
	WP_FLAMETHROWER, WP_VENOM, WP_TESLA
};

// Gun Game: only guns this install has. The DLC guns need the DLC (and
// its setting on), and a gun whose weapon file or model isn't in the
// game data isn't offered at all (checked once a level)
static int xbGunOkLevel = -1;
static qboolean xbGunOk[WP_NUM_WEAPONS];

static qboolean G_SkullFileExists( const char *path ) {
	fileHandle_t f;
	int len;
	if ( !path || !path[0] ) {
		return qtrue;   // (nothing to check)
	}
	len = trap_FS_FOpenFile( path, &f, FS_READ );
	if ( f ) {
		trap_FS_FCloseFile( f );
	}
	return len > 0 ? qtrue : qfalse;
}

static qboolean G_SkullGunOk( int w ) {
	if ( xbGunOkLevel != level.startTime ) {
		int i;
		xbGunOkLevel = level.startTime;
		memset( xbGunOk, 0, sizeof( xbGunOk ) );
		for ( i = 0; i < (int)( sizeof( xbGunPool ) / sizeof( xbGunPool[0] ) ); i++ ) {
			int g = xbGunPool[i];
			gitem_t *item = BG_FindItemForWeapon( g );
			char *file = BG_GetWeaponFilename( g );
			qboolean ok = qtrue;
			switch ( g ) {
			case WP_M1941: case WP_M1941SCOPE: case WP_DELISLE: case WP_DELISLESCOPE: case WP_AUTO5:
				ok = g_dlc1.integer ? qtrue : qfalse;
				break;
			default:
				break;
			}
			if ( ok && !item ) {
				ok = qfalse;
			}
			if ( ok && file && !G_SkullFileExists( va( "weapons/%s", file ) ) ) {
				ok = qfalse;
			}
			if ( ok && item && !G_SkullFileExists( item->world_model[0] ) ) {
				ok = qfalse;
			}
			xbGunOk[g] = ok;
			if ( g_cheats.integer && !ok ) {
				G_DPrintf( "Gun Game: leaving out weapon %i (not in this install)\n", g );
			}
		}
	}
	return ( w > WP_NONE && w < WP_NUM_WEAPONS ) ? xbGunOk[w] : qfalse;
}

void G_SkullKill( gentity_t *targ, gentity_t *attacker, int mod, qboolean headshot, const vec3_t point ) {
	int bits = G_Skulls();
	int num = attacker - g_entities;

	if ( !bits || !targ || !targ->aiCharacter || !G_SkullIsPlayer( attacker ) ) {
		return;
	}
	if ( ( bits & SKULL_VAMPIRE ) && attacker->health > 0 ) {
		int maxh = attacker->client->ps.stats[STAT_MAX_HEALTH];
		attacker->health += 15;
		if ( attacker->health > maxh ) {
			attacker->health = maxh;
		}
		attacker->client->ps.stats[STAT_HEALTH] = attacker->health;
	}
	if ( bits & SKULL_DRUNK ) {
		trap_SendServerCommand( num, "xbkill" );   // sobers you up a little (cgame)
	}
	if ( headshot && ( bits & SKULL_PARTY ) ) {
		vec3_t o;
		VectorCopy( point ? point : targ->r.currentOrigin, o );
		if ( !point ) {
			o[2] += targ->client ? targ->client->ps.viewheight : 40;
		}
		trap_SendServerCommand( num, va( "xbparty %i %i %i", (int)o[0], (int)o[1], (int)o[2] ) );
	}
	if ( headshot && ( bits & SKULL_BULLETTIME ) ) {
		trap_Cvar_Set( "timescale", "0.3" );
		xbSlowUntil = trap_Milliseconds() + 1600;
	}
	if ( bits & SKULL_GUNGAME ) {
		int w = WP_NONE, i, n = 0, pick[sizeof( xbGunPool ) / sizeof( xbGunPool[0] )];
		for ( i = 0; i < (int)( sizeof( xbGunPool ) / sizeof( xbGunPool[0] ) ); i++ ) {
			if ( G_SkullGunOk( xbGunPool[i] ) && xbGunPool[i] != attacker->client->ps.weapon ) {
				pick[n++] = xbGunPool[i];
			}
		}
		if ( n == 0 ) {
			return;
		}
		w = pick[rand() % n];
		COM_BitSet( attacker->client->ps.weapons, w );
		Add_Ammo( attacker, w, ammoTable[w].maxclip * 4, qtrue );
		trap_SendServerCommand( num, va( "xbgun %i", w ) );
	}
	(void)mod;
}

/*
===============
Bullets
===============
*/
static int xbLastBlastTime;
static gentity_t *xbLastBlastEnt;

// a small blast (Explosive Rounds) or a corpse going up (Kaboom)
static void G_SkullBlast( gentity_t *attacker, const vec3_t pos, qboolean big ) {
	gentity_t *tent;
	vec3_t up = { 0, 0, 1 }, p;
	VectorCopy( pos, p );
	tent = G_TempEntity( p, big ? EV_MISSILE_MISS : EV_MISSILE_MISS_SMALL );
	tent->s.weapon = WP_GRENADE_LAUNCHER;
	tent->s.eventParm = DirToByte( up );
	// the player is never hurt by these
	G_RadiusDamage( p, attacker, big ? 120 : 25, big ? 220 : 90, attacker, MOD_GRENADE_SPLASH );
}

// the player's bullet hit someone (alive or dead): Explosive Rounds
void G_SkullBulletHit( gentity_t *attacker, gentity_t *traceEnt, const vec3_t pos ) {
	if ( !G_SkullIsPlayer( attacker ) || !traceEnt || traceEnt == attacker || !traceEnt->client ||
		 !G_Skull( SKULL_EXPLOSIVE ) ) {
		return;
	}
	// every bullet that hits blasts; a shotgun's pellets hitting the same
	// target in the same frame make one bang, not ten
	if ( level.time == xbLastBlastTime && traceEnt == xbLastBlastEnt ) {
		return;
	}
	xbLastBlastTime = level.time;
	xbLastBlastEnt = traceEnt;
	G_SkullBlast( attacker, pos, qfalse );
}

// Toy Soldiers: the enemy is drawn at half size, so a bullet through
// the empty top half of its box carries on
qboolean G_SkullToyMiss( gentity_t *attacker, gentity_t *traceEnt, const vec3_t pos ) {
	float z, full;
	if ( !G_SkullIsPlayer( attacker ) || !traceEnt || !traceEnt->client || !traceEnt->aiCharacter ||
		 traceEnt->health <= 0 || !G_Skull( SKULL_TOY ) ) {
		return qfalse;
	}
	z = pos[2] - ( traceEnt->r.currentOrigin[2] + traceEnt->r.mins[2] );
	full = traceEnt->r.maxs[2] - traceEnt->r.mins[2];
	return z > full * 0.55f ? qtrue : qfalse;
}

// Big Head: the heads are drawn twice the size, and the top of one sticks
// out above the enemy's box, so a bullet through it would miss everything.
// Moves the trace's hit to the first big head on the bullet's way.
void G_SkullBigHeadTrace( gentity_t *attacker, gentity_t *source, const vec3_t start, const vec3_t end, trace_t *tr ) {
	vec3_t dir;
	float len, bestT;
	int i, best = -1;

	if ( !G_SkullIsPlayer( attacker ) || !G_Skull( SKULL_BIGHEAD ) ) {
		return;
	}
	VectorSubtract( end, start, dir );
	len = VectorNormalize( dir );
	if ( len < 1 ) {
		return;
	}
	bestT = tr->fraction * len;
	for ( i = 0; i < level.maxclients; i++ ) {
		gentity_t *e = &g_entities[i];
		vec3_t org;
		float half, tmin = 0, tmax = bestT;
		int k;
		if ( !e->inuse || !e->client || !e->aiCharacter || e->health <= 0 || !e->takedamage ||
			 e == source || e == attacker || tr->entityNum == i ) {
			continue;
		}
		G_XbHeadBox( e, org, &half );
		// ray against the box (slabs)
		for ( k = 0; k < 3; k++ ) {
			float lo = org[k] - half, hi = org[k] + half;
			if ( fabs( dir[k] ) < 1e-6f ) {
				if ( start[k] < lo || start[k] > hi ) {
					break;
				}
			} else {
				float t1 = ( lo - start[k] ) / dir[k], t2 = ( hi - start[k] ) / dir[k];
				if ( t1 > t2 ) {
					float t = t1; t1 = t2; t2 = t;
				}
				if ( t1 > tmin ) {
					tmin = t1;
				}
				if ( t2 < tmax ) {
					tmax = t2;
				}
				if ( tmin > tmax ) {
					break;
				}
			}
		}
		if ( k < 3 || tmin >= bestT ) {
			continue;
		}
		bestT = tmin;
		best = i;
	}
	if ( best < 0 ) {
		return;
	}
	// just short of the box: IsHeadShot traces on from here into the head
	if ( bestT > 1 ) {
		bestT -= 1;
	}
	tr->entityNum = best;
	tr->fraction = bestT / len;
	VectorMA( start, bestT, dir, tr->endpos );
	VectorNegate( dir, tr->plane.normal );
	tr->surfaceFlags = 0;
	tr->contents = CONTENTS_BODY;
}

// Ricochet: the player's bullet bounces once off a wall
qboolean G_SkullRicochet( gentity_t *attacker, int recursion ) {
	return ( recursion == 0 && G_SkullIsPlayer( attacker ) && G_Skull( SKULL_RICOCHET ) ) ? qtrue : qfalse;
}

/*
===============
Boot Camp: your kick launches people
===============
*/
qboolean G_SkullKickKills( gentity_t *attacker, gentity_t *traceEnt ) {
	return ( G_SkullIsPlayer( attacker ) && traceEnt && traceEnt->client && traceEnt->aiCharacter &&
			 traceEnt->health > 0 && G_Skull( SKULL_BOOTCAMP ) ) ? qtrue : qfalse;
}

void G_SkullKick( gentity_t *attacker, gentity_t *traceEnt, const vec3_t forward ) {
	// (the kick has killed them by now: their body flies on the client,
	// see CG_XbThrowScale; anything a kick didn't kill still gets thrown)
	if ( !G_SkullIsPlayer( attacker ) || !traceEnt || !traceEnt->client || !traceEnt->aiCharacter ||
		 traceEnt->health <= 0 || !G_Skull( SKULL_BOOTCAMP ) ) {
		return;
	}
	VectorScale( forward, 1100, traceEnt->client->ps.velocity );
	traceEnt->client->ps.velocity[2] = 380;
	traceEnt->client->ps.pm_time = 800;
	traceEnt->client->ps.pm_flags |= PMF_TIME_KNOCKBACK;
	traceEnt->s.groundEntityNum = ENTITYNUM_NONE;
	traceEnt->client->ps.groundEntityNum = ENTITYNUM_NONE;
}

/*
===============
Famine: enemies drop half the ammo
===============
*/
void G_SkullDrop( gentity_t *self, gentity_t *drop, int weapon ) {
	int maxclip, q;
	if ( !drop || !self || !self->aiCharacter || !G_Skull( SKULL_FAMINE ) ) {
		return;
	}
	maxclip = ammoTable[weapon].maxclip;
	q = maxclip > 4 ? (int)( random() * ( maxclip - 4 ) ) + 4 : maxclip;
	drop->count = q / 2 > 0 ? q / 2 : 1;
}

/*
===============
Catch / Silent Assassin (asked by the AI code)
===============
*/
int G_SkullGrenadeDelay( int delay ) {
	return G_Skull( SKULL_CATCH ) ? delay / 4 : delay;
}

qboolean G_SkullSilent( gentity_t *shooter ) {
	return ( G_SkullIsPlayer( shooter ) && G_Skull( SKULL_SILENT ) ) ? qtrue : qfalse;
}

/*
===============
xbblast x y z big - from cgame, when the player's bullets hit a corpse
(corpses are ragdolls on the client, so it sees those hits)
===============
*/
void Cmd_XbBlast_f( gentity_t *ent ) {
	char buf[32];
	vec3_t p;
	int big, i;
	if ( trap_Argc() < 5 || !G_SkullIsPlayer( ent ) || ent->health <= 0 ) {
		return;
	}
	for ( i = 0; i < 3; i++ ) {
		trap_Argv( i + 1, buf, sizeof( buf ) );
		p[i] = atof( buf );
	}
	trap_Argv( 4, buf, sizeof( buf ) );
	big = atoi( buf );
	if ( !G_Skull( big ? SKULL_KABOOM : SKULL_EXPLOSIVE ) || Distance( p, ent->r.currentOrigin ) > 8192 ) {
		return;
	}
	G_SkullBlast( ent, p, big ? qtrue : qfalse );
}

/*
===============
Every server frame: Moon Gravity, the end of Bullet Time, Hunted, Catch
===============
*/
void G_SkullFrame( void ) {
	static char savedGravity[16];
	static int nextHunt, nextCatch;
	int bits = G_Skulls(), i;
	gentity_t *player = &g_entities[0];
	char buf[16];

	// Moon Gravity (players and AI; bodies on the client)
	trap_Cvar_VariableStringBuffer( "g_gravity", buf, sizeof( buf ) );
	if ( bits & SKULL_MOON ) {
		if ( atoi( buf ) != 260 ) {
			Q_strncpyz( savedGravity, buf, sizeof( savedGravity ) );
			trap_Cvar_Set( "g_gravity", "260" );
		}
	} else if ( savedGravity[0] ) {
		trap_Cvar_Set( "g_gravity", savedGravity );
		savedGravity[0] = 0;
	}

	// Bullet Time ends (real time, the game clock itself is slowed)
	if ( xbSlowUntil && ( trap_Milliseconds() > xbSlowUntil || !( bits & SKULL_BULLETTIME ) ) ) {
		trap_Cvar_Set( "timescale", "1" );
		xbSlowUntil = 0;
	}

	if ( !player->client || player->health <= 0 ) {
		return;
	}

	// Hunted: every enemy "hears" where you are, all the time
	if ( ( bits & SKULL_HUNTED ) && ( level.time > nextHunt || nextHunt - level.time > 5000 ) ) {
		nextHunt = level.time + 1500;
		AICast_AudibleEvent( 0, player->r.currentOrigin, 100000 );
	}

	// Catch: soldiers always have grenades on them
	if ( ( bits & SKULL_CATCH ) && ( level.time > nextCatch || nextCatch - level.time > 5000 ) ) {
		nextCatch = level.time + 2000;
		for ( i = 1; i < level.maxclients; i++ ) {
			gentity_t *e = &g_entities[i];
			if ( !e->inuse || !e->client || !( e->r.svFlags & SVF_CASTAI ) || e->health <= 0 ) {
				continue;
			}
			switch ( e->aiCharacter ) {
			case AICHAR_SOLDIER:
			case AICHAR_ELITEGUARD:
			case AICHAR_BLACKGUARD:
			case AICHAR_MERCENARY:
				break;
			default:
				continue;
			}
			COM_BitSet( e->client->ps.weapons, WP_GRENADE_LAUNCHER );
			if ( e->client->ps.ammoclip[BG_FindClipForWeapon( WP_GRENADE_LAUNCHER )] < 2 ) {
				Add_Ammo( e, WP_GRENADE_LAUNCHER, 2, qtrue );
			}
		}
	}
}
