/* The game HUD hook (hud.c, docs/HUD.md). */
#pragma once

#include "hle.h"

extern const HlePatch g_hud_patches[];
extern const unsigned g_hud_patch_count;

/* Set while one of the HUD module's draw methods runs (REJET_HUDTEX dumps its textures). */
extern volatile int g_hud_in_draw;

/* What the game's own HUD would show this frame, read from its nodes (docs/HUD.md). */
typedef struct HudValues {
    float health;     /* 0..1: current health, over the whole bar */
    float health_max; /* 0..1: this character's maximum, over the whole bar (lost health shows black) */
    int cans, cans_max;
    float speed; /* 0..1: the speed meter (full when boosting) */
    int show_health, show_cans, show_speed; /* the game's own visibility flags (hidden in story scenes) */
    int score;                          /* stat 6: the score, from tricks */
    int trick_points, trick_mult, trick_total; /* stats 0xD/0xE/0xF: the combo popup (points x multiplier, total) */
    int tricks;                         /* stat 0xA: tricks in the last combo ("TRICKS" popup) */
    int tags_done, tags_total;          /* the stage's tag spots (FUN_0004DC00), as the "5/16" popup */
    /* the game's popups, as their nodes draw them (0 when not drawing): fade, and slide in (0..1) */
    float combo_fade, combo_slide, combo_total_fade; /* 0x1009F0: points x multiplier, then the total */
    float tricks_fade, tricks_slide;                 /* 0x102940: "N TRICKS" */
    float tag_fade;                                  /* 0x104AD0: "5/16" when a tag is done */
    float soul_fade;                                 /* 0x1054D0: "GET No. N", a graffiti soul picked up */
    int soul;                                        /* its number, as the game shows it */
} HudValues;

/* The values, and 1 while the game is drawing its gameplay HUD (not on the map or in cutscenes). */
int hud_values(HudValues *out);
