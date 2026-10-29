/* XPP (Xbox USB peripherals) -> host XInput. Four gamepad ports; port 1 falls back to the
 * keyboard when no controller is connected there. Memory units and headsets report "none".
 *
 * Xbox layouts (XINPUT_STATE/GAMEPAD) differ from Windows XInput: digital buttons are only
 * the d-pad, Start, Back and stick clicks; A/B/X/Y/Black/White and both triggers are
 * pressure-sensitive bytes (bAnalogButtons).
 *
 * Copied from ReWhacked. JSRF's engine polls through its own DirectInput-style pad objects
 * (docs/SUBSYSTEMS.md, poll 0x167C50); the XDK 4134 XPP signatures match 4831's. Modern
 * controls (a camera hook in ReWhacked) are not ported yet. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Xinput.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "kernel.h"
#include "ui/menu.h"
#include "xboxkrnl_impl.h"
#include "xdk_addrs.gen.h"

#define GAMEPAD_TYPE ((const void *)(UINT_PTR)0x001BD320) /* XDEVICE_TYPE_GAMEPAD (pushed by the engine, 0x1677BA) */
#define PORTS 4

/* Xbox structures (natural alignment gives the Xbox offsets). */
typedef struct XB_GAMEPAD {
    WORD wButtons;            /* 1 up, 2 down, 4 left, 8 right, 0x10 start, 0x20 back, 0x40 LS, 0x80 RS */
    BYTE bAnalogButtons[8];   /* A, B, X, Y, Black, White, left trigger, right trigger */
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XB_GAMEPAD;

typedef struct XB_INPUT_STATE {
    DWORD dwPacketNumber;
    XB_GAMEPAD Gamepad;
} XB_INPUT_STATE;

typedef struct XB_FEEDBACK {
    DWORD dwStatus;
    HANDLE hEvent;
    BYTE Reserved[58];
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
} XB_FEEDBACK;

typedef struct Port {
    int open;
    DWORD packet;
    XB_GAMEPAD last;
} Port;

static Port g_ports[PORTS];
static DWORD g_known_devices;

static int keyboard_active(void)
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static int key(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* Keyboard layout for port 1: arrows = d-pad, Enter = Start, Backspace = Back,
 * Space/Z = A, X/Esc = B, C = X, V = Y, Q/E = left/right trigger, WASD = left stick. */
static void read_keyboard(XB_GAMEPAD *g)
{
    memset(g, 0, sizeof *g);
    if (!keyboard_active())
        return;
    g->wButtons = (WORD)((key(VK_UP) ? 1 : 0) | (key(VK_DOWN) ? 2 : 0) | (key(VK_LEFT) ? 4 : 0) |
                         (key(VK_RIGHT) ? 8 : 0) | (key(VK_RETURN) && !key(VK_MENU) ? 0x10 : 0) /* not Alt+Enter: full screen */ | (key(VK_BACK) ? 0x20 : 0));
    g->bAnalogButtons[0] = (key(VK_SPACE) || key('Z')) ? 255 : 0;
    g->bAnalogButtons[1] = (key('X') || key(VK_ESCAPE)) ? 255 : 0;
    g->bAnalogButtons[2] = key('C') ? 255 : 0;
    g->bAnalogButtons[3] = key('V') ? 255 : 0;
    g->bAnalogButtons[6] = key('Q') ? 255 : 0;
    g->bAnalogButtons[7] = key('E') ? 255 : 0;
    g->sThumbLX = (SHORT)((key('D') ? 32767 : 0) - (key('A') ? 32767 : 0));
    g->sThumbLY = (SHORT)((key('W') ? 32767 : 0) - (key('S') ? 32767 : 0));
}

/* Test hook (REJET_AUTOPRESS=1): on port 1, tap Start then A for 0.2 s every 3 s, so
 * automated runs can get past the intro and through menus. */
static int autopress(XB_GAMEPAD *g)
{
    static int enabled = -1;
    ULONGLONG t;
    if (enabled < 0) { /* 1 = Start then A, 2 = A only */
        char env[4] = "";
        enabled = GetEnvironmentVariableA("REJET_AUTOPRESS", env, sizeof env) ? env[0] - '0' : 0;
    }
    if (enabled <= 0)
        return 0;
    { /* REJET_AUTOPRESS_UNTIL=<seconds>: stop pressing after that long */
        static ULONGLONG start, until = (ULONGLONG)-1;
        if (!start) {
            char env[16] = "";
            start = GetTickCount64();
            if (GetEnvironmentVariableA("REJET_AUTOPRESS_UNTIL", env, sizeof env))
                until = (ULONGLONG)atoi(env) * 1000;
        }
        if (GetTickCount64() - start > until)
            return 0;
    }
    t = GetTickCount64() % 3000;
    if (t < 200 && enabled == 1)
        g->wButtons |= 0x10;            /* Start */
    else if (t >= 1500 && t < 1700)
        g->bAnalogButtons[0] = 255;     /* A */
    return 1;
}

/* Test hook: REJET_INPUT="<seconds>[@<port 1-4>]:<button>,..." presses a button for 0.2 s at
 * that time (seconds from the first input poll), e.g. "3:start,5:down,6:a,9@2:start". Buttons:
 * a b x y black white start back up down left right lt rt, and ls_up/ls_down/ls_left/ls_right
 * (left stick pushed fully). Ports named in the script count as connected controllers, so split
 * screen can be tested without real pads. */
#define SCRIPT_MAX 128
typedef struct ScriptPress {
    ULONGLONG at_ms;
    DWORD port;
    char button[12];
} ScriptPress;
static ScriptPress g_script[SCRIPT_MAX];
static int g_script_n = -1;
static DWORD g_script_ports; /* ports the script uses (virtual controllers) */
static ULONGLONG g_script_t0;

static void script_load(void)
{
    char env[4096], *tok, *ctx = NULL;
    g_script_n = 0;
    if (!GetEnvironmentVariableA("REJET_INPUT", env, sizeof env))
        return;
    for (tok = strtok_s(env, ",", &ctx); tok && g_script_n < SCRIPT_MAX; tok = strtok_s(NULL, ",", &ctx)) {
        ScriptPress *p = &g_script[g_script_n];
        char *colon = strchr(tok, ':'), *at = strchr(tok, '@');
        if (!colon)
            continue;
        *colon = 0;
        p->port = 0;
        if (at && at < colon) {
            *at = 0;
            p->port = (DWORD)(atoi(at + 1) - 1) & 3;
        }
        p->at_ms = (ULONGLONG)(atof(tok) * 1000.0);
        snprintf(p->button, sizeof p->button, "%s", colon + 1);
        g_script_ports |= 1u << p->port;
        g_script_n++;
    }
    log_printf("input: script with %d presses, virtual controllers mask %lX\n", g_script_n, g_script_ports);
}

static void script_apply(DWORD port, XB_GAMEPAD *g)
{
    ULONGLONG t;
    if (g_script_n < 0)
        script_load();
    if (!g_script_n)
        return;
    if (!g_script_t0)
        g_script_t0 = GetTickCount64();
    t = GetTickCount64() - g_script_t0;
    for (int i = 0; i < g_script_n; i++) {
        const ScriptPress *p = &g_script[i];
        const char *b = p->button;
        if (p->port != port || t < p->at_ms || t >= p->at_ms + 200)
            continue;
        { /* log each press once, when the game first sees it */
            static unsigned char logged[SCRIPT_MAX];
            if (!logged[i]) {
                logged[i] = 1;
                log_printf("input: script %.1f s port %lu %s (seen at %.2f s)\n", p->at_ms / 1000.0, p->port + 1,
                           b, t / 1000.0);
            }
        }
        if (!strcmp(b, "up")) g->wButtons |= 1;
        else if (!strcmp(b, "down")) g->wButtons |= 2;
        else if (!strcmp(b, "left")) g->wButtons |= 4;
        else if (!strcmp(b, "right")) g->wButtons |= 8;
        else if (!strcmp(b, "start")) g->wButtons |= 0x10;
        else if (!strcmp(b, "back")) g->wButtons |= 0x20;
        else if (!strcmp(b, "a")) g->bAnalogButtons[0] = 255;
        else if (!strcmp(b, "b")) g->bAnalogButtons[1] = 255;
        else if (!strcmp(b, "x")) g->bAnalogButtons[2] = 255;
        else if (!strcmp(b, "y")) g->bAnalogButtons[3] = 255;
        else if (!strcmp(b, "black")) g->bAnalogButtons[4] = 255;
        else if (!strcmp(b, "white")) g->bAnalogButtons[5] = 255;
        else if (!strcmp(b, "lt")) g->bAnalogButtons[6] = 255;
        else if (!strcmp(b, "rt")) g->bAnalogButtons[7] = 255;
        else if (!strcmp(b, "ls_up")) g->sThumbLY = 32767;
        else if (!strcmp(b, "ls_down")) g->sThumbLY = -32767;
        else if (!strcmp(b, "ls_left")) g->sThumbLX = -32767;
        else if (!strcmp(b, "ls_right")) g->sThumbLX = 32767;
    }
}

/* Host controller -> Xbox gamepad. Returns 0 if nothing is connected on that port. */
static int read_port(DWORD port, XB_GAMEPAD *g)
{
    XINPUT_STATE s;
    static int no_xinput = -1; /* REJET_NOXINPUT=1: ignore controllers (keyboard only) */
    if (no_xinput < 0)
        no_xinput = GetEnvironmentVariableA("REJET_NOXINPUT", NULL, 0) > 0;
    if (g_script_n < 0)
        script_load();
    if (port == 0 && (no_xinput || XInputGetState(0, &s) != ERROR_SUCCESS)) {
        read_keyboard(g);
        autopress(g);
        script_apply(port, g);
        return 1;
    }
    if (no_xinput || XInputGetState(port, &s) != ERROR_SUCCESS) {
        if (port == 0) {
            read_keyboard(g);
            script_apply(port, g);
            return 1;
        }
        if (g_script_ports & (1u << port)) { /* virtual controller from REJET_INPUT */
            memset(g, 0, sizeof *g);
            script_apply(port, g);
            return 1;
        }
        return 0;
    }
    memset(g, 0, sizeof *g);
    g->wButtons = s.Gamepad.wButtons & 0x00FF; /* d-pad, start, back, stick clicks share bits */
    g->bAnalogButtons[0] = (s.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    g->bAnalogButtons[1] = (s.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    g->bAnalogButtons[2] = (s.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    g->bAnalogButtons[3] = (s.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    g->bAnalogButtons[4] = (s.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0; /* Black */
    g->bAnalogButtons[5] = (s.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;  /* White */
    g->bAnalogButtons[6] = s.Gamepad.bLeftTrigger;
    g->bAnalogButtons[7] = s.Gamepad.bRightTrigger;
    g->sThumbLX = s.Gamepad.sThumbLX;
    g->sThumbLY = s.Gamepad.sThumbLY;
    g->sThumbRX = s.Gamepad.sThumbRX;
    g->sThumbRY = s.Gamepad.sThumbRY;
    if (port == 0)
        autopress(g); /* test hook also works with a controller connected */
    script_apply(port, g);
    return 1;
}

static DWORD connected_mask(void)
{
    DWORD mask = 0;
    XB_GAMEPAD g;
    for (DWORD p = 0; p < PORTS; p++)
        if (read_port(p, &g))
            mask |= 1u << p;
    return mask;
}

static void XBAPI hle_XInitDevices(DWORD PreallocTypeCount, const void *PreallocTypes)
{
    (void)PreallocTypeCount;
    (void)PreallocTypes;
    g_known_devices = connected_mask();
    log_printf("input: XInitDevices, gamepads on ports mask %lX (port 1 falls back to keyboard)\n",
               g_known_devices);
}

static DWORD XBAPI hle_XGetDevices(const void *DeviceType)
{
    if (DeviceType != GAMEPAD_TYPE)
        return 0; /* no memory units or headsets */
    return g_known_devices = connected_mask();
}

static BOOL XBAPI hle_XGetDeviceChanges(const void *DeviceType, DWORD *pInsertions, DWORD *pRemovals)
{
    DWORD now, before = g_known_devices;
    *pInsertions = *pRemovals = 0;
    if (DeviceType != GAMEPAD_TYPE)
        return FALSE;
    now = connected_mask();
    g_known_devices = now;
    *pInsertions = now & ~before;
    *pRemovals = before & ~now;
    if (*pInsertions || *pRemovals)
        log_printf("input: ports inserted %lX removed %lX\n", *pInsertions, *pRemovals);
    return *pInsertions || *pRemovals;
}

static HANDLE XBAPI hle_XInputOpen(const void *DeviceType, DWORD dwPort, DWORD dwSlot, const void *pPolling)
{
    (void)dwSlot;
    (void)pPolling;
    if (DeviceType != GAMEPAD_TYPE || dwPort >= PORTS)
        return NULL;
    g_ports[dwPort].open = 1;
    return (HANDLE)&g_ports[dwPort];
}

static void XBAPI hle_XInputClose(HANDLE h)
{
    if (h)
        ((Port *)h)->open = 0;
}

static DWORD XBAPI hle_XInputPoll(HANDLE h)
{
    (void)h;
    return ERROR_SUCCESS;
}

static DWORD XBAPI hle_XInputGetState(HANDLE h, XB_INPUT_STATE *pState)
{
    Port *p = (Port *)h;
    XB_GAMEPAD g;
    if (!p || !read_port((DWORD)(p - g_ports), &g))
        return ERROR_DEVICE_NOT_CONNECTED;
    if (menu_filter_pad((unsigned)(p - g_ports), &g.wButtons)) /* ReJet menu open, or just closed */
        memset(&g, 0, sizeof g);
    if (memcmp(&g, &p->last, sizeof g)) {
        p->packet++;
        p->last = g;
    }
    pState->dwPacketNumber = p->packet;
    pState->Gamepad = g;
    return ERROR_SUCCESS;
}

static DWORD XBAPI hle_XInputSetState(HANDLE h, XB_FEEDBACK *pFeedback)
{
    Port *p = (Port *)h;
    if (p) {
        XINPUT_VIBRATION v;
        v.wLeftMotorSpeed = pFeedback->wLeftMotorSpeed;
        v.wRightMotorSpeed = pFeedback->wRightMotorSpeed;
        XInputSetState((DWORD)(p - g_ports), &v);
    }
    pFeedback->dwStatus = ERROR_SUCCESS; /* completes immediately */
    if (pFeedback->hEvent)
        SetEvent(pFeedback->hEvent);
    return ERROR_IO_PENDING;
}

static DWORD XBAPI hle_XInputGetCapabilities(HANDLE h, BYTE *pCapabilities)
{
    if (!h)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(pCapabilities, 0xFF, 26); /* every input and both motors supported */
    pCapabilities[0] = 1;            /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    pCapabilities[1] = pCapabilities[2] = pCapabilities[3] = 0;
    return ERROR_SUCCESS;
}

/* XInitDevices has two entries named by XbSymbolDatabase; the engine calls the jmp stub at
 * 0x1BE0BA (to 0x1BD68C), which is patched. */
const HlePatch g_input_patches[] = {
    {0x001BE0BAu, (void *)hle_XInitDevices, "XInitDevices"},
    {XDK_XAPILIB_XGetDevices, (void *)hle_XGetDevices, "XGetDevices"},
    {XDK_XAPILIB_XGetDeviceChanges, (void *)hle_XGetDeviceChanges, "XGetDeviceChanges"},
    {XDK_XAPILIB_XInputOpen, (void *)hle_XInputOpen, "XInputOpen"},
    {XDK_XAPILIB_XInputClose, (void *)hle_XInputClose, "XInputClose"},
    {XDK_XAPILIB_XInputPoll, (void *)hle_XInputPoll, "XInputPoll"},
    {XDK_XAPILIB_XInputGetState, (void *)hle_XInputGetState, "XInputGetState"},
    {XDK_XAPILIB_XInputSetState, (void *)hle_XInputSetState, "XInputSetState"},
    {XDK_XAPILIB_XInputGetCapabilities, (void *)hle_XInputGetCapabilities, "XInputGetCapabilities"},
};
const unsigned g_input_patch_count = sizeof g_input_patches / sizeof g_input_patches[0];
