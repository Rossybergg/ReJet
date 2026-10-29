/* Texture data conversion, Xbox to host (see tex.c): format table, DXT decoding with colour
 * bleed, (un)swizzling and content hashes. Pure functions on memory, so tests/ can check them. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>

typedef struct FormatInfo {
    D3DFORMAT host;
    unsigned bpp;     /* bits per pixel (DXT: per pixel of a 4x4 block) */
    int linear, dxt;
    int expand_al8;   /* AL8: one byte is both luminance and alpha */
    D3DFORMAT host_dxt; /* the DXT kind (DXT1/3/5), kept when decoding */
    int decode;       /* DXT with transparency: decoded to A8R8G8B8 with colour bled into transparent texels */
} FormatInfo;

/* Host format for an Xbox format; 0 if unsupported. */
int format_info(uint32_t f, FormatInfo *fi);
/* Bytes in one level (one slice) of w x h. */
unsigned level_bytes(const FormatInfo *fi, unsigned w, unsigned h);
/* Offset in texels of (x, y) in a swizzled w x h surface, and of (x, y, z) in a swizzled volume. */
unsigned swizzle(unsigned x, unsigned y, unsigned w, unsigned h);
unsigned swizzle3(unsigned x, unsigned y, unsigned z, unsigned w, unsigned h, unsigned d);
/* Decode DXT1/3/5 (fi->host_dxt) to A8R8G8B8. */
void dxt_decode(const FormatInfo *fi, const unsigned char *src, unsigned w, unsigned h, DWORD *dst);
/* Give transparent texels (alpha 0) the average colour of their opaque neighbours (4 passes,
 * wrapping); alpha is unchanged. */
void bleed_colour(DWORD *px, unsigned w, unsigned h);
/* Does a DXT texture have transparent texels (on level 0)? */
int dxt_has_transparency(const FormatInfo *fi, const unsigned char *src, unsigned w, unsigned h);
/* The texture's name for HD replacements (hdtex): never change it, or packs stop matching. */
unsigned long long sample_hash(const unsigned char *p, unsigned bytes);
/* The per-frame change check (32-bit arithmetic; REJET_TEXFULL=1 reads every word). */
unsigned long long quick_hash(const unsigned char *p, unsigned bytes);
