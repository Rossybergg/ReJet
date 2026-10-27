/* Helpers shared by the kernel HLE modules. */
#include <stdarg.h>
#include <stdio.h>

#include "kernel.h"
#include "xboxkrnl_impl.h"

void ktrace(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_printf("  k [%5lu] %s", GetCurrentThreadId(), buf);
}

void ktrace_call(const char *name, void *caller, int has_result, unsigned long long result)
{
    if (has_result)
        ktrace("%s -> %08llX (from %08X)\n", name, result, (unsigned)(UINT_PTR)caller);
    else
        ktrace("%s (from %08X)\n", name, (unsigned)(UINT_PTR)caller);
}

DWORD nt_timeout_ms(const LARGE_INTEGER *timeout)
{
    LONGLONG t;
    if (!timeout)
        return INFINITE;
    t = timeout->QuadPart;
    if (t < 0) /* relative, 100 ns units */
        return (DWORD)((-t + 9999) / 10000);
    if (t == 0)
        return 0;
    { /* absolute system time */
        FILETIME ft;
        ULARGE_INTEGER now;
        GetSystemTimeAsFileTime(&ft);
        now.LowPart = ft.dwLowDateTime;
        now.HighPart = ft.dwHighDateTime;
        return t <= (LONGLONG)now.QuadPart ? 0 : (DWORD)((t - (LONGLONG)now.QuadPart + 9999) / 10000);
    }
}

NTSTATUS nt_wait_status(DWORD r, DWORD count)
{
    if (r < WAIT_OBJECT_0 + count)
        return (NTSTATUS)(r - WAIT_OBJECT_0);             /* STATUS_WAIT_0 + i */
    if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + count)
        return (NTSTATUS)(0x80 + r - WAIT_ABANDONED_0);   /* STATUS_ABANDONED_WAIT_0 + i */
    if (r == WAIT_TIMEOUT)
        return (NTSTATUS)0x00000102L;                     /* STATUS_TIMEOUT */
    if (r == WAIT_IO_COMPLETION)
        return (NTSTATUS)0x000000C0L;                     /* STATUS_USER_APC */
    return STATUS_INVALID_HANDLE;
}

NTSTATUS nt_last_error_status(void)
{
    switch (GetLastError()) {
    case ERROR_INVALID_HANDLE: return STATUS_INVALID_HANDLE;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return STATUS_NO_MEMORY;
    case ERROR_INVALID_PARAMETER: return STATUS_INVALID_PARAMETER_X;
    case ERROR_ACCESS_DENIED: return STATUS_ACCESS_VIOLATION;
    default: return STATUS_UNSUCCESSFUL;
    }
}
