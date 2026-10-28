/* HLE patches: XDK library functions inside the XBE replaced by host implementations.
 *
 * Each patch overwrites the function entry with a 5-byte `jmp rel32` to a C function with
 * the same signature and calling convention (hybrid layer only; see CLAUDE.md). Addresses
 * are those of our own Default.xbe, so the file's SHA-1 must match config/xbe.sha1 before
 * anything is patched. */
#include <string.h>

#include "hle.h"
#include "kernel.h"
#include "xboxkrnl_impl.h"
#include "xdk_addrs.gen.h"

/* config/xbe.sha1 */
static const UCHAR XBE_SHA1[20] = {0xed, 0xd8, 0x54, 0x71, 0x85, 0x02, 0x58, 0x34, 0xdf, 0x91,
                                   0x47, 0x24, 0x06, 0x9a, 0xa1, 0xe6, 0x31, 0x09, 0xcb, 0xc1};

/* --- Time (XAPILIB in .text) --- */

/* XAPI QueryPerformanceCounter (0x145FF0) is `rdtsc`, and QueryPerformanceFrequency (0x146001)
 * returns a constant 733,333,333 (the Xbox CPU clock), as in ReWhacked's XDK 4831. On a PC the
 * TSC runs several times faster, which would make the game's file-load timeouts and profiler
 * (and D3D) run fast. Return host time on the Xbox clock. */
#define XBOX_TSC_HZ 733333333ull

static BOOL XBAPI hle_QueryPerformanceCounter(LARGE_INTEGER *pCount)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    ULONGLONG q;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    q = (ULONGLONG)c.QuadPart;
    pCount->QuadPart = (LONGLONG)((q / (ULONGLONG)f.QuadPart) * XBOX_TSC_HZ +
                                  (q % (ULONGLONG)f.QuadPart) * XBOX_TSC_HZ / (ULONGLONG)f.QuadPart);
    return TRUE;
}

/* XAPI SetThreadPriority goes through ObReferenceObjectByHandle and the kernel thread object;
 * our thread handles are host thread handles (kernel/ps.c), so set the priority directly.
 * JSRF raises the CRI ADX vblank thread's priority (0x13BDE0). */
static BOOL XBAPI hle_SetThreadPriority(HANDLE thread, int priority)
{
    if (priority > THREAD_PRIORITY_HIGHEST && priority != THREAD_PRIORITY_TIME_CRITICAL)
        priority = THREAD_PRIORITY_HIGHEST;
    if (priority < THREAD_PRIORITY_LOWEST && priority != THREAD_PRIORITY_IDLE)
        priority = THREAD_PRIORITY_LOWEST;
    return SetThreadPriority(thread, priority) ? TRUE : TRUE; /* best effort, like a console */
}

/* Same reason (ObReferenceObjectByHandle): CRI ADX's thread setup (0x13BB50, 0x13BDE0) reads
 * its thread's priority and turns priority boost off. */
static int XBAPI hle_GetThreadPriority(HANDLE thread)
{
    int p = GetThreadPriority(thread);
    return p == THREAD_PRIORITY_ERROR_RETURN ? THREAD_PRIORITY_NORMAL : p;
}

static BOOL XBAPI hle_SetThreadPriorityBoost(HANDLE thread, BOOL disable)
{
    SetThreadPriorityBoost(thread, disable);
    return TRUE;
}

static const HlePatch g_time_patches[] = {
    {XDK_XAPILIB_QueryPerformanceCounter, (void *)hle_QueryPerformanceCounter, "XAPILIB::QueryPerformanceCounter"},
    {XDK_XAPILIB_SetThreadPriority, (void *)hle_SetThreadPriority, "XAPILIB::SetThreadPriority"},
    {XDK_XAPILIB_GetThreadPriority, (void *)hle_GetThreadPriority, "XAPILIB::GetThreadPriority"},
    {XDK_XAPILIB_SetThreadPriorityBoost, (void *)hle_SetThreadPriorityBoost, "XAPILIB::SetThreadPriorityBoost"},
};

static unsigned patch_table(const HlePatch *t, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        unsigned char *at = (unsigned char *)(UINT_PTR)t[i].address;
        LONG rel = (LONG)((UINT_PTR)t[i].impl - ((UINT_PTR)at + 5));
        at[0] = 0xE9; /* jmp rel32 */
        memcpy(at + 1, &rel, sizeof rel);
    }
    return count;
}

int hle_xbe_supported(const unsigned char *xbe_file, unsigned long xbe_size)
{
    UCHAR digest[20];
    sha1_digest(xbe_file, xbe_size, digest);
    return !memcmp(digest, XBE_SHA1, sizeof digest);
}

int hle_apply(const unsigned char *xbe_file, unsigned long xbe_size)
{
    UCHAR digest[20];
    unsigned n = 0;

    sha1_digest(xbe_file, xbe_size, digest);
    if (memcmp(digest, XBE_SHA1, sizeof digest)) {
        log_printf("error: Default.xbe SHA-1 does not match config/xbe.sha1; HLE addresses would be wrong\n");
        return 0;
    }
    n += patch_table(g_time_patches, sizeof g_time_patches / sizeof g_time_patches[0]);
    n += patch_table(g_d3d_patches, g_d3d_patch_count);
    n += patch_table(g_input_patches, g_input_patch_count);
    n += patch_table(g_audio_patches, g_audio_patch_count);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    log_printf("hle: xbe hash ok, %u functions patched\n", n);
    return 1;
}
