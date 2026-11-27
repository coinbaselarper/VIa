#pragma once

#include <cstdio>
#include <cstdlib>

static int g_checks = 0, g_failures = 0;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        ++g_checks;                                                   \
        if (!(cond)) {                                                \
            ++g_failures;                                             \
            std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
        }                                                             \
    } while (0)

static int finish(const char* name) {
    std::printf("%s: %d checks, %d failures: %s\n", name, g_checks, g_failures, g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
