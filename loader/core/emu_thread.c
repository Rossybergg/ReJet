#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "emu_thread.h"
#include "kernel.h"

#define TEB_TLS_SLOTS 0xE10 /* offset of TlsSlots[64] in the 32-bit TEB */

static DWORD g_slot[FS_SLOT_COUNT];
static DWORD g_self = TLS_OUT_OF_INDEXES;

int emu_fs_init(void)
{
    unsigned patched = 0;

    g_self = TlsAlloc();
    for (unsigned s = 0; s < FS_SLOT_COUNT; s++) {
        g_slot[s] = TlsAlloc();
        if (g_slot[s] >= 64) { /* only TlsSlots[0..63] live directly in the TEB */
            log_printf("error: TLS slot %lu is not a direct TEB slot\n", g_slot[s]);
            return 0;
        }
    }
    for (unsigned i = 0; i < g_fs_patch_count; i++) {
        const FsPatch *p = &g_fs_patches[i];
        unsigned char *ins = (unsigned char *)(UINT_PTR)p->address;
        unsigned *disp = (unsigned *)(ins + p->disp_offset);
        if (ins[0] != 0x64 || *disp != p->old_disp) {
            log_printf("error: fs: site %08X does not match (byte %02X, disp %08X)\n", p->address, ins[0], *disp);
            return 0;
        }
        *disp = TEB_TLS_SLOTS + 4 * g_slot[p->slot];
        patched++;
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    log_printf("fs: %u sites redirected to TLS slots %lu..%lu\n", patched, g_slot[0], g_slot[FS_SLOT_COUNT - 1]);
    return 1;
}

/* Debugging: REJET_WATCH=<hex address> arms a 4-byte hardware write watchpoint (DR0) on
 * every Xbox thread; the loader's exception handler logs who writes there. */
void emu_arm_watchpoint(void)
{
    static UINT_PTR addr = (UINT_PTR)-1;
    CONTEXT c;
    if (addr == (UINT_PTR)-1) {
        char env[32] = "";
        addr = GetEnvironmentVariableA("REJET_WATCH", env, sizeof env) ? (UINT_PTR)strtoul(env, NULL, 16) : 0;
        if (addr)
            log_printf("watch: hardware write watchpoint on %08X\n", (unsigned)addr);
    }
    if (!addr)
        return;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    c.Dr0 = addr & ~(UINT_PTR)3;
    c.Dr7 = 1 | (1u << 16) | (3u << 18); /* L0, write-only, 4 bytes */
    SetThreadContext(GetCurrentThread(), &c);
}

/* The Xbox has one CPU, and some of its code depends on that. CRI ADX makes a lock out of
 * thread priorities (0x13BB50: raise the caller to 15, then resume a priority-2 spinner,
 * 0x13BC30, so no thread at priority 1 or below can run; 0x13BB90 suspends it again). On
 * several cores that excludes nothing and the spinner takes a core. So every thread that runs
 * Xbox code (XAPI threads, the entry thread, kernel DPC/timer threads) shares one host core,
 * with Windows' priority scheduling standing in for the Xbox's; host threads (window, D3D,
 * mixer, XAudio2) use the others. REJET_XBOXCPU=<n> picks the core, =all turns this off. */
/* Keep ReJet's host threads (and the graphics driver's) off the Xbox core and its hyperthread
 * sibling: work on the sibling slowed the game thread to less than half speed in some frames
 * (a timed fixed loop took 77 instead of 33 us), and each of those frames missed its vblank.
 * Soft: Windows' default CPU sets for the process; the Xbox threads' affinity overrides it. */
static void reserve_xbox_core(DWORD_PTR mask)
{
    DWORD_PTR core = mask;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION lpi[256];
    DWORD len = sizeof lpi;
    unsigned char cs[64 * 32];
    ULONG cs_len = 0, ids[64], n = 0;
    if (GetLogicalProcessorInformation(lpi, &len))
        for (DWORD i = 0; i < len / sizeof lpi[0]; i++)
            if (lpi[i].Relationship == RelationProcessorCore && (lpi[i].ProcessorMask & mask))
                core = lpi[i].ProcessorMask;
    if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)cs, sizeof cs, &cs_len, GetCurrentProcess(), 0))
        return;
    for (ULONG off = 0; off < cs_len && n < 64;) {
        PSYSTEM_CPU_SET_INFORMATION c = (PSYSTEM_CPU_SET_INFORMATION)(cs + off);
        if (c->Type == CpuSetInformation && c->CpuSet.Group == 0 &&
            !(core & ((DWORD_PTR)1 << c->CpuSet.LogicalProcessorIndex)))
            ids[n++] = c->CpuSet.Id;
        off += c->Size;
    }
    if (n && SetProcessDefaultCpuSets(GetCurrentProcess(), ids, n))
        log_printf("emu: host threads kept off the Xbox core (mask %08lX)\n", (unsigned long)core);
}

static DWORD_PTR xbox_cpu_mask(void)
{
    static DWORD_PTR mask = (DWORD_PTR)-1;
    if (mask == (DWORD_PTR)-1) {
        DWORD_PTR proc = 0, sys = 0;
        char env[8] = "";
        mask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        if (GetEnvironmentVariableA("REJET_XBOXCPU", env, sizeof env) && !_stricmp(env, "all")) {
            log_printf("emu: Xbox threads on all cores (REJET_XBOXCPU=all)\n");
        }
        else if (env[0] && (proc & ((DWORD_PTR)1 << (atoi(env) & 31)))) {
            mask = (DWORD_PTR)1 << (atoi(env) & 31);
        }
        else {
            for (int i = 31; i >= 0 && !mask; i--) /* the highest core the process may use */
                if (proc & ((DWORD_PTR)1 << i))
                    mask = (DWORD_PTR)1 << i;
            if (mask == proc) /* a single-core machine: nothing to separate */
                mask = 0;
        }
        if (mask)
            reserve_xbox_core(mask);
        if (mask)
            log_printf("emu: Xbox threads on host core mask %08lX (process mask %08lX)\n", (unsigned long)mask,
                       (unsigned long)proc);
    }
    return mask;
}

EmuThread *emu_thread_attach(unsigned tls_size)
{
    if (xbox_cpu_mask())
        SetThreadAffinityMask(GetCurrentThread(), xbox_cpu_mask());
    emu_arm_watchpoint();
    EmuThread *t = (EmuThread *)calloc(1, sizeof *t);
    unsigned char *prcb;

    if (!t)
        return NULL;
    t->thread_id = GetCurrentThreadId();
    t->tls_size = tls_size;
    t->tls = (unsigned char *)calloc(1, tls_size ? (tls_size + 15) & ~15u : 16);
    if (!t->tls) {
        free(t);
        return NULL;
    }
    prcb = t->kpcr + 0x28;
    *(void **)(prcb + KPRCB_CURRENT_THREAD) = t->kthread;
    *(void **)(t->kthread + KTHREAD_TLS_DATA) = t->tls;
    *(unsigned *)(t->kthread + ETHREAD_THREAD_ID) = t->thread_id;

    /* Xbox TLS grows down from the stack base: fs:[4] points at the END of the block and the
     * title's TLS index is negative (-(size / 4)), so [fs:[4] + index*4] is its start. */
    TlsSetValue(g_slot[FS_SLOT_TLS], t->tls + tls_size);
    TlsSetValue(g_slot[FS_SLOT_PRCB], prcb);
    TlsSetValue(g_slot[FS_SLOT_IRQL], NULL); /* PASSIVE_LEVEL */
    TlsSetValue(g_slot[FS_SLOT_THREAD], t->kthread);
    TlsSetValue(g_slot[FS_SLOT_DPC], NULL);
    TlsSetValue(g_self, t);
    return t;
}

EmuThread *emu_thread_current(void)
{
    return g_self == TLS_OUT_OF_INDEXES ? NULL : (EmuThread *)TlsGetValue(g_self);
}

unsigned char emu_irql_get(void)
{
    return (unsigned char)(UINT_PTR)TlsGetValue(g_slot[FS_SLOT_IRQL]);
}

void emu_irql_set(unsigned char irql)
{
    TlsSetValue(g_slot[FS_SLOT_IRQL], (void *)(UINT_PTR)irql);
}
