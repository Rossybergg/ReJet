/* Texture conversion (loader/core/d3d/texconv.c). */
#include <stdlib.h>
#include <string.h>

#include "d3d/texconv.h"
#include "test.h"
#include "xbox/xbox_d3d.h"

/* Uploads unswizzle with one table per axis (offset = x part | y part); that is only right
 * if the swizzle really is separable like that, and a bijection, for every shape the game uses,
 * square or not. A mistake here scrambles every swizzled texture. */
static void swizzle_tables(void)
{
    static const unsigned sizes[][2] = {{1, 1}, {4, 4}, {64, 64}, {256, 64}, {8, 512}, {512, 512}, {1024, 16}};
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        unsigned w = sizes[s][0], h = sizes[s][1], bad = 0;
        unsigned char *hit = (unsigned char *)calloc((size_t)w * h, 1);
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++) {
                unsigned o = swizzle(x, y, w, h);
                if (o != (swizzle(x, 0, w, h) | swizzle(0, y, w, h)) || o >= w * h || hit[o]++)
                    bad++;
            }
        CHECK_EQ(bad, 0);
        free(hit);
    }
    /* the Xbox order: x bit first, then y, while both have bits left */
    CHECK_EQ(swizzle(1, 0, 4, 4), 1);
    CHECK_EQ(swizzle(0, 1, 4, 4), 2);
    CHECK_EQ(swizzle(2, 0, 4, 4), 4);
    CHECK_EQ(swizzle(3, 3, 4, 4), 15);
    CHECK_EQ(swizzle(5, 0, 8, 2), 1 | 8); /* y runs out after one bit: x continues alone */
}

/* Dark seams around 2D art at high resolution: transparent texels must take the colour of
 * their opaque neighbours (alpha stays 0), and opaque ones must not change. */
static void colour_bleed(void)
{
    DWORD px[4 * 4];
    for (int i = 0; i < 16; i++)
        px[i] = 0x00000000; /* transparent black */
    px[5] = 0xFFFF0000;     /* one opaque red texel */
    bleed_colour(px, 4, 4);
    CHECK_EQ(px[5], 0xFFFF0000);
    for (int i = 0; i < 16; i++)
        if (i != 5) {
            CHECK_EQ(px[i] >> 24, 0);           /* still transparent */
            CHECK_EQ(px[i] & 0xFFFFFF, 0xFF0000); /* but red, not black */
        }
}

/* DXT1 three-colour blocks (colour0 <= colour1) use index 3 for transparent black: those
 * textures are decoded and bled; four-colour blocks have no transparency. */
static void dxt1_transparency(void)
{
    FormatInfo fi;
    unsigned char block[8];
    DWORD out[16];
    CHECK(format_info(XD3DFMT_DXT1, &fi));
    /* three-colour: c0 = c1 = white, all indices 3 (transparent) */
    block[0] = block[2] = 0xFF;
    block[1] = block[3] = 0xFF;
    block[4] = block[5] = block[6] = block[7] = 0xFF;
    CHECK(dxt_has_transparency(&fi, block, 4, 4));
    dxt_decode(&fi, block, 4, 4, out);
    CHECK_EQ(out[0] >> 24, 0);
    /* four-colour: c0 (white) > c1 (black), all indices 0 (c0): opaque white */
    block[2] = block[3] = 0;
    memset(block + 4, 0, 4);
    CHECK(!dxt_has_transparency(&fi, block, 4, 4));
    dxt_decode(&fi, block, 4, 4, out);
    CHECK_EQ(out[15], 0xFFFFFFFF);
}

/* sample_hash names textures for HD replacement packs (hdtex.c): if its value for the same
 * data ever changes, every existing pack and texture dump silently stops matching. Pinned for
 * a small (every word) and a large (sampled) texture, with the values of release 0.2.1. */
static void hd_texture_names(void)
{
    unsigned char *p = (unsigned char *)malloc(128 * 1024);
    for (unsigned i = 0; i < 128 * 1024; i++)
        p[i] = (unsigned char)(i * 7 + (i >> 9));
    CHECK_EQ(sample_hash(p, 4096), 0x41F110ADD8F12C83ll);
    CHECK_EQ(sample_hash(p, 128 * 1024), 0x3401813203D456D5ll);
    free(p);
}

/* The per-frame change check must see a one-word change anywhere in a small texture (every
 * word is read up to 64 KB), or textures the game rewrites (movie frames) go stale. */
static void change_check(void)
{
    unsigned char *p = (unsigned char *)calloc(65536, 1);
    unsigned long long h0 = quick_hash(p, 65536);
    for (unsigned at = 0; at < 65536; at += 4096 + 4) {
        p[at] ^= 1;
        CHECK(quick_hash(p, 65536) != h0);
        p[at] ^= 1;
    }
    CHECK(quick_hash(p, 65536) == h0);
    free(p);
}

void test_texconv(void)
{
    swizzle_tables();
    colour_bleed();
    dxt1_transparency();
    hd_texture_names();
    change_check();
}
