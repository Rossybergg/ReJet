/* User settings (see settings.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#include "kernel.h"
#include "settings.h"

Settings g_settings = {
    1280, 960, 960, 0, 0, 1, 0, /* display (host vsync off: the emulated vblank paces the game) */
    100, 0,                     /* audio */
    GAME_LANG_ENGLISH,          /* console */
};

static WCHAR g_path[MAX_PATH]; /* rejet.ini, or REJET_INI */

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void settings_load(void)
{
    WCHAR *slash;
    DWORD n = GetModuleFileNameW(NULL, g_path, MAX_PATH);
    Settings *s = &g_settings;

    if (!n || n >= MAX_PATH || !(slash = wcsrchr(g_path, L'\\'))) {
        g_path[0] = 0;
        return;
    }
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_path), L"rejet.ini");
    { /* REJET_INI=<file>: another settings file */
        WCHAR alt[MAX_PATH];
        DWORD k = GetEnvironmentVariableW(L"REJET_INI", alt, MAX_PATH);
        if (k && k < MAX_PATH)
            GetFullPathNameW(alt, MAX_PATH, g_path, NULL);
    }

#define GET(section, key, field, lo, hi) \
    s->field = clampi((int)GetPrivateProfileIntW(L##section, L##key, s->field, g_path), lo, hi)
    GET("Display", "Width", width, 320, 7680);
    GET("Display", "Height", height, 240, 4320);
    GET("Display", "WindowHeight", window_height, 240, 4320);
    GET("Display", "VSync", vsync, 0, 1);
    GET("Display", "Fullscreen", fullscreen, 0, 1);
    GET("Display", "Anisotropy", anisotropy, 1, 16);
    GET("Display", "ShowFPS", show_fps, 0, 1);
    GET("Audio", "Volume", volume, 0, 100);
    GET("Audio", "MuteInBackground", mute_in_background, 0, 1);
    GET("Console", "Language", language, GAME_LANG_JAPANESE, GAME_LANG_ITALIAN);
#undef GET
    log_printf("settings: %dx%d (window height %d%s), vsync %d, anisotropy %d, language %d\n", s->width, s->height,
               s->window_height, s->fullscreen ? ", full screen" : "", s->vsync, s->anisotropy, s->language);
}

void settings_save(void)
{
    const Settings *s = &g_settings;
    if (!g_path[0])
        return;
#define PUT(section, key, field)                                                \
    do {                                                                        \
        WCHAR v[16];                                                            \
        swprintf(v, 16, L"%d", s->field);                                       \
        WritePrivateProfileStringW(L##section, L##key, v, g_path);              \
    } while (0)
    PUT("Display", "Width", width);
    PUT("Display", "Height", height);
    PUT("Display", "WindowHeight", window_height);
    PUT("Display", "VSync", vsync);
    PUT("Display", "Fullscreen", fullscreen);
    PUT("Display", "Anisotropy", anisotropy);
    PUT("Display", "ShowFPS", show_fps);
    PUT("Audio", "Volume", volume);
    PUT("Audio", "MuteInBackground", mute_in_background);
    PUT("Console", "Language", language);
#undef PUT
}
