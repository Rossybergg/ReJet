/* REJET_PROFILE=1: a sampling profiler for the game thread. A host thread samples the thread
 * that presents (the game's main thread) about every millisecond while it works on a frame (not
 * while it waits for the vblank), and every 30 s logs where its
 * time went: ReJet functions by name (from rejet.pdb, when it is next to the dll), other
 * modules (d3d9, the graphics driver, ntdll) by module, game code by address. For finding
 * frame-time costs; off by default. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "perf.h"
#include <dbghelp.h>
#include <psapi.h>
#include <timeapi.h>

#define SLOTS 65536
static struct { DWORD eip, n; } g_hits[SLOTS];
static LONG g_samples;
static HANDLE g_target;
extern volatile LONG g_perf_working;

static void hit(DWORD eip)
{
    DWORD k = eip & ~0xFu;
    for (DWORD i = (k * 2654435761u) >> 16;; i = (i + 1) & (SLOTS - 1)) {
        if (g_hits[i].eip == k || !g_hits[i].eip) {
            g_hits[i].eip = k;
            g_hits[i].n++;
            return;
        }
    }
}

typedef struct Bucket { char name[96]; DWORD n; } Bucket;

static int by_count(const void *a, const void *b)
{
    return (int)((const Bucket *)b)->n - (int)((const Bucket *)a)->n;
}

static void report(void)
{
    static Bucket b[4096];
    static int sym_ready;
    int nb = 0;
    HANDLE proc = GetCurrentProcess();
    if (!sym_ready) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        sym_ready = SymInitialize(proc, NULL, TRUE) ? 1 : -1;
    }
    for (int i = 0; i < SLOTS; i++) {
        char name[96], mod[MAX_PATH] = "";
        HMODULE hm = NULL;
        int j;
        if (!g_hits[i].eip)
            continue;
        if (g_hits[i].eip >= 0x11000 && g_hits[i].eip < 0x27F000)
            snprintf(name, sizeof name, "game %05X", g_hits[i].eip & ~0xFFu); /* 256-byte buckets */
        else {
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(UINT_PTR)g_hits[i].eip, &hm);
            if (hm)
                GetModuleBaseNameA(proc, hm, mod, sizeof mod);
            snprintf(name, sizeof name, "%s", hm ? mod : "?");
            if (hm && hm == GetModuleHandleA("rejet.dll") && sym_ready > 0) {
                char buf[sizeof(SYMBOL_INFO) + 64];
                SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
                DWORD64 disp = 0;
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 63;
                if (SymFromAddr(proc, g_hits[i].eip, &disp, si))
                    snprintf(name, sizeof name, "rejet!%s", si->Name);
            }
        }
        for (j = 0; j < nb && strcmp(b[j].name, name); j++)
            ;
        if (j == nb) {
            if (nb == 4096)
                continue;
            strcpy(b[nb].name, name);
            b[nb++].n = 0;
        }
        b[j].n += g_hits[i].n;
    }
    qsort(b, nb, sizeof b[0], by_count);
    log_printf("profile: %ld samples of the game thread\n", g_samples);
    for (int j = 0; j < nb && j < 40; j++)
        log_printf("profile: %5.1f%%  %s\n", 100.0 * b[j].n / (g_samples ? g_samples : 1), b[j].name);
}

static DWORD WINAPI sampler(void *arg)
{
    ULONGLONG last = GetTickCount64();
    (void)arg;
    timeBeginPeriod(1);
    for (;;) {
        CONTEXT c;
        Sleep(1);
        if (!g_perf_working) /* waiting for the vblank: not frame cost */
            continue;
        if (SuspendThread(g_target) == (DWORD)-1)
            return 0;
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(g_target, &c)) {
            hit(c.Eip);
            g_samples++;
        }
        ResumeThread(g_target);
        if (GetTickCount64() - last > 30000) {
            report();
            last = GetTickCount64();
        }
    }
}

void profile_start(void)
{
    static LONG started;
    if (InterlockedExchange(&started, 1) || !GetEnvironmentVariableA("REJET_PROFILE", NULL, 0))
        return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    CloseHandle(CreateThread(NULL, 0, sampler, NULL, 0, NULL));
    log_printf("profile: sampling the game thread\n");
}
