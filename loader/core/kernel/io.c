/* Io / Nt file system. Xbox object-manager paths are resolved through a symbolic-link
 * table to a device, then to a host folder, and the call is forwarded to Windows' own
 * NtCreateFile & co. in ntdll, so dispositions, sharing and status codes keep exact NT
 * semantics. Only Xbox differences are converted: OBJECT_ATTRIBUTES without a Length,
 * ANSI names, and NtReadFile/NtWriteFile without the trailing Key argument.
 *
 * Devices:
 *   \Device\CdRom0                -> folder holding the XBE (read-only), with overrides\ on top
 *   \Device\Harddisk0\PartitionN  -> %LOCALAPPDATA%\rejet\hdd\PartitionN */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "kernel.h"
#include "perf.h"
#include "xboxkrnl_impl.h"
#include <winioctl.h>

#define MAX_LINKS 64
#define OB_DOS_DEVICES_DIRECTORY ((HANDLE)(LONG_PTR)-3) /* Xbox pseudo-handle for \??\ */
#define STATUS_ACCESS_DENIED_X        ((NTSTATUS)0xC0000022L)
#define STATUS_OBJECT_PATH_NOT_FOUND_X ((NTSTATUS)0xC000003AL)

typedef struct WUNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} WUNICODE_STRING;

typedef struct WOBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    WUNICODE_STRING *ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} WOBJECT_ATTRIBUTES;

#define OBJ_CASE_INSENSITIVE_W 0x40

typedef NTSTATUS(WINAPI *NtCreateFile_t)(PHANDLE, ACCESS_MASK, WOBJECT_ATTRIBUTES *, PIO_STATUS_BLOCK_X,
                                         PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS(WINAPI *NtReadWrite_t)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK_X, PVOID, ULONG,
                                        PLARGE_INTEGER, PULONG);
typedef NTSTATUS(WINAPI *NtQuerySetInfo_t)(HANDLE, PIO_STATUS_BLOCK_X, PVOID, ULONG, ULONG);
typedef NTSTATUS(WINAPI *NtFlush_t)(HANDLE, PIO_STATUS_BLOCK_X);

static NtCreateFile_t p_NtCreateFile;
static NtReadWrite_t p_NtReadFile, p_NtWriteFile;
static NtQuerySetInfo_t p_NtQueryInformationFile, p_NtSetInformationFile, p_NtQueryVolumeInformationFile;
static NtFlush_t p_NtFlushBuffersFile;

typedef struct Link {
    char name[MAX_PATH];   /* e.g. \??\D: */
    char target[MAX_PATH]; /* e.g. \Device\CdRom0 */
} Link;

static Link g_links[MAX_LINKS];
static unsigned g_link_count;

/* Which emulated device each open handle belongs to (volume geometry, raw partition IOCTLs). */
enum { DEV_HDD = 1, DEV_DISC = 2, DEV_RAW = 3 };
#define MAX_HANDLES 1024
static struct {
    HANDLE h;
    int dev;
    int partition; /* DEV_RAW: partition number */
} g_handles[MAX_HANDLES];
static unsigned g_handle_count;
static CRITICAL_SECTION g_io_lock;
static WCHAR g_disc_dir[MAX_PATH];
static WCHAR g_override_dir[MAX_PATH];
static WCHAR g_hdd_dir[MAX_PATH];

static void add_link(const char *name, const char *target)
{
    EnterCriticalSection(&g_io_lock);
    for (unsigned i = 0; i < g_link_count; i++) {
        if (!_stricmp(g_links[i].name, name)) {
            snprintf(g_links[i].target, sizeof g_links[i].target, "%s", target);
            LeaveCriticalSection(&g_io_lock);
            return;
        }
    }
    if (g_link_count < MAX_LINKS) {
        snprintf(g_links[g_link_count].name, sizeof g_links[0].name, "%s", name);
        snprintf(g_links[g_link_count].target, sizeof g_links[0].target, "%s", target);
        g_link_count++;
    }
    LeaveCriticalSection(&g_io_lock);
}

void io_init(const char *xbe_path)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    WCHAR local[MAX_PATH];
    WCHAR *slash;

    InitializeCriticalSection(&g_io_lock);
    p_NtCreateFile = (NtCreateFile_t)GetProcAddress(nt, "NtCreateFile");
    p_NtReadFile = (NtReadWrite_t)GetProcAddress(nt, "NtReadFile");
    p_NtWriteFile = (NtReadWrite_t)GetProcAddress(nt, "NtWriteFile");
    p_NtQueryInformationFile = (NtQuerySetInfo_t)GetProcAddress(nt, "NtQueryInformationFile");
    p_NtSetInformationFile = (NtQuerySetInfo_t)GetProcAddress(nt, "NtSetInformationFile");
    p_NtQueryVolumeInformationFile = (NtQuerySetInfo_t)GetProcAddress(nt, "NtQueryVolumeInformationFile");
    p_NtFlushBuffersFile = (NtFlush_t)GetProcAddress(nt, "NtFlushBuffersFile");

    MultiByteToWideChar(CP_ACP, 0, xbe_path, -1, local, MAX_PATH);
    GetFullPathNameW(local, MAX_PATH, g_disc_dir, NULL); /* separate buffers: no aliasing */
    slash = wcsrchr(g_disc_dir, L'\\');
    if (slash)
        *slash = 0;
    swprintf(g_override_dir, MAX_PATH, L"%ls\\overrides", g_disc_dir);
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    wcscat_s(local, MAX_PATH, L"\\rejet");
    CreateDirectoryW(local, NULL);
    swprintf(g_hdd_dir, MAX_PATH, L"%ls\\hdd", local);
    CreateDirectoryW(g_hdd_dir, NULL);

    /* The kernel provides these before the title runs. */
    add_link("\\??\\D:", "\\Device\\CdRom0");
    log_printf("io: D: = %ls (overrides: %ls), hdd = %ls\n", g_disc_dir, g_override_dir, g_hdd_dir);
}

/* Retail 8 GB HDD layout: partition sizes in bytes. */
static ULONGLONG partition_size(int part)
{
    switch (part) {
    case 0: return 0x80000;        /* config area: 512 KB */
    case 1: return 0x131F00000ull; /* E: data, ~4.9 GB */
    case 2: return 0x1F400000;     /* C: system, 500 MB */
    default: return 0x2EE00000;    /* X/Y/Z cache, 750 MB each */
    }
}

/* Create a raw partition image as a sparse file of the partition's size (no real space used). */
static void ensure_raw(const WCHAR *path, int part)
{
    HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_NEW,
                           0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD got;
        LARGE_INTEGER size;
        DeviceIoControl(f, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL);
        size.QuadPart = (LONGLONG)partition_size(part);
        SetFilePointerEx(f, size, NULL, FILE_BEGIN);
        SetEndOfFile(f);
        CloseHandle(f);
    }
}

/* Drop "." components and resolve ".." in place (the Xbox file system accepts them, NT paths
 * do not). A trailing backslash is kept: it marks "the directory itself". */
static void normalize(char *path)
{
    char out[MAX_PATH * 2];
    size_t o = 0, len = strlen(path);
    int trailing = len > 0 && path[len - 1] == '\\';
    const char *p = path;

    while (*p) {
        const char *seg;
        size_t n;
        while (*p == '\\')
            p++;
        seg = p;
        while (*p && *p != '\\')
            p++;
        n = (size_t)(p - seg);
        if (n == 0 || (n == 1 && seg[0] == '.'))
            continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '\\')
                o--;
            if (o > 0)
                o--;
            continue;
        }
        if (o + n + 2 >= sizeof out)
            break;
        out[o++] = '\\';
        memcpy(out + o, seg, n);
        o += n;
    }
    if (trailing || o == 0)
        out[o++] = '\\';
    out[o] = 0;
    memcpy(path, out, o + 1);
}

/* Resolve an Xbox object path to a host Win32 path. Returns 0 if unknown; sets the device
 * kind (DEV_*) and, for raw partitions, the partition number. */
static int resolve(const char *xpath, WCHAR *out, size_t out_len, int *dev, int *partition)
{
    char path[MAX_PATH * 2];
    int guard = 0;

    snprintf(path, sizeof path, "%s", xpath);
    /* Bare drive paths ("D:\x") are \??\ paths. */
    if (path[0] && path[1] == ':') {
        snprintf(path, sizeof path, "\\??\\%s", xpath);
    }
    normalize(path);
    for (;;) {
        unsigned best = MAX_LINKS;
        size_t best_len = 0;
        EnterCriticalSection(&g_io_lock);
        for (unsigned i = 0; i < g_link_count; i++) {
            size_t n = strlen(g_links[i].name);
            if (n > best_len && !_strnicmp(path, g_links[i].name, n) && (path[n] == '\\' || path[n] == 0)) {
                best = i;
                best_len = n;
            }
        }
        if (best != MAX_LINKS) {
            char rest[MAX_PATH * 2];
            snprintf(rest, sizeof rest, "%s", path + best_len);
            snprintf(path, sizeof path, "%s%s", g_links[best].target, rest);
        }
        LeaveCriticalSection(&g_io_lock);
        if (best == MAX_LINKS || ++guard > 8)
            break;
    }

    *dev = DEV_HDD;
    *partition = 0;
    if (!_strnicmp(path, "\\Device\\CdRom0", 14) && (path[14] == '\\' || !path[14])) {
        *dev = DEV_DISC;
        swprintf(out, out_len, L"%ls%hs", g_disc_dir, path + 14);
        return 1;
    }
    if (!_strnicmp(path, "\\Device\\Harddisk0\\Partition", 27)) {
        unsigned part = (unsigned)atoi(path + 27);
        const char *rest = strchr(path + 27, '\\');
        if (!rest) {
            /* Raw partition (no trailing backslash): a host file absorbs sector I/O such as
             * XAPI's FATX format; Partition0 holds the cache-partition table at 0x800.
             * The partition's files live in the PartitionN folder. */
            *dev = DEV_RAW;
            *partition = (int)part;
            swprintf(out, out_len, L"%ls\\Partition%u.raw", g_hdd_dir, part);
            ensure_raw(out, (int)part);
            return 1;
        }
        {
            WCHAR dir[MAX_PATH];
            swprintf(dir, MAX_PATH, L"%ls\\Partition%u", g_hdd_dir, part);
            CreateDirectoryW(dir, NULL);
            swprintf(out, out_len, L"%ls%hs", dir, rest);
        }
        return 1;
    }
    log_printf("io: cannot resolve '%s' (from '%s')\n", path, xpath);
    return 0;
}

static void remember_handle(HANDLE h, int dev, int partition)
{
    EnterCriticalSection(&g_io_lock);
    if (g_handle_count < MAX_HANDLES) {
        g_handles[g_handle_count].h = h;
        g_handles[g_handle_count].dev = dev;
        g_handles[g_handle_count].partition = partition;
        g_handle_count++;
    }
    LeaveCriticalSection(&g_io_lock);
}

static int handle_device(HANDLE h, int *partition)
{
    int dev = 0;
    EnterCriticalSection(&g_io_lock);
    for (unsigned i = 0; i < g_handle_count; i++) {
        if (g_handles[i].h == h) {
            dev = g_handles[i].dev;
            if (partition)
                *partition = g_handles[i].partition;
        }
    }
    LeaveCriticalSection(&g_io_lock);
    return dev;
}

static void forget_link_handle(HANDLE h);

void io_forget_handle(HANDLE h)
{
    EnterCriticalSection(&g_io_lock);
    forget_link_handle(h);
    for (unsigned i = 0; i < g_handle_count; i++) {
        if (g_handles[i].h == h) {
            KTRACE("NtClose(file handle %p)\n", h);
            g_handles[i] = g_handles[--g_handle_count];
            break;
        }
    }
    LeaveCriticalSection(&g_io_lock);
}

static NTSTATUS open_host(PHANDLE h, ACCESS_MASK access, HANDLE root, const WCHAR *path, PIO_STATUS_BLOCK_X iosb,
                          PLARGE_INTEGER alloc, ULONG attrs, ULONG share, ULONG disposition, ULONG options)
{
    WCHAR ntpath[MAX_PATH + 8];
    WUNICODE_STRING us;
    WOBJECT_ATTRIBUTES oa;

    if (root) /* relative to an open host handle */
        swprintf(ntpath, MAX_PATH + 8, L"%ls", path);
    else
        swprintf(ntpath, MAX_PATH + 8, L"\\??\\%ls", path);
    us.Buffer = ntpath;
    us.Length = (USHORT)(wcslen(ntpath) * sizeof(WCHAR));
    us.MaximumLength = us.Length + sizeof(WCHAR);
    memset(&oa, 0, sizeof oa);
    oa.Length = sizeof oa;
    oa.RootDirectory = root;
    oa.ObjectName = &us;
    oa.Attributes = OBJ_CASE_INSENSITIVE_W;
    /* The Xbox lets any handle query basic file information; Windows needs read-attributes.
     * Sharing is not enforced the Windows way on the Xbox: the game's first-time install opens
     * Z:\MainInstallDone.dat for reading (share read), never closes that handle, and later
     * rewrites the file with exclusive access. Windows refuses that (sharing violation), the
     * game shows its disc-error screen and stops. The game is the only user of these files, so
     * every host open allows full sharing. (The game's own share flags are still logged.) */
    (void)share;
    /* JSRF opens its cache and disc files unbuffered (CreateFile flag 0x20000000) with buffers
     * aligned for the Xbox's 2 KB/512-byte sectors. Windows checks unbuffered transfers against
     * the host disk's sector size and fails them otherwise, so host opens are always buffered.
     * Overlapped (asynchronous) I/O is kept: FILE_SYNCHRONOUS_IO_* comes from the game as is. */
    options &= ~0x00000008u; /* FILE_NO_INTERMEDIATE_BUFFERING */
    return p_NtCreateFile(h, access | FILE_READ_ATTRIBUTES, &oa, iosb, alloc, attrs,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition, options, NULL, 0);
}

static NTSTATUS xbox_open(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, PXOBJECT_ATTRIBUTES ObjectAttributes,
                          PIO_STATUS_BLOCK_X IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                          ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, const char *api)
{
    char xpath[MAX_PATH * 2];
    WCHAR host[MAX_PATH * 2];
    int dev, partition;
    NTSTATUS st;
    const ACCESS_MASK writes = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA |
                               DELETE | GENERIC_WRITE | GENERIC_ALL;

    snprintf(xpath, sizeof xpath, "%.*s", ObjectAttributes->ObjectName->Length, ObjectAttributes->ObjectName->Buffer);
    if (ObjectAttributes->RootDirectory == OB_DOS_DEVICES_DIRECTORY) {
        /* XAPI passes (HANDLE)-3 for names relative to \??\ (e.g. "D:\file"). */
        char full[MAX_PATH * 2];
        snprintf(full, sizeof full, "\\??\\%s", xpath);
        snprintf(xpath, sizeof xpath, "%s", full);
    }
    else if (ObjectAttributes->RootDirectory) {
        /* Relative open: the root is a host handle, so Windows resolves it natively. */
        MultiByteToWideChar(CP_ACP, 0, xpath, -1, host, MAX_PATH * 2);
        st = open_host(FileHandle, DesiredAccess, ObjectAttributes->RootDirectory, host, IoStatusBlock,
                       AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions);
        KTRACE("%s(root %p, '%s') -> %08lX\n", api, ObjectAttributes->RootDirectory, xpath, st);
        return st;
    }
    if (!resolve(xpath, host, MAX_PATH * 2, &dev, &partition))
        return STATUS_OBJECT_PATH_NOT_FOUND_X;

    if (dev == DEV_DISC) {
        if ((DesiredAccess & writes) || CreateDisposition != 1 /* FILE_OPEN */) {
            KTRACE("%s('%s') denied: disc is read-only\n", api, xpath);
            return STATUS_ACCESS_DENIED_X;
        }
        static int has_overrides = -1; /* checked once: most installs have no overrides folder */
        if (has_overrides < 0)
            has_overrides = GetFileAttributesW(g_override_dir) != INVALID_FILE_ATTRIBUTES;
        if (has_overrides) { /* asset overrides: overrides\<path on disc> wins if present */
            WCHAR ov[MAX_PATH * 2];
            swprintf(ov, MAX_PATH * 2, L"%ls%ls", g_override_dir, host + wcslen(g_disc_dir));
            if (GetFileAttributesW(ov) != INVALID_FILE_ATTRIBUTES)
                wcscpy(host, ov);
        }
    }
    st = open_host(FileHandle, DesiredAccess, NULL, host, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess,
                   CreateDisposition, CreateOptions);
    KTRACE("%s('%s' -> %ls, access %08lX, share %lu, disp %lu, opt %08lX) -> %08lX handle %p\n", api, xpath, host,
           DesiredAccess, ShareAccess, CreateDisposition, CreateOptions, st, st >= 0 ? *FileHandle : NULL);
    if (st == (NTSTATUS)0xC0000034 && dev != DEV_DISC) { /* not found on the hard disk: does Windows see it? */
        DWORD attrs = GetFileAttributesW(host), err = GetLastError();
        log_printf("io: %ls not found by NtCreateFile; GetFileAttributes %08lX (error %lu), %u characters\n", host,
                   attrs, attrs == INVALID_FILE_ATTRIBUTES ? err : 0, (unsigned)wcslen(host));
    }
    if (st >= 0)
        remember_handle(*FileHandle, dev, partition);
    return st;
}

NTSTATUS XBAPI xbox_NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, PXOBJECT_ATTRIBUTES ObjectAttributes,
                                 PIO_STATUS_BLOCK_X IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                 ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions)
{
    LONGLONG t0 = perf_now();
    NTSTATUS st = xbox_open(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize,
                            FileAttributes, ShareAccess, CreateDisposition, CreateOptions, "NtCreateFile");
    perf_end(PERF_IO, t0);
    return st;
}

NTSTATUS XBAPI xbox_NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, PXOBJECT_ATTRIBUTES ObjectAttributes,
                               PIO_STATUS_BLOCK_X IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions)
{
    LONGLONG t0 = perf_now();
    NTSTATUS st = xbox_open(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, NULL, 0, ShareAccess,
                            1 /* FILE_OPEN */, OpenOptions, "NtOpenFile");
    perf_end(PERF_IO, t0);
    return st;
}

NTSTATUS XBAPI xbox_NtReadFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                               PIO_STATUS_BLOCK_X IoStatusBlock, PVOID Buffer, ULONG Length,
                               PLARGE_INTEGER ByteOffset)
{
    return p_NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, NULL);
}

NTSTATUS XBAPI xbox_NtWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                PIO_STATUS_BLOCK_X IoStatusBlock, PVOID Buffer, ULONG Length,
                                PLARGE_INTEGER ByteOffset)
{
    return p_NtWriteFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, NULL);
}

/* XAPI's ReadFileEx/WriteFileEx pass this kernel export as the APC routine and the caller's
 * completion routine as its context. The reads go to host NtReadFile, so Windows calls it
 * (an APC on the issuing Xbox thread, in an alertable wait) with the host IO_STATUS_BLOCK,
 * which is the start of the caller's OVERLAPPED. JSRF's engine file reader (0x17B034) uses it.
 * Like the Xbox kernel: call the completion routine with the Win32 error, the bytes moved and
 * the OVERLAPPED. */
typedef VOID(XBAPI *XOVERLAPPED_COMPLETION_ROUTINE)(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered,
                                                    void *lpOverlapped);

VOID XBAPI xbox_NtUserIoApcDispatcher(PVOID ApcContext, PIO_STATUS_BLOCK_X IoStatusBlock, ULONG Reserved)
{
    XOVERLAPPED_COMPLETION_ROUTINE done = (XOVERLAPPED_COMPLETION_ROUTINE)ApcContext;
    DWORD err = IoStatusBlock->Status >= 0 ? 0 : xbox_RtlNtStatusToDosError(IoStatusBlock->Status);
    (void)Reserved;
    KTRACE("NtUserIoApcDispatcher(routine %p, status %08lX, %lu bytes)\n", ApcContext, IoStatusBlock->Status,
           (unsigned long)IoStatusBlock->Information);
    if (done)
        done(err, (DWORD)IoStatusBlock->Information, IoStatusBlock);
}

/* Information classes whose structures are identical on Xbox and Windows (no file names). */
static int same_layout_file_class(ULONG c)
{
    switch (c) {
    case 4:  /* FileBasicInformation */
    case 5:  /* FileStandardInformation */
    case 6:  /* FileInternalInformation */
    case 7:  /* FileEaInformation */
    case 8:  /* FileAccessInformation */
    case 13: /* FileDispositionInformation */
    case 14: /* FilePositionInformation */
    case 16: /* FileModeInformation */
    case 17: /* FileAlignmentInformation */
    case 19: /* FileAllocationInformation */
    case 20: /* FileEndOfFileInformation */
    case 34: /* FileNetworkOpenInformation */
        return 1;
    default:
        return 0;
    }
}

NTSTATUS XBAPI xbox_NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK_X IoStatusBlock,
                                           PVOID FileInformation, ULONG Length, ULONG FileInformationClass)
{
    if (!same_layout_file_class(FileInformationClass)) {
        log_printf("io: NtQueryInformationFile class %lu not supported yet\n", FileInformationClass);
        return STATUS_INVALID_PARAMETER_X;
    }
    return p_NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
}

/* FileRenameInformation (10). The Xbox structure names the target with an ANSI object string
 * (an Xbox path, absolute or relative to \??\); Windows wants a UTF-16 NT path. JSRF's Z:
 * cache writes "<name>~" and renames it to "<name>" when the copy is complete. */
typedef struct XFILE_RENAME_INFORMATION {
    BOOLEAN ReplaceIfExists;
    HANDLE RootDirectory;
    USHORT Length, MaximumLength; /* OBJECT_STRING */
    PCHAR Buffer;
} XFILE_RENAME_INFORMATION;

typedef struct WFILE_RENAME_INFORMATION {
    BOOLEAN ReplaceIfExists;
    HANDLE RootDirectory;
    ULONG FileNameLength;
    WCHAR FileName[MAX_PATH * 2 + 8];
} WFILE_RENAME_INFORMATION;

static NTSTATUS rename_file(HANDLE h, PIO_STATUS_BLOCK_X iosb, const XFILE_RENAME_INFORMATION *x)
{
    char xpath[MAX_PATH * 2];
    WCHAR host[MAX_PATH * 2];
    WFILE_RENAME_INFORMATION w;
    int dev, partition;
    NTSTATUS st;

    snprintf(xpath, sizeof xpath, "%.*s", x->Length, x->Buffer);
    if (x->RootDirectory && x->RootDirectory != OB_DOS_DEVICES_DIRECTORY) {
        log_printf("io: rename to '%s' relative to a handle is not supported yet\n", xpath);
        return STATUS_INVALID_PARAMETER_X;
    }
    if (x->RootDirectory == OB_DOS_DEVICES_DIRECTORY) {
        char full[MAX_PATH * 2];
        snprintf(full, sizeof full, "\\??\\%s", xpath);
        snprintf(xpath, sizeof xpath, "%s", full);
    }
    if (!resolve(xpath, host, MAX_PATH * 2, &dev, &partition) || dev == DEV_DISC)
        return dev == DEV_DISC ? STATUS_ACCESS_DENIED_X : STATUS_OBJECT_PATH_NOT_FOUND_X;
    memset(&w, 0, sizeof w);
    w.ReplaceIfExists = x->ReplaceIfExists;
    swprintf(w.FileName, MAX_PATH * 2 + 8, L"\\??\\%ls", host);
    w.FileNameLength = (ULONG)(wcslen(w.FileName) * sizeof(WCHAR));
    st = p_NtSetInformationFile(h, iosb, &w, (ULONG)(FIELD_OFFSET(WFILE_RENAME_INFORMATION, FileName) + w.FileNameLength),
                                10);
    KTRACE("NtSetInformationFile(%p, rename -> '%s' = %ls, replace %d) -> %08lX\n", h, xpath, host,
           x->ReplaceIfExists, st);
    return st;
}

NTSTATUS XBAPI xbox_NtSetInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK_X IoStatusBlock,
                                         PVOID FileInformation, ULONG Length, ULONG FileInformationClass)
{
    if (FileInformationClass == 10 && Length >= sizeof(XFILE_RENAME_INFORMATION))
        return rename_file(FileHandle, IoStatusBlock, (const XFILE_RENAME_INFORMATION *)FileInformation);
    if (!same_layout_file_class(FileInformationClass)) {
        log_printf("io: NtSetInformationFile class %lu not supported yet\n", FileInformationClass);
        return STATUS_INVALID_PARAMETER_X;
    }
    return p_NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
}

NTSTATUS XBAPI xbox_NtQueryVolumeInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK_X IoStatusBlock,
                                                 PVOID FsInformation, ULONG Length, ULONG FsInformationClass)
{
    NTSTATUS st;
    if (FsInformationClass != 3 /* FileFsSizeInformation */ && FsInformationClass != 4 /* FileFsDeviceInformation */) {
        log_printf("io: NtQueryVolumeInformationFile class %lu not supported yet\n", FsInformationClass);
        return STATUS_INVALID_PARAMETER_X;
    }
    st = p_NtQueryVolumeInformationFile(FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass);
    if (st >= 0 && FsInformationClass == 3 && Length >= sizeof(XFILE_FS_SIZE_INFORMATION)) {
        /* Report Xbox geometry: XAPI requires 16 KB FATX clusters on the HDD (else it boots
         * to the dashboard); the disc has 2 KB sectors. Sizes are converted from the host's. */
        XFILE_FS_SIZE_INFORMATION *fs = (XFILE_FS_SIZE_INFORMATION *)FsInformation;
        ULONGLONG unit = (ULONGLONG)fs->SectorsPerAllocationUnit * fs->BytesPerSector;
        ULONGLONG total = fs->TotalAllocationUnits.QuadPart * unit, avail = fs->AvailableAllocationUnits.QuadPart * unit;
        if (handle_device(FileHandle, NULL) == DEV_DISC) {
            fs->BytesPerSector = 2048;
            fs->SectorsPerAllocationUnit = 1;
        }
        else {
            fs->BytesPerSector = 512;
            fs->SectorsPerAllocationUnit = 32;
        }
        unit = (ULONGLONG)fs->SectorsPerAllocationUnit * fs->BytesPerSector;
        fs->TotalAllocationUnits.QuadPart = (LONGLONG)(total / unit);
        fs->AvailableAllocationUnits.QuadPart = (LONGLONG)(avail / unit);
    }
    return st;
}

NTSTATUS XBAPI xbox_NtFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK_X IoStatusBlock)
{
    return p_NtFlushBuffersFile(FileHandle, IoStatusBlock);
}

typedef NTSTATUS(WINAPI *NtQueryDirectoryFile_t)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK_X, PVOID, ULONG, ULONG,
                                                 BOOLEAN, WUNICODE_STRING *, BOOLEAN);

/* Host FILE_DIRECTORY_INFORMATION: same fixed part as the Xbox one, but a WCHAR name. */
typedef struct WFILE_DIRECTORY_INFORMATION {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    LARGE_INTEGER EndOfFile, AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength; /* bytes */
    WCHAR FileName[1];
} WFILE_DIRECTORY_INFORMATION;

/* Xbox returns one entry per call with an ANSI name; forward in single-entry mode. */
NTSTATUS XBAPI xbox_NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                         PIO_STATUS_BLOCK_X IoStatusBlock, PVOID FileInformation, ULONG Length,
                                         ULONG FileInformationClass, PXANSI_STRING FileMask, BOOLEAN RestartScan)
{
    static NtQueryDirectoryFile_t query;
    union {
        WFILE_DIRECTORY_INFORMATION info;
        BYTE raw[sizeof(WFILE_DIRECTORY_INFORMATION) + MAX_PATH * sizeof(WCHAR)];
    } host;
    WCHAR mask[MAX_PATH];
    WUNICODE_STRING us, *pus = NULL;
    NTSTATUS st;
    XFILE_DIRECTORY_INFORMATION *out = (XFILE_DIRECTORY_INFORMATION *)FileInformation;
    ULONG fixed = FIELD_OFFSET(XFILE_DIRECTORY_INFORMATION, FileName), n;

    if (!query)
        query = (NtQueryDirectoryFile_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFile");
    if (FileInformationClass != 1 /* FileDirectoryInformation */) {
        log_printf("io: NtQueryDirectoryFile class %lu not supported yet\n", FileInformationClass);
        return STATUS_INVALID_PARAMETER_X;
    }
    if (FileMask && FileMask->Buffer && FileMask->Length) {
        int len = MultiByteToWideChar(CP_ACP, 0, FileMask->Buffer, FileMask->Length, mask, MAX_PATH);
        us.Buffer = mask;
        us.Length = (USHORT)(len * sizeof(WCHAR));
        us.MaximumLength = us.Length;
        pus = &us;
    }
    /* FATX listings have no "." / ".." entries; NTFS ones do, and the game's recursive
     * installer would descend into them forever. Skip them. */
    for (;;) {
        st = query(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, &host, sizeof host, 1, TRUE, pus,
                   RestartScan);
        if (st < 0)
            return st;
        RestartScan = FALSE;
        n = host.info.FileNameLength / sizeof(WCHAR);
        if (!((n == 1 && host.info.FileName[0] == L'.') ||
              (n == 2 && host.info.FileName[0] == L'.' && host.info.FileName[1] == L'.')))
            break;
    }
    if (Length < fixed + n)
        return (NTSTATUS)0x80000005L; /* STATUS_BUFFER_OVERFLOW */
    memcpy(out, &host.info, fixed);
    out->NextEntryOffset = 0;
    out->FileNameLength = (ULONG)WideCharToMultiByte(CP_ACP, 0, host.info.FileName, (int)n, out->FileName,
                                                     (int)(Length - fixed), NULL, NULL);
    if (IoStatusBlock)
        IoStatusBlock->Information = fixed + out->FileNameLength;
    return st;
}

#define IOCTL_DISK_GET_DRIVE_GEOMETRY_X  0x00070000
#define IOCTL_DISK_GET_PARTITION_INFO_X  0x00074004
#define STATUS_INVALID_DEVICE_REQUEST_X  ((NTSTATUS)0xC0000010L)
#define STATUS_BUFFER_TOO_SMALL_X        ((NTSTATUS)0xC0000023L)

NTSTATUS XBAPI xbox_NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                          PIO_STATUS_BLOCK_X IoStatusBlock, ULONG IoControlCode, PVOID InputBuffer,
                                          ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength)
{
    int part = 0;
    int dev = handle_device(FileHandle, &part);
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST_X;
    ULONG info = 0;

    (void)ApcRoutine;
    (void)ApcContext;
    (void)InputBuffer;
    (void)InputBufferLength;
    if (dev == DEV_RAW && IoControlCode == IOCTL_DISK_GET_DRIVE_GEOMETRY_X) {
        XDISK_GEOMETRY *g = (XDISK_GEOMETRY *)OutputBuffer;
        if (OutputBufferLength < sizeof *g) {
            st = STATUS_BUFFER_TOO_SMALL_X;
        }
        else {
            g->Cylinders.QuadPart = (LONGLONG)(partition_size(part) / 512);
            g->MediaType = 12; /* FixedMedia */
            g->TracksPerCylinder = 1;
            g->SectorsPerTrack = 1;
            g->BytesPerSector = 512;
            info = sizeof *g;
            st = STATUS_SUCCESS;
        }
    }
    else if (dev == DEV_RAW && IoControlCode == IOCTL_DISK_GET_PARTITION_INFO_X) {
        XPARTITION_INFORMATION *p = (XPARTITION_INFORMATION *)OutputBuffer;
        if (OutputBufferLength < sizeof *p) {
            st = STATUS_BUFFER_TOO_SMALL_X;
        }
        else {
            memset(p, 0, sizeof *p);
            p->PartitionLength.QuadPart = (LONGLONG)partition_size(part);
            p->PartitionNumber = (ULONG)part;
            p->RecognizedPartition = TRUE;
            info = sizeof *p;
            st = STATUS_SUCCESS;
        }
    }
    else {
        log_printf("io: NtDeviceIoControlFile(%p, %08lX) not supported (device %d)\n", FileHandle, IoControlCode, dev);
    }
    if (IoStatusBlock) {
        IoStatusBlock->Status = st;
        IoStatusBlock->Information = info;
    }
    if (Event && st >= 0)
        SetEvent(Event);
    return st;
}

/* Symbolic-link handles: a dummy host event (so NtClose works) mapped to a link name. */
#define MAX_LINK_HANDLES 64
static struct {
    HANDLE h;
    char name[MAX_PATH];
} g_link_handles[MAX_LINK_HANDLES];

NTSTATUS XBAPI xbox_NtOpenSymbolicLinkObject(PHANDLE LinkHandle, PXOBJECT_ATTRIBUTES ObjectAttributes)
{
    char name[MAX_PATH];
    NTSTATUS st = STATUS_OBJECT_NAME_NOT_FOUND_X;
    snprintf(name, sizeof name, "%.*s", ObjectAttributes->ObjectName->Length, ObjectAttributes->ObjectName->Buffer);
    EnterCriticalSection(&g_io_lock);
    for (unsigned i = 0; i < g_link_count && st < 0; i++) {
        if (_stricmp(g_links[i].name, name))
            continue;
        for (unsigned j = 0; j < MAX_LINK_HANDLES; j++) {
            if (!g_link_handles[j].h) {
                g_link_handles[j].h = CreateEventW(NULL, TRUE, FALSE, NULL);
                snprintf(g_link_handles[j].name, MAX_PATH, "%s", name);
                *LinkHandle = g_link_handles[j].h;
                st = STATUS_SUCCESS;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_io_lock);
    KTRACE("NtOpenSymbolicLinkObject('%s') -> %08lX\n", name, st);
    return st;
}

NTSTATUS XBAPI xbox_NtQuerySymbolicLinkObject(HANDLE LinkHandle, PXANSI_STRING LinkTarget, PULONG ReturnedLength)
{
    NTSTATUS st = STATUS_INVALID_HANDLE;
    EnterCriticalSection(&g_io_lock);
    for (unsigned j = 0; j < MAX_LINK_HANDLES; j++) {
        if (g_link_handles[j].h != LinkHandle)
            continue;
        for (unsigned i = 0; i < g_link_count; i++) {
            if (!_stricmp(g_links[i].name, g_link_handles[j].name)) {
                USHORT n = (USHORT)strlen(g_links[i].target);
                if (ReturnedLength)
                    *ReturnedLength = n;
                if (n > LinkTarget->MaximumLength) {
                    st = STATUS_BUFFER_TOO_SMALL_X;
                }
                else {
                    memcpy(LinkTarget->Buffer, g_links[i].target, n);
                    LinkTarget->Length = n;
                    st = STATUS_SUCCESS;
                }
                KTRACE("NtQuerySymbolicLinkObject('%s') -> '%s'\n", g_links[i].name, g_links[i].target);
            }
        }
    }
    LeaveCriticalSection(&g_io_lock);
    return st;
}

/* Called from NtClose: forget a symbolic-link handle. */
static void forget_link_handle(HANDLE h)
{
    for (unsigned j = 0; j < MAX_LINK_HANDLES; j++)
        if (g_link_handles[j].h == h)
            g_link_handles[j].h = NULL;
}

#define FSCTL_DISMOUNT_VOLUME_X 0x00090020

NTSTATUS XBAPI xbox_NtFsControlFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                    PIO_STATUS_BLOCK_X IoStatusBlock, ULONG FsControlCode, PVOID InputBuffer,
                                    ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength)
{
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST_X;
    (void)ApcRoutine;
    (void)ApcContext;
    (void)InputBuffer;
    (void)InputBufferLength;
    (void)OutputBuffer;
    (void)OutputBufferLength;
    if (FsControlCode == FSCTL_DISMOUNT_VOLUME_X) {
        /* Host folders need no remount. TODO: a format of a cache partition should also
         * empty its PartitionN folder. */
        st = STATUS_SUCCESS;
    }
    else {
        log_printf("io: NtFsControlFile(%p, %08lX) not supported\n", FileHandle, FsControlCode);
    }
    if (IoStatusBlock) {
        IoStatusBlock->Status = st;
        IoStatusBlock->Information = 0;
    }
    if (Event && st >= 0)
        SetEvent(Event);
    return st;
}

NTSTATUS XBAPI xbox_IoCreateSymbolicLink(PXANSI_STRING SymbolicLinkName, PXANSI_STRING DeviceName)
{
    char name[MAX_PATH], target[MAX_PATH];
    snprintf(name, sizeof name, "%.*s", SymbolicLinkName->Length, SymbolicLinkName->Buffer);
    snprintf(target, sizeof target, "%.*s", DeviceName->Length, DeviceName->Buffer);
    add_link(name, target);
    KTRACE("IoCreateSymbolicLink('%s' -> '%s')\n", name, target);
    return STATUS_SUCCESS;
}

NTSTATUS XBAPI xbox_IoDeleteSymbolicLink(PXANSI_STRING SymbolicLinkName)
{
    char name[MAX_PATH];
    snprintf(name, sizeof name, "%.*s", SymbolicLinkName->Length, SymbolicLinkName->Buffer);
    EnterCriticalSection(&g_io_lock);
    for (unsigned i = 0; i < g_link_count; i++) {
        if (!_stricmp(g_links[i].name, name)) {
            g_links[i] = g_links[--g_link_count];
            break;
        }
    }
    LeaveCriticalSection(&g_io_lock);
    KTRACE("IoDeleteSymbolicLink('%s')\n", name);
    return STATUS_SUCCESS;
}
