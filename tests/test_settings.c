/* Settings (loader/core/settings.c), on a temporary rejet.ini (REJET_INI). */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "settings.h"
#include "test.h"

static void write_ini(const WCHAR *path, const char *text)
{
    FILE *f = _wfopen(path, L"w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* Players edit rejet.ini by hand and old versions leave keys behind: values out of range are
 * clamped, missing keys keep the defaults, and what is saved loads back the same. */
static void load_clamp_and_round_trip(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH], folder[MAX_PATH];
    const Settings defaults = g_settings;
    Settings saved;
    GetTempPathW(MAX_PATH, dir);
    swprintf(path, MAX_PATH, L"%srejet_test.ini", dir);
    SetEnvironmentVariableW(L"REJET_INI", path);
    write_ini(path, "[Display]\nHeight=99999\nAnisotropy=0\nAspect=2\n[Audio]\nVolume=-5\n[Old]\nPAL60=1\n");
    settings_load();
    CHECK_EQ(g_settings.height, 4320);   /* clamped */
    CHECK_EQ(g_settings.anisotropy, 1);  /* clamped */
    CHECK_EQ(g_settings.aspect, ASPECT_16_9);
    CHECK_EQ(g_settings.volume, 0);
    CHECK_EQ(g_settings.vsync, defaults.vsync); /* missing: the default */
    CHECK_EQ(g_settings.language, defaults.language);
    g_settings.height = 1440;
    g_settings.show_fps = 1;
    settings_save();
    saved = g_settings;
    g_settings = defaults;
    settings_load();
    CHECK(memcmp(&g_settings, &saved, sizeof saved) == 0);
    settings_set_game_folder(L"D:\\Games\\JSRF");
    settings_game_folder(folder, MAX_PATH);
    CHECK(wcscmp(folder, L"D:\\Games\\JSRF") == 0);
    g_settings = defaults;
    SetEnvironmentVariableW(L"REJET_INI", NULL);
    DeleteFileW(path);
}

void test_settings(void)
{
    load_clamp_and_round_trip();
}
