/* HLE patches on XDK functions inside the XBE (see hle.c). */
#pragma once

typedef struct HlePatch {
    unsigned address; /* function entry in Default.xbe (XDK_* from xdk_addrs.gen.h where named) */
    void *impl;       /* host replacement, same signature and calling convention */
    const char *name;
} HlePatch;

/* Verifies the XBE file's SHA-1, then patches every table entry. Returns 0 on mismatch. */
int hle_apply(const unsigned char *xbe_file, unsigned long xbe_size);
