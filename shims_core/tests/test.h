#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace t {

struct Case {
    const char* name;
    void (*fn)();
};
std::vector<Case>& cases();
extern int g_failures;
extern int g_checks;
struct Reg {
    Reg(const char* n, void (*f)()) { cases().push_back({n, f}); }
};

}  // namespace t

#define TEST(name)                                  \
    static void test_##name();                      \
    static ::t::Reg reg_##name(#name, &test_##name); \
    static void test_##name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++::t::g_checks;                                                              \
        if (!(cond)) {                                                                \
            ++::t::g_failures;                                                        \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                                          \
    do {                                                                                                        \
        ++::t::g_checks;                                                                                        \
        const auto va_ = (a);                                                                                   \
        const auto vb_ = (b);                                                                                   \
        if (!(va_ == vb_)) {                                                                                    \
            ++::t::g_failures;                                                                                  \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s (got %lld vs %lld)\n", __FILE__, __LINE__, #a, #b,    \
                         static_cast<long long>(va_), static_cast<long long>(vb_));                             \
        }                                                                                                       \
    } while (0)
