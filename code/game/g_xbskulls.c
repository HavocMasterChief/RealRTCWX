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
		int w, tries = 0;
		do {
			w = xbGunPool[rand() % ( sizeof( xbGunPool ) / sizeof( xbGunPool[0] ) )];
		} while ( w == attacker->client->ps.weapon && ++tries < 8 );
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

// Ricochet: the player's bullet bounces once off a wall
qboolean G_SkullRicochet( gentity_t *attacker, int recursion ) {
	return ( recursion == 0 && G_SkullIsPlayer( attacker ) && G_Skull( SKULL_RICOCHET ) ) ? qtrue : qfalse;
}

/*
===============
Boot Camp: your kick launches people
===============
*/
void G_SkullKick( gentity_t *attacker, gentity_t *traceEnt, const vec3_t forward ) {
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
