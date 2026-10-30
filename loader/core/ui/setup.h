/* First-run setup: find and prepare the player's own game files (see setup.c). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Nonzero if `folder` holds the supported Default.xbe. */
int setup_folder_ok(const WCHAR *folder);

/* Where setup extracts the game: %LOCALAPPDATA%\rejet\Game, beside the saves, so every ReJet
 * version finds it and an update needn't extract again. */
void setup_shared_folder(WCHAR *folder, unsigned cap);

/* Show the setup screen; on success the game folder (also saved in rejet.ini) is written to
 * `folder` and 1 is returned. 0 if the player closed it. */
int setup_run(const WCHAR *exe_dir, WCHAR *folder, unsigned cap);
