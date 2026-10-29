/* Widescreen (Hor+) maths (see widescreen.h). */
#include <math.h>

#include "widescreen.h"

float ws_k(unsigned w, unsigned h)
{
    return ((float)w / (float)h) / (4.0f / 3.0f);
}

void ws_widen_planes(float planes[4], float k)
{
    if (k > 1.01f) {
        float t = planes[3] * k; /* tan of the wider half angle; the other term is 1 / cos */
        planes[0] = planes[2] = sqrtf(1.0f + t * t);
        planes[1] = -t;
        planes[3] = t;
    }
}

void ws_patch_projection(WsProjection *st, D3DMATRIX *m, float k)
{
    if (m->_34 == 0.0f || m->_44 != 0.0f) /* not a perspective projection */
        return;
    if (m->_11 != st->patched || st->patched == 0.0f)
        st->original = m->_11;
    m->_11 = st->original / k;
    st->patched = m->_11;
}
