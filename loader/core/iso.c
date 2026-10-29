/* Xbox disc images (see iso.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "iso.h"

static const unsigned long long PARTITIONS[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};

int iso_read(Iso *iso, unsigned long long at, void *buf, DWORD size)
{
    LARGE_INTEGER pos;
    DWORD got = 0;
    pos.QuadPart = (LONGLONG)at;
    return SetFilePointerEx(iso->f, pos, NULL, FILE_BEGIN) && ReadFile(iso->f, buf, size, &got, NULL) && got == size;
}

static void iso_walk(Iso *iso, DWORD sector, DWORD size, const WCHAR *prefix, int depth)
{
    BYTE *table;
    unsigned stack[512], sp = 0;
    unsigned char *seen;
    if (!size || depth > 32 || size > (64u << 20) || !(table = (BYTE *)malloc(size)))
        return;
    seen = (unsigned char *)calloc(size / 4 + 1, 1);
    if (!seen || !iso_read(iso, iso->base + sector * ISO_SECTOR, table, size)) {
        free(seen);
        free(table);
        return;
    }
    stack[sp++] = 0;
    while (sp) {
        unsigned off = stack[--sp], left, right, nlen;
        IsoFile *e;
        if (off + 14 > size || seen[off / 4])
            continue;
        seen[off / 4] = 1;
        left = table[off] | table[off + 1] << 8;
        right = table[off + 2] | table[off + 3] << 8;
        if (left == 0xFFFF && right == 0xFFFF)
            continue;
        nlen = table[off + 13];
        if (off + 14 + nlen > size)
            continue;
        if (iso->n == iso->cap) {
            IsoFile *grown = (IsoFile *)realloc(iso->files, (iso->cap ? iso->cap * 2 : 256) * sizeof *grown);
            if (!grown)
                break;
            iso->files = grown;
            iso->cap = iso->cap ? iso->cap * 2 : 256;
        }
        e = &iso->files[iso->n++];
        memcpy(&e->sector, table + off + 4, 4);
        memcpy(&e->size, table + off + 8, 4);
        e->dir = (table[off + 12] & 0x10) != 0;
        {
            WCHAR name[256];
            for (unsigned i = 0; i < nlen; i++)
                name[i] = (WCHAR)table[off + 14 + i];
            name[nlen] = 0;
            swprintf(e->path, 260, L"%s%s", prefix, name);
        }
        if (!e->dir)
            iso->total += e->size;
        if (left && sp < 512)
            stack[sp++] = left * 4;
        if (right && sp < 512)
            stack[sp++] = right * 4;
    }
    free(seen);
    free(table);
    /* recurse into the directories found at this level */
    {
        unsigned count = iso->n;
        for (unsigned i = 0; i < count; i++) {
            IsoFile d = iso->files[i];
            size_t plen = wcslen(prefix);
            if (d.dir && wcsncmp(d.path, prefix, plen) == 0 && !wcschr(d.path + plen, L'\\')) {
                WCHAR sub[262];
                swprintf(sub, 262, L"%s\\", d.path);
                iso_walk(iso, d.sector, d.size, sub, depth + 1);
            }
        }
    }
}

int iso_open(Iso *iso, const WCHAR *path)
{
    static const char MAGIC[] = "MICROSOFT*XBOX*MEDIA";
    memset(iso, 0, sizeof *iso);
    iso->f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (iso->f == INVALID_HANDLE_VALUE)
        return 0;
    for (unsigned i = 0; i < sizeof PARTITIONS / sizeof PARTITIONS[0]; i++) {
        BYTE hdr[28];
        if (iso_read(iso, PARTITIONS[i] + 32 * ISO_SECTOR, hdr, sizeof hdr) && !memcmp(hdr, MAGIC, 20)) {
            DWORD root, rsize;
            iso->base = PARTITIONS[i];
            memcpy(&root, hdr + 20, 4);
            memcpy(&rsize, hdr + 24, 4);
            iso_walk(iso, root, rsize, L"", 0);
            return iso->n > 0;
        }
    }
    return 0;
}

void iso_close(Iso *iso)
{
    if (iso->f && iso->f != INVALID_HANDLE_VALUE)
        CloseHandle(iso->f);
    free(iso->files);
    memset(iso, 0, sizeof *iso);
}

const IsoFile *iso_find(const Iso *iso, const WCHAR *path)
{
    for (unsigned i = 0; i < iso->n; i++)
        if (!iso->files[i].dir && !_wcsicmp(iso->files[i].path, path))
            return &iso->files[i];
    return NULL;
}

