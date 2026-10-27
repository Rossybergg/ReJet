/* Xc: kernel crypto exports. SHA-1, HMAC-SHA1 and RC4 are implemented here from the
 * public algorithm descriptions (FIPS 180-1, RFC 2104, RC4). State lives in the
 * caller-provided context buffers so contexts remain plain copyable memory, as on Xbox.
 * DES/3DES and RSA are added when something calls them. */
#include <string.h>

#include "kernel.h"
#include "xboxkrnl_impl.h"

typedef struct Sha1 {
    ULONG h[5];
    ULONGLONG bytes;
    UCHAR block[64];
} Sha1; /* 92 bytes; fits the Xbox SHA context buffer */

static ULONG rol(ULONG v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(Sha1 *s, const UCHAR *p)
{
    ULONG w[80], a, b, c, d, e;
    for (int i = 0; i < 16; i++)
        w[i] = (ULONG)p[4 * i] << 24 | (ULONG)p[4 * i + 1] << 16 | (ULONG)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    for (int i = 0; i < 80; i++) {
        ULONG f, k, t;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        }
        else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        }
        else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        }
        else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

static void sha1_init(Sha1 *s)
{
    s->h[0] = 0x67452301;
    s->h[1] = 0xEFCDAB89;
    s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476;
    s->h[4] = 0xC3D2E1F0;
    s->bytes = 0;
}

static void sha1_update(Sha1 *s, const UCHAR *p, ULONG n)
{
    while (n) {
        ULONG used = (ULONG)(s->bytes & 63), take = 64 - used < n ? 64 - used : n;
        memcpy(s->block + used, p, take);
        s->bytes += take;
        p += take;
        n -= take;
        if ((s->bytes & 63) == 0)
            sha1_block(s, s->block);
    }
}

static void sha1_final(Sha1 *s, UCHAR digest[20])
{
    ULONGLONG bits = s->bytes * 8;
    UCHAR pad = 0x80, zero = 0, len[8];
    sha1_update(s, &pad, 1);
    while ((s->bytes & 63) != 56)
        sha1_update(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        len[i] = (UCHAR)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (int i = 0; i < 20; i++)
        digest[i] = (UCHAR)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

VOID XBAPI xbox_XcSHAInit(PUCHAR pbSHAContext)
{
    sha1_init((Sha1 *)pbSHAContext);
}

VOID XBAPI xbox_XcSHAUpdate(PUCHAR pbSHAContext, PUCHAR pbInput, ULONG dwInputLength)
{
    sha1_update((Sha1 *)pbSHAContext, pbInput, dwInputLength);
}

VOID XBAPI xbox_XcSHAFinal(PUCHAR pbSHAContext, PUCHAR pbDigest)
{
    sha1_final((Sha1 *)pbSHAContext, pbDigest);
}

/* HMAC-SHA1 over Data || Data2 (RFC 2104). */
VOID XBAPI xbox_XcHMAC(PUCHAR pbKeyMaterial, ULONG cbKeyMaterial, PUCHAR pbData, ULONG cbData, PUCHAR pbData2,
                       ULONG cbData2, PUCHAR HmacData)
{
    UCHAR key[64], pad[64], inner[20];
    Sha1 s;
    memset(key, 0, sizeof key);
    if (cbKeyMaterial > 64) {
        sha1_init(&s);
        sha1_update(&s, pbKeyMaterial, cbKeyMaterial);
        sha1_final(&s, key);
    }
    else {
        memcpy(key, pbKeyMaterial, cbKeyMaterial);
    }
    for (int i = 0; i < 64; i++)
        pad[i] = key[i] ^ 0x36;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    if (pbData)
        sha1_update(&s, pbData, cbData);
    if (pbData2)
        sha1_update(&s, pbData2, cbData2);
    sha1_final(&s, inner);
    for (int i = 0; i < 64; i++)
        pad[i] = key[i] ^ 0x5C;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, HmacData);
}

/* RC4 key structure: i, j, S[256] (258 bytes, as on Xbox). */
VOID XBAPI xbox_XcRC4Key(PUCHAR pbKeyStruct, ULONG dwKeyLength, PUCHAR pbKey)
{
    UCHAR *S = pbKeyStruct + 2, j = 0;
    for (int i = 0; i < 256; i++)
        S[i] = (UCHAR)i;
    for (int i = 0; i < 256; i++) {
        UCHAR t = S[i];
        j = (UCHAR)(j + t + pbKey[i % dwKeyLength]);
        S[i] = S[j];
        S[j] = t;
    }
    pbKeyStruct[0] = 0;
    pbKeyStruct[1] = 0;
}

VOID XBAPI xbox_XcRC4Crypt(PUCHAR pbKeyStruct, ULONG dwInputLength, PUCHAR pbInput)
{
    UCHAR i = pbKeyStruct[0], j = pbKeyStruct[1], *S = pbKeyStruct + 2;
    for (ULONG n = 0; n < dwInputLength; n++) {
        UCHAR t;
        i = (UCHAR)(i + 1);
        j = (UCHAR)(j + S[i]);
        t = S[i];
        S[i] = S[j];
        S[j] = t;
        pbInput[n] ^= S[(UCHAR)(S[i] + S[j])];
    }
    pbKeyStruct[0] = i;
    pbKeyStruct[1] = j;
}

void sha1_digest(const void *data, unsigned long size, UCHAR digest[20])
{
    Sha1 s;
    sha1_init(&s);
    sha1_update(&s, (const UCHAR *)data, size);
    sha1_final(&s, digest);
}

/* Self-test against the FIPS 180-1 "abc" vector; logged once at start-up. */
void xc_selftest(void)
{
    static const UCHAR expect[20] = {0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E,
                                     0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D};
    Sha1 s;
    UCHAR d[20];
    sha1_init(&s);
    sha1_update(&s, (const UCHAR *)"abc", 3);
    sha1_final(&s, d);
    log_printf("xc: SHA-1 self-test %s\n", memcmp(d, expect, 20) ? "FAILED" : "ok");
}
