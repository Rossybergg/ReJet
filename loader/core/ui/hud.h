/* The game HUD hook (hud.c, docs/HUD.md). */
#pragma once

#include "hle.h"

extern const HlePatch g_hud_patches[];
extern const unsigned g_hud_patch_count;

/* What the game's own HUD would show this frame, read from its nodes (docs/HUD.md). */
typedef struct HudValues {
    float health;     /* 0..1: current health, over the whole bar */
    float health_max; /* 0..1: this character's maximum, over the whole bar (lost health shows black) */
    int cans, cans_max;
    float speed; /* 0..1: the speed meter (full when boosting) */
    int show_health, show_cans, show_speed; /* the game's own visibility flags (hidden in story scenes) */
} HudValues;

/* The values, and 1 while the game is drawing its gameplay HUD (not on the map or in cutscenes). */
int hud_values(HudValues *out);
