/* REJET_MEMDUMP=1 (debugging): the game's writable memory to memdump\ at each F10 marker (memdump.c). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* A block the game allocated (kernel/mm.c). */
void memdump_note(const void *p, SIZE_T len);
/* Write everything for marker number `mark`. */
void memdump_mark(long mark);
