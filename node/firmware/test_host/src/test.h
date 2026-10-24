/* Minimal test framework for the host tests (no external dependencies). */
#pragma once

#include <stdio.h>
#include <string.h>

extern int g_failures;
extern int g_checks;
extern const char *g_context; /* e.g. the current vector name */

#define CHECK(cond)                                                                                         \
    do {                                                                                                    \
        g_checks++;                                                                                         \
        if (!(cond)) {                                                                                      \
            g_failures++;                                                                                   \
            fprintf(stderr, "%s:%d: [%s] CHECK failed: %s\n", __FILE__, __LINE__, g_context ? g_context : "", \
                    #cond);                                                                                 \
        }                                                                                                   \
    } while (0)

#define CHECK_EQ(a, b)                                                                                      \
    do {                                                                                                    \
        long long _a = (long long)(a), _b = (long long)(b);                                                 \
        g_checks++;                                                                                         \
        if (_a != _b) {                                                                                     \
            g_failures++;                                                                                   \
            fprintf(stderr, "%s:%d: [%s] CHECK_EQ failed: %s = %lld, %s = %lld\n", __FILE__, __LINE__,      \
                    g_context ? g_context : "", #a, _a, #b, _b);                                            \
        }                                                                                                   \
    } while (0)

typedef void (*test_fn)(void);
typedef struct {
    const char *name;
    test_fn fn;
} test_case_t;
