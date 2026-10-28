/* First-run setup (see setup.h): find the player's own game files before the game starts.
 *
 * ReJet runs the original Default.xbe from an extracted copy of the disc. On a first start
 * (no files found) this screen asks for the disc image or an extracted folder, checks that its
 * Default.xbe is the PAL release ReJet supports (SHA-1, hle_xbe_supported), and extracts an
 * image into Game\ next to ReJet.exe, with progress. The folder is remembered in
 * rejet.ini ([Game] Folder). The window is drawn with GDI in the ReJet menu's style (the game's
 * Direct3D device doesn't exist yet); mouse, keyboard and controller all work.
 *
 * Disc images: XDVDFS ("MICROSOFT*XBOX*MEDIA" at sector 32 of the game partition), either a
 * trimmed XISO (partition at 0) or a full image (partition at 0x18300000 on XGD1 discs like
 * JSRF; XGD2/3 offsets are accepted too). Directory tables are binary trees of entries:
 * left, right (in dwords), start sector, size, attributes (0x10 = directory), name. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <Xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "hle.h"
#include "kernel.h"
#include "settings.h"
#include "ui/menu.h"
#include "ui/setup.h"
#include "version.gen.h"

#define SECTOR 2048ull
static const unsigned long long PARTITIONS[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};

/* ------------------------------------------------------------------ disc image */

typedef struct IsoFile {
    WCHAR path[260]; /* relative, backslashes */
    DWORD sector, size;
    int dir;
} IsoFile;

typedef struct Iso {
    HANDLE f;
    unsigned long long base;
    IsoFile *files;
    unsigned n, cap;
    unsigned long long total; /* bytes in files */
} Iso;

static int iso_read(Iso *iso, unsigned long long at, void *buf, DWORD size)
{
    LARGE_INTEGER pos;
    DWORD got = 0;
    pos.QuadPart = (LONGLONG)at;
    return SetFilePointerEx(iso->f, pos, NULL, FILE_BEGIN) && ReadFile(iso->f, buf, size, &got, NULL) && got == size;
}

static void iso_walk(Iso *iso, DWORD sector, DWORD size, const WCHAR *prefix, int depth)
{
    BYTE *table;
    unsigned stack[512], sp = 0;
    unsigned char *seen;
    if (!size || depth > 32 || size > (64u << 20) || !(table = (BYTE *)malloc(size)))
        return;
    seen = (unsigned char *)calloc(size / 4 + 1, 1);
    if (!seen || !iso_read(iso, iso->base + sector * SECTOR, table, size)) {
        free(seen);
        free(table);
        return;
    }
    stack[sp++] = 0;
    while (sp) {
        unsigned off = stack[--sp], left, right, nlen;
        IsoFile *e;
        if (off + 14 > size || seen[off / 4])
            continue;
        seen[off / 4] = 1;
        left = table[off] | table[off + 1] << 8;
        right = table[off + 2] | table[off + 3] << 8;
        if (left == 0xFFFF && right == 0xFFFF)
            continue;
        nlen = table[off + 13];
        if (off + 14 + nlen > size)
            continue;
        if (iso->n == iso->cap) {
            IsoFile *grown = (IsoFile *)realloc(iso->files, (iso->cap ? iso->cap * 2 : 256) * sizeof *grown);
            if (!grown)
                break;
            iso->files = grown;
            iso->cap = iso->cap ? iso->cap * 2 : 256;
        }
        e = &iso->files[iso->n++];
        memcpy(&e->sector, table + off + 4, 4);
        memcpy(&e->size, table + off + 8, 4);
        e->dir = (table[off + 12] & 0x10) != 0;
        {
            WCHAR name[256];
            for (unsigned i = 0; i < nlen; i++)
                name[i] = (WCHAR)table[off + 14 + i];
            name[nlen] = 0;
            swprintf(e->path, 260, L"%s%s", prefix, name);
        }
        if (!e->dir)
            iso->total += e->size;
        if (left && sp < 512)
            stack[sp++] = left * 4;
        if (right && sp < 512)
            stack[sp++] = right * 4;
    }
    free(seen);
    free(table);
    /* recurse into the directories found at this level */
    {
        unsigned count = iso->n;
        for (unsigned i = 0; i < count; i++) {
            IsoFile d = iso->files[i];
            size_t plen = wcslen(prefix);
            if (d.dir && wcsncmp(d.path, prefix, plen) == 0 && !wcschr(d.path + plen, L'\\')) {
                WCHAR sub[262];
                swprintf(sub, 262, L"%s\\", d.path);
                iso_walk(iso, d.sector, d.size, sub, depth + 1);
            }
        }
    }
}

static int iso_open(Iso *iso, const WCHAR *path)
{
    static const char MAGIC[] = "MICROSOFT*XBOX*MEDIA";
    memset(iso, 0, sizeof *iso);
    iso->f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (iso->f == INVALID_HANDLE_VALUE)
        return 0;
    for (unsigned i = 0; i < sizeof PARTITIONS / sizeof PARTITIONS[0]; i++) {
        BYTE hdr[28];
        if (iso_read(iso, PARTITIONS[i] + 32 * SECTOR, hdr, sizeof hdr) && !memcmp(hdr, MAGIC, 20)) {
            DWORD root, rsize;
            iso->base = PARTITIONS[i];
            memcpy(&root, hdr + 20, 4);
            memcpy(&rsize, hdr + 24, 4);
            iso_walk(iso, root, rsize, L"", 0);
            return iso->n > 0;
        }
    }
    return 0;
}

static void iso_close(Iso *iso)
{
    if (iso->f && iso->f != INVALID_HANDLE_VALUE)
        CloseHandle(iso->f);
    free(iso->files);
    memset(iso, 0, sizeof *iso);
}

static const IsoFile *iso_find(const Iso *iso, const WCHAR *path)
{
    for (unsigned i = 0; i < iso->n; i++)
        if (!iso->files[i].dir && !_wcsicmp(iso->files[i].path, path))
            return &iso->files[i];
    return NULL;
}

/* ------------------------------------------------------------------ checks */

static unsigned char *read_whole(const WCHAR *path, DWORD *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    unsigned char *buf = NULL;
    DWORD got = 0;
    *size = 0;
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    *size = GetFileSize(f, NULL);
    if (*size && *size < (64u << 20) && (buf = (unsigned char *)malloc(*size)) != NULL &&
        (!ReadFile(f, buf, *size, &got, NULL) || got != *size)) {
        free(buf);
        buf = NULL;
    }
    CloseHandle(f);
    return buf;
}

int setup_folder_ok(const WCHAR *folder)
{
    WCHAR path[MAX_PATH];
    DWORD size;
    unsigned char *xbe;
    int ok;
    if (!folder || !folder[0])
        return 0;
    swprintf(path, MAX_PATH, L"%s\\default.xbe", folder);
    if (!(xbe = read_whole(path, &size)))
        return 0;
    ok = hle_xbe_supported(xbe, size);
    free(xbe);
    return ok;
}

/* ------------------------------------------------------------------ the window */

enum { ST_WELCOME, ST_WORKING, ST_ERROR, ST_DONE };

static HWND g_wnd;
static int g_state, g_sel;
static WCHAR g_message[512];
static WCHAR g_target[MAX_PATH]; /* Game\ next to the exe */
static WCHAR g_result[MAX_PATH]; /* the folder to run from */
static volatile LONG g_cancel;
static volatile LONGLONG g_done_bytes, g_total_bytes;
static WCHAR g_current[260];
static CRITICAL_SECTION g_lock;
static HANDLE g_worker;
static UINT g_dpi = 96;

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static const WCHAR *const WELCOME_BUTTONS[] = {L"Choose disc image (.iso)", L"Choose game folder", L"Quit"};

static void fail(const WCHAR *msg)
{
    EnterCriticalSection(&g_lock);
    wcsncpy_s(g_message, 512, msg, _TRUNCATE);
    LeaveCriticalSection(&g_lock);
    g_state = ST_ERROR;
    g_sel = 0;
}

static DWORD WINAPI extract_thread(void *arg)
{
    Iso *iso = (Iso *)arg;
    static BYTE buf[1 << 20];
    const IsoFile *xbe = iso_find(iso, L"default.xbe");
    ULARGE_INTEGER free_bytes = {0};
    int ok = 1;

    /* the right disc? */
    if (!xbe || xbe->size > (64u << 20)) {
        fail(L"We couldn't find the game in that file. Make sure it's a Jet Set Radio Future Xbox disc image.");
        ok = 0;
    }
    else {
        unsigned char *data = (unsigned char *)malloc(xbe->size);
        if (!data || !iso_read(iso, iso->base + xbe->sector * SECTOR, data, xbe->size) ||
            !hle_xbe_supported(data, xbe->size)) {
            fail(L"That's not the version ReJet supports yet. ReJet needs the European (PAL) Xbox release of "
                 L"Jet Set Radio Future.");
            ok = 0;
        }
        free(data);
    }
    if (ok && GetDiskFreeSpaceExW(g_target, &free_bytes, NULL, NULL) == 0) {
        WCHAR parent[MAX_PATH], *slash;
        wcscpy_s(parent, MAX_PATH, g_target);
        if ((slash = wcsrchr(parent, L'\\')) != NULL)
            *slash = 0;
        GetDiskFreeSpaceExW(parent, &free_bytes, NULL, NULL);
    }
    if (ok && free_bytes.QuadPart < iso->total + (64ull << 20)) {
        WCHAR msg[256];
        swprintf(msg, 256, L"There isn't enough free space. The game needs about %.1f GB on the drive ReJet is on.",
                 (double)iso->total / (1 << 30));
        fail(msg);
        ok = 0;
    }
    g_total_bytes = (LONGLONG)iso->total;
    g_done_bytes = 0;
    if (ok)
        CreateDirectoryW(g_target, NULL);
    for (unsigned i = 0; ok && i < iso->n && !g_cancel; i++) {
        const IsoFile *e = &iso->files[i];
        WCHAR out[MAX_PATH];
        swprintf(out, MAX_PATH, L"%s\\%s", g_target, e->path);
        if (e->dir) {
            CreateDirectoryW(out, NULL);
            continue;
        }
        EnterCriticalSection(&g_lock);
        wcsncpy_s(g_current, 260, e->path, _TRUNCATE);
        LeaveCriticalSection(&g_lock);
        {
            HANDLE f = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            unsigned long long at = iso->base + e->sector * SECTOR;
            DWORD left = e->size;
            if (f == INVALID_HANDLE_VALUE) {
                WCHAR msg[512];
                swprintf(msg, 512,
                         L"ReJet couldn't save the game files (Windows error %lu). Try moving the ReJet folder "
                         L"somewhere you can write to, like Documents.",
                         GetLastError());
                fail(msg);
                ok = 0;
                break;
            }
            while (left && !g_cancel) {
                DWORD n = left < sizeof buf ? left : (DWORD)sizeof buf, wrote = 0;
                if (!iso_read(iso, at, buf, n) || !WriteFile(f, buf, n, &wrote, NULL) || wrote != n) {
                    fail(L"Something went wrong while copying the game files. The disc image may be incomplete or damaged.");
                    ok = 0;
                    break;
                }
                at += n;
                left -= n;
                InterlockedExchangeAdd64(&g_done_bytes, n);
            }
            CloseHandle(f);
        }
    }
    if (ok && g_cancel) {
        g_state = ST_WELCOME;
        g_sel = 0;
    }
    else if (ok) {
        wcscpy_s(g_result, MAX_PATH, g_target);
        settings_set_game_folder(g_target);
        log_printf("setup: extracted %u files (%.1f MB)\n", iso->n, (double)iso->total / (1 << 20));
        g_state = ST_DONE;
    }
    iso_close(iso);
    free(iso);
    return 0;
}

static void choose_image(void)
{
    WCHAR path[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    Iso *iso;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"Xbox disc images (*.iso;*.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Your Jet Set Radio Future disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetEnvironmentVariableW(L"REJET_SETUP_IMAGE", path, MAX_PATH) && /* tests: skip the dialog */
        !GetOpenFileNameW(&ofn))
        return;
    iso = (Iso *)calloc(1, sizeof *iso);
    if (!iso || !iso_open(iso, path)) {
        if (iso) {
            iso_close(iso);
            free(iso);
        }
        fail(L"That file doesn't look like an Xbox disc image. Choose your Jet Set Radio Future .iso file.");
        return;
    }
    log_printf("setup: image with %u entries, %.1f MB\n", iso->n, (double)iso->total / (1 << 20));
    g_cancel = 0;
    g_state = ST_WORKING;
    g_worker = CreateThread(NULL, 0, extract_thread, iso, 0, NULL);
}

static void choose_folder(void)
{
    BROWSEINFOW bi;
    WCHAR name[MAX_PATH], path[MAX_PATH];
    PIDLIST_ABSOLUTE pidl;
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_wnd;
    bi.pszDisplayName = name;
    bi.lpszTitle = L"The folder with Jet Set Radio Future's default.xbe";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (!(pidl = SHBrowseForFolderW(&bi)))
        return;
    if (SHGetPathFromIDListW(pidl, path)) {
        if (setup_folder_ok(path)) {
            wcscpy_s(g_result, MAX_PATH, path);
            settings_set_game_folder(path);
            g_state = ST_DONE;
        }
        else
            fail(L"We couldn't find the supported version of the game in that folder. ReJet needs the "
                 L"European (PAL) Xbox release of Jet Set Radio Future.");
    }
    CoTaskMemFree(pidl);
}

static void activate(void)
{
    if (g_state == ST_WELCOME) {
        if (g_sel == 0)
            choose_image();
        else if (g_sel == 1)
            choose_folder();
        else
            PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
    else if (g_state == ST_WORKING)
        InterlockedExchange(&g_cancel, 1);
    else if (g_state == ST_ERROR) {
        g_state = ST_WELCOME;
        g_sel = 0;
    }
    InvalidateRect(g_wnd, NULL, FALSE);
}

static int button_count(void)
{
    return g_state == ST_WELCOME ? 3 : 1;
}

static RECT button_rect(RECT c, int i)
{
    RECT r;
    r.left = S(80);
    r.right = c.right - S(80);
    r.top = S(300) + i * S(64);
    r.bottom = r.top + S(50);
    return r;
}

/* ---- drawing (GDI, the ReJet menu's colours) */

static void fill_skew(HDC dc, int x0, int y0, int x1, int y1, int skew, COLORREF c)
{
    POINT p[4] = {{x0 + skew, y0}, {x1 + skew, y0}, {x1, y1}, {x0, y1}};
    HBRUSH b = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    Polygon(dc, p, 4);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(pen);
}

static void text_at(HDC dc, const WCHAR *s, RECT r, int size, int heavy, COLORREF c, UINT fmt)
{
    HFONT f = CreateFontW(-S(size), 0, 0, 0, heavy ? FW_HEAVY : FW_SEMIBOLD, heavy, 0, 0, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                          heavy ? L"Arial Black" : L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, fmt);
    SelectObject(dc, old);
    DeleteObject(f);
}

static void draw_logo(HDC dc, int x, int y, int h)
{
    const DWORD *px;
    UINT w, lh;
    BITMAPINFO bi;
    void *bits;
    HBITMAP bm;
    HDC mem;
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!menu_logo(&px, &w, &lh)) {
        RECT r = {x, y, x + S(600), y + h};
        text_at(dc, L"ReJet", r, 64, 1, RGB(0, 0, 0), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)lh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    mem = CreateCompatibleDC(dc);
    bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bm && bits) {
        memcpy(bits, px, (size_t)w * lh * 4);
        SelectObject(mem, bm);
        SetStretchBltMode(dc, HALFTONE);
        GdiAlphaBlend(dc, x, y, MulDiv(h, (int)w, (int)lh), h, mem, 0, 0, (int)w, (int)lh, bf);
    }
    DeleteDC(mem);
    if (bm)
        DeleteObject(bm);
}

static void paint(HWND w)
{
    PAINTSTRUCT ps;
    RECT c, r;
    HDC wdc = BeginPaint(w, &ps), dc;
    HBITMAP back;
    HBRUSH bg;
    WCHAR msg[512], cur[260];
    GetClientRect(w, &c);
    dc = CreateCompatibleDC(wdc);
    back = CreateCompatibleBitmap(wdc, c.right, c.bottom);
    SelectObject(dc, back);
    bg = CreateSolidBrush(RGB(10, 26, 28));
    FillRect(dc, &c, bg);
    DeleteObject(bg);
    fill_skew(dc, c.right - S(260), 0, c.right - S(170), c.bottom, -S(220), RGB(20, 44, 30));
    fill_skew(dc, c.right - S(140), 0, c.right - S(100), c.bottom, -S(220), RGB(40, 20, 34));

    /* title strip */
    fill_skew(dc, -S(40), S(40), S(620), S(150), S(36), RGB(200, 240, 30));
    fill_skew(dc, -S(40), S(142), S(580), S(156), S(36), RGB(255, 45, 138));
    draw_logo(dc, S(50), S(58), S(74));
    r.left = S(80);
    r.right = c.right - S(60);
    r.top = S(180);
    r.bottom = S(290);

    EnterCriticalSection(&g_lock);
    wcscpy_s(msg, 512, g_message);
    wcscpy_s(cur, 260, g_current);
    LeaveCriticalSection(&g_lock);

    if (g_state == ST_WELCOME)
        text_at(dc,
                L"Welcome to ReJet! Pick your Jet Set Radio Future disc image and ReJet will set everything up "
                L"for you. Already extracted the game? Choose its folder instead.",
                r, 17, 0, RGB(255, 255, 255), DT_LEFT | DT_WORDBREAK);
    else if (g_state == ST_WORKING) {
        WCHAR line[400];
        double f = g_total_bytes ? (double)g_done_bytes / (double)g_total_bytes : 0;
        RECT bar = {S(80), S(230), c.right - S(80), S(250)}, fillr = bar;
        HBRUSH b1 = CreateSolidBrush(RGB(40, 60, 62)), b2 = CreateSolidBrush(RGB(200, 240, 30));
        swprintf(line, 400, L"Setting things up... %.0f%%", f * 100);
        text_at(dc, line, r, 17, 0, RGB(255, 255, 255), DT_LEFT | DT_SINGLELINE);
        FillRect(dc, &bar, b1);
        fillr.right = bar.left + (LONG)((bar.right - bar.left) * f);
        FillRect(dc, &fillr, b2);
        DeleteObject(b1);
        DeleteObject(b2);
        r.top = S(258);
        text_at(dc, cur, r, 13, 0, RGB(154, 164, 164), DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }
    else if (g_state == ST_ERROR)
        text_at(dc, msg, r, 17, 0, RGB(255, 45, 138), DT_LEFT | DT_WORDBREAK);
    else
        text_at(dc, L"All set! Starting the game...", r, 17, 0, RGB(255, 255, 255), DT_LEFT | DT_SINGLELINE);

    for (int i = 0; i < button_count() && g_state != ST_DONE; i++) {
        RECT b = button_rect(c, i);
        const WCHAR *label = g_state == ST_WELCOME ? WELCOME_BUTTONS[i] : g_state == ST_WORKING ? L"Cancel" : L"Back";
        int on = i == g_sel;
        fill_skew(dc, b.left, b.top, b.right, b.bottom, S(14), on ? RGB(200, 240, 30) : RGB(24, 48, 50));
        if (on)
            fill_skew(dc, b.left, b.top, b.left + S(10), b.bottom, S(14), RGB(255, 45, 138));
        b.left += S(30);
        text_at(dc, label, b, 20, 1, on ? RGB(0, 0, 0) : RGB(255, 255, 255), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    r.top = c.bottom - S(40);
    r.bottom = c.bottom - S(12);
    text_at(dc, L"Mouse, arrow keys + Enter, or a controller (d-pad + A)", r, 13, 0, RGB(154, 164, 164),
            DT_LEFT | DT_SINGLELINE);
    text_at(dc, L"ReJet " TEXT(REJET_BUILD), r, 13, 0, RGB(154, 164, 164), DT_RIGHT | DT_SINGLELINE);

    BitBlt(wdc, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    DeleteObject(back);
    DeleteDC(dc);
    EndPaint(w, &ps);
}

static LRESULT CALLBACK setup_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT:
        paint(w);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER: { /* progress, and controllers */
        static WORD prev;
        XINPUT_STATE s;
        WORD b = 0, pressed;
        for (DWORD p = 0; p < 4; p++)
            if (XInputGetState(p, &s) == ERROR_SUCCESS)
                b |= s.Gamepad.wButtons;
        pressed = (WORD)(b & ~prev);
        prev = b;
        if (pressed & XINPUT_GAMEPAD_DPAD_UP)
            g_sel = (g_sel + button_count() - 1) % button_count();
        if (pressed & XINPUT_GAMEPAD_DPAD_DOWN)
            g_sel = (g_sel + 1) % button_count();
        if (pressed & XINPUT_GAMEPAD_A)
            activate();
        if (pressed & XINPUT_GAMEPAD_B && g_state == ST_ERROR)
            activate();
        if (g_state == ST_DONE) {
            KillTimer(w, 1);
            DestroyWindow(w);
            return 0;
        }
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_UP)
            g_sel = (g_sel + button_count() - 1) % button_count();
        else if (wp == VK_DOWN)
            g_sel = (g_sel + 1) % button_count();
        else if (wp == VK_RETURN || wp == VK_SPACE)
            activate();
        else if (wp == VK_ESCAPE && g_state != ST_WORKING)
            PostMessageW(w, WM_CLOSE, 0, 0);
        InvalidateRect(w, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP: {
        RECT c;
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        GetClientRect(w, &c);
        for (int i = 0; i < button_count(); i++) {
            RECT b = button_rect(c, i);
            if (PtInRect(&b, p)) {
                if (g_sel != i) {
                    g_sel = i;
                    InvalidateRect(w, NULL, FALSE);
                }
                if (m == WM_LBUTTONUP)
                    activate();
            }
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_state == ST_WORKING) { /* stop the copy first */
            InterlockedExchange(&g_cancel, 1);
            WaitForSingleObject(g_worker, INFINITE);
        }
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

int setup_run(const WCHAR *exe_dir, WCHAR *folder, unsigned cap)
{
    WNDCLASSEXW wc;
    MSG msg;
    RECT r;
    typedef BOOL(WINAPI * SetDpiContextFn)(HANDLE);
    SetDpiContextFn dpi_aware =
        (SetDpiContextFn)(void *)GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext");
    if (dpi_aware)
        dpi_aware((HANDLE)-4); /* per-monitor v2, as the game window */
    InitializeCriticalSection(&g_lock);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); /* the folder picker */
    swprintf(g_target, MAX_PATH, L"%s\\Game", exe_dir);
    g_state = ST_WELCOME;
    g_sel = 0;
    g_result[0] = 0;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = setup_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1)); /* the exe's ReJet icon */
    wc.hIconSm = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"rejet_setup";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowW(L"rejet_setup", L"ReJet - Jet Set Radio Future", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, wc.hInstance, NULL);
    {
        typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
        GetDpiForWindowFn fn =
            (GetDpiForWindowFn)(void *)GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForWindow");
        g_dpi = fn ? fn(g_wnd) : 96;
    }
    r.left = r.top = 0;
    r.right = S(760);
    r.bottom = S(560);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    SetWindowPos(g_wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(g_wnd, SW_SHOW);
    SetTimer(g_wnd, 1, 50, NULL);
    log_printf("setup: no game files found; asking\n");
    if (GetEnvironmentVariableW(L"REJET_SETUP_IMAGE", NULL, 0)) /* tests: start with that image */
        choose_image();
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_worker) {
        WaitForSingleObject(g_worker, INFINITE);
        CloseHandle(g_worker);
    }
    if (!g_result[0]) {
        log_printf("setup: closed without game files\n");
        return 0;
    }
    wcsncpy_s(folder, cap, g_result, _TRUNCATE);
    return 1;
}
