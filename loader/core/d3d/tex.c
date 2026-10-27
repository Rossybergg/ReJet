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
    unsigned long long hash;
    unsigned long checked; /* tex frame of the last content check */
    IDirect3DBaseTexture9 *host;
    int hd; /* host is an HD replacement (hdtex.c), not sized like the original */
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
} FormatInfo;

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
    case XD3DFMT_DXT1: fi->host = D3DFMT_DXT1; fi->bpp = 4; fi->dxt = 1; return 1;
    case XD3DFMT_DXT2: fi->host = D3DFMT_DXT3; fi->bpp = 8; fi->dxt = 1; return 1;
    case XD3DFMT_DXT4: fi->host = D3DFMT_DXT5; fi->bpp = 8; fi->dxt = 1; return 1;
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
        unsigned bpp = fi->bpp / 8;
        for (unsigned y = 0; y < h; y++) {
            unsigned char *d = dst + y * lr->Pitch;
            for (unsigned x = 0; x < w; x++) {
                const unsigned char *s = src + swizzle(x, y, w, h) * bpp;
                if (fi->expand_al8)
                    ((unsigned short *)d)[x] = (unsigned short)(s[0] * 0x0101);
                else
                    memcpy(d + x * (fi->expand_al8 ? 2 : bpp), s, bpp);
            }
        }
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
    unsigned long long hash;
    LONGLONG t_up;

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
        hash = sample_hash(data, total);
        if (e->hash == hash)
            return e->host;
    }
    else {
        hash = sample_hash(data, total);
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
        perf_end(PERF_TEXUP, t_up);
        return e->host;
    }
    if (!e->host) {
        HRESULT hr;
        if (volume) {
            static LONG logged;
            hr = IDirect3DDevice9_CreateVolumeTexture(dev, w, h, depth, levels, D3DUSAGE_DYNAMIC, fi.host,
                                                      D3DPOOL_DEFAULT, (IDirect3DVolumeTexture9 **)&e->host, NULL);
            if (InterlockedIncrement(&logged) <= 8)
                log_printf("tex: volume %ux%ux%u levels %u fmt %02X -> %08lX\n", w, h, depth, levels, xf, hr);
        }
        else if (faces == 6)
            hr = IDirect3DDevice9_CreateCubeTexture(dev, w, levels, D3DUSAGE_DYNAMIC, fi.host, D3DPOOL_DEFAULT,
                                                    (IDirect3DCubeTexture9 **)&e->host, NULL);
        else
            hr = IDirect3DDevice9_CreateTexture(dev, w, h, levels, D3DUSAGE_DYNAMIC, fi.host, D3DPOOL_DEFAULT,
                                                (IDirect3DTexture9 **)&e->host, NULL);
        if (FAILED(hr)) {
            log_printf("tex: CreateTexture %ux%u fmt %02X failed (%08lX)\n", w, h, xf, hr);
            e->host = NULL;
            return NULL;
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
            if (SUCCEEDED(IDirect3DVolumeTexture9_LockBox((IDirect3DVolumeTexture9 *)e->host, l, &lb, NULL,
                                                          l == 0 ? D3DLOCK_DISCARD : 0))) {
                upload_volume_level(&fi, p, lw, lh, ld, &lb);
                IDirect3DVolumeTexture9_UnlockBox((IDirect3DVolumeTexture9 *)e->host, l);
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
                /* DISCARD may drop the whole resource, so only the very first lock may use it. */
                DWORD lock = (f == 0 && l == 0) ? D3DLOCK_DISCARD : 0;
                HRESULT hr = faces == 6
                    ? IDirect3DCubeTexture9_LockRect((IDirect3DCubeTexture9 *)e->host, (D3DCUBEMAP_FACES)f, l, &lr,
                                                     NULL, lock)
                    : IDirect3DTexture9_LockRect((IDirect3DTexture9 *)e->host, l, &lr, NULL, lock);
                if (SUCCEEDED(hr)) {
                    upload_level(&fi, p, lw, lh, pitch, &lr);
                    if (faces == 6)
                        IDirect3DCubeTexture9_UnlockRect((IDirect3DCubeTexture9 *)e->host, (D3DCUBEMAP_FACES)f, l);
                    else
                        IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)e->host, l);
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
            if (SUCCEEDED(IDirect3DTexture9_LockRect((IDirect3DTexture9 *)e->host, 0, &lr, NULL, D3DLOCK_READONLY))) {
                if ((fp = fopen(name, "wb")) != NULL) {
                    for (unsigned y = 0; y < h; y++)
                        fwrite((const unsigned char *)lr.pBits + y * lr.Pitch, 4, w, fp);
                    fclose(fp);
                }
                IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)e->host, 0);
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
                SUCCEEDED(IDirect3DTexture9_LockRect((IDirect3DTexture9 *)e->host, 0, &lr, NULL, D3DLOCK_READONLY))) {
                if ((fp = fopen(name, "wb")) != NULL) {
                    for (unsigned y = 0; y < rows; y++)
                        fwrite((const unsigned char *)lr.pBits + y * lr.Pitch, 1, row, fp);
                    fclose(fp);
                }
                IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)e->host, 0);
            }
        }
    }
    e->data = x->Resource.Data;
    e->format = x->Format;
    e->size = x->Size;
    e->hash = hash;
    perf_end(PERF_TEXUP, t_up);
    return e->host;
}
