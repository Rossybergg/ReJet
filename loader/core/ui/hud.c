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
static HudValues g_values;
static ULONG g_values_frame = (ULONG)-10; /* when the health node last drew */
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
        {0x106AD0, "health", {0x48, 0x60, 0xA8, 0xC0}},
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
            int skip = g_hide == 1 && draw >= HUD_CODE_START && draw < HUD_CODE_END;
            if (draw == 0x10DEA0) { /* speed: current (+0x1D0, clamped) over the maximum (+0x1D4), as its update (0x10DDA0) */
                float top = *(const float *)(node + 0x1D4);
                g_values.speed = top > 0 ? *(const float *)(node + 0x1D0) / top : 0;
            }
            if (draw == 0x106AD0) { /* health: the fill quad (+0xA8..+0xC0) over the background (+0x48..+0x60) */
                float bg = *(const float *)(node + 0x60) - *(const float *)(node + 0x48);
                float fill = *(const float *)(node + 0xC0) - *(const float *)(node + 0xA8);
                g_values.health = bg > 0 ? fill / bg : 0;
                g_values_frame = d3d_frame();
            }
            else if (draw == 0x10E680) { /* cans held, and the cap */
                g_values.cans = *(const int *)(node + 0xB8);
                g_values.cans_max = *(const int *)(node + 0xD8);
            }
            /* HUD: ReJet draws its own health, cans and speed meter (menu.c) */
            if (g_settings.hud_style == 1 && (draw == 0x106AD0 || draw == 0x10E680 || draw == 0x10DEA0))
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
                ((NodeDraw)(UINT_PTR)draw)(node, NULL, 0);
                ((MatrixFn)(UINT_PTR)0x1BB510)();
            }
        }
        if (*(unsigned char **)(node + 0x28))
            hud_draw_tree(*(unsigned char **)(node + 0x28));
    }
}

int hud_values(HudValues *out)
{
    *out = g_values;
    return d3d_frame() - g_values_frame <= 2;
}

const HlePatch g_hud_patches[] = {
    {0x11220, (void *)hud_draw_tree, "game::draw node tree (HUD)"},
};
const unsigned g_hud_patch_count = sizeof g_hud_patches / sizeof g_hud_patches[0];
