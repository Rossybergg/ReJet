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
 * surfaces, vertex/index buffers as Xbox-layout objects in game memory) and their locks, shader
 * handles. Render state, transforms, shaders, textures and draws are stubs that log their first
 * call: they come over from ReWhacked next. Pure helpers that only read static data or call the
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
#include "settings.h"
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

/* Wait for the next vertical blank boundary. */
static void wait_vblank(void)
{
    ULONG start = vblank_now();
    run_vblank_callbacks();
    while (vblank_now() <= start)
        Sleep(0);
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

/* Swap(Flags): 0 = present at the next vblank. The engine also uses 2 (present and keep the
 * old front buffer readable, for its screenshot path 0x14DBC0) and 4 (0x14DB80). */
static DWORD XBAPI hle_D3DDevice_Swap(DWORD Flags)
{
    LONGLONG t0;
    perf_frame(g_frame);
    wait_vblank(); /* presentation interval ONE */
    perf_wait_done();
    t0 = perf_now();
    if (g_dev)
        present();
    perf_end(PERF_PRESENT, t0);
    g_frame++;
    if (g_frame <= 3 || g_frame % 600 == 0)
        log_printf("d3d: frame %lu (Swap flags %lX)\n", g_frame, Flags);
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

/* ------------------------------------------------------------------ shaders (handles only) */

static LONG g_next_shader = 0x100;

static HRESULT XBAPI hle_D3DDevice_CreateVertexShader(const DWORD *pDeclaration, const DWORD *pFunction,
                                                      DWORD *pHandle, DWORD Usage)
{
    (void)pDeclaration;
    (void)Usage;
    {
        SURVEY(5)
        log_printf("survey: CreateVertexShader decl %p function %p (header %08lX) (from %08X)\n",
                   (const void *)pDeclaration, (const void *)pFunction, pFunction ? pFunction[0] : 0,
                   (unsigned)(UINT_PTR)_ReturnAddress());
    }
    /* Xbox vertex shader handles are odd (even values are FVF codes): keep bit 0 set. */
    *pHandle = (DWORD)(InterlockedAdd(&g_next_shader, 2) | 1);
    return S_OK;
}

static HRESULT XBAPI hle_D3DDevice_CreatePixelShader(const void *pPSDef, DWORD *pHandle)
{
    (void)pPSDef;
    *pHandle = (DWORD)InterlockedAdd(&g_next_shader, 2);
    return S_OK;
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
STUB(SetTransform, 0, D a, D b)
STUB(GetTransform, 0, D a, D b)
STUB(SetMaterial, 0, D a)
STUB(GetTile, 0, D a, D b)
STUB(SetTile, 0, D a, D b)
STUB(SetRenderTarget, 0, D a, D b)
STUB(DisplayQuery_18E120, 0, D a)
STUB(CopyRects, 0, D a, D b, D c, D d, D e)
STUB(SetViewport, 0, D a)
STUB(SetLight, 0, D a, D b)
STUB(LightEnable, 0, D a, D b)
STUB(GetTexture2, 0, D a, D b)
STUB(SetTexture, 0, D a, D b)
STUB(SetIndices, 0, D a, D b)
STUB(SetRenderState_1arg, 0, D a)
STUB(SetTextureState_2arg, 0, D a, D b)
STUB(SetTextureState_BumpEnv, 0, D a, D b, D c)
STUB(SetStreamSource, 0, D a, D b, D c)
STUB(SetShaderConstantMode, 0, D a)
STUB(DeleteVertexShader, 0, D a)
STUB(SetVertexShader, 0, D a)
STUB(SetVertexShaderConstant, 0, D a, D b, D c)
STUB(DeletePixelShader, 0, D a)
STUB(SetPixelShader, 0, D a)
STUB(SetPixelShaderConstant, 0, D a, D b, D c)
STUB(DrawVerticesUP, 0, D a, D b, D c, D d)
STUB(DrawIndexedVerticesUP, 0, D a, D b, D c, D d, D e)
STUB(DrawVertices, 0, D a, D b, D c)
STUB(DrawIndexedVertices, 0, D a, D b, D c)
STUB(SetVertexData2f, 0, D a, D b, D c)
STUB(SetVertexData4f, 0, D a, D b, D c, D d, D e)
STUB(Begin, 0, D a)
STUB(End, 0, void)
#undef D
#undef STUB

/* SetRenderState_Simple(method = ecx, value = edx): an NV2A method and its value. */
static void FASTCALL stub_SetRenderState_Simple(DWORD Method, DWORD Value)
{
    SURVEY(3)
    log_printf("d3d: SetRenderState_Simple not implemented yet (method %04lX value %08lX)\n", Method, Value);
}
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
    P(D3DDevice_DeleteVertexShader, stub_DeleteVertexShader),
    P(D3DDevice_SetVertexShader, stub_SetVertexShader),
    P(D3DDevice_SetVertexShaderConstant, stub_SetVertexShaderConstant),
    P(D3DDevice_SetShaderConstantMode, stub_SetShaderConstantMode),
    P(D3DDevice_CreatePixelShader, hle_D3DDevice_CreatePixelShader),
    P(D3DDevice_DeletePixelShader, stub_DeletePixelShader),
    P(D3DDevice_SetPixelShader, stub_SetPixelShader),
    P(D3DDevice_SetPixelShaderConstant, stub_SetPixelShaderConstant),
    /* transforms, lights, viewport, targets */
    P(D3DDevice_SetGammaRamp, stub_SetGammaRamp),
    P(D3DDevice_GetGammaRamp, stub_GetGammaRamp),
    P(D3DDevice_SetTransform, stub_SetTransform),
    P(D3DDevice_GetTransform, stub_GetTransform),
    P(D3DDevice_SetMaterial, stub_SetMaterial),
    P(D3DDevice_SetLight, stub_SetLight),
    P(D3DDevice_LightEnable, stub_LightEnable),
    P(D3DDevice_SetViewport, stub_SetViewport),
    P(D3DDevice_SetRenderTarget, stub_SetRenderTarget),
    P(D3DDevice_CopyRects, stub_CopyRects),
    P(D3DDevice_GetTile, stub_GetTile),
    P(D3DDevice_SetTile, stub_SetTile),
    /* textures and streams */
    P(D3DDevice_GetTexture2, stub_GetTexture2),
    P(D3DDevice_SetTexture, stub_SetTexture),
    P(D3DDevice_SetIndices, stub_SetIndices),
    P(D3DDevice_SetStreamSource, stub_SetStreamSource),
    /* draws */
    P(D3DDevice_DrawVerticesUP, stub_DrawVerticesUP),
    P(D3DDevice_DrawIndexedVerticesUP, stub_DrawIndexedVerticesUP),
    P(D3DDevice_DrawVertices, stub_DrawVertices),
    P(D3DDevice_DrawIndexedVertices, stub_DrawIndexedVertices),
    P(D3DDevice_SetVertexData2f, stub_SetVertexData2f),
    P(D3DDevice_SetVertexData4f, stub_SetVertexData4f),
    P(D3DDevice_Begin, stub_Begin),
    P(D3DDevice_End, stub_End),
    /* render and texture state */
    P(D3DDevice_SetRenderState_Simple, stub_SetRenderState_Simple),
    P(D3DDevice_SetRenderState_MultiSampleMode, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_MultiSampleRenderTargetMode, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_PSTextureModes, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_EdgeAntiAlias, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_ShadowFunc, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_FogColor, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_CullMode, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_FrontFace, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_NormalizeNormals, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_TextureFactor, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_LineWidth, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_Dxt1NoiseEnable, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_ZBias, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_LogicOp, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_FillMode, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_BackFillMode, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_TwoSidedLighting, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_VertexBlend, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_ZEnable, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_StencilEnable, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_StencilFail, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_YuvEnable, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_OcclusionCullEnable, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_StencilCullEnable, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_RopZCmpAlwaysRead, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_RopZRead, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_DoNotCullUncompressed, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_MultiSampleAntiAlias, stub_SetRenderState_1arg),
    P(D3DDevice_SetRenderState_MultiSampleMask, stub_SetRenderState_1arg),
    P(D3DDevice_SetTextureState_TexCoordIndex, stub_SetTextureState_2arg),
    P(D3DDevice_SetTextureState_BorderColor, stub_SetTextureState_2arg),
    P(D3DDevice_SetTextureState_ColorKeyColor, stub_SetTextureState_2arg),
    P(D3DDevice_SetTextureState_BumpEnv, stub_SetTextureState_BumpEnv),
};
#undef P
const unsigned g_d3d_patch_count = sizeof g_d3d_patches / sizeof g_d3d_patches[0];
