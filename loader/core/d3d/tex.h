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

/* The texture's content name (sample_hash, as for HD replacements) from its last upload; 0 if
 * it hasn't been used yet. Recognises specific images, such as the graffiti warning. */
unsigned long long tex_name(const XD3DPixelContainer *x);
/* Level 0 of an Xbox 2D texture as a PNG (for artists redrawing HUD pieces); 1 if written. */
int tex_dump_png(const XD3DPixelContainer *x, const char *path);
