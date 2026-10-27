/* Ob handles and Nt synchronisation objects. Xbox handles for events, semaphores, mutants
 * and threads are host Windows handles. Object names are logged but not shared yet. */
#include "kernel.h"
#include "xboxkrnl_impl.h"

static void note_name(const char *what, PXOBJECT_ATTRIBUTES oa)
{
    if (oa && oa->ObjectName && oa->ObjectName->Buffer)
        KTRACE("%s: named object '%.*s' created unnamed\n", what, oa->ObjectName->Length, oa->ObjectName->Buffer);
}

NTSTATUS XBAPI xbox_NtClose(HANDLE Handle)
{
    io_forget_handle(Handle);
    return CloseHandle(Handle) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

NTSTATUS XBAPI xbox_NtDuplicateObject(HANDLE SourceHandle, PHANDLE TargetHandle, DWORD Options)
{
    HANDLE p = GetCurrentProcess();
    return DuplicateHandle(p, SourceHandle, p, TargetHandle, 0, FALSE, Options | DUPLICATE_SAME_ACCESS)
               ? STATUS_SUCCESS : nt_last_error_status();
}

NTSTATUS XBAPI xbox_NtCreateEvent(PHANDLE EventHandle, PXOBJECT_ATTRIBUTES ObjectAttributes, ULONG EventType,
                                  BOOLEAN InitialState)
{
    /* EventType: 0 = NotificationEvent (manual reset), 1 = SynchronizationEvent (auto reset). */
    note_name("NtCreateEvent", ObjectAttributes);
    *EventHandle = CreateEventW(NULL, EventType == 0, InitialState != 0, NULL);
    return *EventHandle ? STATUS_SUCCESS : nt_last_error_status();
}

NTSTATUS XBAPI xbox_NtSetEvent(HANDLE EventHandle, PLONG PreviousState)
{
    if (PreviousState)
        *PreviousState = WaitForSingleObject(EventHandle, 0) == WAIT_OBJECT_0; /* approximate */
    return SetEvent(EventHandle) ? STATUS_SUCCESS : nt_last_error_status();
}

NTSTATUS XBAPI xbox_NtClearEvent(HANDLE EventHandle)
{
    return ResetEvent(EventHandle) ? STATUS_SUCCESS : nt_last_error_status();
}

NTSTATUS XBAPI xbox_NtPulseEvent(HANDLE EventHandle, PLONG PreviousState)
{
    if (PreviousState)
        *PreviousState = 0;
    return PulseEvent(EventHandle) ? STATUS_SUCCESS : nt_last_error_status();
}

NTSTATUS XBAPI xbox_NtCreateSemaphore(PHANDLE SemaphoreHandle, PXOBJECT_ATTRIBUTES ObjectAttributes,
                                      LONG InitialCount, LONG MaximumCount)
{
    note_name("NtCreateSemaphore", ObjectAttributes);
    *SemaphoreHandle = CreateSemaphoreW(NULL, InitialCount, MaximumCount, NULL);
    return *SemaphoreHandle ? STATUS_SUCCESS : nt_last_error_status();
}

NTSTATUS XBAPI xbox_NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount, PLONG PreviousCount)
{
    return ReleaseSemaphore(SemaphoreHandle, ReleaseCount, PreviousCount) ? STATUS_SUCCESS
                                                                           : nt_last_error_status();
}

NTSTATUS XBAPI xbox_NtCreateMutant(PHANDLE MutantHandle, PXOBJECT_ATTRIBUTES ObjectAttributes, BOOLEAN InitialOwner)
{
    note_name("NtCreateMutant", ObjectAttributes);
    *MutantHandle = CreateMutexW(NULL, InitialOwner != 0, NULL);
    return *MutantHandle ? STATUS_SUCCESS : nt_last_error_status();
}

NTSTATUS XBAPI xbox_NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount)
{
    if (PreviousCount)
        *PreviousCount = 0;
    return ReleaseMutex(MutantHandle) ? STATUS_SUCCESS : nt_last_error_status();
}

NTSTATUS XBAPI xbox_NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout)
{
    return nt_wait_status(WaitForSingleObjectEx(Handle, nt_timeout_ms(Timeout), Alertable), 1);
}

NTSTATUS XBAPI xbox_NtWaitForSingleObjectEx(HANDLE Handle, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                            PLARGE_INTEGER Timeout)
{
    (void)WaitMode;
    return nt_wait_status(WaitForSingleObjectEx(Handle, nt_timeout_ms(Timeout), Alertable), 1);
}

NTSTATUS XBAPI xbox_NtWaitForMultipleObjectsEx(ULONG Count, HANDLE *Handles, ULONG WaitType,
                                               KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                               PLARGE_INTEGER Timeout)
{
    (void)WaitMode;
    /* WaitType: 0 = WaitAll, 1 = WaitAny. */
    return nt_wait_status(WaitForMultipleObjectsEx(Count, Handles, WaitType == 0, nt_timeout_ms(Timeout),
                                                   Alertable), Count);
}

NTSTATUS XBAPI xbox_NtSignalAndWaitForSingleObjectEx(HANDLE SignalHandle, HANDLE WaitHandle,
                                                     KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                                     PLARGE_INTEGER Timeout)
{
    (void)WaitMode;
    return nt_wait_status(SignalObjectAndWait(SignalHandle, WaitHandle, nt_timeout_ms(Timeout), Alertable), 1);
}

NTSTATUS XBAPI xbox_NtYieldExecution(void)
{
    SwitchToThread();
    return STATUS_SUCCESS;
}

NTSTATUS XBAPI xbox_KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval)
{
    (void)WaitMode;
    return SleepEx(nt_timeout_ms(Interval), Alertable) == WAIT_IO_COMPLETION ? (NTSTATUS)0x000000C0L
                                                                              : STATUS_SUCCESS;
}

NTSTATUS XBAPI xbox_NtResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)
{
    DWORD r = ResumeThread(ThreadHandle);
    if (r == (DWORD)-1)
        return nt_last_error_status();
    if (PreviousSuspendCount)
        *PreviousSuspendCount = r;
    return STATUS_SUCCESS;
}

NTSTATUS XBAPI xbox_NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount)
{
    DWORD r = SuspendThread(ThreadHandle);
    if (r == (DWORD)-1)
        return nt_last_error_status();
    if (PreviousSuspendCount)
        *PreviousSuspendCount = r;
    return STATUS_SUCCESS;
}
