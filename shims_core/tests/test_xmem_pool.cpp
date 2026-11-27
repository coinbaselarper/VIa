// The title pool (xmem.h, GTA6RT_METAL_TITLE_POOL=1): run as `GTA6RT_METAL_TITLE_POOL=1 shims_core_test xmem`. Without
// the switch these cases check that nothing is pool-backed (the default path).
#include <cstdint>
#include <cstring>
#include <vector>

#include "shim_call.h"
#include "test.h"
#include "xmem.h"

using tcall::call;

namespace {
constexpr const char* kK = "kernel32.dll";
constexpr const char* kX = "xmem.dll";
constexpr uint64_t kPage = 0x10000;
constexpr uint32_t kCommitReserve = 0x3000, kReserve = 0x2000, kCommit = 0x1000, kDecommit = 0x4000, kRelease = 0x8000,
                   kGraphics = 0x10000000, kReadWrite = 4;

bool pool_on() {
    void* base = nullptr;
    uint64_t size = 0;
    return rt::xmem::title_pool_range(&base, &size);
}

uint8_t* view_of(const void* va, uint64_t* run = nullptr) {
    void* host = nullptr;
    uint64_t bytes = 0;
    if (!rt::xmem::title_pool_view(reinterpret_cast<uintptr_t>(va), &host, &bytes)) return nullptr;
    if (run) *run = bytes;
    return static_cast<uint8_t*>(host);
}

bool removed(uint64_t since, const void* lo, uint64_t bytes) {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    rt::xmem::title_pool_removals(since, out);
    const uint64_t a = reinterpret_cast<uintptr_t>(lo);
    for (const auto& [from, to] : out)
        if (from <= a && a + bytes <= to) return true;
    return false;
}
}  // namespace

TEST(xmem_pool_virtual_alloc_is_a_view_of_the_pool) {
    auto* p = call<uint8_t*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{300 << 10}, kCommitReserve, uint64_t{1}, kReadWrite);
    CHECK(p != nullptr);
    if (!p) return;
    if (!pool_on()) {
        CHECK(view_of(p) == nullptr);
        call<int>(kK, "VirtualFree", p, size_t{0}, kRelease);
        return;
    }
    void* base = nullptr;
    uint64_t size = 0;
    CHECK(rt::xmem::title_pool_range(&base, &size));
    CHECK_EQ(reinterpret_cast<uintptr_t>(base) & 0x3fff, 0u);
    uint64_t run = 0;
    uint8_t* host = view_of(p + 0x1234, &run);
    CHECK(host != nullptr);
    if (!host) return;
    CHECK(host >= static_cast<uint8_t*>(base) && host < static_cast<uint8_t*>(base) + size);
    CHECK_EQ(run, (300u << 10) - 0x1234);
    p[0x1234] = 0x5a;
    CHECK_EQ(host[0], 0x5a);
    host[1] = 0xa5;
    CHECK_EQ(p[0x1235], 0xa5);

    const uint64_t g0 = rt::xmem::title_pool_generation();
    CHECK_EQ(call<int>(kK, "VirtualFree", p + kPage, size_t{kPage}, kDecommit), 1);
    CHECK(rt::xmem::title_pool_generation() > g0);
    CHECK(view_of(p + kPage) == nullptr);
    CHECK(removed(g0, p + kPage, kPage));
    uint8_t* kept = view_of(p);
    CHECK(kept != nullptr && kept[0x1234] == 0x5a);
    uint8_t* pool_bytes = host - 0x1234 + kPage;
    pool_bytes[8] = 0x77;
    CHECK(call<void*>(kK, "VirtualAlloc", p + kPage, size_t{kPage}, kCommit, kReadWrite) == p + kPage);
    CHECK(view_of(p + kPage) == pool_bytes);
    CHECK_EQ(p[kPage + 8], 0);
    p[kPage + 9] = 3;
    CHECK_EQ(pool_bytes[9], 3);
    CHECK_EQ(p[0x1234], 0x5a);

    const uint64_t g1 = rt::xmem::title_pool_generation();
    CHECK_EQ(call<int>(kK, "VirtualFree", p, size_t{0}, kRelease), 1);
    CHECK(view_of(p) == nullptr);
    CHECK(removed(g1, p, 300 << 10));
}

TEST(xmem_pool_runs_are_reused_zeroed_and_grow) {
    std::vector<uint8_t*> blocks;
    for (int i = 0; i < 64; ++i) {
        auto* b = call<uint8_t*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{1 << 20}, kCommitReserve, uint64_t{1}, kReadWrite);
        CHECK(b != nullptr);
        if (b) std::memset(b, i + 1, 1 << 20);
        blocks.push_back(b);
    }
    if (!pool_on()) {
        for (auto* b : blocks) call<int>(kK, "VirtualFree", b, size_t{0}, kRelease);
        return;
    }
    int bad = 0;
    std::vector<uint8_t*> hosts;
    for (int i = 0; i < 64; ++i) {
        uint64_t run = 0;
        uint8_t* h = view_of(blocks[i], &run);
        bad += !h || run != (1u << 20) || h[12345] != i + 1;
        hosts.push_back(h);
    }
    for (int i = 0; i < 64; ++i)
        for (int j = i + 1; j < 64; ++j) bad += hosts[i] && hosts[j] && hosts[i] < hosts[j] + (1 << 20) && hosts[j] < hosts[i] + (1 << 20);
    CHECK_EQ(bad, 0);
    CHECK(rt::xmem::title_pool_stats().runs >= 64u);
    call<int>(kK, "VirtualFree", blocks[10], size_t{0}, kRelease);
    auto* again = call<uint8_t*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{512 << 10}, kCommitReserve, uint64_t{1}, kReadWrite);
    CHECK(again != nullptr);
    if (again) {
        CHECK(view_of(again) != nullptr);
        bool zero = true;
        for (uint32_t k = 0; k < (512u << 10); k += 4093) zero &= again[k] == 0;
        CHECK(zero);
        call<int>(kK, "VirtualFree", again, size_t{0}, kRelease);
    }
    for (int i = 0; i < 64; ++i)
        if (i != 10) call<int>(kK, "VirtualFree", blocks[i], size_t{0}, kRelease);
}

TEST(xmem_pool_commits_inside_a_reservation_page_by_page) {
    auto* r = call<uint8_t*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{1 << 20}, kReserve, uint64_t{1}, kReadWrite);
    CHECK(r != nullptr);
    if (!r) return;
    CHECK(call<void*>(kX, "XMemVirtualAlloc", r + 0x23000, size_t{0x3000}, kCommit, uint64_t{1}, kReadWrite) == r + 0x23000);
    r[0x23000] = 1;
    r[0x25fff] = 2;
    if (pool_on()) {
        uint64_t run = 0;
        uint8_t* h = view_of(r + 0x23000, &run);
        CHECK(h != nullptr);
        CHECK_EQ(run, 0x3000u);
        if (h) CHECK(h[0] == 1 && h[0x2fff] == 2);
        CHECK(view_of(r + 0x22000) == nullptr);
        CHECK(view_of(r + 0x26000) == nullptr);
    } else {
        CHECK(view_of(r + 0x23000) == nullptr);
    }
    call<int>(kK, "VirtualFree", r, size_t{0}, kRelease);
}

TEST(xmem_pool_graphics_virtualalloc_only) {
    auto* g = call<uint8_t*>(kK, "VirtualAlloc", static_cast<void*>(nullptr), size_t{256 << 10}, kCommitReserve | kGraphics, kReadWrite);
    auto* plain = call<uint8_t*>(kK, "VirtualAlloc", static_cast<void*>(nullptr), size_t{256 << 10}, kCommitReserve, kReadWrite);
    CHECK(g != nullptr && plain != nullptr);
    if (!g || !plain) return;
    g[100] = 9;
    plain[100] = 9;
    if (pool_on()) {
        uint8_t* h = view_of(g + 100);
        CHECK(h != nullptr && h[0] == 9);
        CHECK(view_of(plain + 100) == nullptr);
    } else {
        CHECK(view_of(g + 100) == nullptr);
    }
    call<int>(kK, "VirtualFree", g, size_t{0}, kRelease);
    call<int>(kK, "VirtualFree", plain, size_t{0}, kRelease);
}

TEST(xmem_pool_physical_windows_follow_map_and_unmap) {
    uint64_t pages[4] = {};
    uint64_t count = 4;
    CHECK_EQ(call<int>(kX, "XMemAllocatePhysicalPages", uint64_t{0}, &count, pages), 1);
    auto* w = call<uint8_t*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{4 * kPage}, 0x20402000u, uint64_t{1}, kReadWrite);
    CHECK(w != nullptr);
    if (!w) return;
    CHECK(call<void*>(kX, "XMemMapPhysicalPages", w, uint64_t{4}, static_cast<const uint64_t*>(pages)) == w);
    w[0x100] = 0x42;
    if (!pool_on()) {
        CHECK(view_of(w) == nullptr);
    } else {
        uint64_t run = 0;
        uint8_t* h = view_of(w + 0x100, &run);
        CHECK(h == static_cast<uint8_t*>(rt::xmem::physical_page_host_address(pages[0])) + 0x100);
        CHECK(h && h[0] == 0x42);
        const bool consecutive = rt::xmem::physical_page_host_address(pages[3]) ==
                                 static_cast<uint8_t*>(rt::xmem::physical_page_host_address(pages[0])) + 3 * kPage;
        CHECK(!consecutive || run == 4 * kPage - 0x100);
        const uint64_t g0 = rt::xmem::title_pool_generation();
        const uint64_t swapped[2] = {pages[2], pages[1]};
        CHECK(call<void*>(kX, "XMemMapPhysicalPages", w + kPage, uint64_t{2}, swapped) == w + kPage);
        CHECK(removed(g0, w + kPage, 2 * kPage));
        CHECK(view_of(w + kPage) == rt::xmem::physical_page_host_address(pages[2]));
        CHECK(view_of(w + 2 * kPage) == rt::xmem::physical_page_host_address(pages[1]));
        const uint64_t g1 = rt::xmem::title_pool_generation();
        CHECK(call<void*>(kX, "XMemMapPhysicalPages", w, uint64_t{4}, static_cast<const uint64_t*>(nullptr)) == w);
        CHECK(view_of(w) == nullptr);
        CHECK(removed(g1, w, 4 * kPage));
    }
    call<void*>(kX, "XMemMapPhysicalPages", w, uint64_t{4}, static_cast<const uint64_t*>(nullptr));
    call<int>(kK, "VirtualFree", w, size_t{0}, kRelease);
    CHECK_EQ(call<int>(kX, "XMemFreePhysicalPages", uint64_t{4}, static_cast<const uint64_t*>(pages)), 1);
    uint64_t again[4] = {};
    count = 4;
    CHECK_EQ(call<int>(kX, "XMemAllocatePhysicalPages", uint64_t{0}, &count, again), 1);
    bool zero = true;
    for (uint64_t p : again) {
        const auto* h = static_cast<const uint8_t*>(rt::xmem::physical_page_host_address(p));
        zero &= h && h[0x100] == 0;
    }
    CHECK(zero);
    call<int>(kX, "XMemFreePhysicalPages", uint64_t{4}, static_cast<const uint64_t*>(again));
}
