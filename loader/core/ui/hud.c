/* The game's HUD, seen from its display-node tree (docs/HUD.md).
 *
 * JSRF draws its 2D overlay as a tree of nodes: FUN_00011220 walks it (children at +0x28,
 * siblings at +0x30) and, unless a node's flags (+4) have 0x40000, pushes the matrix stack
 * (0x1BB480), calls the node's draw method (vtable slot 3, thiscall, one argument 0) and pops
 * it (0x1BB510). The HUD's draw methods live in one block of game code, 0x100000-0x10F000.
 * hud_draw_tree replaces the walker with the same steps in C, so ReJet can leave out the
 * original HUD's nodes when it draws its own. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "hle.h"
#include "kernel.h"
#include "settings.h"
#include "ui/hud.h"

#define HUD_CODE_START 0x100000u /* the HUD module's draw methods */
#define HUD_CODE_END 0x10F000u

typedef void(__fastcall *NodeDraw)(void *self, void *edx_unused, int arg); /* thiscall, 1 argument */
typedef void (*MatrixFn)(void);

ULONG d3d_frame(void); /* d3d.c */

static int g_hide = -1; /* REJET_HUDHIDE=1: leave out the original HUD; =cycle: one method at a time */
static const DWORD HUD_METHODS[] = {0x1080A0, 0x10DEA0, 0x106AD0, 0x105A90, 0x10E680, 0x10A590};
volatile int g_hud_in_draw;
static HudValues g_values;

/* The HUD-visibility flags of the player a HUD node belongs to, as its draw method reads them:
 * player = *(manager + 0x98 + index * 4), manager at 0x236580 (FUN_00012890); flags at player
 * + 0x16AC. Health shows when bit 31 is set, cans with 0x4000000, the speed meter with 0x2000000. */
static DWORD player_flags(int index)
{
    const unsigned char *manager = *(const unsigned char *const *)(UINT_PTR)0x236580, *player;
    if (!manager || index < 0)
        return 0;
    player = *(const unsigned char *const *)(manager + 0x98 + (UINT_PTR)index * 4);
    return player ? *(const DWORD *)(player + 0x16AC) : 0;
}
/* The game's stat getter (FUN_00108f00, thiscall on the player): 0xD/0xE/0xF are the trick
 * popup's points, multiplier and total (drawn by 0x1009F0), 0xA the number 0x102940 shows. */
typedef int(__fastcall *StatFn)(const void *player, void *edx_unused, int id);
static int player_stat(int index, int id)
{
    const unsigned char *manager = *(const unsigned char *const *)(UINT_PTR)0x236580, *player;
    if (!manager || index < 0)
        return -1;
    player = *(const unsigned char *const *)(manager + 0x98 + (UINT_PTR)index * 4);
    return player ? ((StatFn)(UINT_PTR)0x108F00)(player, NULL, id) : -1;
}

/* The stage's tag spots, as the game's "5/16" popup gets them (FUN_001077B0): FUN_0004DC00 on
 * the object behind handle 9 fills a summary whose first two fields are the tags in the stage
 * and the ones done. */
typedef void(__fastcall *SummaryFn)(void *self, void *edx_unused, DWORD *out);
static void stage_tags(int *done, int *total)
{
    const unsigned char *manager = *(const unsigned char *const *)(UINT_PTR)0x236580;
    void *stage = manager ? *(void *const *)(manager + 0x98 + 9 * 4) : NULL;
    DWORD summary[0x46] = {0};
    *done = *total = 0;
    if (!stage)
        return;
    ((SummaryFn)(UINT_PTR)0x4DC00)(stage, NULL, summary);
    *total = (int)summary[0];
    *done = (int)summary[1];
}

/* REJET_HUDLOG=1: log the player's stats (0..0x10) whenever one changes, to name them from play
 * (which one counts the stage's tags, the score, ...). */
static void log_stats(int index)
{
    static int on = -1, last[17];
    int now[17];
    if (on < 0) {
        char env[4] = "";
        on = GetEnvironmentVariableA("REJET_HUDLOG", env, sizeof env) && env[0] == '1';
        memset(last, 0x7F, sizeof last);
    }
    if (!on)
        return;
    for (int i = 0; i < 17; i++)
        now[i] = player_stat(index, i);
    if (memcmp(now, last, sizeof now)) {
        char line[320];
        int n = snprintf(line, sizeof line, "hud: stats");
        for (int i = 0; i < 17; i++)
            n += snprintf(line + n, sizeof line - n, " %X=%d", i, now[i]);
        log_printf("%s\n", line);
        memcpy(last, now, sizeof now);
    }
}
static ULONG g_values_frame = (ULONG)-10; /* when the health node last drew */
static ULONG g_combo_frame = (ULONG)-10, g_tricks_frame = (ULONG)-10, g_tag_frame = (ULONG)-10; /* popups */
static ULONG g_race_frame = (ULONG)-10;
#define SOUL_HOLD 0 /* the soul popup lasts as long as the game's, then fades over 20 frames */
/* The game's soul popups seen lately, one per node: two pickups close together leave two alive (the
 * first paused by its spin), and ReJet shows only the newest. state is +0x128: 3 holding (paused
 * while not drawn), 5 fading, 6 done. */
static struct { const void *node; ULONG first, last; int soul, state, retired; } g_souls[4];
static DWORD g_seen[256];
static unsigned g_nseen;

/* Log each draw method the first time it runs, for the HUD inventory in docs/HUD.md. */
static void note(DWORD draw, const unsigned char *node)
{
    for (unsigned i = 0; i < g_nseen; i++)
        if (g_seen[i] == draw)
            return;
    if (g_nseen < sizeof g_seen / sizeof g_seen[0])
        g_seen[g_nseen++] = draw;
    log_printf("hud: node draw %08lX (vtable %08lX, flags %08lX)%s\n", draw, *(const DWORD *)node,
               *(const DWORD *)(node + 4), draw >= HUD_CODE_START && draw < HUD_CODE_END ? " HUD module" : "");
}

/* REJET_HUDLOG=1: log the HUD nodes' candidate value fields whenever they change, to confirm
 * them from a real play session (docs/HUD.md): health segments, cans, boost geometry. */
static void log_values(DWORD draw, const unsigned char *node)
{
    static int on = -1;
    static DWORD last[3][4];
    static const struct { DWORD draw; const char *name; unsigned off[4]; } fields[] = {
        {0x106AD0, "health", {0x60, 0xC0, 0x108, 0x120}},
        {0x10E680, "cans", {0xB8, 0xD8, 0xD0, 0xD4}},
        {0x10DEA0, "speed", {0x1D0, 0x1D4, 0xA8, 0xC0}},
    };
    if (on < 0) {
        char env[4] = "";
        on = GetEnvironmentVariableA("REJET_HUDLOG", env, sizeof env) && env[0] == '1';
    }
    if (!on)
        return;
    for (int i = 0; i < 3; i++) {
        DWORD v[4];
        if (fields[i].draw != draw)
            continue;
        for (int k = 0; k < 4; k++)
            v[k] = *(const DWORD *)(node + fields[i].off[k]);
        if (memcmp(v, last[i], sizeof v)) {
            memcpy(last[i], v, sizeof v);
            log_printf("hud: %s node %p: +%X=%08lX +%X=%08lX +%X=%08lX +%X=%08lX\n", fields[i].name, (void *)node,
                       fields[i].off[0], v[0], fields[i].off[1], v[1], fields[i].off[2], v[2], fields[i].off[3], v[3]);
        }
    }
}

/* REJET_HUDLOG=1 also logs which HUD-module methods draw, whenever that set changes from one
 * frame to the next (to find state-dependent pieces, such as the boost icon from 10 cans). */
static void log_active(DWORD draw)
{
    static int on = -1;
    static ULONG frame;
    static DWORD now[16], prev[16];
    static unsigned nnow, nprev;
    if (on < 0) {
        char env[4] = "";
        on = GetEnvironmentVariableA("REJET_HUDLOG", env, sizeof env) && env[0] == '1';
    }
    if (!on || draw < HUD_CODE_START || draw >= HUD_CODE_END)
        return;
    if (d3d_frame() != frame) { /* a new frame: compare the last one with the one before */
        int same = nnow == nprev;
        for (unsigned i = 0; same && i < nnow; i++) {
            unsigned k = 0;
            while (k < nprev && prev[k] != now[i])
                k++;
            same = k < nprev;
        }
        if (!same) {
            char line[256];
            int n = snprintf(line, sizeof line, "hud: drawing (cans %d)", g_values.cans);
            for (unsigned i = 0; i < nnow && n < (int)sizeof line - 12; i++)
                n += snprintf(line + n, sizeof line - n, " %06lX", now[i]);
            log_printf("%s\n", line);
        }
        memcpy(prev, now, sizeof now);
        nprev = nnow;
        nnow = 0;
        frame = d3d_frame();
    }
    for (unsigned i = 0; i < nnow; i++)
        if (now[i] == draw)
            return;
    if (nnow < 16)
        now[nnow++] = draw;
}

/* REJET_HUDLOG=1: while a soul popup is alive, log each node (draw method and instance) the first time
 * it draws, to find what else a pickup shows. */
static void log_soul_nodes(DWORD draw, const unsigned char *node)
{
    static int on = -1;
    static const void *seen[64];
    static unsigned n;
    int alive = 0;
    if (on < 0) {
        char env[4] = "";
        on = GetEnvironmentVariableA("REJET_HUDLOG", env, sizeof env) && env[0] == '1';
    }
    if (!on)
        return;
    for (int k = 0; k < 4; k++)
        alive |= g_souls[k].node && d3d_frame() - g_souls[k].last < 600;
    if (!alive)
        return;
    for (unsigned i = 0; i < n; i++)
        if (seen[i] == node)
            return;
    if (n < 64)
        seen[n++] = node;
    log_printf("hud: soul-time node %p draw %08lX flags %08lX\n", (const void *)node, draw, *(const DWORD *)(node + 4));
}

static void __fastcall hud_draw_tree(unsigned char *node)
{
    if (g_hide < 0) {
        char env[16] = "";
        g_hide = !GetEnvironmentVariableA("REJET_HUDHIDE", env, sizeof env) ? 0 : env[0] == 'c' ? 2 : env[0] == '1';
    }
    for (; node; node = *(unsigned char **)(node + 0x30)) {
        if (!(*(DWORD *)(node + 4) & 0x40000)) {
            DWORD draw = (*(DWORD **)node)[3];
            note(draw, node);
            log_values(draw, node);
            log_active(draw);
            log_soul_nodes(draw, node);
            int skip = g_hide == 1 && draw >= HUD_CODE_START && draw < HUD_CODE_END;
            if (draw == 0x10DEA0) { /* speed: current (+0x1D0, clamped) over the maximum (+0x1D4), as its update (0x10DDA0) */
                float top = *(const float *)(node + 0x1D4);
                g_values.speed = top > 0 ? *(const float *)(node + 0x1D0) / top : 0;
                g_values.show_speed = (player_flags(*(const int *)(node + 0x1D8)) & 0x2000000u) != 0;
            }
            if (draw == 0x106AD0) { /* health: quads over the background (+0x48..+0x60): the character's
                                     * maximum (+0xA8..+0xC0) and the current health (+0x108..+0x120) */
                float bg = *(const float *)(node + 0x60) - *(const float *)(node + 0x48);
                float top = *(const float *)(node + 0xC0) - *(const float *)(node + 0xA8);
                float now = *(const float *)(node + 0x120) - *(const float *)(node + 0x108);
                g_values.health_max = bg > 0 ? top / bg : 0;
                g_values.health = bg > 0 ? now / bg : 0;
                g_values.show_health = (player_flags(*(const int *)(node + 0x2B4)) & 0x80000000u) != 0;
                g_values_frame = d3d_frame();
                {
                    int index = *(const int *)(node + 0x2B4);
                    log_stats(index);
                    g_values.score = player_stat(index, 6);
                    g_values.trick_points = player_stat(index, 0xD);
                    g_values.trick_mult = player_stat(index, 0xE);
                    g_values.trick_total = player_stat(index, 0xF);
                    g_values.tricks = player_stat(index, 0xA);
                    stage_tags(&g_values.tags_done, &g_values.tags_total);
                }
            }
            else if (draw == 0x10E680) { /* cans held, and the cap */
                g_values.cans = *(const int *)(node + 0xB8);
                g_values.cans_max = *(const int *)(node + 0xD8);
                g_values.show_cans = (player_flags(*(const int *)(node + 0x100)) & 0x4000000u) != 0;
            }
            if (draw == 0x1009F0) { /* the combo popup: its fade (+0x114), slide-in (+0x118) and the total's fade (+0x11C) */
                g_values.combo_fade = player_flags(*(const int *)(node + 0x120)) & 0x100u ? *(const float *)(node + 0x114) : 0;
                g_values.combo_slide = *(const float *)(node + 0x118);
                g_values.combo_total_fade = *(const float *)(node + 0x11C);
                g_combo_frame = d3d_frame();
            }
            else if (draw == 0x102940) { /* "N TRICKS": fade +0x134, slide-in +0x13C */
                g_values.tricks_fade =
                    player_flags(*(const int *)(node + 0x138)) & 0x20000000u ? *(const float *)(node + 0x134) : 0;
                g_values.tricks_slide = *(const float *)(node + 0x13C);
                g_tricks_frame = d3d_frame();
            }
            else if (draw == 0x104AD0) { /* a tag done ("5/16"): fade +0x134 */
                g_values.tag_fade = *(const float *)(node + 0x134);
                g_tag_frame = d3d_frame();
            }
            else if (draw == 0x1054D0) { /* a graffiti soul picked up: its number (+0x11C, its own player's stat
                                          * 0xB + 1). The game shows it on and off through the pickup's camera spin
                                          * (flag 0x100000), with a 3D title (0xFBD40); ReJet shows "GET No. N" for
                                          * as long as the node lives */
                if (*(const int *)(node + 0x11C) > 0) {
                    ULONG f = d3d_frame();
                    unsigned k = 0, oldest = 0;
                    while (k < 4 && g_souls[k].node != node)
                        k++;
                    if (k < 4 && g_souls[k].retired && f - g_souls[k].last <= 600) { /* an older popup: ignore it */
                        g_souls[k].last = f;
                        k = 4 + 1;
                    }
                    else if (k == 4 || f - g_souls[k].last > 600) { /* a new popup (a node unseen for 10 s is a new one) */
                        for (k = 0; k < 4; k++)
                            if (g_souls[k].last < g_souls[oldest].last)
                                oldest = k;
                        k = oldest;
                        for (unsigned j = 0; j < 4; j++) /* a newer pickup retires the older popups */
                            g_souls[j].retired = 1;
                        g_souls[k].node = node;
                        g_souls[k].retired = 0;
                        g_souls[k].first = f;
                    }
                    if (k < 4) {
                        g_souls[k].last = f;
                        g_souls[k].soul = *(const int *)(node + 0x11C);
                        g_souls[k].state = *(const int *)(node + 0x128);
                    }
                }
            }
            else if (draw == 0x10F640) { /* the race timer (update 0x10F540): seconds, a float at +0x84; hidden
                                          * while +0x48 has bit 31; mode +0x160 */
                int mode = *(const int *)(node + 0x160);
                g_values.race_time = *(const DWORD *)(node + 0x48) & 0x80000000u ? -1 : *(const float *)(node + 0x84);
                if (mode != g_values.race_mode)
                    log_printf("hud: race timer mode %d -> %d at %.2f s\n", g_values.race_mode, mode, g_values.race_time);
                g_values.race_mode = mode;
                g_race_frame = d3d_frame();
            }
            /* HUD: ReJet draws its own health, cans and speed meter (menu.c) */
            /* 0x10A590: the boost icon shown from 10 cans (seen in play-test bursts), redrawn by ReJet */
            /* 0x1009F0, 0x102940, 0x104AD0, 0x1054D0: the combo, tricks, tag and soul popups, redrawn by ReJet */
            if (g_settings.hud_style == 1 &&
                (draw == 0x106AD0 || draw == 0x10E680 || draw == 0x10DEA0 || draw == 0x10A590 || draw == 0x1009F0 ||
                 draw == 0x102940 || draw == 0x104AD0 || draw == 0x1054D0 || draw == 0x10F640 ||
                 draw == 0xFBD40)) /* 0xFBD40: the soul pickup's 3D title, replaced by ReJet's "GET No. N" */
                skip = 1;
            if (g_hide == 2) { /* research: from frame 4200, hide HUD_METHODS[k] for 480 frames each, then none */
                static int last = -1;
                ULONG f = d3d_frame();
                int k = f < 4200 ? 6 : (int)((f - 4200) / 480) % 7;
                if (k != last)
                    log_printf("hud: cycle now hides %s %08lX\n", k < 6 ? "method" : "nothing", k < 6 ? HUD_METHODS[k] : 0);
                last = k;
                skip = k < 6 && draw == HUD_METHODS[k];
            }
            if (!skip) {
                ((MatrixFn)(UINT_PTR)0x1BB480)();
                g_hud_in_draw = draw >= HUD_CODE_START && draw < HUD_CODE_END;
                ((NodeDraw)(UINT_PTR)draw)(node, NULL, 0);
                g_hud_in_draw = 0;
                ((MatrixFn)(UINT_PTR)0x1BB510)();
            }
        }
        if (*(unsigned char **)(node + 0x28))
            hud_draw_tree(*(unsigned char **)(node + 0x28));
    }
}

int hud_values(HudValues *out)
{
    ULONG f = d3d_frame();
    *out = g_values;
    if (f - g_combo_frame > 2)
        out->combo_fade = out->combo_total_fade = 0;
    if (f - g_tricks_frame > 2)
        out->tricks_fade = 0;
    if (f - g_tag_frame > 2)
        out->tag_fade = 0;
    if (f - g_race_frame > 2)
        out->race_time = -1;
    { /* the newest popup still showing: full while the game's lives, then a 20-frame fade. Its timer stops
       * while the pickup's camera spin hides it, so while it was last seen holding (state < 5) it stays up (10 s cap) */
        int best = -1;
        out->soul_fade = 0;
        for (int k = 0; k < 4; k++) {
            ULONG t = f - g_souls[k].last;
            float fade;
            if (!g_souls[k].node || g_souls[k].retired)
                continue;
            if (g_souls[k].state < 5 && t < 600)
                t = 0;
            fade = t <= SOUL_HOLD ? 1.0f : t < SOUL_HOLD + 20 ? (float)(SOUL_HOLD + 20 - t) / 20.0f : 0;
            if (fade > 0 && (best < 0 || g_souls[k].first > g_souls[best].first)) {
                best = k;
                out->soul_fade = fade;
                out->soul = g_souls[k].soul;
            }
        }
    }
    return f - g_values_frame <= 2;
}

const HlePatch g_hud_patches[] = {
    {0x11220, (void *)hud_draw_tree, "game::draw node tree (HUD)"},
};
const unsigned g_hud_patch_count = sizeof g_hud_patches / sizeof g_hud_patches[0];
