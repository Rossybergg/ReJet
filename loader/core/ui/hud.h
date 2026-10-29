/* The game HUD hook (hud.c, docs/HUD.md). */
#pragma once

#include "hle.h"

extern const HlePatch g_hud_patches[];
extern const unsigned g_hud_patch_count;

/* What the game's own HUD would show this frame, read from its nodes (docs/HUD.md). */
typedef struct HudValues {
    float health; /* 0..1: the original bar's fill over its background */
    int cans, cans_max;
    float speed; /* 0..1: the speed meter (full when boosting) */
} HudValues;

/* The values, and 1 while the game is drawing its gameplay HUD (not on the map or in cutscenes). */
int hud_values(HudValues *out);
