/* Xbox texture resources -> Direct3D 9 textures.
 *
 * Host textures are cached per Xbox texture header and re-uploaded when the Xbox data
 * changes (sampled hash: catches whole-surface updates such as movie frames; lock tracking
 * comes later). Swizzled formats are unswizzled (Xbox bit interleave: x then y, until the
 * smaller dimension runs out); linear formats are copied row by row using their pitch and
 * sampled with texel coordinates on Xbox, so they report a texcoord scale. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdtex.h"
#include "kernel.h"
#include "perf.h"
#include "tex.h"

#define MAX_TEX 4096

typedef struct Entry {
    const XD3DPixelContainer *x;
    uint32_t data, format, size;
    unsigned long long hash; /* sample_hash: the texture's name for HD replacements */
    unsigned long long quick; /* quick_hash: the per-frame change check */
    unsigned long checked; /* tex frame of the last content check */
    IDirect3DBaseTexture9 *host;
    IDirect3DBaseTexture9 *stage; /* system-memory copy, kept for textures that are re-uploaded */
    int hd; /* host is an HD replacement (hdtex.c), not sized like the original */
    int decoded; /* DXT decoded to A8R8G8B8 (FormatInfo.decode) */
} Entry;

static Entry g_tex[MAX_TEX];
static unsigned g_count;

/* Lookup by Xbox header address (open addressing; entries are never removed). */
#define SLOTS (MAX_TEX * 2)
static unsigned short g_slot[SLOTS]; /* entry index + 1, 0 = empty */
static unsigned long g_tex_frame = 1;

void tex_new_frame(void)
{
    g_tex_frame++;
}

static unsigned slot_of(const XD3DPixelContainer *x)
{
    unsigned h = (unsigned)(UINT_PTR)x;
    h ^= h >> 13;
    h *= 0x9E3779B1u;
    return (h >> 8) % SLOTS;
}

typedef struct FormatInfo {
    D3DFORMAT host;
    unsigned bpp;     /* bits per pixel (DXT: per pixel of a 4x4 block) */
    int linear, dxt;
    int expand_al8;   /* AL8: one byte is both luminance and alpha */
    D3DFORMAT host_dxt; /* the DXT kind (DXT1/3/5), kept when decoding */
    int decode;       /* DXT with transparency: decoded to A8R8G8B8 with colour bled into transparent texels */
} FormatInfo;

/* ---- DXT decoding, for textures with transparent texels.
 * DXT1's transparent texels are black (and DXT3/5 art usually is too): drawn larger than
 * the texture (any resolution above 640x480), bilinear filtering blends that black into the
 * edges and outlines 2D art (text boxes, "Press Start") with a dark seam. Decoded textures get
 * the colour of their opaque neighbours in the transparent texels; alpha is unchanged. */

static DWORD rgb565(unsigned c)
{
    unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (DWORD)((r * 255 + 15) / 31) << 16 | (DWORD)((g * 255 + 31) / 63) << 8 | (DWORD)((b * 255 + 15) / 31);
}

static DWORD mix(DWORD a, DWORD b, unsigned wa, unsigned wb, unsigned div)
{
    DWORD out = 0;
    for (int k = 0; k < 24; k += 8)
        out |= (((((a >> k) & 0xFF) * wa + ((b >> k) & 0xFF) * wb) / div) & 0xFF) << k;
    return out;
}

/* The colour part of a block (8 bytes) into 16 texels; DXT1 three-colour blocks give alpha 0 for
 * index 3 (alpha_from_colour), DXT3/5 always use four colours. */
static void dxt_colour_block(const unsigned char *b, DWORD out[16], int dxt1)
{
    unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    DWORD p[4];
    unsigned idx = b[4] | b[5] << 8 | b[6] << 16 | (unsigned)b[7] << 24;
    p[0] = rgb565(c0) | 0xFF000000u;
    p[1] = rgb565(c1) | 0xFF000000u;
    if (!dxt1 || c0 > c1) {
        p[2] = mix(p[0], p[1], 2, 1, 3) | 0xFF000000u;
        p[3] = mix(p[0], p[1], 1, 2, 3) | 0xFF000000u;
    }
    else {
        p[2] = mix(p[0], p[1], 1, 1, 2) | 0xFF000000u;
        p[3] = 0; /* transparent black */
    }
    for (int i = 0; i < 16; i++)
        out[i] = p[(idx >> (2 * i)) & 3];
}

static void dxt_decode(const FormatInfo *fi, const unsigned char *src, unsigned w, unsigned h, DWORD *dst)
{
    unsigned block = fi->bpp * 2, bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (unsigned by = 0; by < bh; by++)
        for (unsigned bx = 0; bx < bw; bx++) {
            const unsigned char *b = src + (by * bw + bx) * block;
            DWORD t[16];
            if (block == 8)
                dxt_colour_block(b, t, 1);
            else {
                dxt_colour_block(b + 8, t, 0);
                if (fi->host_dxt == D3DFMT_DXT3) {
                    for (int i = 0; i < 16; i++) {
                        unsigned a = (b[i / 2] >> (4 * (i & 1))) & 15;
                        t[i] = (t[i] & 0x00FFFFFFu) | (DWORD)(a * 17) << 24;
                    }
                }
                else { /* DXT5: two endpoints, 3-bit indices */
                    unsigned a0 = b[0], a1 = b[1], av[8];
                    unsigned long long bits = 0;
                    for (int i = 0; i < 6; i++)
                        bits |= (unsigned long long)b[2 + i] << (8 * i);
                    av[0] = a0;
                    av[1] = a1;
                    for (int i = 2; i < 8; i++)
                        av[i] = a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7
                                        : i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : (i == 6 ? 0 : 255);
                    for (int i = 0; i < 16; i++)
                        t[i] = (t[i] & 0x00FFFFFFu) | (DWORD)av[(bits >> (3 * i)) & 7] << 24;
                }
            }
            for (unsigned y = 0; y < 4; y++)
                for (unsigned x = 0; x < 4; x++)
                    if (by * 4 + y < h && bx * 4 + x < w)
                        dst[(by * 4 + y) * w + bx * 4 + x] = t[y * 4 + x];
        }
}

/* Fill fully transparent texels with the average colour of their non-transparent neighbours,
 * a few texels deep (enough for bilinear filtering at any scale and the smaller mip levels). */
static void bleed_colour(DWORD *px, unsigned w, unsigned h)
{
    unsigned char *known = (unsigned char *)malloc((size_t)w * h);
    if (!known)
        return;
    for (unsigned i = 0; i < w * h; i++)
        known[i] = (px[i] >> 24) != 0;
    for (int pass = 0; pass < 4; pass++) {
        int changed = 0;
        for (unsigned y = 0; y < h; y++) {
            const unsigned rows[3] = {(y + h - 1) % h * w, y * w, (y + 1) % h * w}; /* textures wrap */
            for (unsigned x = 0; x < w; x++) {
                const unsigned cols[3] = {x ? x - 1 : w - 1, x, x + 1 < w ? x + 1 : 0};
                unsigned r = 0, g = 0, b = 0, n = 0;
                if (known[y * w + x])
                    continue;
                for (int dy = 0; dy < 3; dy++)
                    for (int dx = 0; dx < 3; dx++) {
                        const unsigned i = rows[dy] + cols[dx];
                        DWORD p;
                        if (known[i] != 1)
                            continue;
                        p = px[i];
                        r += (p >> 16) & 0xFF;
                        g += (p >> 8) & 0xFF;
                        b += p & 0xFF;
                        n++;
                    }
                if (n) {
                    px[y * w + x] = (px[y * w + x] & 0xFF000000u) | (r / n) << 16 | (g / n) << 8 | (b / n);
                    known[y * w + x] = 2; /* filled this pass */
                    changed = 1;
                }
            }
        }
        if (!changed)
            break;
        for (unsigned i = 0; i < w * h; i++)
            if (known[i] == 2)
                known[i] = 1;
    }
    free(known);
}

/* Does this DXT texture have transparent texels? DXT1: a three-colour block that uses index 3;
 * DXT3/5: any alpha below opaque (checked on level 0). */
static int dxt_has_transparency(const FormatInfo *fi, const unsigned char *src, unsigned w, unsigned h)
{
    unsigned blocks = ((w + 3) / 4) * ((h + 3) / 4);
    for (unsigned i = 0; i < blocks; i++) {
        const unsigned char *b = src + i * fi->bpp * 2;
        if (fi->bpp == 4) {
            unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
            unsigned idx = b[4] | b[5] << 8 | b[6] << 16 | (unsigned)b[7] << 24;
            if (c0 <= c1)
                for (int k = 0; k < 16; k++)
                    if (((idx >> (2 * k)) & 3) == 3)
                        return 1;
        }
        else if (fi->host_dxt == D3DFMT_DXT3) {
            for (int k = 0; k < 8; k++)
                if (b[k] != 0xFF)
                    return 1;
        }
        else if (b[0] != 0xFF || b[1] != 0xFF)
            return 1;
    }
    return 0;
}

static int format_info(uint32_t f, FormatInfo *fi)
{
    memset(fi, 0, sizeof *fi);
    switch (f) {
    case XD3DFMT_A8R8G8B8: fi->host = D3DFMT_A8R8G8B8; fi->bpp = 32; return 1;
    case XD3DFMT_X8R8G8B8: fi->host = D3DFMT_X8R8G8B8; fi->bpp = 32; return 1;
    case XD3DFMT_LIN_A8R8G8B8: fi->host = D3DFMT_A8R8G8B8; fi->bpp = 32; fi->linear = 1; return 1;
    case XD3DFMT_LIN_X8R8G8B8: fi->host = D3DFMT_X8R8G8B8; fi->bpp = 32; fi->linear = 1; return 1;
    case XD3DFMT_R5G6B5: fi->host = D3DFMT_R5G6B5; fi->bpp = 16; return 1;
    case XD3DFMT_LIN_R5G6B5: fi->host = D3DFMT_R5G6B5; fi->bpp = 16; fi->linear = 1; return 1;
    case XD3DFMT_A1R5G5B5: fi->host = D3DFMT_A1R5G5B5; fi->bpp = 16; return 1;
    case XD3DFMT_LIN_A1R5G5B5: fi->host = D3DFMT_A1R5G5B5; fi->bpp = 16; fi->linear = 1; return 1;
    case XD3DFMT_X1R5G5B5: fi->host = D3DFMT_X1R5G5B5; fi->bpp = 16; return 1;
    case XD3DFMT_LIN_X1R5G5B5: fi->host = D3DFMT_X1R5G5B5; fi->bpp = 16; fi->linear = 1; return 1;
    case XD3DFMT_A4R4G4B4: fi->host = D3DFMT_A4R4G4B4; fi->bpp = 16; return 1;
    case XD3DFMT_LIN_A4R4G4B4: fi->host = D3DFMT_A4R4G4B4; fi->bpp = 16; fi->linear = 1; return 1;
    case XD3DFMT_L8: fi->host = D3DFMT_L8; fi->bpp = 8; return 1;
    case XD3DFMT_LIN_L8: fi->host = D3DFMT_L8; fi->bpp = 8; fi->linear = 1; return 1;
    case XD3DFMT_A8: fi->host = D3DFMT_A8; fi->bpp = 8; return 1;
    case XD3DFMT_LIN_A8: fi->host = D3DFMT_A8; fi->bpp = 8; fi->linear = 1; return 1;
    case XD3DFMT_AL8: fi->host = D3DFMT_A8L8; fi->bpp = 8; fi->expand_al8 = 1; return 1;
    case XD3DFMT_LIN_AL8: fi->host = D3DFMT_A8L8; fi->bpp = 8; fi->expand_al8 = 1; fi->linear = 1; return 1;
    case XD3DFMT_A8L8: fi->host = D3DFMT_A8L8; fi->bpp = 16; return 1;
    case XD3DFMT_LIN_A8L8: fi->host = D3DFMT_A8L8; fi->bpp = 16; fi->linear = 1; return 1;
    case XD3DFMT_DXT1: fi->host = fi->host_dxt = D3DFMT_DXT1; fi->bpp = 4; fi->dxt = 1; return 1;
    case XD3DFMT_DXT2: fi->host = fi->host_dxt = D3DFMT_DXT3; fi->bpp = 8; fi->dxt = 1; return 1;
    case XD3DFMT_DXT4: fi->host = fi->host_dxt = D3DFMT_DXT5; fi->bpp = 8; fi->dxt = 1; return 1;
    default: return 0;
    }
}

/* Xbox swizzle: interleave x and y bits (x first) while both dimensions have bits left. */
static unsigned swizzle(unsigned x, unsigned y, unsigned w, unsigned h)
{
    unsigned off = 0, bit = 0;
    for (unsigned m = 1; m < w || m < h; m <<= 1) {
        if (m < w) {
            if (x & m)
                off |= 1u << bit;
            bit++;
        }
        if (m < h) {
            if (y & m)
                off |= 1u << bit;
            bit++;
        }
    }
    return off;
}

static unsigned long long sample_hash(const unsigned char *p, unsigned bytes)
{
    unsigned long long h = 1469598103934665603ull;
    unsigned step = bytes > 65536 ? 61 : 1; /* sparse for large textures */
    for (unsigned i = 0; i + 4 <= bytes; i += 4 * step) {
        h ^= *(const unsigned *)(p + i);
        h *= 1099511628211ull;
    }
    return h;
}

/* The per-frame change check: the same samples as sample_hash, but in 32-bit arithmetic. A
 * 64-bit multiply is a call (_allmul) in a 32-bit build, and sample_hash per texture per frame
 * was a third of each frame's CPU time. Two independent lanes, so a change that one lane misses
 * is still caught by the other. */
static unsigned long long quick_hash(const unsigned char *p, unsigned bytes)
{
    unsigned a = 2166136261u, b = 0x9E3779B9u;
    unsigned step = bytes > 65536 ? 61 : 1;
    for (unsigned i = 0; i + 4 <= bytes; i += 4 * step) {
        unsigned v = *(const unsigned *)(p + i);
        a = (a ^ v) * 16777619u;
        b = (b + v) * 0x85EBCA6Bu;
        b ^= b >> 13;
    }
    return (unsigned long long)a << 32 | b;
}

static unsigned level_bytes(const FormatInfo *fi, unsigned w, unsigned h)
{
    if (fi->dxt)
        return ((w + 3) / 4) * ((h + 3) / 4) * (fi->bpp * 2); /* 8 or 16 bytes per block */
    return w * h * fi->bpp / 8;
}

/* Copy one level into a locked host rect. */
static void upload_level(const FormatInfo *fi, const unsigned char *src, unsigned w, unsigned h, unsigned pitch,
                         D3DLOCKED_RECT *lr)
{
    unsigned char *dst = (unsigned char *)lr->pBits;
    if (fi->decode) {
        DWORD *px = (DWORD *)malloc((size_t)w * h * 4);
        if (!px)
            return;
        dxt_decode(fi, src, w, h, px);
        bleed_colour(px, w, h);
        for (unsigned y = 0; y < h; y++)
            memcpy(dst + y * lr->Pitch, px + (size_t)y * w, (size_t)w * 4);
        free(px);
        return;
    }
    if (fi->dxt) {
        unsigned row = ((w + 3) / 4) * fi->bpp * 2;
        for (unsigned y = 0; y < (h + 3) / 4; y++)
            memcpy(dst + y * lr->Pitch, src + y * row, row);
        return;
    }
    if (fi->linear) {
        unsigned row = w * fi->bpp / 8;
        for (unsigned y = 0; y < h; y++) {
            if (fi->expand_al8) {
                for (unsigned x = 0; x < w; x++)
                    ((unsigned short *)(dst + y * lr->Pitch))[x] = (unsigned short)(src[y * pitch + x] * 0x0101);
            }
            else {
                memcpy(dst + y * lr->Pitch, src + y * pitch, row);
            }
        }
        return;
    }
    {
        /* The swizzle interleaves x and y bits, so an offset is the OR of an x part and a y part:
         * one table per axis instead of the bit loop per texel (uploads at area loads were
         * 3.5 ms a texture and stalled frames). */
        unsigned bpp = fi->bpp / 8;
        unsigned *sx = (unsigned *)malloc(((size_t)w + h) * sizeof *sx), *sy = sx + w;
        if (!sx)
            return;
        for (unsigned x = 0; x < w; x++)
            sx[x] = swizzle(x, 0, w, h);
        for (unsigned y = 0; y < h; y++)
            sy[y] = swizzle(0, y, w, h);
        for (unsigned y = 0; y < h; y++) {
            unsigned char *d = dst + y * lr->Pitch;
            const unsigned yo = sy[y];
            if (fi->expand_al8)
                for (unsigned x = 0; x < w; x++)
                    ((unsigned short *)d)[x] = (unsigned short)(src[sx[x] | yo] * 0x0101);
            else if (bpp == 4)
                for (unsigned x = 0; x < w; x++)
                    ((DWORD *)d)[x] = ((const DWORD *)src)[sx[x] | yo];
            else if (bpp == 2)
                for (unsigned x = 0; x < w; x++)
                    ((unsigned short *)d)[x] = ((const unsigned short *)src)[sx[x] | yo];
            else
                for (unsigned x = 0; x < w; x++)
                    memcpy(d + x * bpp, src + (size_t)(sx[x] | yo) * bpp, bpp);
        }
        free(sx);
    }
}

/* Xbox 3D swizzle: interleave x, y and z bits (in that order) while each has bits left. */
static unsigned swizzle3(unsigned x, unsigned y, unsigned z, unsigned w, unsigned h, unsigned d)
{
    unsigned off = 0, bit = 0;
    for (unsigned m = 1; m < w || m < h || m < d; m <<= 1) {
        if (m < w) {
            if (x & m)
                off |= 1u << bit;
            bit++;
        }
        if (m < h) {
            if (y & m)
                off |= 1u << bit;
            bit++;
        }
        if (m < d) {
            if (z & m)
                off |= 1u << bit;
            bit++;
        }
    }
    return off;
}

/* Copy one volume level into a locked host box. Uncompressed volumes are 3D-swizzled.
 * Compressed volumes are stored in groups of 4 slices: for each 4x4 block position (rows,
 * then columns) the blocks of the group's slices follow each other; D3D9 wants each slice
 * as its own block grid. */
static void upload_volume_level(const FormatInfo *fi, const unsigned char *src, unsigned w, unsigned h, unsigned d,
                                D3DLOCKED_BOX *lb)
{
    unsigned bpp = fi->bpp / 8;
    if (fi->dxt) {
        unsigned block = fi->bpp * 2, bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (unsigned z0 = 0; z0 < d; z0 += 4) {
            unsigned ns = d - z0 < 4 ? d - z0 : 4;
            for (unsigned by = 0; by < bh; by++) {
                for (unsigned bx = 0; bx < bw; bx++) {
                    for (unsigned sl = 0; sl < ns; sl++) {
                        unsigned char *dst = (unsigned char *)lb->pBits + (z0 + sl) * lb->SlicePitch +
                                             by * lb->RowPitch + bx * block;
                        memcpy(dst, src, block);
                        src += block;
                    }
                }
            }
        }
        return;
    }
    for (unsigned z = 0; z < d; z++) {
        for (unsigned y = 0; y < h; y++) {
            unsigned char *row = (unsigned char *)lb->pBits + z * lb->SlicePitch + y * lb->RowPitch;
            for (unsigned x = 0; x < w; x++) {
                const unsigned char *t = src + swizzle3(x, y, z, w, h, d) * bpp;
                if (fi->expand_al8)
                    ((unsigned short *)row)[x] = (unsigned short)(t[0] * 0x0101);
                else
                    memcpy(row + x * bpp, t, bpp);
            }
        }
    }
}

int tex_is_depth(const XD3DPixelContainer *x)
{
    uint32_t xf = XFMT_FORMAT(x->Format);
    return xf == XD3DFMT_D24S8 || xf == XD3DFMT_LIN_D24S8 || xf == XD3DFMT_F24S8 || xf == XD3DFMT_D16 ||
           xf == XD3DFMT_LIN_D16 || xf == XD3DFMT_F16;
}

IDirect3DBaseTexture9 *tex_get(IDirect3DDevice9 *dev, const XD3DPixelContainer *x, float texcoord_scale[2])
{
    FormatInfo fi;
    uint32_t xf = XFMT_FORMAT(x->Format);
    unsigned w, h, depth = 1, levels, pitch = 0, total = 0, faces = (x->Format & XFMT_CUBEMAP) ? 6 : 1;
    int volume = XFMT_DIMENSION(x->Format) == 3;
    const unsigned char *data = (const unsigned char *)(UINT_PTR)x->Resource.Data;
    Entry *e = NULL;
    unsigned long long hash, quick;
    LONGLONG t_up;
    int keep_stage;

    texcoord_scale[0] = texcoord_scale[1] = 1.0f;
    if (tex_is_depth(x)) {
        /* Depth textures are sampled as shadow maps (NV2A compares and returns lit = 1). Until
         * shadow maps are rendered, report everything as lit instead of everything in shadow.
         * Linear depth textures are still addressed in texels, so keep their texcoord scale. */
        static IDirect3DTexture9 *white;
        if (x->Size) {
            texcoord_scale[0] = 1.0f / (float)XSIZE_WIDTH(x->Size);
            texcoord_scale[1] = 1.0f / (float)XSIZE_HEIGHT(x->Size);
        }
        if (!white && SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                                &white, NULL))) {
            IDirect3DTexture9 *staging;
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                         &staging, NULL))) {
                if (SUCCEEDED(IDirect3DTexture9_LockRect(staging, 0, &lr, NULL, 0))) {
                    *(DWORD *)lr.pBits = 0xFFFFFFFF;
                    IDirect3DTexture9_UnlockRect(staging, 0);
                    IDirect3DDevice9_UpdateTexture(dev, (IDirect3DBaseTexture9 *)staging,
                                                   (IDirect3DBaseTexture9 *)white);
                }
                IDirect3DTexture9_Release(staging);
            }
        }
        return (IDirect3DBaseTexture9 *)white;
    }
    if (!data || !format_info(xf, &fi)) {
        static LONG warned[256];
        if (!InterlockedExchange(&warned[xf & 0xFF], 1))
            log_printf("tex: unsupported Xbox format %02X (or no data)\n", xf);
        return NULL;
    }
    if (volume && x->Size) { /* linear volumes: not seen in this game */
        static LONG warned3d;
        if (!InterlockedExchange(&warned3d, 1))
            log_printf("tex: linear volume texture format %02X not supported yet\n", xf);
        return NULL;
    }
    if (x->Size) { /* linear */
        w = XSIZE_WIDTH(x->Size);
        h = XSIZE_HEIGHT(x->Size);
        pitch = XSIZE_PITCH(x->Size);
        levels = 1;
        total = pitch * h;
        texcoord_scale[0] = 1.0f / (float)w;
        texcoord_scale[1] = 1.0f / (float)h;
    }
    else {
        w = 1u << XFMT_USIZE(x->Format);
        h = 1u << XFMT_VSIZE(x->Format);
        depth = volume ? 1u << XFMT_PSIZE(x->Format) : 1;
        levels = XFMT_MIPMAPS(x->Format) ? XFMT_MIPMAPS(x->Format) : 1;
        for (unsigned l = 0, lw = w, lh = h, ld = depth; l < levels;
             l++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1, ld = ld > 1 ? ld / 2 : 1)
            total += level_bytes(&fi, lw, lh) * ld;
        total *= faces;
    }
    {
        unsigned k = slot_of(x);
        while (g_slot[k] && g_tex[g_slot[k] - 1].x != x)
            k = (k + 1) % SLOTS;
        if (g_slot[k])
            e = &g_tex[g_slot[k] - 1];
        else {
            if (g_count == MAX_TEX) {
                log_printf("tex: cache full\n");
                return NULL;
            }
            e = &g_tex[g_count++];
            memset(e, 0, sizeof *e);
            e->x = x;
            g_slot[k] = (unsigned short)g_count;
        }
    }
    /* Contents are checked (sampled hash) at most once per frame per texture. */
    if (e->host && e->data == x->Resource.Data && e->format == x->Format && e->size == x->Size) {
        if (e->checked == g_tex_frame)
            return e->host;
        e->checked = g_tex_frame;
        quick = quick_hash(data, total);
        if (e->quick == quick)
            return e->host;
        hash = sample_hash(data, total);
    }
    else {
        hash = sample_hash(data, total);
        quick = quick_hash(data, total);
    }
    e->checked = g_tex_frame;
    t_up = perf_now();
    if (e->host && (e->hd || e->format != x->Format || e->size != x->Size)) {
        IDirect3DBaseTexture9_Release(e->host);
        e->host = NULL;
    }
    e->hd = 0;
    if (!e->host && !volume && faces == 1 && hdtex_has(hash) &&
        (e->host = (IDirect3DBaseTexture9 *)hdtex_load(dev, hash)) != NULL) {
        e->hd = 1;
        e->data = x->Resource.Data;
        e->format = x->Format;
        e->size = x->Size;
        e->hash = hash;
        e->quick = quick;
        perf_end(PERF_TEXUP, t_up);
        return e->host;
    }
    if (fi.dxt && !volume && faces == 1 && !x->Size && dxt_has_transparency(&fi, data, w, h)) {
        fi.decode = 1; /* see dxt_decode: no dark seams around 2D art */
        fi.host = D3DFMT_A8R8G8B8;
    }
    if (e->host && e->decoded != fi.decode) { /* transparency appeared or went: other host format */
        IDirect3DBaseTexture9_Release(e->host);
        e->host = NULL;
    }
    e->decoded = fi.decode;
    /* Uploads go through a system-memory texture and UpdateTexture into an ordinary (static)
     * texture in video memory. Dynamic video-memory textures, locked directly, cost the driver
     * milliseconds each to create and lock (2-3 ms for a 128x128 DXT1), and area loads upload
     * a hundred at once. A texture uploaded a second time keeps its staging copy. */
    if (!e->host && e->stage) {
        IDirect3DBaseTexture9_Release(e->stage);
        e->stage = NULL;
    }
    keep_stage = e->host != NULL;
    if (!e->host) {
        HRESULT hr;
        if (volume) {
            static LONG logged;
            hr = IDirect3DDevice9_CreateVolumeTexture(dev, w, h, depth, levels, 0, fi.host, D3DPOOL_DEFAULT,
                                                      (IDirect3DVolumeTexture9 **)&e->host, NULL);
            if (InterlockedIncrement(&logged) <= 8)
                log_printf("tex: volume %ux%ux%u levels %u fmt %02X -> %08lX\n", w, h, depth, levels, xf, hr);
        }
        else if (faces == 6)
            hr = IDirect3DDevice9_CreateCubeTexture(dev, w, levels, 0, fi.host, D3DPOOL_DEFAULT,
                                                    (IDirect3DCubeTexture9 **)&e->host, NULL);
        else
            hr = IDirect3DDevice9_CreateTexture(dev, w, h, levels, 0, fi.host, D3DPOOL_DEFAULT,
                                                (IDirect3DTexture9 **)&e->host, NULL);
        if (FAILED(hr)) {
            log_printf("tex: CreateTexture %ux%u fmt %02X failed (%08lX)\n", w, h, xf, hr);
            e->host = NULL;
            return NULL;
        }
    }
    if (!e->stage) {
        HRESULT hr;
        if (volume)
            hr = IDirect3DDevice9_CreateVolumeTexture(dev, w, h, depth, levels, 0, fi.host, D3DPOOL_SYSTEMMEM,
                                                      (IDirect3DVolumeTexture9 **)&e->stage, NULL);
        else if (faces == 6)
            hr = IDirect3DDevice9_CreateCubeTexture(dev, w, levels, 0, fi.host, D3DPOOL_SYSTEMMEM,
                                                    (IDirect3DCubeTexture9 **)&e->stage, NULL);
        else
            hr = IDirect3DDevice9_CreateTexture(dev, w, h, levels, 0, fi.host, D3DPOOL_SYSTEMMEM,
                                                (IDirect3DTexture9 **)&e->stage, NULL);
        if (FAILED(hr)) {
            log_printf("tex: staging texture %ux%u fmt %02X failed (%08lX)\n", w, h, xf, hr);
            e->stage = NULL;
            return e->host;
        }
    }
    {
        /* Debugging: REJET_DUMPTEX=<min width> writes level 0 of wide textures as raw files. */
        char env[16] = "";
        if (GetEnvironmentVariableA("REJET_DUMPTEX", env, sizeof env) && w >= (unsigned)atoi(env)) {
            char name[96];
            FILE *fp;
            snprintf(name, sizeof name, "tex_%08X_%ux%u_fmt%02X.bin", x->Resource.Data, w, h, xf);
            if ((fp = fopen(name, "wb")) != NULL) {
                fwrite(data, 1, total, fp);
                fclose(fp);
            }
        }
    }
    if (volume) {
        const unsigned char *p = data;
        unsigned lw = w, lh = h, ld = depth;
        for (unsigned l = 0; l < levels; l++) {
            D3DLOCKED_BOX lb;
            if (SUCCEEDED(IDirect3DVolumeTexture9_LockBox((IDirect3DVolumeTexture9 *)e->stage, l, &lb, NULL, 0))) {
                upload_volume_level(&fi, p, lw, lh, ld, &lb);
                IDirect3DVolumeTexture9_UnlockBox((IDirect3DVolumeTexture9 *)e->stage, l);
            }
            p += level_bytes(&fi, lw, lh) * ld;
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
            ld = ld > 1 ? ld / 2 : 1;
        }
    }
    else {
        const unsigned char *p = data;
        for (unsigned f = 0; f < faces; f++) {
            unsigned lw = w, lh = h;
            for (unsigned l = 0; l < levels; l++) {
                D3DLOCKED_RECT lr;
                const DWORD lock = 0;
                HRESULT hr = faces == 6
                    ? IDirect3DCubeTexture9_LockRect((IDirect3DCubeTexture9 *)e->stage, (D3DCUBEMAP_FACES)f, l, &lr,
                                                     NULL, lock)
                    : IDirect3DTexture9_LockRect((IDirect3DTexture9 *)e->stage, l, &lr, NULL, lock);
                if (SUCCEEDED(hr)) {
                    upload_level(&fi, p, lw, lh, pitch, &lr);
                    if (faces == 6)
                        IDirect3DCubeTexture9_UnlockRect((IDirect3DCubeTexture9 *)e->stage, (D3DCUBEMAP_FACES)f, l);
                    else
                        IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)e->stage, l);
                }
                p += level_bytes(&fi, lw, lh);
                lw = lw > 1 ? lw / 2 : 1;
                lh = lh > 1 ? lh / 2 : 1;
            }
        }
    }
    if (!volume && faces == 1) {
        /* REJET_DUMPHOST=<min width>: write level 0 as converted for the host (texdump\h<hash>_<w>x<h>_f<host
         * format>.bin), for upscaling experiments; tools/texdump.py turns these into PNGs. */
        char env[16] = "";
        /* REJET_DUMPMOVIE=1: movie frames (X8R8G8B8, 640x480 and up) in order, as
         * movie\f<tex frame>_<w>x<h>.bin, for video upscaling tests. */
        if (fi.host == D3DFMT_X8R8G8B8 && w >= 640 && h >= 480 &&
            GetEnvironmentVariableA("REJET_DUMPMOVIE", env, sizeof env)) {
            D3DLOCKED_RECT lr;
            char name[96];
            FILE *fp;
            CreateDirectoryA("movie", NULL);
            snprintf(name, sizeof name, "movie\\f%06lu_%ux%u.bin", g_tex_frame, w, h);
            if (SUCCEEDED(IDirect3DTexture9_LockRect((IDirect3DTexture9 *)e->stage, 0, &lr, NULL, D3DLOCK_READONLY))) {
                if ((fp = fopen(name, "wb")) != NULL) {
                    for (unsigned y = 0; y < h; y++)
                        fwrite((const unsigned char *)lr.pBits + y * lr.Pitch, 4, w, fp);
                    fclose(fp);
                }
                IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)e->stage, 0);
            }
        }
        env[0] = 0;
        if (GetEnvironmentVariableA("REJET_DUMPHOST", env, sizeof env) && w >= (unsigned)atoi(env)) {
            D3DLOCKED_RECT lr;
            unsigned row = level_bytes(&fi, w, 1), rows = level_bytes(&fi, w, h) / row;
            char name[96];
            FILE *fp;
            CreateDirectoryA("texdump", NULL);
            snprintf(name, sizeof name, "texdump\\h%016llX_%ux%u_f%08lX.bin", hash, w, h, (unsigned long)fi.host);
            if (GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES &&
                SUCCEEDED(IDirect3DTexture9_LockRect((IDirect3DTexture9 *)e->stage, 0, &lr, NULL, D3DLOCK_READONLY))) {
                if ((fp = fopen(name, "wb")) != NULL) {
                    for (unsigned y = 0; y < rows; y++)
                        fwrite((const unsigned char *)lr.pBits + y * lr.Pitch, 1, row, fp);
                    fclose(fp);
                }
                IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)e->stage, 0);
            }
        }
    }
    IDirect3DDevice9_UpdateTexture(dev, e->stage, e->host);
    if (!keep_stage) {
        IDirect3DBaseTexture9_Release(e->stage);
        e->stage = NULL;
    }
    e->data = x->Resource.Data;
    e->format = x->Format;
    e->size = x->Size;
    e->hash = hash;
    e->quick = quick;
    { /* uploads that stall a frame, for REJET_PERFLOG investigations */
        static LONGLONG freq;
        static LONG logged;
        LONGLONG dt = perf_now() - t_up;
        if (!freq) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            freq = f.QuadPart;
        }
        if (dt * 1000 > 2 * freq && InterlockedIncrement(&logged) <= 64)
            log_printf("tex: slow upload %.1f ms: %ux%u x%u levels %u faces %u fmt %02X%s%s\n",
                       (double)dt * 1000.0 / (double)freq, w, h, depth, levels, faces, xf,
                       fi.decode ? " (DXT decoded)" : "", fi.linear ? " linear" : "");
    }
    perf_end(PERF_TEXUP, t_up);
    return e->host;
}
