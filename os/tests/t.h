#pragma once

#include <gta6rt/os.h>
#include <mach/mach.h>
#include <windows.h>

#include <cstdio>
#include <cstring>

static int g_checks, g_failed;

#define CHECK(c)                                                                \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(c)) {                                                             \
            g_failed++;                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                                                       \
    do {                                                                                                     \
        g_checks++;                                                                                          \
        long long _a = (long long)(a), _b = (long long)(b);                                                  \
        if (_a != _b) {                                                                                      \
            g_failed++;                                                                                      \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld, %#llx vs %#llx)\n", __FILE__, __LINE__, #a, #b, \
                    _a, _b, _a, _b);                                                                         \
        }                                                                                                    \
    } while (0)

#define SECTION(name) fprintf(stderr, "-- %s\n", name)

static inline int t_finish(const char* name) {
    printf("%s: %s (%d checks, %d failed)\n", name, g_failed ? "FAIL" : "PASS", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

static inline double t_ms_since(uint64_t t0_ns) { return (double)(gta6rt_time_monotonic_ns() - t0_ns) / 1e6; }
