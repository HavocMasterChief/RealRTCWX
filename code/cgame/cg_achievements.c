/*
===========================================================================
RealRTCWX - local achievements (Xbox)

The game awards RealRTCW's Steam achievements as usual; on Xbox they reach
us as an "xbach <id>" server command (see G_UWP_Achievement). We keep the
unlocks in achievements.dat (one ID per line, in the game's home folder)
and show each new one as a pop-up: an iron plate drops from the top of the
screen, blood runs from its bottom edge, it holds for five seconds and
fades. Names, descriptions and icons come from the overlay
(achievements/list.txt, achievements/<ID>.png; tools/gen_achievement_icons.py).

"xbachreset" (the Cheats menu, after three confirmations) clears them.
===========================================================================
*/

#include "cg_local.h"
#include "../ui/ui_shared.h"

#define ACH_MAX         192
#define ACH_QUEUE       16
#define ACH_FILE        "achievements.dat"
#define ACH_DROP_MS     520
#define ACH_HOLD_MS     5000
#define ACH_FADE_MS     600

typedef struct {
	char id[48];
	char name[80];
	char desc[120];
	qboolean unlocked;
	qhandle_t icon;
} achievement_t;

static achievement_t achList[ACH_MAX];
static int achNum;
static qboolean achLoaded;

static int achQueue[ACH_QUEUE];
static int achQueueLen;
static int achShowing = -1;
static int achShowStart;

static qhandle_t achPlate, achDrip, achCog[8];
static int achLevel[ACH_QUEUE];     // unlocked during this level (for the mission stats)
static int achLevelNum;
static int achSavedTime = -100000;
static sfxHandle_t achSound;

static achievement_t *CG_AchFind( const char *id ) {
	int i;
	for ( i = 0; i < achNum; i++ ) {
		if ( !Q_stricmp( achList[i].id, id ) ) {
			return &achList[i];
		}
	}
	return NULL;
}

// achievements/list.txt: ID <tab> name <tab> description
static void CG_AchLoadList( void ) {
	char buf[32768];
	fileHandle_t f;
	int len;
	char *p, *line;

	achNum = 0;
	len = trap_FS_FOpenFile( "achievements/list.txt", &f, FS_READ );
	if ( len <= 0 ) {
		if ( f ) {
			trap_FS_FCloseFile( f );
		}
		return;
	}
	if ( len >= (int)sizeof( buf ) ) {
		len = sizeof( buf ) - 1;
	}
	trap_FS_Read( buf, len, f );
	trap_FS_FCloseFile( f );
	buf[len] = 0;

	for ( p = buf; *p && achNum < ACH_MAX; ) {
		char *t1, *t2, *e;
		achievement_t *a;
		line = p;
		while ( *p && *p != '\n' ) {
			p++;
		}
		if ( *p ) {
			*p++ = 0;
		}
		if ( ( e = strchr( line, '\r' ) ) != NULL ) {
			*e = 0;
		}
		if ( !line[0] || ( line[0] == '/' && line[1] == '/' ) ) {
			continue;
		}
		t1 = strchr( line, '\t' );
		if ( !t1 ) {
			continue;
		}
		*t1++ = 0;
		t2 = strchr( t1, '\t' );
		if ( t2 ) {
			*t2++ = 0;
		}
		a = &achList[achNum++];
		memset( a, 0, sizeof( *a ) );
		Q_strncpyz( a->id, line, sizeof( a->id ) );
		Q_strncpyz( a->name, t1, sizeof( a->name ) );
		Q_strncpyz( a->desc, t2 ? t2 : "", sizeof( a->desc ) );
	}
}

static void CG_AchLoadUnlocks( void ) {
	char buf[8192];
	fileHandle_t f;
	int len, i;
	char *p, *line;

	for ( i = 0; i < achNum; i++ ) {
		achList[i].unlocked = qfalse;
	}
	len = trap_FS_FOpenFile( ACH_FILE, &f, FS_READ );
	if ( len <= 0 ) {
		if ( f ) {
			trap_FS_FCloseFile( f );
		}
		return;
	}
	if ( len >= (int)sizeof( buf ) ) {
		len = sizeof( buf ) - 1;
	}
	trap_FS_Read( buf, len, f );
	trap_FS_FCloseFile( f );
	buf[len] = 0;
	for ( p = buf; *p; ) {
		achievement_t *a;
		char *e;
		line = p;
		while ( *p && *p != '\n' ) {
			p++;
		}
		if ( *p ) {
			*p++ = 0;
		}
		if ( ( e = strchr( line, '\r' ) ) != NULL ) {
			*e = 0;
		}
		if ( ( a = CG_AchFind( line ) ) != NULL ) {
			a->unlocked = qtrue;
		}
	}
}

static void CG_AchSaveUnlocks( void ) {
	fileHandle_t f;
	int i;
	if ( trap_FS_FOpenFile( ACH_FILE, &f, FS_WRITE ) < 0 ) {
		return;
	}
	for ( i = 0; i < achNum; i++ ) {
		if ( achList[i].unlocked ) {
			trap_FS_Write( achList[i].id, strlen( achList[i].id ), f );
			trap_FS_Write( "\n", 1, f );
		}
	}
	trap_FS_FCloseFile( f );
}

void CG_AchInit( void ) {
	int i;
	achQueueLen = 0;
	achShowing = -1;
	achLevelNum = 0;
	achSavedTime = -100000;
	for ( i = 0; i < 8; i++ ) {
		achCog[i] = trap_R_RegisterShaderNoMip( va( "achievements/save%i", i ) );
	}
	achPlate = trap_R_RegisterShaderNoMip( "achievements/plate" );
	achDrip = trap_R_RegisterShaderNoMip( "achievements/drip" );
	achSound = trap_S_RegisterSound( "sound/xbox/achievement.wav" );   // an iron bell (overlay)
	if ( !achLoaded ) {
		CG_AchLoadList();
		achLoaded = qtrue;
	}
	CG_AchLoadUnlocks();
}

int CG_AchCount( int *total ) {
	int i, n = 0;
	for ( i = 0; i < achNum; i++ ) {
		if ( achList[i].unlocked ) {
			n++;
		}
	}
	if ( total ) {
		*total = achNum;
	}
	return n;
}

// "xbach <id>" from the game
void CG_AchUnlock( const char *id ) {
	achievement_t *a;

	if ( !achLoaded ) {
		CG_AchInit();
	}
	a = CG_AchFind( id );
	if ( !a ) {
		CG_Printf( "achievement '%s' isn't in achievements/list.txt\n", id );
		return;
	}
	if ( a->unlocked ) {
		return;
	}
	a->unlocked = qtrue;
	CG_AchSaveUnlocks();
	CG_Printf( "Achievement unlocked: %s\n", a->name );
	if ( achQueueLen < ACH_QUEUE ) {
		achQueue[achQueueLen++] = (int)( a - achList );
	}
	if ( achLevelNum < ACH_QUEUE ) {
		achLevel[achLevelNum++] = (int)( a - achList );
	}
}

// the game just saved (cg_ragdoll.c sees it in g_lastSave)
void CG_AchSaved( void ) {
	achSavedTime = cg.time;
}

/*
===============
CG_AchDrawMissionRow

Under the end-of-mission stats: the achievements earned on this level.
===============
*/
void CG_AchDrawMissionRow( float y, float alpha ) {
	vec4_t c;
	int i;
	float x, size = 30;
	if ( !achLevelNum ) {
		return;
	}
	Vector4Set( c, 0.72f, 0.27f, 0.18f, alpha );
	CG_Text_Paint( 150, y + 12, UI_FONT_DEFAULT, 0.2f, c,
				   va( "ACHIEVEMENTS THIS MISSION: %i", achLevelNum ), 0, 0, ITEM_TEXTSTYLE_SHADOWED );
	x = 150;
	Vector4Set( c, 1, 1, 1, alpha );
	trap_R_SetColor( c );
	for ( i = 0; i < achLevelNum && i < 10; i++ ) {
		achievement_t *a = &achList[achLevel[i]];
		if ( !a->icon ) {
			a->icon = trap_R_RegisterShaderNoMip( va( "achievements/%s", a->id ) );
		}
		CG_DrawPic( x, y + 18, size, size, a->icon );
		x += size + 4;
	}
	trap_R_SetColor( NULL );
	if ( achLevelNum == 1 ) {
		Vector4Set( c, 0.94f, 0.89f, 0.78f, alpha );
		CG_Text_Paint( x + 6, y + 38, UI_FONT_DEFAULT, 0.22f, c, achList[achLevel[0]].name, 0, 34, ITEM_TEXTSTYLE_SHADOWED );
	}
}

// bottom-right "Game saved" with a turning brass cog, for 1.6 s after a save
static void CG_AchDrawSaved( void ) {
	vec4_t c;
	int dt = cg.time - achSavedTime;
	float a;
	if ( dt < 0 || dt > 1600 || !achCog[0] ) {
		return;
	}
	a = dt < 150 ? dt / 150.0f : ( dt > 1200 ? ( 1600 - dt ) / 400.0f : 1.0f );
	Vector4Set( c, 1, 1, 1, a );
	trap_R_SetColor( c );
	CG_DrawPic( SCREEN_WIDTH - 46, SCREEN_HEIGHT - 92, 24, 24, achCog[( dt / 60 ) & 7] );
	trap_R_SetColor( NULL );
	Vector4Set( c, 0.94f, 0.89f, 0.78f, a );
	CG_Text_Paint( SCREEN_WIDTH - 52 - CG_Text_Width( "GAME SAVED", UI_FONT_DEFAULT, 0.2f, 0 ), SCREEN_HEIGHT - 75,
				   UI_FONT_DEFAULT, 0.2f, c, "GAME SAVED", 0, 0, ITEM_TEXTSTYLE_SHADOWED );
}

// "xbachreset" (Cheats menu, after three confirmations)
void CG_AchReset_f( void ) {
	int i;
	for ( i = 0; i < achNum; i++ ) {
		achList[i].unlocked = qfalse;
	}
	CG_AchSaveUnlocks();
	achQueueLen = 0;
	achShowing = -1;
	CG_CenterPrint( "All achievements have been reset", SCREEN_HEIGHT * 0.3, SMALLCHAR_WIDTH );
}

/*
===============
CG_AchDraw

The pop-up: drops in from the top (with a little overshoot), blood runs
from the bottom edge, holds, fades. Drawn over everything except menus.
===============
*/
void CG_AchDraw( void ) {
	static const float dripX[5] = { 0.11f, 0.30f, 0.57f, 0.80f, 0.93f };
	static const float dripLen[5] = { 0.30f, 0.16f, 0.46f, 0.22f, 0.12f };
	static const int dripDelay[5] = { 0, 160, 80, 240, 320 };
	achievement_t *a;
	float t, y, alpha, w = 280, h = 58, x, s, total;
	vec4_t col;
	int i, num, all;

	CG_AchDrawSaved();
	if ( achShowing < 0 ) {
		if ( !achQueueLen || cg.cameraMode ) {
			return;     // wait for cutscenes to end
		}
		achShowing = achQueue[0];
		memmove( achQueue, achQueue + 1, ( --achQueueLen ) * sizeof( achQueue[0] ) );
		achShowStart = cg.time;
		if ( achSound ) {
			trap_S_StartLocalSound( achSound, CHAN_LOCAL_SOUND );
		}
		CG_PadRumble( 0.0f, 0.25f, 120, 0.0f );
	}
	a = &achList[achShowing];
	if ( !a->icon ) {
		a->icon = trap_R_RegisterShaderNoMip( va( "achievements/%s", a->id ) );
	}

	t = cg.time - achShowStart;
	if ( t < 0 ) {
		achShowStart = cg.time;
		t = 0;
	}
	if ( t >= ACH_HOLD_MS + ACH_FADE_MS ) {
		achShowing = -1;
		return;
	}

	// drop: ease out to 6% past the rest position, then settle
	if ( t < ACH_DROP_MS ) {
		float k = t / (float)ACH_DROP_MS;
		if ( k < 0.62f ) {
			float e = k / 0.62f;
			e = 1 - ( 1 - e ) * ( 1 - e ) * ( 1 - e );
			y = -1.3f + e * ( 1.3f + 0.06f );
		} else if ( k < 0.8f ) {
			y = 0.06f - ( k - 0.62f ) / 0.18f * 0.08f;
		} else {
			y = -0.02f + ( k - 0.8f ) / 0.2f * 0.02f;
		}
	} else {
		y = 0;
	}
	y *= h;
	alpha = t > ACH_HOLD_MS ? 1.0f - ( t - ACH_HOLD_MS ) / (float)ACH_FADE_MS : 1.0f;
	x = ( SCREEN_WIDTH - w ) * 0.5f;

	// drips (behind the plate's bottom edge)
	for ( i = 0; i < 5; i++ ) {
		float dt = t - 120 - dripDelay[i], len;
		if ( dt <= 0 ) {
			continue;
		}
		len = dt < 900 ? dt / 900.0f : 1.0f + ( dt - 900 ) / 4200.0f * 0.35f;
		len *= dripLen[i] * 64;
		Vector4Set( col, 1, 1, 1, alpha );
		trap_R_SetColor( col );
		CG_DrawPic( x + w * dripX[i] - 2.5f, y + h - 3, 5, len + 4, achDrip );
	}

	Vector4Set( col, 1, 1, 1, alpha );
	trap_R_SetColor( col );
	CG_DrawPic( x, y, w, h, achPlate );
	if ( a->icon ) {
		CG_DrawPic( x + 9, y + 8, 40, 40, a->icon );
	}
	trap_R_SetColor( NULL );

	// text
	s = 0.17f;
	Vector4Set( col, 0.72f, 0.27f, 0.18f, alpha );
	CG_Text_Paint( x + 58, y + 16, UI_FONT_DEFAULT, s, col, "ACHIEVEMENT UNLOCKED", 0.6f, 0, ITEM_TEXTSTYLE_NORMAL );
	Vector4Set( col, 0.94f, 0.89f, 0.78f, alpha );
	CG_Text_Paint( x + 58, y + 33, UI_FONT_BIG, a->desc[0] ? 0.28f : 0.30f, col, a->name, 0, 30, ITEM_TEXTSTYLE_SHADOWED );
	if ( a->desc[0] ) {
		Vector4Set( col, 0.73f, 0.65f, 0.52f, alpha );
		CG_Text_Paint( x + 58, y + 46, UI_FONT_DEFAULT, 0.17f, col, a->desc, 0, 52, ITEM_TEXTSTYLE_NORMAL );
	}
	num = CG_AchCount( &all );
	total = all;
	Vector4Set( col, 0.56f, 0.49f, 0.39f, alpha );
	{
		const char *cnt = va( "%i / %i", num, (int)total );
		CG_Text_Paint( x + w - 8 - CG_Text_Width( cnt, UI_FONT_DEFAULT, 0.15f, 0 ), y + 52, UI_FONT_DEFAULT, 0.15f, col, cnt, 0, 0, ITEM_TEXTSTYLE_NORMAL );
	}
}
