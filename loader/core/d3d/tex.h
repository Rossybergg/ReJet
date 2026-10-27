/* Xbox texture resources -> Direct3D 9 textures (see tex.c). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include "xbox/xbox_d3d.h"

/* Host texture for an Xbox texture header (uploaded/refreshed as needed), or NULL if the
 * format is not supported yet. texcoord_scale: 1/size for linear (texel-addressed) formats. */
IDirect3DBaseTexture9 *tex_get(IDirect3DDevice9 *dev, const XD3DPixelContainer *x, float texcoord_scale[2]);

/* Nonzero for depth-buffer formats (sampled as shadow maps by the NV2A). */
int tex_is_depth(const XD3DPixelContainer *x);

/* Call once per presented frame: texture contents are re-checked at most once per frame. */
void tex_new_frame(void);
