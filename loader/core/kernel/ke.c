/* Ke: time, IRQL, bug check. IRQL lives in the TLS slot that patched fs:[0x24] reads. */
#include <intrin.h>
#include <string.h>

#include "emu_thread.h"
#include "kernel.h"
#include "xboxkrnl_impl.h"

ULONGLONG XBAPI xbox_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (ULONGLONG)c.QuadPart;
}

ULONGLONG XBAPI xbox_KeQueryPerformanceFrequency(void)
{
    /* Host frequency: callers use counter/frequency ratios, so the pair stays consistent. */
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (ULONGLONG)f.QuadPart;
}

VOID XBAPI xbox_KeQuerySystemTime(PLARGE_INTEGER CurrentTime)
{
    FILETIME ft; /* NT system time and FILETIME share the 1601 epoch and 100 ns unit */
    GetSystemTimeAsFileTime(&ft);
    CurrentTime->LowPart = ft.dwLowDateTime;
    CurrentTime->HighPart = (LONG)ft.dwHighDateTime;
}

ULONGLONG XBAPI xbox_KeQueryInterruptTime(void)
{
    /* 100 ns units since an arbitrary start, like the kernel's interrupt time. */
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (ULONGLONG)(c.QuadPart / f.QuadPart) * 10000000ull +
           (ULONGLONG)(c.QuadPart % f.QuadPart) * 10000000ull / (ULONGLONG)f.QuadPart;
}

VOID XBAPI xbox_KeStallExecutionProcessor(ULONG MicroSeconds)
{
    LARGE_INTEGER f, start, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&start);
    do
        QueryPerformanceCounter(&now);
    while ((now.QuadPart - start.QuadPart) * 1000000 / f.QuadPart < MicroSeconds);
}

KIRQL FASTCALL xbox_KfRaiseIrql(KIRQL NewIrql)
{
    KIRQL old = emu_irql_get();
    emu_irql_set(NewIrql);
    return old;
}

VOID FASTCALL xbox_KfLowerIrql(KIRQL NewIrql)
{
    emu_irql_set(NewIrql);
}

KIRQL XBAPI xbox_KeRaiseIrqlToDpcLevel(void)
{
    return xbox_KfRaiseIrql(2 /* DISPATCH_LEVEL */);
}

__declspec(noreturn) VOID XBAPI xbox_KeBugCheck(ULONG BugCheckCode)
{
    log_printf("KeBugCheck(%08lX) from %08X, thread %lu, irql %u\n", BugCheckCode,
               (unsigned)(UINT_PTR)_ReturnAddress(), GetCurrentThreadId(), emu_irql_get());
    log_backtrace(_AddressOfReturnAddress(), 16);
    ExitProcess(5);
}

/* KeTickCount is a kernel variable the game reads directly through its thunk slot. */
static DWORD WINAPI ticker(void *tick_count)
{
    volatile ULONG *t = (volatile ULONG *)tick_count;
    ULONGLONG start = GetTickCount64();
    for (;;) {
        *t = (ULONG)(GetTickCount64() - start);
        Sleep(1);
    }
}

void kernel_ticker_start(void)
{
    for (unsigned i = 0; i < g_kernel_import_count; i++) {
        const KernelImport *k = &g_kernel_imports[i];
        if (k->is_data && !strcmp(k->name, "KeTickCount"))
            CloseHandle(CreateThread(NULL, 0, ticker, k->target, 0, NULL));
        if (k->is_data && !strcmp(k->name, "HalDiskCachePartitionCount"))
            *(ULONG *)k->target = 3; /* retail HDD: cache partitions 3, 4, 5 */
        if (k->is_data && !strcmp(k->name, "KeTimeIncrement"))
            *(ULONG *)k->target = 10000; /* 1 ms in 100 ns units */
        if (k->is_data && !strcmp(k->name, "XboxKrnlVersion")) {
            /* 1.0.5838.1, the last retail kernel. Never report 3944/4034/4039/4817: XAPI
             * patches kernel memory at 0x8003xxxx for those builds (FUN_001e3551). */
            USHORT *v = (USHORT *)k->target;
            v[0] = 1;
            v[1] = 0;
            v[2] = 5838;
            v[3] = 1;
        }
    }
}
