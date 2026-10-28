/* The ReJet menu (see menu.h).
 *
 * Drawn with the game's own Direct3D 9Ex device at the end of each frame: a font atlas made
 * with GDI at start-up (no outside libraries), coloured parallelograms for the JSRF look, all
 * as pre-transformed quads in one texture. The device state is saved and restored around it
 * with a state block, so the game's next draws are unaffected. While the menu is open the game
 * thread stays inside Swap (the game is paused: no frames, no vblanks), and the menu redraws
 * over a frozen copy of the last frame. Music keeps playing, quieter.
 *
 * Layout is designed on a 1280x960 canvas and scaled to the back buffer. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <Xinput.h>
#include <wincodec.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "settings.h"
#include "ui/menu.h"

static MenuHost g_host;
#define DEV ((IDirect3DDevice9Ex *)g_host.dev)

/* ------------------------------------------------------------------ colours (ARGB) */

#define C_LIME    0xFFC8F01Eu /* JSRF's yellow-green */
#define C_PINK    0xFFFF2D8Au
#define C_TEAL    0xFF19C8C8u
#define C_WHITE   0xFFFFFFFFu
#define C_GREY    0xFF9AA4A4u
#define C_BLACK   0xFF000000u
#define C_PANEL   0xE00A1A1Cu
#define C_SHADE   0x99000000u

static DWORD with_alpha(DWORD c, float a)
{
    DWORD ca = (DWORD)((float)(c >> 24) * (a < 0 ? 0 : a > 1 ? 1 : a));
    return (c & 0x00FFFFFFu) | ca << 24;
}

/* ------------------------------------------------------------------ animation helpers */

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static float clamp01(float t)
{
    return t < 0 ? 0 : t > 1 ? 1 : t;
}

/* 0..1 progress of an animation that started at `start` and lasts `ms` */
static float progress(double start, double ms)
{
    return clamp01((float)((now_ms() - start) / ms));
}

static float ease_out(float t) /* fast, then settles */
{
    t = 1 - clamp01(t);
    return 1 - t * t * t;
}

static float ease_back(float t) /* overshoots a little, then settles */
{
    const float c1 = 1.70158f, c3 = c1 + 1;
    t = clamp01(t) - 1;
    return 1 + c3 * t * t * t + c1 * t * t;
}

/* ------------------------------------------------------------------ font atlas */

enum { FONT_TITLE, FONT_ITEM, FONT_SMALL, FONT_COUNT };
#define ATLAS 2048
#define FIRST_CHAR 32
#define LAST_CHAR 126

typedef struct Glyph {
    float u0, v0, u1, v1; /* atlas texture coordinates */
    float w, h;           /* cell size in pixels */
    float adv;            /* advance */
} Glyph;

static Glyph g_glyph[FONT_COUNT][LAST_CHAR - FIRST_CHAR + 1];
static float g_font_px[FONT_COUNT]; /* line height in back-buffer pixels */
static float g_white_u, g_white_v;  /* a white texel, for solid shapes */
static IDirect3DTexture9 *g_atlas;
static UINT g_bw, g_bh;             /* back-buffer size the atlas was built for */
static float g_scale, g_ox;         /* canvas -> back buffer: x' = g_ox + x * g_scale */

/* ------------------------------------------------------------------ logo */

/* The ReJet logo: rejet_logo.png next to ReJetLoader.exe, else the copy built into rejet.dll
 * (resource REJET_LOGO, loader/res/), decoded with Windows Imaging Component. Without one the
 * menu writes "ReJet" in the title font. */
static DWORD *g_logo_px; /* premultiplied BGRA, original size */
static UINT g_logo_w, g_logo_h;
static float g_lu0, g_lv0, g_lu1, g_lv1, g_logo_aspect; /* where it sits in the atlas */

static void load_logo(void)
{
    IWICImagingFactory *wic = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    IWICStream *stream = NULL;
    WCHAR path[MAX_PATH], *slash;
    HRESULT hr, co = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&wic)))
        goto done;
    hr = E_FAIL;
    if (GetModuleFileNameW(NULL, path, MAX_PATH) && (slash = wcsrchr(path, L'\\')) != NULL) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"rejet_logo.png");
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            hr = IWICImagingFactory_CreateDecoderFromFilename(wic, path, NULL, GENERIC_READ,
                                                              WICDecodeMetadataCacheOnDemand, &dec);
    }
    if (FAILED(hr)) { /* built in */
        HMODULE self = NULL;
        HRSRC res;
        HGLOBAL mem;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(void *)load_logo, &self);
        if (!self || !(res = FindResourceW(self, L"REJET_LOGO", (LPCWSTR)RT_RCDATA)) ||
            !(mem = LoadResource(self, res)) ||
            FAILED(IWICImagingFactory_CreateStream(wic, &stream)) ||
            FAILED(IWICStream_InitializeFromMemory(stream, (BYTE *)LockResource(mem), SizeofResource(self, res))) ||
            FAILED(IWICImagingFactory_CreateDecoderFromStream(wic, (IStream *)stream, NULL,
                                                              WICDecodeMetadataCacheOnDemand, &dec)))
            goto done;
    }
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) ||
        FAILED(IWICImagingFactory_CreateFormatConverter(wic, &conv)) ||
        FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppPBGRA,
                                              WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(IWICBitmapSource_GetSize((IWICBitmapSource *)conv, &g_logo_w, &g_logo_h)) || !g_logo_w ||
        !g_logo_h || g_logo_w > 8192 || g_logo_h > 8192)
        goto done;
    g_logo_px = (DWORD *)malloc((size_t)g_logo_w * g_logo_h * 4);
    if (g_logo_px && FAILED(IWICBitmapSource_CopyPixels((IWICBitmapSource *)conv, NULL, g_logo_w * 4,
                                                        g_logo_w * g_logo_h * 4, (BYTE *)g_logo_px))) {
        free(g_logo_px);
        g_logo_px = NULL;
    }
    if (g_logo_px) { /* trim fully transparent edges */
        UINT x0 = g_logo_w, y0 = g_logo_h, x1 = 0, y1 = 0;
        for (UINT y = 0; y < g_logo_h; y++)
            for (UINT x = 0; x < g_logo_w; x++)
                if ((g_logo_px[y * g_logo_w + x] >> 24) > 8) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        if (x1 >= x0 && y1 >= y0) {
            UINT w = x1 - x0 + 1, h = y1 - y0 + 1;
            for (UINT y = 0; y < h; y++)
                memmove(g_logo_px + (size_t)y * w, g_logo_px + (size_t)(y + y0) * g_logo_w + x0, (size_t)w * 4);
            log_printf("menu: logo %ux%u (trimmed from %ux%u)\n", w, h, g_logo_w, g_logo_h);
            g_logo_w = w;
            g_logo_h = h;
        }
        else {
            free(g_logo_px);
            g_logo_px = NULL;
        }
    }
done:
    if (conv)
        IWICFormatConverter_Release(conv);
    if (frame)
        IWICBitmapFrameDecode_Release(frame);
    if (dec)
        IWICBitmapDecoder_Release(dec);
    if (stream)
        IWICStream_Release(stream);
    if (wic)
        IWICImagingFactory_Release(wic);
    if (SUCCEEDED(co))
        CoUninitialize();
}

/* Box-filter the logo down to w x h (premultiplied, so edges stay clean). */
static void scale_logo(DWORD *dst, UINT w, UINT h, UINT pitch)
{
    for (UINT y = 0; y < h; y++) {
        UINT sy0 = y * g_logo_h / h, sy1 = (y + 1) * g_logo_h / h;
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (UINT x = 0; x < w; x++) {
            UINT sx0 = x * g_logo_w / w, sx1 = (x + 1) * g_logo_w / w, n = 0, acc[4] = {0, 0, 0, 0};
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            for (UINT sy = sy0; sy < sy1 && sy < g_logo_h; sy++)
                for (UINT sx = sx0; sx < sx1 && sx < g_logo_w; sx++, n++) {
                    DWORD p = g_logo_px[sy * g_logo_w + sx];
                    for (int k = 0; k < 4; k++)
                        acc[k] += (p >> (8 * k)) & 0xFF;
                }
            if (!n)
                n = 1;
            dst[y * pitch + x] = (acc[0] / n) | (acc[1] / n) << 8 | (acc[2] / n) << 16 | (acc[3] / n) << 24;
        }
    }
}

static int build_atlas(UINT bw, UINT bh)
{
    static const struct { const WCHAR *face; int size, weight, italic; } fonts[FONT_COUNT] = {
        {L"Arial Black", 76, FW_HEAVY, 1},
        {L"Arial Black", 34, FW_HEAVY, 1},
        {L"Segoe UI", 24, FW_BOLD, 0},
    };
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi;
    DWORD *bits = NULL;
    HBITMAP bm;
    int x = 0, y = 0, row = 0, logo_top = ATLAS - 8;
    UINT lw = 0, lh = 0;
    DWORD *logo = NULL;
    D3DLOCKED_RECT lr;

    if (g_atlas) {
        IDirect3DTexture9_Release(g_atlas);
        g_atlas = NULL;
    }
    g_bw = bw;
    g_bh = bh;
    g_scale = (float)bh / 960.0f;
    if ((float)bw < 1280.0f * g_scale) /* narrower than 4:3: fit the width */
        g_scale = (float)bw / 1280.0f;
    g_ox = ((float)bw - 1280.0f * g_scale) * 0.5f;

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = ATLAS;
    bi.bmiHeader.biHeight = -ATLAS;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!bm || !bits) {
        DeleteDC(dc);
        return 0;
    }
    SelectObject(dc, bm);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    if (g_logo_px) { /* the logo goes in the bottom-left of the atlas, at the size it is drawn */
        lh = (UINT)(90.0f * g_scale + 0.5f); /* a little above the drawn size (76) */
        if (lh > g_logo_h)
            lh = g_logo_h;
        lw = (UINT)((float)lh * (float)g_logo_w / (float)g_logo_h + 0.5f);
        if (lw > ATLAS - 16) {
            lw = ATLAS - 16;
            lh = (UINT)((float)lw * (float)g_logo_h / (float)g_logo_w + 0.5f);
        }
        logo_top = ATLAS - 8 - (int)lh;
        g_lu0 = 0.5f / ATLAS;
        g_lv0 = ((float)logo_top + 0.5f) / ATLAS;
        g_lu1 = ((float)lw - 0.5f) / ATLAS;
        g_lv1 = ((float)(logo_top + (int)lh) - 0.5f) / ATLAS;
        g_logo_aspect = (float)g_logo_w / (float)g_logo_h;
    }
    for (int f = 0; f < FONT_COUNT; f++) {
        int px = (int)((float)fonts[f].size * g_scale + 0.5f);
        HFONT h = CreateFontW(-px, 0, 0, 0, fonts[f].weight, fonts[f].italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, fonts[f].face);
        HGDIOBJ old = SelectObject(dc, h);
        TEXTMETRICW tm;
        int pad = fonts[f].italic ? px / 3 : 2; /* italic overhang */
        GetTextMetricsW(dc, &tm);
        g_font_px[f] = (float)tm.tmHeight;
        for (int c = FIRST_CHAR; c <= LAST_CHAR; c++) {
            WCHAR wc = (WCHAR)c;
            SIZE sz;
            int cw;
            Glyph *g = &g_glyph[f][c - FIRST_CHAR];
            GetTextExtentPoint32W(dc, &wc, 1, &sz);
            cw = sz.cx + pad + 2;
            if (x + cw > ATLAS) {
                x = 0;
                y += row + 2;
                row = 0;
            }
            if (y + tm.tmHeight > logo_top - 4)
                break;
            TextOutW(dc, x + 1, y, &wc, 1);
            g->u0 = (float)x / ATLAS;
            g->v0 = (float)y / ATLAS;
            g->u1 = (float)(x + cw) / ATLAS;
            g->v1 = (float)(y + tm.tmHeight) / ATLAS;
            g->w = (float)cw;
            g->h = (float)tm.tmHeight;
            g->adv = (float)sz.cx;
            x += cw + 2;
            if (tm.tmHeight > row)
                row = tm.tmHeight;
        }
        SelectObject(dc, old);
        DeleteObject(h);
    }
    GdiFlush();
    /* a white block in the bottom-right corner for solid fills */
    for (int j = ATLAS - 6; j < ATLAS; j++)
        for (int i = ATLAS - 6; i < ATLAS; i++)
            bits[j * ATLAS + i] = 0x00FFFFFFu;
    g_white_u = (ATLAS - 3.0f) / ATLAS;
    g_white_v = (ATLAS - 3.0f) / ATLAS;

    if (SUCCEEDED(IDirect3DDevice9Ex_CreateTexture(DEV, ATLAS, ATLAS, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                                   D3DPOOL_DEFAULT, &g_atlas, NULL)) &&
        SUCCEEDED(IDirect3DTexture9_LockRect(g_atlas, 0, &lr, NULL, D3DLOCK_DISCARD))) {
        for (int j = 0; j < ATLAS; j++) {
            DWORD *dst = (DWORD *)((BYTE *)lr.pBits + (size_t)j * lr.Pitch);
            const DWORD *src = bits + (size_t)j * ATLAS;
            for (int i = 0; i < ATLAS; i++) /* coverage (grey text) -> alpha on white */
                dst[i] = (src[i] & 0xFF) << 24 | 0x00FFFFFFu;
        }
        if (lw && (logo = (DWORD *)malloc((size_t)lw * lh * 4)) != NULL) {
            scale_logo(logo, lw, lh, lw);
            for (UINT j = 0; j < lh; j++) {
                DWORD *dst = (DWORD *)((BYTE *)lr.pBits + (size_t)(logo_top + (int)j) * lr.Pitch);
                for (UINT i = 0; i < lw; i++) { /* premultiplied -> straight alpha */
                    DWORD p = logo[j * lw + i], a = p >> 24;
                    DWORD r = a ? ((p >> 16) & 0xFF) * 255 / a : 0, g = a ? ((p >> 8) & 0xFF) * 255 / a : 0,
                          b = a ? (p & 0xFF) * 255 / a : 0;
                    dst[i] = a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
                }
            }
            free(logo);
        }
        IDirect3DTexture9_UnlockRect(g_atlas, 0);
    }
    DeleteObject(bm);
    DeleteDC(dc);
    return g_atlas != NULL;
}

/* ------------------------------------------------------------------ drawing */

typedef struct Vertex {
    float x, y, z, rhw;
    DWORD c;
    float u, v;
} Vertex;
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define MAX_VERTS 24576
static Vertex g_v[MAX_VERTS];
static int g_nv;
static IDirect3DStateBlock9 *g_state;
static float g_fade = 1.0f; /* opacity for what is drawn next (animations) */

static float X(float x) { return g_ox + x * g_scale; }
static float Y(float y) { return y * g_scale; }

/* A quad in back-buffer pixels; `skew` shifts the top edge right (parallelograms). */
static void quad_px(float x0, float y0, float x1, float y1, float skew, DWORD c, float u0, float v0, float u1, float v1)
{
    Vertex *v = g_v + g_nv;
    if (g_nv + 6 > MAX_VERTS)
        return;
    x0 -= 0.5f; x1 -= 0.5f; y0 -= 0.5f; y1 -= 0.5f;
    v[0] = (Vertex){x0 + skew, y0, 0, 1, c, u0, v0};
    v[1] = (Vertex){x1 + skew, y0, 0, 1, c, u1, v0};
    v[2] = (Vertex){x0, y1, 0, 1, c, u0, v1};
    v[3] = v[1];
    v[4] = (Vertex){x1, y1, 0, 1, c, u1, v1};
    v[5] = v[2];
    g_nv += 6;
}

/* A solid parallelogram on the canvas. */
static void box(float x0, float y0, float x1, float y1, float skew, DWORD c)
{
    quad_px(X(x0), Y(y0), X(x1), Y(y1), skew * g_scale, with_alpha(c, g_fade), g_white_u, g_white_v, g_white_u,
            g_white_v);
}

static float text_width(int font, const char *s)
{
    float w = 0;
    for (; *s; s++)
        if ((unsigned char)*s >= FIRST_CHAR && (unsigned char)*s <= LAST_CHAR)
            w += g_glyph[font][(unsigned char)*s - FIRST_CHAR].adv;
    return w / g_scale; /* canvas units */
}

/* Text on the canvas at (x, y) = top left; align 0 left, 1 centre, 2 right. Drop shadow. */
static void text(int font, float x, float y, int align, DWORD c, const char *s)
{
    float px, py = Y(y), w = text_width(font, s);
    if (align == 1)
        x -= w * 0.5f;
    else if (align == 2)
        x -= w;
    for (int pass = 0; pass < 2; pass++) {
        DWORD col = pass ? with_alpha(c, g_fade) : with_alpha(0xC0000000u, (float)(c >> 24) / 255.0f * g_fade);
        float off = pass ? 0.0f : (font == FONT_SMALL ? 1.0f : 2.5f) * g_scale;
        px = X(x);
        for (const char *p = s; *p; p++) {
            unsigned ch = (unsigned char)*p;
            const Glyph *g;
            if (ch < FIRST_CHAR || ch > LAST_CHAR)
                continue;
            g = &g_glyph[font][ch - FIRST_CHAR];
            if (ch != ' ')
                quad_px(px + off, py + off, px + off + g->w, py + off + g->h, 0, col, g->u0, g->v0, g->u1, g->v1);
            px += g->adv;
        }
    }
}

/* The logo, `h` canvas units tall, top left at (x, y); returns its width. */
static float logo(float x, float y, float h, DWORD c)
{
    float w = h * g_logo_aspect;
    if (!g_logo_px || g_lu1 <= g_lu0)
        return 0;
    quad_px(X(x), Y(y), X(x + w), Y(y + h), 0, with_alpha(c, g_fade), g_lu0, g_lv0, g_lu1, g_lv1);
    return w;
}

static void flush(void)
{
    IDirect3DSurface9 *bb = NULL, *old_rt = NULL;
    D3DVIEWPORT9 vp = {0, 0, g_bw, g_bh, 0.0f, 1.0f};
    if (!g_nv || !g_atlas)
        return;
    if (!g_state && FAILED(IDirect3DDevice9Ex_CreateStateBlock(DEV, D3DSBT_ALL, &g_state)))
        return;
    IDirect3DStateBlock9_Capture(g_state);
    IDirect3DDevice9Ex_GetRenderTarget(DEV, 0, &old_rt);
    IDirect3DDevice9Ex_GetBackBuffer(DEV, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    IDirect3DDevice9Ex_SetRenderTarget(DEV, 0, bb);
    IDirect3DDevice9Ex_SetViewport(DEV, &vp);
    IDirect3DDevice9Ex_SetVertexShader(DEV, NULL);
    IDirect3DDevice9Ex_SetPixelShader(DEV, NULL);
    IDirect3DDevice9Ex_SetFVF(DEV, FVF);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_FILLMODE, D3DFILL_SOLID);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_COLORWRITEENABLE, 0xF);
    IDirect3DDevice9Ex_SetTexture(DEV, 0, (IDirect3DBaseTexture9 *)g_atlas);
    IDirect3DDevice9Ex_SetTexture(DEV, 1, NULL);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 0, D3DTSS_TEXCOORDINDEX, 0);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice9Ex_SetSamplerState(DEV, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9Ex_SetSamplerState(DEV, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9Ex_SetSamplerState(DEV, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9Ex_SetSamplerState(DEV, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9Ex_SetSamplerState(DEV, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9Ex_SetSamplerState(DEV, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    IDirect3DDevice9Ex_BeginScene(DEV);
    IDirect3DDevice9Ex_DrawPrimitiveUP(DEV, D3DPT_TRIANGLELIST, (UINT)g_nv / 3, g_v, sizeof(Vertex));
    IDirect3DDevice9Ex_EndScene(DEV);
    g_nv = 0;
    IDirect3DStateBlock9_Apply(g_state);
    IDirect3DDevice9Ex_SetRenderTarget(DEV, 0, old_rt);
    if (old_rt)
        IDirect3DSurface9_Release(old_rt);
    if (bb)
        IDirect3DSurface9_Release(bb);
}

/* Make sure the atlas matches the back buffer. */
static int ready(void)
{
    IDirect3DSurface9 *bb = NULL;
    D3DSURFACE_DESC d;
    if (!DEV)
        return 0;
    if (FAILED(IDirect3DDevice9Ex_GetBackBuffer(DEV, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return 0;
    IDirect3DSurface9_GetDesc(bb, &d);
    IDirect3DSurface9_Release(bb);
    if (!g_atlas || d.Width != g_bw || d.Height != g_bh)
        build_atlas(d.Width, d.Height);
    return g_atlas != NULL;
}

/* ------------------------------------------------------------------ messages and counter */

#define MAX_TOASTS 4
static CRITICAL_SECTION g_toast_lock;
static struct { char text[96]; ULONGLONG at; } g_toasts[MAX_TOASTS];
static int g_ntoasts;

void menu_toast(const char *fmt, ...)
{
    va_list ap;
    if (!g_host.dev)
        return;
    EnterCriticalSection(&g_toast_lock);
    if (g_ntoasts == MAX_TOASTS) {
        memmove(g_toasts, g_toasts + 1, sizeof g_toasts[0] * (MAX_TOASTS - 1));
        g_ntoasts--;
    }
    va_start(ap, fmt);
    vsnprintf(g_toasts[g_ntoasts].text, sizeof g_toasts[0].text, fmt, ap);
    va_end(ap);
    g_toasts[g_ntoasts].at = GetTickCount64();
    g_ntoasts++;
    LeaveCriticalSection(&g_toast_lock);
}

static void draw_toasts(void)
{
    ULONGLONG now = GetTickCount64();
    float y = g_settings.show_fps ? 70.0f : 24.0f;
    EnterCriticalSection(&g_toast_lock);
    while (g_ntoasts && now - g_toasts[0].at > 3000) {
        memmove(g_toasts, g_toasts + 1, sizeof g_toasts[0] * (MAX_TOASTS - 1));
        g_ntoasts--;
    }
    for (int i = 0; i < g_ntoasts; i++) {
        float age = (float)(now - g_toasts[i].at), a = age < 150 ? age / 150 : age > 2500 ? (3000 - age) / 500 : 1;
        float w = text_width(FONT_SMALL, g_toasts[i].text);
        float dx = -(w + 120) * (1 - ease_out(age / 260)); /* slides in from the left */
        float saved = g_fade;
        g_fade = a;
        box(24 + dx, y, 24 + dx + w + 44, y + 40, 10, C_PANEL);
        box(24 + dx, y, 32 + dx, y + 40, 10, C_LIME);
        text(FONT_SMALL, 46 + dx, y + 6, 0, C_WHITE, g_toasts[i].text);
        g_fade = saved;
        y += 48 * (age < 200 ? ease_out(age / 200) : 1);
    }
    LeaveCriticalSection(&g_toast_lock);
}

static void draw_fps(void)
{
    static LARGE_INTEGER last, freq;
    static float fps;
    static int frames;
    static ULONGLONG since;
    char s[32];
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        since = GetTickCount64();
    }
    frames++;
    if (GetTickCount64() - since >= 500) { /* average over half a second */
        fps = (float)frames * (float)freq.QuadPart / (float)(now.QuadPart - last.QuadPart);
        frames = 0;
        since = GetTickCount64();
        last = now;
    }
    else if (!last.QuadPart)
        last = now;
    if (!g_settings.show_fps)
        return;
    snprintf(s, sizeof s, "%.0f FPS", fps);
    box(24, 18, 24 + text_width(FONT_SMALL, s) + 32, 56, 10, C_PANEL);
    text(FONT_SMALL, 40, 24, 0, fps >= 58.5f ? C_LIME : C_PINK, s);
}

/* ------------------------------------------------------------------ input */

enum { IN_NONE, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_OK, IN_BACK, IN_CLOSE, IN_CHAR, IN_ERASE };

#define MAX_EVENTS 64
static CRITICAL_SECTION g_event_lock;
static struct { int type; char ch; } g_events[MAX_EVENTS];
static int g_nevents;
static volatile LONG g_open_request; /* F1 */
static volatile LONG g_open;         /* the menu is showing */
static volatile LONG g_swallow;      /* after closing: hide controllers until released */
static int g_text_focus;             /* the bug-report note has the keyboard */

static void push_event(int type, char ch)
{
    EnterCriticalSection(&g_event_lock);
    if (g_nevents < MAX_EVENTS) {
        g_events[g_nevents].type = type;
        g_events[g_nevents].ch = ch;
        g_nevents++;
    }
    LeaveCriticalSection(&g_event_lock);
}

int menu_window_message(UINT msg, WPARAM wp)
{
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F1) {
        if (g_open)
            push_event(IN_CLOSE, 0);
        else
            InterlockedExchange(&g_open_request, 1);
        return 1;
    }
    if (!g_open)
        return 0;
    if (msg == WM_KEYDOWN) {
        switch (wp) {
        case VK_UP: push_event(IN_UP, 0); return 1;
        case VK_DOWN: push_event(IN_DOWN, 0); return 1;
        case VK_LEFT: push_event(IN_LEFT, 0); return 1;
        case VK_RIGHT: push_event(IN_RIGHT, 0); return 1;
        case VK_RETURN: push_event(IN_OK, 0); return 1;
        case VK_ESCAPE: push_event(IN_BACK, 0); return 1;
        case VK_BACK: push_event(g_text_focus ? IN_ERASE : IN_BACK, 0); return 1;
        case VK_SPACE:
            if (!g_text_focus)
                push_event(IN_OK, 0);
            return 1;
        default: return 1;
        }
    }
    if (msg == WM_CHAR) {
        if (g_text_focus && wp >= 32 && wp < 127)
            push_event(IN_CHAR, (char)wp);
        return 1;
    }
    return msg == WM_KEYUP || msg == WM_SYSKEYDOWN;
}

int menu_is_open(void)
{
    return g_open != 0;
}

/* Controllers, read directly from the host: Back + Start opens the menu from any controller;
 * inside, the d-pad or left stick moves (with repeat), A selects, B goes back, Start closes. */
static WORD g_pad_prev[4];
static int g_pad_dir_prev[4];
static ULONGLONG g_repeat_at[4];

static WORD pad_buttons(DWORD port, int *dir)
{
    XINPUT_STATE s;
    WORD b;
    *dir = IN_NONE;
    if (XInputGetState(port, &s) != ERROR_SUCCESS)
        return 0;
    b = s.Gamepad.wButtons;
    if ((b & XINPUT_GAMEPAD_DPAD_UP) || s.Gamepad.sThumbLY > 16000)
        *dir = IN_UP;
    else if ((b & XINPUT_GAMEPAD_DPAD_DOWN) || s.Gamepad.sThumbLY < -16000)
        *dir = IN_DOWN;
    else if ((b & XINPUT_GAMEPAD_DPAD_LEFT) || s.Gamepad.sThumbLX < -16000)
        *dir = IN_LEFT;
    else if ((b & XINPUT_GAMEPAD_DPAD_RIGHT) || s.Gamepad.sThumbLX > 16000)
        *dir = IN_RIGHT;
    return b;
}

static int any_input_held(void)
{
    static const int keys[] = {VK_RETURN, VK_ESCAPE, VK_SPACE, VK_BACK, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', 'X', VK_F1};
    for (DWORD p = 0; p < 4; p++) {
        int dir;
        if (pad_buttons(p, &dir) & ~0x0Fu || dir != IN_NONE)
            return 1;
    }
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (GetAsyncKeyState(keys[i]) & 0x8000)
            return 1;
    return 0;
}

static void poll_pads(void)
{
    ULONGLONG now = GetTickCount64();
    for (DWORD p = 0; p < 4; p++) {
        int dir;
        WORD b = pad_buttons(p, &dir), pressed = (WORD)(b & ~g_pad_prev[p]);
        if (pressed & XINPUT_GAMEPAD_A)
            push_event(IN_OK, 0);
        if (pressed & XINPUT_GAMEPAD_B)
            push_event(IN_BACK, 0);
        if (pressed & XINPUT_GAMEPAD_START)
            push_event(IN_CLOSE, 0);
        if (dir != IN_NONE && (dir != g_pad_dir_prev[p] || now >= g_repeat_at[p])) {
            push_event(dir, 0);
            g_repeat_at[p] = now + (dir != g_pad_dir_prev[p] ? 400 : 110);
        }
        g_pad_dir_prev[p] = dir;
        g_pad_prev[p] = b;
    }
}

/* Back + Start on any controller (checked every frame while the game runs). */
static int pad_open_combo(void)
{
    int open = 0;
    for (DWORD p = 0; p < 4; p++) {
        int dir;
        WORD b = pad_buttons(p, &dir);
        WORD both = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
        if ((b & both) == both && (g_pad_prev[p] & both) != both)
            open = 1;
        g_pad_prev[p] = b;
        g_pad_dir_prev[p] = dir;
    }
    return open;
}

int menu_filter_pad(unsigned port, unsigned short *buttons)
{
    (void)port;
    if (g_open || g_swallow)
        return 1;
    if (*buttons & 0x20) /* Back held: Start belongs to the menu */
        *buttons &= (unsigned short)~0x10;
    return 0;
}

/* ------------------------------------------------------------------ pages */

enum { PAGE_MAIN, PAGE_DISPLAY, PAGE_AUDIO, PAGE_GAME, PAGE_CONTROLS, PAGE_REPORT, PAGE_QUIT, PAGE_COUNT };
enum { IT_ACTION, IT_CHOICE, IT_TOGGLE, IT_SLIDER, IT_TEXT, IT_INFO };

typedef struct Item {
    int type;
    const char *label;
    int *value;            /* choice index / toggle / slider value */
    const char *const *choices;
    int nchoices;
    int restart;           /* applies on the next start */
    int target;            /* action: page to open, -1 close the menu, -2 back */
    void (*changed)(void); /* after a change */
    const char *help;
} Item;

static int g_page, g_sel[PAGE_COUNT];
static int g_stack[8], g_depth;

/* Animation state (times from now_ms). */
static double g_t_open, g_t_close, g_t_page, g_t_sel, g_t_value;
static int g_page_dir = 1, g_value_dir, g_closing;
static float g_sel_pos; /* the selection bar's row, gliding towards the selected one */

/* Choice lists and the indices the menu edits (synced with g_settings). */
static const int RES[][2] = {{640, 480}, {960, 720}, {1280, 960}, {1440, 1080}, {1600, 1200},
                             {1920, 1440}, {2560, 1920}, {2880, 2160}};
static const char *const RES_NAMES[] = {"640 x 480", "960 x 720", "1280 x 960", "1440 x 1080", "1600 x 1200",
                                        "1920 x 1440", "2560 x 1920", "2880 x 2160"};
static const int WIN_H[] = {480, 600, 720, 840, 960, 1080, 1200, 1440};
static const char *const WIN_NAMES[] = {"640 x 480", "800 x 600", "960 x 720", "1120 x 840", "1280 x 960",
                                        "1440 x 1080", "1600 x 1200", "1920 x 1440"};
static const int ANISO[] = {1, 2, 4, 8, 16};
static const char *const ANISO_NAMES[] = {"Original", "2x", "4x", "8x", "16x"};
static const char *const LANG_NAMES[] = {"Japanese", "English", "French", "German", "Spanish", "Italian"};
static const char *const REPORT_KINDS[] = {"Graphics", "Audio", "Controls", "Gameplay", "Crash or freeze",
                                           "Slow or stutter", "Other"};
static const char *const ONOFF[] = {"Off", "On"};

static int g_res_i, g_win_i, g_aniso_i, g_kind_i;
static char g_note[160];

static int index_of(const int *list, int n, int v)
{
    int best = 0;
    for (int i = 0; i < n; i++)
        if (list[i] <= v)
            best = i;
    return best;
}

static void sync_from_settings(void)
{
    g_res_i = 2;
    for (int i = 0; i < (int)(sizeof RES / sizeof RES[0]); i++)
        if (RES[i][0] == g_settings.width && RES[i][1] == g_settings.height)
            g_res_i = i;
    g_win_i = index_of(WIN_H, (int)(sizeof WIN_H / sizeof WIN_H[0]), g_settings.window_height);
    g_aniso_i = index_of(ANISO, (int)(sizeof ANISO / sizeof ANISO[0]), g_settings.anisotropy);
}

/* Resolution or VSync: reset the device now. The menu's state block belongs to the old device
 * state; the atlas is rebuilt for the new size on the next draw. */
static void changed_display(void)
{
    g_settings.width = RES[g_res_i][0];
    g_settings.height = RES[g_res_i][1];
    settings_save();
    if (g_state) {
        IDirect3DStateBlock9_Release(g_state);
        g_state = NULL;
    }
    if (g_host.change_display)
        g_host.change_display();
    sync_from_settings(); /* a failed reset keeps the old resolution */
    if (g_host.apply_window)
        g_host.apply_window();
}
static void changed_window(void)
{
    g_settings.window_height = WIN_H[g_win_i];
    settings_save();
    if (g_host.apply_window)
        g_host.apply_window();
}
static void changed_aniso(void)
{
    g_settings.anisotropy = ANISO[g_aniso_i];
    settings_save();
}
static void changed_save(void) { settings_save(); }

static void report_save(void);
static void quit_now(void);

#define ACTION(label, page, fn, help) {IT_ACTION, label, NULL, NULL, 0, 0, page, fn, help}
#define CHOICE(label, var, list, restart, fn, help) \
    {IT_CHOICE, label, &var, list, (int)(sizeof list / sizeof list[0]), restart, -1, fn, help}
#define TOGGLE(label, var, restart, fn, help) {IT_TOGGLE, label, &var, ONOFF, 2, restart, -1, fn, help}
#define INFO(label, value) {IT_INFO, label, NULL, NULL, 0, 0, -1, NULL, value}

static const char *const PAGE_TITLE[PAGE_COUNT] = {"PAUSED", "DISPLAY", "AUDIO", "GAME", "CONTROLS", "REPORT A BUG",
                                                   "QUIT"};

static Item g_main[] = {
    ACTION("Resume", -1, NULL, "Back to the game."),
    ACTION("Report a bug", PAGE_REPORT, NULL, "Save a capture of this moment with a note, for the developers."),
    ACTION("Display", PAGE_DISPLAY, NULL, "Resolution, window, texture filtering, frame-rate counter."),
    ACTION("Audio", PAGE_AUDIO, NULL, "Volume."),
    ACTION("Game", PAGE_GAME, NULL, "Language."),
    ACTION("Controls", PAGE_CONTROLS, NULL, "Controller and keyboard layout."),
    ACTION("Quit ReJet", PAGE_QUIT, NULL, "Close the game. Save in the game first."),
};
static Item g_display[] = {
    CHOICE("Resolution", g_res_i, RES_NAMES, 0, changed_display,
           "How sharp the game is drawn (the Xbox drew 640 x 480)."),
    CHOICE("Window size", g_win_i, WIN_NAMES, 0, changed_window,
           "How big the window is (fitted to the screen). Full screen ignores it."),
    TOGGLE("Full screen", g_settings.fullscreen, 0, NULL, "Borderless window over the whole monitor."),
    CHOICE("Texture filtering", g_aniso_i, ANISO_NAMES, 0, changed_aniso, "Sharper textures at an angle."),
    TOGGLE("Show FPS", g_settings.show_fps, 0, changed_save, "Frame-rate counter in the top left corner."),
    TOGGLE("VSync", g_settings.vsync, 0, changed_display,
           "Also wait for the monitor. Off is smoother: the game already runs at 60."),
};
static Item g_audio[] = {
    {IT_SLIDER, "Volume", &g_settings.volume, NULL, 0, 0, -1, changed_save, "Master volume."},
    TOGGLE("Mute in background", g_settings.mute_in_background, 0, changed_save,
           "Silent while another program has the focus."),
};
static Item g_game[] = {
    CHOICE("Language", g_settings.language, LANG_NAMES, 1, changed_save, "Text and voice language."),
};
static Item g_controls[] = {
    INFO("ReJet menu", "F1, or Back + Start"),
    INFO("Move", "Left stick / WASD"),
    INFO("Jump (A)", "A / Space or Z"),
    INFO("Boost, spray (B)", "B / X"),
    INFO("X / Y", "X, Y / C, V"),
    INFO("Triggers", "LT, RT / Q, E"),
    INFO("Start, Back", "Start, Back / Enter, Backspace"),
    INFO("D-pad", "D-pad / arrow keys"),
    INFO("Screenshot, capture, marker", "F12, F11, F10"),
};
static Item g_report[] = {
    CHOICE("What went wrong", g_kind_i, REPORT_KINDS, 0, NULL, "Pick the closest."),
    {IT_TEXT, "Note", NULL, NULL, 0, 0, -1, NULL, "Optional: type what happened (keyboard)."},
    ACTION("Save report", -1, report_save, "Saves a capture of the game screen with your note."),
    ACTION("Cancel", -2, NULL, NULL),
};
static Item g_quit[] = {
    ACTION("No, keep playing", -2, NULL, NULL),
    ACTION("Yes, quit", -1, quit_now, "Unsaved progress is lost."),
};

static Item *page_items(int page, int *n)
{
    switch (page) {
#define P(arr) *n = (int)(sizeof arr / sizeof arr[0]); return arr
    case PAGE_MAIN: P(g_main);
    case PAGE_DISPLAY: P(g_display);
    case PAGE_AUDIO: P(g_audio);
    case PAGE_GAME: P(g_game);
    case PAGE_CONTROLS: P(g_controls);
    case PAGE_REPORT: P(g_report);
    default: P(g_quit);
#undef P
    }
}

static void start_close(const char *why)
{
    if (g_closing)
        return;
    log_printf("menu: close (%s)\n", why);
    g_closing = 1;
    g_t_close = now_ms();
    g_text_focus = 0;
}

static void open_page(int page, int dir)
{
    g_page = page;
    g_page_dir = dir;
    g_t_page = g_t_sel = now_ms();
    g_sel_pos = (float)g_sel[page];
}

static void go_back(void)
{
    if (g_depth)
        open_page(g_stack[--g_depth], -1);
    else
        start_close("back from the main page");
}

static void report_save(void)
{
    char note[256];
    snprintf(note, sizeof note, "%s: %s", REPORT_KINDS[g_kind_i], g_note[0] ? g_note : "(no note)");
    if (g_host.request_capture)
        g_host.request_capture(note);
    log_printf("menu: bug report: %s\n", note);
    g_note[0] = 0;
    g_kind_i = 0;
}

static void quit_now(void)
{
    log_printf("menu: quit\n");
    settings_save();
    PostMessageW(g_host.wnd, WM_CLOSE, 0, 0);
}

static void apply_full_screen(void)
{
    settings_save();
    if (g_host.apply_window)
        g_host.apply_window();
}

static void handle(int ev, char ch)
{
    int n;
    Item *items = page_items(g_page, &n), *it;
    int *sel = &g_sel[g_page];
    if (g_closing)
        return;
    if (*sel >= n)
        *sel = 0;
    it = &items[*sel];
    switch (ev) {
    case IN_UP:
    case IN_DOWN:
        *sel = (*sel + (ev == IN_UP ? n - 1 : 1)) % n;
        if (abs(*sel - (int)(g_sel_pos + 0.5f)) > 1) /* wrapped round: jump, don't glide across */
            g_sel_pos = (float)*sel;
        g_t_sel = now_ms();
        break;
    case IN_LEFT:
    case IN_RIGHT: {
        int d = ev == IN_LEFT ? -1 : 1;
        if (it->type == IT_CHOICE || it->type == IT_TOGGLE) {
            *it->value = (*it->value + d + it->nchoices) % it->nchoices;
            g_value_dir = d;
            g_t_value = now_ms();
            if (it->value == &g_settings.fullscreen)
                apply_full_screen();
            else if (it->changed)
                it->changed();
        }
        else if (it->type == IT_SLIDER) {
            int v = *it->value + d * 10;
            *it->value = v < 0 ? 0 : v > 100 ? 100 : v;
            g_value_dir = d;
            g_t_value = now_ms();
            if (it->changed)
                it->changed();
        }
        break;
    }
    case IN_OK:
        if (it->type == IT_ACTION) {
            if (it->changed)
                it->changed();
            if (it->target == -1)
                start_close(it->changed == report_save ? "bug report saved" : "resume");
            else if (it->target == -2)
                go_back();
            else if (g_depth < 8) {
                g_stack[g_depth++] = g_page;
                g_sel[it->target] = 0;
                open_page(it->target, 1);
            }
        }
        else if (it->type == IT_TOGGLE || it->type == IT_CHOICE)
            handle(IN_RIGHT, 0);
        else if (it->type == IT_TEXT)
            handle(IN_DOWN, 0);
        break;
    case IN_BACK:
        go_back();
        break;
    case IN_CLOSE:
        start_close("Start / F1");
        break;
    case IN_CHAR:
        if (it->type == IT_TEXT && strlen(g_note) + 1 < sizeof g_note) {
            size_t l = strlen(g_note);
            g_note[l] = ch;
            g_note[l + 1] = 0;
        }
        break;
    case IN_ERASE:
        if (it->type == IT_TEXT && g_note[0])
            g_note[strlen(g_note) - 1] = 0;
        break;
    }
    items = page_items(g_page, &n);
    g_text_focus = !g_closing && items[g_sel[g_page] < n ? g_sel[g_page] : 0].type == IT_TEXT;
}

/* ------------------------------------------------------------------ menu drawing */

static IDirect3DTexture9 *g_frozen; /* the last game frame, behind the menu */

/* How open the menu is, 0..1 (opening, then closing). */
static float openness(void)
{
    float o = ease_out(progress(g_t_open, 280));
    if (g_closing)
        o *= 1 - ease_out(progress(g_t_close, 180));
    return o;
}

static void draw_frozen(float o)
{
    BYTE k = (BYTE)(255 - (255 - 0x5A) * o); /* darkens as the menu opens */
    DWORD c = 0xFF000000u | (DWORD)k << 16 | (DWORD)k << 8 | k;
    Vertex v[6];
    float w = (float)g_bw - 0.5f, h = (float)g_bh - 0.5f;
    if (!g_frozen)
        return;
    v[0] = (Vertex){-0.5f, -0.5f, 0, 1, c, 0, 0};
    v[1] = (Vertex){w, -0.5f, 0, 1, c, 1, 0};
    v[2] = (Vertex){-0.5f, h, 0, 1, c, 0, 1};
    v[3] = v[1];
    v[4] = (Vertex){w, h, 0, 1, c, 1, 1};
    v[5] = v[2];
    IDirect3DDevice9Ex_SetTexture(DEV, 0, (IDirect3DBaseTexture9 *)g_frozen);
    IDirect3DDevice9Ex_SetFVF(DEV, FVF);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9Ex_SetRenderState(DEV, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9Ex_SetVertexShader(DEV, NULL);
    IDirect3DDevice9Ex_SetPixelShader(DEV, NULL);
    IDirect3DDevice9Ex_SetSamplerState(DEV, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9Ex_SetSamplerState(DEV, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9Ex_SetTextureStageState(DEV, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9Ex_BeginScene(DEV);
    IDirect3DDevice9Ex_DrawPrimitiveUP(DEV, D3DPT_TRIANGLELIST, 2, v, sizeof(Vertex));
    IDirect3DDevice9Ex_EndScene(DEV);
}

/* Slow diagonal stripes drifting across the background, JSRF poster style. */
static void draw_stripes(double t)
{
    static const DWORD col[] = {0x14C8F01Eu, 0x10FF2D8Au, 0x0E19C8C8u, 0x12C8F01Eu, 0x0CFF2D8Au};
    for (int i = 0; i < 5; i++) {
        float w = 90.0f + 50.0f * (float)(i % 3);
        float x = (float)fmod(t * (0.025 + 0.008 * i) + i * 410.0, 2200.0) - 600.0f;
        box(x, -10, x + w, 970, 380, col[i]);
    }
}

static void draw_menu(void)
{
    int n;
    Item *items = page_items(g_page, &n);
    int sel = g_sel[g_page] < n ? g_sel[g_page] : 0;
    float y0 = 250, row = 62;
    double t = now_ms();
    float o = openness(), slide = 1 - o;
    float pg = progress(g_t_page, 160), saved_fade = g_fade;
    int restart_note = 0;

    g_fade = o;
    draw_stripes(t);

    /* title strip: slides in from the left; the pink underline grows */
    {
        float dx = -1000 * slide, tx, right;
        /* the strip fits the logo (or "ReJet") and the longest page name */
        tx = g_logo_px ? 64 + 76 * g_logo_aspect + 30 : 360;
        right = tx + text_width(FONT_ITEM, "REPORT A BUG") + 60;
        if (right < 700)
            right = 700;
        box(-40 + dx, 70, right + dx, 190, 40, C_LIME);
        for (int i = 0; i < 4; i++) { /* moving speed lines inside the strip */
            float sx = 120 + (float)fmod(t * 0.12 + i * 150.0, (double)right - 140.0);
            box(sx + dx, 78, sx + 26 + dx, 182, 40, 0x22000000u);
        }
        box(-40 + dx, 182, -40 + dx + (right - 20) * ease_out(progress(g_t_open + 120, 300)), 196, 40, C_PINK);
        if (g_logo_px)
            logo(64 + dx, 92, 76, C_WHITE); /* centred in the strip */
        else
            text(FONT_TITLE, 70 + dx, 82, 0, C_BLACK, "ReJet");
        /* page name drops in on every page change */
        g_fade = o * ease_out(pg);
        text(FONT_ITEM, tx + dx, 116 - 18 * (1 - ease_out(pg)), 0, C_BLACK, PAGE_TITLE[g_page]);
        g_fade = o;
    }

    /* the list panel slides in from the right */
    {
        float dx = 900 * slide * slide;
        float target = (float)sel, k = 1 - (float)exp(-(1000.0 / 60.0) / 55.0);
        box(90 + dx, y0 - 26, 1210 + dx, y0 + row * n + 14, 24, C_PANEL);
        g_sel_pos += (target - g_sel_pos) * k; /* the selection bar glides */
        if (fabsf(target - g_sel_pos) < 0.002f)
            g_sel_pos = target;
        {
            float y = y0 + row * g_sel_pos, pulse = 0.88f + 0.12f * (float)sin(t * 0.006);
            float grow = ease_back(progress(g_t_sel, 220));
            box(110 + dx, y - 6, 110 + dx + 1080 * (0.9f + 0.1f * grow), y + row - 14, 14, with_alpha(C_LIME, pulse));
            box(110 + dx, y - 6, 122 + dx, y + row - 14, 14, C_PINK);
        }
        for (int i = 0; i < n; i++) {
            const Item *it = &items[i];
            /* rows arrive one after another, from the side the page came from */
            float rp = progress(g_t_page + 24.0 * i, 240);
            float rx = dx + 70 * (float)g_page_dir * (1 - ease_back(rp)), y = y0 + row * i;
            int on = i == sel;
            DWORD fg = on ? C_BLACK : (it->type == IT_INFO ? C_GREY : C_WHITE);
            char val[200] = "";
            g_fade = o * ease_out(rp);
            text(FONT_ITEM, 140 + rx, y, 0, fg, it->label);
            switch (it->type) {
            case IT_CHOICE:
            case IT_TOGGLE:
                snprintf(val, sizeof val, on ? "<  %s%s  >" : "%s%s", it->choices[*it->value],
                         it->restart ? " *" : "");
                break;
            case IT_SLIDER: {
                float x0 = 820 + rx, x1 = 1110 + rx, f = (float)*it->value / 100.0f;
                box(x0, y + 14, x1, y + 30, 6, on ? 0x66000000u : 0x55FFFFFFu);
                box(x0, y + 14, x0 + (x1 - x0) * f, y + 30, 6, on ? C_BLACK : C_TEAL);
                snprintf(val, sizeof val, "%d", *it->value);
                text(FONT_SMALL, 1170 + rx, y + 6, 2, fg, val);
                val[0] = 0;
                break;
            }
            case IT_TEXT:
                snprintf(val, sizeof val, "%s%s", g_note[0] ? g_note : (on ? "" : "(optional)"),
                         on && fmod(t, 1000.0) < 500.0 ? "_" : "");
                break;
            case IT_INFO:
                snprintf(val, sizeof val, "%s", it->help);
                break;
            }
            if (it->restart)
                restart_note = 1;
            if (val[0]) {
                int font = it->type == IT_TEXT || it->type == IT_INFO ? FONT_SMALL : FONT_ITEM;
                float maxw = it->type == IT_TEXT ? 760.0f : 600.0f, w = text_width(font, val);
                float nudge = on ? 16.0f * (float)g_value_dir * (1 - ease_out(progress(g_t_value, 200))) : 0;
                const char *v = val;
                while (w > maxw && *v) /* long notes: show the end */
                    w -= text_width(font, (char[]){*v++, 0});
                text(font, 1170 + rx + nudge, y + (font == FONT_SMALL ? 6 : 0), 2, fg, v);
            }
        }
        g_fade = o;
    }

    /* help line for the selected row (fades in), and the footer (rises in) */
    {
        const Item *it = &items[sel];
        float y = y0 + row * n + 34;
        g_fade = o * ease_out(progress(g_t_sel, 200));
        if (it->help && it->type != IT_INFO)
            text(FONT_SMALL, 110, y, 0, C_WHITE, it->help);
        g_fade = o;
        if (restart_note)
            text(FONT_SMALL, 110, y + 34, 0, C_GREY, "* applies the next time ReJet starts");
    }
    {
        float dy = 80 * slide;
        box(-40, 880 + dy, 1320, 930 + dy, 0, C_SHADE);
        text(FONT_SMALL, 640, 891 + dy, 1, C_WHITE,
             g_text_focus ? "Type your note   Enter / Down  next   Esc  back"
                          : "A / Enter  select     B / Esc  back     Left / Right  change     Start / F1  resume");
    }
    g_fade = saved_fade;
}

/* ------------------------------------------------------------------ the loop */

static void freeze_frame(void)
{
    IDirect3DSurface9 *bb = NULL, *dst = NULL;
    if (g_frozen) {
        IDirect3DTexture9_Release(g_frozen);
        g_frozen = NULL;
    }
    if (FAILED(IDirect3DDevice9Ex_CreateTexture(DEV, g_bw, g_bh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                                D3DPOOL_DEFAULT, &g_frozen, NULL)))
        return;
    IDirect3DDevice9Ex_GetBackBuffer(DEV, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    IDirect3DTexture9_GetSurfaceLevel(g_frozen, 0, &dst);
    if (bb && dst)
        IDirect3DDevice9Ex_StretchRect(DEV, bb, NULL, dst, NULL, D3DTEXF_NONE);
    if (dst)
        IDirect3DSurface9_Release(dst);
    if (bb)
        IDirect3DSurface9_Release(bb);
}

static void run_menu(void)
{
    log_printf("menu: opened\n");
    sync_from_settings();
    freeze_frame();
    g_depth = 0;
    g_closing = 0;
    g_sel[PAGE_MAIN] = 0;
    g_t_open = now_ms();
    open_page(PAGE_MAIN, 1);
    EnterCriticalSection(&g_event_lock);
    g_nevents = 0;
    LeaveCriticalSection(&g_event_lock);
    for (DWORD p = 0; p < 4; p++) { /* the buttons that opened it don't count */
        int dir;
        g_pad_prev[p] = pad_buttons(p, &dir);
        g_pad_dir_prev[p] = dir;
        g_repeat_at[p] = GetTickCount64() + 400;
    }
    InterlockedExchange(&g_open, 1);
    for (;;) {
        IDirect3DStateBlock9 *saved = NULL;
        poll_pads();
        for (;;) {
            int ev;
            char ch;
            EnterCriticalSection(&g_event_lock);
            if (!g_nevents) {
                LeaveCriticalSection(&g_event_lock);
                break;
            }
            ev = g_events[0].type;
            ch = g_events[0].ch;
            memmove(g_events, g_events + 1, sizeof g_events[0] * (size_t)--g_nevents);
            LeaveCriticalSection(&g_event_lock);
            handle(ev, ch);
        }
        if (g_closing && progress(g_t_close, 180) >= 1)
            break;
        ready();
        if (SUCCEEDED(IDirect3DDevice9Ex_CreateStateBlock(DEV, D3DSBT_ALL, &saved))) {
            draw_frozen(openness());
            IDirect3DStateBlock9_Apply(saved);
            IDirect3DStateBlock9_Release(saved);
        }
        draw_menu();
        draw_toasts();
        flush();
        g_host.present();
        Sleep(15);
    }
    g_text_focus = 0;
    if (g_frozen) {
        IDirect3DTexture9_Release(g_frozen);
        g_frozen = NULL;
    }
    InterlockedExchange(&g_open, 0);
    InterlockedExchange(&g_swallow, 1);
    log_printf("menu: closed\n");
}

void menu_frame(void)
{
    if (!DEV || !ready())
        return;
    if (g_swallow && !any_input_held())
        InterlockedExchange(&g_swallow, 0);
    {
        int key = InterlockedExchange(&g_open_request, 0) != 0, pad = pad_open_combo();
        if (key || pad) {
            log_printf("menu: open (%s)\n", key ? "F1" : "Back + Start");
            run_menu();
        }
    }
    draw_fps();
    draw_toasts();
    flush();
}

void menu_init(const MenuHost *host)
{
    static int once;
    if (!once) {
        InitializeCriticalSection(&g_toast_lock);
        InitializeCriticalSection(&g_event_lock);
        load_logo();
        once = 1;
    }
    g_host = *host;
    sync_from_settings();
    menu_toast("ReJet menu: F1, or Back + Start on a controller");
}
