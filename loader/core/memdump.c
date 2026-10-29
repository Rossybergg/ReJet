/* REJET_MEMDUMP=1 (debugging): each F10 marker writes the game's writable memory to
 * memdump\mark<N>.bin: the XBE image and every block the game allocated through the kernel
 * (virtual memory, contiguous memory, pool). tools/memdiff.py compares the marks to find a
 * value, such as the spray can count, from how it changed between them. Records: address (4
 * bytes), length (4), then the bytes. The dumps hold game data: they stay on this machine. */
#include <stdio.h>
#include <stdlib.h>

#include "kernel.h"
#include "memdump.h"

#define MAX_BLOCKS 16384
#define XBE_BASE 0x10000u
#define XBE_SIZE 0x27EEE0u

static struct { UINT_PTR at; SIZE_T len; } g_blocks[MAX_BLOCKS];
static LONG g_count;
static int g_on = -1;

static int enabled(void)
{
    if (g_on < 0)
        g_on = GetEnvironmentVariableA("REJET_MEMDUMP", NULL, 0) > 0;
    return g_on;
}

void memdump_note(const void *p, SIZE_T len)
{
    LONG i;
    if (!p || !len || !enabled())
        return;
    i = InterlockedIncrement(&g_count) - 1;
    if (i < MAX_BLOCKS) {
        g_blocks[i].at = (UINT_PTR)p;
        g_blocks[i].len = len;
    }
}

/* Write [at, at + len) page by page, skipping pages that are no longer committed or readable. */
static void dump_range(FILE *f, UINT_PTR at, SIZE_T len)
{
    UINT_PTR end = at + len;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mi;
        UINT_PTR stop;
        if (!VirtualQuery((const void *)at, &mi, sizeof mi))
            return;
        stop = (UINT_PTR)mi.BaseAddress + mi.RegionSize;
        if (stop > end)
            stop = end;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (mi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            DWORD a = (DWORD)at, n = (DWORD)(stop - at);
            fwrite(&a, 4, 1, f);
            fwrite(&n, 4, 1, f);
            fwrite((const void *)at, 1, n, f);
        }
        at = stop;
    }
}

void memdump_mark(long mark)
{
    char path[64];
    FILE *f;
    LONG n;
    if (!enabled())
        return;
    CreateDirectoryA("memdump", NULL);
    snprintf(path, sizeof path, "memdump\\mark%ld.bin", mark);
    if (!(f = fopen(path, "wb")))
        return;
    dump_range(f, XBE_BASE, XBE_SIZE);
    n = g_count < MAX_BLOCKS ? g_count : MAX_BLOCKS;
    for (LONG i = 0; i < n; i++)
        dump_range(f, g_blocks[i].at, g_blocks[i].len);
    fclose(f);
    log_printf("memdump: %s (%ld blocks)\n", path, (long)n);
}
