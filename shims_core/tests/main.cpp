#include "test.h"

#include <cstring>

namespace t {
std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}
int g_failures = 0;
int g_checks = 0;
}  // namespace t

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed_cases = 0;
    for (auto& c : t::cases()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = t::g_failures;
        c.fn();
        ++ran;
        const bool ok = t::g_failures == before;
        if (!ok) ++failed_cases;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", c.name);
    }
    std::printf("shims_core_test: %d cases, %d checks, %d failed cases, %d failed checks\n", ran, t::g_checks,
                failed_cases, t::g_failures);
    return t::g_failures ? 1 : 0;
}
