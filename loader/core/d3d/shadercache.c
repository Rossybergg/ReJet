/* Shader compilation with an on-disk cache.
 *
 * D3DCompile takes 10-45 ms per translated NV2A program, which showed up as hitches in play.
 * Compiled bytecode is kept in %LOCALAPPDATA%\rejet\shadercache, named by a 64-bit hash
 * of the HLSL source and profile, so each shader is compiled once per machine. The cache
 * holds only our own translator's output; deleting the folder is always safe. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "shadercache.h"

#define CACHE_VERSION "rejet-shadercache-1"

void d3d_note_shader_compile(void); /* d3d/d3d.c */

static unsigned long long fnv64(unsigned long long h, const char *s)
{
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ull;
    }
    return h;
}

static int cache_path(char *out, size_t cap, unsigned long long key)
{
    static char dir[MAX_PATH];
    if (!dir[0]) {
        char base[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base);
        if (!n || n >= sizeof base)
            return 0;
        snprintf(dir, sizeof dir, "%s\\rejet", base);
        CreateDirectoryA(dir, NULL);
        snprintf(dir, sizeof dir, "%s\\rejet\\shadercache", base);
        CreateDirectoryA(dir, NULL);
    }
    snprintf(out, cap, "%s\\%016llx.bin", dir, key);
    return 1;
}

void *shader_compile(const char *src, const char *name, const char *profile, char *err, size_t errcap)
{
    unsigned long long key = fnv64(fnv64(fnv64(1469598103934665603ull, CACHE_VERSION), profile), src);
    char path[MAX_PATH];
    int have_path = cache_path(path, sizeof path, key);
    ID3DBlob *code = NULL, *errors = NULL;
    void *out = NULL;
    FILE *f;

    if (err && errcap)
        err[0] = 0;
    if (have_path && (f = fopen(path, "rb")) != NULL) {
        long n;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && (out = malloc((size_t)n)) != NULL && fread(out, 1, (size_t)n, f) != (size_t)n) {
            free(out);
            out = NULL;
        }
        fclose(f);
        if (out)
            return out;
    }
    d3d_note_shader_compile(); /* first-run progress screen (d3d.c) */
    if (FAILED(D3DCompile(src, strlen(src), name, NULL, NULL, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                          &code, &errors))) {
        if (err && errcap)
            snprintf(err, errcap, "%s", errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
    }
    else {
        size_t n = ID3D10Blob_GetBufferSize(code);
        if ((out = malloc(n)) != NULL) {
            memcpy(out, ID3D10Blob_GetBufferPointer(code), n);
            if (have_path) { /* write to a temp name, then rename, so a crash never leaves a torn file */
                char tmp[MAX_PATH + 8];
                snprintf(tmp, sizeof tmp, "%s.tmp", path);
                if ((f = fopen(tmp, "wb")) != NULL) {
                    int ok = fwrite(out, 1, n, f) == n;
                    ok &= fclose(f) == 0;
                    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
                        DeleteFileA(tmp);
                }
            }
        }
    }
    if (code)
        ID3D10Blob_Release(code);
    if (errors)
        ID3D10Blob_Release(errors);
    return out;
}

void shader_dump(const char *kind, unsigned index, const char *src)
{
    static int on = -1;
    char path[64];
    FILE *fp;
    if (on < 0)
        on = GetEnvironmentVariableA("REJET_DUMPHLSL", NULL, 0) > 0;
    if (!on)
        return;
    CreateDirectoryA("hlsl", NULL);
    snprintf(path, sizeof path, "hlsl\\%s_%03u.hlsl", kind, index);
    if ((fp = fopen(path, "w")) != NULL) {
        fputs(src, fp);
        fclose(fp);
    }
}
