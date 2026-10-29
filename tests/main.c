/* ReJet's unit tests: run every group, report, exit non-zero on any failure. Built with the
 * loader (rejet_tests.exe in build\loader); tools/release_check.py runs it. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "test.h"

int g_test_failures;

/* The loader code under test logs through log_printf; the tests don't need its output. */
void log_printf(const char *fmt, ...)
{
    (void)fmt;
}

/* psh.c compiles through the shader cache; the tests never compile, they only translate. */
void *shader_compile(const char *src, const char *name, const char *profile, char *err, size_t errcap)
{
    (void)src, (void)name, (void)profile, (void)err, (void)errcap;
    return NULL;
}

void shader_dump(const char *kind, unsigned index, const char *src)
{
    (void)kind, (void)index, (void)src;
}

static const struct {
    const char *name;
    void (*run)(void);
} GROUPS[] = {
    {"texture conversion", test_texconv},
    {"pixel shader constants", test_psh},
    {"widescreen", test_widescreen},
    {"disc images", test_iso},
    {"settings", test_settings},
};

int main(void)
{
    for (size_t i = 0; i < sizeof GROUPS / sizeof GROUPS[0]; i++) {
        int before = g_test_failures;
        GROUPS[i].run();
        printf("%-24s %s\n", GROUPS[i].name, g_test_failures == before ? "ok" : "FAILED");
    }
    printf(g_test_failures ? "%d check(s) failed\n" : "all tests passed\n", g_test_failures);
    return g_test_failures ? 1 : 0;
}
