#include "fault_guard.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {

int g_checks = 0, g_failed = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

int main() {
    namespace fg = rthost::fault_guard;
    check(fg::install(), "install");

    std::vector<unsigned char> a(70001), b(70001);
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<unsigned char>(i * 131 + 7);
    for (std::size_t n : {0u, 1u, 15u, 63u, 64u, 65u, 4096u, 70001u}) {
        std::memset(b.data(), 0, b.size());
        check(fg::copy(b.data(), a.data(), n) && std::memcmp(a.data(), b.data(), n) == 0, "copy sizes");
        check(fg::compare(a.data(), b.data(), n) == 0, "compare equal");
    }
    for (std::size_t at : {0u, 5u, 63u, 64u, 127u, 4095u, 70000u}) {
        std::memcpy(b.data(), a.data(), a.size());
        b[at] ^= 0x40;
        check(fg::compare(a.data(), b.data(), a.size()) == 1, "compare differs");
    }

    {
        std::vector<unsigned char> packed(5 * 64);
        check(fg::gather(packed.data(), a.data(), 5, 2048), "gather");
        bool same = true;
        for (int i = 0; i < 5; ++i) same = same && std::memcmp(packed.data() + i * 64, a.data() + i * 2048, 64) == 0;
        check(same, "gather packs the samples in order");
    }
    const long page = sysconf(_SC_PAGESIZE);
    auto* region = static_cast<unsigned char*>(
        mmap(nullptr, size_t(page) * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    check(region != MAP_FAILED, "mmap");
    std::memset(region, 0x5a, size_t(page) * 3);
    check(mprotect(region + page, size_t(page), PROT_NONE) == 0, "mprotect");
    std::vector<unsigned char> to(size_t(page) * 3);
    check(!fg::copy(to.data(), region, size_t(page) * 3), "copy across a no-access page returns false");
    check(fg::copy(to.data(), region, size_t(page)), "copy before it still works");
    check(fg::compare(region, to.data(), size_t(page) * 3) == -1, "compare across it returns -1");
    check(!fg::gather(to.data(), region, 3, size_t(page)), "gather across it returns false");
    check(munmap(region + page, size_t(page)) == 0, "munmap");
    check(!fg::copy(to.data(), region, size_t(page) * 3), "copy across an unmapped page returns false");
    check(fg::recovered() == 4, "four faults recovered");
    munmap(region, size_t(page));
    munmap(region + 2 * page, size_t(page));

    constexpr std::size_t kBytes = 64u << 20;
    std::vector<unsigned char> src(kBytes, 1), dst(kBytes, 2);
    for (std::size_t i = 0; i < kBytes; i += 4096) src[i] = static_cast<unsigned char>(i >> 12);
    double best_memcpy = 1e9, best_guarded = 1e9, best_memcmp = 1e9, best_compare = 1e9;
    for (int round = 0; round < 5; ++round) {
        auto t = std::chrono::steady_clock::now();
        std::memcpy(dst.data(), src.data(), kBytes);
        best_memcpy = std::min(best_memcpy, ms_since(t));
        t = std::chrono::steady_clock::now();
        fg::copy(dst.data(), src.data(), kBytes);
        best_guarded = std::min(best_guarded, ms_since(t));
        t = std::chrono::steady_clock::now();
        volatile int r1 = std::memcmp(dst.data(), src.data(), kBytes);
        (void)r1;
        best_memcmp = std::min(best_memcmp, ms_since(t));
        t = std::chrono::steady_clock::now();
        volatile int r2 = fg::compare(dst.data(), src.data(), kBytes);
        (void)r2;
        best_compare = std::min(best_compare, ms_since(t));
    }
    check(fg::compare(dst.data(), src.data(), kBytes) == 0, "64 MiB copy is exact");
    std::printf("64 MiB, best of 5: memcpy %.2f ms, guarded copy %.2f ms, memcmp %.2f ms, guarded compare %.2f ms\n",
                best_memcpy, best_guarded, best_memcmp, best_compare);
    std::printf("fault_guard_test: %d checks, %d failed: %s\n", g_checks, g_failed, g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
