/* Ps: thread creation. Each Xbox thread is a Windows thread with an attached EmuThread. */
#include <stdlib.h>

#include "emu_thread.h"
#include "kernel.h"
#include "xboxkrnl_impl.h"

/* Windows stacks also carry our HLE code, so never go below this. */
#define MIN_HOST_STACK (1u << 20)

typedef struct ThreadStart {
    PKSTART_ROUTINE start;
    PVOID context;
    PKSYSTEM_ROUTINE system;
    ULONG tls_size;
} ThreadStart;

static DWORD WINAPI xbox_thread_main(void *param)
{
    ThreadStart s = *(ThreadStart *)param;
    free(param);
    if (!emu_thread_attach(s.tls_size)) {
        log_printf("error: thread %lu: cannot attach Xbox state\n", GetCurrentThreadId());
        return 1;
    }
    /* Kernel behaviour: SystemRoutine (XAPI's XapiThreadStartup) wraps the start routine. */
    if (s.system)
        s.system(s.start, s.context);
    else
        s.start(s.context);
    xbox_PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS XBAPI xbox_PsCreateSystemThreadEx(PHANDLE ThreadHandle, ULONG ThreadExtensionSize,
                                           ULONG KernelStackSize, ULONG TlsDataSize, PHANDLE ThreadId,
                                           PKSTART_ROUTINE StartRoutine, PVOID StartContext,
                                           BOOLEAN CreateSuspended, BOOLEAN DebuggerThread,
                                           PKSYSTEM_ROUTINE SystemRoutine)
{
    ThreadStart *s = (ThreadStart *)malloc(sizeof *s);
    DWORD id = 0;
    HANDLE h;
    SIZE_T stack = KernelStackSize > MIN_HOST_STACK ? KernelStackSize : MIN_HOST_STACK;

    (void)ThreadExtensionSize;
    (void)DebuggerThread;
    if (!s)
        return STATUS_NO_MEMORY;
    s->start = StartRoutine;
    s->context = StartContext;
    s->system = SystemRoutine;
    s->tls_size = TlsDataSize;
    h = CreateThread(NULL, stack, xbox_thread_main, s, CreateSuspended ? CREATE_SUSPENDED : 0, &id);
    KTRACE("PsCreateSystemThreadEx(start %08X, ctx %08X, system %08X, tls %lu, stack %lu%s) -> thread %lu\n",
           (unsigned)(UINT_PTR)StartRoutine, (unsigned)(UINT_PTR)StartContext,
           (unsigned)(UINT_PTR)SystemRoutine, TlsDataSize, KernelStackSize,
           CreateSuspended ? ", suspended" : "", id);
    if (!h) {
        free(s);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (ThreadHandle)
        *ThreadHandle = h;
    else
        CloseHandle(h);
    if (ThreadId)
        *ThreadId = (HANDLE)(UINT_PTR)id;
    return STATUS_SUCCESS;
}

__declspec(noreturn) VOID XBAPI xbox_PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    KTRACE("PsTerminateSystemThread(%08lX) thread %lu\n", ExitStatus, GetCurrentThreadId());
    ExitThread((DWORD)ExitStatus);
}
