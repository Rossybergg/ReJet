/* Rtl: critical sections, strings, memory helpers, SEH forwarding. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "xboxkrnl_impl.h"

/* Host CRITICAL_SECTION behind an Xbox one, created on first use, its pointer kept in the
 * first dword. Static Xbox critical sections may be entered without
 * RtlInitializeCriticalSection: Whacked!'s are zeroed, and JSRF's XAPI ones (e.g. the thread
 * notify lock at 0x234848) are pre-initialised with the kernel event header 0x00040001 (type 1,
 * size 4) there. malloc'd pointers are 8-byte aligned and that header is odd, so zero or an
 * unaligned value means "not yet". */
#define HOST_CS_UNSET(v) (!(v) || ((UINT_PTR)(v) & 3))

static CRITICAL_SECTION *host_cs(PXRTL_CRITICAL_SECTION cs)
{
    CRITICAL_SECTION *h = (CRITICAL_SECTION *)cs->Event[0];
    if (HOST_CS_UNSET(h)) {
        CRITICAL_SECTION *n = (CRITICAL_SECTION *)malloc(sizeof *n);
        InitializeCriticalSection(n);
        h = (CRITICAL_SECTION *)InterlockedCompareExchangePointer((PVOID *)&cs->Event[0], n, h);
        if (!HOST_CS_UNSET(h)) { /* another thread won */
            DeleteCriticalSection(n);
            free(n);
        }
        else {
            h = n;
        }
    }
    return h;
}

VOID XBAPI xbox_RtlInitializeCriticalSection(PXRTL_CRITICAL_SECTION CriticalSection)
{
    memset(CriticalSection, 0, sizeof *CriticalSection);
    CriticalSection->LockCount = -1;
    host_cs(CriticalSection);
}

VOID XBAPI xbox_RtlEnterCriticalSection(PXRTL_CRITICAL_SECTION CriticalSection)
{
    EnterCriticalSection(host_cs(CriticalSection));
    CriticalSection->OwningThread = (HANDLE)(UINT_PTR)GetCurrentThreadId();
    CriticalSection->RecursionCount++;
    CriticalSection->LockCount++;
}

VOID XBAPI xbox_RtlLeaveCriticalSection(PXRTL_CRITICAL_SECTION CriticalSection)
{
    CriticalSection->LockCount--;
    if (--CriticalSection->RecursionCount == 0)
        CriticalSection->OwningThread = NULL;
    LeaveCriticalSection(host_cs(CriticalSection));
}

VOID XBAPI xbox_RtlInitAnsiString(PXANSI_STRING DestinationString, PCSTR SourceString)
{
    size_t n = SourceString ? strlen(SourceString) : 0;
    DestinationString->Buffer = (PCHAR)SourceString;
    DestinationString->Length = (USHORT)n;
    DestinationString->MaximumLength = (USHORT)(SourceString ? n + 1 : 0);
}

VOID XBAPI xbox_RtlInitUnicodeString(PXUNICODE_STRING DestinationString, PCWSTR SourceString)
{
    size_t n = SourceString ? wcslen(SourceString) * sizeof(WCHAR) : 0;
    DestinationString->Buffer = (PWSTR)SourceString;
    DestinationString->Length = (USHORT)n;
    DestinationString->MaximumLength = (USHORT)(SourceString ? n + sizeof(WCHAR) : 0);
}

SIZE_T XBAPI xbox_RtlCompareMemory(const VOID *Source1, const VOID *Source2, SIZE_T Length)
{
    const unsigned char *a = (const unsigned char *)Source1, *b = (const unsigned char *)Source2;
    SIZE_T i = 0;
    while (i < Length && a[i] == b[i])
        i++;
    return i;
}

SIZE_T XBAPI xbox_RtlCompareMemoryUlong(PVOID Source, SIZE_T Length, ULONG Pattern)
{
    const ULONG *p = (const ULONG *)Source;
    SIZE_T i = 0;
    while (i < Length / 4 && p[i] == Pattern)
        i++;
    return i * 4;
}

VOID XBAPI xbox_RtlFillMemoryUlong(PVOID Destination, SIZE_T Length, ULONG Pattern)
{
    ULONG *p = (ULONG *)Destination;
    for (SIZE_T i = 0; i < Length / 4; i++)
        p[i] = Pattern;
}

LONG XBAPI xbox_RtlCompareString(PXANSI_STRING String1, PXANSI_STRING String2, BOOLEAN CaseInSensitive)
{
    USHORT n = String1->Length < String2->Length ? String1->Length : String2->Length;
    for (USHORT i = 0; i < n; i++) {
        int a = (unsigned char)String1->Buffer[i], b = (unsigned char)String2->Buffer[i];
        if (CaseInSensitive) {
            a = toupper(a);
            b = toupper(b);
        }
        if (a != b)
            return a - b;
    }
    return (LONG)String1->Length - (LONG)String2->Length;
}

BOOLEAN XBAPI xbox_RtlEqualString(PXANSI_STRING String1, PXANSI_STRING String2, BOOLEAN CaseInSensitive)
{
    return String1->Length == String2->Length && xbox_RtlCompareString(String1, String2, CaseInSensitive) == 0;
}

ULONG XBAPI xbox_RtlNtStatusToDosError(NTSTATUS Status)
{
    typedef ULONG(WINAPI * Fn)(NTSTATUS);
    static Fn fn;
    if (!fn)
        fn = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError");
    return fn ? fn(Status) : ERROR_MR_MID_NOT_FOUND;
}

VOID XBAPI xbox_RtlTimeToTimeFields(PLARGE_INTEGER Time, PXTIME_FIELDS TimeFields)
{
    FILETIME ft;
    SYSTEMTIME st;
    ft.dwLowDateTime = Time->LowPart;
    ft.dwHighDateTime = (DWORD)Time->HighPart;
    FileTimeToSystemTime(&ft, &st);
    TimeFields->Year = (SHORT)st.wYear;
    TimeFields->Month = (SHORT)st.wMonth;
    TimeFields->Day = (SHORT)st.wDay;
    TimeFields->Hour = (SHORT)st.wHour;
    TimeFields->Minute = (SHORT)st.wMinute;
    TimeFields->Second = (SHORT)st.wSecond;
    TimeFields->Milliseconds = (SHORT)st.wMilliseconds;
    TimeFields->Weekday = (SHORT)st.wDayOfWeek;
}

BOOLEAN XBAPI xbox_RtlTimeFieldsToTime(PXTIME_FIELDS TimeFields, PLARGE_INTEGER Time)
{
    SYSTEMTIME st;
    FILETIME ft;
    memset(&st, 0, sizeof st);
    st.wYear = (WORD)TimeFields->Year;
    st.wMonth = (WORD)TimeFields->Month;
    st.wDay = (WORD)TimeFields->Day;
    st.wHour = (WORD)TimeFields->Hour;
    st.wMinute = (WORD)TimeFields->Minute;
    st.wSecond = (WORD)TimeFields->Second;
    st.wMilliseconds = (WORD)TimeFields->Milliseconds;
    if (!SystemTimeToFileTime(&st, &ft))
        return FALSE;
    Time->LowPart = ft.dwLowDateTime;
    Time->HighPart = (LONG)ft.dwHighDateTime;
    return TRUE;
}

/* The game's SEH frames are native Windows frames (fs:[0] is shared), so unwinding and
 * raising go straight to the host implementations. */
VOID XBAPI xbox_RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
{
    RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue);
}

VOID XBAPI xbox_RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
{
    RaiseException(ExceptionRecord->ExceptionCode, ExceptionRecord->ExceptionFlags,
                   ExceptionRecord->NumberParameters, ExceptionRecord->ExceptionInformation);
}
