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
    HANDLE ready, go; /* start-up handshake (see xbox_PsCreateSystemThreadEx) */
} ThreadStart;

static DWORD WINAPI xbox_thread_main(void *param)
{
    ThreadStart s = *(ThreadStart *)param;
    free(param);
    if (!emu_thread_attach(s.tls_size)) {
        log_printf("error: thread %lu: cannot attach Xbox state\n", GetCurrentThreadId());
        SetEvent(s.ready);
        return 1;
    }
    /* Windows has finished starting this thread (DLL thread-attach, under the loader lock); a
     * thread created suspended is suspended by its creator here, at a safe point. */
    SetEvent(s.ready);
    WaitForSingleObject(s.go, INFINITE);
    CloseHandle(s.go);
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
    /* Never create the host thread suspended. A new Windows thread first runs DLL thread-attach
     * code holding the loader lock; if the title suspends it again before its Xbox code starts
     * (CRI ADX's priority lock resumes and suspends a thread made suspended, 0x13BB50/0x13BB90),
     * it stays suspended holding that lock, and every thread start (XAudio2) and ExitProcess
     * waits for it. So the thread starts, reports ready from our own code, and is suspended there
     * before it may go on: the Xbox sees a thread created suspended (suspend count 1). */
    s->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    s->go = CreateEventW(NULL, TRUE, FALSE, NULL);
    h = CreateThread(NULL, stack, xbox_thread_main, s, 0, &id);
    KTRACE("PsCreateSystemThreadEx(start %08X, ctx %08X, system %08X, tls %lu, stack %lu%s) -> thread %lu\n",
           (unsigned)(UINT_PTR)StartRoutine, (unsigned)(UINT_PTR)StartContext,
           (unsigned)(UINT_PTR)SystemRoutine, TlsDataSize, KernelStackSize,
           CreateSuspended ? ", suspended" : "", id);
    if (!h) {
        CloseHandle(s->ready);
        CloseHandle(s->go);
        free(s);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    {
        HANDLE ready = s->ready, go = s->go; /* s belongs to the thread now */
        WaitForSingleObject(ready, INFINITE);
        CloseHandle(ready);
        if (CreateSuspended)
            SuspendThread(h); /* parked in xbox_thread_main, waiting for `go` */
        SetEvent(go);
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
