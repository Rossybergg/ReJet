/* Xbox register-combiner pixel shaders -> HLSL (see psh.c). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

/* `bound`: 2 bits per texture stage describing what is bound there (variants are compiled per value). */
#define PSH_BOUND_2D 0u
#define PSH_BOUND_CUBE 1u
#define PSH_BOUND_DEPTH 2u
#define PSH_BOUND_VOLUME 3u
#define PSH_DEBUG 0x100u /* apply REWHACKED_PSOUT */
int psh_to_hlsl(const DWORD *def, unsigned bound, char *hlsl, size_t cap);
IDirect3DPixelShader9 *psh_compile(IDirect3DDevice9 *dev, const DWORD *def, unsigned bound, unsigned index);
void psh_constants(const DWORD *def, const float user[16][4], float out[18][4]);
/* As on the Xbox: SetPixelShader loads the definition's own constants (psh_constants with
 * user NULL), then SetPixelShaderConstant(reg) overwrites only the stages whose C0/C1 (or
 * final combiner) mapping names reg. */
void psh_set_constant(const DWORD *def, unsigned reg, const float value[4], float live[18][4]);
