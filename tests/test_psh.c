/* Pixel shader (register combiner) constants (loader/core/d3d/psh.c). */
#include <string.h>

#include "d3d/psh.h"
#include "test.h"

/* A definition (D3DPIXELSHADERDEF dwords) set up like JSRF's graffiti combiner: 8 stages,
 * unique C0 per stage, stage 0 mapped to register 1, stage 2 to register 2, stage 5 to
 * register 0, the rest unmapped (F); each stage's own C0 in the definition is distinct. */
static void graffiti_like(DWORD def[64])
{
    memset(def, 0, 64 * sizeof def[0]);
    def[53] = 8 | (0x10 << 8);   /* PSCombinerCount: 8 stages, PS_COMBINERCOUNT_UNIQUE_C0 */
    def[57] = 0xFF0FF2F1;        /* PSC0Mapping, a nibble per stage (stage 0 lowest) */
    def[58] = 0xFFFFFFFF;        /* PSC1Mapping: none */
    def[59] = 0xFF;              /* final combiner constants: unmapped */
    for (int st = 0; st < 8; st++)
        def[10 + st] = 0xFF000000u | (unsigned)(st + 1) * 0x10; /* PSConstant0[st]: blue = (st+1)*16 */
}

/* On the Xbox, SetPixelShader loads the definition's own constants and SetPixelShaderConstant
 * overwrites only the stages mapped to that register (D3D 0x19A6A0). Using the game's
 * register for every mapped stage instead made a register the game never sets read as zero,
 * and sprayed graffiti came out fully transparent (2026-10-29). */
static void constants_follow_the_xbox(void)
{
    DWORD def[64];
    float live[18][4];
    const float zero[4] = {0, 0, 0, 0}, one[4] = {1, 1, 1, 1};
    graffiti_like(def);
    psh_constants(def, NULL, live); /* SetPixelShader */
    for (int st = 0; st < 8; st++)
        CHECK_NEAR(live[st][2], (st + 1) * 16 / 255.0, 1e-6); /* every stage: its own value */
    psh_set_constant(def, 0, zero, live); /* the game sets register 0... */
    CHECK_NEAR(live[5][2], 0.0, 1e-6);   /* ...which only stage 5 maps */
    CHECK_NEAR(live[0][2], 1 * 16 / 255.0, 1e-6);
    CHECK_NEAR(live[2][2], 3 * 16 / 255.0, 1e-6); /* register 2, never set: the definition's value */
    psh_set_constant(def, 1, one, live);
    CHECK_NEAR(live[0][0], 1.0, 1e-6); /* stage 0 maps register 1 */
    CHECK_NEAR(live[1][2], 2 * 16 / 255.0, 1e-6); /* unmapped stages never change */
    psh_constants(def, NULL, live); /* SetPixelShader again: back to the definition */
    CHECK_NEAR(live[5][2], 6 * 16 / 255.0, 1e-6);
}

/* Without UNIQUE_C0 every stage uses stage 0's constant and mapping. */
static void shared_constant(void)
{
    DWORD def[64];
    float live[18][4];
    const float one[4] = {1, 1, 1, 1};
    graffiti_like(def);
    def[53] = 8; /* PS_COMBINERCOUNT_SAME_C0 */
    psh_constants(def, NULL, live);
    for (int st = 0; st < 8; st++)
        CHECK_NEAR(live[st][2], 16 / 255.0, 1e-6);
    psh_set_constant(def, 1, one, live); /* stage 0's register: all stages */
    for (int st = 0; st < 8; st++)
        CHECK_NEAR(live[st][0], 1.0, 1e-6);
}

void test_psh(void)
{
    constants_follow_the_xbox();
    shared_constant();
}
