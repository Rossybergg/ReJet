/* Frame profiler (see perf.h). */
#include <stdlib.h>

#include "kernel.h"
#include "perf.h"

volatile LONGLONG g_perf_ticks[PERF_COUNT];
volatile LONG g_perf_calls[PERF_COUNT];

static const char *const NAMES[PERF_COUNT] = {"tex", "texup", "vsc", "psc", "draw", "present", "io", "log"};

static LONGLONG last;

void perf_wait_done(void)
{
    last = perf_now();
}

void perf_frame(unsigned long frame)
{
    static LONGLONG freq;
    static long threshold_ms = -1;
    LONGLONG now = perf_now();
    LONGLONG ticks[PERF_COUNT];
    LONG calls[PERF_COUNT];

    if (threshold_ms < 0) {
        char env[16] = "";
        LARGE_INTEGER f;
        threshold_ms = GetEnvironmentVariableA("REJET_PERF", env, sizeof env) ? atol(env) : 40;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
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
}
