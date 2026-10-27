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
 * render and texture state, transforms, lights, viewport and draws. Not yet: render-to-texture,
 * immediate mode (Begin/SetVertexData/End), tiling, gamma; these are typed stubs. Pure helpers that only read static data or call the
 * kernel (adapter queries, D3D_SetPushBufferSize, D3D_AllocContiguousMemory,
 * D3DResource_GetType/Register, XGRAPHC) keep running as original code. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "kernel.h"
#include "perf.h"
#include "psh.h"
#include "settings.h"
#include "tex.h"
#include "vsh.h"
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

/* Render resolution: the game draws for g_xw x g_xh (640x480); the host back buffer is
 * g_hw x g_hh (settings). Clear rectangles are given in game pixels and scaled. */
static UINT g_xw = 640, g_xh = 480, g_hw = 640, g_hh = 480;
static float g_sx = 1.0f, g_sy = 1.0f;
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
static volatile LONG g_shot_request;          /* F12 */

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
    if (m == WM_KEYDOWN && wp == VK_F12) {
        InterlockedExchange(&g_shot_request, 1);
        return 0;
    }
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

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"rejet";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); /* letterbox bars */
    RegisterClassExW(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowW(L"rejet", L"Jet Set Radio Future (ReJet)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                          CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    SetEvent(req->ready);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
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
    g_hw = (UINT)g_settings.width;
    g_hh = (UINT)g_settings.height;
    g_sx = (float)g_hw / (float)g_xw;
    g_sy = (float)g_hh / (float)g_xh;
    create_window((int)g_hw * g_settings.window_scale / 100, (int)g_hh * g_settings.window_scale / 100);

    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &g_d3d))) {
        log_printf("error: Direct3DCreate9Ex failed\n");
        return E_FAIL;
    }
    memset(&hp, 0, sizeof hp);
    hp.BackBufferWidth = g_hw;
    hp.BackBufferHeight = g_hh;
    hp.BackBufferFormat = D3DFMT_X8R8G8B8; /* the game asks for 16-bit R5G6B5; 32-bit avoids banding */
    hp.BackBufferCount = 1;
    hp.SwapEffect = D3DSWAPEFFECT_DISCARD;
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
    /* no widescreen squeeze yet: 2D programs keep their x scale */
    g_vconst[VSH_C_2D][0] = g_vconst[VSH_C_2D][1] = g_vconst[VSH_C_2D][2] = g_vconst[VSH_C_2D][3] = 1.0f;
    log_printf("d3d: Direct3D 9Ex device ready, window %p\n", (void *)g_wnd);
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
    if (!g_dev)
        return S_OK;
    if (Count && pRects) {
        D3DRECT r[16];
        if (Count > 16)
            Count = 16;
        for (DWORD i = 0; i < Count; i++) {
            r[i].x1 = (LONG)(pRects[i].x1 * g_sx);
            r[i].y1 = (LONG)(pRects[i].y1 * g_sy);
            r[i].x2 = (LONG)(pRects[i].x2 * g_sx);
            r[i].y2 = (LONG)(pRects[i].y2 * g_sy);
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
static LARGE_INTEGER g_vblank_start, g_qpf;
static ULONG g_vblank_hz = 60, g_vblank_count;
static CRITICAL_SECTION g_vblank_lock;
static void *g_vblank_callback;
static INIT_ONCE g_vblank_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK vblank_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_vblank_lock);
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_vblank_start);
    g_vblank_hz = g_settings.pal60 ? 60 : 50;
    return TRUE;
}

static ULONG vblank_now(void)
{
    LARGE_INTEGER now;
    InitOnceExecuteOnce(&g_vblank_once, vblank_init, NULL, NULL);
    QueryPerformanceCounter(&now);
    return (ULONG)((now.QuadPart - g_vblank_start.QuadPart) * g_vblank_hz / g_qpf.QuadPart);
}

static void run_vblank_callbacks(void)
{
    ULONG now = vblank_now();
    EnterCriticalSection(&g_vblank_lock);
    while (g_vblank_count < now) {
        g_vblank_count++;
        if (g_vblank_callback)
            ((void(__cdecl *)(void *))g_vblank_callback)(NULL);
    }
    LeaveCriticalSection(&g_vblank_lock);
}

/* Wait for the next vertical blank boundary. This must really sleep: CRI ADX raises its vblank
 * thread's priority, and a spinning wait (Sleep(0)) at that priority starved the normal-priority
 * threads XAudio2 needs, so CreateMasteringVoice never returned and the process could not exit.
 * Each waiting thread sleeps on its own high-resolution waitable timer until the boundary. */
static __declspec(thread) HANDLE t_vblank_timer;

static void wait_vblank(void)
{
    ULONG start = vblank_now();
    LARGE_INTEGER now, due;
    run_vblank_callbacks();
    if (!t_vblank_timer) {
        t_vblank_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t_vblank_timer) /* older Windows: a normal timer (about 1 ms resolution with timeBeginPeriod) */
            t_vblank_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    for (;;) {
        LONGLONG boundary = g_vblank_start.QuadPart + ((LONGLONG)(start + 1) * g_qpf.QuadPart + g_vblank_hz - 1) / g_vblank_hz;
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= boundary)
            break;
        /* relative due time in 100 ns units (negative) */
        due.QuadPart = -(LONGLONG)((boundary - now.QuadPart) * 10000000 / g_qpf.QuadPart);
        if (due.QuadPart == 0 || !t_vblank_timer || !SetWaitableTimer(t_vblank_timer, &due, 0, NULL, NULL, FALSE))
            Sleep(1);
        else
            WaitForSingleObject(t_vblank_timer, INFINITE);
    }
    run_vblank_callbacks();
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
    GetClientRect(g_wnd, &c);
    cw = c.right - c.left;
    ch = c.bottom - c.top;
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

/* Save the back buffer as a BMP (ReWhacked). */
static void save_backbuffer(const char *name)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    FILE *f;

    if (FAILED(IDirect3DDevice9Ex_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return;
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
    IDirect3DSurface9_Release(bb);
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
    perf_frame(g_frame);
    wait_vblank(); /* presentation interval ONE */
    perf_wait_done();
    t0 = perf_now();
    if (g_dev && g_in_scene) {
        IDirect3DDevice9Ex_EndScene(g_dev);
        g_in_scene = 0;
    }
    maybe_screenshot();
    if (g_dev)
        present();
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

static HRESULT XBAPI hle_D3DDevice_SetPixelShader(DWORD Handle)
{
    g_cur_ps = Handle;
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_SetPixelShaderConstant(DWORD Register, const void *pData, DWORD Count)
{
    if (Register + Count <= 16)
        memcpy(g_psconst[Register], pData, Count * sizeof g_psconst[0]);
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

static void rs(D3DRENDERSTATETYPE s, DWORD v)
{
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
static void XBAPI hle_D3DDevice_SetRenderState_CullMode(DWORD v)
{
    rs(D3DRS_CULLMODE, v == 0x900 ? D3DCULL_CW : v == 0x901 ? D3DCULL_CCW : D3DCULL_NONE);
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

static void XBAPI hle_D3DDevice_SetRenderState_TextureFactor(DWORD v)
{
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
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MINFILTER, xbox_filter(t[4]));
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MIPFILTER, xbox_filter(t[5]));
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MIPMAPLODBIAS, t[6]);
        IDirect3DDevice9Ex_SetSamplerState(g_dev, s, D3DSAMP_MAXMIPLEVEL, t[7]);
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

static HRESULT XBAPI hle_D3DDevice_SetTransform(DWORD State, const D3DMATRIX *pMatrix)
{
    if (State < 10 && pMatrix) {
        g_xform[State] = *pMatrix;
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

/* D3DMATERIAL8 and D3DLIGHT8 share D3D9's layouts. */
static HRESULT XBAPI hle_D3DDevice_SetMaterial(const D3DMATERIAL9 *pMaterial)
{
    if (g_dev && pMaterial)
        IDirect3DDevice9Ex_SetMaterial(g_dev, pMaterial);
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_SetLight(DWORD Index, const D3DLIGHT9 *pLight)
{
    if (g_dev && pLight)
        IDirect3DDevice9Ex_SetLight(g_dev, Index, pLight);
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_LightEnable(DWORD Index, BOOL Enable)
{
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
        int full = v->X == 0 && v->Y == 0 && v->Width >= g_xw && v->Height >= g_xh;
        g_vp_rect.left = (LONG)(v->X * g_sx + 0.5f);
        g_vp_rect.top = (LONG)(v->Y * g_sy + 0.5f);
        g_vp_rect.right = (LONG)((v->X + v->Width) * g_sx + 0.5f);
        g_vp_rect.bottom = (LONG)((v->Y + v->Height) * g_sy + 0.5f);
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

/* Render targets: drawing into textures (the engine's 512x512 and 128x128 R5G6B5 targets) is
 * not done yet; everything goes to the back buffer, and the target texture keeps its contents. */
static XD3DSurface *g_cur_rt;

static HRESULT XBAPI hle_D3DDevice_SetRenderTarget(XD3DSurface *pRenderTarget, XD3DSurface *pNewZStencil)
{
    if (pRenderTarget)
        g_cur_rt = pRenderTarget;
    {
        SURVEY(12)
        log_printf("survey: SetRenderTarget %p%s fmt %08X size %08X, z %p%s (from %08X)\n", (void *)pRenderTarget,
                   pRenderTarget == &g_backbuffer ? " (back buffer)" : "",
                   pRenderTarget ? pRenderTarget->Container.Format : 0, pRenderTarget ? pRenderTarget->Container.Size : 0,
                   (void *)pNewZStencil, pNewZStencil == &g_depth ? " (depth)" : "",
                   (unsigned)(UINT_PTR)_ReturnAddress());
    }
    return S_OK;
}

/* Is the current target the back buffer? Draws into other targets are skipped for now, so
 * render-to-texture passes don't land on the screen. */
static int drawing_to_backbuffer(void)
{
    return !g_cur_rt || g_cur_rt == &g_backbuffer;
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

/* Bind the current vertex program (or FVF), constants, textures and pixel shader (ReWhacked).
 * Returns 0 to skip the draw. */
static int prepare_draw(void)
{
    if (!g_dev)
        return 0;
    if (!drawing_to_backbuffer()) {
        g_skipped++;
        return 0;
    }
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
        IDirect3DBaseTexture9 *t = g_textures[s] ? tex_get((IDirect3DDevice9 *)g_dev, g_textures[s], scale) : NULL;
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
                    float pc[18][4];
                    psh_constants(g_ps_def[i], (const float(*)[4])g_psconst, pc);
                    IDirect3DDevice9Ex_SetPixelShaderConstantF(g_dev, 0, pc[0], 18);
                }
            }
        }
        IDirect3DDevice9Ex_SetPixelShader(g_dev, ps);
    }
    IDirect3DDevice9Ex_SetVertexShaderConstantF(g_dev, 0, g_vconst[0], VSH_CONSTANTS);
    g_draws++;
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

/* Pre-transformed (XYZRHW) vertices are in Xbox screen pixels; at another render resolution
 * they are copied with x and y scaled. Vertices first..last of `verts` are converted; the
 * returned pointer is used with the same indices. */
static const BYTE *scale_rhw(const BYTE *verts, UINT first, UINT last, UINT stride)
{
    static BYTE *buf;
    static size_t cap;
    size_t need = (size_t)(last + 1) * stride;
    if ((g_cur_vs & 1) || (g_cur_vs & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || (g_sx == 1.0f && g_sy == 1.0f))
        return verts;
    if (need > cap) {
        BYTE *n = (BYTE *)realloc(buf, need);
        if (!n)
            return verts;
        buf = n;
        cap = need;
    }
    for (UINT i = first; i <= last; i++) {
        float *xy = (float *)(buf + (size_t)i * stride);
        memcpy(xy, verts + (size_t)i * stride, stride);
        xy[0] *= g_sx;
        xy[1] *= g_sy;
    }
    return buf;
}

/* Draw `count` vertices from `verts` (optionally indexed by 16-bit `idx`). */
static void draw_up(DWORD xprim, UINT count, const BYTE *verts, UINT stride, const WORD *idx)
{
    D3DPRIMITIVETYPE pt;
    UINT prims, lo = 0, hi = 0;
    int quads;
    LONGLONG t0 = perf_now();
    if (!verts || !stride || !convert_prim(xprim, count, &pt, &prims, &quads) || !prepare_draw())
        return;
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
        verts = scale_rhw(verts, lo, hi, stride);
        IDirect3DDevice9Ex_DrawIndexedPrimitiveUP(g_dev, pt, lo, hi - lo + 1, prims, idx, D3DFMT_INDEX16, verts, stride);
    }
    else {
        verts = scale_rhw(verts, 0, count - 1, stride);
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
STUB(CopyRects, 0, D a, D b, D c, D d, D e)
STUB(GetTexture2, 0, D a, D b)
STUB(SetRenderState_1arg, 0, D a)
STUB(SetVertexData2f, 0, D a, D b, D c)                /* immediate mode (Begin/SetVertexData/End) */
STUB(SetVertexData4f, 0, D a, D b, D c, D d, D e)
STUB(Begin, 0, D a)
STUB(End, 0, void)
#undef D
#undef STUB
#pragma warning(pop)

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
    P(D3DDevice_CopyRects, stub_CopyRects),
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
    P(D3DDevice_SetVertexData2f, stub_SetVertexData2f),
    P(D3DDevice_SetVertexData4f, stub_SetVertexData4f),
    P(D3DDevice_Begin, stub_Begin),
    P(D3DDevice_End, stub_End),
    /* render and texture state */
    P(D3DDevice_SetRenderState_Simple, hle_D3DDevice_SetRenderState_Simple),
    P(D3DDevice_SetRenderState_CullMode, hle_D3DDevice_SetRenderState_CullMode),
    P(D3DDevice_SetRenderState_FrontFace, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_ZEnable, hle_D3DDevice_SetRenderState_ZEnable),
    P(D3DDevice_SetRenderState_FillMode, hle_D3DDevice_SetRenderState_FillMode),
    P(D3DDevice_SetRenderState_StencilEnable, hle_D3DDevice_SetRenderState_StencilEnable),
    P(D3DDevice_SetRenderState_StencilFail, hle_D3DDevice_SetRenderState_StencilFail),
    P(D3DDevice_SetRenderState_TextureFactor, hle_D3DDevice_SetRenderState_TextureFactor),
    P(D3DDevice_SetRenderState_FogColor, hle_D3DDevice_SetRenderState_FogColor),
    P(D3DDevice_SetRenderState_NormalizeNormals, hle_D3DDevice_SetRenderState_NormalizeNormals),
    P(D3DDevice_SetRenderState_MultiSampleMode, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_MultiSampleRenderTargetMode, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_PSTextureModes, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_EdgeAntiAlias, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_ShadowFunc, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_LineWidth, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_Dxt1NoiseEnable, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_ZBias, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_LogicOp, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_BackFillMode, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_TwoSidedLighting, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_VertexBlend, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_YuvEnable, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_OcclusionCullEnable, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_StencilCullEnable, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_RopZCmpAlwaysRead, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_RopZRead, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_DoNotCullUncompressed, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_MultiSampleAntiAlias, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_MultiSampleMask, stub_SetRenderState_1arg),
    P(D3DDevice_SetTextureState_TexCoordIndex, hle_D3DDevice_SetTextureState_TexCoordIndex),
    P(D3DDevice_SetTextureState_BorderColor, hle_D3DDevice_SetTextureState_BorderColor),
    P(D3DDevice_SetTextureState_ColorKeyColor, hle_D3DDevice_SetTextureState_ColorKeyColor),
    P(D3DDevice_SetTextureState_BumpEnv, hle_D3DDevice_SetTextureState_BumpEnv),
};
#undef P
const unsigned g_d3d_patch_count = sizeof g_d3d_patches / sizeof g_d3d_patches[0];
