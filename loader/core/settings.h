/* User settings: rejet.ini next to ReJetLoader.exe. Every option defaults to the original
 * behaviour, except where noted; environment variables (REJET_*) used for debugging still
 * override where they exist. Display, controls and audio options come back with their
 * subsystems (Phase 3+). */
#pragma once

/* Game language index (docs/SUBSYSTEMS.md): what the console's language setting maps to. */
enum { GAME_LANG_JAPANESE = 0, GAME_LANG_ENGLISH = 1, GAME_LANG_FRENCH = 2, GAME_LANG_GERMAN = 3, GAME_LANG_SPANISH = 4, GAME_LANG_ITALIAN = 5 };

typedef struct Settings {
    /* [Display] */
    int width, height; /* render resolution (default 1280x960; the Xbox renders 640x480) */
    int window_scale;  /* window size in percent of the render size (default 100) */
    int vsync;         /* 1 = also wait for the monitor's refresh when presenting (default 0: the emulated
                          vblank already paces the game at 60 Hz, and the extra wait could halve the frame rate) */
    /* [Audio] */
    int volume;             /* master volume in percent (default 100) */
    int mute_in_background; /* silent while another program has the focus */
    /* [Console] */
    int language;  /* GAME_LANG_* (default English) */
    int pal60;     /* 1 = report the PAL60 video flag so the game runs at 60 Hz (default); 0 = PAL 50 Hz */
} Settings;

extern Settings g_settings;

/* Load rejet.ini (missing keys keep their defaults). Called once at startup. */
void settings_load(void);
