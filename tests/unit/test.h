/**
 * @file test.h
 * @brief Tiny self-contained unit-test framework used by the treenity tests.
 *
 * Keeping the tests dependency free makes them trivial to run on a bare
 * toolchain (and on CI) without pulling in a third-party framework.
 */
#ifndef TREENET_TEST_H
#define TREENET_TEST_H

#include <stdio.h>
#include <string.h>

extern int g_checks;
extern int g_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        g_checks++;                                                        \
        long long _a = (long long)(a);                                     \
        long long _b = (long long)(b);                                     \
        if (_a != _b) {                                                    \
            printf("    FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__,  \
                   __LINE__, #a, #b, _a, _b);                              \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                              \
    do {                                                                   \
        g_checks++;                                                        \
        double _a = (double)(a);                                           \
        double _b = (double)(b);                                           \
        double _d = _a - _b;                                               \
        if (_d < 0) _d = -_d;                                              \
        if (_d > (eps)) {                                                  \
            printf("    FAIL %s:%d: |%s - %s| = %f > %f\n", __FILE__,      \
                   __LINE__, #a, #b, _d, (double)(eps));                   \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

#define RUN_TEST(fn)                                                       \
    do {                                                                   \
        printf("  %s ... ", #fn);                                          \
        fflush(stdout);                                                    \
        int _before = g_failures;                                          \
        fn();                                                              \
        if (g_failures == _before) {                                       \
            printf("ok\n");                                                \
        } else {                                                           \
            printf("FAILED\n");                                            \
        }                                                                  \
    } while (0)

#endif /* TREENET_TEST_H */
