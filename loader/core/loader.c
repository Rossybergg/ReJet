/* rejet.dll: the loader (copied from ReWhacked's, Phase 2).
 *
 * Maps orig Default.xbe over the stub exe's image at its original addresses, points every
 * kernel thunk slot at a logging stub (or a zeroed block for data exports), and runs the
 * entry point on its own thread. Phase 2 stops at the first kernel call or fault and logs
 * it to loader.log next to the exe. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "emu_thread.h"
#include "hle.h"
#include "kernel.h"
#include "perf.h"
#include "settings.h"
#include "ui/setup.h"
#include "version.gen.h"
#include "xboxkrnl_impl.h"
#include "xbox/xbe.h"

static FILE *g_log;
static const XbeTls *g_tls; /* the XBE's TLS directory, once mapped */

/* Per-thread TLS block size as the kernel computes it (matches what XAPI passes to
 * PsCreateSystemThreadEx): template + zero fill rounded to 16, plus the 4-byte pointer slot. */
static unsigned xbe_tls_size(void)
{
    unsigned data = g_tls ? g_tls->raw_data_end - g_tls->raw_data_start + g_tls->zero_fill_size : 0;
    return ((data + 15) & ~15u) + 4;
}

void log_printf(const char *fmt, ...)
{
    va_list ap;
    LONGLONG t0;
    if (!g_log)
        return;
    t0 = perf_now();
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
    perf_end(PERF_LOG, t0);
}

/* Heuristic backtrace: XBE code is compiled with frame-pointer omission, so scan the stack
 * for dwords that point into the XBE right after a call instruction. */
static unsigned g_code_lo, g_code_hi; /* first section .. kernel thunk (start of .rdata); set in map_xbe */

void log_backtrace(const void *stack, unsigned depth)
{
    const unsigned *sp = (const unsigned *)stack;
    unsigned found = 0;
    log_printf("  backtrace:");
    for (unsigned i = 0; i < 1024 && found < depth; i++) {
        unsigned v;
        const unsigned char *r;
        __try {
            v = sp[i];
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (v < g_code_lo || v >= g_code_hi)
            continue;
        r = (const unsigned char *)(UINT_PTR)v;
        if (r[-5] == 0xE8 || (r[-6] == 0xFF && r[-5] == 0x15) || (r[-2] == 0xFF && (r[-1] & 0xF8) == 0xD0) ||
            (r[-3] == 0xFF && (r[-2] & 0x38) == 0x10)) {
            log_printf(" %08X", v);
            found++;
        }
    }
    log_printf("\n");
}

/* Log the first dwords behind a register value if it points at readable memory. */
static void log_memory(const char *reg, DWORD value)
{
    const unsigned *p = (const unsigned *)(UINT_PTR)value;
    unsigned v[4];
    __try {
        v[0] = p[0];
        v[1] = p[1];
        v[2] = p[2];
        v[3] = p[3];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    log_printf("  [%s] %08lX: %08X %08X %08X %08X\n", reg, value, v[0], v[1], v[2], v[3]);
}

static const char *import_name(unsigned ordinal)
{
    for (unsigned i = 0; i < g_kernel_import_count; i++)
        if (g_kernel_imports[i].ordinal == ordinal)
            return g_kernel_imports[i].name;
    return "?";
}

__declspec(noreturn) void kernel_unimplemented(unsigned ordinal, void *caller, void *stack)
{
    const unsigned *args = (const unsigned *)stack + 1; /* skip our return address */
    log_printf("KERNEL %u %s called from %08X, args %08X %08X %08X %08X %08X %08X\n",
               ordinal, import_name(ordinal), (unsigned)(UINT_PTR)caller,
               args[0], args[1], args[2], args[3], args[4], args[5]);
    log_backtrace(stack, 12);
    log_printf("stop: kernel function not implemented\n");
    ExitProcess(3);
}

static void log_exception(EXCEPTION_POINTERS *ep);

/* First-chance handler: only faults in XBE code (or jumps to low/null addresses) are ours.
 * Host DLLs such as graphics drivers raise and catch exceptions internally, so anything
 * else continues to normal SEH; real host crashes reach on_unhandled. */
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    UINT_PTR at = (UINT_PTR)er->ExceptionAddress;
    int xbe = at < g_code_hi; /* includes null and low addresses */
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 1)) {
        static LONG hits;
        if (InterlockedIncrement(&hits) <= 8) {
            log_printf("WATCH: write to %08lX by instruction before %08X (thread %lu)\n", ep->ContextRecord->Dr0,
                       (unsigned)at, GetCurrentThreadId());
            log_backtrace((const void *)(UINT_PTR)ep->ContextRecord->Esp, 12);
        }
        ep->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (!xbe)
        return EXCEPTION_CONTINUE_SEARCH;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
        er->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION) {
        log_exception(ep);
        log_printf("stop: fatal fault in XBE code\n");
        ExitProcess(4);
    }
    return EXCEPTION_CONTINUE_SEARCH; /* game code may use SEH legitimately */
}

static LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep)
{
    log_exception(ep);
    log_printf("stop: unhandled exception in host code\n");
    ExitProcess(5);
}

static void log_exception(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const CONTEXT *c = ep->ContextRecord;
    log_printf("EXCEPTION %08lX at %08X", er->ExceptionCode, (unsigned)(UINT_PTR)er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        log_printf(" (%s %08X)", er->ExceptionInformation[0] ? "write" : "read",
                   (unsigned)er->ExceptionInformation[1]);
    log_printf("\n  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX\n",
               c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    {
        EmuThread *t = emu_thread_current();
        log_printf("  thread %lu, xbox state %p, tls index %u", GetCurrentThreadId(), (void *)t,
                   g_tls && g_tls->index_address ? *(unsigned *)(UINT_PTR)g_tls->index_address : 0u);
        if (t)
            log_printf(", tls %p [%08X %08X %08X %08X]", (void *)t->tls, ((unsigned *)t->tls)[0],
                       ((unsigned *)t->tls)[1], ((unsigned *)t->tls)[2], ((unsigned *)t->tls)[3]);
        log_printf("\n");
        log_backtrace((const void *)(UINT_PTR)c->Esp, 12);
        log_memory("eip", c->Eip);
        log_memory("esp-16", c->Esp - 16);
        log_memory("esp", c->Esp);
        log_memory("esp+16", c->Esp + 16);
        log_memory("esp+32", c->Esp + 32);
        log_memory("eax", c->Eax);
        log_memory("ebx", c->Ebx);
        log_memory("ecx", c->Ecx);
        log_memory("edx", c->Edx);
        log_memory("esi", c->Esi);
        log_memory("edi", c->Edi);
    }
}

/* Hang diagnostics: who owns the loader lock (PEB+0xA0 -> RTL_CRITICAL_SECTION, owner at +0xC),
 * and for every other thread its instruction pointer (module + offset) and an XBE backtrace.
 * Written on timeout (before ExitProcess, which needs the loader lock) and after
 * REJET_HANGDUMP=<seconds>. */
static void log_module_address(const char *what, DWORD addr)
{
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (addr >= g_code_lo && addr < g_code_hi) {
        log_printf(" %s %08lX (xbe)", what, addr);
        return;
    }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(UINT_PTR)addr, &m) && m) {
        char *slash;
        GetModuleFileNameA(m, name, MAX_PATH);
        slash = strrchr(name, '\\');
        log_printf(" %s %s+%lX", what, slash ? slash + 1 : name, addr - (DWORD)(UINT_PTR)m);
        return;
    }
    log_printf(" %s %08lX", what, addr);
}

static void dump_threads(const char *why)
{
    const unsigned char *peb = (const unsigned char *)(UINT_PTR)__readfsdword(0x30);
    const RTL_CRITICAL_SECTION *ll = *(RTL_CRITICAL_SECTION *const *)(peb + 0xA0);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD self = GetCurrentThreadId();
    log_printf("threads (%s): loader lock owner %lu, lock count %ld\n", why,
               (unsigned long)(UINT_PTR)ll->OwningThread, ll->LockCount);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        HANDLE h;
        CONTEXT c;
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self)
            continue;
        h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h)
            continue;
        SuspendThread(h);
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(h, &c)) {
            log_printf("  thread %lu prio %d:", te.th32ThreadID, GetThreadPriority(h));
            log_module_address("eip", c.Eip);
            log_printf(" esp %08lX\n", c.Esp);
            log_backtrace((const void *)(UINT_PTR)c.Esp, 10);
        }
        ResumeThread(h);
        CloseHandle(h);
    }
    CloseHandle(snap);
}

static DWORD WINAPI hangdump_thread(void *arg)
{
    Sleep((DWORD)(UINT_PTR)arg * 1000);
    dump_threads("REJET_HANGDUMP");
    return 0;
}

static unsigned char *read_file(const char *path, DWORD *size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    unsigned char *buf = NULL;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    *size = GetFileSize(h, NULL);
    buf = (unsigned char *)VirtualAlloc(NULL, *size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (buf && (!ReadFile(h, buf, *size, &got, NULL) || got != *size)) {
        VirtualFree(buf, 0, MEM_RELEASE);
        buf = NULL;
    }
    CloseHandle(h);
    return buf;
}

/* The entry point runs on a kernel-created thread, so give it Xbox state and a TLS block
 * initialised the way XapiThreadStartup does for XAPI threads: slot 0 points just past
 * itself, followed by the raw TLS template and zero fill. */
static DWORD WINAPI run_entry(void *entry)
{
    unsigned raw = g_tls ? g_tls->raw_data_end - g_tls->raw_data_start : 0;
    unsigned size = xbe_tls_size();
    EmuThread *t = emu_thread_attach(size);

    if (!t) {
        log_printf("error: cannot attach Xbox state to entry thread\n");
        return 1;
    }
    *(unsigned char **)t->tls = t->tls + 4;
    if (raw)
        memcpy(t->tls + 4, (const void *)(UINT_PTR)g_tls->raw_data_start, raw);
    log_printf("entry: jumping to %08X (tls %u bytes)\n", (unsigned)(UINT_PTR)entry, size);
    ((void(__cdecl *)(void))entry)();
    log_printf("entry: returned\n");
    return 0;
}

/* Windows keeps reading the exe's PE header at run time (CreateThread sizes stacks from it,
 * error 193 otherwise), but the XBE header must sit at the same address. The XBE's RSA
 * signature (0x004-0x103) is unused at run time, so a cut-down PE header lives there:
 * "MZ" over the first two magic bytes, e_lfanew = 0x40, NT headers at 0x40 with no section
 * table and only as many data directories as fit before 0x104 (XBE base_address). */
#define CHIMERA_NT_OFFSET 0x40
#define CHIMERA_LIMIT     0x104

static IMAGE_NT_HEADERS32 g_stub_nt;

static void write_chimera_pe(unsigned char *image)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(image + CHIMERA_NT_OFFSET);
    const unsigned fixed = FIELD_OFFSET(IMAGE_NT_HEADERS32, OptionalHeader.DataDirectory);
    const unsigned dirs = (CHIMERA_LIMIT - CHIMERA_NT_OFFSET - fixed) / sizeof(IMAGE_DATA_DIRECTORY);

    ((IMAGE_DOS_HEADER *)image)->e_magic = IMAGE_DOS_SIGNATURE;
    ((IMAGE_DOS_HEADER *)image)->e_lfanew = CHIMERA_NT_OFFSET;
    memcpy(nt, &g_stub_nt, fixed + dirs * sizeof(IMAGE_DATA_DIRECTORY));
    nt->FileHeader.NumberOfSections = 0;
    nt->FileHeader.SizeOfOptionalHeader =
        (WORD)(FIELD_OFFSET(IMAGE_OPTIONAL_HEADER32, DataDirectory) + dirs * sizeof(IMAGE_DATA_DIRECTORY));
    nt->OptionalHeader.NumberOfRvaAndSizes = dirs;
    log_printf("pe: chimera header, NT at %08X, %u data directories\n",
               (unsigned)(UINT_PTR)nt, dirs);
}

static int map_xbe(unsigned char *image, unsigned image_size, const unsigned char *file, DWORD file_size,
                   void **entry_out)
{
    const XbeHeader *h = (const XbeHeader *)file;
    const XbeSectionHeader *sec;
    unsigned entry, thunk, *slot;
    DWORD old;

    if (file_size < sizeof *h || h->magic != XBE_MAGIC) {
        log_printf("error: not an XBE\n");
        return 0;
    }
    if (h->base_address != (unsigned)(UINT_PTR)image || h->size_of_image > image_size) {
        log_printf("error: XBE wants %08X+%08X, reserved %08X+%08X\n", h->base_address, h->size_of_image,
                   (unsigned)(UINT_PTR)image, image_size);
        return 0;
    }
    entry = h->entry_point_xored ^ XBE_ENTRY_XOR_RETAIL;
    thunk = h->kernel_thunk_xored ^ XBE_THUNK_XOR_RETAIL;
    g_code_lo = h->base_address + h->size_of_headers;
    g_code_hi = thunk;
    log_printf("xbe: base %08X image %08X headers %08X sections %u entry %08X thunk %08X tls %08X stack %08X\n",
               h->base_address, h->size_of_image, h->size_of_headers, h->section_count, entry, thunk,
               h->tls_address, h->stack_size);

    /* The stub's own code and IAT live in this range too; nothing returns into them. */
    if (!VirtualProtect(image, image_size, PAGE_EXECUTE_READWRITE, &old)) {
        log_printf("error: VirtualProtect failed (%lu)\n", GetLastError());
        return 0;
    }
    g_stub_nt = *(const IMAGE_NT_HEADERS32 *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
    memset(image, 0, image_size);
    memcpy(image, file, h->size_of_headers);
    write_chimera_pe(image);

    sec = (const XbeSectionHeader *)(file + (h->section_headers_address - h->base_address));
    for (unsigned i = 0; i < h->section_count; i++, sec++) {
        const char *name = (const char *)file + (sec->name_address - h->base_address);
        if (sec->raw_address + sec->raw_size > file_size ||
            sec->virtual_address + sec->virtual_size > h->base_address + image_size) {
            log_printf("error: section %s out of range\n", name);
            return 0;
        }
        memcpy((unsigned char *)(UINT_PTR)sec->virtual_address, file + sec->raw_address, sec->raw_size);
        log_printf("  section %-9s %08X vsize %08X raw %08X\n", name, sec->virtual_address, sec->virtual_size,
                   sec->raw_size);
    }

    /* Kernel thunks: 0x80000000 | ordinal, zero-terminated. */
    unsigned patched = 0, missing = 0;
    char trace_env[8] = "";
    int trace_all = GetEnvironmentVariableA("REJET_TRACE", trace_env, sizeof trace_env) && trace_env[0] == '1';
    if (trace_all)
        log_printf("trace: logging every implemented kernel call (REJET_TRACE=1)\n");
    for (slot = (unsigned *)(UINT_PTR)thunk; *slot; slot++) {
        unsigned ordinal = *slot & 0x7FFFFFFF;
        const KernelImport *k = NULL;
        for (unsigned i = 0; i < g_kernel_import_count; i++)
            if (g_kernel_imports[i].ordinal == ordinal)
                k = &g_kernel_imports[i];
        if (!k) {
            log_printf("  thunk %08X: ordinal %u has no table entry\n", (unsigned)(UINT_PTR)slot, ordinal);
            missing++;
            continue;
        }
        *slot = (unsigned)(UINT_PTR)(trace_all ? k->traced : k->target);
        patched++;
    }
    log_printf("thunks: %u patched, %u missing\n", patched, missing);

    if (h->tls_address) {
        g_tls = (const XbeTls *)(UINT_PTR)h->tls_address;
        if (g_tls->index_address) /* negative: the block lies below fs:[4] */
            *(int *)(UINT_PTR)g_tls->index_address = -(int)(xbe_tls_size() / 4);
        log_printf("tls: raw %08X-%08X zero fill %u, block %u bytes, index variable at %08X\n",
                   g_tls->raw_data_start, g_tls->raw_data_end, g_tls->zero_fill_size, xbe_tls_size(),
                   g_tls->index_address);
    }
    FlushInstructionCache(GetCurrentProcess(), image, image_size);
    *entry_out = (void *)(UINT_PTR)entry;
    return missing == 0;
}

static int run(void *image_base, unsigned image_size)
{
    char exe_dir[MAX_PATH], log_path[MAX_PATH], xbe_path[MAX_PATH];
    unsigned char *file;
    DWORD file_size = 0;
    void *entry = NULL;
    int argc = 0;
    LPWSTR *argv;
    HANDLE thread;
    unsigned run_seconds = 30;

    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    *strrchr(exe_dir, '\\') = 0;
    snprintf(log_path, sizeof log_path, "%s\\loader.log", exe_dir);
    { /* keep the previous session's log: logs\loader_<its date and time>.log */
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExA(log_path, GetFileExInfoStandard, &a)) {
            char old[MAX_PATH], dir[MAX_PATH];
            FILETIME local;
            SYSTEMTIME t;
            FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
            FileTimeToSystemTime(&local, &t);
            snprintf(dir, sizeof dir, "%s\\logs", exe_dir);
            CreateDirectoryA(dir, NULL);
            snprintf(old, sizeof old, "%s\\loader_%04u%02u%02u_%02u%02u%02u.log", dir, t.wYear, t.wMonth, t.wDay, t.wHour,
                     t.wMinute, t.wSecond);
            MoveFileExA(log_path, old, MOVEFILE_REPLACE_EXISTING);
        }
    }
    g_log = fopen(log_path, "w");
    {
        SYSTEMTIME t;
        OSVERSIONINFOW os = {sizeof os};
        typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
        RtlGetVersionFn rtl = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll"), "RtlGetVersion");
        GetLocalTime(&t);
        if (rtl)
            rtl(&os);
        log_printf("ReJet %s (built %s), session %04u-%02u-%02u %02u:%02u:%02u, Windows %lu.%lu.%lu, image %08X size "
                   "%08X\n", REJET_BUILD, REJET_BUILD_DATE, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                   os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, (unsigned)(UINT_PTR)image_base, image_size);
    }

    settings_load(); /* before the title runs (console settings), and for the game folder */
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2) { /* developers: ReJet.exe <Default.xbe> [seconds, 0 = until closed] */
        WideCharToMultiByte(CP_ACP, 0, argv[1], -1, xbe_path, MAX_PATH, NULL, NULL);
        if (argc >= 3)
            run_seconds = (unsigned)_wtoi(argv[2]);
    }
    else { /* players: the saved game folder, Game\ next to the exe, or the setup screen */
        WCHAR wexe[MAX_PATH], folder[MAX_PATH], *slash;
        GetModuleFileNameW(NULL, wexe, MAX_PATH);
        if ((slash = wcsrchr(wexe, L'\\')) != NULL)
            *slash = 0;
        settings_game_folder(folder, MAX_PATH);
        if (!setup_folder_ok(folder)) {
            swprintf(folder, MAX_PATH, L"%s\\Game", wexe);
            if (setup_folder_ok(folder))
                settings_set_game_folder(folder);
            else if (!setup_run(wexe, folder, MAX_PATH))
                return 0;
        }
        WideCharToMultiByte(CP_ACP, 0, folder, -1, xbe_path, MAX_PATH - 16, NULL, NULL);
        strcat_s(xbe_path, MAX_PATH, "\\default.xbe");
        run_seconds = 0;
    }
    log_printf("xbe: %s\n", xbe_path);

    file = read_file(xbe_path, &file_size);
    if (!file) {
        log_printf("error: cannot read XBE (%lu)\n", GetLastError());
        return 2;
    }
    if (!map_xbe((unsigned char *)image_base, image_size, file, file_size, &entry))
        return 2;
    if (!emu_fs_init())
        return 2;
    if (!hle_apply(file, file_size))
        return 2;
    kernel_ticker_start();
    io_init(xbe_path);
    xc_selftest();

    {
        char env[16] = "";
        if (GetEnvironmentVariableA("REJET_HANGDUMP", env, sizeof env) && atoi(env) > 0)
            CloseHandle(CreateThread(NULL, 0, hangdump_thread, (void *)(UINT_PTR)atoi(env), 0, NULL));
    }
    AddVectoredExceptionHandler(1, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);
    thread = CreateThread(NULL, 1 << 20, run_entry, entry, 0, NULL);
    if (!thread) {
        log_printf("error: CreateThread failed (%lu)\n", GetLastError());
        return 2;
    }
    /* Xbox threads outlive the entry thread; run for a fixed time during bring-up. */
    WaitForSingleObject(thread, INFINITE);
    log_printf("entry thread ended\n");
    if (!run_seconds) /* 0: run until the window is closed (the launcher uses this) */
        Sleep(INFINITE);
    Sleep(run_seconds * 1000);
    log_printf("timeout: stopping after %u s\n", run_seconds);
    dump_threads("timeout");
    return 0;
}

/* Never returns: the stub's code is overwritten by the XBE mapping. */
__declspec(dllexport) __declspec(noreturn) void __stdcall rw_main(void *image_base, unsigned image_size)
{
    int rc = run(image_base, image_size);
    log_printf("exit %d\n", rc);
    ExitProcess(rc);
}
