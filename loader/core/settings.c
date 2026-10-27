/* User settings (see settings.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#include "kernel.h"
#include "settings.h"

Settings g_settings = {
    GAME_LANG_ENGLISH, 1, /* console */
};

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void settings_load(void)
{
    WCHAR path[MAX_PATH], *slash;
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    Settings *s = &g_settings;

    if (!n || n >= MAX_PATH || !(slash = wcsrchr(path, L'\\')))
        return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"rejet.ini");
    { /* REJET_INI=<file>: another settings file */
        WCHAR alt[MAX_PATH];
        DWORD k = GetEnvironmentVariableW(L"REJET_INI", alt, MAX_PATH);
        if (k && k < MAX_PATH)
            GetFullPathNameW(alt, MAX_PATH, path, NULL);
    }

#define GET(section, key, field, lo, hi) \
    s->field = clampi((int)GetPrivateProfileIntW(L##section, L##key, s->field, path), lo, hi)
    GET("Console", "Language", language, GAME_LANG_JAPANESE, GAME_LANG_ITALIAN);
    GET("Console", "PAL60", pal60, 0, 1);
#undef GET
    log_printf("settings: language %d, %s\n", s->language, s->pal60 ? "PAL60 (60 Hz)" : "PAL 50 Hz");
}
