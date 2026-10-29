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

#include "hle.h"
#include "kernel.h"
#include "ui/hud.h"

#define HUD_CODE_START 0x100000u /* the HUD module's draw methods */
#define HUD_CODE_END 0x10F000u

typedef void(__fastcall *NodeDraw)(void *self, void *edx_unused, int arg); /* thiscall, 1 argument */
typedef void (*MatrixFn)(void);

ULONG d3d_frame(void); /* d3d.c */

static int g_hide = -1; /* REJET_HUDHIDE=1: leave out the original HUD; =cycle: one method at a time */
static const DWORD HUD_METHODS[] = {0x1080A0, 0x10DEA0, 0x106AD0, 0x105A90, 0x10E680, 0x10A590};
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
            int skip = g_hide == 1 && draw >= HUD_CODE_START && draw < HUD_CODE_END;
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

const HlePatch g_hud_patches[] = {
    {0x11220, (void *)hud_draw_tree, "game::draw node tree (HUD)"},
};
const unsigned g_hud_patch_count = sizeof g_hud_patches / sizeof g_hud_patches[0];
