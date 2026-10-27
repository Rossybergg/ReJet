/* Implemented Xbox kernel exports (HLE). Clean-room declarations: NT-style signatures,
 * argument counts checked against call sites in our own Default.xbe.
 *
 * Every function declared here as `xbox_<ExportName>(` is picked up by
 * tools/loader/gen_kernel.py and wired into the thunk table; other imports keep their
 * logging stub. Kernel exports are __stdcall unless marked FASTCALL / CDECL. Note the Xbox
 * kernel has a single process: Nt* calls take no ProcessHandle. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define XBAPI    __stdcall
#define FASTCALL __fastcall

typedef LONG NTSTATUS;
typedef UCHAR KIRQL;
typedef CHAR KPROCESSOR_MODE;
typedef VOID (XBAPI *PKSTART_ROUTINE)(PVOID StartContext);
typedef VOID (XBAPI *PKSYSTEM_ROUTINE)(PKSTART_ROUTINE StartRoutine, PVOID StartContext);

typedef struct XANSI_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} XANSI_STRING, *PXANSI_STRING;

typedef struct XUNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} XUNICODE_STRING, *PXUNICODE_STRING;

/* Xbox OBJECT_ATTRIBUTES: no Length/SecurityDescriptor fields, unlike Windows. */
typedef struct XOBJECT_ATTRIBUTES {
    HANDLE RootDirectory;
    PXANSI_STRING ObjectName;
    ULONG Attributes;
} XOBJECT_ATTRIBUTES, *PXOBJECT_ATTRIBUTES;

/* Xbox RTL_CRITICAL_SECTION (0x1C bytes). We keep a host CRITICAL_SECTION pointer in
 * Event[0] and maintain the visible counters. */
typedef struct XRTL_CRITICAL_SECTION {
    ULONG_PTR Event[4];
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
} XRTL_CRITICAL_SECTION, *PXRTL_CRITICAL_SECTION;

typedef struct XIO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
} XIO_STATUS_BLOCK, *PIO_STATUS_BLOCK_X;

/* Dispatcher objects that live in game memory (Xbox layouts). */
typedef struct XDISPATCHER_HEADER {
    UCHAR Type;
    UCHAR Absolute;
    UCHAR Size;           /* in LONGs */
    UCHAR Inserted;
    LONG SignalState;
    LIST_ENTRY WaitListHead;
} XDISPATCHER_HEADER;

typedef struct XKDPC *PXKDPC;
typedef VOID (XBAPI *PKDEFERRED_ROUTINE)(PXKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1,
                                         PVOID SystemArgument2);
typedef struct XKDPC {
    SHORT Type;
    UCHAR Inserted;
    UCHAR Padding;
    LIST_ENTRY DpcListEntry;
    PKDEFERRED_ROUTINE DeferredRoutine;
    PVOID DeferredContext;
    PVOID SystemArgument1;
    PVOID SystemArgument2;
} XKDPC;

typedef struct XKTIMER {
    XDISPATCHER_HEADER Header;
    ULARGE_INTEGER DueTime;
    LIST_ENTRY TimerListEntry;
    PXKDPC Dpc;
    LONG Period;
} XKTIMER, *PXKTIMER;

#define XTIMER_NOTIFICATION_OBJECT 8 /* + 1 for a synchronization timer */
#define XDPC_OBJECT 19

_Static_assert(sizeof(XDISPATCHER_HEADER) == 0x10, "DISPATCHER_HEADER layout");
_Static_assert(sizeof(XKDPC) == 0x1C, "KDPC layout");
_Static_assert(sizeof(XKTIMER) == 0x28, "KTIMER layout");

#define STATUS_SUCCESS                ((NTSTATUS)0x00000000L)
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL           ((NTSTATUS)0xC0000001L)
#endif
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
#define STATUS_INVALID_PARAMETER_X    ((NTSTATUS)0xC000000DL)
#define STATUS_OBJECT_NAME_NOT_FOUND_X ((NTSTATUS)0xC0000034L)

/* Trace each implemented call (on while Phase 3 bring-up is in progress). */
void ktrace(const char *fmt, ...);
#define KTRACE(...) ktrace(__VA_ARGS__)

/* Helpers shared by the kernel modules (kernel/common.c). */
DWORD nt_timeout_ms(const LARGE_INTEGER *timeout); /* NT relative/absolute 100 ns -> ms */
NTSTATUS nt_wait_status(DWORD wait_result, DWORD count);
NTSTATUS nt_last_error_status(void);

/* --- Ps: threads --- */
NTSTATUS XBAPI xbox_PsCreateSystemThreadEx(PHANDLE ThreadHandle, ULONG ThreadExtensionSize,
                                           ULONG KernelStackSize, ULONG TlsDataSize, PHANDLE ThreadId,
                                           PKSTART_ROUTINE StartRoutine, PVOID StartContext,
                                           BOOLEAN CreateSuspended, BOOLEAN DebuggerThread,
                                           PKSYSTEM_ROUTINE SystemRoutine);
__declspec(noreturn) VOID XBAPI xbox_PsTerminateSystemThread(NTSTATUS ExitStatus);
NTSTATUS XBAPI xbox_NtResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
NTSTATUS XBAPI xbox_NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);

/* --- Ob: handles --- */
NTSTATUS XBAPI xbox_NtClose(HANDLE Handle);
NTSTATUS XBAPI xbox_NtDuplicateObject(HANDLE SourceHandle, PHANDLE TargetHandle, DWORD Options);

/* --- Synchronisation objects and waits --- */
NTSTATUS XBAPI xbox_NtCreateEvent(PHANDLE EventHandle, PXOBJECT_ATTRIBUTES ObjectAttributes, ULONG EventType,
                                  BOOLEAN InitialState);
NTSTATUS XBAPI xbox_NtSetEvent(HANDLE EventHandle, PLONG PreviousState);
NTSTATUS XBAPI xbox_NtClearEvent(HANDLE EventHandle);
NTSTATUS XBAPI xbox_NtPulseEvent(HANDLE EventHandle, PLONG PreviousState);
NTSTATUS XBAPI xbox_NtCreateSemaphore(PHANDLE SemaphoreHandle, PXOBJECT_ATTRIBUTES ObjectAttributes,
                                      LONG InitialCount, LONG MaximumCount);
NTSTATUS XBAPI xbox_NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount, PLONG PreviousCount);
NTSTATUS XBAPI xbox_NtCreateMutant(PHANDLE MutantHandle, PXOBJECT_ATTRIBUTES ObjectAttributes,
                                   BOOLEAN InitialOwner);
NTSTATUS XBAPI xbox_NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);
NTSTATUS XBAPI xbox_NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
NTSTATUS XBAPI xbox_NtWaitForSingleObjectEx(HANDLE Handle, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                            PLARGE_INTEGER Timeout);
NTSTATUS XBAPI xbox_NtWaitForMultipleObjectsEx(ULONG Count, HANDLE *Handles, ULONG WaitType,
                                               KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                               PLARGE_INTEGER Timeout);
NTSTATUS XBAPI xbox_NtSignalAndWaitForSingleObjectEx(HANDLE SignalHandle, HANDLE WaitHandle,
                                                     KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                                     PLARGE_INTEGER Timeout);
NTSTATUS XBAPI xbox_NtYieldExecution(void);
NTSTATUS XBAPI xbox_KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);

/* --- Ke: time, IRQL, misc --- */
ULONGLONG XBAPI xbox_KeQueryPerformanceCounter(void);
ULONGLONG XBAPI xbox_KeQueryPerformanceFrequency(void);
VOID XBAPI xbox_KeQuerySystemTime(PLARGE_INTEGER CurrentTime);
ULONGLONG XBAPI xbox_KeQueryInterruptTime(void);
VOID XBAPI xbox_KeStallExecutionProcessor(ULONG MicroSeconds);
KIRQL FASTCALL xbox_KfRaiseIrql(KIRQL NewIrql);
VOID FASTCALL xbox_KfLowerIrql(KIRQL NewIrql);
KIRQL XBAPI xbox_KeRaiseIrqlToDpcLevel(void);
__declspec(noreturn) VOID XBAPI xbox_KeBugCheck(ULONG BugCheckCode);

/* --- Ke: DPCs and timers (kernel/dpc.c) --- */
VOID XBAPI xbox_KeInitializeDpc(PXKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext);
BOOLEAN XBAPI xbox_KeInsertQueueDpc(PXKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2);
BOOLEAN XBAPI xbox_KeRemoveQueueDpc(PXKDPC Dpc);
VOID XBAPI xbox_KeInitializeTimerEx(PXKTIMER Timer, ULONG Type);
BOOLEAN XBAPI xbox_KeSetTimer(PXKTIMER Timer, LARGE_INTEGER DueTime, PXKDPC Dpc);
BOOLEAN XBAPI xbox_KeSetTimerEx(PXKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PXKDPC Dpc);
BOOLEAN XBAPI xbox_KeCancelTimer(PXKTIMER Timer);
void kernel_ticker_start(void); /* keeps the KeTickCount data export current */

/* --- Rtl --- */
typedef struct XTIME_FIELDS { /* note: different field order from SYSTEMTIME */
    SHORT Year;
    SHORT Month;
    SHORT Day;
    SHORT Hour;
    SHORT Minute;
    SHORT Second;
    SHORT Milliseconds;
    SHORT Weekday;
} XTIME_FIELDS, *PXTIME_FIELDS;

VOID XBAPI xbox_RtlTimeToTimeFields(PLARGE_INTEGER Time, PXTIME_FIELDS TimeFields);
BOOLEAN XBAPI xbox_RtlTimeFieldsToTime(PXTIME_FIELDS TimeFields, PLARGE_INTEGER Time);
VOID XBAPI xbox_RtlInitializeCriticalSection(PXRTL_CRITICAL_SECTION CriticalSection);
VOID XBAPI xbox_RtlEnterCriticalSection(PXRTL_CRITICAL_SECTION CriticalSection);
VOID XBAPI xbox_RtlLeaveCriticalSection(PXRTL_CRITICAL_SECTION CriticalSection);
VOID XBAPI xbox_RtlInitAnsiString(PXANSI_STRING DestinationString, PCSTR SourceString);
VOID XBAPI xbox_RtlInitUnicodeString(PXUNICODE_STRING DestinationString, PCWSTR SourceString);
SIZE_T XBAPI xbox_RtlCompareMemory(const VOID *Source1, const VOID *Source2, SIZE_T Length);
SIZE_T XBAPI xbox_RtlCompareMemoryUlong(PVOID Source, SIZE_T Length, ULONG Pattern);
VOID XBAPI xbox_RtlFillMemoryUlong(PVOID Destination, SIZE_T Length, ULONG Pattern);
LONG XBAPI xbox_RtlCompareString(PXANSI_STRING String1, PXANSI_STRING String2, BOOLEAN CaseInSensitive);
BOOLEAN XBAPI xbox_RtlEqualString(PXANSI_STRING String1, PXANSI_STRING String2, BOOLEAN CaseInSensitive);
ULONG XBAPI xbox_RtlNtStatusToDosError(NTSTATUS Status);
VOID XBAPI xbox_RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord,
                          PVOID ReturnValue);
VOID XBAPI xbox_RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord);

/* --- Mm / Nt virtual memory --- */
NTSTATUS XBAPI xbox_NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG_PTR ZeroBits, PSIZE_T AllocationSize,
                                            DWORD AllocationType, DWORD Protect);
NTSTATUS XBAPI xbox_NtFreeVirtualMemory(PVOID *BaseAddress, PSIZE_T FreeSize, DWORD FreeType);
NTSTATUS XBAPI xbox_NtQueryVirtualMemory(PVOID BaseAddress, PMEMORY_BASIC_INFORMATION Buffer);
NTSTATUS XBAPI xbox_NtProtectVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, DWORD NewProtect,
                                           PDWORD OldProtect);
PVOID XBAPI xbox_MmAllocateContiguousMemory(ULONG NumberOfBytes);
PVOID XBAPI xbox_MmAllocateContiguousMemoryEx(ULONG NumberOfBytes, ULONG_PTR LowestAcceptableAddress,
                                              ULONG_PTR HighestAcceptableAddress, ULONG Alignment,
                                              ULONG ProtectionType);
VOID XBAPI xbox_MmFreeContiguousMemory(PVOID BaseAddress);
PVOID XBAPI xbox_MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect);
ULONG XBAPI xbox_MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes);
ULONG XBAPI xbox_MmQueryAllocationSize(PVOID BaseAddress);
ULONG_PTR XBAPI xbox_MmGetPhysicalAddress(PVOID BaseAddress);
VOID XBAPI xbox_MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist);
VOID XBAPI xbox_MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect);
ULONG XBAPI xbox_MmQueryAddressProtect(PVOID VirtualAddress);

typedef struct XMM_STATISTICS {
    ULONG Length;
    ULONG TotalPhysicalPages;
    ULONG AvailablePages;
    ULONG VirtualMemoryBytesCommitted;
    ULONG VirtualMemoryBytesReserved;
    ULONG CachePagesCommitted;
    ULONG PoolPagesCommitted;
    ULONG StackPagesCommitted;
    ULONG ImagePagesCommitted;
} XMM_STATISTICS, *PXMM_STATISTICS;
NTSTATUS XBAPI xbox_MmQueryStatistics(PXMM_STATISTICS MemoryStatistics);

/* --- Ex: pool, settings --- */
PVOID XBAPI xbox_ExAllocatePool(SIZE_T NumberOfBytes);
PVOID XBAPI xbox_ExAllocatePoolWithTag(SIZE_T NumberOfBytes, ULONG Tag);
VOID XBAPI xbox_ExFreePool(PVOID P);
ULONG XBAPI xbox_ExQueryPoolBlockSize(PVOID PoolBlock);
NTSTATUS XBAPI xbox_ExQueryNonVolatileSetting(ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength,
                                              PULONG ResultLength);

/* --- Io / Nt file system (kernel/io.c) --- */
typedef struct XFILE_FS_SIZE_INFORMATION {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER AvailableAllocationUnits;
    ULONG SectorsPerAllocationUnit;
    ULONG BytesPerSector;
} XFILE_FS_SIZE_INFORMATION;

typedef struct XDISK_GEOMETRY {
    LARGE_INTEGER Cylinders;
    ULONG MediaType;
    ULONG TracksPerCylinder;
    ULONG SectorsPerTrack;
    ULONG BytesPerSector;
} XDISK_GEOMETRY;

typedef struct XPARTITION_INFORMATION {
    LARGE_INTEGER StartingOffset;
    LARGE_INTEGER PartitionLength;
    ULONG HiddenSectors;
    ULONG PartitionNumber;
    UCHAR PartitionType;
    BOOLEAN BootIndicator;
    BOOLEAN RecognizedPartition;
    BOOLEAN RewritePartition;
} XPARTITION_INFORMATION;

typedef struct XFILE_DIRECTORY_INFORMATION { /* ANSI name, unlike Windows */
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    LARGE_INTEGER EndOfFile, AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    CHAR FileName[1];
} XFILE_DIRECTORY_INFORMATION;

NTSTATUS XBAPI xbox_NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                         PIO_STATUS_BLOCK_X IoStatusBlock, PVOID FileInformation, ULONG Length,
                                         ULONG FileInformationClass, PXANSI_STRING FileMask, BOOLEAN RestartScan);

void io_init(const char *xbe_path); /* D: = the XBE's folder */
NTSTATUS XBAPI xbox_NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                          PIO_STATUS_BLOCK_X IoStatusBlock, ULONG IoControlCode, PVOID InputBuffer,
                                          ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength);
void io_forget_handle(HANDLE h);   /* called by NtClose */
NTSTATUS XBAPI xbox_NtFsControlFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                    PIO_STATUS_BLOCK_X IoStatusBlock, ULONG FsControlCode, PVOID InputBuffer,
                                    ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength);
NTSTATUS XBAPI xbox_NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, PXOBJECT_ATTRIBUTES ObjectAttributes,
                                 PIO_STATUS_BLOCK_X IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                 ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions);
NTSTATUS XBAPI xbox_NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, PXOBJECT_ATTRIBUTES ObjectAttributes,
                               PIO_STATUS_BLOCK_X IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions);
NTSTATUS XBAPI xbox_NtReadFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                               PIO_STATUS_BLOCK_X IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset);
NTSTATUS XBAPI xbox_NtWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                PIO_STATUS_BLOCK_X IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset);
NTSTATUS XBAPI xbox_NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK_X IoStatusBlock, PVOID FileInformation,
                                           ULONG Length, ULONG FileInformationClass);
NTSTATUS XBAPI xbox_NtSetInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK_X IoStatusBlock, PVOID FileInformation,
                                         ULONG Length, ULONG FileInformationClass);
NTSTATUS XBAPI xbox_NtQueryVolumeInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK_X IoStatusBlock,
                                                 PVOID FsInformation, ULONG Length, ULONG FsInformationClass);
NTSTATUS XBAPI xbox_NtFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK_X IoStatusBlock);
NTSTATUS XBAPI xbox_IoCreateSymbolicLink(PXANSI_STRING SymbolicLinkName, PXANSI_STRING DeviceName);
NTSTATUS XBAPI xbox_NtOpenSymbolicLinkObject(PHANDLE LinkHandle, PXOBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS XBAPI xbox_NtQuerySymbolicLinkObject(HANDLE LinkHandle, PXANSI_STRING LinkTarget, PULONG ReturnedLength);
NTSTATUS XBAPI xbox_IoDeleteSymbolicLink(PXANSI_STRING SymbolicLinkName);

/* --- Xc: crypto (kernel/xc.c) --- */
void xc_selftest(void);
void sha1_digest(const void *data, unsigned long size, UCHAR digest[20]);
VOID XBAPI xbox_XcSHAInit(PUCHAR pbSHAContext);
VOID XBAPI xbox_XcSHAUpdate(PUCHAR pbSHAContext, PUCHAR pbInput, ULONG dwInputLength);
VOID XBAPI xbox_XcSHAFinal(PUCHAR pbSHAContext, PUCHAR pbDigest);
VOID XBAPI xbox_XcHMAC(PUCHAR pbKeyMaterial, ULONG cbKeyMaterial, PUCHAR pbData, ULONG cbData, PUCHAR pbData2,
                       ULONG cbData2, PUCHAR HmacData);
VOID XBAPI xbox_XcRC4Key(PUCHAR pbKeyStruct, ULONG dwKeyLength, PUCHAR pbKey);
VOID XBAPI xbox_XcRC4Crypt(PUCHAR pbKeyStruct, ULONG dwInputLength, PUCHAR pbInput);

/* --- Hal / Av / Xe / Dbg --- */
__declspec(noreturn) VOID XBAPI xbox_HalReturnToFirmware(ULONG Routine);
PVOID XBAPI xbox_AvGetSavedDataAddress(void);
ULONG XBAPI xbox_PhyGetLinkState(BOOLEAN Verify);
NTSTATUS XBAPI xbox_PhyInitialize(BOOLEAN ForceReset, PVOID Parameter2);
VOID XBAPI xbox_AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, PULONG Result);
NTSTATUS XBAPI xbox_XeLoadSection(PVOID Section);
NTSTATUS XBAPI xbox_XeUnloadSection(PVOID Section);
ULONG __cdecl xbox_DbgPrint(PCSTR Format, ...);
