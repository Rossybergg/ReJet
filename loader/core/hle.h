/* HLE patches on XDK functions inside the XBE (see hle.c). */
#pragma once

typedef struct HlePatch {
    unsigned address; /* function entry in Default.xbe (XDK_* from xdk_addrs.gen.h where named) */
    void *impl;       /* host replacement, same signature and calling convention */
    const char *name;
} HlePatch;

/* Per-subsystem tables (d3d/d3d.c, ...). */
extern const HlePatch g_d3d_patches[];
extern const unsigned g_d3d_patch_count;
extern const HlePatch g_input_patches[];
extern const unsigned g_input_patch_count;
extern const HlePatch g_audio_patches[];
extern const unsigned g_audio_patch_count;

/* Nonzero if this is the Default.xbe ReJet supports (SHA-1 in config/xbe.sha1). */
int hle_xbe_supported(const unsigned char *xbe_file, unsigned long xbe_size);

/* Verifies the XBE file's SHA-1, then patches every table entry. Returns 0 on mismatch. */
int hle_apply(const unsigned char *xbe_file, unsigned long xbe_size);
