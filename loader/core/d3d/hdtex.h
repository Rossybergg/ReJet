/* HD texture replacement packs (ReWhacked's hdtex.c) are not ported yet: no replacements. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

static __inline int hdtex_has(unsigned long long hash)
{
    (void)hash;
    return 0;
}

static __inline IDirect3DTexture9 *hdtex_load(IDirect3DDevice9 *dev, unsigned long long hash)
{
    (void)dev;
    (void)hash;
    return NULL;
}
