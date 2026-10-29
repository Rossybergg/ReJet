/* Xbox D3D8 (XDK 4134) -> Direct3D 9Ex shim (HLE), milestone 1: window, device, Clear, Swap.
 *
 * Built from ReWhacked's shim (XDK 4831), re-derived for JSRF. Every D3D entry point the engine
 * layer calls is patched (config/d3d_abi.csv gives each one's argument bytes and register
 * use, from the machine code), because any original one left in place would write the NV2A
 * push buffer or GPU registers. Game code never touches D3D's device or push buffer itself
 * (no .text references to D3D__pDevice 0x19E7A0 or the push-buffer pointers 0x19BCC0), so
 * nothing else needs catching.
 *
 * Implemented: device creation, Reset, Clear, Swap, the vertical blank, resources (textures,
 * surfaces, vertex/index buffers as Xbox-layout objects in game memory) and their locks, and,
 * from ReWhacked, NV2A vertex programs (vsh.c), register combiners (psh.c), textures (tex.c),
 * render and texture state, transforms, lights, viewport and draws; render-to-texture and
 * immediate mode (Begin/SetVertexData/End). Not yet: tiling, gamma; these are typed stubs. Pure helpers that only read static data or call the
 * kernel (adapter queries, D3D_SetPushBufferSize, D3D_AllocContiguousMemory,
 * D3DResource_GetType/Register, XGRAPHC) keep running as original code. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <dwmapi.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Xinput.h>

#include "hle.h"
#include "kernel.h"
#include "memdump.h"
#include "perf.h"
#include "psh.h"
#include "settings.h"
#include "tex.h"
#include "ui/menu.h"
#include "version.gen.h"
#include "vsh.h"
#include "widescreen.h"
#include "xbox/xbox_d3d.h"
#include "xboxkrnl_impl.h"
#include "xdk_addrs.gen.h"

#define D3D_DEVICE_GLOBAL 0x0019E7A0u /* D3D__pDevice (D3D section BSS) */
#define XDEVICE_SIZE      0x10000u    /* original code reads fields up to ~0x2440 */

static IDirect3D9Ex *g_d3d;
static IDirect3DDevice9Ex *g_dev;
static HWND g_wnd;
static unsigned char *g_xdevice; /* stand-in Xbox device object */
static XD3DSurface g_backbuffer, g_depth;
static ULONG g_frame;

/* The frame number, for research switches elsewhere (hud.c). */
ULONG d3d_frame(void) { return g_frame; }

/* Render resolution: the game draws for g_xw x g_xh (640x480); the host back buffer is
 * g_hw x g_hh (settings). Clear rectangles are given in game pixels and scaled. */
static UINT g_xw = 640, g_xh = 480, g_hw = 640, g_hh = 480;
static float g_sx = 1.0f, g_sy = 1.0f;
/* The current render target (the back buffer or a texture): size in game pixels and host scale. */
static UINT g_tw = 640, g_th = 480;
static float g_tsx = 1.0f, g_tsy = 1.0f;
static IDirect3DSurface9 *g_host_bb, *g_host_depth; /* the device's own, for switching back from a render target */
static RECT g_vp_rect;   /* the Xbox viewport in render pixels (split screen) */
static int g_vp_partial; /* viewport smaller than the screen: scissor and Clear use g_vp_rect */

/* Draw state (ReWhacked's model). */
static float g_vconst[VSH_CONSTANTS][4]; /* NV2A constant file (+ our extras) */
static const XD3DPixelContainer *g_textures[4];
static XD3DResource *g_stream_vb;
static UINT g_stream_stride;
static UINT g_base_vertex;
static int g_in_scene;
static ULONG g_draws, g_skipped, g_untextured; /* per-600-frame counters for the log */
static ULONG g_frame_draws;                   /* this frame's draws (the start-up splash) */
static volatile LONG g_shot_request;          /* F12 */
/* Manual-testing aids (docs/TESTING.md): F10 writes a numbered marker into the log; F11 captures
 * the next frame (screenshot, every draw with its state, the render-target textures) into
 * captures\capture_NNN_<time>\. REJET_CAPTURE_AT=<frame> does the same at a given frame. */
static volatile LONG g_mark_request, g_capture_request;
static ULONG g_capture_frame = 0xFFFFFFFF; /* frame being captured */
static char g_capture_dir[MAX_PATH];
static FILE *g_capture_log;
static const struct RenderTarget *g_cur_rt; /* the current render target, NULL = the back buffer */
static char g_capture_note[256];             /* bug report from the ReJet menu, for the next capture */
static CRITICAL_SECTION g_capture_note_lock;

static void present(void);
static void change_display(void);
static void apply_window(void);
static void widescreen_install(void);
static D3DPRESENT_PARAMETERS g_hp; /* the device's present parameters (for ResetEx) */
static void request_capture(const char *note);

/* Log the first call of each not-yet-implemented entry point. */
#define TODO_ONCE(name)                                                     \
    do {                                                                    \
        static LONG seen;                                                   \
        if (!InterlockedExchange(&seen, 1))                                 \
            log_printf("d3d: %s not implemented yet (first call from %08X)\n", name, \
                       (unsigned)(UINT_PTR)_ReturnAddress());               \
    } while (0)

/* Survey logging: the first `n` calls of a kind. */
#define SURVEY(n) static LONG survey_count; if (InterlockedIncrement(&survey_count) <= (n))

/* ------------------------------------------------------------------ window */

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_CLOSE) {
        log_printf("d3d: window closed; exiting\n");
        ExitProcess(0);
    }
    if (m == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && !menu_is_open()) { /* the game has no pointer */
        SetCursor(NULL);
        return TRUE;
    }
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && wp == VK_F12) {
        InterlockedExchange(&g_shot_request, 1);
        return 0;
    }
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && wp == VK_F11) {
        InterlockedExchange(&g_capture_request, 1);
        return 0;
    }
    if (m == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) { /* Alt+Enter: full screen on or off */
        if (!(lp & (1 << 30))) { /* not a key repeat */
            g_settings.fullscreen = !g_settings.fullscreen;
            settings_save();
            apply_window(); /* this thread owns the window */
            log_printf("d3d: Alt+Enter: %s\n", g_settings.fullscreen ? "full screen" : "window");
        }
        return 0;
    }
    if (m == WM_SYSCHAR && wp == VK_RETURN) /* no beep for Alt+Enter */
        return 0;
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && wp == VK_F10) { /* F10 comes as a system key */
        InterlockedIncrement(&g_mark_request);
        return 0;
    }
    if (menu_window_message(m, wp))
        return 0;
    return DefWindowProcW(w, m, wp, lp);
}

/* The window lives on its own thread, which only pumps its messages: game threads present
 * irregularly (loading), and Windows marks a window "Not Responding" when its owner thread
 * stops reading messages for 5 s. */
typedef struct WindowRequest {
    int width, height;
    HANDLE ready;
} WindowRequest;

static DWORD WINAPI window_thread(void *arg)
{
    WindowRequest *req = (WindowRequest *)arg;
    WNDCLASSEXW wc;
    RECT r = {0, 0, req->width, req->height};
    MSG msg;
    HMODULE self = NULL;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    /* the ReJet icon from rejet.dll: ReJet.exe's resources are overwritten by the XBE image */
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)wnd_proc, &self);
    wc.hIcon = LoadIconW(self, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"rejet";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); /* letterbox bars */
    RegisterClassExW(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowW(L"rejet", L"ReJet - Jet Set Radio Future",
                          WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                          CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    SetEvent(req->ready);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* Window size or full screen changed (ReJet menu, or at start-up): a borderless window over the
 * monitor, or a normal window of the render size times the window scale, centred. */
static UINT window_dpi(void)
{
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    GetDpiForWindowFn fn = (GetDpiForWindowFn)(void *)GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForWindow");
    UINT dpi = fn && g_wnd ? fn(g_wnd) : 96;
    return dpi ? dpi : 96;
}

static void apply_window(void)
{
    HMONITOR mon = MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof mi};
    UINT dpi = window_dpi();
    GetMonitorInfoW(mon, &mi);
    if (g_settings.fullscreen) {
        SetWindowLongW(g_wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    else {
        /* the window height is in Windows' scaled pixels, independent of the render resolution; the
         * aspect setting's shape, shrunk to fit the screen */
        float ar = settings_aspect_ratio(g_settings.aspect);
        LONG ch = (LONG)((LONGLONG)g_settings.window_height * dpi / 96), cw = (LONG)((float)ch * ar + 0.5f);
        RECT r, frame = {0, 0, 0, 0};
        LONG ww, wh;
        AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
        if (ch + (frame.bottom - frame.top) > mi.rcWork.bottom - mi.rcWork.top) {
            ch = mi.rcWork.bottom - mi.rcWork.top - (frame.bottom - frame.top);
            cw = (LONG)((float)ch * ar + 0.5f);
        }
        r.left = r.top = 0;
        r.right = cw;
        r.bottom = ch;
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        ww = r.right - r.left;
        wh = r.bottom - r.top;
        SetWindowLongW(g_wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_wnd, HWND_NOTOPMOST, (mi.rcWork.left + mi.rcWork.right - ww) / 2,
                     (mi.rcWork.top + mi.rcWork.bottom - wh) / 2 > mi.rcWork.top
                         ? (mi.rcWork.top + mi.rcWork.bottom - wh) / 2
                         : mi.rcWork.top,
                     ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

/* ReJet draws its own pixels at any resolution: tell Windows, so it doesn't enlarge (and blur)
 * the window on scaled displays. Before any window exists. */
static void become_dpi_aware(void)
{
    typedef BOOL(WINAPI * SetDpiContextFn)(HANDLE);
    SetDpiContextFn fn =
        (SetDpiContextFn)(void *)GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext");
    if (fn && !fn((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */))
        log_printf("d3d: could not make the process DPI aware (%lu)\n", GetLastError());
}

static void create_window(int width, int height)
{
    WindowRequest req = {width, height, CreateEventW(NULL, TRUE, FALSE, NULL)};
    CloseHandle(CreateThread(NULL, 0, window_thread, &req, 0, NULL));
    WaitForSingleObject(req.ready, INFINITE);
    CloseHandle(req.ready);
}

static D3DFORMAT host_format(uint32_t xfmt)
{
    switch (xfmt) {
    case XD3DFMT_A8R8G8B8: case XD3DFMT_LIN_A8R8G8B8: return D3DFMT_A8R8G8B8;
    case XD3DFMT_X8R8G8B8: case XD3DFMT_LIN_X8R8G8B8: return D3DFMT_X8R8G8B8;
    case XD3DFMT_R5G6B5: case XD3DFMT_LIN_R5G6B5: return D3DFMT_R5G6B5;
    case XD3DFMT_D24S8: case XD3DFMT_LIN_D24S8: case XD3DFMT_F24S8: return D3DFMT_D24S8;
    case XD3DFMT_D16: case XD3DFMT_LIN_D16: case XD3DFMT_F16: return D3DFMT_D16;
    default: return D3DFMT_UNKNOWN;
    }
}

/* ------------------------------------------------------------------ device */

/* Xbox runtime behaviour: the viewport sets the reserved constants c-38 (scale) and c-37
 * (offset) that every 3D program's screen-space epilogue uses. Depth stays in [0, 1]. */
static void set_screen_constants(const D3DVIEWPORT9 *vp)
{
    float *scale = g_vconst[96 - 38], *offset = g_vconst[96 - 37];
    scale[0] = (float)vp->Width * 0.5f;
    scale[1] = -(float)vp->Height * 0.5f;
    scale[2] = vp->MaxZ - vp->MinZ;
    scale[3] = 1.0f;
    offset[0] = (float)vp->X + (float)vp->Width * 0.5f;
    offset[1] = (float)vp->Y + (float)vp->Height * 0.5f;
    offset[2] = vp->MinZ;
    offset[3] = 0.0f;
}

void d3d_note_shader_compile(void) /* shadercache.c, on a cache miss (no progress screen yet) */
{
}

/* ------------------------------------------------------------------ anti-aliasing (MSAA) */

/* With anti-aliasing on, the game's back buffer and depth (g_host_bb, g_host_depth) are a
 * multisampled colour and depth surface instead of the device's own; the scene is resolved
 * into the device's back buffer (g_real_bb) at Swap, before screenshots, captures, the ReJet
 * menu and Present read it, and before the game copies the screen (CopyRects: the boost
 * effect). Render-to-texture targets stay single-sampled. Off (the default) uses the device's
 * surfaces directly, as before. */
static IDirect3DSurface9 *g_real_bb, *g_ms_bb, *g_ms_depth;
static D3DMULTISAMPLE_TYPE g_ms_type;

/* The setting's sample count, lowered to what the graphics card supports. */
static D3DMULTISAMPLE_TYPE msaa_wanted(void)
{
    for (int n = g_settings.antialiasing >= 8 ? 8 : g_settings.antialiasing >= 4 ? 4 : g_settings.antialiasing >= 2 ? 2 : 0;
         n >= 2; n /= 2)
        if (SUCCEEDED(IDirect3D9Ex_CheckDeviceMultiSampleType(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                               D3DFMT_X8R8G8B8, TRUE, (D3DMULTISAMPLE_TYPE)n, NULL)) &&
            SUCCEEDED(IDirect3D9Ex_CheckDeviceMultiSampleType(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                               D3DFMT_D24S8, TRUE, (D3DMULTISAMPLE_TYPE)n, NULL)))
            return (D3DMULTISAMPLE_TYPE)n;
    return D3DMULTISAMPLE_NONE;
}

static void msaa_release(void)
{
    IDirect3DSurface9 **s[] = {&g_host_bb, &g_host_depth, &g_real_bb, &g_ms_bb, &g_ms_depth};
    for (unsigned i = 0; i < sizeof s / sizeof s[0]; i++)
        if (*s[i]) {
            IDirect3DSurface9_Release(*s[i]);
            *s[i] = NULL;
        }
    g_ms_type = D3DMULTISAMPLE_NONE;
}

/* After device creation or a reset (the device's own back buffer and depth are current):
 * pick the game's back buffer and depth, multisampled or not, and make them current. */
static void msaa_setup(void)
{
    D3DMULTISAMPLE_TYPE t = msaa_wanted();
    msaa_release();
    IDirect3DDevice9Ex_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &g_real_bb);
    if (t != D3DMULTISAMPLE_NONE &&
        SUCCEEDED(IDirect3DDevice9Ex_CreateRenderTarget(g_dev, g_hw, g_hh, D3DFMT_X8R8G8B8, t, 0, FALSE, &g_ms_bb,
                                                         NULL)) &&
        SUCCEEDED(IDirect3DDevice9Ex_CreateDepthStencilSurface(g_dev, g_hw, g_hh, D3DFMT_D24S8, t, 0, FALSE,
                                                                &g_ms_depth, NULL))) {
        g_ms_type = t;
        g_host_bb = g_ms_bb;
        g_host_depth = g_ms_depth;
        IDirect3DSurface9_AddRef(g_host_bb);
        IDirect3DSurface9_AddRef(g_host_depth);
        IDirect3DDevice9Ex_SetRenderTarget(g_dev, 0, g_host_bb);
        IDirect3DDevice9Ex_SetDepthStencilSurface(g_dev, g_host_depth);
        IDirect3DDevice9Ex_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    }
    else {
        if (g_ms_bb) { /* the depth surface failed */
            IDirect3DSurface9_Release(g_ms_bb);
            g_ms_bb = NULL;
        }
        IDirect3DDevice9Ex_GetRenderTarget(g_dev, 0, &g_host_bb);
        IDirect3DDevice9Ex_GetDepthStencilSurface(g_dev, &g_host_depth);
    }
    log_printf("d3d: anti-aliasing %s (setting %d)\n", g_ms_type ? (g_ms_type == 2 ? "2x MSAA" : g_ms_type == 4 ? "4x MSAA" : "8x MSAA") : "off",
               g_settings.antialiasing);
}

/* The multisampled scene into the device's back buffer (nothing to do with anti-aliasing off). */
static void msaa_resolve(void)
{
    if (g_ms_bb && g_real_bb)
        IDirect3DDevice9Ex_StretchRect(g_dev, g_ms_bb, NULL, g_real_bb, NULL, D3DTEXF_NONE);
}

static HRESULT XBAPI hle_Direct3D_CreateDevice(UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                                               XD3DPRESENT_PARAMETERS *pp, void **ppDevice)
{
    D3DPRESENT_PARAMETERS hp;
    HRESULT hr;
    UINT w = pp->BackBufferWidth ? pp->BackBufferWidth : 640, h = pp->BackBufferHeight ? pp->BackBufferHeight : 480;

    (void)Adapter;
    (void)DeviceType;
    (void)hFocusWindow;
    (void)BehaviorFlags;
    log_printf("d3d: CreateDevice %ux%u fmt %02X x%u depth %d/%02X flags %08X msaa %08X refresh %u interval %08X\n", w,
               h, pp->BackBufferFormat, pp->BackBufferCount, pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat,
               pp->Flags, pp->MultiSampleType, pp->FullScreen_RefreshRateInHz, pp->FullScreen_PresentationInterval);
    if (g_dev) { /* the renderer's Initialize only creates once; Reset handles mode changes */
        *ppDevice = g_xdevice;
        return S_OK;
    }
    g_xw = w;
    g_xh = h;
    g_settings.width = ((int)((float)g_settings.height * settings_aspect_ratio(g_settings.aspect) + 0.5f) + 1) & ~1;
    g_hw = (UINT)g_settings.width;
    g_hh = (UINT)g_settings.height;
    g_sx = (float)g_hw / (float)g_xw;
    g_sy = (float)g_hh / (float)g_xh;
    become_dpi_aware();
    create_window(g_settings.window_height * 4 / 3, g_settings.window_height); /* apply_window sizes it properly */

    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &g_d3d))) {
        log_printf("error: Direct3DCreate9Ex failed\n");
        return E_FAIL;
    }
    memset(&hp, 0, sizeof hp);
    hp.BackBufferWidth = g_hw;
    hp.BackBufferHeight = g_hh;
    hp.BackBufferFormat = D3DFMT_X8R8G8B8; /* the game asks for 16-bit R5G6B5; 32-bit avoids banding */
    hp.BackBufferCount = 1;
    /* COPY keeps the back buffer's contents after Present: the boost effect blends the new frame
     * over the previous one (its speed trails); with DISCARD the screen darkened instead. */
    hp.SwapEffect = D3DSWAPEFFECT_COPY;
    hp.hDeviceWindow = g_wnd;
    hp.Windowed = TRUE;
    hp.EnableAutoDepthStencil = pp->EnableAutoDepthStencil;
    hp.AutoDepthStencilFormat = D3DFMT_D24S8;
    hp.PresentationInterval = g_settings.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    log_printf("d3d: rendering at %ux%u (game %ux%u), vsync %s\n", g_hw, g_hh, g_xw, g_xh,
               g_settings.vsync ? "on" : "off");
    /* FPU_PRESERVE: D3D9 would otherwise drop the x87 FPU to single precision and change the
     * game's floating-point results (CLAUDE.md). */
    hr = IDirect3D9Ex_CreateDeviceEx(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE |
                                         D3DCREATE_MULTITHREADED,
                                     &hp, NULL, &g_dev);
    if (FAILED(hr)) {
        log_printf("error: CreateDeviceEx failed (%08lX)\n", hr);
        return hr;
    }
    g_hp = hp;

    /* Stand-in Xbox device object: original helpers that still run read fields through it. */
    g_xdevice = (unsigned char *)calloc(1, XDEVICE_SIZE);
    *(void **)(UINT_PTR)D3D_DEVICE_GLOBAL = g_xdevice;
    if (ppDevice)
        *ppDevice = g_xdevice;

    g_backbuffer.Container.Resource.Common = 1 | (XD3DCOMMON_TYPE_SURFACE << XD3DCOMMON_TYPE_SHIFT);
    g_backbuffer.Container.Format = (pp->BackBufferFormat << 8) | (2 << 4);
    g_backbuffer.Container.Size = (w - 1) | ((h - 1) << 12) | (((w * 2 + 63) / 64 - 1) << 24);
    g_depth.Container.Resource.Common = 1 | (XD3DCOMMON_TYPE_SURFACE << XD3DCOMMON_TYPE_SHIFT);
    g_depth.Container.Format = (pp->AutoDepthStencilFormat << 8) | (2 << 4);
    g_depth.Container.Size = g_backbuffer.Container.Size;
    IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    for (DWORD s = 0; s < 4; s++) {
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    }
    { /* screen-space constants for a full-target viewport until SetViewport is called */
        D3DVIEWPORT9 vp = {0, 0, w, h, 0.0f, 1.0f};
        set_screen_constants(&vp);
    }
    g_vconst[VSH_C_SCREEN][0] = 2.0f / (float)w;
    g_vconst[VSH_C_SCREEN][1] = -2.0f / (float)h;
    g_vconst[VSH_C_SCREEN][2] = g_vconst[VSH_C_SCREEN][3] = 1.0f;
    g_tw = w;
    g_th = h;
    g_tsx = g_sx;
    g_tsy = g_sy;
    msaa_setup(); /* the game's back buffer and depth (multisampled with anti-aliasing on) */
    /* no widescreen squeeze yet: 2D programs keep their x scale */
    g_vconst[VSH_C_2D][0] = g_vconst[VSH_C_2D][3] = 1.0f;
    g_vconst[VSH_C_2D][1] = g_vconst[VSH_C_2D][2] = 0.0f; /* not HUD, no shift */
    log_printf("d3d: Direct3D 9Ex device ready, window %p\n", (void *)g_wnd);
    {
        MenuHost host = {(struct IDirect3DDevice9Ex *)g_dev, g_wnd, present, request_capture, apply_window, change_display};
        menu_init(&host);
    }
    apply_window(); /* full screen, or the window sized for the display scaling */
    widescreen_install();
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_Reset(XD3DPRESENT_PARAMETERS *pp)
{
    log_printf("d3d: Reset %ux%u flags %08X (ignored for now)\n", pp->BackBufferWidth, pp->BackBufferHeight,
               pp->Flags);
    return S_OK;
}

static ULONG XBAPI hle_D3DDevice_AddRef(void)
{
    return 2;
}

/* The engine tears down with `while (Release() != 0)` loops, so report the last reference. */
static ULONG XBAPI hle_D3DDevice_Release(void)
{
    log_printf("d3d: D3DDevice_Release (device kept; from %08X)\n", (unsigned)(UINT_PTR)_ReturnAddress());
    return 0;
}

/* Xbox clear flags differ from PC: ZBUFFER 0x01, STENCIL 0x02, TARGET 0xF0 (per channel). */
static HRESULT XBAPI hle_D3DDevice_Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z,
                                         DWORD Stencil)
{
    DWORD f = 0;
    if (Flags & 0xF0)
        f |= D3DCLEAR_TARGET;
    if (Flags & 0x01)
        f |= D3DCLEAR_ZBUFFER;
    if (Flags & 0x02)
        f |= D3DCLEAR_STENCIL;
    {
        SURVEY(5)
        log_printf("survey: Clear count %lu flags %02lX colour %08lX z %.3f (from %08X)\n", Count, Flags, Color,
                   (double)Z, (unsigned)(UINT_PTR)_ReturnAddress());
    }
    if (g_capture_log && g_frame == g_capture_frame)
        fprintf(g_capture_log, "clear: count %lu flags %02lX colour %08lX z %.3f%s\n", Count, Flags, Color, (double)Z,
                g_cur_rt ? " -> rt" : "");
    if (!g_dev)
        return S_OK;
    if (Count && pRects) {
        D3DRECT r[16];
        if (Count > 16)
            Count = 16;
        for (DWORD i = 0; i < Count; i++) {
            r[i].x1 = (LONG)(pRects[i].x1 * g_tsx);
            r[i].y1 = (LONG)(pRects[i].y1 * g_tsy);
            r[i].x2 = (LONG)(pRects[i].x2 * g_tsx);
            r[i].y2 = (LONG)(pRects[i].y2 * g_tsy);
        }
        return IDirect3DDevice9Ex_Clear(g_dev, Count, r, f, Color, Z, Stencil);
    }
    if (g_vp_partial) { /* the Xbox clears the viewport only (split screen: one player's area) */
        D3DRECT r = {g_vp_rect.left, g_vp_rect.top, g_vp_rect.right, g_vp_rect.bottom};
        return IDirect3DDevice9Ex_Clear(g_dev, 1, &r, f, Color, Z, Stencil);
    }
    return IDirect3DDevice9Ex_Clear(g_dev, 0, NULL, f, Color, Z, Stencil);
}

/* ------------------------------------------------------------------ vertical blank and Swap */

/* Emulated vertical blank on a fixed time grid (60 Hz, or 50 Hz without PAL60). Two threads
 * wait on it in JSRF: the game thread in Swap (presentation interval ONE) and CRI ADX's server
 * thread, which loops on BlockUntilVerticalBlank (0x13BC70) and decodes audio once per vblank.
 * Each waiter waits for the next boundary on its own; the game's vblank callback (if any) runs
 * once per elapsed vblank, under a lock, on whichever thread notices first. */
static LARGE_INTEGER g_qpf;
/* The grid: vblank n is at g_vb_start + n * g_vb_period (QPC ticks). 60 Hz from the start; the
 * frame pacing below moves it onto the display's own refresh when it can. */
static volatile double g_vb_start, g_vb_period;
static ULONG g_vblank_count;
static CRITICAL_SECTION g_vblank_lock;
static void *g_vblank_callback;
static INIT_ONCE g_vblank_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK vblank_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_vblank_lock);
    InitializeCriticalSection(&g_capture_note_lock);
    {
        LARGE_INTEGER now;
        QueryPerformanceFrequency(&g_qpf);
        QueryPerformanceCounter(&now);
        g_vb_start = (double)now.QuadPart;
        g_vb_period = (double)g_qpf.QuadPart / 60.0; /* always PAL60 (kernel/hal.c) */
    }
    return TRUE;
}

static ULONG vblank_now(void)
{
    LARGE_INTEGER now;
    InitOnceExecuteOnce(&g_vblank_once, vblank_init, NULL, NULL);
    QueryPerformanceCounter(&now);
    return (ULONG)(((double)now.QuadPart - g_vb_start) / g_vb_period);
}

static void run_vblank_callbacks(void)
{
    ULONG now = vblank_now();
    EnterCriticalSection(&g_vblank_lock);
    while (g_vblank_count < now) {
        g_vblank_count++;
        if (g_vblank_callback) {
            LONGLONG t0 = perf_now();
            ((void(__cdecl *)(void *))g_vblank_callback)(NULL);
            perf_end(PERF_VBCB, t0);
        }
    }
    LeaveCriticalSection(&g_vblank_lock);
}

/* Wait for the next vertical blank boundary. This must really sleep: CRI ADX raises its vblank
 * thread's priority, and a spinning wait (Sleep(0)) at that priority starved the normal-priority
 * threads XAudio2 needs, so CreateMasteringVoice never returned and the process could not exit.
 * Each waiting thread sleeps on its own high-resolution waitable timer until the boundary. */
static __declspec(thread) HANDLE t_vblank_timer;

/* Sleep until vertical blank number `target` on the grid (g_vb_start, g_vb_period). */
static void wait_vblank_until(ULONG target)
{
    LARGE_INTEGER now, due;
    run_vblank_callbacks();
    if (!t_vblank_timer) {
        t_vblank_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t_vblank_timer) /* older Windows: a normal timer (about 1 ms resolution with timeBeginPeriod) */
            t_vblank_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    for (;;) {
        LONGLONG boundary = (LONGLONG)ceil(g_vb_start + (double)target * g_vb_period);
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= boundary) {
            InterlockedExchangeAdd64(&g_perf_ticks[PERF_LATE], now.QuadPart - boundary);
            InterlockedIncrement(&g_perf_calls[PERF_LATE]);
            break;
        }
        /* relative due time in 100 ns units (negative) */
        due.QuadPart = -(LONGLONG)((boundary - now.QuadPart) * 10000000 / g_qpf.QuadPart);
        if (due.QuadPart == 0 || !t_vblank_timer || !SetWaitableTimer(t_vblank_timer, &due, 0, NULL, NULL, FALSE))
            Sleep(1);
        else
            WaitForSingleObject(t_vblank_timer, INFINITE);
    }
    run_vblank_callbacks();
}

static void wait_vblank(void)
{
    wait_vblank_until(vblank_now() + 1);
}

/* Frame pacing for Swap. Each frame is due at the vblank after the previous one. A frame that
 * misses its vblank by less than a frame is shown straight away and the next one is due at the
 * following vblank, as before: the cadence holds and the game still makes 60 frames a second.
 * Waiting for the next vblank instead (as BlockUntilVerticalBlank does) turned every frame that
 * was a fraction of a millisecond late into a 33 ms stutter, and on a lightly loaded PC the CPU
 * clock drops often enough to make that happen several times a minute. More than a frame late
 * (loading): start a new cadence at the next vblank. */
static ULONG g_swap_due; /* the vblank the next frame is due at; 0 = none yet */

/* Frame pacing on the display. A fixed 60 Hz grid drifts against the monitor's own refresh
 * (59.94 Hz is common), so every few seconds a 60 Hz display showed one frame twice and skipped
 * the next, and a 240 Hz one showed a frame for 3 or 5 refreshes instead of 4. When the refresh
 * is a whole multiple of 60 (within 0.5%), the grid runs at the display's rate (every k-th
 * refresh) and in its phase: each frame is due half a refresh before a vblank, far from the
 * compositor's deadline either way. The phase is nudged at most 0.2 ms a frame, and a new period
 * keeps the vblank count continuous (the game and CRI ADX count vblanks). Other refresh rates
 * (144, 165 Hz) can't divide evenly and keep the 60 Hz grid. REJET_PACING=60 forces it. */
static int g_pacing = -1; /* -1 not decided yet, 0 the 60 Hz grid, 1 on the display */
static double g_disp_period, g_disp_vblank;
static double g_pacing_err_sum;
static ULONG g_pacing_err_n;

static void set_vblank_period(double period, double now)
{
    if (period > 0 && fabs(period - g_vb_period) > 0.5) {
        double count = (now - g_vb_start) / g_vb_period; /* the same count at the new rate */
        g_vb_period = period;
        g_vb_start = now - count * period;
    }
}

static void pacing_sync(void)
{
    static int forced = -1;
    static ULONG calls;
    LARGE_INTEGER now;
    if (forced < 0) {
        char env[8] = "";
        forced = GetEnvironmentVariableA("REJET_PACING", env, sizeof env) && atoi(env) == 60;
        if (forced)
            log_printf("pacing: fixed 60 Hz (REJET_PACING=60)\n");
    }
    if (forced)
        return;
    QueryPerformanceCounter(&now);
    if (calls++ % 30 == 0) { /* the display's timing, twice a second */
        DWM_TIMING_INFO ti;
        memset(&ti, 0, sizeof ti);
        ti.cbSize = sizeof ti;
        if (SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &ti)) && ti.qpcRefreshPeriod) {
            double hz = (double)g_qpf.QuadPart / (double)ti.qpcRefreshPeriod;
            int k = (int)floor(hz / 60.0 + 0.5), lock = k >= 1 && k <= 8 && fabs(hz / k - 60.0) < 0.3;
            if (lock != g_pacing)
                log_printf(lock ? "pacing: on the display (%.3f Hz, every %d refresh%s)\n"
                                : "pacing: 60 Hz grid (display %.3f Hz isn't a multiple of 60)\n",
                           hz, k, k == 1 ? "" : "es");
            g_pacing = lock;
            g_disp_period = (double)ti.qpcRefreshPeriod;
            g_disp_vblank = (double)ti.qpcVBlank;
            set_vblank_period(lock ? k * g_disp_period : (double)g_qpf.QuadPart / 60.0, (double)now.QuadPart);
        }
    }
    if (g_pacing == 1 && g_swap_due) { /* due half a refresh before a display vblank */
        double due = g_vb_start + (double)g_swap_due * g_vb_period;
        double err = fmod(due + 0.5 * g_disp_period - g_disp_vblank, g_disp_period);
        double step = (double)g_qpf.QuadPart * 0.0002;
        if (err < 0)
            err += g_disp_period;
        if (err > 0.5 * g_disp_period)
            err -= g_disp_period;
        g_pacing_err_sum += fabs(err);
        g_pacing_err_n++;
        g_vb_start -= err > step ? step : err < -step ? -step : err;
        if (g_pacing_err_n == 3600) { /* about a minute */
            log_printf("pacing: average phase error %.3f ms\n",
                       g_pacing_err_sum / g_pacing_err_n * 1000.0 / (double)g_qpf.QuadPart);
            g_pacing_err_sum = 0;
            g_pacing_err_n = 0;
        }
    }
}

/* The graffiti warning at start-up ("Graffiti is art. However...") can't be skipped in the
 * original, unlike the logo movies around it. ReJet recognises its picture (texture content
 * name, as for HD replacements) and, when Start or A is pressed while it shows, stops waiting
 * for vblanks until it is gone: the game's own frame-counted timer runs out in a moment and the
 * game carries on as normal. */
#define WARNING_IMAGE 0xC2BC81234870958Aull
static ULONG g_warning_frame;    /* the last frame that drew the warning (0 = not yet) */
static int g_warning_state;      /* 0 waiting for it, 1 skipping it, 2 done with it */

static void warning_check_draw(void)
{
    if (g_warning_state < 2 && g_textures[0] && tex_name(g_textures[0]) == WARNING_IMAGE)
        g_warning_frame = g_frame ? g_frame : 1;
}

static int skip_pressed(void)
{
    for (DWORD i = 0; i < 4; i++) {
        XINPUT_STATE st;
        if (XInputGetState(i, &st) == ERROR_SUCCESS &&
            (st.Gamepad.wButtons & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_A)))
            return 1;
    }
    return GetForegroundWindow() == g_wnd &&
           ((GetAsyncKeyState(VK_RETURN) | GetAsyncKeyState(VK_SPACE) | GetAsyncKeyState('Z')) & 0x8000);
}

/* 1 = don't wait this frame (skipping the warning). */
static int warning_skip(void)
{
    int showing = g_warning_frame && g_frame - g_warning_frame <= 2;
    if (g_warning_state == 2)
        return 0;
    static int automatic = -1; /* REJET_SKIPWARNING=1: always, without a press (debugging) */
    if (automatic < 0)
        automatic = GetEnvironmentVariableA("REJET_SKIPWARNING", NULL, 0) > 0;
    if (g_warning_state == 0 && showing && (automatic || skip_pressed())) {
        g_warning_state = 1;
        log_printf("d3d: skipping the graffiti warning\n");
    }
    if (g_warning_state == 1 && !showing) {
        g_warning_state = 2;
        g_swap_due = 0; /* a new cadence from here */
    }
    return g_warning_state == 1;
}

static void wait_swap(void)
{
    ULONG now;
    if (warning_skip())
        return;
    pacing_sync();
    now = vblank_now();
    if (g_swap_due && now < g_swap_due) /* on time: wait for it */
        wait_vblank_until(g_swap_due);
    else if (g_swap_due && now == g_swap_due) /* late by less than a frame: show it now */
        run_vblank_callbacks();
    else { /* the first frame, or a long one: a new cadence */
        g_swap_due = now + 1;
        wait_vblank_until(g_swap_due);
    }
    g_swap_due++;
}

static void XBAPI hle_D3DDevice_BlockUntilVerticalBlank(void)
{
    wait_vblank();
}

static void XBAPI hle_D3DDevice_SetVerticalBlankCallback(void *cb)
{
    vblank_now();
    EnterCriticalSection(&g_vblank_lock);
    g_vblank_callback = cb;
    LeaveCriticalSection(&g_vblank_lock);
    log_printf("d3d: vertical blank callback %p\n", cb);
}

/* Present into the window keeping the render's aspect ratio (black bars around it). */
static void present(void)
{
    RECT c, d;
    LONG cw, ch, w, h;
    static LONG last_cw, last_ch;
    GetClientRect(g_wnd, &c);
    cw = c.right - c.left;
    ch = c.bottom - c.top;
    if (cw != last_cw || ch != last_ch) { /* resized: repaint the bars around the picture black */
        last_cw = cw;
        last_ch = ch;
        InvalidateRect(g_wnd, NULL, TRUE);
    }
    if (cw <= 0 || ch <= 0) {
        IDirect3DDevice9Ex_PresentEx(g_dev, NULL, NULL, NULL, NULL, 0);
        return;
    }
    if ((LONGLONG)cw * g_hh > (LONGLONG)ch * g_hw) {
        h = ch;
        w = (LONG)((LONGLONG)ch * g_hw / g_hh);
    }
    else {
        w = cw;
        h = (LONG)((LONGLONG)cw * g_hh / g_hw);
    }
    d.left = (cw - w) / 2;
    d.top = (ch - h) / 2;
    d.right = d.left + w;
    d.bottom = d.top + h;
    IDirect3DDevice9Ex_PresentEx(g_dev, NULL, &d, NULL, NULL, 0);
}

/* Save a 32-bit render-target surface as a BMP (ReWhacked's back-buffer saver, generalised). */
static void save_surface(IDirect3DSurface9 *bb, const char *name)
{
    IDirect3DSurface9 *sys = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    FILE *f;

    IDirect3DSurface9_GetDesc(bb, &desc);
    if (SUCCEEDED(IDirect3DDevice9Ex_CreateOffscreenPlainSurface(g_dev, desc.Width, desc.Height, desc.Format,
                                                                 D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
        SUCCEEDED(IDirect3DDevice9Ex_GetRenderTargetData(g_dev, bb, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        if ((f = fopen(name, "wb")) != NULL) {
            BITMAPFILEHEADER fh = {0};
            BITMAPINFOHEADER ih = {0};
            ih.biSize = sizeof ih;
            ih.biWidth = (LONG)desc.Width;
            ih.biHeight = -(LONG)desc.Height; /* top-down */
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + desc.Width * desc.Height * 4;
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            for (UINT y = 0; y < desc.Height; y++)
                fwrite((BYTE *)lr.pBits + y * lr.Pitch, 4, desc.Width, f);
            fclose(f);
        }
        IDirect3DSurface9_UnlockRect(sys);
    }
    if (sys)
        IDirect3DSurface9_Release(sys);
}

static void save_backbuffer(const char *name)
{
    IDirect3DSurface9 *bb = NULL;
    if (FAILED(IDirect3DDevice9Ex_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return;
    save_surface(bb, name);
    IDirect3DSurface9_Release(bb);
}

static void capture_finish(void); /* render-target section */

/* F10 / F11 / REJET_CAPTURE_AT, at the end of each frame (see g_capture_request). The capture
 * covers the frame after the request, so its draws are logged from the start. */
static void testing_hotkeys(void)
{
    static LONG marks_done;
    static long capture_at = -2;
    LONG marks = g_mark_request;
    while (marks_done < marks) {
        marks_done++;
        log_printf("==================== MARK %ld at frame %lu ====================\n", marks_done, g_frame);
        menu_toast("Marker %ld written to the log", marks_done);
        perf_mark(marks_done);
        memdump_mark(marks_done);
    }
    if (g_capture_log && g_frame == g_capture_frame) { /* the captured frame just ended */
        capture_finish();
        return;
    }
    if (capture_at == -2) {
        char env[16] = "";
        capture_at = GetEnvironmentVariableA("REJET_CAPTURE_AT", env, sizeof env) ? atol(env) : -1;
    }
    if ((InterlockedExchange(&g_capture_request, 0) || (capture_at >= 0 && g_frame + 1 == (ULONG)capture_at)) &&
        !g_capture_log) {
        static int n;
        SYSTEMTIME t;
        char path[MAX_PATH];
        GetLocalTime(&t);
        CreateDirectoryA("captures", NULL);
        snprintf(g_capture_dir, sizeof g_capture_dir, "captures\\capture_%03d_%04u%02u%02u_%02u%02u%02u", ++n, t.wYear,
                 t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        CreateDirectoryA(g_capture_dir, NULL);
        snprintf(path, sizeof path, "%s\\draws.txt", g_capture_dir);
        g_capture_log = fopen(path, "w");
        g_capture_frame = g_frame + 1;
        log_printf("==================== CAPTURE %s (frame %lu) ====================\n", g_capture_dir,
                   g_capture_frame);
        EnterCriticalSection(&g_capture_note_lock);
        if (g_capture_note[0]) { /* a bug report from the ReJet menu */
            FILE *f;
            snprintf(path, sizeof path, "%s\\note.txt", g_capture_dir);
            if ((f = fopen(path, "w")) != NULL) {
                fprintf(f, "%s\nReJet %s, frame %lu, %04u-%02u-%02u %02u:%02u:%02u\n", g_capture_note, REJET_BUILD,
                        g_capture_frame, t.wYear,
                        t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                fclose(f);
            }
            log_printf("note: %s\n", g_capture_note);
            g_capture_note[0] = 0;
        }
        LeaveCriticalSection(&g_capture_note_lock);
    }
}

/* ReJet menu: "Report a bug" captures the next frame with the player's note. */
static void request_capture(const char *note)
{
    EnterCriticalSection(&g_capture_note_lock);
    snprintf(g_capture_note, sizeof g_capture_note, "%s", note);
    LeaveCriticalSection(&g_capture_note_lock);
    InterlockedExchange(&g_capture_request, 1);
}

/* F12: Screenshots\jsrf_<date>_<time>.bmp next to the loader. REJET_SHOTS=<n> (debugging):
 * shot_NNNNN.bmp every n frames (1 = every 300). */
static void maybe_screenshot(void)
{
    static int every = -1;
    char name[MAX_PATH];
    if (!g_dev)
        return;
    if (InterlockedExchange(&g_shot_request, 0)) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        CreateDirectoryA("Screenshots", NULL);
        snprintf(name, sizeof name, "Screenshots\\jsrf_%04u%02u%02u_%02u%02u%02u_%03u.bmp", t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        save_backbuffer(name);
        log_printf("d3d: screenshot %s\n", name);
        menu_toast("Screenshot saved");
    }
    if (every < 0) {
        char env[8] = "";
        every = GetEnvironmentVariableA("REJET_SHOTS", env, sizeof env) ? atoi(env) : 0;
        if (every == 1)
            every = 300;
    }
    if (every > 0 && g_frame % (ULONG)every == 0) {
        snprintf(name, sizeof name, "shot_%05lu.bmp", g_frame);
        save_backbuffer(name);
    }
}

/* Swap(Flags): 0 = present at the next vblank. The engine also uses 2 (present and keep the
 * old front buffer readable, for its screenshot path 0x14DBC0) and 4 (0x14DB80). */
static DWORD XBAPI hle_D3DDevice_Swap(DWORD Flags)
{
    LONGLONG t0;
    perf_frame(g_frame, g_frame_draws);
    wait_swap(); /* presentation interval ONE */
    perf_wait_done();
    t0 = perf_now();
    if (g_dev && g_in_scene) {
        IDirect3DDevice9Ex_EndScene(g_dev);
        g_in_scene = 0;
    }
    msaa_resolve(); /* everything below reads the device's back buffer */
    {
        static int overlay = -1; /* REJET_SHOTSOVERLAY=1: screenshots include ReJet's overlay (HUD, messages) */
        if (overlay < 0) {
            char env[4] = "";
            overlay = GetEnvironmentVariableA("REJET_SHOTSOVERLAY", env, sizeof env) && env[0] == '1';
        }
        if (!overlay)
            maybe_screenshot();
        testing_hotkeys(); /* before Present: the back buffer still holds this frame */
        if (g_dev) {
            menu_frame(g_frame_draws); /* splash, messages, FPS; runs the menu (game paused) when opened */
            if (overlay)
                maybe_screenshot();
        }
    }
    if (g_dev) {
        g_frame_draws = 0;
        present();
    }
    perf_end(PERF_PRESENT, t0);
    tex_new_frame();
    g_frame++;
    if (g_frame <= 3 || g_frame % 600 == 0) {
        log_printf("d3d: frame %lu at vblank %lu (Swap flags %lX), draws %lu (untextured %lu), skipped %lu\n", g_frame,
                   vblank_now(), Flags, g_draws, g_untextured, g_skipped);
        g_draws = g_skipped = g_untextured = 0;
    }
    return g_frame;
}

/* ------------------------------------------------------------------ resources */

static unsigned bits_per_pixel(uint32_t xfmt)
{
    switch (xfmt) {
    case XD3DFMT_L8: case XD3DFMT_AL8: case XD3DFMT_P8: case XD3DFMT_LIN_L8: case XD3DFMT_A8:
    case XD3DFMT_LIN_AL8: case XD3DFMT_LIN_A8:
        return 8;
    case XD3DFMT_DXT1:
        return 4;
    case XD3DFMT_DXT2: case XD3DFMT_DXT4:
        return 8;
    case XD3DFMT_A8R8G8B8: case XD3DFMT_X8R8G8B8: case XD3DFMT_LIN_A8R8G8B8: case XD3DFMT_LIN_X8R8G8B8:
    case XD3DFMT_D24S8: case XD3DFMT_F24S8: case XD3DFMT_LIN_D24S8:
        return 32;
    default:
        return 16;
    }
}

static int is_linear_format(uint32_t f)
{
    return f == XD3DFMT_LIN_A8R8G8B8 || f == XD3DFMT_LIN_X8R8G8B8 || f == XD3DFMT_LIN_R5G6B5 ||
           f == XD3DFMT_LIN_A1R5G5B5 || f == XD3DFMT_LIN_A4R4G4B4 || f == XD3DFMT_LIN_L8 || f == XD3DFMT_LIN_A8 ||
           f == XD3DFMT_LIN_AL8 || f == XD3DFMT_LIN_A8L8 || f == XD3DFMT_LIN_X1R5G5B5 ||
           f == XD3DFMT_LIN_D24S8 || f == XD3DFMT_LIN_D16;
}

static int is_dxt(uint32_t xfmt)
{
    return xfmt == XD3DFMT_DXT1 || xfmt == XD3DFMT_DXT2 || xfmt == XD3DFMT_DXT4;
}

/* Bytes of one mip level (DXT: 4x4 blocks). */
static unsigned level_size(uint32_t xfmt, unsigned w, unsigned h, unsigned d)
{
    if (is_dxt(xfmt)) {
        unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
        return bw * bh * (xfmt == XD3DFMT_DXT1 ? 8 : 16) * d;
    }
    return w * h * d * bits_per_pixel(xfmt) / 8;
}

static unsigned log2u(unsigned v)
{
    unsigned n = 0;
    while ((1u << n) < v)
        n++;
    return n;
}

static void *alloc_data(SIZE_T bytes)
{
    return VirtualAlloc(NULL, bytes ? bytes : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

/* An Xbox-layout texture (2D, volume or cube) with its data in game-visible memory. */
static XD3DPixelContainer *make_texture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Format, int volume,
                                        int cube)
{
    XD3DPixelContainer *t = (XD3DPixelContainer *)calloc(1, sizeof *t);
    unsigned levels = Levels ? Levels : log2u(Width > Height ? Width : Height) + 1, total = 0;
    unsigned faces = cube ? 6 : 1;
    unsigned w = Width, h = Height, d = Depth ? Depth : 1;

    for (unsigned l = 0; l < levels; l++) {
        total += level_size(Format, w, h, d);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        d = d > 1 ? d / 2 : 1;
    }
    total *= faces;
    if (is_linear_format(Format)) /* rows are padded to a 64-byte pitch */
        total = ((Width * bits_per_pixel(Format) / 8 + 63) & ~63u) * Height;
    t->Resource.Common = 1 | (XD3DCOMMON_TYPE_TEXTURE << XD3DCOMMON_TYPE_SHIFT) | XD3DCOMMON_D3DCREATED;
    t->Resource.Data = (uint32_t)(UINT_PTR)alloc_data(total);
    t->Format = (Format << 8) | ((volume ? 3u : 2u) << 4) | (levels << 16) | (log2u(Width) << 20) |
                (log2u(Height) << 24) | (log2u(Depth ? Depth : 1) << 28) | (cube ? XFMT_CUBEMAP : 0);
    if (is_linear_format(Format)) {
        unsigned pitch = (Width * bits_per_pixel(Format) / 8 + 63) & ~63u;
        t->Format &= ~0xFFF00000u;
        t->Size = (Width - 1) | ((Height - 1) << 12) | ((pitch / 64 - 1) << 24);
    }
    {
        SURVEY(40)
        log_printf("survey: texture %ux%ux%u levels %u fmt %02lX%s -> %p data %08X bytes %u (from %08X)\n", Width,
                   Height, Depth, Levels, Format, volume ? " volume" : cube ? " cube" : "", (void *)t, t->Resource.Data,
                   total, (unsigned)(UINT_PTR)_ReturnAddress());
    }
    return t;
}

static HRESULT XBAPI hle_D3DDevice_CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, DWORD Format,
                                                 DWORD Pool, XD3DPixelContainer **ppTexture)
{
    (void)Usage;
    (void)Pool;
    *ppTexture = make_texture(Width, Height, 1, Levels, Format, 0, 0);
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                                       DWORD Format, DWORD Pool, XD3DPixelContainer **ppVolumeTexture)
{
    (void)Usage;
    (void)Pool;
    *ppVolumeTexture = make_texture(Width, Height, Depth, Levels, Format, 1, 0);
    return S_OK;
}

/* Byte offset of mip level `level` (and cube face) inside a texture's data. */
static unsigned level_offset(const XD3DPixelContainer *t, unsigned face, unsigned level, unsigned *w_out,
                             unsigned *h_out)
{
    uint32_t f = XFMT_FORMAT(t->Format);
    unsigned w = t->Size ? XSIZE_WIDTH(t->Size) : 1u << XFMT_USIZE(t->Format);
    unsigned h = t->Size ? XSIZE_HEIGHT(t->Size) : 1u << XFMT_VSIZE(t->Format);
    unsigned levels = XFMT_MIPMAPS(t->Format) ? XFMT_MIPMAPS(t->Format) : 1, face_size = 0, off = 0;
    unsigned lw = w, lh = h;
    for (unsigned l = 0; l < levels; l++) {
        face_size += level_size(f, lw, lh, 1);
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    off = face * face_size;
    for (unsigned l = 0; l < level; l++) {
        off += level_size(f, w, h, 1);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    *w_out = w;
    *h_out = h;
    return off;
}

static XD3DSurface *make_surface(XD3DPixelContainer *parent, unsigned face, unsigned level)
{
    XD3DSurface *s = (XD3DSurface *)calloc(1, sizeof *s);
    unsigned w, h, off = level_offset(parent, face, level, &w, &h);
    s->Container.Resource.Common = 1 | (XD3DCOMMON_TYPE_SURFACE << XD3DCOMMON_TYPE_SHIFT) | XD3DCOMMON_D3DCREATED;
    s->Container.Resource.Data = parent->Resource.Data + off;
    s->Container.Format = (parent->Format & 0x0000FFFFu & ~XFMT_CUBEMAP) | (log2u(w) << 20) | (log2u(h) << 24);
    s->Container.Size = parent->Size;
    s->Parent = parent;
    parent->Resource.Common++;
    return s;
}

static HRESULT XBAPI hle_D3DTexture_GetSurfaceLevel(XD3DPixelContainer *Texture, UINT Level, XD3DSurface **ppSurface)
{
    *ppSurface = make_surface(Texture, 0, Level);
    return S_OK;
}

/* CreateImageSurface (0x18D710 jumps to 0x18F2C0, ret 16). */
static HRESULT XBAPI hle_D3DDevice_CreateImageSurface(UINT Width, UINT Height, DWORD Format, XD3DSurface **ppSurface)
{
    XD3DPixelContainer *t = make_texture(Width, Height, 1, 1, Format, 0, 0);
    *ppSurface = make_surface(t, 0, 0);
    return S_OK;
}

typedef struct XLockedRect {
    INT Pitch;
    void *pBits;
} XLockedRect;

static HRESULT lock_surface(XD3DPixelContainer *c, XLockedRect *pLockedRect, const RECT *pRect)
{
    uint32_t f = XFMT_FORMAT(c->Format);
    unsigned w = c->Size ? XSIZE_WIDTH(c->Size) : 1u << XFMT_USIZE(c->Format);
    unsigned pitch = c->Size ? XSIZE_PITCH(c->Size) : (is_dxt(f) ? (w / 4) * (f == XD3DFMT_DXT1 ? 8 : 16)
                                                                   : w * bits_per_pixel(f) / 8);
    unsigned char *bits = (unsigned char *)(UINT_PTR)c->Resource.Data;
    if (!bits) { /* back buffer / depth: no CPU copy yet */
        static unsigned char scratch[2048 * 2048 * 4];
        TODO_ONCE("LockRect on a device surface (returns scratch memory)");
        bits = scratch;
    }
    if (pRect && !is_dxt(f))
        bits += pRect->top * pitch + pRect->left * bits_per_pixel(f) / 8;
    pLockedRect->Pitch = (INT)pitch;
    pLockedRect->pBits = bits;
    return S_OK;
}

static HRESULT XBAPI hle_D3DSurface_LockRect(XD3DSurface *Surface, XLockedRect *pLockedRect, const RECT *pRect,
                                             DWORD Flags)
{
    (void)Flags;
    return lock_surface(&Surface->Container, pLockedRect, pRect);
}

static HRESULT XBAPI hle_D3DTexture_LockRect(XD3DPixelContainer *Texture, UINT Level, XLockedRect *pLockedRect,
                                             const RECT *pRect, DWORD Flags)
{
    XD3DPixelContainer level;
    unsigned w, h, off = level_offset(Texture, 0, Level, &w, &h);
    (void)Flags;
    level = *Texture;
    level.Resource.Data += off;
    level.Format = (Texture->Format & 0x000FFFFFu) | (log2u(w) << 20) | (log2u(h) << 24);
    return lock_surface(&level, pLockedRect, pRect);
}

typedef struct XLockedBox {
    INT RowPitch;
    INT SlicePitch;
    void *pBits;
} XLockedBox;

/* Volume textures: levels are stored one after another, each a stack of depth slices.
 * (0x18F120 jumps to 0x193DA0, ret 20.) */
static HRESULT XBAPI hle_D3DVolumeTexture_LockBox(XD3DPixelContainer *Volume, UINT Level, XLockedBox *pLockedVolume,
                                                  const void *pBox, DWORD Flags)
{
    uint32_t f = XFMT_FORMAT(Volume->Format);
    unsigned w = 1u << XFMT_USIZE(Volume->Format), h = 1u << XFMT_VSIZE(Volume->Format);
    unsigned d = 1u << XFMT_PSIZE(Volume->Format), off = 0;
    (void)Flags;
    if (pBox)
        TODO_ONCE("LockBox with a sub-box (whole level returned)");
    for (UINT l = 0; l < Level; l++) {
        off += level_size(f, w, h, d);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        d = d > 1 ? d / 2 : 1;
    }
    if (is_dxt(f)) {
        pLockedVolume->RowPitch = (INT)(((w + 3) / 4) * (f == XD3DFMT_DXT1 ? 8 : 16));
        pLockedVolume->SlicePitch = pLockedVolume->RowPitch * (INT)((h + 3) / 4);
    }
    else {
        pLockedVolume->RowPitch = (INT)(w * bits_per_pixel(f) / 8);
        pLockedVolume->SlicePitch = pLockedVolume->RowPitch * (INT)h;
    }
    pLockedVolume->pBits = (BYTE *)(UINT_PTR)Volume->Resource.Data + off;
    return S_OK;
}

static XD3DResource *make_buffer(UINT Length, unsigned type)
{
    XD3DResource *b = (XD3DResource *)calloc(1, sizeof *b);
    b->Common = 1 | (type << XD3DCOMMON_TYPE_SHIFT) | XD3DCOMMON_D3DCREATED;
    b->Data = (uint32_t)(UINT_PTR)alloc_data(Length);
    return b;
}

static HRESULT XBAPI hle_D3DDevice_CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, DWORD Pool,
                                                      XD3DResource **ppVertexBuffer)
{
    (void)Usage;
    (void)FVF;
    (void)Pool;
    *ppVertexBuffer = make_buffer(Length, XD3DCOMMON_TYPE_VERTEXBUFFER);
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_CreateIndexBuffer(UINT Length, DWORD Usage, DWORD Format, DWORD Pool,
                                                     XD3DResource **ppIndexBuffer)
{
    (void)Usage;
    (void)Format;
    (void)Pool;
    *ppIndexBuffer = make_buffer(Length, XD3DCOMMON_TYPE_INDEXBUFFER);
    return S_OK;
}

static HRESULT XBAPI hle_D3DVertexBuffer_Lock(XD3DResource *VertexBuffer, UINT OffsetToLock, UINT SizeToLock,
                                              BYTE **ppbData, DWORD Flags)
{
    (void)SizeToLock;
    (void)Flags;
    *ppbData = (BYTE *)(UINT_PTR)VertexBuffer->Data + OffsetToLock;
    return S_OK;
}

static ULONG XBAPI hle_D3DResource_AddRef(XD3DResource *Resource)
{
    return (++Resource->Common) & XD3DCOMMON_REFCOUNT_MASK;
}

static ULONG XBAPI hle_D3DResource_Release(XD3DResource *Resource)
{
    ULONG refs = Resource->Common & XD3DCOMMON_REFCOUNT_MASK;
    if (refs)
        Resource->Common--;
    /* Freeing comes with host-object tracking; leaking is safe for bring-up. */
    return refs ? refs - 1 : 0;
}

/* Xbox D3DSURFACE_DESC (7 dwords; the engine reads Format, Width and Height, 0x14DBC0). */
typedef struct XD3DSURFACE_DESC {
    DWORD Format, Type, Usage, Size, MultiSampleType, Width, Height;
} XD3DSURFACE_DESC;

/* Get2DSurfaceDesc(container, level, desc): the original (0x193A10) reads device fields for
 * device-owned surfaces (back buffer, depth), so it is replaced; the description comes from the
 * container's own Format and Size dwords. */
static void XBAPI hle_Get2DSurfaceDesc(const XD3DPixelContainer *c, DWORD Level, XD3DSURFACE_DESC *d)
{
    uint32_t f = XFMT_FORMAT(c->Format);
    unsigned w = c->Size ? XSIZE_WIDTH(c->Size) : 1u << XFMT_USIZE(c->Format);
    unsigned h = c->Size ? XSIZE_HEIGHT(c->Size) : 1u << XFMT_VSIZE(c->Format);
    for (DWORD l = 0; l < Level; l++) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    d->Format = f;
    d->Type = 1; /* D3DRTYPE_SURFACE */
    d->Usage = c == &g_backbuffer.Container ? 1u /* RENDERTARGET */ : c == &g_depth.Container ? 2u /* DEPTHSTENCIL */ : 0u;
    d->Size = c->Size ? XSIZE_PITCH(c->Size) * h : level_size(f, w, h, 1);
    d->MultiSampleType = 0x0011; /* D3DMULTISAMPLE_NONE on the Xbox */
    d->Width = w;
    d->Height = h;
}

static HRESULT XBAPI hle_D3DSurface_GetDesc(const XD3DSurface *Surface, XD3DSURFACE_DESC *pDesc)
{
    hle_Get2DSurfaceDesc(&Surface->Container, 0, pDesc);
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_GetBackBuffer(INT BackBuffer, DWORD Type, XD3DSurface **ppBackBuffer)
{
    (void)BackBuffer;
    (void)Type;
    g_backbuffer.Container.Resource.Common++;
    *ppBackBuffer = &g_backbuffer;
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_GetDepthStencilSurface(XD3DSurface **ppZStencilSurface)
{
    g_depth.Container.Resource.Common++;
    *ppZStencilSurface = &g_depth;
    return S_OK;
}

/* ------------------------------------------------------------------ shaders */

#define MAX_SHADERS 4096
static const DWORD *g_vs_decl[MAX_SHADERS], *g_vs_func[MAX_SHADERS];
static unsigned g_vs_decl_n[MAX_SHADERS], g_vs_func_n[MAX_SHADERS]; /* dwords, for de-duplication */
static IDirect3DVertexShader9 *g_vs_host[MAX_SHADERS];
static IDirect3DVertexDeclaration9 *g_vs_vdecl[MAX_SHADERS];
static BYTE g_vs_state[MAX_SHADERS]; /* 0 untried, 1 ready, 2 failed */
static short g_vs_t0_off[MAX_SHADERS]; /* byte offset of the input copied to texcoord 0, or -1 */
static BYTE g_vs_2d[MAX_SHADERS];      /* 2D program (screen positions, no viewport constants) */
static unsigned g_vs_count, g_ps_count;

/* Translate and compile program i (once), when the game creates it, so play never waits on the
 * compiler (ReWhacked); prepare_draw retries if needed. */
static void ensure_vs(unsigned i)
{
    static int logged;
    LONGLONG t0;
    if (g_vs_state[i] != 0 || !g_dev || !g_vs_func[i])
        return;
    if (!logged++) { /* show one translation in full */
        static char hlsl[64 * 1024];
        if (vsh_to_hlsl(g_vs_func[i], g_vs_decl[i], hlsl, sizeof hlsl))
            log_printf("vsh: program %u as HLSL:\n%s\n", i, hlsl);
    }
    t0 = perf_now();
    g_vs_host[i] = vsh_compile((IDirect3DDevice9 *)g_dev, g_vs_func[i], g_vs_decl[i], i);
    g_vs_2d[i] = (BYTE)vsh_last_is_2d();
    {
        int reg = vsh_last_t0_input();
        g_vs_t0_off[i] = (short)(reg >= 0 ? vsh_input_offset(g_vs_decl[i], (unsigned)reg) : -1);
    }
    perf_end(PERF_VSC, t0);
    g_vs_vdecl[i] = vsh_declaration((IDirect3DDevice9 *)g_dev, g_vs_decl[i], NULL);
    g_vs_state[i] = (g_vs_host[i] && g_vs_vdecl[i]) ? 1 : 2;
    if (g_vs_state[i] == 2)
        log_printf("vsh: program %u failed (%s)\n", i, g_vs_host[i] ? "declaration" : "translation or compile");
}

/* Handles have bit 0 set so they can't be mistaken for FVF codes. The engine passes buffers it
 * reuses (copied here, as the XDK does); identical programs return the existing handle. */
static HRESULT XBAPI hle_D3DDevice_CreateVertexShader(const DWORD *pDeclaration, const DWORD *pFunction,
                                                      DWORD *pHandle, DWORD Usage)
{
    unsigned decl_n = 0, func_n = pFunction ? 1 + 4 * (pFunction[0] >> 16) : 0;
    DWORD *d, *f = NULL;
    (void)Usage;
    if (g_vs_count >= MAX_SHADERS)
        return E_OUTOFMEMORY;
    {
        /* REJET_DUMPVS=1: write each program (header + microcode) and its declaration. */
        char env[4] = "";
        if (pFunction && GetEnvironmentVariableA("REJET_DUMPVS", env, sizeof env) && env[0] == '1') {
            char path[64];
            FILE *fp;
            unsigned n = 0;
            CreateDirectoryA("vsh", NULL);
            snprintf(path, sizeof path, "vsh\\%03u.bin", g_vs_count);
            if ((fp = fopen(path, "wb")) != NULL) {
                fwrite(pFunction, 4, func_n, fp);
                fclose(fp);
            }
            while (pDeclaration && pDeclaration[n] != 0xFFFFFFFF && n < 64)
                n++;
            snprintf(path, sizeof path, "vsh\\%03u.decl", g_vs_count);
            if ((fp = fopen(path, "wb")) != NULL) {
                fwrite(pDeclaration, 4, n, fp);
                fclose(fp);
            }
        }
    }
    while (pDeclaration && pDeclaration[decl_n] != 0xFFFFFFFF && decl_n < 256)
        decl_n++;
    for (unsigned i = 0; i < g_vs_count; i++) {
        if (g_vs_decl_n[i] == decl_n && g_vs_func_n[i] == func_n &&
            (!decl_n || !memcmp(g_vs_decl[i], pDeclaration, decl_n * sizeof(DWORD))) &&
            (!func_n || !memcmp(g_vs_func[i], pFunction, func_n * sizeof(DWORD)))) {
            *pHandle = (i << 1) | 1;
            return S_OK;
        }
    }
    g_vs_decl_n[g_vs_count] = decl_n;
    g_vs_func_n[g_vs_count] = func_n;
    d = (DWORD *)malloc((decl_n + 1) * sizeof *d);
    if (decl_n)
        memcpy(d, pDeclaration, decl_n * sizeof *d);
    d[decl_n] = 0xFFFFFFFF;
    if (func_n) {
        f = (DWORD *)malloc(func_n * sizeof *f);
        memcpy(f, pFunction, func_n * sizeof *f);
    }
    g_vs_decl[g_vs_count] = d;
    g_vs_func[g_vs_count] = f;
    *pHandle = (g_vs_count << 1) | 1;
    ensure_vs(g_vs_count++);
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_DeleteVertexShader(DWORD Handle)
{
    (void)Handle;
    return S_OK;
}

static DWORD g_cur_vs;

static HRESULT XBAPI hle_D3DDevice_SetVertexShader(DWORD Handle)
{
    g_cur_vs = Handle;
    return S_OK;
}

/* Pixel shaders: an Xbox D3DPIXELSHADERDEF (register-combiner program, 60 dwords) copied at
 * creation; handle = (index << 1) | 1. */
#define PS_DEF_DWORDS 60
#define MAX_PS 256
static DWORD g_ps_def[MAX_PS][PS_DEF_DWORDS];
/* Compiled variants per shader, keyed by what is bound to each texture stage (PSH_BOUND_*). */
#define PS_VARIANTS 16
typedef struct { unsigned key; BYTE state; IDirect3DPixelShader9 *host; } PsVariant; /* state 0/1 ok/2 failed */
static PsVariant g_ps_var[MAX_PS][PS_VARIANTS];
static DWORD g_cur_ps;
static float g_psconst[16][4];

static HRESULT XBAPI hle_D3DDevice_CreatePixelShader(const void *pDef, DWORD *pHandle)
{
    unsigned i = g_ps_count;
    for (unsigned k = 0; k < g_ps_count; k++) { /* identical definition: reuse */
        if (!memcmp(g_ps_def[k], pDef, sizeof g_ps_def[k])) {
            *pHandle = (k << 1) | 1;
            return S_OK;
        }
    }
    if (i >= MAX_PS)
        return E_OUTOFMEMORY;
    memcpy(g_ps_def[i], pDef, sizeof g_ps_def[i]);
    g_ps_count++;
    *pHandle = (i << 1) | 1;
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_DeletePixelShader(DWORD Handle)
{
    (void)Handle;
    return S_OK;
}

/* The active pixel shader's per-stage constants, kept as the Xbox does (see psh_set_constant):
 * a stage mapped to a register the game never sets keeps the definition's value. Sprayed
 * graffiti depends on this: its combiner reads register 2, which the game never sets, and
 * zero there made every tag fully transparent. */
static float g_ps_live[18][4];

static HRESULT XBAPI hle_D3DDevice_SetPixelShader(DWORD Handle)
{
    g_cur_ps = Handle;
    if ((Handle & 1) && (Handle >> 1) < g_ps_count)
        psh_constants(g_ps_def[Handle >> 1], NULL, g_ps_live);
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_SetPixelShaderConstant(DWORD Register, const void *pData, DWORD Count)
{
    if (Register + Count <= 16) {
        memcpy(g_psconst[Register], pData, Count * sizeof g_psconst[0]);
        if ((g_cur_ps & 1) && (g_cur_ps >> 1) < g_ps_count)
            for (DWORD k = 0; k < Count; k++)
                psh_set_constant(g_ps_def[g_cur_ps >> 1], Register + k, g_psconst[Register + k], g_ps_live);
    }
    if (g_capture_log && g_frame == g_capture_frame) {
        const float *f = (const float *)pData;
        fprintf(g_capture_log, "SetPixelShaderConstant(%lu, %lu):", Register, Count);
        for (DWORD k = 0; k < Count * 4 && k < 32; k++)
            fprintf(g_capture_log, " %.3f", f[k]);
        fprintf(g_capture_log, "\n");
    }
    return S_OK;
}

/* XDK 4134: SetVertexShaderConstant(Register, pData, Count) takes D3D register numbers
 * (-96..95) and Count in registers, and adds 0x60 itself (0x1910C4): c0 is NV2A register 96. */
static HRESULT XBAPI hle_D3DDevice_SetVertexShaderConstant(INT Register, const void *pData, DWORD Count)
{
    INT first = Register + 96;
    if (first < 0 || first + (INT)Count > 192) {
        log_printf("d3d: vertex constant range %d+%lu out of bounds\n", Register, Count);
        return S_OK;
    }
    memcpy(g_vconst[first], pData, Count * sizeof g_vconst[0]);
    return S_OK;
}

/* Mode bit 0x10 = no reserved constants; the rest selects 96/192 constants. Logged for now:
 * the translator assumes the default (96 constants, c-38/c-37 reserved by the viewport). */
static HRESULT XBAPI hle_D3DDevice_SetShaderConstantMode(DWORD Mode)
{
    static DWORD last = 0xFFFFFFFF;
    if (Mode != last)
        log_printf("d3d: SetShaderConstantMode %lX\n", Mode);
    last = Mode;
    return S_OK;
}

/* ------------------------------------------------------------------ render state */

/* ---- NV2A / OpenGL enums (what Xbox D3D passes) -> D3D9 (ReWhacked) ---- */

static DWORD gl_cmp(DWORD v) /* GL_NEVER 0x200 .. GL_ALWAYS 0x207 -> D3DCMP_NEVER 1 .. ALWAYS 8 */
{
    return (v >= 0x200 && v <= 0x207) ? v - 0x200 + 1 : D3DCMP_ALWAYS;
}

static DWORD gl_blend(DWORD v)
{
    switch (v) {
    case 0x0000: return D3DBLEND_ZERO;
    case 0x0001: return D3DBLEND_ONE;
    case 0x0300: return D3DBLEND_SRCCOLOR;
    case 0x0301: return D3DBLEND_INVSRCCOLOR;
    case 0x0302: return D3DBLEND_SRCALPHA;
    case 0x0303: return D3DBLEND_INVSRCALPHA;
    case 0x0304: return D3DBLEND_DESTALPHA;
    case 0x0305: return D3DBLEND_INVDESTALPHA;
    case 0x0306: return D3DBLEND_DESTCOLOR;
    case 0x0307: return D3DBLEND_INVDESTCOLOR;
    case 0x0308: return D3DBLEND_SRCALPHASAT;
    case 0x8001: case 0x8003: return D3DBLEND_BLENDFACTOR; /* constant colour / alpha */
    case 0x8002: case 0x8004: return D3DBLEND_INVBLENDFACTOR;
    default: return D3DBLEND_ONE;
    }
}

static DWORD gl_blendop(DWORD v)
{
    switch (v) {
    case 0x800A: return D3DBLENDOP_SUBTRACT;
    case 0x800B: return D3DBLENDOP_REVSUBTRACT;
    case 0x8007: return D3DBLENDOP_MIN;
    case 0x8008: return D3DBLENDOP_MAX;
    default: return D3DBLENDOP_ADD; /* 0x8006 */
    }
}

static DWORD gl_stencilop(DWORD v)
{
    switch (v) {
    case 0x0000: return D3DSTENCILOP_ZERO;
    case 0x1E01: return D3DSTENCILOP_REPLACE;
    case 0x1E02: return D3DSTENCILOP_INCRSAT;
    case 0x1E03: return D3DSTENCILOP_DECRSAT;
    case 0x150A: return D3DSTENCILOP_INVERT;
    case 0x8507: return D3DSTENCILOP_INCR;
    case 0x8508: return D3DSTENCILOP_DECR;
    default: return D3DSTENCILOP_KEEP; /* 0x1E00 */
    }
}

static DWORD g_rs[256]; /* what rs() last set, for the draw log */

static BYTE g_rs_set[256]; /* states rs() has set: put back after a device reset */

static void rs(D3DRENDERSTATETYPE s, DWORD v)
{
    if ((unsigned)s < 256) {
        g_rs[s] = v;
        g_rs_set[s] = 1;
    }
    if (g_dev)
        IDirect3DDevice9Ex_SetRenderState(g_dev, s, v);
}

/* SetRenderState_Simple(ecx = NV2A push-buffer method, edx = value). XDK 4134 passes the whole
 * method word, count bits included (0x0004035C = 1 dword to 0x35C), so only the low bits count. */
static void FASTCALL hle_D3DDevice_SetRenderState_Simple(DWORD Method, DWORD Value)
{
    switch (Method & 0x1FFC) {
    case 0x0300: rs(D3DRS_ALPHATESTENABLE, Value != 0); break;
    case 0x0304: rs(D3DRS_ALPHABLENDENABLE, Value != 0); break;
    case 0x030C: rs(D3DRS_ZENABLE, Value ? D3DZB_TRUE : D3DZB_FALSE); break;
    case 0x0310: rs(D3DRS_DITHERENABLE, Value != 0); break;
    case 0x0314: rs(D3DRS_LIGHTING, Value != 0); break;
    case 0x032C: rs(D3DRS_STENCILENABLE, Value != 0); break;
    case 0x033C: rs(D3DRS_ALPHAFUNC, gl_cmp(Value)); break;
    case 0x0340: rs(D3DRS_ALPHAREF, Value & 0xFF); break;
    case 0x0344: rs(D3DRS_SRCBLEND, gl_blend(Value)); break;
    case 0x0348: rs(D3DRS_DESTBLEND, gl_blend(Value)); break;
    case 0x034C: rs(D3DRS_BLENDFACTOR, Value); break;
    case 0x0350: rs(D3DRS_BLENDOP, gl_blendop(Value)); break;
    case 0x0354: rs(D3DRS_ZFUNC, gl_cmp(Value)); break;
    case 0x0358: /* NV2A colour mask: A 0x01000000, R 0x00010000, G 0x00000100, B 0x00000001 */
        rs(D3DRS_COLORWRITEENABLE, ((Value & 0x00010000) ? D3DCOLORWRITEENABLE_RED : 0) |
                                       ((Value & 0x00000100) ? D3DCOLORWRITEENABLE_GREEN : 0) |
                                       ((Value & 0x00000001) ? D3DCOLORWRITEENABLE_BLUE : 0) |
                                       ((Value & 0x01000000) ? D3DCOLORWRITEENABLE_ALPHA : 0));
        break;
    case 0x035C: rs(D3DRS_ZWRITEENABLE, Value != 0); break;
    case 0x0360: rs(D3DRS_STENCILWRITEMASK, Value); break;
    case 0x0364: rs(D3DRS_STENCILFUNC, gl_cmp(Value)); break;
    case 0x0368: rs(D3DRS_STENCILREF, Value); break;
    case 0x036C: rs(D3DRS_STENCILMASK, Value); break;
    case 0x0370: rs(D3DRS_STENCILFAIL, gl_stencilop(Value)); break;
    case 0x0374: rs(D3DRS_STENCILZFAIL, gl_stencilop(Value)); break;
    case 0x0378: rs(D3DRS_STENCILPASS, gl_stencilop(Value)); break;
    case 0x037C: rs(D3DRS_SHADEMODE, Value == 0x1D00 ? D3DSHADE_FLAT : D3DSHADE_GOURAUD); break;
    default: {
        static DWORD seen[64];
        static unsigned n;
        unsigned i;
        for (i = 0; i < n && seen[i] != (Method & 0x1FFC); i++)
            ;
        if (i == n && n < 64) {
            seen[n++] = Method & 0x1FFC;
            log_printf("d3d: render-state method %04lX not mapped (value %08lX)\n", Method & 0x1FFC, Value);
        }
    }
    }
}

/* Culling: Xbox D3D's own D3DCULL values (NONE 0, CW 0x900, CCW 0x901). */
static DWORD g_x_cull, g_x_front = 0x900; /* Xbox CullMode and FrontFace (default D3DFRONT_CW) */

static void XBAPI hle_D3DDevice_SetRenderState_CullMode(DWORD v)
{
    g_x_cull = v;
    rs(D3DRS_CULLMODE, v == 0x900 ? D3DCULL_CW : v == 0x901 ? D3DCULL_CCW : D3DCULL_NONE);
}

/* FrontFace: only recorded for now (see the draw log); D3D9 has no separate winding state. */
static void XBAPI hle_D3DDevice_SetRenderState_FrontFace(DWORD v)
{
    static DWORD last = 0xFFFFFFFF;
    if (v != last)
        log_printf("d3d: FrontFace %lX (frame %lu, from %08X)\n", v, g_frame, (unsigned)(UINT_PTR)_ReturnAddress());
    last = v;
    g_x_front = v;
}

static void XBAPI hle_D3DDevice_SetRenderState_ZEnable(DWORD v)
{
    rs(D3DRS_ZENABLE, v ? D3DZB_TRUE : D3DZB_FALSE);
}

static void XBAPI hle_D3DDevice_SetRenderState_FillMode(DWORD v)
{
    rs(D3DRS_FILLMODE, v == 0x1B00 ? D3DFILL_POINT : v == 0x1B01 ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
}

static void XBAPI hle_D3DDevice_SetRenderState_StencilEnable(DWORD v)
{
    rs(D3DRS_STENCILENABLE, v != 0);
}

static void XBAPI hle_D3DDevice_SetRenderState_StencilFail(DWORD v)
{
    rs(D3DRS_STENCILFAIL, gl_stencilop(v));
}

static DWORD g_x_tfactor = 0xFFFFFFFF; /* the game's texture factor (apply_lazy_states borrows it) */

static void XBAPI hle_D3DDevice_SetRenderState_TextureFactor(DWORD v)
{
    g_x_tfactor = v;
    rs(D3DRS_TEXTUREFACTOR, v);
}

static void XBAPI hle_D3DDevice_SetRenderState_FogColor(DWORD v)
{
    rs(D3DRS_FOGCOLOR, v);
}

static void XBAPI hle_D3DDevice_SetRenderState_NormalizeNormals(DWORD v)
{
    rs(D3DRS_NORMALIZENORMALS, v != 0);
}

/* ZBias (0-16, higher = nearer) pulls coplanar decals in front of the surface under them:
 * sprayed graffiti on walls. D3D9 has a depth bias added to z instead; the scale is the usual
 * D3D8 to D3D9 one (as d3d8to9). */
static void XBAPI hle_D3DDevice_SetRenderState_ZBias(DWORD v)
{
    float bias = (float)v * -0.000005f;
    SURVEY(8) log_printf("d3d: ZBias %lu\n", v);
    rs(D3DRS_DEPTHBIAS, *(DWORD *)&bias);
}

/* Xbox texture-stage state: D3D keeps TextureState[stage][32] at 0x19E9A0 and the engine writes
 * it inline (dirty flags at 0x19E998), as in ReWhacked (4831 had it at 0x2529C0). Indices, as
 * in 4831 (TexCoordIndex 28 at 0x19EA10 and ColorKeyColor 30 at 0x19EA18 confirm the layout):
 * 0 ADDRESSU 1 ADDRESSV 2 ADDRESSW 3 MAGFILTER 4 MINFILTER 5 MIPFILTER 6 MIPMAPLODBIAS
 * 7 MAXMIPLEVEL 8 MAXANISOTROPY 9 COLORKEYOP 10 COLORSIGN 11 ALPHAKILL 12 COLOROP 13 COLORARG0
 * 14 COLORARG1 15 COLORARG2 16 ALPHAOP 17 ALPHAARG0 18 ALPHAARG1 19 ALPHAARG2 20 RESULTARG
 * 21 TEXTURETRANSFORMFLAGS 22..27 bump env 28 TEXCOORDINDEX 29 BORDERCOLOR 30 COLORKEYCOLOR */
#define XTEXTURE_STATE ((DWORD(*)[32])(UINT_PTR)0x0019E9A0u)

static void XBAPI hle_D3DDevice_SetTextureState_TexCoordIndex(DWORD Stage, DWORD Value)
{
    if (Stage < 4)
        XTEXTURE_STATE[Stage][28] = Value;
}

static void XBAPI hle_D3DDevice_SetTextureState_BorderColor(DWORD Stage, DWORD Value)
{
    if (Stage < 4)
        XTEXTURE_STATE[Stage][29] = Value;
}

static void XBAPI hle_D3DDevice_SetTextureState_ColorKeyColor(DWORD Stage, DWORD Value)
{
    if (Stage < 4)
        XTEXTURE_STATE[Stage][30] = Value;
}

/* Type is the texture-state index itself (22..27: BUMPENVMAT00..BUMPENVLOFFSET). */
static void XBAPI hle_D3DDevice_SetTextureState_BumpEnv(DWORD Stage, DWORD Type, DWORD Value)
{
    if (Stage < 4 && Type < 32)
        XTEXTURE_STATE[Stage][Type] = Value;
}

/* Xbox D3DTOP numbering differs from PC from 13 on. */
static DWORD xbox_top(DWORD op)
{
    static const BYTE map[27] = {
        0, D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE, D3DTOP_MODULATE2X,
        D3DTOP_MODULATE4X, D3DTOP_ADD, D3DTOP_ADDSIGNED, D3DTOP_ADDSIGNED2X, D3DTOP_SUBTRACT, D3DTOP_ADDSMOOTH,
        D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDCURRENTALPHA, D3DTOP_BLENDTEXTUREALPHA, D3DTOP_BLENDFACTORALPHA,
        D3DTOP_BLENDTEXTUREALPHAPM, D3DTOP_PREMODULATE, D3DTOP_MODULATEALPHA_ADDCOLOR,
        D3DTOP_MODULATECOLOR_ADDALPHA, D3DTOP_MODULATEINVALPHA_ADDCOLOR, D3DTOP_MODULATEINVCOLOR_ADDALPHA,
        D3DTOP_DOTPRODUCT3, D3DTOP_MULTIPLYADD, D3DTOP_LERP, D3DTOP_BUMPENVMAP, D3DTOP_BUMPENVMAPLUMINANCE};
    return op < 27 && map[op] ? map[op] : D3DTOP_DISABLE;
}

static DWORD xbox_address(DWORD a)
{
    return a == 5 ? D3DTADDRESS_CLAMP /* CLAMPTOEDGE */ : (a >= 1 && a <= 4) ? a : D3DTADDRESS_WRAP;
}

static DWORD xbox_filter(DWORD f)
{
    return f <= 3 ? f : D3DTEXF_LINEAR; /* quincunx / gaussian cubic -> linear */
}

static void apply_texture_stages(void)
{
    for (DWORD s = 0; s < 4; s++) {
        const DWORD *t = XTEXTURE_STATE[s];
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_ADDRESSU, xbox_address(t[0]));
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_ADDRESSV, xbox_address(t[1]));
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_ADDRESSW, xbox_address(t[2]));
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MAGFILTER, xbox_filter(t[3]));
        if (g_settings.anisotropy > 1 && xbox_filter(t[4]) == D3DTEXF_LINEAR) { /* ReJet menu: texture filtering */
            IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
            IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MAXANISOTROPY, (DWORD)g_settings.anisotropy);
        }
        else
            IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MINFILTER, xbox_filter(t[4]));
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MIPFILTER, xbox_filter(t[5]));
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MIPMAPLODBIAS, t[6]);
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MAXMIPLEVEL, t[7]);
        if (g_settings.anisotropy <= 1 || xbox_filter(t[4]) != D3DTEXF_LINEAR)
            IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MAXANISOTROPY, t[8] ? t[8] : 1);
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_BORDERCOLOR, t[29]);
        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_COLOROP, xbox_top(t[12]));
        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_COLORARG0, t[13]);
        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_COLORARG1, t[14]);
        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_COLORARG2, t[15]);
        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_ALPHAOP, xbox_top(t[16]));
        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_ALPHAARG0, t[17]);
        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_ALPHAARG1, t[18]);
        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_ALPHAARG2, t[19]);
        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_RESULTARG, t[20]);
        /* Texture transforms are done by the vertex programs; texcoords arrive final. */
        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_TEXCOORDINDEX, s);
    }
}

/* ------------------------------------------------------------------ transforms, lights, viewport */

/* Xbox D3DTRANSFORMSTATETYPE: VIEW 0, PROJECTION 1, TEXTURE0-3 2..5, WORLD 6, WORLD1-3 7..9.
 * Kept for GetTransform (the engine reads non-projection states back, 0x154D00) and passed to
 * the host for fixed-function draws. */
static D3DMATRIX g_xform[10];
static WORD g_xform_set; /* bit per state: put back after a device reset */

static D3DTRANSFORMSTATETYPE host_transform(DWORD s)
{
    switch (s) {
    case 0: return D3DTS_VIEW;
    case 1: return D3DTS_PROJECTION;
    case 2: case 3: case 4: case 5: return (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + (s - 2));
    case 6: return D3DTS_WORLD;
    default: return (D3DTRANSFORMSTATETYPE)D3DTS_WORLDMATRIX(s - 6);
    }
}

/* Widescreen (Hor+). The game draws for a 640x480 screen, which ReJet stretches over the
 * render target whatever its shape. Its projection fixes the horizontal field of view (m00 = 1;
 * w' = tan(fov / 2) z) and derives the vertical one for 4:3 (m22 = 1.333); dividing m00 by
 * k = (render aspect / 4:3) keeps the vertical view and shows more at the sides once stretched.
 * The engine builds the matrix in its own memory (0x234F58, m00 never rewritten), reuses it for
 * its vertex-program constants and passes it here (from 0x154A39), so it is patched in place; a
 * matrix already patched is recognised, so repeated calls don't compound.
 * The engine's own screen-space values (focal length 0x26B7A8.., used to anchor speech bubbles
 * and the like) are left alone, so that 2D squeezed to 4:3 (squeeze_2d) still meets them. Its
 * culling planes are not: the renderer's SetProjection (vtable 0x1E6BA0 slot 31, 0x154A40,
 * stdcall this/fov/near/far; the vtable is its only reference) derives the horizontal ones
 * (0x26B740..0x26B74C) from tan(fov / 2), so objects just outside the old 4:3 view were culled
 * and popped in at the screen edges. After it runs, those planes are widened by k too. */
#define RENDERER_SET_PROJECTION_SLOT 0x001E6C1Cu /* vtable 0x1E6BA0 + 31 * 4 */
#define RENDERER_SET_PROJECTION 0x00154A40u
#define CULL_PLANES 0x0026B740u /* 4 floats: 1 / cos, -tan, 1 / cos, tan (of fov / 2) */

typedef void(XBAPI *SetProjectionFn)(void *self, int fov, float zn, float zf);
static SetProjectionFn g_orig_set_projection;

static float widescreen_k(void)
{
    return ws_k(g_hw, g_hh);
}

static void XBAPI hook_set_projection(void *self, int fov, float zn, float zf)
{
    float *planes = (float *)(UINT_PTR)CULL_PLANES, k = widescreen_k();
    g_orig_set_projection(self, fov, zn, zf);
    {
        static int logged;
        if (!logged++)
            log_printf("widescreen: culling planes for fov %X: %.4f %.4f %.4f %.4f\n", fov, (double)planes[0],
                       (double)planes[1], (double)planes[2], (double)planes[3]);
    }
    ws_widen_planes(planes, k);
}

static void widescreen_install(void)
{
    void **slot = (void **)(UINT_PTR)RENDERER_SET_PROJECTION_SLOT;
    DWORD old;
    if (g_orig_set_projection)
        return;
    if ((UINT_PTR)*slot != RENDERER_SET_PROJECTION) {
        log_printf("widescreen: renderer vtable not as expected; culling stays 4:3\n");
        return;
    }
    g_orig_set_projection = (SetProjectionFn)*slot;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *slot = (void *)hook_set_projection;
    VirtualProtect(slot, sizeof *slot, old, &old);
}

static D3DMATRIX *g_proj; /* the game's projection matrix (its memory), re-patched on display changes */

static void widescreen_projection(D3DMATRIX *m)
{
    static WsProjection st;
    g_proj = m;
    ws_patch_projection(&st, m, widescreen_k());
}

static HRESULT XBAPI hle_D3DDevice_SetTransform(DWORD State, const D3DMATRIX *pMatrix)
{
    if (State == 1 && pMatrix)
        widescreen_projection((D3DMATRIX *)pMatrix); /* game memory */
    if (State < 10 && pMatrix) {
        static LONG logged[10];
        if (InterlockedIncrement(&logged[State]) <= 3)
            log_printf("d3d: SetTransform %lu (frame %lu, from %08X): %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / "
                       "%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f\n", State, g_frame,
                       (unsigned)(UINT_PTR)_ReturnAddress(), pMatrix->_11, pMatrix->_12, pMatrix->_13, pMatrix->_14,
                       pMatrix->_21, pMatrix->_22, pMatrix->_23, pMatrix->_24, pMatrix->_31, pMatrix->_32,
                       pMatrix->_33, pMatrix->_34, pMatrix->_41, pMatrix->_42, pMatrix->_43, pMatrix->_44);
        g_xform[State] = *pMatrix;
        g_xform_set |= (WORD)(1u << State);
        if (g_dev)
            IDirect3DDevice9Ex_SetTransform(g_dev, host_transform(State), pMatrix);
    }
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_GetTransform(DWORD State, D3DMATRIX *pMatrix)
{
    if (State < 10 && pMatrix)
        *pMatrix = g_xform[State];
    return S_OK;
}

/* D3DMATERIAL8 and D3DLIGHT8 share D3D9's layouts. Copies are kept for lighting draws without
 * normals (apply_lazy_states). */
#define MAX_LIGHTS 8
static D3DMATERIAL9 g_material;
static D3DLIGHT9 g_light[MAX_LIGHTS];
static BOOL g_light_on[MAX_LIGHTS];

static HRESULT XBAPI hle_D3DDevice_SetMaterial(const D3DMATERIAL9 *pMaterial)
{
    if (pMaterial)
        g_material = *pMaterial;
    if (pMaterial && g_capture_log && g_frame == g_capture_frame)
        fprintf(g_capture_log, "material: diffuse %.2f %.2f %.2f %.2f ambient %.2f %.2f %.2f emissive %.2f %.2f %.2f\n",
                pMaterial->Diffuse.r, pMaterial->Diffuse.g, pMaterial->Diffuse.b, pMaterial->Diffuse.a,
                pMaterial->Ambient.r, pMaterial->Ambient.g, pMaterial->Ambient.b, pMaterial->Emissive.r,
                pMaterial->Emissive.g, pMaterial->Emissive.b);
    if (g_dev && pMaterial)
        IDirect3DDevice9Ex_SetMaterial(g_dev, pMaterial);
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_SetLight(DWORD Index, const D3DLIGHT9 *pLight)
{
    if (pLight && g_capture_log && g_frame == g_capture_frame)
        fprintf(g_capture_log, "light %lu: type %d diffuse %.2f %.2f %.2f ambient %.2f %.2f %.2f dir %.2f %.2f %.2f\n",
                Index, (int)pLight->Type, pLight->Diffuse.r, pLight->Diffuse.g, pLight->Diffuse.b, pLight->Ambient.r,
                pLight->Ambient.g, pLight->Ambient.b, pLight->Direction.x, pLight->Direction.y, pLight->Direction.z);
    if (pLight && Index < MAX_LIGHTS)
        g_light[Index] = *pLight;
    if (g_dev && pLight)
        IDirect3DDevice9Ex_SetLight(g_dev, Index, pLight);
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_LightEnable(DWORD Index, BOOL Enable)
{
    if (Index < MAX_LIGHTS)
        g_light_on[Index] = Enable;
    if (g_dev)
        IDirect3DDevice9Ex_LightEnable(g_dev, Index, Enable);
    return S_OK;
}

/* The host viewport stays the full render target: programs output screen-space pixels, which
 * the vertex epilogue maps over the whole target. The Xbox viewport feeds c-38/c-37, and its
 * clipping becomes a scissor rect (split screen; ReWhacked). */
static HRESULT XBAPI hle_D3DDevice_SetViewport(const D3DVIEWPORT9 *pViewport)
{
    const D3DVIEWPORT9 *v = pViewport;
    set_screen_constants(pViewport);
    if (g_dev) {
        int full = v->X == 0 && v->Y == 0 && v->Width >= g_tw && v->Height >= g_th;
        g_vp_rect.left = (LONG)(v->X * g_tsx + 0.5f);
        g_vp_rect.top = (LONG)(v->Y * g_tsy + 0.5f);
        g_vp_rect.right = (LONG)((v->X + v->Width) * g_tsx + 0.5f);
        g_vp_rect.bottom = (LONG)((v->Y + v->Height) * g_tsy + 0.5f);
        g_vp_partial = !full;
        if (g_vp_partial)
            IDirect3DDevice9Ex_SetScissorRect(g_dev, &g_vp_rect);
        IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_SCISSORTESTENABLE, g_vp_partial ? TRUE : FALSE);
        {
            SURVEY(16)
            log_printf("d3d: viewport %lu,%lu %lux%lu z %.2f-%.2f%s\n", v->X, v->Y, v->Width, v->Height,
                       (double)v->MinZ, (double)v->MaxZ, g_vp_partial ? " (clipped)" : "");
        }
    }
    return S_OK;
}

/* Render-to-texture. Each Xbox texture used as a render target (the engine's 512x512 and
 * 128x128 LIN_R5G6B5 targets, set through renderer slot 42 and the camera apply methods) gets a
 * host render-target texture at the render scale and its own depth surface (D3D9 needs depth at
 * least as large as the target). It is found by the texture's data address (the same for the
 * texture and its level-0 surface), and prepare_draw binds it wherever the game samples that
 * texture. Linear formats are sampled with texel coordinates, so the game size gives the scale. */
#define MAX_RT 64
typedef struct RenderTarget {
    DWORD data;          /* Xbox texture data address (the key) */
    UINT w, h;           /* game size */
    UINT hw, hh;         /* host size */
    int linear;          /* texel-addressed format */
    D3DFORMAT format;    /* host format */
    IDirect3DTexture9 *tex;
    IDirect3DSurface9 *surf, *depth;
} RenderTarget;
static RenderTarget g_rt[MAX_RT];
static unsigned g_rt_count;

/* (Re)create a render target's host texture and depth at the current render scale. */
static int rt_create_host(RenderTarget *r)
{
    if (r->depth)
        IDirect3DSurface9_Release(r->depth);
    if (r->surf)
        IDirect3DSurface9_Release(r->surf);
    if (r->tex)
        IDirect3DTexture9_Release(r->tex);
    r->tex = NULL;
    r->surf = r->depth = NULL;
    r->hw = (UINT)(r->w * g_sx + 0.5f);
    r->hh = (UINT)(r->h * g_sy + 0.5f);
    if (r->hw > 4096)
        r->hw = 4096;
    if (r->hh > 4096)
        r->hh = 4096;
    return SUCCEEDED(IDirect3DDevice9Ex_CreateTexture(g_dev, r->hw, r->hh, 1, D3DUSAGE_RENDERTARGET, r->format,
                                                      D3DPOOL_DEFAULT, &r->tex, NULL)) &&
           SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(r->tex, 0, &r->surf)) &&
           SUCCEEDED(IDirect3DDevice9Ex_CreateDepthStencilSurface(g_dev, r->hw, r->hh, D3DFMT_D24S8,
                                                                 D3DMULTISAMPLE_NONE, 0, TRUE, &r->depth, NULL));
}

static const RenderTarget *rt_find(DWORD data)
{
    for (unsigned i = 0; i < g_rt_count; i++)
        if (g_rt[i].data == data)
            return &g_rt[i];
    return NULL;
}

static const RenderTarget *rt_get(const XD3DSurface *s)
{
    const XD3DPixelContainer *c = &s->Container;
    uint32_t f = XFMT_FORMAT(c->Format);
    RenderTarget *r;
    const RenderTarget *found = rt_find(c->Resource.Data);
    D3DFORMAT hf = (f == XD3DFMT_A8R8G8B8 || f == XD3DFMT_LIN_A8R8G8B8 || f == XD3DFMT_A4R4G4B4 ||
                    f == XD3DFMT_LIN_A4R4G4B4 || f == XD3DFMT_A1R5G5B5 || f == XD3DFMT_LIN_A1R5G5B5)
                       ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
    if (found)
        return found;
    if (g_rt_count >= MAX_RT || !c->Resource.Data)
        return NULL;
    r = &g_rt[g_rt_count];
    memset(r, 0, sizeof *r);
    r->data = c->Resource.Data;
    r->w = c->Size ? XSIZE_WIDTH(c->Size) : 1u << XFMT_USIZE(c->Format);
    r->h = c->Size ? XSIZE_HEIGHT(c->Size) : 1u << XFMT_VSIZE(c->Format);
    r->linear = c->Size != 0;
    r->format = hf;
    if (!rt_create_host(r)) {
        log_printf("d3d: render target %ux%u (format %02X) could not be created\n", r->w, r->h, f);
        return NULL;
    }
    log_printf("d3d: render target #%u: %ux%u format %02X%s at %08X -> host %ux%u\n", g_rt_count, r->w, r->h, f,
               r->linear ? " (linear)" : "", r->data, r->hw, r->hh);
    g_rt_count++;
    return r;
}

/* Current target's size in game pixels and the host/game scale (Clear, viewport, scissor,
 * pre-transformed vertices). */
static void set_target_metrics(UINT w, UINT h, float sx, float sy)
{
    g_tw = w;
    g_th = h;
    g_tsx = sx;
    g_tsy = sy;
    g_vconst[VSH_C_SCREEN][0] = 2.0f / (float)w;
    g_vconst[VSH_C_SCREEN][1] = -2.0f / (float)h;
    g_vp_partial = 0;
    IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_SCISSORTESTENABLE, FALSE);
}

static HRESULT XBAPI hle_D3DDevice_SetRenderTarget(XD3DSurface *pRenderTarget, XD3DSurface *pNewZStencil)
{
    const RenderTarget *r;
    {
        SURVEY(12)
        log_printf("survey: SetRenderTarget %p%s fmt %08X size %08X, z %p%s (from %08X)\n", (void *)pRenderTarget,
                   pRenderTarget == &g_backbuffer ? " (back buffer)" : "",
                   pRenderTarget ? pRenderTarget->Container.Format : 0, pRenderTarget ? pRenderTarget->Container.Size : 0,
                   (void *)pNewZStencil, pNewZStencil == &g_depth ? " (depth)" : "",
                   (unsigned)(UINT_PTR)_ReturnAddress());
    }
    if (pRenderTarget && g_capture_log && g_frame == g_capture_frame)
        fprintf(g_capture_log, "target: %s, Xbox data %08X, depth %s (from %08X)\n",
                pRenderTarget == &g_backbuffer ? "back buffer" : "texture", pRenderTarget->Container.Resource.Data,
                pNewZStencil ? "yes" : "none", (unsigned)(UINT_PTR)_ReturnAddress());
    if (!g_dev || !pRenderTarget)
        return S_OK;
    if (pRenderTarget == &g_backbuffer) {
        if (g_cur_rt) {
            IDirect3DDevice9Ex_SetRenderTarget(g_dev, 0, g_host_bb);
            IDirect3DDevice9Ex_SetDepthStencilSurface(g_dev, pNewZStencil ? g_host_depth : NULL);
            g_cur_rt = NULL;
            set_target_metrics(g_xw, g_xh, g_sx, g_sy);
        }
        else {
            IDirect3DDevice9Ex_SetDepthStencilSurface(g_dev, pNewZStencil ? g_host_depth : NULL);
        }
        return S_OK;
    }
    r = rt_get(pRenderTarget);
    if (!r)
        return S_OK;
    IDirect3DDevice9Ex_SetRenderTarget(g_dev, 0, r->surf);
    IDirect3DDevice9Ex_SetDepthStencilSurface(g_dev, pNewZStencil ? r->depth : NULL);
    g_cur_rt = r;
    set_target_metrics(r->w, r->h, (float)r->hw / (float)r->w, (float)r->hh / (float)r->h);
    return S_OK;
}

/* ReJet menu: resolution or VSync changed. Called on the game thread inside Swap (between
 * frames). ResetEx gives the back buffer and depth their new size; D3D9Ex keeps textures,
 * shaders and buffers, but resets device state, so what ReJet has set is put back, and the
 * render-target textures are recreated at the new scale (the game redraws them every frame). */
static void change_display(void)
{
    D3DPRESENT_PARAMETERS hp = g_hp;
    UINT w, h = (UINT)g_settings.height;
    g_settings.width = ((int)((float)h * settings_aspect_ratio(g_settings.aspect) + 0.5f) + 1) & ~1;
    w = (UINT)g_settings.width;
    HRESULT hr;
    if (!g_dev)
        return;
    hp.BackBufferWidth = w;
    hp.BackBufferHeight = h;
    hp.PresentationInterval = g_settings.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    if (hp.BackBufferWidth == g_hp.BackBufferWidth && hp.BackBufferHeight == g_hp.BackBufferHeight &&
        hp.PresentationInterval == g_hp.PresentationInterval && msaa_wanted() == g_ms_type)
        return;
    msaa_release(); /* the scene surfaces are made again at the new size */
    hr = IDirect3DDevice9Ex_ResetEx(g_dev, &hp, NULL);
    if (FAILED(hr)) {
        log_printf("d3d: ResetEx to %ux%u failed (%08lX); keeping %ux%u\n", w, h, hr, g_hw, g_hh);
        hp = g_hp;
        if (FAILED(IDirect3DDevice9Ex_ResetEx(g_dev, &hp, NULL)))
            log_printf("d3d: ResetEx back to %ux%u failed too\n", g_hw, g_hh);
        g_settings.width = (int)g_hw;
        g_settings.height = (int)g_hh;
    }
    else {
        g_hp = hp;
        g_hw = w;
        g_hh = h;
        g_sx = (float)g_hw / (float)g_xw;
        g_sy = (float)g_hh / (float)g_xh;
    }
    msaa_setup();
    for (unsigned i = 0; i < g_rt_count; i++)
        if (!rt_create_host(&g_rt[i]))
            log_printf("d3d: render target #%u could not be recreated\n", i);
    if (g_proj && (g_xform_set & 2)) { /* the new aspect into the game's projection */
        widescreen_projection(g_proj);
        g_xform[1] = *g_proj;
    }
    /* state D3D9 forgot */
    for (unsigned i = 0; i < 256; i++)
        if (g_rs_set[i])
            IDirect3DDevice9Ex_SetRenderState(g_dev, (D3DRENDERSTATETYPE)i, g_rs[i]);
    for (DWORD i = 0; i < 10; i++)
        if (g_xform_set & (1u << i))
            IDirect3DDevice9Ex_SetTransform(g_dev, host_transform(i), &g_xform[i]);
    IDirect3DDevice9Ex_SetMaterial(g_dev, &g_material);
    for (DWORD i = 0; i < MAX_LIGHTS; i++) {
        if (g_light[i].Type)
            IDirect3DDevice9Ex_SetLight(g_dev, i, &g_light[i]);
        IDirect3DDevice9Ex_LightEnable(g_dev, i, g_light_on[i]);
    }
    if (g_cur_rt) {
        IDirect3DDevice9Ex_SetRenderTarget(g_dev, 0, g_cur_rt->surf);
        IDirect3DDevice9Ex_SetDepthStencilSurface(g_dev, g_cur_rt->depth);
        set_target_metrics(g_cur_rt->w, g_cur_rt->h, (float)g_cur_rt->hw / (float)g_cur_rt->w,
                           (float)g_cur_rt->hh / (float)g_cur_rt->h);
    }
    else
        set_target_metrics(g_xw, g_xh, g_sx, g_sy);
    log_printf("d3d: display changed: rendering at %ux%u, vsync %s\n", g_hw, g_hh, g_settings.vsync ? "on" : "off");
}

/* End of a captured frame: the screenshot, each render-target texture as the game left it, and
 * a summary, next to the draw list. */
static void capture_finish(void)
{
    char path[MAX_PATH];
    FILE *f;
    snprintf(path, sizeof path, "%s\\screen.bmp", g_capture_dir);
    save_backbuffer(path);
    for (unsigned i = 0; i < g_rt_count; i++) {
        snprintf(path, sizeof path, "%s\\rendertarget%u_%ux%u.bmp", g_capture_dir, i, g_rt[i].w, g_rt[i].h);
        save_surface(g_rt[i].surf, path);
    }
    fclose(g_capture_log);
    g_capture_log = NULL;
    snprintf(path, sizeof path, "%s\\info.txt", g_capture_dir);
    if ((f = fopen(path, "w")) != NULL) {
        fprintf(f, "ReJet %s\n", REJET_BUILD);
        fprintf(f, "frame %lu, vblank %lu, %lu draws\nrender %ux%u, game %ux%u\nrender targets: %u\n", g_frame,
                vblank_now(), g_draws, g_hw, g_hh, g_xw, g_xh, g_rt_count);
        for (unsigned i = 0; i < g_rt_count; i++)
            fprintf(f, "  #%u: %ux%u%s, Xbox data %08lX, host %ux%u\n", i, g_rt[i].w, g_rt[i].h,
                    g_rt[i].linear ? " linear" : "", g_rt[i].data, g_rt[i].hw, g_rt[i].hh);
        fprintf(f, "Xbox render-state array at 0x19EBA0 (index: values), at the end of the frame:\n");
        for (unsigned i = 0; i < 0xC0; i += 8) {
            const DWORD *x = (const DWORD *)(UINT_PTR)0x0019EBA0u + i;
            fprintf(f, "  %3u: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\n", i, x[0], x[1], x[2], x[3], x[4],
                    x[5], x[6], x[7]);
        }
        fclose(f);
    }
    log_printf("==================== CAPTURE done: %s ====================\n", g_capture_dir);
    menu_toast("Bug capture saved: %s", strrchr(g_capture_dir, '\\') ? strrchr(g_capture_dir, '\\') + 1 : g_capture_dir);
    g_capture_frame = 0xFFFFFFFF;
}

/* CopyRects(src, rects, count, dst, points): surface-to-surface copies on the GPU. JSRF's boost
 * effect copies the picture into its 512x512 / 128x128 textures this way; without the copy
 * they kept old contents and the screen went grey and red. Sources: the back buffer or a
 * render-target texture; the destination becomes (or is) a render-target texture, which later
 * draws sample. Rectangles are in game pixels of each surface and scaled to host pixels. */
static HRESULT XBAPI hle_D3DDevice_CopyRects(XD3DSurface *pSrc, const RECT *pSrcRects, UINT cRects, XD3DSurface *pDst,
                                             const POINT *pDstPoints)
{
    IDirect3DSurface9 *src;
    const RenderTarget *s = NULL, *d;
    float ssx, ssy;
    UINT sw, sh;
    {
        SURVEY(12)
        log_printf("survey: CopyRects %p%s -> %p, %u rects (first %ld,%ld-%ld,%ld) (from %08X)\n", (void *)pSrc,
                   pSrc == &g_backbuffer ? " (back buffer)" : "", (void *)pDst, cRects,
                   pSrcRects ? pSrcRects->left : 0, pSrcRects ? pSrcRects->top : 0, pSrcRects ? pSrcRects->right : 0,
                   pSrcRects ? pSrcRects->bottom : 0, (unsigned)(UINT_PTR)_ReturnAddress());
    }
    if (pSrc && pDst && g_capture_log && g_frame == g_capture_frame)
        fprintf(g_capture_log, "copyrects: %08X%s -> %08X%s, %u rects (from %08X)\n", pSrc->Container.Resource.Data,
                pSrc == &g_backbuffer ? " (back buffer)" : "", pDst->Container.Resource.Data,
                pDst == &g_backbuffer ? " (back buffer)" : "", cRects, (unsigned)(UINT_PTR)_ReturnAddress());
    if (!g_dev || !pSrc || !pDst)
        return S_OK;
    if (pSrc == &g_backbuffer) {
        src = g_host_bb;
        ssx = g_sx;
        ssy = g_sy;
        sw = g_xw;
        sh = g_xh;
    }
    else if ((s = rt_find(pSrc->Container.Resource.Data)) != NULL) {
        src = s->surf;
        ssx = (float)s->hw / (float)s->w;
        ssy = (float)s->hh / (float)s->h;
        sw = s->w;
        sh = s->h;
    }
    else {
        TODO_ONCE("CopyRects from a texture the GPU has not drawn");
        return S_OK;
    }
    if (pDst == &g_backbuffer || !(d = rt_get(pDst))) {
        TODO_ONCE("CopyRects to the back buffer");
        return S_OK;
    }
    if (g_in_scene) { /* copies go between scenes */
        IDirect3DDevice9Ex_EndScene(g_dev);
        g_in_scene = 0;
    }
    if (src == g_ms_bb && g_ms_bb) { /* a multisampled surface can't be copied part by part: resolve it */
        msaa_resolve();
        src = g_real_bb;
    }
    {
        float dsx = (float)d->hw / (float)d->w, dsy = (float)d->hh / (float)d->h;
        UINT n = (cRects && pSrcRects) ? cRects : 1;
        for (UINT i = 0; i < n; i++) {
            RECT sr = pSrcRects && cRects ? pSrcRects[i] : (RECT){0, 0, (LONG)sw, (LONG)sh};
            POINT p = pDstPoints ? pDstPoints[i] : (POINT){sr.left, sr.top};
            RECT hs = {(LONG)(sr.left * ssx), (LONG)(sr.top * ssy), (LONG)(sr.right * ssx), (LONG)(sr.bottom * ssy)};
            RECT hd = {(LONG)(p.x * dsx), (LONG)(p.y * dsy), (LONG)((p.x + sr.right - sr.left) * dsx),
                       (LONG)((p.y + sr.bottom - sr.top) * dsy)};
            if (hd.right > (LONG)d->hw)
                hd.right = (LONG)d->hw;
            if (hd.bottom > (LONG)d->hh)
                hd.bottom = (LONG)d->hh;
            if (hs.right > hs.left && hs.bottom > hs.top && hd.right > hd.left && hd.bottom > hd.top)
                IDirect3DDevice9Ex_StretchRect(g_dev, src, &hs, d->surf, &hd, D3DTEXF_LINEAR);
        }
    }
    return S_OK;
}

/* ------------------------------------------------------------------ textures and streams */

static HRESULT XBAPI hle_D3DDevice_SetTexture(DWORD Stage, const XD3DPixelContainer *pTexture)
{
    {
        SURVEY(24)
        if (pTexture)
            log_printf("survey: SetTexture stage %lu %p data %08X format %08X (fmt %02X dim %u mips %u %ux%u) size "
                       "%08X\n", Stage, (const void *)pTexture, pTexture->Resource.Data, pTexture->Format,
                       XFMT_FORMAT(pTexture->Format), XFMT_DIMENSION(pTexture->Format), XFMT_MIPMAPS(pTexture->Format),
                       1u << XFMT_USIZE(pTexture->Format), 1u << XFMT_VSIZE(pTexture->Format), pTexture->Size);
    }
    if (Stage < 4)
        g_textures[Stage] = pTexture;
    return S_OK;
}

/* Xbox index buffers are plain memory. The engine computes the index pointer for
 * DrawIndexedVertices as D3D__IndexData + start (0x15697A), so SetIndices sets it, as the
 * original does. */
#define D3D_INDEX_DATA_GLOBAL 0x0019E994u

static HRESULT XBAPI hle_D3DDevice_SetIndices(const XD3DResource *pIndexData, UINT BaseVertexIndex)
{
    *(DWORD *)(UINT_PTR)D3D_INDEX_DATA_GLOBAL = pIndexData ? pIndexData->Data : 0;
    g_base_vertex = BaseVertexIndex;
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_SetStreamSource(UINT Stream, XD3DResource *pStreamData, UINT Stride)
{
    if (Stream == 0) {
        g_stream_vb = pStreamData;
        g_stream_stride = Stride;
    }
    else {
        TODO_ONCE("SetStreamSource on streams other than 0");
    }
    return S_OK;
}

/* ------------------------------------------------------------------ drawing */

/* Debugging: REJET_DRAWLOG=<frame> lists that frame's draws with the states that decide how
 * they look (shader, texture, blend, alpha test, depth, stencil, cull). */
static DWORD g_dl_prim, g_dl_count;
static const BYTE *g_dl_verts; /* a small draw-up's vertices, dumped into captures */
static UINT g_dl_stride;
static DWORD g_dl_callers[8]; /* the game functions behind this draw (find_game_callers) */

static void draw_log(void)
{
    static long at = -2;
    const XD3DPixelContainer *t = g_textures[0];
    char line[512];
    int capture = g_capture_log && g_frame == g_capture_frame;
    if (at == -2) {
        char env[16] = "";
        at = GetEnvironmentVariableA("REJET_DRAWLOG", env, sizeof env) ? atol(env) : -1;
    }
    if (!capture && (at < 0 || g_frame != (ULONG)at))
        return;
    snprintf(line, sizeof line,
             "drawlog %4lu: prim %lu x%-5lu %s %-4lu ps %-3ld t0 %02X %4ux%-4u%s @%08X blend %lu %lu/%lu at %lu/%lu "
             "z %lu/%lu/%lu st %lu/%lu/%lu/%lu cull %lX front %lX%s",
             g_draws, g_dl_prim, g_dl_count, (g_cur_vs & 1) ? "vs " : "fvf", (g_cur_vs & 1) ? g_cur_vs >> 1 : g_cur_vs,
             (g_cur_ps & 1) ? (long)(g_cur_ps >> 1) : -1L, t ? XFMT_FORMAT(t->Format) : 0,
             t ? (t->Size ? XSIZE_WIDTH(t->Size) : 1u << XFMT_USIZE(t->Format)) : 0,
             t ? (t->Size ? XSIZE_HEIGHT(t->Size) : 1u << XFMT_VSIZE(t->Format)) : 0,
             t && rt_find(t->Resource.Data) ? " (rt)" : "", t ? t->Resource.Data : 0, g_rs[D3DRS_ALPHABLENDENABLE],
             g_rs[D3DRS_SRCBLEND], g_rs[D3DRS_DESTBLEND], g_rs[D3DRS_ALPHATESTENABLE], g_rs[D3DRS_ALPHAREF],
             g_rs[D3DRS_ZENABLE], g_rs[D3DRS_ZWRITEENABLE], g_rs[D3DRS_ZFUNC], g_rs[D3DRS_STENCILENABLE],
             g_rs[D3DRS_STENCILFUNC], g_rs[D3DRS_STENCILREF], g_rs[D3DRS_STENCILPASS], g_x_cull, g_x_front,
             g_cur_rt ? " -> rt" : "");
    if (capture) {
        fprintf(g_capture_log, "%s\n", line + 8);
        fprintf(g_capture_log,
                "      stage0 colorop %lu args %lX %lX alphaop %lu args %lX %lX | viewport %ld,%ld-%ld,%ld%s | lighting %lu\n",
                XTEXTURE_STATE[0][12], XTEXTURE_STATE[0][14], XTEXTURE_STATE[0][15], XTEXTURE_STATE[0][16],
                XTEXTURE_STATE[0][18], XTEXTURE_STATE[0][19], g_vp_rect.left, g_vp_rect.top, g_vp_rect.right,
                g_vp_rect.bottom, g_vp_partial ? " (clipped)" : "", g_rs[D3DRS_LIGHTING]);
        for (int s = 1; s < 4; s++) { /* the other stages: multi-texture draws (decals, effects) */
            const XD3DPixelContainer *ts = g_textures[s];
            if (!ts && XTEXTURE_STATE[s][12] <= 1) /* empty and disabled */
                continue;
            fprintf(g_capture_log, "      stage%d t %02X %ux%u%s @%08X colorop %lu args %lX %lX alphaop %lu texcoord %lX\n",
                    s, ts ? XFMT_FORMAT(ts->Format) : 0,
                    ts ? (ts->Size ? XSIZE_WIDTH(ts->Size) : 1u << XFMT_USIZE(ts->Format)) : 0,
                    ts ? (ts->Size ? XSIZE_HEIGHT(ts->Size) : 1u << XFMT_VSIZE(ts->Format)) : 0,
                    ts && rt_find(ts->Resource.Data) ? " (rt)" : "", ts ? ts->Resource.Data : 0,
                    XTEXTURE_STATE[s][12], XTEXTURE_STATE[s][14], XTEXTURE_STATE[s][15], XTEXTURE_STATE[s][16],
                    XTEXTURE_STATE[s][28]);
        }
        if (g_cur_vs & 1) { /* a vertex program: its declaration and constants c[96..135], once per program */
            static unsigned char listed[MAX_SHADERS];
            static ULONG listed_frame;
            unsigned i = g_cur_vs >> 1;
            if (listed_frame != g_frame) {
                memset(listed, 0, sizeof listed);
                listed_frame = g_frame;
            }
            if (i < MAX_SHADERS && !listed[i]) {
                listed[i] = 1;
                fprintf(g_capture_log, "      vs %u declaration:", i);
                for (unsigned k = 0; k < g_vs_decl_n[i]; k++)
                    fprintf(g_capture_log, " %08lX", g_vs_decl[i][k]);
                fprintf(g_capture_log, "\n");
                for (unsigned r = 96; r < 136; r++)
                    fprintf(g_capture_log, "      c%u: %g %g %g %g\n", r, g_vconst[r][0], g_vconst[r][1],
                            g_vconst[r][2], g_vconst[r][3]);
            }
        }
        if (g_textures[0] && tex_name(g_textures[0]))
            fprintf(g_capture_log, "      t0 name %016llX\n", tex_name(g_textures[0]));
        if (g_dl_callers[0]) {
            fprintf(g_capture_log, "      game callers:");
            for (int k = 0; k < 8 && g_dl_callers[k]; k++)
                fprintf(g_capture_log, " %08lX", g_dl_callers[k]);
            fputc('\n', g_capture_log);
        }
        if (g_dl_verts) /* raw vertices: all of small draws, the first 8 of others */
            for (DWORD v = 0; v < g_dl_count && v < 8; v++) {
                const DWORD *d = (const DWORD *)(g_dl_verts + v * g_dl_stride);
                fprintf(g_capture_log, "      v%lu:", v);
                for (UINT k = 0; k < g_dl_stride / 4; k++)
                    fprintf(g_capture_log, " %08lX", d[k]);
                fprintf(g_capture_log, "\n");
            }
    }
    else {
        log_printf("%s\n", line);
    }
}

/* Lighting states that D3D applies lazily at draw time: the engine writes them straight into
 * D3D's render-state array at 0x19EBA0 (XDK order: fog start/end at 84/85 and the 64.0 maximum
 * point size at 113 confirm the indices). Only fixed-function draws use them; the map screen is
 * drawn lit with materials, and came out white without them.
 * Lit vertices without a normal: D3D9 gives them ambient light only (black here), while the
 * NV2A keeps the last normal, which faces the light (the map's counter panel and icons). They
 * are drawn unlit with the material's fully lit colour as the texture factor instead. */
#define XRENDER_STATE ((const DWORD *)(UINT_PTR)0x0019EBA0u)

static DWORD lit_colour_facing_light(void)
{
    const D3DMATERIAL9 *m = &g_material;
    DWORD a = XRENDER_STATE[105];
    float c[3] = {m->Emissive.r + m->Ambient.r * ((a >> 16) & 0xFF) / 255.0f,
                  m->Emissive.g + m->Ambient.g * ((a >> 8) & 0xFF) / 255.0f,
                  m->Emissive.b + m->Ambient.b * (a & 0xFF) / 255.0f};
    DWORD out = 0;
    for (unsigned i = 0; i < MAX_LIGHTS; i++) {
        if (!g_light_on[i])
            continue;
        c[0] += g_light[i].Ambient.r * m->Ambient.r + g_light[i].Diffuse.r * m->Diffuse.r;
        c[1] += g_light[i].Ambient.g * m->Ambient.g + g_light[i].Diffuse.g * m->Diffuse.g;
        c[2] += g_light[i].Ambient.b * m->Ambient.b + g_light[i].Diffuse.b * m->Diffuse.b;
    }
    for (unsigned i = 0; i < 3; i++)
        out |= (DWORD)(c[i] < 0 ? 0 : c[i] > 1 ? 255 : c[i] * 255 + 0.5f) << (16 - 8 * i);
    return out | (DWORD)(m->Diffuse.a < 0 ? 0 : m->Diffuse.a > 1 ? 255 : m->Diffuse.a * 255 + 0.5f) << 24;
}

static void apply_lazy_states(void)
{
    int lit = XRENDER_STATE[92] != 0 && (g_cur_vs & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW;
    if (lit && !(g_cur_vs & D3DFVF_NORMAL)) {
        rs(D3DRS_LIGHTING, FALSE);
        if (!(g_cur_vs & D3DFVF_DIFFUSE)) {
            rs(D3DRS_TEXTUREFACTOR, lit_colour_facing_light());
            for (DWORD s = 0; s < 4 && XTEXTURE_STATE[s][12] > 1; s++) { /* 1 = disable */
                static const D3DTEXTURESTAGESTATETYPE arg[6] = {D3DTSS_COLORARG0, D3DTSS_COLORARG1,
                    D3DTSS_COLORARG2, D3DTSS_ALPHAARG0, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2};
                static const int idx[6] = {13, 14, 15, 17, 18, 19};
                if (s == 0 && !g_textures[0]) { /* the empty-stage rule disabled it: vertex colour */
                    IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                    IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_COLORARG1, D3DTA_TFACTOR);
                    IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                    IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
                    break;
                }
                for (int k = 0; k < 6; k++) {
                    DWORD v = XTEXTURE_STATE[s][idx[k]];
                    if ((v & D3DTA_SELECTMASK) == D3DTA_DIFFUSE)
                        IDirect3DDevice9Ex_SetTextureStageState(g_dev, s, arg[k], (v & ~D3DTA_SELECTMASK) | D3DTA_TFACTOR);
                }
            }
        }
        return;
    }
    if (g_rs[D3DRS_TEXTUREFACTOR] != g_x_tfactor)
        rs(D3DRS_TEXTUREFACTOR, g_x_tfactor);
    rs(D3DRS_LIGHTING, lit);
    rs(D3DRS_SPECULARENABLE, XRENDER_STATE[93] != 0);
    rs(D3DRS_LOCALVIEWER, XRENDER_STATE[94] != 0);
    rs(D3DRS_COLORVERTEX, XRENDER_STATE[95] != 0);
    rs(D3DRS_SPECULARMATERIALSOURCE, XRENDER_STATE[100]);
    rs(D3DRS_DIFFUSEMATERIALSOURCE, XRENDER_STATE[101]);
    rs(D3DRS_AMBIENTMATERIALSOURCE, XRENDER_STATE[102]);
    rs(D3DRS_EMISSIVEMATERIALSOURCE, XRENDER_STATE[103]);
    rs(D3DRS_AMBIENT, XRENDER_STATE[105]);
}

/* Bind the current vertex program (or FVF), constants, textures and pixel shader (ReWhacked).
 * Returns 0 to skip the draw. */
static int prepare_draw(void)
{
    if (!g_dev)
        return 0;
    if (!g_in_scene) {
        IDirect3DDevice9Ex_BeginScene(g_dev);
        g_in_scene = 1;
    }
    if (g_cur_vs & 1) {
        unsigned i = g_cur_vs >> 1;
        if (i >= g_vs_count || !g_vs_func[i])
            return 0;
        ensure_vs(i);
        if (g_vs_state[i] != 1) {
            g_skipped++;
            return 0;
        }
        IDirect3DDevice9Ex_SetVertexShader(g_dev, g_vs_host[i]);
        IDirect3DDevice9Ex_SetVertexDeclaration(g_dev, g_vs_vdecl[i]);
    }
    else {
        IDirect3DDevice9Ex_SetVertexShader(g_dev, NULL);
        IDirect3DDevice9Ex_SetFVF(g_dev, g_cur_vs);
    }
    for (DWORD s = 0; s < 4; s++) {
        float scale[2] = {1.0f, 1.0f};
        LONGLONG t0 = perf_now();
        const RenderTarget *rt = g_textures[s] ? rt_find(g_textures[s]->Resource.Data) : NULL;
        IDirect3DBaseTexture9 *t;
        if (rt) { /* drawn by the game into a render target: sample the host copy */
            t = (IDirect3DBaseTexture9 *)rt->tex;
            if (rt->linear) {
                scale[0] = 1.0f / (float)rt->w;
                scale[1] = 1.0f / (float)rt->h;
            }
        }
        else {
            t = g_textures[s] ? tex_get((IDirect3DDevice9 *)g_dev, g_textures[s], scale) : NULL;
        }
        perf_end(PERF_TEX, t0);
        IDirect3DDevice9Ex_SetTexture(g_dev, s, t);
        g_vconst[VSH_C_TEXSCALE + s][0] = scale[0];
        g_vconst[VSH_C_TEXSCALE + s][1] = scale[1];
        g_vconst[VSH_C_TEXSCALE + s][2] = g_vconst[VSH_C_TEXSCALE + s][3] = 1.0f;
        if (s == 0 && !t)
            g_untextured++;
    }
    apply_texture_stages();
    /* Pixel shader: register combiners when the game set one, else fixed-function stages. */
    {
        IDirect3DPixelShader9 *ps = NULL;
        if (g_cur_ps & 1) {
            unsigned i = g_cur_ps >> 1;
            if (i < g_ps_count) {
                unsigned bound = 0, v;
                PsVariant *pv;
                for (unsigned k = 0; k < 4; k++) {
                    const XD3DPixelContainer *t = g_textures[k];
                    if (t)
                        bound |= (tex_is_depth(t) ? PSH_BOUND_DEPTH
                                  : (t->Format & XFMT_CUBEMAP) ? PSH_BOUND_CUBE
                                  : XFMT_DIMENSION(t->Format) == 3 ? PSH_BOUND_VOLUME : PSH_BOUND_2D) << (2 * k);
                }
                for (v = 0; v < PS_VARIANTS && g_ps_var[i][v].state && g_ps_var[i][v].key != bound; v++)
                    ;
                if (v == PS_VARIANTS)
                    v = PS_VARIANTS - 1; /* full: recompile into the last slot */
                pv = &g_ps_var[i][v];
                if (!pv->state || pv->key != bound) {
                    static char hlsl[32 * 1024];
                    LONGLONG t0;
                    if (pv->host)
                        IDirect3DPixelShader9_Release(pv->host);
                    pv->key = bound;
                    t0 = perf_now();
                    pv->host = psh_compile((IDirect3DDevice9 *)g_dev, g_ps_def[i], bound, i);
                    perf_end(PERF_PSC, t0);
                    pv->state = pv->host ? 1 : 2;
                    if (psh_to_hlsl(g_ps_def[i], bound, hlsl, sizeof hlsl))
                        log_printf("psh: shader %u (bound %02X) first used at frame %lu (%s):\n%s\n", i, bound,
                                   g_frame, pv->host ? "ok" : "FAILED", hlsl);
                }
                ps = pv->state == 1 ? pv->host : NULL;
                if (ps) {
                    float (*pc)[4] = g_ps_live;
                    IDirect3DDevice9Ex_SetPixelShaderConstantF(g_dev, 0, pc[0], 18);
                    if (g_capture_log && g_frame == g_capture_frame) { /* captures: what each draw gets */
                        const DWORD *def = g_ps_def[i];
                        fprintf(g_capture_log, "      ps %u: count %08lX c0map %08lX c1map %08lX fcmap %08lX, constants:",
                                i, def[53], def[57], def[58], def[59]);
                        for (unsigned k = 0; k < 18; k++)
                            fprintf(g_capture_log, " c%u(%.2f %.2f %.2f %.2f)", k, pc[k][0], pc[k][1], pc[k][2],
                                    pc[k][3]);
                        fprintf(g_capture_log, "\n");
                    }
                }
            }
        }
        IDirect3DDevice9Ex_SetPixelShader(g_dev, ps);
        /* A fixed-function stage with no Xbox texture whose operation reads the texture is switched
         * off, so the colour passes through (the vertex colour at stage 0) and later stages are off
         * too. That is the one rule that fits every JSRF case: the shadow quad (MODULATE texture x
         * diffuse, black at alpha 0.5) darkens, the cutscene sky strip keeps its vertex colours,
         * and untextured cel-outline hulls stay their own colour. Reading the empty stage as
         * D3D9's default made the quad and sky vanish, as white made outlines white and covered
         * the speech bubbles, and as opaque black blacked out the cutscene backgrounds. */
        if (!ps) {
            for (DWORD s2 = 0; s2 < 4; s2++) {
                const DWORD *t = XTEXTURE_STATE[s2];
                int reads_texture = (t[13] & 0xF) == D3DTA_TEXTURE || (t[14] & 0xF) == D3DTA_TEXTURE ||
                                    (t[15] & 0xF) == D3DTA_TEXTURE || (t[17] & 0xF) == D3DTA_TEXTURE ||
                                    (t[18] & 0xF) == D3DTA_TEXTURE || (t[19] & 0xF) == D3DTA_TEXTURE;
                if (!g_textures[s2] && reads_texture) {
                    IDirect3DDevice9Ex_SetTextureStageState(g_dev, s2, D3DTSS_COLOROP, D3DTOP_DISABLE);
                    IDirect3DDevice9Ex_SetTextureStageState(g_dev, s2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                    break; /* D3D9 ignores every stage after a disabled one */
                }
            }
        }
    }
    IDirect3DDevice9Ex_SetVertexShaderConstantF(g_dev, 0, g_vconst[0], VSH_CONSTANTS);
    if (!(g_cur_vs & 1))
        apply_lazy_states();
    if ((g_cur_vs & 1) && g_rs[D3DRS_STENCILENABLE] && g_rs[D3DRS_STENCILPASS] >= D3DSTENCILOP_INCRSAT) {
        static BYTE seen[MAX_SHADERS];
        unsigned i = g_cur_vs >> 1;
        if (i < MAX_SHADERS && !seen[i]) { /* stencil-counting programs (shadow volumes) */
            seen[i] = 1;
            log_printf("d3d: vertex program %u first counts stencil at frame %lu\n", i, g_frame);
        }
    }
    draw_log();
    g_dl_verts = NULL;
    g_draws++;
    g_frame_draws++;
    return 1;
}

/* Xbox primitive type -> D3D9. Quad lists become indexed triangle lists (*quads = 1). */
static int convert_prim(DWORD xprim, UINT count, D3DPRIMITIVETYPE *pt, UINT *prims, int *quads)
{
    *quads = 0;
    switch (xprim) {
    case 1: *pt = D3DPT_POINTLIST; *prims = count; break;
    case 2: *pt = D3DPT_LINELIST; *prims = count / 2; break;
    case 3: /* line loop: closing segment missing for now */
    case 4: *pt = D3DPT_LINESTRIP; *prims = count > 1 ? count - 1 : 0; break;
    case 5: *pt = D3DPT_TRIANGLELIST; *prims = count / 3; break;
    case 6: case 9: *pt = D3DPT_TRIANGLESTRIP; *prims = count > 2 ? count - 2 : 0; break;
    case 7: case 10: *pt = D3DPT_TRIANGLEFAN; *prims = count > 2 ? count - 2 : 0; break;
    case 8: *pt = D3DPT_TRIANGLELIST; *prims = count / 4 * 2; *quads = 1; break;
    default: return 0;
    }
    return *prims > 0;
}

static WORD g_quad_idx[65536 / 4 * 6];

/* Byte offset of texture coordinate set `set` in an FVF vertex, or -1. */
static int fvf_texcoord_offset(DWORD fvf, unsigned set)
{
    int off;
    unsigned n = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    static const int size[4] = {8, 12, 16, 4}; /* D3DFVF_TEXCOORDSIZE2, 3, 4, 1 */
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: off = 12; break;
    case D3DFVF_XYZRHW: off = 16; break;
    case D3DFVF_XYZB1: off = 16; break;
    case D3DFVF_XYZB2: off = 20; break;
    case D3DFVF_XYZB3: off = 24; break;
    case D3DFVF_XYZB4: off = 28; break;
    case D3DFVF_XYZB5: off = 32; break;
    default: return -1;
    }
    if (fvf & D3DFVF_NORMAL)
        off += 12;
    if (fvf & D3DFVF_PSIZE)
        off += 4;
    if (fvf & D3DFVF_DIFFUSE)
        off += 4;
    if (fvf & D3DFVF_SPECULAR)
        off += 4;
    if (set >= n)
        return -1;
    for (unsigned i = 0; i < set; i++)
        off += size[(fvf >> (16 + 2 * i)) & 3];
    return off;
}

/* Fixed-function vertices that need converting for the host, copied first..last (the returned
 * pointer is used with the same indices):
 * - pre-transformed (XYZRHW) positions are in Xbox screen pixels: x and y scaled to the render
 *   resolution. Their z is already 0..1, as on the PC (scaling it as depth-buffer units hid the
 *   speech bubbles behind the letterbox bars);
 * - stages sampling a linear (texel-addressed) texture, render targets included, have their
 *   texture coordinates scaled to 0..1. Vertex programs get this through VSH_C_TEXSCALE; JSRF's
 *   boost draws its render target onto the screen with a fixed-function quad in texels, which
 *   read a single edge texel (a flat grey screen) until this. */
/* Widescreen, 2D. The game's 640x480 screen space is stretched over a wide screen, while its
 * perspective 3D is widened instead (widescreen_projection). UI drawn both ways (a dialogue box
 * in 3D-projected pieces, its text in screen pixels; the game's menus) then disagreed, the text
 * running out of its box. So 2D keeps 4:3 proportions in the middle of the screen, where
 * projected UI lands too: each 2D draw's x is squeezed by 1/k around the centre, unless it
 * spans the whole width (fades, cutscene bars, the boost and shadow overlays), which stays
 * stretched. 2D here: pre-transformed vertices (screen pixels, squeezed on the CPU), 2D vertex
 * programs (screen pixels in, the c[197] epilogue constant) and fixed-function geometry under
 * an orthographic projection (a narrowed host projection). Render targets aren't touched. */
static float g_squeeze = 1.0f; /* this draw's squeeze for pre-transformed vertices (1 = none) */
static float g_pivot = 0.5f;   /* ...around this fraction of the width: 0.5 the centre, 0 or 1 an edge */

/* The gameplay HUD on the right (health bar, can count, tag counter, boost gauge, the spray can)
 * is squeezed around the right edge instead of the centre, so it sits in the corner of a wide
 * screen as it does at 4:3 (setting HudEdges). It is told apart by the game code that draws it:
 * every HUD draw, flat or 3D, runs under the HUD layer at 0x1367B; menus, the map, character
 * select and speech bubbles are drawn under another layer (0x1269B / 0x1386F) and stay centred,
 * so text keeps to its bubbles (captures of each, 10-29). Found by its return address on the
 * stack. The left side stays centred: the trick display there is drawn partly by the HUD layer
 * and partly by the shared sprite list, which can't be told apart from cutscene text, and moving
 * only part of it split "20 1 x 30" apart. */
static int on_stack(DWORD ret_a, DWORD ret_b)
{
    static __declspec(thread) const DWORD *stack_top; /* the end of this thread's stack region */
    const DWORD *sp = (const DWORD *)_AddressOfReturnAddress(), *end = sp + 4096; /* the 3D can sits deep */
    if (!stack_top) {
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery(sp, &mi, sizeof mi))
            stack_top = (const DWORD *)((const BYTE *)mi.BaseAddress + mi.RegionSize);
    }
    if (stack_top && end > stack_top)
        end = stack_top;
    for (; sp < end; sp++)
        if (*sp == ret_a || *sp == ret_b)
            return 1;
    return 0;
}

static int hud_draw(void)
{
    return on_stack(0x0001367B, 0x0001367B);
}

/* The icon beside the can count (the yellow square) is drawn by a 3D vertex program, which
 * decides per vertex, so only that piece is shifted, by the code that draws it (0x10A76B and
 * 0x10A83F): the HUD's centre pieces drawn the same way (the direction marker, from 0x102F67)
 * straddle the middle and were stretched. */
static int hud_icon_draw(void)
{
    return on_stack(0x0010A76B, 0x0010A83F);
}

/* The pivot for a HUD piece spanning mn..mx of a width from lo to hi: the right edge for the
 * right side's HUD, else the centre. */
static float hud_pivot(float mn, float mx, float lo, float hi)
{
    (void)mx;
    if (!g_settings.hud_edges || mn < (lo + hi) * 0.5f || !hud_draw())
        return 0.5f;
    return 1.0f;
}

static void squeeze_2d(const BYTE *verts, UINT first, UINT last, UINT stride)
{
    float k = widescreen_k(), mn = 1e30f, mx = -1e30f, sq, c[4] = {1.0f, 0.0f, 0.0f, 1.0f}; /* 2D scale, 2D HUD, 3D HUD shift, - */
    int fvf = !(g_cur_vs & 1), rhw = fvf && (g_cur_vs & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    g_squeeze = 1.0f;
    g_pivot = 0.5f;
    if (!fvf) { /* 2D programs get the constant (reset for everything else) */
        unsigned i = g_cur_vs >> 1;
        if (k > 1.01f && !g_cur_rt && verts && i < MAX_SHADERS && g_vs_2d[i]) {
            for (UINT v = first; v <= last; v++) {
                float x = *(const float *)(verts + (SIZE_T)v * stride);
                mn = x < mn ? x : mn;
                mx = x > mx ? x : mx;
            }
            if (!(mn <= 2.0f && mx >= (float)g_xw - 2.0f)) {
                c[0] = 1.0f / k;
                c[1] = hud_pivot(mn, mx, 0.0f, (float)g_xw) > 0.75f ? 1.0f : 0.0f; /* right-side HUD: pivot on the edge */
            }
        }
        else if (k > 1.01f && !g_cur_rt && i < MAX_SHADERS && !g_vs_2d[i] && g_settings.hud_edges && hud_icon_draw())
            c[2] = 1.0f - 1.0f / k; /* a 3D HUD piece (the icon by the can count): the shader shifts it */
        IDirect3DDevice9Ex_SetVertexShaderConstantF(g_dev, VSH_C_2D, c, 1);
        return;
    }
    if (rhw) {
        if (k <= 1.01f || g_cur_rt || !verts)
            return;
        for (UINT v = first; v <= last; v++) {
            float x = *(const float *)(verts + (SIZE_T)v * stride);
            mn = x < mn ? x : mn;
            mx = x > mx ? x : mx;
        }
        if (!(mn <= 2.0f && mx >= (float)g_xw - 2.0f)) {
            g_squeeze = 1.0f / k;
            g_pivot = hud_pivot(mn, mx, 0.0f, (float)g_xw);
        }
        return;
    }
    { /* fixed-function geometry: only orthographic projections are 2D */
        D3DMATRIX p = g_xform[1], wvp;
        const D3DMATRIX *w = &g_xform[6], *vw = &g_xform[0];
        float pivot = 0.0f; /* clip-space x: 0 the centre, -1 or 1 an edge (HUD) */
        if (!(g_xform_set & 2))
            return;
        sq = 1.0f;
        if (k > 1.01f && !g_cur_rt && verts && p._34 == 0.0f && p._44 != 0.0f) {
            /* clip-space x extent through world, view and projection */
            D3DMATRIX wv;
            for (int r = 0; r < 4; r++)
                for (int col = 0; col < 4; col++) {
                    wv.m[r][col] = w->m[r][0] * vw->m[0][col] + w->m[r][1] * vw->m[1][col] +
                                   w->m[r][2] * vw->m[2][col] + w->m[r][3] * vw->m[3][col];
                }
            for (int r = 0; r < 4; r++)
                for (int col = 0; col < 4; col++)
                    wvp.m[r][col] = wv.m[r][0] * p.m[0][col] + wv.m[r][1] * p.m[1][col] +
                                    wv.m[r][2] * p.m[2][col] + wv.m[r][3] * p.m[3][col];
            for (UINT v = first; v <= last; v++) {
                const float *q = (const float *)(verts + (SIZE_T)v * stride);
                float cx = q[0] * wvp._11 + q[1] * wvp._21 + q[2] * wvp._31 + wvp._41;
                float cw = q[0] * wvp._14 + q[1] * wvp._24 + q[2] * wvp._34 + wvp._44;
                float x = cw != 0.0f ? cx / cw : cx;
                mn = x < mn ? x : mn;
                mx = x > mx ? x : mx;
            }
            if (!(mn <= -0.98f && mx >= 0.98f)) {
                sq = 1.0f / k;
                pivot = hud_pivot(mn, mx, -1.0f, 1.0f) * 2.0f - 1.0f; /* in clip space */
            }
        }
        else if (k > 1.01f && !g_cur_rt && verts && p._34 != 0.0f && g_settings.hud_edges && hud_draw()) {
            /* A 3D HUD piece (the spray can) in the perspective view: widescreen keeps its shape
             * but draws it k times nearer the centre. On the right side, move it as far as the
             * edge squeeze moves the flat pieces beside it: x' = x + (1 - 1/k) in clip space. */
            D3DMATRIX wv;
            float sum = 0.0f;
            UINT n = 0;
            for (int r = 0; r < 4; r++)
                for (int col = 0; col < 4; col++)
                    wv.m[r][col] = w->m[r][0] * vw->m[0][col] + w->m[r][1] * vw->m[1][col] +
                                   w->m[r][2] * vw->m[2][col] + w->m[r][3] * vw->m[3][col];
            for (UINT v = first; v <= last; v++) { /* where its vertices land: the offset is in them */
                const float *q = (const float *)(verts + (SIZE_T)v * stride);
                float ex = q[0] * wv._11 + q[1] * wv._21 + q[2] * wv._31 + wv._41;
                float ey = q[0] * wv._12 + q[1] * wv._22 + q[2] * wv._32 + wv._42;
                float ez = q[0] * wv._13 + q[1] * wv._23 + q[2] * wv._33 + wv._43;
                float cx = ex * p._11 + ey * p._21 + ez * p._31 + p._41;
                float cw = ex * p._14 + ey * p._24 + ez * p._34 + p._44;
                if (cw > 0.0f) {
                    sum += cx / cw;
                    n++;
                }
            }
            mn = mx = n ? sum / (float)n : 0.0f; /* the piece's centre, clip x (for the capture log) */
            if (n && mn > 0.0f) {
                float shift = 1.0f - 1.0f / k;
                p._11 += shift * p._14;
                p._21 += shift * p._24;
                p._31 += shift * p._34;
                p._41 += shift * p._44;
            }
        }
        if (sq != 1.0f) { /* x' = (x - pivot) * sq + pivot, in clip space */
            p._11 = p._11 * sq + pivot * (1.0f - sq) * p._14;
            p._21 = p._21 * sq + pivot * (1.0f - sq) * p._24;
            p._31 = p._31 * sq + pivot * (1.0f - sq) * p._34;
            p._41 = p._41 * sq + pivot * (1.0f - sq) * p._44;
        }
        if (g_capture_log && g_frame == g_capture_frame && (p._34 == 0.0f || hud_draw())) /* where 2D and HUD land */
            fprintf(g_capture_log, "      squeeze: %s, x %.3f..%.3f, scale %.3f, pivot %.2f, hud %d, projection _31 %.3f _34 %.3f\n",
                    p._34 == 0.0f ? "orthographic" : "perspective", mn, mx, sq, pivot,
                    g_settings.hud_edges && hud_draw(), p._31, p._34);
        IDirect3DDevice9Ex_SetTransform(g_dev, D3DTS_PROJECTION, &p);
    }
}

static const BYTE *fix_ff_vertices(const BYTE *verts, UINT first, UINT last, UINT stride)
{
    static BYTE *buf;
    static size_t cap;
    size_t need = (size_t)(last + 1) * stride;
    int rhw, nscale = 0, off[4];
    float su[4], sv[4];
    if (g_cur_vs & 1)
        return verts;
    rhw = (g_cur_vs & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW &&
          (g_tsx != 1.0f || g_tsy != 1.0f || g_squeeze != 1.0f || (g_ms_type && !g_cur_rt));
    for (unsigned s = 0; s < 4; s++) {
        const XD3DPixelContainer *t = g_textures[s];
        const RenderTarget *r = t ? rt_find(t->Resource.Data) : NULL;
        UINT w = 0, h = 0;
        if (r && r->linear) {
            w = r->w;
            h = r->h;
        }
        else if (t && !r && t->Size) {
            w = XSIZE_WIDTH(t->Size);
            h = XSIZE_HEIGHT(t->Size);
        }
        if (!w || !h)
            continue;
        off[nscale] = fvf_texcoord_offset(g_cur_vs, XTEXTURE_STATE[s][28] & 0xFFFF);
        if (off[nscale] < 0 || off[nscale] + 8 > (int)stride)
            continue;
        su[nscale] = 1.0f / (float)w;
        sv[nscale] = 1.0f / (float)h;
        nscale++;
    }
    if (!rhw && !nscale)
        return verts;
    if (need > cap) {
        BYTE *n = (BYTE *)realloc(buf, need);
        if (!n)
            return verts;
        buf = n;
        cap = need;
    }
    for (UINT i = first; i <= last; i++) {
        BYTE *v = buf + (size_t)i * stride;
        memcpy(v, verts + (size_t)i * stride, stride);
        if (rhw) {
            float at = (float)g_hw * g_pivot; /* squeezing only happens on the back buffer */
            /* With anti-aliasing, edges on pixel boundaries (D3D9 puts pixel centres there) left the
             * top and left edge pixels half covered: a seam along the cutscene bars at the screen's
             * top and left. Half a pixel up and left covers them, as Direct3D 9 expects. */
            float half_px = g_ms_type && !g_cur_rt ? 0.5f : 0.0f;
            ((float *)v)[0] = (g_squeeze != 1.0f ? (((float *)v)[0] * g_tsx - at) * g_squeeze + at
                                                 : ((float *)v)[0] * g_tsx) - half_px;
            ((float *)v)[1] = ((float *)v)[1] * g_tsy - half_px;
        }
        for (int k = 0; k < nscale; k++) {
            float *uv = (float *)(v + off[k]);
            uv[0] *= su[k];
            uv[1] *= sv[k];
        }
    }
    return buf;
}

/* Textured quads (2D art: text boxes, "Press Start", icons) drawn above 640x480: their
 * outermost pixels sample up to half a texel past the quad's edge, into the next image of the
 * game's texture atlases (a thin coloured line along the edge; at 640x480 the Xbox samples
 * texel centres there). Pull each quad's texture coordinates in by half a texel. Vertex
 * programs whose texcoord 0 is a copy of an input register only, a single quad or a batch of
 * them (`group` vertices each: 6 for triangle-list pairs, 4 for quad lists); a copy is drawn,
 * the game's vertices are untouched. */
static const BYTE *inset_quad_texcoords(const BYTE *verts, UINT first, UINT last, UINT stride, UINT group)
{
    static BYTE copy[96 * 128];
    const XD3DPixelContainer *t = g_textures[0];
    unsigned i = g_cur_vs >> 1, n = last - first + 1, changed = 0;
    int off;
    float du, dv;
    if (!group && n >= 3 && n <= 6)
        group = n; /* one quad (fan, strip, or indexed) */
    if (!(g_cur_vs & 1) || !t || !group || n % group || n > 96 || stride > 128 || g_tsx <= 1.01f ||
        i >= MAX_SHADERS || (off = g_vs_t0_off[i]) < 0 || (UINT)off + 8 > stride)
        return verts;
    if (t->Size) /* linear: texel coordinates */
        du = dv = 0.5f;
    else {
        du = 0.5f / (float)(1u << XFMT_USIZE(t->Format));
        dv = 0.5f / (float)(1u << XFMT_VSIZE(t->Format));
    }
    memcpy(copy, verts + (SIZE_T)first * stride, (SIZE_T)n * stride);
    for (unsigned q = 0; q < n; q += group) {
        float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
        for (unsigned k = q; k < q + group; k++) {
            const float *uv = (const float *)(copy + k * stride + off);
            u0 = uv[0] < u0 ? uv[0] : u0;
            u1 = uv[0] > u1 ? uv[0] : u1;
            v0 = uv[1] < v0 ? uv[1] : v0;
            v1 = uv[1] > v1 ? uv[1] : v1;
        }
        if (u1 - u0 < 4 * du || v1 - v0 < 4 * dv) /* not a whole image (or degenerate) */
            continue;
        for (unsigned k = q; k < q + group; k++) {
            float *uv = (float *)(copy + k * stride + off);
            uv[0] += uv[0] == u0 ? du : uv[0] == u1 ? -du : 0;
            uv[1] += uv[1] == v0 ? dv : uv[1] == v1 ? -dv : 0;
        }
        changed++;
    }
    if (g_capture_log && g_frame == g_capture_frame)
        fprintf(g_capture_log, "      inset: %u of %u quads, half texel %.4f,%.4f, texcoord offset %d\n", changed,
                n / group, (double)du, (double)dv, off);
    return changed ? copy - (SIZE_T)first * stride : verts;
}

/* Draw `count` vertices from `verts` (optionally indexed by 16-bit `idx`). */
/* The game functions behind a draw: return addresses into game code (0x11000-0x13B010) found on
 * the stack, each just after a CALL (the game has no frame pointers to walk). For captures. */

static int after_call(DWORD a)
{
    const BYTE *p = (const BYTE *)(UINT_PTR)a;
    return p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) ||
           (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10);
}

static void find_game_callers(const void *frame)
{
    const DWORD *sp = (const DWORD *)frame;
    unsigned n = 0;
    memset(g_dl_callers, 0, sizeof g_dl_callers);
    for (unsigned i = 0; i < 2048 && n < 8; i++) {
        DWORD v = sp[i];
        if (v >= 0x11005 && v < 0x13B010 && after_call(v) && (n == 0 || g_dl_callers[n - 1] != v))
            g_dl_callers[n++] = v;
    }
}

static void draw_up(DWORD xprim, UINT count, const BYTE *verts, UINT stride, const WORD *idx)
{
    D3DPRIMITIVETYPE pt;
    UINT prims, lo = 0, hi = 0;
    /* quad batches for inset_quad_texcoords: triangle-list pairs, or quad lists */
    UINT group = idx ? 0 : (xprim == 5 && count % 6 == 0) ? 6 : (xprim == 8 && count % 4 == 0) ? 4 : 0;
    int quads;
    LONGLONG t0 = perf_now();
    g_dl_prim = xprim;
    g_dl_count = count;
    warning_check_draw();
    if (g_capture_log && g_frame == g_capture_frame)
        find_game_callers(_AddressOfReturnAddress());
    if (stride <= 64) { /* captures list the first vertices (in buffer order) */
        g_dl_verts = verts;
        g_dl_stride = stride;
    }
    if (!verts || !stride || !convert_prim(xprim, count, &pt, &prims, &quads) || !prepare_draw())
        return;
    { /* TEMPORARY shadow debugging: REJET_SHADOWDEBUG=1 draws the stencil-tested darkening quad red
       * without the stencil test; =2 keeps the stencil test but makes it red */
        static int dbg = -1;
        if (dbg < 0) {
            char env[4] = "";
            dbg = GetEnvironmentVariableA("REJET_SHADOWDEBUG", env, sizeof env) ? env[0] - '0' : 0;
        }
        if (dbg == 3 && g_cur_vs == 0x42 && g_rs[D3DRS_STENCILENABLE]) { /* =3: show the volumes */
            IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_STENCILENABLE, FALSE);
            IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_ZENABLE, FALSE);
            IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, FALSE);
            IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_FILLMODE, D3DFILL_WIREFRAME);
            IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
            IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_TEXTUREFACTOR, 0xFFFF0000);
            IDirect3DDevice9Ex_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice9Ex_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        }
        if (dbg && dbg < 3 && !(g_cur_vs & 1) && g_cur_vs == 0x1C4 && !g_textures[0] && g_rs[D3DRS_STENCILENABLE]) {
            if (dbg == 1)
                IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_STENCILENABLE, FALSE);
            IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, FALSE);
            IDirect3DDevice9Ex_SetRenderState(g_dev, D3DRS_TEXTUREFACTOR, 0xFFFF0000);
            IDirect3DDevice9Ex_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            IDirect3DDevice9Ex_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
            IDirect3DDevice9Ex_SetPixelShader(g_dev, NULL);
        }
    }
    if (quads) {
        UINT q = count / 4;
        if (q > 65536 / 4)
            return;
        for (UINT i = 0; i < q; i++) {
            WORD *d = g_quad_idx + i * 6;
            WORD a = idx ? idx[i * 4] : (WORD)(i * 4), b = idx ? idx[i * 4 + 1] : (WORD)(i * 4 + 1);
            WORD c = idx ? idx[i * 4 + 2] : (WORD)(i * 4 + 2), e = idx ? idx[i * 4 + 3] : (WORD)(i * 4 + 3);
            d[0] = a; d[1] = b; d[2] = c;
            d[3] = a; d[4] = c; d[5] = e;
        }
        idx = g_quad_idx;
        count = q * 6;
    }
    if (idx) {
        lo = 0xFFFF;
        for (UINT i = 0; i < count; i++) {
            if (idx[i] < lo)
                lo = idx[i];
            if (idx[i] > hi)
                hi = idx[i];
        }
        squeeze_2d(verts, lo, hi, stride);
        verts = inset_quad_texcoords(verts, lo, hi, stride, group);
        verts = fix_ff_vertices(verts, lo, hi, stride);
        IDirect3DDevice9Ex_DrawIndexedPrimitiveUP(g_dev, pt, lo, hi - lo + 1, prims, idx, D3DFMT_INDEX16, verts, stride);
    }
    else {
        squeeze_2d(verts, 0, count - 1, stride);
        verts = inset_quad_texcoords(verts, 0, count - 1, stride, group);
        verts = fix_ff_vertices(verts, 0, count - 1, stride);
        IDirect3DDevice9Ex_DrawPrimitiveUP(g_dev, pt, prims, verts, stride);
    }
    perf_end(PERF_DRAW, t0);
}

static void XBAPI hle_D3DDevice_DrawVerticesUP(DWORD PrimitiveType, UINT VertexCount, const void *pData, UINT Stride)
{
    draw_up(PrimitiveType, VertexCount, (const BYTE *)pData, Stride, NULL);
}

static void XBAPI hle_D3DDevice_DrawIndexedVerticesUP(DWORD PrimitiveType, UINT VertexCount, const WORD *pIndexData,
                                                      const void *pVertexData, UINT Stride)
{
    draw_up(PrimitiveType, VertexCount, (const BYTE *)pVertexData, Stride, pIndexData);
}

static const BYTE *stream_data(void)
{
    if (!g_stream_vb || !g_stream_vb->Data || !g_stream_stride) {
        SURVEY(5)
        log_printf("survey: draw from stream 0 skipped: vb %p stride %u\n", (void *)g_stream_vb, g_stream_stride);
        return NULL;
    }
    return (const BYTE *)(UINT_PTR)g_stream_vb->Data;
}

/* DrawVertices(type, start vertex, count): non-indexed, from stream 0. */
static void XBAPI hle_D3DDevice_DrawVertices(DWORD PrimitiveType, UINT StartVertex, UINT VertexCount)
{
    const BYTE *v = stream_data();
    if (v)
        draw_up(PrimitiveType, VertexCount, v + (SIZE_T)StartVertex * g_stream_stride, g_stream_stride, NULL);
}

/* DrawIndexedVertices(type, count, indices): indices already offset by the engine; vertices
 * from stream 0 with SetIndices' base vertex. */
static void XBAPI hle_D3DDevice_DrawIndexedVertices(DWORD PrimitiveType, UINT VertexCount, const WORD *pIndexData)
{
    const BYTE *v = stream_data();
    if (v && pIndexData)
        draw_up(PrimitiveType, VertexCount, v + (SIZE_T)g_base_vertex * g_stream_stride, g_stream_stride, pIndexData);
}

/* ------------------------------------------------------------------ immediate mode */

/* Begin(type) / SetVertexData*(register, ...) / End(): the engine draws full-screen fades
 * (0x151DA0, a strip with diffuse colour and stencil) and debug text (0x1522B0, textured quads)
 * this way, with FVF 0x1C4 (XYZRHW, diffuse, specular, one texcoord). Registers are the Xbox
 * D3DVSDE_* numbers: 0 position, 2 normal, 3 diffuse, 4 specular, 9..12 texcoords; -1 (VERTEX)
 * sets the position and emits a vertex with the current values of the others. At End the
 * vertices are packed in the current FVF's layout and drawn like DrawVerticesUP. */
#define IMM_MAX 4096
static float g_imm_reg[16][4];
static float g_imm_vtx[IMM_MAX][16][4];
static UINT g_imm_count;
static DWORD g_imm_prim;

static void XBAPI hle_D3DDevice_Begin(DWORD PrimitiveType)
{
    g_imm_prim = PrimitiveType;
    g_imm_count = 0;
}

static void imm_set(INT Register, float a, float b, float c, float d)
{
    int r = Register < 0 ? 0 : Register;
    if (r >= 16)
        return;
    g_imm_reg[r][0] = a;
    g_imm_reg[r][1] = b;
    g_imm_reg[r][2] = c;
    g_imm_reg[r][3] = d;
    if (Register < 0 && g_imm_count < IMM_MAX) /* D3DVSDE_VERTEX: emit */
        memcpy(g_imm_vtx[g_imm_count++], g_imm_reg, sizeof g_imm_reg);
}

static void XBAPI hle_D3DDevice_SetVertexData2f(INT Register, float a, float b)
{
    imm_set(Register, a, b, 0.0f, 1.0f);
}

static void XBAPI hle_D3DDevice_SetVertexData4f(INT Register, float a, float b, float c, float d)
{
    imm_set(Register, a, b, c, d);
}

static DWORD imm_color(const float *v) /* (r, g, b, a) in 0..1 -> D3DCOLOR */
{
    DWORD out = 0;
    static const int shift[4] = {16, 8, 0, 24};
    for (int i = 0; i < 4; i++) {
        float x = v[i] < 0.0f ? 0.0f : v[i] > 1.0f ? 1.0f : v[i];
        out |= (DWORD)(x * 255.0f + 0.5f) << shift[i];
    }
    return out;
}

static void XBAPI hle_D3DDevice_End(void)
{
    static BYTE buf[IMM_MAX * 64];
    DWORD fvf = g_cur_vs;
    UINT stride = 0, ntex;
    BYTE *p = buf;
    if (!g_imm_count)
        return;
    if (fvf & 1) { /* a vertex program: not used this way by JSRF so far */
        TODO_ONCE("immediate mode with a vertex program");
        return;
    }
    ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (UINT i = 0; i < g_imm_count; i++) {
        float (*v)[4] = g_imm_vtx[i];
        BYTE *start = p;
        switch (fvf & D3DFVF_POSITION_MASK) {
        case D3DFVF_XYZRHW: memcpy(p, v[0], 16); p += 16; break;
        case D3DFVF_XYZ: memcpy(p, v[0], 12); p += 12; break;
        default: TODO_ONCE("immediate mode with this FVF position type"); return;
        }
        if (fvf & D3DFVF_NORMAL) {
            memcpy(p, v[2], 12);
            p += 12;
        }
        if (fvf & D3DFVF_DIFFUSE) {
            *(DWORD *)p = imm_color(v[3]);
            p += 4;
        }
        if (fvf & D3DFVF_SPECULAR) {
            *(DWORD *)p = imm_color(v[4]);
            p += 4;
        }
        for (UINT t = 0; t < ntex && t < 4; t++) {
            memcpy(p, v[9 + t], 8);
            p += 8;
        }
        stride = (UINT)(p - start);
    }
    draw_up(g_imm_prim, g_imm_count, buf, stride, NULL);
    g_imm_count = 0;
}

/* ------------------------------------------------------------------ not yet implemented */

/* Typed stubs: each pops exactly the argument bytes the original does (config/d3d_abi.csv),
 * logs its first call and returns `ret` (S_OK for HRESULTs). */
#pragma warning(push)
#pragma warning(disable : 4100) /* unreferenced parameters */
#define STUB(name, ret, ...)                                  \
    static DWORD XBAPI stub_##name(__VA_ARGS__)              \
    {                                                         \
        TODO_ONCE(#name);                                     \
        return ret;                                           \
    }
#define D DWORD
STUB(SetGammaRamp, 0, D a, D b)
STUB(GetGammaRamp, 0, D a)
STUB(GetTile, 0, D a, D b)
STUB(SetTile, 0, D a, D b)
STUB(DisplayQuery_18E120, 0, D a)
STUB(GetTexture2, 0, D a, D b)
#undef D
#undef STUB
#pragma warning(pop)

/* Render states ReJet doesn't apply yet, one function each so the log names them: the first
 * calls of each, with the value and caller. */
#define RS_STUB(name)                                                                              \
    static void XBAPI stub_rs_##name(DWORD v)                                                      \
    {                                                                                              \
        static LONG n;                                                                             \
        if (InterlockedIncrement(&n) <= 4)                                                         \
            log_printf("d3d: SetRenderState_" #name "(%lX) not applied (from %08X)\n", v,          \
                       (unsigned)(UINT_PTR)_ReturnAddress());                                       \
    }
RS_STUB(MultiSampleMode)
RS_STUB(MultiSampleRenderTargetMode)
RS_STUB(PSTextureModes)
RS_STUB(EdgeAntiAlias)
RS_STUB(ShadowFunc)
RS_STUB(LineWidth)
RS_STUB(Dxt1NoiseEnable)
RS_STUB(LogicOp)
RS_STUB(BackFillMode)
RS_STUB(TwoSidedLighting)
RS_STUB(VertexBlend)
RS_STUB(YuvEnable)
RS_STUB(OcclusionCullEnable)
RS_STUB(StencilCullEnable)
RS_STUB(RopZCmpAlwaysRead)
RS_STUB(RopZRead)
RS_STUB(DoNotCullUncompressed)
RS_STUB(MultiSampleAntiAlias)
RS_STUB(MultiSampleMask)
#undef RS_STUB

/* ------------------------------------------------------------------ patch table */

#define P(name, impl) {XDK_D3D8_##name, (void *)(impl), #name}
const HlePatch g_d3d_patches[] = {
    /* device, frames */
    P(Direct3D_CreateDevice, hle_Direct3D_CreateDevice),
    P(D3DDevice_Reset, hle_D3DDevice_Reset),
    P(D3DDevice_AddRef, hle_D3DDevice_AddRef),
    P(D3DDevice_Release, hle_D3DDevice_Release),
    P(D3DDevice_Clear, hle_D3DDevice_Clear),
    P(D3DDevice_Swap, hle_D3DDevice_Swap),
    P(D3DDevice_BlockUntilVerticalBlank, hle_D3DDevice_BlockUntilVerticalBlank),
    P(D3DDevice_SetVerticalBlankCallback, hle_D3DDevice_SetVerticalBlankCallback),
    P(D3DDevice_GetBackBuffer, hle_D3DDevice_GetBackBuffer),
    P(D3DDevice_GetDepthStencilSurface, hle_D3DDevice_GetDepthStencilSurface),
    {0x0018E120u, (void *)stub_DisplayQuery_18E120, "D3D 0x18E120 (reads NV2A registers)"},
    /* resources */
    P(D3DDevice_CreateTexture, hle_D3DDevice_CreateTexture),
    P(D3DDevice_CreateVolumeTexture, hle_D3DDevice_CreateVolumeTexture),
    P(D3DDevice_CreateImageSurface, hle_D3DDevice_CreateImageSurface),
    P(D3DDevice_CreateVertexBuffer, hle_D3DDevice_CreateVertexBuffer),
    P(D3DDevice_CreateIndexBuffer, hle_D3DDevice_CreateIndexBuffer),
    P(D3DTexture_GetSurfaceLevel, hle_D3DTexture_GetSurfaceLevel),
    P(D3DTexture_LockRect, hle_D3DTexture_LockRect),
    P(D3DSurface_LockRect, hle_D3DSurface_LockRect),
    P(D3DVolumeTexture_LockBox, hle_D3DVolumeTexture_LockBox),
    P(D3DVertexBuffer_Lock, hle_D3DVertexBuffer_Lock),
    P(D3DSurface_GetDesc, hle_D3DSurface_GetDesc),
    /* two entries named Get2DSurfaceDesc: the engine's jmp stub 0x18F090 and the body, patched here */
    {0x00193A10u, (void *)hle_Get2DSurfaceDesc, "Get2DSurfaceDesc"},
    P(D3DResource_AddRef, hle_D3DResource_AddRef),
    P(D3DResource_Release, hle_D3DResource_Release),
    /* shaders */
    P(D3DDevice_CreateVertexShader, hle_D3DDevice_CreateVertexShader),
    P(D3DDevice_DeleteVertexShader, hle_D3DDevice_DeleteVertexShader),
    P(D3DDevice_SetVertexShader, hle_D3DDevice_SetVertexShader),
    P(D3DDevice_SetVertexShaderConstant, hle_D3DDevice_SetVertexShaderConstant),
    P(D3DDevice_SetShaderConstantMode, hle_D3DDevice_SetShaderConstantMode),
    P(D3DDevice_CreatePixelShader, hle_D3DDevice_CreatePixelShader),
    P(D3DDevice_DeletePixelShader, hle_D3DDevice_DeletePixelShader),
    P(D3DDevice_SetPixelShader, hle_D3DDevice_SetPixelShader),
    P(D3DDevice_SetPixelShaderConstant, hle_D3DDevice_SetPixelShaderConstant),
    /* transforms, lights, viewport, targets */
    P(D3DDevice_SetGammaRamp, stub_SetGammaRamp),
    P(D3DDevice_GetGammaRamp, stub_GetGammaRamp),
    P(D3DDevice_SetTransform, hle_D3DDevice_SetTransform),
    P(D3DDevice_GetTransform, hle_D3DDevice_GetTransform),
    P(D3DDevice_SetMaterial, hle_D3DDevice_SetMaterial),
    P(D3DDevice_SetLight, hle_D3DDevice_SetLight),
    P(D3DDevice_LightEnable, hle_D3DDevice_LightEnable),
    P(D3DDevice_SetViewport, hle_D3DDevice_SetViewport),
    P(D3DDevice_SetRenderTarget, hle_D3DDevice_SetRenderTarget),
    P(D3DDevice_CopyRects, hle_D3DDevice_CopyRects),
    P(D3DDevice_GetTile, stub_GetTile),
    P(D3DDevice_SetTile, stub_SetTile),
    /* textures and streams */
    P(D3DDevice_GetTexture2, stub_GetTexture2),
    P(D3DDevice_SetTexture, hle_D3DDevice_SetTexture),
    P(D3DDevice_SetIndices, hle_D3DDevice_SetIndices),
    P(D3DDevice_SetStreamSource, hle_D3DDevice_SetStreamSource),
    /* draws */
    P(D3DDevice_DrawVerticesUP, hle_D3DDevice_DrawVerticesUP),
    P(D3DDevice_DrawIndexedVerticesUP, hle_D3DDevice_DrawIndexedVerticesUP),
    P(D3DDevice_DrawVertices, hle_D3DDevice_DrawVertices),
    P(D3DDevice_DrawIndexedVertices, hle_D3DDevice_DrawIndexedVertices),
    P(D3DDevice_SetVertexData2f, hle_D3DDevice_SetVertexData2f),
    P(D3DDevice_SetVertexData4f, hle_D3DDevice_SetVertexData4f),
    P(D3DDevice_Begin, hle_D3DDevice_Begin),
    P(D3DDevice_End, hle_D3DDevice_End),
    /* render and texture state */
    P(D3DDevice_SetRenderState_Simple, hle_D3DDevice_SetRenderState_Simple),
    P(D3DDevice_SetRenderState_CullMode, hle_D3DDevice_SetRenderState_CullMode),
    P(D3DDevice_SetRenderState_FrontFace, hle_D3DDevice_SetRenderState_FrontFace),
    P(D3DDevice_SetRenderState_ZEnable, hle_D3DDevice_SetRenderState_ZEnable),
    P(D3DDevice_SetRenderState_FillMode, hle_D3DDevice_SetRenderState_FillMode),
    P(D3DDevice_SetRenderState_StencilEnable, hle_D3DDevice_SetRenderState_StencilEnable),
    P(D3DDevice_SetRenderState_StencilFail, hle_D3DDevice_SetRenderState_StencilFail),
    P(D3DDevice_SetRenderState_TextureFactor, hle_D3DDevice_SetRenderState_TextureFactor),
    P(D3DDevice_SetRenderState_FogColor, hle_D3DDevice_SetRenderState_FogColor),
    P(D3DDevice_SetRenderState_NormalizeNormals, hle_D3DDevice_SetRenderState_NormalizeNormals),
    P(D3DDevice_SetRenderState_MultiSampleMode, stub_rs_MultiSampleMode),
    P(D3DDevice_SetRenderState_MultiSampleRenderTargetMode, stub_rs_MultiSampleRenderTargetMode),
    P(D3DDevice_SetRenderState_PSTextureModes, stub_rs_PSTextureModes),
    P(D3DDevice_SetRenderState_EdgeAntiAlias, stub_rs_EdgeAntiAlias),
    P(D3DDevice_SetRenderState_ShadowFunc, stub_rs_ShadowFunc),
    P(D3DDevice_SetRenderState_LineWidth, stub_rs_LineWidth),
    P(D3DDevice_SetRenderState_Dxt1NoiseEnable, stub_rs_Dxt1NoiseEnable),
    P(D3DDevice_SetRenderState_ZBias, hle_D3DDevice_SetRenderState_ZBias),
    P(D3DDevice_SetRenderState_LogicOp, stub_rs_LogicOp),
    P(D3DDevice_SetRenderState_BackFillMode, stub_rs_BackFillMode),
    P(D3DDevice_SetRenderState_TwoSidedLighting, stub_rs_TwoSidedLighting),
    P(D3DDevice_SetRenderState_VertexBlend, stub_rs_VertexBlend),
    P(D3DDevice_SetRenderState_YuvEnable, stub_rs_YuvEnable),
    P(D3DDevice_SetRenderState_OcclusionCullEnable, stub_rs_OcclusionCullEnable),
    P(D3DDevice_SetRenderState_StencilCullEnable, stub_rs_StencilCullEnable),
    P(D3DDevice_SetRenderState_RopZCmpAlwaysRead, stub_rs_RopZCmpAlwaysRead),
    P(D3DDevice_SetRenderState_RopZRead, stub_rs_RopZRead),
    P(D3DDevice_SetRenderState_DoNotCullUncompressed, stub_rs_DoNotCullUncompressed),
    P(D3DDevice_SetRenderState_MultiSampleAntiAlias, stub_rs_MultiSampleAntiAlias),
    P(D3DDevice_SetRenderState_MultiSampleMask, stub_rs_MultiSampleMask),
    P(D3DDevice_SetTextureState_TexCoordIndex, hle_D3DDevice_SetTextureState_TexCoordIndex),
    P(D3DDevice_SetTextureState_BorderColor, hle_D3DDevice_SetTextureState_BorderColor),
    P(D3DDevice_SetTextureState_ColorKeyColor, hle_D3DDevice_SetTextureState_ColorKeyColor),
    P(D3DDevice_SetTextureState_BumpEnv, hle_D3DDevice_SetTextureState_BumpEnv),
};
#undef P
const unsigned g_d3d_patch_count = sizeof g_d3d_patches / sizeof g_d3d_patches[0];
