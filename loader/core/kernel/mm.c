/* Mm / Nt virtual memory and Ex pool. The Xbox has one flat address space; host
 * VirtualAlloc/HeapAlloc stand in. "Physical" addresses are the virtual ones: D3D and
 * DSOUND, the only real users of physical memory, are replaced by HLE shims. */
#include <string.h>

#include "kernel.h"
#include "memdump.h"
#include "xboxkrnl_impl.h"

#define XBOX_MEM_NOZERO 0x00800000 /* Xbox-only allocation flag */

/* Strip Xbox-only protection bits (PAGE_WRITECOMBINE / PAGE_NOCACHE). */
static DWORD host_protect(DWORD p)
{
    p &= ~(PAGE_NOCACHE | PAGE_WRITECOMBINE);
    return p ? p : PAGE_READWRITE;
}

NTSTATUS XBAPI xbox_NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG_PTR ZeroBits, PSIZE_T AllocationSize,
                                            DWORD AllocationType, DWORD Protect)
{
    PVOID p;
    (void)ZeroBits;
    p = VirtualAlloc(*BaseAddress, *AllocationSize, AllocationType & ~XBOX_MEM_NOZERO, host_protect(Protect));
    KTRACE("NtAllocateVirtualMemory(%08X, %lu, type %lX, prot %lX) -> %08X\n", (unsigned)(UINT_PTR)*BaseAddress,
           (unsigned long)*AllocationSize, AllocationType, Protect, (unsigned)(UINT_PTR)p);
    if (!p)
        return nt_last_error_status();
    {
        /* NT semantics: the requested range rounded out to pages. Never report the whole
         * VirtualQuery region: adjacent pages already committed (and in use by the caller's
         * heap) would be handed back as new memory and get reused on top of live objects. */
        UINT_PTR start = (UINT_PTR)(*BaseAddress ? *BaseAddress : p) & ~(UINT_PTR)0xFFF;
        UINT_PTR end = ((UINT_PTR)(*BaseAddress ? *BaseAddress : p) + *AllocationSize + 0xFFF) & ~(UINT_PTR)0xFFF;
        *BaseAddress = (PVOID)start;
        *AllocationSize = end - start;
        if (AllocationType & MEM_COMMIT)
            memdump_note(*BaseAddress, *AllocationSize);
    }
    return STATUS_SUCCESS;
}

NTSTATUS XBAPI xbox_NtFreeVirtualMemory(PVOID *BaseAddress, PSIZE_T FreeSize, DWORD FreeType)
{
    SIZE_T size = (FreeType & MEM_RELEASE) ? 0 : *FreeSize;
    return VirtualFree(*BaseAddress, size, FreeType) ? STATUS_SUCCESS : nt_last_error_status();
}

NTSTATUS XBAPI xbox_NtQueryVirtualMemory(PVOID BaseAddress, PMEMORY_BASIC_INFORMATION Buffer)
{
    return VirtualQuery(BaseAddress, Buffer, sizeof *Buffer) ? STATUS_SUCCESS : nt_last_error_status();
}

NTSTATUS XBAPI xbox_NtProtectVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize, DWORD NewProtect,
                                           PDWORD OldProtect)
{
    return VirtualProtect(*BaseAddress, *RegionSize, host_protect(NewProtect), OldProtect) ? STATUS_SUCCESS
                                                                                           : nt_last_error_status();
}

PVOID XBAPI xbox_MmAllocateContiguousMemoryEx(ULONG NumberOfBytes, ULONG_PTR LowestAcceptableAddress,
                                              ULONG_PTR HighestAcceptableAddress, ULONG Alignment,
                                              ULONG ProtectionType)
{
    /* VirtualAlloc returns 64 KB-aligned blocks, enough for any Xbox alignment request. */
    PVOID p = VirtualAlloc(NULL, NumberOfBytes, MEM_RESERVE | MEM_COMMIT, host_protect(ProtectionType));
    memdump_note(p, NumberOfBytes);
    KTRACE("MmAllocateContiguousMemoryEx(%lu, %08lX-%08lX, align %lu) -> %08X\n", NumberOfBytes,
           (unsigned long)LowestAcceptableAddress, (unsigned long)HighestAcceptableAddress, Alignment,
           (unsigned)(UINT_PTR)p);
    return p;
}

PVOID XBAPI xbox_MmAllocateContiguousMemory(ULONG NumberOfBytes)
{
    return xbox_MmAllocateContiguousMemoryEx(NumberOfBytes, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
}

VOID XBAPI xbox_MmFreeContiguousMemory(PVOID BaseAddress)
{
    VirtualFree(BaseAddress, 0, MEM_RELEASE);
}

PVOID XBAPI xbox_MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
{
    return VirtualAlloc(NULL, NumberOfBytes, MEM_RESERVE | MEM_COMMIT, host_protect(Protect));
}

ULONG XBAPI xbox_MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes)
{
    (void)NumberOfBytes;
    return VirtualFree(BaseAddress, 0, MEM_RELEASE) ? NumberOfBytes : 0;
}

ULONG XBAPI xbox_MmQueryAllocationSize(PVOID BaseAddress)
{
    MEMORY_BASIC_INFORMATION m;
    ULONG total = 0;
    PVOID base;
    if (!VirtualQuery(BaseAddress, &m, sizeof m))
        return 0;
    base = m.AllocationBase;
    for (BYTE *p = (BYTE *)base; VirtualQuery(p, &m, sizeof m) && m.AllocationBase == base && m.State != MEM_FREE;
         p += m.RegionSize)
        total += (ULONG)m.RegionSize;
    return total;
}

ULONG_PTR XBAPI xbox_MmGetPhysicalAddress(PVOID BaseAddress)
{
    return (ULONG_PTR)BaseAddress;
}

VOID XBAPI xbox_MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
{
    (void)BaseAddress;
    (void)NumberOfBytes;
    (void)Persist;
}

VOID XBAPI xbox_MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect)
{
    DWORD old;
    VirtualProtect(BaseAddress, NumberOfBytes, host_protect(NewProtect), &old);
}

ULONG XBAPI xbox_MmQueryAddressProtect(PVOID VirtualAddress)
{
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery(VirtualAddress, &m, sizeof m) ? m.Protect : 0;
}

/* Xbox-shaped answer: 64 MB of 4 KB pages, ~40 MB available. Callers size buffers from it. */
NTSTATUS XBAPI xbox_MmQueryStatistics(PXMM_STATISTICS MemoryStatistics)
{
    ULONG len;
    if (!MemoryStatistics || MemoryStatistics->Length < 3 * sizeof(ULONG))
        return STATUS_INVALID_PARAMETER_X;
    len = MemoryStatistics->Length; /* fill only what the caller's structure holds */
    memset((char *)MemoryStatistics + sizeof(ULONG), 0, len - sizeof(ULONG));
    MemoryStatistics->TotalPhysicalPages = 64 * 1024 * 1024 / 4096;
    MemoryStatistics->AvailablePages = 40 * 1024 * 1024 / 4096;
    return STATUS_SUCCESS;
}

/* --- Ex pool --- */

PVOID XBAPI xbox_ExAllocatePoolWithTag(SIZE_T NumberOfBytes, ULONG Tag)
{
    PVOID p = HeapAlloc(GetProcessHeap(), 0, NumberOfBytes);
    (void)Tag;
    memdump_note(p, NumberOfBytes);
    return p;
}

PVOID XBAPI xbox_ExAllocatePool(SIZE_T NumberOfBytes)
{
    PVOID p = HeapAlloc(GetProcessHeap(), 0, NumberOfBytes);
    memdump_note(p, NumberOfBytes);
    return p;
}

VOID XBAPI xbox_ExFreePool(PVOID P)
{
    HeapFree(GetProcessHeap(), 0, P);
}

ULONG XBAPI xbox_ExQueryPoolBlockSize(PVOID PoolBlock)
{
    return (ULONG)HeapSize(GetProcessHeap(), 0, PoolBlock);
}
