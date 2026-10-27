/* Ke DPCs and timers. These kernel objects live in game memory. One "DPC thread" (an Xbox
 * thread running at DISPATCH_LEVEL) owns the active-timer list and runs queued DPCs, the way
 * the kernel's DPC processing does; DPC routines are Xbox code and may read fs: fields, so
 * they never run on host pool threads. */
#include <string.h>

#include "emu_thread.h"
#include "kernel.h"
#include "xboxkrnl_impl.h"

#define MAX_TIMERS 256
#define MAX_DPCS   256

typedef struct ActiveTimer {
    PXKTIMER timer;
    ULONGLONG due_ms;
    LONG period_ms;
} ActiveTimer;

static CRITICAL_SECTION g_lock;
static HANDLE g_wake;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static ActiveTimer g_timers[MAX_TIMERS];
static unsigned g_timer_count;
static PXKDPC g_queue[MAX_DPCS];
static unsigned g_queue_count;

static ULONGLONG now_ms(void)
{
    return GetTickCount64();
}

/* NT due time: negative = relative 100 ns, positive = absolute system time. */
static ULONGLONG due_to_ms(LARGE_INTEGER due)
{
    return now_ms() + nt_timeout_ms(&due);
}

static void queue_dpc_locked(PXKDPC dpc, PVOID arg1, PVOID arg2)
{
    if (dpc->Inserted || g_queue_count == MAX_DPCS)
        return;
    dpc->Inserted = 1;
    dpc->SystemArgument1 = arg1;
    dpc->SystemArgument2 = arg2;
    g_queue[g_queue_count++] = dpc;
}

static DWORD WINAPI dpc_thread(void *unused)
{
    (void)unused;
    if (!emu_thread_attach(16))
        return 1;
    emu_irql_set(2); /* DISPATCH_LEVEL */
    for (;;) {
        DWORD wait = INFINITE;
        ULONGLONG now = now_ms();

        EnterCriticalSection(&g_lock);
        for (unsigned i = 0; i < g_timer_count;) {
            ActiveTimer *t = &g_timers[i];
            if (t->due_ms <= now) {
                t->timer->Header.SignalState = 1;
                if (t->timer->Dpc)
                    queue_dpc_locked(t->timer->Dpc, NULL, NULL);
                if (t->period_ms > 0) {
                    t->due_ms += (ULONGLONG)t->period_ms;
                }
                else {
                    t->timer->Header.Inserted = 0;
                    g_timers[i] = g_timers[--g_timer_count];
                    continue;
                }
            }
            if (t->due_ms - now < wait)
                wait = (DWORD)(t->due_ms - now);
            i++;
        }
        while (g_queue_count) {
            PXKDPC dpc = g_queue[0];
            memmove(g_queue, g_queue + 1, --g_queue_count * sizeof g_queue[0]);
            dpc->Inserted = 0;
            LeaveCriticalSection(&g_lock);
            dpc->DeferredRoutine(dpc, dpc->DeferredContext, dpc->SystemArgument1, dpc->SystemArgument2);
            EnterCriticalSection(&g_lock);
        }
        LeaveCriticalSection(&g_lock);
        WaitForSingleObject(g_wake, wait);
    }
}

static BOOL CALLBACK start_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&g_lock);
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 1 << 20, dpc_thread, NULL, 0, NULL));
    return TRUE;
}

static void ensure_started(void)
{
    InitOnceExecuteOnce(&g_once, start_once, NULL, NULL);
}

VOID XBAPI xbox_KeInitializeDpc(PXKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext)
{
    memset(Dpc, 0, sizeof *Dpc);
    Dpc->Type = XDPC_OBJECT;
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
}

BOOLEAN XBAPI xbox_KeInsertQueueDpc(PXKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2)
{
    BOOLEAN queued;
    ensure_started();
    EnterCriticalSection(&g_lock);
    queued = !Dpc->Inserted;
    queue_dpc_locked(Dpc, SystemArgument1, SystemArgument2);
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
    return queued;
}

BOOLEAN XBAPI xbox_KeRemoveQueueDpc(PXKDPC Dpc)
{
    BOOLEAN removed = FALSE;
    ensure_started();
    EnterCriticalSection(&g_lock);
    for (unsigned i = 0; i < g_queue_count; i++) {
        if (g_queue[i] == Dpc) {
            memmove(g_queue + i, g_queue + i + 1, (--g_queue_count - i) * sizeof g_queue[0]);
            Dpc->Inserted = 0;
            removed = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return removed;
}

VOID XBAPI xbox_KeInitializeTimerEx(PXKTIMER Timer, ULONG Type)
{
    memset(Timer, 0, sizeof *Timer);
    Timer->Header.Type = (UCHAR)(XTIMER_NOTIFICATION_OBJECT + Type); /* 0 = notification, 1 = sync */
    Timer->Header.Size = sizeof *Timer / sizeof(LONG);
    Timer->Header.WaitListHead.Flink = Timer->Header.WaitListHead.Blink = &Timer->Header.WaitListHead;
}

static BOOLEAN cancel_locked(PXKTIMER Timer)
{
    for (unsigned i = 0; i < g_timer_count; i++) {
        if (g_timers[i].timer == Timer) {
            g_timers[i] = g_timers[--g_timer_count];
            Timer->Header.Inserted = 0;
            return TRUE;
        }
    }
    return FALSE;
}

BOOLEAN XBAPI xbox_KeSetTimerEx(PXKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PXKDPC Dpc)
{
    BOOLEAN was_set;
    ensure_started();
    EnterCriticalSection(&g_lock);
    was_set = cancel_locked(Timer);
    if (g_timer_count < MAX_TIMERS) {
        Timer->Header.SignalState = 0;
        Timer->Header.Inserted = 1;
        Timer->Dpc = Dpc;
        Timer->Period = Period;
        g_timers[g_timer_count].timer = Timer;
        g_timers[g_timer_count].due_ms = due_to_ms(DueTime);
        g_timers[g_timer_count].period_ms = Period;
        g_timer_count++;
    }
    else {
        log_printf("error: too many active kernel timers\n");
    }
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
    return was_set;
}

BOOLEAN XBAPI xbox_KeSetTimer(PXKTIMER Timer, LARGE_INTEGER DueTime, PXKDPC Dpc)
{
    return xbox_KeSetTimerEx(Timer, DueTime, 0, Dpc);
}

BOOLEAN XBAPI xbox_KeCancelTimer(PXKTIMER Timer)
{
    BOOLEAN was_set;
    ensure_started();
    EnterCriticalSection(&g_lock);
    was_set = cancel_locked(Timer);
    LeaveCriticalSection(&g_lock);
    return was_set;
}
