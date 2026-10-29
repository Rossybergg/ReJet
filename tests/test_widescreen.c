/* Widescreen maths (loader/core/widescreen.c). */
#include <math.h>

#include "test.h"
#include "widescreen.h"

static D3DMATRIX perspective(float xscale)
{
    D3DMATRIX m = {0};
    m._11 = xscale;
    m._22 = 1.3333f;
    m._33 = 1.0f;
    m._34 = 1.0f; /* perspective: w = z */
    m._43 = -0.5f;
    return m;
}

/* The projection is patched in game memory, and the game may pass the same matrix again: the
 * x scale must be divided once, not again on every call (widening once compounded, 10-28). */
static void projection_patched_once(void)
{
    WsProjection st = {0};
    D3DMATRIX m = perspective(1.5f);
    float k = ws_k(1920, 1080);
    CHECK_NEAR(k, 4.0 / 3.0, 1e-5);
    ws_patch_projection(&st, &m, k);
    CHECK_NEAR(m._11, 1.5 / k, 1e-6);
    ws_patch_projection(&st, &m, k); /* the same, already patched matrix */
    CHECK_NEAR(m._11, 1.5 / k, 1e-6);
    ws_patch_projection(&st, &m, ws_k(2560, 1080)); /* aspect changed in the menu */
    CHECK_NEAR(m._11, 1.5 / ws_k(2560, 1080), 1e-6);
    ws_patch_projection(&st, &m, 1.0f); /* back to 4:3: the original */
    CHECK_NEAR(m._11, 1.5, 1e-6);
    {
        D3DMATRIX fresh = perspective(2.0f); /* a new projection (another fov) */
        ws_patch_projection(&st, &fresh, k);
        CHECK_NEAR(fresh._11, 2.0 / k, 1e-6);
    }
}

/* Orthographic and other non-perspective matrices (2D, shadows) are never touched. */
static void other_matrices_untouched(void)
{
    WsProjection st = {0};
    D3DMATRIX m = perspective(1.5f);
    m._34 = 0.0f;
    m._44 = 1.0f;
    ws_patch_projection(&st, &m, 4.0f / 3.0f);
    CHECK_NEAR(m._11, 1.5, 0);
}

/* Culling planes: objects at the widened edges popped in and out while the planes stayed 4:3
 * (10-28). Widened, they describe a half angle whose tangent is k times the original. */
static void culling_planes(void)
{
    float half = 0.5f, t = tanf(half);
    float planes[4] = {1.0f / cosf(half), -t, 1.0f / cosf(half), t};
    float k = ws_k(2560, 1080), tw = t * k;
    ws_widen_planes(planes, k);
    CHECK_NEAR(planes[3], tw, 1e-6);
    CHECK_NEAR(planes[1], -tw, 1e-6);
    CHECK_NEAR(planes[0], 1.0 / cos(atan(tw)), 1e-5);
    CHECK_NEAR(planes[2], planes[0], 0);
    {
        float p43[4] = {1.0f / cosf(half), -t, 1.0f / cosf(half), t};
        ws_widen_planes(p43, 1.0f); /* 4:3: unchanged */
        CHECK_NEAR(p43[3], t, 0);
    }
}

void test_widescreen(void)
{
    projection_patched_once();
    other_matrices_untouched();
    culling_planes();
}
