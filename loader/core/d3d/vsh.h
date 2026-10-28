/* NV2A vertex program translation (see vsh.c). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#define VSH_C_SCREEN  192 /* (2/RTwidth, -2/RTheight, 1, 1) */
#define VSH_C_TEXSCALE 193 /* 193..196: per-stage texcoord scale */
#define VSH_C_2D 197      /* (x scale for 2D programs, 1, 1, 1): keeps 2D at 4:3 in widescreen */

/* After vsh_compile/vsh_to_hlsl: nonzero if that program is 2D (gives screen positions itself,
 * without the viewport-constant epilogue of 3D programs). */
int vsh_last_is_2d(void);

/* After vsh_compile: the input register copied straight to texture coordinate 0, or -1. */
int vsh_last_t0_input(void);

/* Byte offset of a float input register (2+ components) in the declaration's vertex, or -1. */
int vsh_input_offset(const DWORD *decl, unsigned reg);
#define VSH_CONSTANTS 198

int vsh_to_hlsl(const DWORD *program, const DWORD *decl, char *hlsl, size_t cap);
IDirect3DVertexShader9 *vsh_compile(IDirect3DDevice9 *dev, const DWORD *program, const DWORD *decl, unsigned index);
IDirect3DVertexDeclaration9 *vsh_declaration(IDirect3DDevice9 *dev, const DWORD *decl, unsigned *stride_out);
