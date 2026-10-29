/* Disc images (loader/core/iso.c), on small synthetic XDVDFS images (no game data). */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "iso.h"
#include "test.h"
#include <winioctl.h>

static void put_entry(unsigned char *t, unsigned off, unsigned left, unsigned right, unsigned sector, unsigned size,
                      int dir, const char *name)
{
    size_t n = strlen(name);
    t[off] = (unsigned char)(left / 4);
    t[off + 1] = (unsigned char)((left / 4) >> 8);
    t[off + 2] = (unsigned char)(right / 4);
    t[off + 3] = (unsigned char)((right / 4) >> 8);
    memcpy(t + off + 4, &sector, 4);
    memcpy(t + off + 8, &size, 4);
    t[off + 12] = dir ? 0x10 : 0x20;
    t[off + 13] = (unsigned char)n;
    memcpy(t + off + 14, name, n);
}

static int write_at(HANDLE f, unsigned long long at, const void *p, DWORD n)
{
    LARGE_INTEGER pos;
    DWORD w;
    pos.QuadPart = (LONGLONG)at;
    return SetFilePointerEx(f, pos, NULL, FILE_BEGIN) && WriteFile(f, p, n, &w, NULL) && w == n;
}

/* An image with its game partition at `base`: default.xbe (100 bytes) and Media\a.bin (5000
 * bytes, three sectors) in a two-level directory tree. */
static int make_image(const WCHAR *path, unsigned long long base)
{
    static unsigned char hdr[2048], root[2048], media[2048], xbe[100], abin[5000];
    DWORD ret;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    int ok;
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    DeviceIoControl(f, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL); /* full images start ~400 MB in */
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "MICROSOFT*XBOX*MEDIA", 20);
    {
        DWORD rs = 33, rsz = 2048;
        memcpy(hdr + 20, &rs, 4);
        memcpy(hdr + 24, &rsz, 4);
    }
    memset(root, 0xFF, sizeof root); /* padding */
    put_entry(root, 0, 0, 32, 35, sizeof xbe, 0, "default.xbe"); /* right child at 32 */
    put_entry(root, 32, 0, 0, 34, 2048, 1, "Media");
    memset(media, 0xFF, sizeof media);
    put_entry(media, 0, 0, 0, 36, sizeof abin, 0, "a.bin");
    for (unsigned i = 0; i < sizeof xbe; i++)
        xbe[i] = (unsigned char)i;
    for (unsigned i = 0; i < sizeof abin; i++)
        abin[i] = (unsigned char)(i * 3);
    ok = write_at(f, base + 32 * 2048, hdr, sizeof hdr) && write_at(f, base + 33 * 2048, root, sizeof root) &&
         write_at(f, base + 34 * 2048, media, sizeof media) && write_at(f, base + 35 * 2048, xbe, sizeof xbe) &&
         write_at(f, base + 36 * 2048, abin, sizeof abin);
    CloseHandle(f);
    return ok;
}

/* Trimmed images (partition at 0) and full ones (XGD1 at 0x18300000, as JSRF's disc; XGD3 at
 * 0x2080000): setup must find the files in all of them, nested directories included. */
static void reads_every_layout(void)
{
    static const unsigned long long bases[] = {0, 0x2080000ull, 0x18300000ull};
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    swprintf(path, MAX_PATH, L"%srejet_test.iso", dir);
    for (size_t b = 0; b < sizeof bases / sizeof bases[0]; b++) {
        Iso iso;
        const IsoFile *x, *a;
        unsigned char buf[5000];
        CHECK(make_image(path, bases[b]));
        CHECK(iso_open(&iso, path));
        CHECK_EQ(iso.base, bases[b]);
        CHECK_EQ(iso.n, 3); /* default.xbe, Media, Media\a.bin */
        CHECK_EQ(iso.total, 100 + 5000);
        x = iso_find(&iso, L"DEFAULT.XBE"); /* case-insensitive, as on the disc */
        a = iso_find(&iso, L"media\\a.bin");
        CHECK(x && x->size == 100);
        CHECK(a && a->size == 5000);
        CHECK(iso_find(&iso, L"Media") == NULL); /* directories aren't files */
        if (a) {
            CHECK(iso_read(&iso, iso.base + a->sector * ISO_SECTOR, buf, a->size));
            CHECK_EQ(buf[4999], (4999 * 3) & 0xFF);
        }
        iso_close(&iso);
    }
    DeleteFileW(path);
}

/* Anything that isn't an Xbox disc image is refused, not half-read. */
static void refuses_other_files(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    Iso iso;
    HANDLE f;
    char junk[70000];
    GetTempPathW(MAX_PATH, dir);
    swprintf(path, MAX_PATH, L"%srejet_test_junk.iso", dir);
    memset(junk, 'x', sizeof junk);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(write_at(f, 0, junk, sizeof junk));
    CloseHandle(f);
    CHECK(!iso_open(&iso, path));
    iso_close(&iso);
    DeleteFileW(path);
}

void test_iso(void)
{
    reads_every_layout();
    refuses_other_files();
}
