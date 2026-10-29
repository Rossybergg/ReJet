/* Xbox disc images (XDVDFS), read-only: the first-run setup lists and extracts the player's
 * own disc image with this (ui/setup.c). Either a trimmed XISO (game partition at offset 0) or a
 * full image (XGD1 at 0x18300000, as JSRF; XGD2/3 offsets accepted). The volume descriptor
 * ("MICROSOFT*XBOX*MEDIA") is at sector 32 of the partition; directory tables are binary trees
 * of entries: left, right (in dwords), start sector, size, attributes (0x10 = directory), name. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ISO_SECTOR 2048ull

typedef struct IsoFile {
    WCHAR path[260]; /* relative, backslashes */
    DWORD sector, size;
    int dir;
} IsoFile;

typedef struct Iso {
    HANDLE f;
    unsigned long long base; /* the game partition's offset in the image */
    IsoFile *files;
    unsigned n, cap;
    unsigned long long total; /* bytes in files */
} Iso;

/* Open an image and list every file and directory; 0 if it isn't an Xbox disc image. */
int iso_open(Iso *iso, const WCHAR *path);
void iso_close(Iso *iso);
/* A file (not a directory) by relative path, case-insensitively. */
const IsoFile *iso_find(const Iso *iso, const WCHAR *path);
/* Read `size` bytes at byte offset `at` of the image. */
int iso_read(Iso *iso, unsigned long long at, void *buf, DWORD size);
