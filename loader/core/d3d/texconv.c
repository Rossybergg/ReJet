/* Texture data conversion (see texconv.h). */
#include <stdlib.h>
#include <string.h>

#include "tex.h"
#include "texconv.h"


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

void dxt_decode(const FormatInfo *fi, const unsigned char *src, unsigned w, unsigned h, DWORD *dst)
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
void bleed_colour(DWORD *px, unsigned w, unsigned h)
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
int dxt_has_transparency(const FormatInfo *fi, const unsigned char *src, unsigned w, unsigned h)
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

int format_info(uint32_t f, FormatInfo *fi)
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
unsigned swizzle(unsigned x, unsigned y, unsigned w, unsigned h)
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

unsigned long long sample_hash(const unsigned char *p, unsigned bytes)
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
unsigned long long quick_hash(const unsigned char *p, unsigned bytes)
{
    static int full = -1; /* REJET_TEXFULL=1: every word of every texture (testing missed changes) */
    unsigned a = 2166136261u, b = 0x9E3779B9u, step;
    if (full < 0)
        full = GetEnvironmentVariableA("REJET_TEXFULL", NULL, 0) > 0;
    step = bytes > 65536 && !full ? 61 : 1;
    for (unsigned i = 0; i + 4 <= bytes; i += 4 * step) {
        unsigned v = *(const unsigned *)(p + i);
        a = (a ^ v) * 16777619u;
        b = (b + v) * 0x85EBCA6Bu;
        b ^= b >> 13;
    }
    return (unsigned long long)a << 32 | b;
}

unsigned level_bytes(const FormatInfo *fi, unsigned w, unsigned h)
{
    if (fi->dxt)
        return ((w + 3) / 4) * ((h + 3) / 4) * (fi->bpp * 2); /* 8 or 16 bytes per block */
    return w * h * fi->bpp / 8;
}

/* Xbox 3D swizzle: interleave x, y and z bits (in that order) while each has bits left. */
unsigned swizzle3(unsigned x, unsigned y, unsigned z, unsigned w, unsigned h, unsigned d)
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

