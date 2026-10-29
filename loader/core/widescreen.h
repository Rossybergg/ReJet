/* Widescreen (Hor+) maths, used by d3d/d3d.c: pure functions so tests/ can check them. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9types.h>

/* How much wider than 4:3 a w x h picture is (1 = 4:3). */
float ws_k(unsigned w, unsigned h);

/* The engine's view-frustum side planes (1/cos, -tan, 1/cos, tan of the half angle), widened
 * so the half angle's tangent is k times larger; k <= 1.01 leaves them alone. */
void ws_widen_planes(float planes[4], float k);

/* A perspective projection's x scale divided by k, in place (the matrix is game memory and may
 * be patched again): a matrix already patched is recognised, so the scale is never divided
 * twice, and a new k re-applies from the original. Other matrices are left alone. */
typedef struct WsProjection {
    float original, patched;
} WsProjection;
void ws_patch_projection(WsProjection *st, D3DMATRIX *m, float k);
