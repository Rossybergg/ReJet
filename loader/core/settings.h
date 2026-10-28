/* User settings: rejet.ini next to ReJetLoader.exe. Every option defaults to the original
 * behaviour, except where noted; environment variables (REJET_*) used for debugging still
 * override where they exist. The in-game ReJet menu (ui/menu.c) changes them and saves them
 * back, so they are kept between sessions. */
#pragma once

/* Game language index (docs/SUBSYSTEMS.md): what the console's language setting maps to. */
enum { GAME_LANG_JAPANESE = 0, GAME_LANG_ENGLISH = 1, GAME_LANG_FRENCH = 2, GAME_LANG_GERMAN = 3, GAME_LANG_SPANISH = 4, GAME_LANG_ITALIAN = 5 };

enum { ASPECT_4_3, ASPECT_16_10, ASPECT_16_9, ASPECT_21_9, ASPECT_COUNT };

/* Width / height of an aspect setting. */
float settings_aspect_ratio(int aspect);

typedef struct Settings {
    /* [Display] */
    int width, height; /* render resolution (default 1280x960; the Xbox renders 640x480) */
    int window_height; /* window height in Windows' scaled pixels (default 960; the aspect's shape, fitted to the screen) */
    int aspect;        /* ASPECT_*: 4:3 is the original; wider ones are Hor+ (more visible at the sides) */
    int vsync;         /* 1 = also wait for the monitor's refresh when presenting (default 0: the emulated
                          vblank already paces the game at 60 Hz, and the extra wait could halve the frame rate) */
    int fullscreen;    /* 1 = borderless window over the whole monitor */
    int anisotropy;    /* texture filtering: 1 = as the game asks (default), 2-16 = anisotropic */
    int show_fps;      /* frame-rate counter in the corner */
    /* [Audio] */
    int volume;             /* master volume in percent (default 100) */
    int mute_in_background; /* silent while another program has the focus */
    /* [Console] */
    int language;  /* GAME_LANG_* (default English); the console always runs at 60 Hz (PAL60) */
} Settings;

extern Settings g_settings;

/* Load rejet.ini (missing keys keep their defaults). Called once at startup. */
void settings_load(void);

/* Write the current settings back to rejet.ini. */
void settings_save(void);
