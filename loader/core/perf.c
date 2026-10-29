/* Frame profiler (see perf.h). */
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>

#include "kernel.h"
#include "perf.h"
#include <tlhelp32.h>
#include "settings.h"
#include "ui/menu.h"
#include "version.gen.h"

volatile LONGLONG g_perf_ticks[PERF_COUNT];
volatile LONG g_perf_calls[PERF_COUNT];

static const char *const NAMES[PERF_COUNT] = {"tex", "texup", "vsc", "psc", "draw", "present", "io", "log", "late", "vbcb"};

static LONGLONG last, last_swap, freq;
static long pending_mark;

/* REJET_PERFLOG=1: one CSV row per frame in logs\perf_<date and time>.csv, for play sessions
 * (tools/perf_report.py summarises it). Rows are buffered; the buffer is flushed every few
 * seconds so a crash loses little. */
static FILE *g_csv;

static void csv_open(void)
{
    char env[8] = "", dir[MAX_PATH], path[MAX_PATH];
    static char buf[1 << 18];
    SYSTEMTIME t;
    DEVMODEW dm = {.dmSize = sizeof dm};
    if (!GetEnvironmentVariableA("REJET_PERFLOG", env, sizeof env) || atoi(env) == 0)
        return;
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    *strrchr(dir, '\\') = 0;
    strncat(dir, "\\logs", sizeof dir - strlen(dir) - 1);
    CreateDirectoryA(dir, NULL);
    GetLocalTime(&t);
    snprintf(path, sizeof path, "%s\\perf_%04u%02u%02u_%02u%02u%02u.csv", dir, t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
    g_csv = fopen(path, "w");
    if (!g_csv) {
        log_printf("perf: cannot write %s\n", path);
        return;
    }
    setvbuf(g_csv, buf, _IOFBF, sizeof buf);
    EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm);
    fprintf(g_csv, "# ReJet %s, render %dx%d, aspect %d, %s, vsync %d, anisotropy %d, display %lux%lu at %lu Hz\n",
            REJET_BUILD, g_settings.width, g_settings.height, g_settings.aspect,
            g_settings.fullscreen ? "full screen" : "window", g_settings.vsync, g_settings.anisotropy,
            dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
    /* interval: Swap to Swap; work: from the end of the vblank wait to this Swap (game + ReJet);
     * wait: the vblank wait before it; the timed ReJet paths are part of work, except present,
     * which is the previous frame's EndScene + Present */
    fprintf(g_csv, "frame,t_ms,interval_ms,work_ms,wait_ms");
    for (int i = 0; i < PERF_COUNT; i++)
        fprintf(g_csv, ",%s_ms,%s_n", NAMES[i], NAMES[i]);
    fprintf(g_csv, ",draws,menu,mark,probe_us\n");
    log_printf("perf: recording every frame to %s\n", path);
}

/* REJET_PERFLOG: which threads had the CPU in a slow frame (cycle counts per thread since the
 * last frame), logged for frames over two vblanks' worth of time. */
typedef struct ThreadCycles {
    DWORD tid;
    ULONG64 cycles;
    HANDLE h;
} ThreadCycles;
static ThreadCycles g_threads[256];
static int g_thread_count;

static void thread_list_refresh(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {.dwSize = sizeof te};
    DWORD pid = GetCurrentProcessId();
    if (snap == INVALID_HANDLE_VALUE)
        return;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        int known = 0;
        if (te.th32OwnerProcessID != pid)
            continue;
        for (int i = 0; i < g_thread_count && !known; i++)
            known = g_threads[i].tid == te.th32ThreadID;
        if (!known && g_thread_count < 256) {
            ThreadCycles *t = &g_threads[g_thread_count++];
            t->tid = te.th32ThreadID;
            t->h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, t->tid);
            if (t->h)
                QueryThreadCycleTime(t->h, &t->cycles);
        }
    }
    CloseHandle(snap);
}

/* The cycle counters are read every frame; the thread list at the start and again in frames
 * that are loading stalls anyway (over 100 ms): a snapshot lists every thread on the system
 * and costs milliseconds. */
static void thread_cycles(int report, int loading, unsigned long frame, double cycles_per_ms)
{
    struct { DWORD tid; double ms; } top[6] = {0};
    if (!g_thread_count || loading)
        thread_list_refresh();
    for (int n = 0; n < g_thread_count; n++) {
        ThreadCycles *t = &g_threads[n];
        ULONG64 c = 0;
        double ms;
        if (!t->h || !QueryThreadCycleTime(t->h, &c))
            continue;
        ms = (double)(c - t->cycles) / cycles_per_ms;
        t->cycles = c;
        for (int i = 0; i < 6; i++)
            if (ms > top[i].ms) {
                memmove(&top[i + 1], &top[i], (5 - i) * sizeof top[0]);
                top[i].tid = t->tid;
                top[i].ms = ms;
                break;
            }
    }
    if (report) {
        log_printf("perf: frame %lu CPU by thread:", frame);
        for (int i = 0; i < 6 && top[i].tid; i++)
            log_printf(" %lu %.1f ms", top[i].tid, top[i].ms);
        log_printf("\n");
    }
}

/* A fixed amount of work, timed: shows whether the core itself ran slower (clock, power state,
 * a busy hyperthread sibling) in a slow frame. */
static double probe_us(void)
{
    volatile unsigned x = 1;
    LONGLONG t0 = perf_now();
    for (int i = 0; i < 20000; i++)
        x = x * 1664525u + 1013904223u;
    return (double)(perf_now() - t0) * 1e6 / (double)freq;
}

volatile LONG g_perf_working; /* between the vblank wait and Swap: profile.c samples only then */

void perf_wait_done(void)
{
    g_perf_working = 1;
    last = perf_now();
}

void perf_mark(long n)
{
    pending_mark = n;
}

void perf_frame(unsigned long frame, unsigned long draws)
{
    static long threshold_ms = -1;
    static LONGLONG start, wait_start, last_flush;
    LONGLONG now = perf_now();
    LONGLONG ticks[PERF_COUNT];
    g_perf_working = 0;
    LONG calls[PERF_COUNT];

    if (threshold_ms < 0) {
        char env[16] = "";
        LARGE_INTEGER f;
        threshold_ms = GetEnvironmentVariableA("REJET_PERF", env, sizeof env) ? atol(env) : 40;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
        start = now;
        csv_open();
        profile_start();
    }
    for (int i = 0; i < PERF_COUNT; i++) {
        ticks[i] = InterlockedExchange64(&g_perf_ticks[i], 0);
        calls[i] = InterlockedExchange(&g_perf_calls[i], 0);
    }
    if (last && threshold_ms > 0 && (now - last) * 1000 >= threshold_ms * freq) {
        log_printf("perf: frame %lu took %.1f ms:", frame, (double)(now - last) * 1000.0 / (double)freq);
        for (int i = 0; i < PERF_COUNT; i++)
            if (calls[i])
                log_printf(" %s %.1f ms/%ld", NAMES[i], (double)ticks[i] * 1000.0 / (double)freq, calls[i]);
        log_printf("\n");
    }
    if (g_csv) { /* thread cycle counts are in TSC ticks: calibrate against the QPC */
        static LONGLONG q0;
        static ULONG64 c0;
        double cycles_per_ms = 0;
        if (!q0) {
            q0 = now;
            c0 = __rdtsc();
        }
        else if (now > q0)
            cycles_per_ms = (double)(__rdtsc() - c0) * (double)freq / 1000.0 / (double)(now - q0);
        if (cycles_per_ms > 0)
            thread_cycles(last_swap && (now - last_swap) * 1000 > 25 * freq,
                          last_swap && (now - last_swap) * 10 > freq, frame, cycles_per_ms);
    }
    if (g_csv && last_swap) {
        const double ms = 1000.0 / (double)freq;
        fprintf(g_csv, "%lu,%.2f,%.3f,%.3f,%.3f", frame, (double)(now - start) * ms, (double)(now - last_swap) * ms,
                (double)(now - last) * ms, (double)(last - wait_start) * ms);
        for (int i = 0; i < PERF_COUNT; i++)
            fprintf(g_csv, ",%.3f,%ld", (double)ticks[i] * ms, calls[i]);
        fprintf(g_csv, ",%lu,%d,%ld,%.1f\n", draws, menu_is_open(), pending_mark, probe_us());
        pending_mark = 0;
        if ((now - last_flush) > 5 * freq) {
            fflush(g_csv);
            last_flush = now;
        }
    }
    last_swap = wait_start = now;
}
