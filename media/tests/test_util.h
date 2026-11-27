#pragma once

#include <gta6rt/abi.h>

#include <cstdint>
#include <cstdio>

namespace mt {

inline int g_checks = 0, g_failures = 0;

inline void check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(c) ::mt::check((c), #c, __LINE__)
#define CHECK_HR(expr, want)                                                                                      \
    do {                                                                                                          \
        const int32_t _h = (expr);                                                                                \
        ::mt::check(_h == static_cast<int32_t>(want), #expr " == " #want, __LINE__);                              \
        if (_h != static_cast<int32_t>(want)) std::printf("      got 0x%08x\n", static_cast<uint32_t>(_h));        \
    } while (0)

// Calls COM method `slot` of a guest-style object the way guest code would (ms_abi on x86_64).
template <class R, class... A>
R call(uint64_t obj, int slot, A... a) {
    using F = R(GTA6RT_GUEST_ABI*)(uint64_t, A...);
    auto vt = *reinterpret_cast<void* const* const*>(obj);
    return reinterpret_cast<F>(vt[slot])(obj, a...);
}

inline int finish(const char* name) {
    std::printf("%s: %d checks, %d failed: %s\n", name, g_checks, g_failures, g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}

}  // namespace mt
