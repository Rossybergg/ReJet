/* Xbox pixel shaders (NV2A register combiners, D3DPIXELSHADERDEF) -> HLSL ps_2_b.
 *
 * Definition layout (dwords): 0-7 alpha inputs, 8 final ABCD, 9 final EFG, 10-17 constant 0,
 * 18-25 constant 1, 26-33 alpha outputs, 34-41 RGB inputs, 42 compare mode, 43-44 final
 * constants, 45-52 RGB outputs, 53 combiner count | flags, 54 texture modes (5 bits/stage),
 * 55 dot mapping, 56 input texture, 57 C0 mapping, 58 C1 mapping, 59 final constant mapping.
 * Checked against the game's 8 shaders (e.g. #0: r0.a = t2.a; r1 = mux(c0, c1); r0 = t0*r1*t1).
 *
 * Inputs: byte = mapping (bits 5-7) | alpha channel (bit 4) | register (bits 0-3). Outputs:
 * cd (bits 0-3), ab (4-7), sum/mux (8-11), flags (12+: 1 cd dot, 2 ab dot, 4 mux, 0x38 output
 * mapping, 0x40/0x80 cd/ab blue-to-alpha). Constants: c[0..7] stage C0, c[8..15] stage C1,
 * c[16] / c[17] final-combiner constants, all filled per draw by the shim. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "kernel.h"
#include "psh.h"
#include "shadercache.h"

typedef struct Out {
    char *p;
    size_t left;
} Out;

static void emit(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < o->left) {
        o->p += n;
        o->left -= (size_t)n;
    }
    else {
        o->left = 0;
    }
}

/* Inside a combiner stage, inputs read the snapshot taken at the stage start (S_*), because
 * both halves of a stage read before either writes. */
static const char *g_prefix = "";

/* Register value as float4 (stage-specific constants). */
static void reg_name(char *buf, size_t cap, unsigned reg, unsigned stage)
{
    switch (reg) {
    case 0x0: snprintf(buf, cap, "float4(0, 0, 0, 0)"); break;
    case 0x1: snprintf(buf, cap, "c[%u]", stage); break;
    case 0x2: snprintf(buf, cap, "c[%u]", 8 + stage); break;
    case 0x3: snprintf(buf, cap, "fog"); break;
    case 0x4: snprintf(buf, cap, "%sv0", g_prefix); break;
    case 0x5: snprintf(buf, cap, "%sv1", g_prefix); break;
    case 0x8: snprintf(buf, cap, "%st0", g_prefix); break;
    case 0x9: snprintf(buf, cap, "%st1", g_prefix); break;
    case 0xA: snprintf(buf, cap, "%st2", g_prefix); break;
    case 0xB: snprintf(buf, cap, "%st3", g_prefix); break;
    case 0xC: snprintf(buf, cap, "%sr0", g_prefix); break;
    case 0xD: snprintf(buf, cap, "%sr1", g_prefix); break;
    case 0xE: snprintf(buf, cap, "float4(v1.rgb + r0.rgb, 0)"); break; /* V1R0_SUM (final only) */
    case 0xF: snprintf(buf, cap, "ef"); break;                         /* EF_PROD (final only) */
    default: snprintf(buf, cap, "float4(0, 0, 0, 0)"); break;
    }
}

/* One combiner input (byte) as an HLSL float3 (rgb) or float (alpha) expression. */
static void input(char *buf, size_t cap, unsigned byte, unsigned stage, int rgb_portion)
{
    char reg[48], val[96];
    unsigned r = byte & 0xF, alpha = (byte >> 4) & 1, map = byte & 0xE0;
    reg_name(reg, sizeof reg, r, stage);
    if (rgb_portion)
        snprintf(val, sizeof val, alpha ? "%s.aaa" : "%s.rgb", reg);
    else
        snprintf(val, sizeof val, alpha ? "%s.a" : "%s.b", reg); /* alpha portion reads blue for rgb */
    switch (map) {
    case 0x00: snprintf(buf, cap, "max(%s, 0)", val); break;               /* unsigned identity */
    case 0x20: snprintf(buf, cap, "(1 - saturate(%s))", val); break;       /* unsigned invert */
    case 0x40: snprintf(buf, cap, "(2 * max(%s, 0) - 1)", val); break;     /* expand normal */
    case 0x60: snprintf(buf, cap, "(1 - 2 * max(%s, 0))", val); break;     /* expand negate */
    case 0x80: snprintf(buf, cap, "(max(%s, 0) - 0.5)", val); break;       /* half-bias normal */
    case 0xA0: snprintf(buf, cap, "(0.5 - max(%s, 0))", val); break;       /* half-bias negate */
    case 0xC0: snprintf(buf, cap, "(%s)", val); break;                     /* signed identity */
    default: snprintf(buf, cap, "(-(%s))", val); break;                   /* signed negate */
    }
}

static const char *out_reg(unsigned r)
{
    switch (r) {
    case 0x4: return "v0";
    case 0x5: return "v1";
    case 0x8: return "t0";
    case 0x9: return "t1";
    case 0xA: return "t2";
    case 0xB: return "t3";
    case 0xC: return "r0";
    case 0xD: return "r1";
    default: return NULL; /* 0 = discard */
    }
}

static const char *out_map(unsigned flags)
{
    switch (flags & 0x38) {
    case 0x08: return "(%s - 0.5)";
    case 0x10: return "(%s * 2)";
    case 0x18: return "((%s - 0.5) * 2)";
    case 0x20: return "(%s * 4)";
    case 0x30: return "(%s * 0.5)";
    default: return "(%s)";
    }
}

/* One stage half (rgb or alpha). */
static void stage_half(Out *o, unsigned stage, DWORD inputs, DWORD outputs, int rgb, int mux_msb)
{
    char a[160], b[160], c[160], d[160];
    unsigned cd = outputs & 0xF, ab = (outputs >> 4) & 0xF, sum = (outputs >> 8) & 0xF, flags = outputs >> 12;
    const char *t = rgb ? "float3" : "float", *sw = rgb ? ".rgb" : ".a";
    char ab_e[400], cd_e[400], tmp[460];

    if (!cd && !ab && !sum)
        return;
    input(a, sizeof a, (inputs >> 24) & 0xFF, stage, rgb);
    input(b, sizeof b, (inputs >> 16) & 0xFF, stage, rgb);
    input(c, sizeof c, (inputs >> 8) & 0xFF, stage, rgb);
    input(d, sizeof d, inputs & 0xFF, stage, rgb);
    if (rgb && (flags & 2))
        snprintf(ab_e, sizeof ab_e, "dot(%s, %s).xxx", a, b);
    else
        snprintf(ab_e, sizeof ab_e, "%s * %s", a, b);
    if (rgb && (flags & 1))
        snprintf(cd_e, sizeof cd_e, "dot(%s, %s).xxx", c, d);
    else
        snprintf(cd_e, sizeof cd_e, "%s * %s", c, d);
    emit(o, "    { %s ab = %s; %s cd = %s;\n", t, ab_e, t, cd_e);
    if (flags & 4) /* mux on r0.a: MSB (>= 0.5) or LSB of the 8-bit value */
        emit(o, mux_msb ? "      %s s = (%sr0.a >= 0.5) ? cd : ab;\n"
                        : "      %s s = (frac(%sr0.a * 255.0 / 2.0) >= 0.25) ? cd : ab;\n",
             t, g_prefix);
    else
        emit(o, "      %s s = ab + cd;\n", t);
    /* Writes after all reads (a stage reads its inputs before writing). */
    if (ab && out_reg(ab)) {
        snprintf(tmp, sizeof tmp, out_map(flags), "ab");
        emit(o, "      %s%s = clamp(%s, -1, 1);\n", out_reg(ab), sw, tmp);
        if (rgb && (flags & 0x80))
            emit(o, "      %s.a = %s.b;\n", out_reg(ab), out_reg(ab));
    }
    if (cd && out_reg(cd)) {
        snprintf(tmp, sizeof tmp, out_map(flags), "cd");
        emit(o, "      %s%s = clamp(%s, -1, 1);\n", out_reg(cd), sw, tmp);
        if (rgb && (flags & 0x40))
            emit(o, "      %s.a = %s.b;\n", out_reg(cd), out_reg(cd));
    }
    if (sum && out_reg(sum)) {
        snprintf(tmp, sizeof tmp, out_map(flags), "s");
        emit(o, "      %s%s = clamp(%s, -1, 1);\n", out_reg(sum), sw, tmp);
    }
    emit(o, "    }\n");
}

/* Final combiner input: only unsigned identity / invert apply; register 0xE/0xF allowed. */
static void final_input(char *buf, size_t cap, unsigned byte, int rgb)
{
    char reg[48];
    unsigned r = byte & 0xF, alpha = (byte >> 4) & 1;
    if (r == 0x1)
        snprintf(reg, sizeof reg, "c[16]");
    else if (r == 0x2)
        snprintf(reg, sizeof reg, "c[17]");
    else
        reg_name(reg, sizeof reg, r, 0);
    if (rgb)
        snprintf(buf, cap, (byte & 0x20) ? "(1 - saturate(%s.%s))" : "saturate(%s.%s)", reg, alpha ? "aaa" : "rgb");
    else
        snprintf(buf, cap, (byte & 0x20) ? "(1 - saturate(%s.%s))" : "saturate(%s.%s)", reg, alpha ? "a" : "b");
}

static const char *TEX_SAMPLE(unsigned mode, unsigned kind, unsigned i, char *buf, size_t cap)
{
    /* What is actually bound wins: NV2A depth textures are shadow-compared (lit = 1; shadow maps
     * are not rendered yet), and a cube map is looked up as one whatever the mode says. */
    if (mode && kind == PSH_BOUND_DEPTH)
        mode = 0x10;
    else if (mode && kind == PSH_BOUND_CUBE)
        mode = 3;
    else if (mode && kind == PSH_BOUND_VOLUME)
        mode = 0x11;
    switch (mode) {
    case 0x10: snprintf(buf, cap, "float4(1, 1, 1, 1)"); break;
    case 0x11: snprintf(buf, cap, "tex3Dproj(s%u, i.t%u)", i, i); break; /* volume (e.g. animated particles) */
    /* NV2A lookups are projective: PROJECT2D divides s,t by q (w). */
    case 1: snprintf(buf, cap, "tex2Dproj(s%u, float4(i.t%u.xy, 0, i.t%u.w))", i, i, i); break;
    case 2: snprintf(buf, cap, "tex2Dproj(s%u, i.t%u)", i, i); break; /* project 3D (2D texture: shadow map) */
    case 3: snprintf(buf, cap, "texCUBE(s%u, i.t%u.xyz)", i, i); break;                     /* cube map */
    case 4: snprintf(buf, cap, "saturate(i.t%u)", i); break;                                /* pass-through */
    default: snprintf(buf, cap, "float4(0, 0, 0, 0)"); break;                              /* unsupported */
    }
    return buf;
}

int psh_to_hlsl(const DWORD *def, unsigned bound, char *hlsl, size_t cap)
{
    Out o = {hlsl, cap};
    unsigned stages = def[53] & 0xFF, flags = (def[53] >> 8) & 0xFFF, modes = def[54];
    int mux_msb = (flags & 0x1) != 0;
    char s[96];

    if (stages > 8)
        stages = 8;
    emit(&o, "float4 c[18] : register(c0);\n");
    emit(&o, "sampler s0 : register(s0); sampler s1 : register(s1); sampler s2 : register(s2);"
             " sampler s3 : register(s3);\n");
    emit(&o, "struct PIn { float4 d0 : COLOR0; float4 d1 : COLOR1; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;"
             " float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n");
    emit(&o, "float4 main(PIn i) : COLOR {\n");
    emit(&o, "    float4 v0 = i.d0, v1 = i.d1, fog = float4(0, 0, 0, 0), ef = float4(0, 0, 0, 0);\n");
    for (unsigned k = 0; k < 4; k++)
        emit(&o, "    float4 t%u = %s;\n", k, TEX_SAMPLE((modes >> (5 * k)) & 0x1F, (bound >> (2 * k)) & 3, k, s, sizeof s));
    /* r0.a starts as t0.a on NV2A. */
    emit(&o, "    float4 r0 = float4(0, 0, 0, t0.a), r1 = float4(0, 0, 0, 0);\n");
    for (unsigned st = 0; st < stages; st++) {
        emit(&o, "    { // stage %u\n    float4 S_v0 = v0, S_v1 = v1, S_t0 = t0, S_t1 = t1, S_t2 = t2, S_t3 = t3,"
                 " S_r0 = r0, S_r1 = r1;\n", st);
        g_prefix = "S_";
        stage_half(&o, st, def[34 + st], def[45 + st], 1, mux_msb);
        stage_half(&o, st, def[0 + st], def[26 + st], 0, mux_msb);
        g_prefix = "";
        emit(&o, "    }\n");
    }
    {
        /* Debugging: REJET_PSOUT=<hlsl expression> replaces the output (e.g. v0, t0, r1). */
        char dbg[256] = "";
        if ((bound & PSH_DEBUG) && GetEnvironmentVariableA("REJET_PSOUT", dbg, sizeof dbg) < sizeof dbg && dbg[0]) {
            if (!strcmp(dbg, "id")) { /* colour-code by shader index (low 3 bits as r, g, b) */
                unsigned id = (bound >> 9) & 0xFF;
                emit(&o, "    return float4(0.25 + 0.75 * float3(%u, %u, %u), 1);\n}\n", id & 1, (id >> 1) & 1,
                     (id >> 2) & 1);
            }
            else
                emit(&o, "    return saturate(float4((%s).rgb, 1));\n}\n", dbg);
            return o.left > 0;
        }
    }
    if (!def[8] && !def[9]) {
        emit(&o, "    return saturate(r0);\n}\n");
    }
    else {
        char a[96], b[96], c[96], d[96], e[96], f[96], g[96];
        final_input(e, sizeof e, (def[9] >> 24) & 0xFF, 1);
        final_input(f, sizeof f, (def[9] >> 16) & 0xFF, 1);
        emit(&o, "    ef = float4(%s * %s, 0);\n", e, f);
        final_input(a, sizeof a, (def[8] >> 24) & 0xFF, 1);
        final_input(b, sizeof b, (def[8] >> 16) & 0xFF, 1);
        final_input(c, sizeof c, (def[8] >> 8) & 0xFF, 1);
        final_input(d, sizeof d, def[8] & 0xFF, 1);
        final_input(g, sizeof g, (def[9] >> 8) & 0xFF, 0);
        emit(&o, "    float3 fa = %s;\n    float3 rgb = fa * %s + (1 - fa) * %s + %s;\n", a, b, c, d);
        emit(&o, "    return saturate(float4(rgb, %s));\n}\n", g);
    }
    return o.left > 0;
}

IDirect3DPixelShader9 *psh_compile(IDirect3DDevice9 *dev, const DWORD *def, unsigned bound, unsigned index)
{
    static char hlsl[64 * 1024], err[1024];
    IDirect3DPixelShader9 *ps = NULL;
    void *code;

    if (!psh_to_hlsl(def, bound, hlsl, sizeof hlsl))
        return NULL;
    if ((code = shader_compile(hlsl, "nv2a_psh", "ps_2_b", err, sizeof err)) == NULL) {
        log_printf("psh: shader %u failed to compile: %s\n--\n%s\n", index, err, hlsl);
        return NULL;
    }
    if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)code, &ps)))
        ps = NULL;
    free(code);
    return ps;
}

/* Constants for one draw: per-stage C0/C1 from the definition, or from SetPixelShaderConstant
 * registers where the mapping nibble names one (0xF = unmapped). */
void psh_constants(const DWORD *def, const float user[16][4], float out[18][4])
{
    unsigned unique_c0 = (def[53] >> 8) & 0x10, unique_c1 = (def[53] >> 8) & 0x100;
    for (unsigned st = 0; st < 8; st++) {
        for (unsigned which = 0; which < 2; which++) {
            unsigned src_stage = (which == 0 ? unique_c0 : unique_c1) ? st : 0;
            unsigned map = ((which == 0 ? def[57] : def[58]) >> (4 * src_stage)) & 0xF;
            float *dst = out[which * 8 + st];
            if (map != 0xF && map < 16) {
                memcpy(dst, user[map], sizeof out[0]);
            }
            else {
                DWORD v = def[(which == 0 ? 10 : 18) + src_stage];
                dst[0] = ((v >> 16) & 0xFF) / 255.0f;
                dst[1] = ((v >> 8) & 0xFF) / 255.0f;
                dst[2] = (v & 0xFF) / 255.0f;
                dst[3] = ((v >> 24) & 0xFF) / 255.0f;
            }
        }
    }
    for (unsigned k = 0; k < 2; k++) {
        unsigned map = (def[59] >> (4 * k)) & 0xF;
        float *dst = out[16 + k];
        if (map != 0xF && map < 16) {
            memcpy(dst, user[map], sizeof out[0]);
        }
        else {
            DWORD v = def[43 + k];
            dst[0] = ((v >> 16) & 0xFF) / 255.0f;
            dst[1] = ((v >> 8) & 0xFF) / 255.0f;
            dst[2] = (v & 0xFF) / 255.0f;
            dst[3] = ((v >> 24) & 0xFF) / 255.0f;
        }
    }
}
