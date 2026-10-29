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
#include "ui/hud.h"
#include "ui/menu.h"
#include "version.gen.h"

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
/* the Garage look (draw_menu) */
#define C_FIELD   0xFFFFC21Au
#define C_ORANGE  0xFFF39A1Eu
#define C_CYAN    0xFF37B8C8u
#define C_INK     0xFF111111u

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
    float left;           /* margin left of the pen position (overhangs) */
} Glyph;

static Glyph g_glyph[FONT_COUNT][LAST_CHAR - FIRST_CHAR + 1];
static float g_font_px[FONT_COUNT]; /* line height in back-buffer pixels */
static float g_white_u, g_white_v;  /* a white texel, for solid shapes */

/* Controller button shapes, drawn into the atlas at the menu's scale (sharp at any resolution,
 * no image files) and tinted per button: face buttons are circles, bumpers and triggers pills,
 * plus the d-pad. */
enum { SHAPE_CIRCLE, SHAPE_PILL, SHAPE_CROSS, SHAPE_COUNT };
typedef struct Shape {
    float u0, v0, u1, v1, w, h;
} Shape;
static Shape g_shape[SHAPE_COUNT];

/* Signed distance to a box of half size (a, b) with rounded corners of radius r. */
static float sdf_box(float px, float py, float a, float b, float r)
{
    float qx = fabsf(px) - (a - r), qy = fabsf(py) - (b - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}
static IDirect3DTexture9 *g_atlas;
static UINT g_bw, g_bh;             /* back-buffer size the atlas was built for */
static float g_scale, g_ox;         /* canvas -> back buffer: x' = g_ox + x * g_scale */

/* ------------------------------------------------------------------ logo */

/* The ReJet logo: rejet_logo.png next to ReJet.exe, else the copy built into rejet.dll
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

/* The logo for the setup screen (ui/setup.c): premultiplied BGRA, loaded on first use. */
int menu_logo(const DWORD **px, UINT *w, UINT *h)
{
    static int tried;
    if (!tried++)
        load_logo();
    *px = g_logo_px;
    *w = g_logo_w;
    *h = g_logo_h;
    return g_logo_px != NULL;
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
        /* Bahnschrift: Windows' own condensed DIN face (Windows 10 and later), nothing to ship */
        {L"Bahnschrift", 64, FW_BOLD, 0},
        {L"Bahnschrift SemiBold Condensed", 34, FW_NORMAL, 0},
        {L"Bahnschrift", 24, FW_NORMAL, 0},
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
    { /* room for the button shapes, bottom right (above the white block), one icon tall */
        int s = (int)(44.0f * g_scale + 0.5f), sx = ATLAS - 12, top = ATLAS - 12 - s;
        static const int widths[SHAPE_COUNT] = {1, 2, 1}; /* circle, pill, cross (in icon heights) */
        for (int k = SHAPE_COUNT - 1; k >= 0; k--) {
            int w = s * widths[k];
            sx -= w + 6;
            g_shape[k].u0 = (float)sx / ATLAS;
            g_shape[k].v0 = (float)top / ATLAS;
            g_shape[k].u1 = (float)(sx + w) / ATLAS;
            g_shape[k].v1 = (float)(top + s) / ATLAS;
            g_shape[k].w = (float)w;
            g_shape[k].h = (float)s;
        }
        if (top - 4 < logo_top)
            logo_top = top - 4; /* fonts stop above them too */
    }
    for (int f = 0; f < FONT_COUNT; f++) {
        int px = (int)((float)fonts[f].size * g_scale + 0.5f);
        HFONT h = CreateFontW(-px, 0, 0, 0, fonts[f].weight, fonts[f].italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, fonts[f].face);
        HGDIOBJ old = SelectObject(dc, h);
        TEXTMETRICW tm;
        int pad = (fonts[f].italic ? px / 3 : px / 8) + 3; /* margin both sides: overhangs and filtering */
        GetTextMetricsW(dc, &tm);
        g_font_px[f] = (float)tm.tmHeight;
        for (int c = FIRST_CHAR; c <= LAST_CHAR; c++) {
            WCHAR wc = (WCHAR)c;
            SIZE sz;
            int cw;
            Glyph *g = &g_glyph[f][c - FIRST_CHAR];
            memset(g, 0, sizeof *g);
            if (f == FONT_TITLE && !strchr("ReJt", c)) /* the title font only writes "ReJet" (no logo) */
                continue;
            GetTextExtentPoint32W(dc, &wc, 1, &sz);
            cw = sz.cx + 2 * pad;
            if (x + cw > ATLAS) {
                x = 0;
                y += row + 6;
                row = 0;
            }
            if (y + tm.tmHeight > logo_top - 4)
                break;
            TextOutW(dc, x + pad, y, &wc, 1);
            g->u0 = (float)x / ATLAS;
            g->v0 = (float)y / ATLAS;
            g->u1 = (float)(x + cw) / ATLAS;
            g->v1 = (float)(y + tm.tmHeight) / ATLAS;
            g->w = (float)cw;
            g->h = (float)tm.tmHeight;
            g->adv = (float)sz.cx;
            x += cw + 6; /* gaps keep neighbours out of the bilinear filter */
            g->left = (float)pad;
            if (tm.tmHeight > row)
                row = tm.tmHeight;
        }
        SelectObject(dc, old);
        DeleteObject(h);
    }
    GdiFlush();
    /* controller button shapes (white coverage, tinted when drawn), bottom right */
    for (int k = 0; k < SHAPE_COUNT; k++) {
        Shape *sh = &g_shape[k];
        int w = (int)sh->w, h = (int)sh->h, x0 = (int)(sh->u0 * ATLAS), y0 = (int)(sh->v0 * ATLAS);
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                float px = (float)i + 0.5f - (float)w * 0.5f, py = (float)j + 0.5f - (float)h * 0.5f, d;
                if (k == SHAPE_CROSS) { /* d-pad: two rounded bars */
                    float arm = (float)h * 0.5f - 1, bar = (float)h * 0.17f;
                    d = sdf_box(px, py, arm, bar, bar * 0.4f);
                    d = fminf(d, sdf_box(px, py, bar, arm, bar * 0.4f));
                }
                else { /* circle or pill: a box with fully rounded ends */
                    float hh = (float)h * 0.5f - 1;
                    d = sdf_box(px, py, (float)w * 0.5f - 1, hh, hh);
                }
                bits[(y0 + j) * ATLAS + x0 + i] = (DWORD)(clamp01(0.5f - d) * 255.0f);
            }
    }
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
/* The screen's edges on the canvas (beyond 0..1280 on wide screens). */
static float canvas_left(void) { return -g_ox / g_scale; }
static float canvas_right(void) { return 1280.0f + g_ox / g_scale; }
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

/* A solid triangle on the canvas (the menu's icons). */
static void tri(float ax, float ay, float bx, float by, float cx, float cy, DWORD c)
{
    Vertex *v = g_v + g_nv;
    DWORD col = with_alpha(c, g_fade);
    if (g_nv + 3 > MAX_VERTS)
        return;
    v[0] = (Vertex){X(ax) - 0.5f, Y(ay) - 0.5f, 0, 1, col, g_white_u, g_white_v};
    v[1] = (Vertex){X(bx) - 0.5f, Y(by) - 0.5f, 0, 1, col, g_white_u, g_white_v};
    v[2] = (Vertex){X(cx) - 0.5f, Y(cy) - 0.5f, 0, 1, col, g_white_u, g_white_v};
    g_nv += 3;
}

static float text_width(int font, const char *s)
{
    float w = 0;
    for (; *s; s++)
        if ((unsigned char)*s >= FIRST_CHAR && (unsigned char)*s <= LAST_CHAR)
            w += g_glyph[font][(unsigned char)*s - FIRST_CHAR].adv;
    return w / g_scale; /* canvas units */
}

/* Text on the canvas at (x, y) = top left; align 0 left, 1 centre, 2 right. Light text gets a
 * drop shadow; dark text (ink on the yellow field) none, where it would only smear the letters. */
static void text(int font, float x, float y, int align, DWORD c, const char *s)
{
    float px, py = Y(y), w = text_width(font, s);
    int light = ((c >> 16 & 0xFF) * 3 + (c >> 8 & 0xFF) * 6 + (c & 0xFF)) / 10 > 128;
    if (align == 1)
        x -= w * 0.5f;
    else if (align == 2)
        x -= w;
    for (int pass = light ? 0 : 1; pass < 2; pass++) {
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
                quad_px(px + off - g->left, py + off, px + off - g->left + g->w, py + off + g->h, 0, col, g->u0, g->v0,
                        g->u1, g->v1);
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

/* Controller buttons as icons, `h` canvas units tall at (x, y); return the width. Face buttons
 * in their Xbox colours with the letter; bumpers, triggers and the d-pad in dark grey; Menu
 * (Start) and View (Back) with their symbols. */
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LB, BTN_RB, BTN_LT, BTN_RT, BTN_START, BTN_BACK, BTN_DPAD };

static float button_width(int b, float h)
{
    return b >= BTN_LB && b <= BTN_RT ? h * 2 : h;
}

static void shape(int k, float x, float y, float w, float h, DWORD c)
{
    const Shape *s = &g_shape[k];
    quad_px(X(x), Y(y), X(x + w), Y(y + h), 0, with_alpha(c, g_fade), s->u0, s->v0, s->u1, s->v1);
}

static float button_icon(int b, float x, float y, float h)
{
    static const DWORD face[4] = {0xFF5DBB3Fu, 0xFFE0453Cu, 0xFF3D8FE0u, 0xFFF2C12Eu};
    static const char *const letter[4] = {"A", "B", "X", "Y"};
    static const char *const pill[4] = {"LB", "RB", "LT", "RT"};
    const DWORD dark = 0xFF3A4446u;
    float w = button_width(b, h), fh = g_font_px[FONT_SMALL] / g_scale;
    if (b <= BTN_Y) {
        shape(SHAPE_CIRCLE, x, y, h, h, face[b]);
        text(FONT_SMALL, x + h / 2, y + (h - fh) / 2, 1, b == BTN_Y ? 0xFF1A1A1Au : C_WHITE, letter[b]);
    }
    else if (b <= BTN_RT) {
        shape(SHAPE_PILL, x, y, w, h, dark);
        text(FONT_SMALL, x + w / 2, y + (h - fh) / 2, 1, C_WHITE, pill[b - BTN_LB]);
    }
    else if (b == BTN_START) { /* Menu: three lines */
        shape(SHAPE_CIRCLE, x, y, h, h, dark);
        for (int i = 0; i < 3; i++)
            box(x + h * 0.3f, y + h * (0.32f + 0.16f * i), x + h * 0.7f, y + h * (0.32f + 0.16f * i) + h * 0.07f, 0,
                C_WHITE);
    }
    else if (b == BTN_BACK) { /* View: two overlapping squares */
        shape(SHAPE_CIRCLE, x, y, h, h, dark);
        box(x + h * 0.36f, y + h * 0.28f, x + h * 0.68f, y + h * 0.56f, 0, C_WHITE);
        box(x + h * 0.30f, y + h * 0.44f, x + h * 0.62f, y + h * 0.72f, 0, 0xFFBFC6C6u);
    }
    else
        shape(SHAPE_CROSS, x, y, h, h, 0xFFD8DEDEu);
    return w;
}

/* Is a controller connected? (checked every 1.5 s: asking about empty ports is slow) */
static int pad_connected(void)
{
    static ULONGLONG checked;
    static int connected, forced = -1;
    ULONGLONG now = GetTickCount64();
    if (forced < 0) /* REJET_PAD_ICONS=1: as if a controller were connected (tests) */
        forced = GetEnvironmentVariableA("REJET_PAD_ICONS", NULL, 0) > 0;
    if (forced)
        return 1;
    if (now - checked > 1500) {
        XINPUT_STATE s;
        checked = now;
        connected = 0;
        for (DWORD p = 0; p < 4 && !connected; p++)
            connected = XInputGetState(p, &s) == ERROR_SUCCESS;
    }
    return connected;
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
static struct { char text[128]; ULONGLONG at, ms; } g_toasts[MAX_TOASTS];
static int g_ntoasts;

static void toast_add(ULONGLONG ms, const char *fmt, va_list ap)
{
    if (!g_host.dev)
        return;
    EnterCriticalSection(&g_toast_lock);
    if (g_ntoasts == MAX_TOASTS) {
        memmove(g_toasts, g_toasts + 1, sizeof g_toasts[0] * (MAX_TOASTS - 1));
        g_ntoasts--;
    }
    vsnprintf(g_toasts[g_ntoasts].text, sizeof g_toasts[0].text, fmt, ap);
    g_toasts[g_ntoasts].at = GetTickCount64();
    g_toasts[g_ntoasts].ms = ms;
    g_ntoasts++;
    LeaveCriticalSection(&g_toast_lock);
}

void menu_toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    toast_add(3000, fmt, ap);
    va_end(ap);
}

/* A toast shown for `ms` milliseconds (the start-up hint: long enough to read). */
static void toast_for(ULONGLONG ms, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    toast_add(ms, fmt, ap);
    va_end(ap);
}

/* Toast text with controller buttons: "[A]", "[Start]", "[Back]", "[LB]" etc. are drawn as
 * button icons `h` tall, the rest in colour c. Returns the width; draws only when `draw` is set. */
static float rich_text(float x, float y, float h, const char *s, int draw, DWORD c)
{
    static const char *const names[] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "Back", "D-pad"};
    float x0 = x;
    char seg[128];
    while (*s) {
        const char *open = strchr(s, '['), *close = open ? strchr(open, ']') : NULL;
        int b = -1;
        size_t n;
        if (open && close)
            for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
                if (strlen(names[i]) == (size_t)(close - open - 1) && !strncmp(open + 1, names[i], close - open - 1))
                    b = i;
        n = b >= 0 ? (size_t)(open - s) : open && close ? (size_t)(close + 1 - s) : strlen(s);
        if (n >= sizeof seg)
            n = sizeof seg - 1;
        memcpy(seg, s, n);
        seg[n] = 0;
        if (draw)
            text(FONT_SMALL, x, y, 0, c, seg);
        x += text_width(FONT_SMALL, seg);
        s += n;
        if (b >= 0) {
            float fh = g_font_px[FONT_SMALL] / g_scale;
            x += draw ? button_icon(b, x, y + (fh - h) / 2, h) : button_width(b, h);
            s = close + 1;
        }
    }
    return x - x0;
}

static void draw_toasts(void)
{
    ULONGLONG now = GetTickCount64();
    float y = g_settings.show_fps ? 70.0f : 24.0f;
    EnterCriticalSection(&g_toast_lock);
    while (g_ntoasts && now - g_toasts[0].at > g_toasts[0].ms) {
        memmove(g_toasts, g_toasts + 1, sizeof g_toasts[0] * (MAX_TOASTS - 1));
        g_ntoasts--;
    }
    for (int i = 0; i < g_ntoasts; i++) {
        float age = (float)(now - g_toasts[i].at), end = (float)g_toasts[i].ms;
        float a = age < 150 ? age / 150 : age > end - 500 ? (end - age) / 500 : 1;
        float w = rich_text(0, 0, 30, g_toasts[i].text, 0, C_INK);
        float dx = -(w + 120) * (1 - ease_out(age / 260)); /* slides in from the left */
        float saved = g_fade;
        g_fade = a;
        /* a Garage tag: yellow, outlined in black, with a drop shadow and an orange diagonal edge */
        box(24 + dx + 6, y + 6, 24 + dx + w + 50, y + 46, 8, 0x80000000u);
        box(24 + dx, y, 24 + dx + w + 44, y + 40, 8, C_INK);
        box(27 + dx, y + 3, 21 + dx + w + 44, y + 37, 8, C_FIELD);
        box(24 + dx, y, 38 + dx, y + 40, 8, C_ORANGE); /* the darker orange diagonal: black would vanish on dark scenes */
        rich_text(46 + dx, y + 6, 30, g_toasts[i].text, 1, C_INK);
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
    box(24, 18, 24 + text_width(FONT_SMALL, s) + 32, 56, 8, C_INK); /* black tag; cyan at full speed */
    text(FONT_SMALL, 40, 24, 0, fps >= 58.5f ? C_CYAN : C_ORANGE, s);
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
    signed char pad[2]; /* info rows: controller buttons shown as icons (BTN_* + 1; 0 = none) */
} Item;

static int g_page, g_sel[PAGE_COUNT];
static int g_win_i;

/* Rows that don't apply right now: greyed, skipped by the selection. */
static int item_disabled(const Item *it)
{
    return it->value == &g_win_i && g_settings.fullscreen; /* the window size, in full screen */
}
static int g_stack[8], g_depth;

/* Animation state (times from now_ms). */
static double g_t_open, g_t_close, g_t_page, g_t_sel, g_t_value;
static int g_page_dir = 1, g_value_dir, g_closing;
static float g_sel_pos; /* the selection bar's row, gliding towards the selected one */

/* Choice lists and the indices the menu edits (synced with g_settings). */
/* Resolution and window size are chosen by height; the width follows the aspect setting. */
static const int RES_H[] = {480, 720, 960, 1080, 1200, 1440, 1600, 2160};
static const int WIN_H[] = {480, 600, 720, 840, 960, 1080, 1200, 1440};
static char g_res_text[8][16], g_win_text[8][16];
static const char *RES_NAMES[8], *WIN_NAMES[8];
static const char *const ASPECT_NAMES[] = {"4:3 (original)", "16:10", "16:9", "21:9"};
static const int ANISO[] = {1, 2, 4, 8, 16};
static const char *const ANISO_NAMES[] = {"Original", "2x", "4x", "8x", "16x"};
static const int AA[] = {1, 2, 4, 8};
static const char *const AA_NAMES[] = {"Off (original)", "2x MSAA", "4x MSAA", "8x MSAA"};
static const char *const HUD_NAMES[] = {"Classic", "ReJet"};
static const char *const LANG_NAMES[] = {"Japanese", "English", "French", "German", "Spanish", "Italian"};
static const char *const REPORT_KINDS[] = {"Graphics", "Audio", "Controls", "Gameplay", "Crash or freeze",
                                           "Slow or stutter", "Other"};
static const char *const ONOFF[] = {"Off", "On"};

static int g_res_i, g_aniso_i, g_aa_i, g_kind_i;
static char g_folder_text[96]; /* where the game files are, for the Game page */

static void size_names(void)
{
    float ar = settings_aspect_ratio(g_settings.aspect);
    for (int i = 0; i < 8; i++) {
        snprintf(g_res_text[i], sizeof g_res_text[i], "%d x %d", ((int)(RES_H[i] * ar + 0.5f) + 1) & ~1, RES_H[i]);
        snprintf(g_win_text[i], sizeof g_win_text[i], "%d x %d", (int)(WIN_H[i] * ar + 0.5f), WIN_H[i]);
        RES_NAMES[i] = g_res_text[i];
        WIN_NAMES[i] = g_win_text[i];
    }
}
static char g_note[160];

static int index_of(const int *list, int n, int v)
{
    int best = 0;
    for (int i = 0; i < n; i++)
        if (list[i] <= v)
            best = i;
    return best;
}

static void setup_again(void)
{
    settings_set_game_folder(L"");
    log_printf("menu: game files will be asked for at the next start\n");
    menu_toast("ReJet will ask for your game files next time it starts");
}

static void sync_from_settings(void)
{
    {
        WCHAR folder[MAX_PATH];
        char narrow[MAX_PATH * 2];
        size_t len;
        settings_game_folder(folder, MAX_PATH);
        WideCharToMultiByte(CP_UTF8, 0, folder[0] ? folder : L"(given on the command line)", -1, narrow,
                            sizeof narrow, NULL, NULL);
        len = strlen(narrow);
        snprintf(g_folder_text, sizeof g_folder_text, "%s%s", len > 60 ? "..." : "", narrow + (len > 60 ? len - 60 : 0));
    }
    size_names();
    g_res_i = index_of(RES_H, (int)(sizeof RES_H / sizeof RES_H[0]), g_settings.height);
    g_win_i = index_of(WIN_H, (int)(sizeof WIN_H / sizeof WIN_H[0]), g_settings.window_height);
    g_aniso_i = index_of(ANISO, (int)(sizeof ANISO / sizeof ANISO[0]), g_settings.anisotropy);
    g_aa_i = index_of(AA, (int)(sizeof AA / sizeof AA[0]), g_settings.antialiasing);
}

/* Resolution or VSync: reset the device now. The menu's state block belongs to the old device
 * state; the atlas is rebuilt for the new size on the next draw. */
static void changed_display(void)
{
    g_settings.height = RES_H[g_res_i]; /* the width follows the aspect (d3d.c) */
    size_names();
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
static void changed_aa(void)
{
    g_settings.antialiasing = AA[g_aa_i];
    changed_display(); /* a new multisampled scene target, through the same device reset */
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
#define INFO(label, value) {IT_INFO, label, NULL, NULL, 0, 0, -1, NULL, value, {0, 0}}
#define PAD(label, b1, b2, keys) {IT_INFO, label, NULL, NULL, 0, 0, -1, NULL, keys, {(b1) + 1, (b2) + 1}}

static const char *const PAGE_TITLE[PAGE_COUNT] = {"PAUSED", "DISPLAY", "AUDIO", "GAME", "CONTROLS", "REPORT A BUG",
                                                   "QUIT"};

static Item g_main[] = {
    ACTION("Resume", -1, NULL, "Back to the game."),
    ACTION("Report a bug", PAGE_REPORT, NULL, "Save a capture of this moment with a note, for the developers."),
    ACTION("Display", PAGE_DISPLAY, NULL, "Resolution, widescreen, full screen and more."),
    ACTION("Audio", PAGE_AUDIO, NULL, "Volume."),
    ACTION("Game", PAGE_GAME, NULL, "Language."),
    ACTION("Controls", PAGE_CONTROLS, NULL, "Controller and keyboard layout."),
    ACTION("Quit ReJet", PAGE_QUIT, NULL, "Close the game. Save in the game first."),
};
static Item g_display[] = {
    CHOICE("Internal resolution", g_res_i, RES_NAMES, 0, changed_display,
           "How sharp the game looks. Higher is clearer but needs a faster graphics card."),
    CHOICE("Aspect ratio", g_settings.aspect, ASPECT_NAMES, 0, changed_display,
           "Wider shapes show more of the world at the sides."),
    CHOICE("Window size", g_win_i, WIN_NAMES, 0, changed_window, "The size of the game window."),
    TOGGLE("Full screen", g_settings.fullscreen, 0, NULL, "Fill the whole screen."),
    CHOICE("Anti-aliasing", g_aa_i, AA_NAMES, 0, changed_aa,
           "Smooths jagged edges on outlines, buildings and rails. Higher settings cost more graphics card time."),
    CHOICE("Texture filtering", g_aniso_i, ANISO_NAMES, 0, changed_aniso,
           "Keeps textures sharp on surfaces seen at an angle, like floors and roads."),
    TOGGLE("Show FPS", g_settings.show_fps, 0, changed_save, "Shows the frame rate in the top left corner."),
    TOGGLE("VSync", g_settings.vsync, 0, changed_display,
           "Matches frames to your monitor to stop screen tearing. Off can feel more responsive."),
};
static Item g_audio[] = {
    {IT_SLIDER, "Volume", &g_settings.volume, NULL, 0, 0, -1, changed_save, "Master volume."},
    TOGGLE("Mute in background", g_settings.mute_in_background, 0, changed_save,
           "No sound while you're using another window."),
};
static void setup_again(void);
static Item g_game[] = {
    CHOICE("HUD", g_settings.hud_style, HUD_NAMES, 0, changed_save,
           "Classic: the game's own HUD. ReJet: our updated HUD (health and cans so far)."),
    CHOICE("Language", g_settings.language, LANG_NAMES, 1, changed_save, "Text and voice language."),
    {IT_INFO, "Game files", NULL, NULL, 0, 0, -1, NULL, g_folder_text},
    ACTION("Choose other game files", -2, setup_again,
           "Next time ReJet starts it asks for your disc image or game folder again."),
};
static Item g_controls[] = { /* the game's own layout (its manual); controller icons, then the keys */
    PAD("ReJet menu", BTN_BACK, BTN_START, "F1"),
    INFO("Move", "Left stick  /  W A S D"),
    PAD("Jump", BTN_A, -1, "Space or Z"),
    PAD("Boost dash (10 cans)", BTN_B, -1, "X"),
    PAD("Tricks", BTN_X, BTN_Y, "C, V"),
    PAD("Spray graffiti, talk", BTN_RT, -1, "E"),
    PAD("Camera behind you", BTN_LT, -1, "Q"),
    INFO("Look around (standing)", "Right stick  /  I J K L"),
    INFO("Shuffle", "Left stick click  /  Shift"),
    PAD("Map / menus", BTN_START, BTN_DPAD, "Enter, arrow keys"),
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
    if (item_disabled(it) && (ev == IN_LEFT || ev == IN_RIGHT || ev == IN_OK))
        return;
    if (g_page == PAGE_MAIN && (ev == IN_LEFT || ev == IN_RIGHT)) /* the pause page's tiles run across */
        ev = ev == IN_LEFT ? IN_UP : IN_DOWN;
    switch (ev) {
    case IN_UP:
    case IN_DOWN:
        for (int k = 0; k < n; k++) { /* the next row that applies */
            *sel = (*sel + (ev == IN_UP ? n - 1 : 1)) % n;
            if (!item_disabled(&items[*sel]))
                break;
        }
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
    float o = ease_out(progress(g_t_open, 160)); /* menus feel slow past ~150 ms (MENU_DESIGN.md) */
    if (g_closing)
        o *= 1 - ease_out(progress(g_t_close, 120));
    return o;
}

/* The frozen game frame as a quad (back-buffer pixels, the top edge shifted right by skew). */
static void frozen_quad(float x0, float y0, float x1, float y1, float skew, DWORD c, float u0, float u1)
{
    Vertex v[6];
    IDirect3DSurface9 *bb = NULL, *old_rt = NULL;
    if (!g_frozen)
        return;
    /* Into the device's back buffer, like the rest of the menu: the current target is the game's,
     * which with anti-aliasing on is a separate multisampled surface. Drawn there, the frozen
     * frame never reached the screen, and the menu's translucent layers piled up frame after
     * frame (the background stripes turned solid). */
    IDirect3DDevice9Ex_GetRenderTarget(DEV, 0, &old_rt);
    IDirect3DDevice9Ex_GetBackBuffer(DEV, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    IDirect3DDevice9Ex_SetRenderTarget(DEV, 0, bb);
    v[0] = (Vertex){x0 + skew, y0, 0, 1, c, u0, 0};
    v[1] = (Vertex){x1 + skew, y0, 0, 1, c, u1, 0};
    v[2] = (Vertex){x0, y1, 0, 1, c, u0, 1};
    v[3] = v[1];
    v[4] = (Vertex){x1, y1, 0, 1, c, u1, 1};
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
    IDirect3DDevice9Ex_SetRenderTarget(DEV, 0, old_rt);
    if (old_rt)
        IDirect3DSurface9_Release(old_rt);
    if (bb)
        IDirect3DSurface9_Release(bb);
}

/* The paused game behind the menu: it shows while the menu slides in and out. */
static void draw_frozen(float o)
{
    BYTE k = (BYTE)(255 - (255 - 0x80) * o); /* darkens to half as the menu opens */
    frozen_quad(-0.5f, -0.5f, (float)g_bw - 0.5f, (float)g_bh - 0.5f, 0,
                0xFF000000u | (DWORD)k << 16 | (DWORD)k << 8 | k, 0, 1);
}

static float g_preview[4]; /* the preview's frame on the canvas (draw_menu), for draw_preview */

/* The paused game, small, in its frame on the right; after the menu's batch. */
static void draw_preview(void)
{
    BYTE a = (BYTE)(255 * openness());
    if (g_preview[2] > g_preview[0]) {
        float fa = (float)g_bw / (float)g_bh, pa = (X(g_preview[2]) - X(g_preview[0])) / (Y(g_preview[3]) - Y(g_preview[1]));
        float uw = pa < fa ? pa / fa : 1; /* the middle of the frame, cropped to the preview's shape */
        frozen_quad(X(g_preview[0]) - 0.5f, Y(g_preview[1]) - 0.5f, X(g_preview[2]) - 0.5f, Y(g_preview[3]) - 0.5f,
                    12 * g_scale, (DWORD)a << 24 | 0x00FFFFFFu, 0.5f - uw / 2, 0.5f + uw / 2);
    }
}

/* Garage (docs/MENU_DESIGN.md, after the game's own Garage menu): a yellow field with orange
 * bars, the logo with a cyan page tag, settings as rows with a black edge, the selected row
 * lifted onto white with a black drop shadow and its value in a cyan tag, the help line on a
 * black strip, and the button bar; a small tilted preview of the paused game sits on the right
 * (draw_preview, after the batch). */
#define ROW_R    904.0f        /* the rows' right edge on the canvas */

static void upper(char *dst, size_t cap, const char *s)
{
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++)
        dst[i] = (char)((s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i]);
    dst[i] = 0;
}

/* A box with a black outline (o canvas units thick). */
static void outlined(float x0, float y0, float x1, float y1, float o, DWORD fill)
{
    box(x0, y0, x1, y1, 0, C_INK);
    box(x0 + o, y0 + o, x1 - o, y1 - o, 0, fill);
}

/* The pause page's icons, in a 64-unit cell at (x, y), drawn in ink with 5-unit strokes. */
static void icon(int page, int target, float x, float y, DWORD bg)
{
    const float t = 5;
    switch (target) {
    case -1: /* resume: play */
        tri(x + 18, y + 8, x + 52, y + 32, x + 18, y + 56, C_INK);
        break;
    case PAGE_REPORT: /* a warning sign */
        tri(x + 32, y + 4, x + 62, y + 58, x + 2, y + 58, C_INK);
        tri(x + 32, y + 16, x + 52, y + 52, x + 12, y + 52, bg);
        box(x + 29, y + 26, x + 35, y + 42, 0, C_INK);
        box(x + 29, y + 45, x + 35, y + 50, 0, C_INK);
        break;
    case PAGE_DISPLAY: /* a monitor */
        box(x + 4, y + 8, x + 60, y + 46, 0, C_INK);
        box(x + 4 + t, y + 8 + t, x + 60 - t, y + 46 - t, 0, bg);
        box(x + 29, y + 46, x + 35, y + 54, 0, C_INK);
        box(x + 18, y + 54, x + 46, y + 58, 0, C_INK);
        break;
    case PAGE_AUDIO: /* a speaker with sound */
        box(x + 6, y + 22, x + 18, y + 42, 0, C_INK);
        tri(x + 16, y + 22, x + 34, y + 8, x + 34, y + 56, C_INK);
        tri(x + 16, y + 22, x + 34, y + 56, x + 16, y + 42, C_INK);
        box(x + 42, y + 26, x + 46, y + 38, 0, C_INK);
        box(x + 52, y + 18, x + 56, y + 46, 0, C_INK);
        break;
    case PAGE_GAME: /* the game: a controller */
        box(x + 2, y + 18, x + 62, y + 48, 0, C_INK);
        box(x + 2 + t, y + 18 + t, x + 62 - t, y + 48 - t, 0, bg);
        box(x + 12, y + 31, x + 26, y + 35, 0, C_INK);
        box(x + 17, y + 26, x + 21, y + 40, 0, C_INK);
        box(x + 40, y + 26, x + 46, y + 32, 0, C_INK);
        box(x + 48, y + 34, x + 54, y + 40, 0, C_INK);
        break;
    case PAGE_CONTROLS: /* controller and keyboard layouts: a keyboard */
        box(x + 2, y + 14, x + 62, y + 52, 0, C_INK);
        box(x + 2 + t, y + 14 + t, x + 62 - t, y + 52 - t, 0, bg);
        for (int r = 0; r < 2; r++)
            for (int k = 0; k < 5; k++)
                box(x + 11 + k * 9.5f, y + 23 + r * 9, x + 16 + k * 9.5f, y + 28 + r * 9, 0, C_INK);
        box(x + 18, y + 41, x + 46, y + 45, 0, C_INK);
        break;
    default: /* quit: a door bracket and an arrow out */
        box(x + 8, y + 8, x + 13, y + 56, 0, C_INK);
        box(x + 8, y + 8, x + 30, y + 13, 0, C_INK);
        box(x + 8, y + 51, x + 30, y + 56, 0, C_INK);
        box(x + 22, y + 30, x + 48, y + 34, 0, C_INK);
        tri(x + 44, y + 22, x + 60, y + 32, x + 44, y + 42, C_INK);
        break;
    }
    (void)page;
}

/* Short names for the tiles (the help strip says the rest). */
static const char *tile_label(const Item *it)
{
    switch (it->target) {
    case -1: return "RESUME";
    case PAGE_REPORT: return "REPORT";
    case PAGE_QUIT: return "QUIT";
    default: return NULL;
    }
}

/* The signed-off Garage designs are drawn on a 1280x720 board spanning the whole 16:9 screen;
 * DX/DY/DW place their coordinates on the canvas (960 units tall, widened by canvas_left/right),
 * so the menu matches them at any aspect ratio. */
static float DX(float x) { return canvas_left() + x * (canvas_right() - canvas_left()) / 1280.0f; }
static float DW(float w) { return w * (canvas_right() - canvas_left()) / 1280.0f; }
#define DY(y) ((y) * (4.0f / 3.0f))

/* An arrow pointing right (the design's chevrons), `w` long, centred on y. */
/* A straight stroke of even thickness t from (x0, y0) to (x1, y1), with square ends. */
static void stroke(float x0, float y0, float x1, float y1, float t, DWORD c)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy), nx, ny;
    if (l <= 0)
        return;
    dx /= l;
    dy /= l;
    x0 -= dx * t / 2; /* square caps: the corners meet cleanly */
    y0 -= dy * t / 2;
    x1 += dx * t / 2;
    y1 += dy * t / 2;
    nx = -dy * t / 2;
    ny = dx * t / 2;
    tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
    tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

/* An arrow pointing right (the design's chevron arrows), `w` long, centred on y. */
static void arrow(float x, float y, float w, DWORD c)
{
    const float t = 6.5f, hh = 20;
    stroke(x, y, x + w - 3, y, t, c);
    stroke(x + w - hh * 1.25f, y - hh, x + w, y, t, c);
    stroke(x + w - hh * 1.25f, y + hh, x + w, y, t, c);
}

/* The pause page: a row of cyan tiles with icons (the design's Main board); the selected tile
 * lifts onto white with a black drop shadow. */
static void draw_tiles(const Item *items, int n, int sel, float slide)
{
    const float size = DY(104), gap = DW(14), y0 = DY(248);
    for (int i = 0; i < n; i++) {
        const Item *it = &items[i];
        float rp = progress(g_t_page + 18.0 * i, 160);
        float x = DX(72) + (DW(104) + gap) * i - 120 * slide + 20 * (1 - ease_out(rp)), y = y0;
        float w = DW(104);
        int on = i == sel;
        DWORD bg = on ? C_WHITE : C_CYAN;
        char label[32];
        const char *l = tile_label(it);
        float saved = g_fade;
        g_fade = saved * ease_out(rp);
        if (on) {
            float lift = 10 * ease_out(progress(g_t_sel, 120));
            x -= lift * 0.3f;
            y -= lift;
            box(x + 8, y + 8, x + w + 8, y + size + 8, 0, C_INK);
        }
        outlined(x, y, x + w, y + size, 4, bg);
        icon(g_page, it->target, x + w / 2 - 32, y + 18, bg);
        if (l)
            snprintf(label, sizeof label, "%s", l);
        else
            upper(label, sizeof label, it->label);
        text(FONT_SMALL, x + w / 2, y + size - 46, 1, C_INK, label);
        g_fade = saved;
    }
}

static void draw_menu(void)
{
    int n;
    Item *items = page_items(g_page, &n);
    int sel = g_sel[g_page] < n ? g_sel[g_page] : 0, main_page = g_page == PAGE_MAIN;
    /* sub-pages: the Display board's rows, 42 + 8 px apart, shrinking to fit long pages */
    float y0 = DY(168), row = n * DY(50) <= DY(424) ? DY(50) : DY(424) / (float)n, rh = row - DY(8);
    float X0 = DX(64), X1 = DX(904);
    double t = now_ms();
    float o = openness(), slide = 1 - o;
    float pg = progress(g_t_page, 140), saved_fade = g_fade, list_end;
    int restart_note = 0;

    /* the field and the orange bars drop in (taller on the pause page, as the boards) */
    g_fade = o;
    box(canvas_left() - 10, -10, canvas_right() + 10, 970, 0, C_FIELD);
    {
        static const float bars[][2] = {{0, 26}, {44, 44}, {106, 18}, {142, 60}, {220, 22}};
        float h = DY(main_page ? 96 : 64), dy = -h * slide;
        for (int i = 0; i < 5; i++)
            box(DX(60 + bars[i][0]), -10 + dy, DX(60 + bars[i][0] + bars[i][1]), h + dy, 0, C_ORANGE);
    }

    /* the logo (black, as drawn) and the page tag: white on the pause page, cyan elsewhere */
    {
        float lh = main_page ? DY(66) : DY(46), ly = main_page ? DY(124) : DY(84);
        float x = (main_page ? DX(72) : DX(64)) - 60 * slide, lw = g_logo_px ? logo(x, ly, lh, C_WHITE) : 0;
        if (!lw) {
            text(FONT_TITLE, x, ly, 0, C_INK, "REJET");
            lw = text_width(FONT_TITLE, "REJET");
        }
        g_fade = o * ease_out(pg);
        {
            float tw = text_width(FONT_SMALL, PAGE_TITLE[g_page]) + 36, tx = x + lw + DW(18);
            float ty = ly + lh - 46 - 12 * (1 - ease_out(pg));
            box(tx, ty, tx + tw, ty + 46, -6, C_INK);
            box(tx + 4, ty + 4, tx + tw - 4, ty + 42, -6, main_page ? C_WHITE : C_CYAN);
            text(FONT_SMALL, tx + tw / 2 - 3, ty + 9, 1, C_INK, PAGE_TITLE[g_page]);
        }
        g_fade = o;
    }

    /* the selection glides between rows */
    {
        float target = (float)sel, k = 1 - (float)exp(-(1000.0 / 60.0) / 40.0);
        g_sel_pos += (target - g_sel_pos) * k;
        if (fabsf(target - g_sel_pos) < 0.002f)
            g_sel_pos = target;
    }
    if (main_page) {
        draw_tiles(items, n, sel, slide);
        list_end = DY(400);
    }
    else
        list_end = y0 + row * n + DY(24);
    for (int i = 0; i < n && !main_page; i++) {
        const Item *it = &items[i];
        float rp = progress(g_t_page + 18.0 * i, 160);
        float rx = 40 * (float)g_page_dir * (1 - ease_out(rp)) - 120 * slide, y = y0 + row * i;
        float ty = y + (rh - 46) * 0.5f, right = X1 - 18 + rx;
        int on = i == sel;
        DWORD fg = item_disabled(it) ? 0xFF6B5A20u : C_INK; /* info rows (Controls) in ink: dimmed reads as disabled */
        char label[96], val[200] = "";
        g_fade = o * ease_out(rp);
        if (on) { /* lifted: white with a black drop shadow */
            float lift = 6 * ease_out(progress(g_t_sel, 120));
            box(X0 + rx + 8, y + 8 - lift, X1 + rx + 8, y + rh + 8 - lift, 0, C_INK);
            outlined(X0 + rx - lift, y - lift, X1 + rx - lift, y + rh - lift, 3.5f, C_WHITE);
            ty -= lift;
            right -= lift;
        }
        else {
            box(X0 + rx, y, X1 + rx, y + rh, 0, 0x14000000u);
            box(X0 + rx, y, X0 + 8 + rx, y + rh, 0, C_INK);
        }
        upper(label, sizeof label, it->label);
        text(FONT_ITEM, X0 + 26 + rx - (on ? 6 : 0), ty, 0, fg, label);
        switch (it->type) {
        case IT_CHOICE:
        case IT_TOGGLE:
            snprintf(val, sizeof val, "%s%s", it->choices[*it->value], it->restart ? " *" : "");
            break;
        case IT_SLIDER: {
            float x0 = right - 300, x1 = right - 80, f = (float)*it->value / 100.0f;
            outlined(x0, ty + 14, x1, ty + 34, 3, 0x66FFFFFFu);
            box(x0 + 3, ty + 17, x0 + 3 + (x1 - x0 - 6) * f, ty + 31, 0, on ? C_CYAN : C_INK);
            snprintf(val, sizeof val, "%d", *it->value);
            text(FONT_ITEM, right, ty, 2, C_INK, val);
            val[0] = 0;
            break;
        }
        case IT_TEXT:
            snprintf(val, sizeof val, "%s%s", g_note[0] ? g_note : (on ? "" : "(optional)"),
                     on && fmod(t, 1000.0) < 500.0 ? "_" : "");
            break;
        case IT_INFO:
            snprintf(val, sizeof val, it->pad[0] ? "/   %s" : "%s", it->help);
            break;
        default:
            if (on)
                snprintf(val, sizeof val, ">");
            break;
        }
        if (it->type == IT_INFO && it->pad[0]) { /* controller icons left of the keys */
            float h = 34, ix = right - text_width(FONT_SMALL, val) - 14;
            for (int b = 1; b >= 0; b--)
                if (it->pad[b]) {
                    ix -= button_width(it->pad[b] - 1, h);
                    button_icon(it->pad[b] - 1, ix, ty + 6, h);
                    ix -= 8;
                }
        }
        if (it->restart)
            restart_note = 1;
        if (val[0]) {
            int tag = it->type == IT_CHOICE || it->type == IT_TOGGLE;
            int font = it->type == IT_TEXT || it->type == IT_INFO ? FONT_SMALL : FONT_ITEM;
            float maxw = it->type == IT_TEXT ? 440.0f : 360.0f, w = text_width(font, val);
            float nudge = on ? 12.0f * (float)g_value_dir * (1 - ease_out(progress(g_t_value, 140))) : 0;
            const char *v = val;
            while (w > maxw && *v) /* long notes: show the end */
                w -= text_width(font, (char[]){*v++, 0});
            if (tag) { /* the value in a tag: cyan when selected, with arrows */
                float x1 = right - (on ? 34 : 0) + nudge, x0 = x1 - w - 28;
                outlined(x0, ty + 6, x1, ty + 40, 3, on ? C_CYAN : 0x80FFFFFFu); /* even margins in the row */
                text(font, x1 - 14, ty, 2, C_INK, v);
                if (on) {
                    text(FONT_ITEM, x0 - 12, ty, 2, C_INK, "<");
                    text(FONT_ITEM, right + 4, ty, 2, C_INK, ">");
                }
            }
            else
                text(font, right + nudge, ty + (font == FONT_SMALL ? 10 : 0), 2, fg, v);
        }
    }
    g_fade = o;

    /* the paused game on sub-pages: the Display board's frame (256 x 300 at 960, 168), slightly
     * tilted; draw_preview fills it, cropped to its shape. The pause page has none (Main board). */
    g_preview[0] = g_preview[2] = 0;
    if (!main_page) {
        float x0 = DX(960) + 200 * slide, x1 = x0 + DW(256), py0 = DY(168), py1 = DY(168 + 300);
        box(x0 + 4, py0 + 10, x1 + 16, py1 + 14, 12, 0x55000000u); /* shadow */
        box(x0 - 7, py0 - 7, x1 + 7, py1 + 7, 12, C_INK);
        g_preview[0] = x0;
        g_preview[1] = py0;
        g_preview[2] = x1;
        g_preview[3] = py1;
    }

    /* the help line on a black strip, the game's own motif at its left */
    {
        const Item *it = &items[sel];
        float y = list_end, h = DY(main_page ? 52 : 44), right = main_page ? DX(72 + 760) : DX(64 + 1152);
        float x = main_page ? DX(72) : DX(64);
        const char *line = it->help && it->type != IT_INFO ? it->help
                           : restart_note                 ? "* applies the next time ReJet starts"
                                                          : NULL;
        if (!main_page && y < g_preview[3] + 24) /* short pages: stop short of the preview */
            right = DX(904);
        if (line) { /* no empty strip on pages without help (Controls) */
            box(x, y, x + 18, y + h, 0, C_INK);
            box(x + 26, y, x + 39, y + h, 0, C_INK);
            box(x + 39, y, right, y + h, 0, C_INK);
            g_fade = o * ease_out(progress(g_t_sel, 140));
            text(FONT_SMALL, x + 64, y + (h - 34) / 2, 0, line == it->help ? C_WHITE : 0xFFC9C9C9u, line);
            g_fade = o;
        }
    }

    /* the pause page's orange rule and arrows (Main board) */
    if (main_page) {
        float dx = -200 * slide;
        box(DX(72) + dx, DY(482), DX(72 + 1136) + dx, DY(482 + 18), 0, C_ORANGE);
        arrow(DX(72) + dx, DY(532), DW(80), 0xFFC96A07u);
        arrow(DX(172) + dx, DY(532), DW(60), 0xFFC96A07u);
    }

    /* the button bar, with the build for bug reports */
    {
        float dy = 80 * slide, top = 960 - DY(60);
        box(canvas_left(), top + dy, canvas_right(), 960 + dy, 0, C_INK);
        text(FONT_SMALL, canvas_right() - 28, top + 22 + dy, 2, 0xFFC9C9C9u, "ReJet " REJET_BUILD);
        if (g_text_focus)
            text(FONT_SMALL, 640, top + 22 + dy, 1, C_WHITE, "Type your note   Enter / Down  next   Esc  back");
        else if (pad_connected()) { /* button icons with a controller, keys without one */
            static const struct { int button; const char *label; } hints[] = {
                {BTN_A, "Select"}, {BTN_B, "Back"}, {BTN_DPAD, "Change"}, {BTN_START, "Resume"}};
            float h = 34, total = 0, x;
            for (int i = 0; i < 4; i++)
                total += button_width(hints[i].button, h) + 10 + text_width(FONT_SMALL, hints[i].label) + 36;
            x = 640 - (total - 36) / 2;
            for (int i = 0; i < 4; i++) {
                x += button_icon(hints[i].button, x, top + 19 + dy, h) + 10;
                text(FONT_SMALL, x, top + 22 + dy, 0, C_WHITE, hints[i].label);
                x += text_width(FONT_SMALL, hints[i].label) + 36;
            }
        }
        else
            text(FONT_SMALL, 640, top + 22 + dy, 1, C_WHITE,
                 "Enter  select     Esc  back     Left / Right  change     F1  resume");
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
    /* Blurred once, here: shrunk to a quarter in two filtered halvings, then drawn back up
     * filtered (draw_frozen), which softens the paused game behind the menu for nothing a frame. */
    if (FAILED(IDirect3DDevice9Ex_CreateTexture(DEV, g_bw / 4 ? g_bw / 4 : 1, g_bh / 4 ? g_bh / 4 : 1, 1,
                                                D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_frozen,
                                                NULL)))
        return;
    IDirect3DDevice9Ex_GetBackBuffer(DEV, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    IDirect3DTexture9_GetSurfaceLevel(g_frozen, 0, &dst);
    if (bb && dst) {
        IDirect3DSurface9 *half = NULL;
        if (SUCCEEDED(IDirect3DDevice9Ex_CreateRenderTarget(DEV, g_bw / 2 ? g_bw / 2 : 1, g_bh / 2 ? g_bh / 2 : 1,
                                                            D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &half,
                                                            NULL))) {
            IDirect3DDevice9Ex_StretchRect(DEV, bb, NULL, half, NULL, D3DTEXF_LINEAR);
            IDirect3DDevice9Ex_StretchRect(DEV, half, NULL, dst, NULL, D3DTEXF_LINEAR);
            IDirect3DSurface9_Release(half);
        }
        else
            IDirect3DDevice9Ex_StretchRect(DEV, bb, NULL, dst, NULL, D3DTEXF_LINEAR);
    }
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
        if (g_closing && progress(g_t_close, 120) >= 1)
            break;
        ready();
        /* the frozen frame is drawn outside flush's state save: keep the game's states (Z, cull,
         * blend, shaders) or it renders see-through after the menu closes */
        if (FAILED(IDirect3DDevice9Ex_CreateStateBlock(DEV, D3DSBT_ALL, &saved)))
            saved = NULL;
        draw_frozen(openness());
        draw_menu();
        draw_toasts();
        flush();
        draw_preview();
        if (saved) {
            IDirect3DStateBlock9_Apply(saved);
            IDirect3DStateBlock9_Release(saved);
        }
        { /* 60 frames a second, like the game: a plain Sleep(15) plus the wait for the display
           * missed every other refresh (30 fps) */
            static LARGE_INTEGER freq, next;
            LARGE_INTEGER now;
            if (!freq.QuadPart)
                QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&now);
            if (!next.QuadPart || now.QuadPart - next.QuadPart > freq.QuadPart / 10)
                next = now; /* the first frame, or after a stall: start again */
            next.QuadPart += freq.QuadPart / 60;
            while (QueryPerformanceCounter(&now), now.QuadPart < next.QuadPart)
                if (next.QuadPart - now.QuadPart > freq.QuadPart / 500) /* more than 2 ms left */
                    Sleep(1);
        }
        g_host.present();
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

/* Start-up splash. The game's window stays black for about 6.5 s (frames 0 to ~390) while it
 * boots, before its first logo. The ReJet logo covers that, with a moving stripe so it doesn't
 * look frozen, and fades into the game's first picture: the game counts as started once it has
 * drawn something on 10 frames in a row (shown at least 1.5 s; at most 30 s). The welcome
 * message waits for the splash to end. */
static double g_splash_start = -1.0, g_splash_end;
static int g_splash_done, g_drawn_frames;

static void draw_splash(unsigned long frame_draws)
{
    double t = now_ms();
    float o, w, x, saved = g_fade;
    if (g_splash_done)
        return;
    if (g_splash_start < 0)
        g_splash_start = t;
    if (!g_splash_end) {
        g_drawn_frames = frame_draws ? g_drawn_frames + 1 : 0;
        if ((g_drawn_frames >= 10 && t - g_splash_start > 1500) || t - g_splash_start > 30000)
            g_splash_end = t;
    }
    o = ease_out(progress(g_splash_start, 400));
    if (g_splash_end) {
        o *= 1 - ease_out(progress(g_splash_end, 500));
        if (progress(g_splash_end, 500) >= 1) {
            g_splash_done = 1;
            if (pad_connected())
                toast_for(8000, "ReJet %s   -   Menu:  [Back] + [Start]   or   F1", REJET_VERSION);
            else
                toast_for(8000, "ReJet %s   -   Menu:  F1   (or Back + Start on a controller)", REJET_VERSION);
            return;
        }
    }
    g_fade = o;
    /* the menu's Garage look: the yellow field, orange bars and the logo as drawn */
    box(canvas_left() - 10, -10, canvas_right() + 10, 970, 0, C_FIELD);
    {
        static const float bars[][2] = {{0, 30}, {48, 52}, {118, 22}, {158, 70}, {246, 26}};
        for (int i = 0; i < 5; i++)
            box(480 + bars[i][0], -10, 480 + bars[i][0] + bars[i][1], 120 - 100 * (1 - o), 0, C_ORANGE);
    }
    w = g_logo_px ? 96 * g_logo_aspect : text_width(FONT_TITLE, "REJET");
    {
        float dx = -(1 - o) * 120;
        if (g_logo_px)
            logo(640 - w / 2 + dx, 392, 96, C_WHITE);
        else
            text(FONT_TITLE, 640 + dx, 400, 1, C_INK, "REJET");
    }
    /* a black stripe sweeping along a darker track */
    box(490, 556, 790, 564, 4, 0x33000000u);
    x = 490 + (float)fmod((t - g_splash_start) * 0.25, 360.0) - 60;
    box(x < 490 ? 490 : x, 556, x + 60 > 790 ? 790 : x + 60, 564, 4, C_INK);
    text(FONT_SMALL, 640, 590, 1, 0xFF5A4A10u, REJET_BUILD);
    g_fade = saved;
}

/* Text with a thin dark outline and a soft offset shadow, like the game's HUD lettering. */
static void outlined_text(int font, float x, float y, int align, DWORD c, const char *s)
{
    static const float d[][2] = {{-1.5f, 0}, {1.5f, 0}, {0, -1.5f}, {0, 1.5f}};
    text(font, x + 3, y + 3, align, 0x90000000u, s);
    for (int i = 0; i < 4; i++)
        text(font, x + d[i][0], y + d[i][1], align, C_INK, s);
    text(font, x, y, align, c, s);
}

/* The ReJet HUD (HUD: ReJet; the Garage HUD board, docs/HUD.md): the original's health bar, can
 * count and speed meter, redrawn cleanly at the screen's edges. Flat cel-shaded colour with a
 * light top and shaded bottom, thin dark outlines and soft shadows, close to the game's own
 * shapes. Only while the game draws its gameplay HUD. */
static void draw_rejet_hud(void)
{
    HudValues v;
    float saved = g_fade;
    if (g_settings.hud_style != 1 || !hud_values(&v))
        return;
    g_fade = 1;
    { /* health: a gold bar */
        float x1 = DX(1280 - 40), x0 = x1 - DW(300), y0 = DY(32), y1 = y0 + DY(16);
        float f = v.health < 0 ? 0 : v.health > 1 ? 1 : v.health, xf = x0 + (x1 - x0) * f;
        box(x0 + 3, y0 + 4, x1 + 4, y1 + 5, 0, 0x50000000u);
        box(x0 - 2.5f, y0 - 2.5f, x1 + 2.5f, y1 + 2.5f, 0, 0xFF1A1A1Au);
        box(x0, y0, x1, y1, 0, 0xFF5E6264u);
        if (f > 0) {
            box(x0, y0, xf, y1, 0, 0xFFF2B705u);
            box(x0, y0, xf, y0 + (y1 - y0) * 0.4f, 0, 0xFFFFD34Du);
            box(x0, y1 - (y1 - y0) * 0.25f, xf, y1, 0, 0xFFD39A00u);
        }
    }
    { /* cans: the spray can, red when empty and yellow with cans, as the game's, and the count */
        char s[16];
        float cx1 = DX(1280 - 40), cy = DY(62);
        DWORD body = v.cans > 0 ? 0xFFF2C21Au : 0xFFE0322Au, shade = v.cans > 0 ? 0xFFC99400u : 0xFFA82020u;
        float tw, cx;
        snprintf(s, sizeof s, "x %d", v.cans);
        tw = text_width(FONT_ITEM, s);
        cx = cx1 - tw - 46;
        box(cx + 3, cy + 15, cx + 31, cy + 63, 0, 0x50000000u);
        box(cx - 2, cy + 10, cx + 28, cy + 60, 0, 0xFF1A1A1Au);
        box(cx, cy + 12, cx + 26, cy + 58, 0, body);
        box(cx + 17, cy + 12, cx + 26, cy + 58, 0, shade);
        box(cx + 5, cy + 18, cx + 9, cy + 46, 0, 0x90FFFFFFu); /* the shine */
        box(cx + 5, cy + 2, cx + 21, cy + 11, 0, 0xFF1A1A1Au);
        box(cx + 7, cy + 4, cx + 19, cy + 10, 0, 0xFFC8C8C8u);
        outlined_text(FONT_ITEM, cx1, cy + 12, 2, C_WHITE, s);
    }
    { /* speed: the game's wedge at the bottom right, filling blue to red with speed */
        float x1 = DX(1280 - 40), x0 = x1 - DW(260), y1 = DY(720 - 40), y0 = y1 - DY(30);
        float f = v.speed < 0 ? 0 : v.speed > 1 ? 1 : v.speed, xf = x0 + (x1 - x0) * f;
        float yf = y1 - (y1 - y0) * f;
        DWORD r = (DWORD)(0x2A + (0xFF - 0x2A) * f), g = (DWORD)(0x58 + (0x3B - 0x58) * f), b = (DWORD)(0xD6 + (0x3B - 0xD6) * f);
        tri(x0 + 4, y1 + 5, x1 + 5, y0 + 4, x1 + 5, y1 + 5, 0x50000000u);
        tri(x0 - 5, y1 + 2.5f, x1 + 2.5f, y0 - 3.5f, x1 + 2.5f, y1 + 2.5f, 0xFF1A1A1Au);
        tri(x0, y1, x1, y0, x1, y1, 0xFF5E6264u);
        if (f > 0)
            tri(x0, y1, xf, yf, xf, y1, 0xFF000000u | r << 16 | g << 8 | b);
    }
    g_fade = saved;
}

void menu_frame(unsigned long frame_draws)
{
    if (!DEV || !ready())
        return;
    draw_splash(frame_draws);
    if (g_swallow && !any_input_held())
        InterlockedExchange(&g_swallow, 0);
    {
        int key = InterlockedExchange(&g_open_request, 0) != 0, pad = pad_open_combo();
        if (key || pad) {
            log_printf("menu: open (%s)\n", key ? "F1" : "Back + Start");
            run_menu();
        }
    }
    draw_rejet_hud();
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
        if (!g_logo_px)
            load_logo();
        once = 1;
    }
    g_host = *host;
    sync_from_settings(); /* the welcome message comes when the splash ends */
}
