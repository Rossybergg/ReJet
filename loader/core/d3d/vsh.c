/* NV2A vertex program -> HLSL (vs_2_a), compiled with D3DCompile.
 *
 * Instruction encoding: 128 bits, dword 0 unused (see tools/loader/vsh_decode.py, which shares
 * this decoding and was used to validate it against the game's 276 programs).
 *
 * NV2A semantics kept:
 *  - MAC (A,B,C) and ILU (C, scalar) issue together; all sources are read before any write.
 *    When both are present the ILU result goes to r1.
 *  - Constants c[0..191] (D3D register n is NV2A c[n+96]); relative addressing via a0.x.
 *  - r12 reads as oPos.
 *  - oPos is in screen space. Programs either write pixels directly or end with the XDK
 *    epilogue (mul by c-38, rcc 1/w, mad c-37). The shim sets c-38/c-37 from the viewport like
 *    the Xbox runtime, and the HLSL epilogue maps screen space back to D3D9 clip space.
 * Extra constants beyond the NV2A range: c192 = (2/RTwidth, -2/RTheight, 1, 1) for the
 * screen->clip mapping, c193..c196 = per-stage texcoord scale (texel -> normalised for linear
 * textures). */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "kernel.h"
#include "shadercache.h"
#include "vsh.h"

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

static unsigned field(DWORD w, int shift, int bits)
{
    return (w >> shift) & ((1u << bits) - 1);
}

static const char *const OUT_NAME[16] = {"opos", NULL, NULL, "od0", "od1", "ofog", "opts", "ob0",
                                         "ob1",  "ot0", "ot1", "ot2", "ot3", NULL, NULL, NULL};

/* Set while translating if the program reads c-38 (the viewport scale its screen-space
 * epilogue uses): 3D programs do, 2D programs give screen pixels directly and do not. */
static int g_reads_viewport;

int vsh_last_is_2d(void)
{
    return !g_reads_viewport;
}

/* Source operand as an HLSL float4 expression. */
static void source(char *buf, size_t cap, unsigned mux, unsigned reg, unsigned v, unsigned cidx, unsigned a0x,
                   unsigned neg, unsigned sw)
{
    char base[48], s[6];
    static const char comp[4] = {'x', 'y', 'z', 'w'};
    if (mux == 1)
        snprintf(base, sizeof base, reg == 12 ? "opos" : "r%u", reg);
    else if (mux == 2)
        snprintf(base, sizeof base, "v%u", v);
    else if (mux == 3) {
        snprintf(base, sizeof base, a0x ? "c[a0 + %u]" : "c[%u]", cidx);
        if (!a0x && cidx == 96 - 38)
            g_reads_viewport = 1;
    }
    else
        snprintf(base, sizeof base, "float4(0,0,0,0)");
    s[0] = comp[(sw >> 6) & 3];
    s[1] = comp[(sw >> 4) & 3];
    s[2] = comp[(sw >> 2) & 3];
    s[3] = comp[sw & 3];
    s[4] = 0;
    snprintf(buf, cap, "%s(%s.%s)", neg ? "-" : "", base, s);
}

static void write_masked(Out *o, const char *dst, unsigned mask, const char *val)
{
    static const char comp[4] = {'x', 'y', 'z', 'w'};
    char m[5];
    unsigned n = 0;
    for (int i = 0; i < 4; i++)
        if (mask & (8u >> i))
            m[n++] = comp[i];
    m[n] = 0;
    if (n)
        emit(o, "    %s.%s = %s.%s;\n", dst, m, val, m);
}

/* Debugging: REJET_FULLBRIGHT=1 forces vertex colour to white (isolates vertex lighting). */
static int fullbright(void)
{
    static int v = -1;
    if (v < 0) {
        char env[4] = "";
        v = GetEnvironmentVariableA("REJET_FULLBRIGHT", env, sizeof env) && env[0] == '1';
    }
    return v;
}

/* Inputs present in the declaration (stream/register tokens), and their component counts. */
static void parse_decl(const DWORD *decl, unsigned present[16])
{
    memset(present, 0, 16 * sizeof present[0]);
    for (; decl && *decl != 0xFFFFFFFF; decl++) {
        DWORD t = *decl;
        if ((t >> 29) == 2 && !((t >> 28) & 1)) { /* register token: type in bits 16..23 */
            unsigned reg = t & 0x1F, type = (t >> 16) & 0xFF;
            if (reg < 16)
                present[reg] = (type >> 4) ? (type >> 4) : 4;
        }
    }
}

int vsh_to_hlsl(const DWORD *program, const DWORD *decl, char *hlsl, size_t cap)
{
    Out o = {hlsl, cap};
    unsigned n = program[0] >> 16, present[16];
    parse_decl(decl, present);

    g_reads_viewport = 0;
    emit(&o, "float4 c[198] : register(c0);\n");
    emit(&o, "struct VIn {\n");
    {
        unsigned any = 0;
        for (unsigned i = 0; i < 16; i++) {
            if (present[i]) {
                emit(&o, "  float%u v%u : TEXCOORD%u;\n", present[i], i, i);
                any = 1;
            }
        }
        if (!any)
            emit(&o, "  float dummy_ : TEXCOORD15;\n");
    }
    emit(&o, "};\n");
    emit(&o, "struct VOut { float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float fog : FOG;\n"
             "  float pts : PSIZE; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2;\n"
             "  float4 t3 : TEXCOORD3; };\n");
    emit(&o, "VOut main(VIn i) {\n");
    for (unsigned k = 0; k < 16; k++) {
        if (!present[k])
            emit(&o, "    float4 v%u = float4(0, 0, 0, 1);\n", k);
        else if (present[k] == 4)
            emit(&o, "    float4 v%u = i.v%u;\n", k, k);
        else /* missing components default to 0, w to 1 */
            emit(&o, "    float4 v%u = float4(i.v%u%s%s);\n", k, k,
                 present[k] == 1 ? ", 0, 0" : present[k] == 2 ? ", 0" : "", ", 1");
    }
    emit(&o, "    float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0,"
             " r10 = 0, r11 = 0;\n");
    /* NV2A defaults for unwritten outputs: texcoords (0, 0, 0, 1), so projective lookups divide by 1. */
    emit(&o, "    float4 opos = float4(0, 0, 0, 1), od0 = 0, od1 = 0, ofog = 0, opts = 0, ob0 = 0, ob1 = 0,"
             " ot0 = float4(0, 0, 0, 1), ot1 = float4(0, 0, 0, 1), ot2 = float4(0, 0, 0, 1),"
             " ot3 = float4(0, 0, 0, 1);\n");
    emit(&o, "    int a0 = 0;\n    float4 mac, ilu;\n");

    for (unsigned k = 0; k < n; k++) {
        const DWORD *w = program + 1 + 4 * k;
        DWORD w1 = w[1], w2 = w[2], w3 = w[3];
        unsigned ilu = field(w1, 25, 3), mac = field(w1, 21, 4), cidx = field(w1, 13, 8), v = field(w1, 9, 4);
        unsigned a0x = field(w3, 1, 1);
        unsigned mac_mask = field(w3, 24, 4), temp = field(w3, 20, 4), ilu_mask = field(w3, 16, 4);
        unsigned o_mask = field(w3, 12, 4), o_is_reg = field(w3, 11, 1), o_addr = field(w3, 3, 8);
        unsigned o_from_ilu = field(w3, 2, 1);
        char a[64], b[64], cc[64], val[160];
        unsigned creg = (field(w2, 0, 2) << 2) | field(w3, 30, 2);

        source(a, sizeof a, field(w2, 26, 2), field(w2, 28, 4), v, cidx, a0x, field(w1, 8, 1), field(w1, 0, 8));
        source(b, sizeof b, field(w2, 11, 2), field(w2, 13, 4), v, cidx, a0x, field(w2, 25, 1), field(w2, 17, 8));
        source(cc, sizeof cc, field(w3, 28, 2), creg, v, cidx, a0x, field(w2, 10, 1), field(w2, 2, 8));
        emit(&o, "    // %u: mac %u ilu %u\n", k, mac, ilu);

        /* Compute both results before any write. */
        if (mac) {
            switch (mac) {
            case 1: snprintf(val, sizeof val, "%s", a); break;
            case 2: snprintf(val, sizeof val, "%s * %s", a, b); break;
            case 3: snprintf(val, sizeof val, "%s + %s", a, cc); break;
            case 4: snprintf(val, sizeof val, "%s * %s + %s", a, b, cc); break;
            case 5: snprintf(val, sizeof val, "dot(%s.xyz, %s.xyz).xxxx", a, b); break;
            case 6: snprintf(val, sizeof val, "(dot(%s.xyz, %s.xyz) + %s.w).xxxx", a, b, b); break;
            case 7: snprintf(val, sizeof val, "dot(%s, %s).xxxx", a, b); break;
            case 8: snprintf(val, sizeof val, "float4(1, %s.y * %s.y, %s.z, %s.w)", a, b, a, b); break;
            case 9: snprintf(val, sizeof val, "min(%s, %s)", a, b); break;
            case 10: snprintf(val, sizeof val, "max(%s, %s)", a, b); break;
            case 11: snprintf(val, sizeof val, "float4(%s < %s)", a, b); break;
            case 12: snprintf(val, sizeof val, "float4(%s >= %s)", a, b); break;
            case 13: snprintf(val, sizeof val, "%s", a); break; /* arl */
            default:
                log_printf("vsh: unknown MAC op %u\n", mac);
                return 0;
            }
            emit(&o, "    mac = %s;\n", val);
        }
        if (ilu) {
            switch (ilu) {
            case 1: snprintf(val, sizeof val, "%s", cc); break;
            case 2: snprintf(val, sizeof val, "(1.0 / %s.x).xxxx", cc); break;
            case 3: /* reciprocal, clamped */
                snprintf(val, sizeof val,
                         "(sign(%s.x) * clamp(1.0 / abs(%s.x), 5.42101e-20, 1.884467e+19)).xxxx", cc, cc);
                break;
            case 4: snprintf(val, sizeof val, "(1.0 / sqrt(abs(%s.x))).xxxx", cc); break;
            case 5: snprintf(val, sizeof val, "float4(exp2(floor(%s.x)), frac(%s.x), exp2(%s.x), 1)", cc, cc, cc); break;
            case 6:
                snprintf(val, sizeof val,
                         "float4(floor(log2(abs(%s.x))), abs(%s.x) / exp2(floor(log2(abs(%s.x)))), "
                         "log2(abs(%s.x)), 1)", cc, cc, cc, cc);
                break;
            case 7:
                snprintf(val, sizeof val,
                         "float4(1, max(%s.x, 0), (%s.x > 0) ? pow(max(%s.y, 1e-30), clamp(%s.w, -127.9961, "
                         "127.9961)) : 0, 1)", cc, cc, cc, cc);
                break;
            default:
                log_printf("vsh: unknown ILU op %u\n", ilu);
                return 0;
            }
            emit(&o, "    ilu = %s;\n", val);
        }

        /* Writes. */
        if (mac == 13) {
            emit(&o, "    a0 = (int)floor(mac.x + 0.001);\n");
        }
        else if (mac && mac_mask) {
            char dst[8];
            snprintf(dst, sizeof dst, "r%u", temp);
            write_masked(&o, dst, mac_mask, "mac");
        }
        if (ilu && ilu_mask) {
            char dst[8];
            snprintf(dst, sizeof dst, "r%u", mac ? 1u : temp);
            write_masked(&o, dst, ilu_mask, "ilu");
        }
        if (o_mask) {
            const char *src = o_from_ilu ? "ilu" : "mac";
            if (!o_is_reg) {
                log_printf("vsh: program writes constants (unsupported), ignored\n");
            }
            else if (o_addr < 16 && OUT_NAME[o_addr]) {
                write_masked(&o, OUT_NAME[o_addr], o_mask, src);
            }
            else {
                log_printf("vsh: unknown output register %u\n", o_addr);
            }
        }
        if (field(w3, 0, 1))
            break; /* final */
    }

    /* Epilogue: screen space (pixels) -> D3D9 clip space. 2D programs (no viewport constant)
     * are squeezed horizontally by c[197].x so they keep 4:3 proportions in widescreen. */
    emit(&o, "    VOut o;\n"
             "    float w = (opos.w == 0) ? 1 : opos.w;\n"
             "    float2 ndc = float2(opos.x * c[192].x - 1, opos.y * c[192].y + 1);\n"
             "%s",
         g_reads_viewport ? "" : "    ndc.x *= c[197].x; // 2D program\n");
    emit(&o, 
             "    o.pos = float4(ndc * w, opos.z * w, w);\n"
             "    o.d0 = saturate(od0); o.d1 = saturate(od1); o.fog = ofog.x; o.pts = opts.x;\n"
             "%s"
             "    o.t0 = ot0 * c[193]; o.t1 = ot1 * c[194]; o.t2 = ot2 * c[195]; o.t3 = ot3 * c[196];\n"
             "    return o;\n}\n",
         fullbright() ? "    o.d0 = float4(1, 1, 1, o.d0.a); // REJET_FULLBRIGHT\n" : "");
    return o.left > 0;
}

IDirect3DVertexShader9 *vsh_compile(IDirect3DDevice9 *dev, const DWORD *program, const DWORD *decl, unsigned index)
{
    static char hlsl[256 * 1024], err[1024];
    IDirect3DVertexShader9 *vs = NULL;
    void *code;

    if (!vsh_to_hlsl(program, decl, hlsl, sizeof hlsl)) {
        log_printf("vsh: program %u could not be translated\n", index);
        return NULL;
    }
    if ((code = shader_compile(hlsl, "nv2a_vsh", "vs_2_a", err, sizeof err)) == NULL) {
        log_printf("vsh: program %u failed to compile: %s\n", index, err);
        return NULL;
    }
    if (FAILED(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)code, &vs)))
        vs = NULL;
    free(code);
    return vs;
}

/* D3D9 declaration for an Xbox one: register n -> TEXCOORDn, single stream. */
IDirect3DVertexDeclaration9 *vsh_declaration(IDirect3DDevice9 *dev, const DWORD *decl, unsigned *stride_out)
{
    D3DVERTEXELEMENT9 el[18];
    unsigned n = 0, offset = 0;
    IDirect3DVertexDeclaration9 *d = NULL;
    for (; decl && *decl != 0xFFFFFFFF && n < 16; decl++) {
        DWORD t = *decl;
        if ((t >> 29) == 2 && !((t >> 28) & 1)) {
            unsigned reg = t & 0x1F, type = (t >> 16) & 0xFF, comps = type >> 4, bytes;
            static const BYTE float_type[5] = {0, D3DDECLTYPE_FLOAT1, D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3,
                                               D3DDECLTYPE_FLOAT4};
            BYTE host;
            /* Xbox D3DVSDT_*: high nibble = components, low nibble = kind. The ones with a D3D9
             * equivalent (JSRF uses PBYTE4 for vertex colours: 0x44 on register 2). */
            switch (type) {
            case 0x12: case 0x22: case 0x32: case 0x42: host = float_type[comps]; bytes = 4 * comps; break;
            case 0x40: host = D3DDECLTYPE_D3DCOLOR; bytes = 4; break; /* D3DCOLOR (BGRA) */
            case 0x44: host = D3DDECLTYPE_UBYTE4N; bytes = 4; break;  /* PBYTE4: bytes / 255 */
            case 0x25: host = D3DDECLTYPE_SHORT2; bytes = 4; break;   /* SHORT2 */
            case 0x45: host = D3DDECLTYPE_SHORT4; bytes = 8; break;   /* SHORT4 */
            case 0x21: host = D3DDECLTYPE_SHORT2N; bytes = 4; break;  /* NORMSHORT2 */
            case 0x41: host = D3DDECLTYPE_SHORT4N; bytes = 8; break;  /* NORMSHORT4 */
            default:
                log_printf("vsh: unsupported declaration type %02X (register %u)\n", type, reg);
                return NULL;
            }
            el[n].Stream = 0;
            el[n].Offset = (WORD)offset;
            el[n].Type = host;
            el[n].Method = D3DDECLMETHOD_DEFAULT;
            el[n].Usage = D3DDECLUSAGE_TEXCOORD;
            el[n].UsageIndex = (BYTE)reg;
            offset += bytes;
            n++;
        }
    }
    {
        D3DVERTEXELEMENT9 end = D3DDECL_END();
        el[n] = end;
    }
    if (stride_out)
        *stride_out = offset;
    if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, el, &d)))
        return NULL;
    return d;
}
