/*
RealRTCWX (Xbox): Skulls - fun/challenge modifiers from the Cheats menu
(cheats have to be on). One bit each in the cvar g_xbskulls, read by the
game (server) and cgame. Set from the UI (ui_main.c UI_XbSkull); cleared by
starting a new campaign or loading a save from another playthrough.
*/
#ifndef BG_XBSKULLS_H
#define BG_XBSKULLS_H

#define SKULL_BIGHEAD       ( 1 << 0 )  // cgame: enemies' heads twice the size
#define SKULL_PARTY         ( 1 << 1 )  // game+cgame: headshot kills burst into confetti, with a cheer
#define SKULL_MOON          ( 1 << 2 )  // game+cgame: low gravity (bodies float too)
#define SKULL_BULLETTIME    ( 1 << 3 )  // game: slow motion for a moment on headshot kills
#define SKULL_LAUNCHER      ( 1 << 4 )  // cgame: hits and blasts throw bodies 5x harder
#define SKULL_KABOOM        ( 1 << 5 )  // game+cgame: a corpse shot a few times explodes
#define SKULL_DRUNK         ( 1 << 6 )  // cgame: swaying camera
#define SKULL_PAINT         ( 1 << 7 )  // cgame+renderer: blood in bright paint colours
#define SKULL_FAMINE        ( 1 << 8 )  // game: enemies drop half the ammo
#define SKULL_MYTHIC        ( 1 << 9 )  // game: enemies take half damage (double health)
#define SKULL_CATCH         ( 1 << 10 ) // game: enemies throw grenades much more often
#define SKULL_BLIND         ( 1 << 11 ) // cgame: no HUD, crosshair or weapon
#define SKULL_NEWSREEL      ( 1 << 12 ) // cgame: 1940s film: black and white, grain, scratches
#define SKULL_VAMPIRE       ( 1 << 13 ) // game: kills give you health
#define SKULL_EXPLOSIVE     ( 1 << 14 ) // game: bullets that hit people or corpses make small blasts
#define SKULL_GUNGAME       ( 1 << 15 ) // game+cgame: a random weapon after every kill
#define SKULL_RICOCHET      ( 1 << 16 ) // game: bullets bounce once off walls
#define SKULL_TOY           ( 1 << 17 ) // cgame: enemies half size
#define SKULL_SILENT        ( 1 << 18 ) // game: enemies don't hear your gunfire
#define SKULL_HUNTED        ( 1 << 19 ) // game: every enemy always knows where you are
#define SKULL_MIRROR        ( 1 << 20 ) // cgame+renderer+client: the world flipped left to right
#define SKULL_BOOTCAMP      ( 1 << 21 ) // game+cgame: your kick launches people
#define SKULL_COUNT         22

#endif
