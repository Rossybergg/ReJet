/* First-run setup (see setup.h): find the player's own game files before the game starts.
 *
 * ReJet runs the original Default.xbe from an extracted copy of the disc. On a first start
 * (no files found) this screen asks for the disc image or an extracted folder, checks that its
 * Default.xbe is the PAL release ReJet supports (SHA-1, hle_xbe_supported), and extracts an
 * image into %LOCALAPPDATA%\rejet\Game (beside the saves, shared by every ReJet version), with progress. The folder is remembered in
 * rejet.ini ([Game] Folder). The window is drawn with GDI in the ReJet menu's Garage style (the game's
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
#include "iso.h"
#include "kernel.h"
#include "settings.h"
#include "ui/menu.h"
#include "ui/setup.h"
#include "version.gen.h"

#define SECTOR ISO_SECTOR

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

void setup_shared_folder(WCHAR *folder, unsigned cap)
{
    WCHAR local[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"REJET_GAMEDIR", folder, cap) && folder[0]) /* tests: a fresh folder */
        return;
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    swprintf(folder, cap, L"%ls\\rejet\\Game", local);
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
static WCHAR g_target[MAX_PATH]; /* where an image is extracted: setup_shared_folder() */
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
    if (ok) {
        WCHAR parent[MAX_PATH], *slash;
        wcscpy_s(parent, MAX_PATH, g_target); /* %LOCALAPPDATA%\rejet may not exist yet */
        if ((slash = wcsrchr(parent, L'\\')) != NULL) {
            *slash = 0;
            CreateDirectoryW(parent, NULL);
        }
        CreateDirectoryW(g_target, NULL);
    }
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
                         L"ReJet couldn't save the game files (Windows error %lu). Check there's about 2.5 GB "
                         L"free on drive C:, or choose a folder you've already extracted the game to.",
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
    r.top = S(310) + i * S(62);
    r.bottom = r.top + S(48);
    return r;
}

/* ---- drawing (GDI, the ReJet menu's Garage look: yellow field, orange bars, black ink) */

#define G_FIELD RGB(255, 194, 26)
#define G_ORANGE RGB(243, 154, 30)
#define G_CYAN RGB(55, 184, 200)
#define G_INK RGB(17, 17, 17)
#define G_TINT RGB(236, 180, 24) /* an unselected row: the field a shade darker */

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

static void fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c)
{
    fill_skew(dc, x0, y0, x1, y1, 0, c);
}

/* A box with a black outline o pixels thick. */
static void outlined(HDC dc, int x0, int y0, int x1, int y1, int o, COLORREF c)
{
    fill(dc, x0, y0, x1, y1, G_INK);
    fill(dc, x0 + o, y0 + o, x1 - o, y1 - o, c);
}

/* Bahnschrift (Windows 10 and later), as the in-game menu; heavy = the condensed label face. */
static void text_at(HDC dc, const WCHAR *s, RECT r, int size, int heavy, COLORREF c, UINT fmt)
{
    HFONT f = CreateFontW(-S(size), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                          heavy ? L"Bahnschrift SemiBold Condensed" : L"Bahnschrift");
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
    static const int bars[][2] = {{0, 22}, {36, 40}, {90, 16}, {120, 54}, {188, 20}};
    PAINTSTRUCT ps;
    RECT c, r;
    HDC wdc = BeginPaint(w, &ps), dc;
    HBITMAP back;
    WCHAR msg[512], cur[260];
    GetClientRect(w, &c);
    dc = CreateCompatibleDC(wdc);
    back = CreateCompatibleBitmap(wdc, c.right, c.bottom);
    SelectObject(dc, back);
    fill(dc, 0, 0, c.right, c.bottom, G_FIELD);
    for (int i = 0; i < 5; i++) /* the Garage's orange bars */
        fill(dc, S(48 + bars[i][0]), 0, S(48 + bars[i][0] + bars[i][1]), S(56), G_ORANGE);
    fill(dc, c.right - S(150), 0, c.right - S(132), S(56), G_ORANGE);
    fill(dc, c.right - S(118), 0, c.right - S(86), S(56), G_ORANGE);

    /* the logo as drawn (black) and a cyan tag */
    draw_logo(dc, S(50), S(78), S(62));
    {
        int lw = 0, tx, tw = S(110);
        const DWORD *px;
        UINT lwp, lhp;
        if (menu_logo(&px, &lwp, &lhp))
            lw = MulDiv(S(62), (int)lwp, (int)lhp);
        tx = S(50) + lw + S(18);
        fill_skew(dc, tx, S(102), tx + tw, S(136), -S(5), G_INK);
        fill_skew(dc, tx + S(3), S(105), tx + tw - S(3), S(133), -S(5), G_CYAN);
        r.left = tx;
        r.right = tx + tw - S(4);
        r.top = S(102);
        r.bottom = S(136);
        text_at(dc, L"SETUP", r, 20, 1, G_INK, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
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
                r, 18, 0, G_INK, DT_LEFT | DT_WORDBREAK);
    else if (g_state == ST_WORKING) {
        WCHAR line[400];
        double f = g_total_bytes ? (double)g_done_bytes / (double)g_total_bytes : 0;
        int x0 = S(80), x1 = c.right - S(80);
        swprintf(line, 400, L"Setting things up... %.0f%%", f * 100);
        text_at(dc, line, r, 18, 0, G_INK, DT_LEFT | DT_SINGLELINE);
        outlined(dc, x0, S(226), x1, S(252), S(3), RGB(255, 226, 140));
        fill(dc, x0 + S(3), S(229), x0 + S(3) + (int)((x1 - x0 - S(6)) * f), S(249), G_CYAN);
        r.top = S(260);
        text_at(dc, cur, r, 14, 0, RGB(90, 74, 16), DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }
    else if (g_state == ST_ERROR) { /* on a black strip, so it reads as a problem */
        RECT t = {S(80) + S(18), S(172), c.right - S(80) - S(14), S(286)};
        fill(dc, S(80), S(168), S(90), S(290), G_INK);
        fill(dc, S(94), S(168), c.right - S(80), S(290), G_INK);
        text_at(dc, msg, t, 18, 0, RGB(255, 255, 255), DT_LEFT | DT_WORDBREAK);
    }
    else
        text_at(dc, L"All set! Starting the game...", r, 18, 0, G_INK, DT_LEFT | DT_SINGLELINE);

    for (int i = 0; i < button_count() && g_state != ST_DONE; i++) {
        RECT b = button_rect(c, i);
        const WCHAR *label = g_state == ST_WELCOME ? WELCOME_BUTTONS[i] : g_state == ST_WORKING ? L"Cancel" : L"Back";
        WCHAR up[64];
        int on = i == g_sel, k;
        for (k = 0; label[k] && k < 63; k++)
            up[k] = towupper(label[k]);
        up[k] = 0;
        if (on) { /* lifted onto white with a black drop shadow, as the in-game menu */
            fill(dc, b.left + S(6), b.top + S(6), b.right + S(6), b.bottom + S(6), G_INK);
            outlined(dc, b.left - S(4), b.top - S(4), b.right - S(4), b.bottom - S(4), S(3), RGB(255, 255, 255));
            OffsetRect(&b, -S(4), -S(4));
        }
        else {
            fill(dc, b.left, b.top, b.right, b.bottom, G_TINT);
            fill(dc, b.left, b.top, b.left + S(5), b.bottom, G_INK);
        }
        b.left += S(22);
        text_at(dc, up, b, 24, 1, G_INK, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (on) {
            b.right -= S(18);
            text_at(dc, L">", b, 24, 1, G_INK, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    /* the black button bar */
    fill(dc, 0, c.bottom - S(46), c.right, c.bottom, G_INK);
    r.left = S(24);
    r.right = c.right - S(20);
    r.top = c.bottom - S(46);
    r.bottom = c.bottom;
    text_at(dc, L"Mouse, arrow keys + Enter, or a controller (d-pad + A)", r, 14, 0, RGB(255, 255, 255),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text_at(dc, L"ReJet " TEXT(REJET_BUILD), r, 14, 0, RGB(201, 201, 201), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

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
    HMODULE self = NULL;
    MSG msg;
    RECT r;
    typedef BOOL(WINAPI * SetDpiContextFn)(HANDLE);
    SetDpiContextFn dpi_aware =
        (SetDpiContextFn)(void *)GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext");
    if (dpi_aware)
        dpi_aware((HANDLE)-4); /* per-monitor v2, as the game window */
    InitializeCriticalSection(&g_lock);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); /* the folder picker */
    setup_shared_folder(g_target, MAX_PATH);
    (void)exe_dir;
    g_state = ST_WELCOME;
    g_sel = 0;
    g_result[0] = 0;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = setup_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    /* the ReJet icon from rejet.dll: ReJet.exe's resources are overwritten by the XBE image */
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)setup_proc, &self);
    wc.hIcon = LoadIconW(self, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
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
