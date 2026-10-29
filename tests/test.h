/* ReJet's unit tests (tests/README.md): a minimal harness. Each test is a function that uses
 * CHECK; a failed CHECK prints where and what, and the run exits non-zero. */
#pragma once
#include <stdio.h>

extern int g_test_failures;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
            g_test_failures++;                                                           \
        }                                                                                \
    } while (0)

/* CHECK with the values printed (integers). */
#define CHECK_EQ(a, b)                                                                   \
    do {                                                                                 \
        long long a_ = (long long)(a), b_ = (long long)(b);                              \
        if (a_ != b_) {                                                                  \
            printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, a_, b_); \
            g_test_failures++;                                                           \
        }                                                                                \
    } while (0)

/* CHECK for floats, within eps. */
#define CHECK_NEAR(a, b, eps)                                                            \
    do {                                                                                 \
        double a_ = (double)(a), b_ = (double)(b);                                       \
        if (a_ - b_ > (eps) || b_ - a_ > (eps)) {                                        \
            printf("  FAIL %s:%d: %s ~ %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, a_, b_); \
            g_test_failures++;                                                           \
        }                                                                                \
    } while (0)

void test_texconv(void);
void test_psh(void);
void test_widescreen(void);
void test_iso(void);
void test_settings(void);
