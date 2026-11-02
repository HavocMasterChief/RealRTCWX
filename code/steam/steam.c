#include "steam.h"

#include "../steamshim/steamshim_child.h"

#include <stdint.h>

#ifdef STEAM

void steamRun(void)
{
	STEAMSHIM_pump();
	
	return;
}

int steamInit(void)
{
	if (!STEAMSHIM_init())
	{
		return 0;
	}

	STEAMSHIM_requestStats();

	return 1;
}

void steamSetAchievement(const char* id)
{
	STEAMSHIM_setAchievement(id, 1);

	STEAMSHIM_storeStats();

	return;
}

void steamResetStats(const int bAlsoAchievements)
{
	STEAMSHIM_resetStats(bAlsoAchievements);

	return;
}

int steamAlive()
{
	return STEAMSHIM_alive();
}

void steamSetRichPresence(const char* key, const char* value)
{
	STEAMSHIM_setRichPresence(key, value);
}

#else

void steamRun(void)
{
	return;
}

int steamInit(void)
{
	return 0;
}

#if defined( RTCW_UWP ) && defined( GAMEDLL )
// Xbox: local achievements (pop-up + saved on the console), see g_cmds.c
void G_UWP_Achievement( const char *id );
void steamSetAchievement(const char* id)
{
	G_UWP_Achievement( id );
}
#else
void steamSetAchievement(const char* id)
{
	return;
}
#endif

int steamAlive()
{
	return 1;
}

void steamSetRichPresence(const char* key, const char* value)
{
	return;
}

void steamResetStats(const int bAlsoAchievements)
{
	return;
}

#endif