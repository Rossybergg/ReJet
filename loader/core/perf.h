/* Frame profiler: time spent in the shim's hot paths, logged for slow frames (see perf.c). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum {
    PERF_TEX,     /* tex_get: lookup + change check */
    PERF_TEXUP,   /* texture (re)uploads */
    PERF_VSC,     /* vertex program translate + compile */
    PERF_PSC,     /* pixel shader translate + compile */
    PERF_DRAW,    /* host draw calls */
    PERF_PRESENT, /* EndScene + Present */
    PERF_IO,      /* NtCreateFile / NtOpenFile */
    PERF_LOG,     /* log_printf */
    PERF_LATE,    /* vblank wait: woken after the boundary (part of wait, not work) */
    PERF_VBCB,    /* the game's vblank callback, run inside the wait */
    PERF_COUNT
};

extern volatile LONGLONG g_perf_ticks[PERF_COUNT];
extern volatile LONG g_perf_calls[PERF_COUNT];

static __inline LONGLONG perf_now(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

static __inline void perf_end(int id, LONGLONG start)
{
    InterlockedExchangeAdd64(&g_perf_ticks[id], perf_now() - start);
    InterlockedIncrement(&g_perf_calls[id]);
}

/* Call once per Swap, before waiting for vblank. Logs frames whose work took longer than
 * REJET_PERF milliseconds (default 40; 0 = off). */
void perf_frame(unsigned long frame, unsigned long draws);
/* F10 marker number, written into the next CSV row (REJET_PERFLOG). */
void perf_mark(long n);
/* REJET_PROFILE=1: start sampling the calling thread (profile.c). */
void profile_start(void);
/* Call after the vblank wait: the next frame's work starts here. */
void perf_wait_done(void);
