#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gta6rt/os.h>

#include "shim_call.h"
#include "test.h"
#include "console_registry.h"
#include "xmem.h"
#include "env_stub.h"

#include <gta6rt/shims.h>

#include <sys/sysctl.h>
#include <cstdlib>

using tcall::call;

namespace {
constexpr const char* kK = "kernel32.dll";
constexpr const char* kX = "xmem.dll";
}  // namespace

TEST(loader_serves_registered_modules_with_pseudo_handles) {
    const uint64_t k32 = call<uint64_t>(kK, "GetModuleHandleW", u"KERNEL32.dll");
    CHECK(k32 != 0);
    CHECK_EQ(call<uint64_t>(kK, "LoadLibraryW", u"kernel32"), k32);
    CHECK_EQ(call<uint64_t>(kK, "LoadLibraryA", "C:\\Windows\\System32\\Kernel32.DLL"), k32);
    const uint64_t proc = call<uint64_t>(kK, "GetProcAddress", k32, "QueryPerformanceFrequency");
    CHECK(proc != 0);
    CHECK_EQ(call<uint64_t>(kK, "GetProcAddress", k32, "NoSuchExport"), 0u);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 127u);
    const uint64_t xm = call<uint64_t>(kK, "LoadLibraryExW", u"xmem.dll", static_cast<void*>(nullptr), 0u);
    CHECK(xm != 0 && xm != k32);
    CHECK(call<uint64_t>(kK, "GetProcAddress", xm, "XMemAlloc") != 0);
    CHECK_EQ(call<uint64_t>(kK, "LoadLibraryW", u"no_such_module.dll"), 0u);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 126u);
    CHECK_EQ(call<int>(kK, "FreeLibrary", xm), 1);
    CHECK_EQ(call<int>(kK, "FreeLibrary", uint64_t{0x1234}), 0);
    CHECK_EQ(call<int>(kK, "DisableThreadLibraryCalls", xm), 1);
    const uint64_t ws = call<uint64_t>(kK, "LoadLibraryW", u"WS2_32.dll");
    CHECK(call<uint64_t>(kK, "GetProcAddress", ws, reinterpret_cast<const char*>(uintptr_t{115})) != 0);
}

namespace {
uint64_t g_bundled_calls = 0;
void* bundled_find(void*, const std::string&) { return nullptr; }
void* bundled_load(void*, const std::string& dll, uint32_t, bool& handled) {
    if (dll != "libhttpclient.gdk.dll") return nullptr;
    handled = true;
    ++g_bundled_calls;
    return reinterpret_cast<void*>(0x180000000ull);
}
void* bundled_proc(void*, void* module, const std::string& sym, bool& owns) {
    if (reinterpret_cast<uintptr_t>(module) != 0x180000000ull) return nullptr;
    owns = true;
    return sym == "XblExport" ? reinterpret_cast<void*>(0x180001000ull) : nullptr;
}
bool bundled_owns(void*, void* module) { return reinterpret_cast<uintptr_t>(module) == 0x180000000ull; }
}  // namespace

TEST(loader_defers_package_dlls_to_the_host_loader_and_module_table) {
    static const rt::shims::BundledModuleOps ops{&bundled_find, &bundled_load, &bundled_proc, &bundled_owns, nullptr, nullptr};
    static int token;
    rt::shims::add_bundled_loader(&token, &ops);
    const uint64_t h = call<uint64_t>(kK, "LoadLibraryW", u"G:\\libHttpClient.GDK.dll");
    CHECK_EQ(h, 0x180000000ull);
    CHECK_EQ(g_bundled_calls, 1u);
    CHECK_EQ(call<uint64_t>(kK, "GetProcAddress", h, "XblExport"), 0x180001000ull);
    CHECK_EQ(call<uint64_t>(kK, "GetProcAddress", h, "Missing"), 0u);
    CHECK_EQ(call<int>(kK, "FreeLibrary", h), 1);
    rt::shims::remove_bundled_loader(&token);

    gta6rt_module_desc d{};
    d.name = "game_scarlett_master.exe";
    d.path_utf8 = "/games/gta/game_scarlett_master.exe";
    d.kind = GTA6RT_MODULE_GUEST_IMAGE;
    d.base = 0x140000000ull;
    d.size = 0x1000;
    d.get_export = [](void*, const char* n, uint32_t) -> gta6rt_gva { return n && !std::strcmp(n, "Exported") ? 0x140000800ull : 0; };
    gta6rt_module* m = nullptr;
    CHECK_EQ(gta6rt_module_register(&d, &m), GTA6RT_OK);
    CHECK_EQ(call<uint64_t>(kK, "GetModuleHandleW", static_cast<const char16_t*>(nullptr)), 0x140000000ull);
    CHECK_EQ(call<uint64_t>(kK, "GetModuleHandleA", "game_scarlett_master.exe"), 0x140000000ull);
    CHECK_EQ(call<uint64_t>(kK, "GetProcAddress", uint64_t{0x140000000ull}, "Exported"), 0x140000800ull);
    char16_t path[64];
    CHECK(call<uint32_t>(kK, "GetModuleFileNameW", uint64_t{0}, path, 64u) > 10);
    CHECK(std::u16string(path) == u"/games/gta/game_scarlett_master.exe");
    gta6rt_module_unregister(m);
}

TEST(xmem_heap_blocks_are_64_aligned_and_cross_freeable) {
    void* a = call<void*>(kX, "XMemAlloc", size_t{100}, uint64_t{0x5e50000});
    void* b = call<void*>(kX, "XMemAllocDefault", size_t{0}, uint64_t{0});
    CHECK(a && b);
    CHECK_EQ(reinterpret_cast<uintptr_t>(a) % 64, 0u);
    CHECK_EQ(reinterpret_cast<uintptr_t>(b) % 64, 0u);
    std::memset(a, 0x5A, 100);
    call<void>(kX, "XMemFreeDefault", a, uint64_t{0});
    call<void>(kX, "XMemFree", b, uint64_t{0});
    call<void>(kX, "XMemFree", static_cast<void*>(nullptr), uint64_t{0});
    CHECK_EQ(call<int>(kX, "XMemIsToolsMemAvailable"), 0);
    CHECK_EQ(call<int>(kX, "XMemTransferMemory", 1u, size_t{2u << 20}), 1);
    uint64_t stats[16] = {};
    CHECK_EQ(call<int>(kX, "XMemGetWorkingSetStatistics", 0x20000001u, stats), 1);
    CHECK(stats[0] >= (2ull << 30) && stats[1] > 0);
    CHECK_EQ(call<int>(kX, "XMemGetWorkingSetStatistics", 0x20000001u, static_cast<void*>(nullptr)), 0);
}

TEST(xmem_physical_pages_alias_through_windows) {
    constexpr uint32_t kWindow64k = 0x20402000u;
    constexpr uint64_t kPage = 0x10000;
    uint64_t count = 4, pages[4] = {};
    CHECK_EQ(call<int>(kX, "XMemAllocatePhysicalPages", uint64_t{0x1234}, &count, pages), 1);
    CHECK_EQ(count, 4u);
    for (uint64_t p : pages) CHECK(p != 0 && p % 16 == 0 && p < (1u << 25));
    CHECK_EQ(rt::xmem::live_physical_pages(), 4u);

    void* w1 = call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{4 * kPage}, kWindow64k, uint64_t{0x5e50000}, 4u);
    void* w2 = call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{4 * kPage}, kWindow64k, uint64_t{0x5e50000}, 4u);
    CHECK(w1 && w2 && w1 != w2);
    CHECK_EQ(reinterpret_cast<uintptr_t>(w1) % kPage, 0u);
    CHECK_EQ(rt::xmem::window_count(), 2u);

    CHECK(call<void*>(kX, "XMemMapPhysicalPages", w1, uint64_t{4}, static_cast<const uint64_t*>(pages)) == w1);
    CHECK(call<void*>(kX, "XMemMapPhysicalPages", w2, uint64_t{2}, static_cast<const uint64_t*>(pages)) == w2);
    CHECK_EQ(rt::xmem::mapped_page_count(), 6u);
    auto* a = static_cast<uint8_t*>(w1);
    auto* b = static_cast<uint8_t*>(w2);
    a[0] = 0x11;
    a[kPage + 5] = 0x22;
    a[3 * kPage + 7] = 0x33;
    CHECK_EQ(b[0], 0x11);
    CHECK_EQ(b[kPage + 5], 0x22);
    b[100] = 0x44;
    CHECK_EQ(a[100], 0x44);

    CHECK(rt::xmem::physical_page_host_address(pages[1]) != nullptr);
    CHECK_EQ(static_cast<uint8_t*>(rt::xmem::physical_page_host_address(pages[1]))[5], 0x22);

    CHECK(call<void*>(kX, "XMemMapPhysicalPages", w2, uint64_t{2}, static_cast<const uint64_t*>(nullptr)) == w2);
    CHECK_EQ(rt::xmem::mapped_page_count(), 4u);
    CHECK_EQ(a[0], 0x11);
    CHECK_EQ(static_cast<uint8_t*>(rt::xmem::physical_page_host_address(pages[0]))[0], 0x11);

    void* addrs[2] = {w2, static_cast<uint8_t*>(w2) + kPage};
    uint64_t scatter[2] = {pages[3], pages[2]};
    CHECK_EQ(call<int>(kX, "XMemMapPhysicalPagesScatter", static_cast<void* const*>(addrs), 2u, static_cast<const uint64_t*>(scatter)), 1);
    CHECK_EQ(b[7], 0x33);

    CHECK(call<void*>(kX, "XMemMapPhysicalPages", static_cast<uint8_t*>(w1) + 1, uint64_t{1}, static_cast<const uint64_t*>(pages)) == nullptr);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 87u);
    uint64_t bogus = 0xDEAD0;
    CHECK(call<void*>(kX, "XMemMapPhysicalPages", w1, uint64_t{1}, static_cast<const uint64_t*>(&bogus)) == nullptr);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 87u);
    void* outside = reinterpret_cast<void*>(0x10000);
    CHECK(call<void*>(kX, "XMemMapPhysicalPages", outside, uint64_t{1}, static_cast<const uint64_t*>(pages)) == nullptr);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 487u);

    CHECK_EQ(call<int>(kX, "XMemFreePhysicalPages", uint64_t{4}, static_cast<const uint64_t*>(pages)), 1);
    CHECK_EQ(call<int>(kX, "XMemFreePhysicalPages", uint64_t{1}, static_cast<const uint64_t*>(pages)), 0);
    CHECK_EQ(rt::xmem::live_physical_pages(), 0u);
    uint64_t again = 1, reuse[1] = {};
    CHECK_EQ(call<int>(kX, "XMemAllocatePhysicalPages", uint64_t{0}, &again, reuse), 1);
    CHECK(reuse[0] != 0 && (reuse[0] == pages[0] || reuse[0] == pages[1] || reuse[0] == pages[2] || reuse[0] == pages[3]));
    call<int>(kX, "XMemFreePhysicalPages", uint64_t{1}, static_cast<const uint64_t*>(reuse));
    CHECK_EQ(call<int>(kK, "VirtualFree", w1, size_t{0}, 0x8000u), 1);
    CHECK_EQ(call<int>(kK, "VirtualFree", w2, size_t{0}, 0x8000u), 1);
    CHECK_EQ(rt::xmem::window_count(), 0u);
}

TEST(xmem_gpu_only_window_is_host_readable) {
    constexpr uint64_t kPage = 0x10000;
    uint64_t count = 1, pages[1] = {};
    CHECK_EQ(call<int>(kX, "XMemAllocatePhysicalPages", uint64_t{0}, &count, pages), 1);
    void* cpu = call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{kPage}, 0x20402000u, uint64_t{1}, 0x4u);
    void* gpu = call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{kPage}, 0x20402000u, uint64_t{1}, 0x801u);
    CHECK(cpu && gpu);
    CHECK(call<void*>(kX, "XMemMapPhysicalPages", cpu, uint64_t{1}, static_cast<const uint64_t*>(pages)) == cpu);
    CHECK(call<void*>(kX, "XMemMapPhysicalPages", gpu, uint64_t{1}, static_cast<const uint64_t*>(pages)) == gpu);
    static_cast<uint8_t*>(cpu)[123] = 0x7E;
    CHECK_EQ(static_cast<volatile uint8_t*>(gpu)[123], 0x7E);
    gta6rt_vm_region r{};
    CHECK_EQ(gta6rt_vm_query(gpu, &r), GTA6RT_OK);
    CHECK_EQ(r.state, GTA6RT_VM_STATE_COMMITTED);
    CHECK_EQ(r.prot & (GTA6RT_PROT_READ | GTA6RT_PROT_WRITE), GTA6RT_PROT_READ | GTA6RT_PROT_WRITE);
    for (void* w : {cpu, gpu}) call<void*>(kX, "XMemMapPhysicalPages", w, uint64_t{1}, static_cast<const uint64_t*>(nullptr));
    call<int>(kX, "XMemFreePhysicalPages", uint64_t{1}, static_cast<const uint64_t*>(pages));
    for (void* w : {cpu, gpu}) call<int>(kK, "VirtualFree", w, size_t{0}, 0x8000u);
}

TEST(xmem_copy_page_mappings_and_transfer) {
    constexpr uint64_t kPage = 0x10000;
    uint64_t count = 2, pages[2] = {};
    CHECK_EQ(call<int>(kX, "XMemAllocatePhysicalPages", uint64_t{0}, &count, pages), 1);
    void* src = call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{2 * kPage}, 0x20402000u, uint64_t{1}, 4u);
    void* dst = call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{2 * kPage}, 0x20402000u, uint64_t{1}, 4u);
    CHECK(src && dst);
    call<void*>(kX, "XMemMapPhysicalPages", src, uint64_t{2}, static_cast<const uint64_t*>(pages));
    static_cast<uint8_t*>(src)[0] = 0x5A;
    static_cast<uint8_t*>(src)[kPage] = 0x6B;
    CHECK(rt::xmem::copy_page_mappings_fn != nullptr);
    CHECK(rt::xmem::copy_page_mappings_fn(dst, src, 2, false));
    CHECK_EQ(static_cast<uint8_t*>(dst)[0], 0x5A);
    CHECK_EQ(static_cast<uint8_t*>(dst)[kPage], 0x6B);
    CHECK(rt::xmem::copy_page_mappings(dst, src, 2, true));
    CHECK_EQ(static_cast<uint8_t*>(dst)[kPage], 0x5A);
    CHECK(rt::xmem::copy_page_mappings(dst, nullptr, 2, false));
    CHECK_EQ(rt::xmem::mapped_page_count(), 2u);

    uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8}, back[8] = {};
    CHECK(rt::xmem::transfer_physical(std::span<const uint64_t>(pages, 2), 0xFFFA, std::span<uint8_t>(data, 8), true));
    CHECK_EQ(static_cast<uint8_t*>(src)[0xFFFF], 6);
    CHECK_EQ(static_cast<uint8_t*>(src)[kPage + 1], 8);
    CHECK(rt::xmem::transfer_physical(std::span<const uint64_t>(pages, 2), 0xFFFA, std::span<uint8_t>(back, 8), false));
    CHECK(std::memcmp(data, back, 8) == 0);
    uint8_t big[2 * 0x10000];
    CHECK(!rt::xmem::transfer_physical(std::span<const uint64_t>(pages, 2), 4, std::span<uint8_t>(big, sizeof big), true));
    call<void*>(kX, "XMemMapPhysicalPages", src, uint64_t{2}, static_cast<const uint64_t*>(nullptr));
    call<int>(kX, "XMemFreePhysicalPages", uint64_t{2}, static_cast<const uint64_t*>(pages));
    call<int>(kK, "VirtualFree", src, size_t{0}, 0x8000u);
    call<int>(kK, "VirtualFree", dst, size_t{0}, 0x8000u);
}

TEST(xmem_virtual_alloc_page_modes_and_placeholders) {
    CHECK(call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{0}, 0x3000u, uint64_t{1}, 4u) == nullptr);
    CHECK_EQ(call<uint32_t>(kK, "GetLastError"), 87u);
    CHECK(call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{4096}, 0x20800000u | 0x3000u, uint64_t{1}, 4u) == nullptr);
    CHECK(call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{4096}, 0x01000000u | 0x20000000u | 0x08000000u, uint64_t{1}, 4u) == nullptr);

    void* big = call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{3000000}, 0x20003000u, uint64_t{1}, 4u);
    CHECK(big != nullptr);
    CHECK_EQ(reinterpret_cast<uintptr_t>(big) % (2u << 20), 0u);
    static_cast<uint8_t*>(big)[2999999] = 1;
    call<int>(kK, "VirtualFree", big, size_t{0}, 0x8000u);

    void* committed = call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{100000}, 0x20403000u, uint64_t{1}, 4u);
    CHECK(committed != nullptr);
    CHECK_EQ(reinterpret_cast<uintptr_t>(committed) % 0x10000, 0u);
    std::memset(committed, 1, 100000);
    call<int>(kK, "VirtualFree", committed, size_t{0}, 0x8000u);

    constexpr uint64_t kPage = 0x10000;
    void* ph = call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{16 * kPage}, 0x20442000u, uint64_t{1}, 1u);
    CHECK(ph != nullptr);
    void* carved = call<void*>(kX, "XMemVirtualAlloc", static_cast<uint8_t*>(ph) + 4 * kPage, size_t{4 * kPage}, 0x20406000u, uint64_t{1}, 4u);
    CHECK(carved == static_cast<uint8_t*>(ph) + 4 * kPage);
    CHECK_EQ(rt::xmem::window_count(), 1u);
    CHECK(call<void*>(kX, "XMemVirtualAlloc", reinterpret_cast<void*>(0x30000), size_t{kPage}, 0x20406000u, uint64_t{1}, 4u) == nullptr);
    call<int>(kK, "VirtualFree", ph, size_t{0}, 0x8000u);
    CHECK_EQ(rt::xmem::window_count(), 0u);
}

namespace {
constexpr uint64_t k4GiB = uint64_t{1} << 32;
constexpr size_t kTitleHeap = 0xb900000;

void* title_heap_request() {
    return call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), kTitleHeap, 0x42000u, uint64_t{7}, 1u);
}
}  // namespace

TEST(xmem_32bit_heap_served_by_default_on_this_host) {
    if (std::getenv("LEONIDA_XMEM_32BIT")) return;
    const uint64_t lowest = gta6rt_vm_lowest_mappable();
#if defined(__arm64__)
    CHECK(lowest >= k4GiB);
#endif
    const uint64_t above = rt::xmem::low32_placed_above();
    void* p = title_heap_request();
    CHECK(p != nullptr);
    if (!p) return;
    const uint64_t a = reinterpret_cast<uintptr_t>(p);
    if (lowest < k4GiB) {
        CHECK(a >= lowest && a + kTitleHeap <= k4GiB);
        CHECK_EQ(rt::xmem::low32_placed_above(), above);
    } else {
        CHECK(a >= k4GiB);
        CHECK_EQ(rt::xmem::low32_placed_above(), above + 1);
    }
    CHECK_EQ(call<int>(kK, "VirtualFree", p, size_t{0}, 0x8000u), 1);
}

TEST(xmem_32bit_heap_in_a_process_without_low_address_space) {
    if (std::getenv("LEONIDA_XMEM_32BIT")) return;
    stub::set_lowest_mappable(k4GiB);
    CHECK_EQ(gta6rt_vm_lowest_mappable(), k4GiB);
    const uint64_t above = rt::xmem::low32_placed_above();
    void* p = title_heap_request();
    CHECK(p != nullptr && reinterpret_cast<uintptr_t>(p) >= k4GiB);
    CHECK_EQ(rt::xmem::low32_placed_above(), above + 1);
    if (p) CHECK_EQ(call<int>(kK, "VirtualFree", p, size_t{0}, 0x8000u), 1);

    setenv("LEONIDA_XMEM_32BIT", "require", 1);
    gta6rt_last_error_set(0);
    CHECK(title_heap_request() == nullptr);
    CHECK_EQ(gta6rt_last_error_get(), 8u);
    CHECK_EQ(rt::xmem::low32_placed_above(), above + 1);

    setenv("LEONIDA_XMEM_32BIT", "anywhere", 1);
    p = title_heap_request();
    CHECK(p != nullptr && reinterpret_cast<uintptr_t>(p) >= k4GiB);
    CHECK_EQ(rt::xmem::low32_placed_above(), above + 2);
    if (p) CHECK_EQ(call<int>(kK, "VirtualFree", p, size_t{0}, 0x8000u), 1);

    setenv("LEONIDA_XMEM_32BIT", "bogus", 1);
    p = title_heap_request();
    CHECK(p != nullptr);
    CHECK_EQ(rt::xmem::low32_placed_above(), above + 3);
    if (p) CHECK_EQ(call<int>(kK, "VirtualFree", p, size_t{0}, 0x8000u), 1);
    unsetenv("LEONIDA_XMEM_32BIT");
    stub::set_lowest_mappable(0);
}

TEST(xmem_32bit_heap_refused_when_low_space_exists_but_is_full) {
    if (std::getenv("LEONIDA_XMEM_32BIT")) return;
    if (gta6rt_vm_lowest_mappable() >= k4GiB) return;
    std::vector<void*> fill;
    for (;;) {
        void* f = nullptr;
        if (gta6rt_vm_reserve_range(0x10000, k4GiB, 0x1000000, 0, 0, &f) != GTA6RT_OK) break;
        fill.push_back(f);
    }
    for (;;) {
        void* f = nullptr;
        if (gta6rt_vm_reserve_range(0x10000, k4GiB, 0x10000, 0, 0, &f) != GTA6RT_OK) break;
        fill.push_back(f);
    }
    const uint64_t above = rt::xmem::low32_placed_above();
    gta6rt_last_error_set(0);
    CHECK(title_heap_request() == nullptr);
    CHECK_EQ(gta6rt_last_error_get(), 8u);
    CHECK_EQ(rt::xmem::low32_placed_above(), above);
    for (void* f : fill) gta6rt_vm_release(f);
}

TEST(xmem_address_width_field_bounds_the_reservation) {
    void* p = call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{0x20000}, 0x20402000u, uint64_t{3u << 12}, 4u);
    CHECK(p != nullptr);
    CHECK(reinterpret_cast<uintptr_t>(p) + 0x20000 <= (uint64_t{1} << 44));
    if (p) call<int>(kK, "VirtualFree", p, size_t{0}, 0x8000u);
}

TEST(xmem_32bit_heap_pages_take_slots_in_address_order) {
    constexpr uint64_t kPage = 0x10000;
    setenv("LEONIDA_XMEM_32BIT", "anywhere", 1);
    void* ph = call<void*>(kX, "XMemVirtualAlloc", static_cast<void*>(nullptr), size_t{8 * kPage}, 0x42000u, uint64_t{7}, 1u);
    unsetenv("LEONIDA_XMEM_32BIT");
    CHECK(ph != nullptr);
    if (!ph) return;
    auto* base = static_cast<uint8_t*>(ph);
    CHECK_EQ(call<int>(kK, "VirtualFree", base + 4 * kPage, size_t{4 * kPage}, 0x8002u), 1);
    CHECK(call<void*>(kX, "XMemVirtualAlloc", base, size_t{4 * kPage}, 0x20406000u, uint64_t{0x27}, 4u) == base);
    CHECK(call<void*>(kX, "XMemVirtualAlloc", base + 4 * kPage, size_t{4 * kPage}, 0x20406000u, uint64_t{0x27}, 4u) ==
          base + 4 * kPage);
    uint64_t count = 8, pages[8] = {};
    CHECK_EQ(call<int>(kX, "XMemAllocatePhysicalPages", uint64_t{0x27}, &count, pages), 1);
    CHECK(call<void*>(kX, "XMemMapPhysicalPages", static_cast<void*>(base + 4 * kPage), uint64_t{4}, static_cast<const uint64_t*>(pages)) ==
          base + 4 * kPage);
    CHECK(call<void*>(kX, "XMemMapPhysicalPages", static_cast<void*>(base), uint64_t{4}, static_cast<const uint64_t*>(pages + 4)) == base);
    const auto low = rt::xmem::low_reservations();
    CHECK_EQ(low.size(), 1u);
    if (low.size() != 1) return;
    CHECK(low[0].base == ph && low[0].size == 8 * kPage && low[0].contiguous);
    auto* stretch = static_cast<uint8_t*>(low[0].host_stretch);
    for (int i = 0; i < 4; ++i) {
        CHECK(rt::xmem::physical_page_host_address(pages[i]) == stretch + (4 + i) * kPage);
        CHECK(rt::xmem::physical_page_host_address(pages[4 + i]) == stretch + i * kPage);
    }
    for (uint64_t off = 0; off < 8 * kPage; off += 0x1234) base[off] = static_cast<uint8_t>(off * 7 + 1);
    bool same = true;
    for (uint64_t off = 0; off < 8 * kPage; off += 0x1234) same &= stretch[off] == static_cast<uint8_t>(off * 7 + 1);
    CHECK(same);
    CHECK(call<void*>(kX, "XMemMapPhysicalPages", static_cast<void*>(base), uint64_t{4}, static_cast<const uint64_t*>(pages)) == base);
    CHECK_EQ(base[0x1234], base[4 * kPage + 0x1234]);
    CHECK_EQ(call<int>(kX, "XMemFreePhysicalPages", uint64_t{8}, static_cast<const uint64_t*>(pages)), 1);
    call<int>(kK, "VirtualFree", base, size_t{0}, 0x8000u);
    call<int>(kK, "VirtualFree", base + 4 * kPage, size_t{0}, 0x8000u);
    CHECK_EQ(rt::xmem::low_reservations().size(), 0u);
}

TEST(console_profile_selection) {
    CHECK(rt::shims::select_console_profile("series-x"));
    CHECK_EQ(rt::shims::console_profile().device_type, 7u);
    CHECK_EQ(rt::shims::console_profile().title_memory_mib, 13824u);
    CHECK(rt::shims::select_console_profile("Series-S"));
    CHECK_EQ(rt::shims::console_profile().device_type, 6u);
    CHECK_EQ(rt::shims::console_profile().title_memory_mib, 7776u);
    CHECK_EQ(rt::shims::console_profile().pool_gib, 8u);
    CHECK(!rt::shims::select_console_profile("xbox-one"));
    CHECK(rt::shims::select_console_profile("auto"));
    uint64_t mem = 0;
    size_t n = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &n, nullptr, 0);
    CHECK_EQ(rt::shims::console_profile().device_type, mem <= (uint64_t{32} << 30) ? 6u : 7u);
    CHECK_EQ(gta6rt_shims_set_console_profile("bogus"), GTA6RT_E_INVALID_ARG);
    CHECK_EQ(gta6rt_shims_set_console_profile("series-x"), GTA6RT_OK);
}

TEST(kernel32_virtual_memory_surface) {
    void* p = call<void*>(kK, "VirtualAlloc", static_cast<void*>(nullptr), size_t{1 << 20}, 0x3000u | 0x10000000u | 0x40000000u, 0x4u | 0x00020000u);
    CHECK(p != nullptr);
    CHECK_EQ(reinterpret_cast<uintptr_t>(p) % 0x10000, 0u);
    std::memset(p, 7, 1 << 20);
    uint32_t old = 0;
    CHECK_EQ(call<int>(kK, "VirtualProtect", p, size_t{4096}, 0x2u, &old), 1);
    CHECK_EQ(old, 0x4u);
    CHECK_EQ(call<int>(kK, "VirtualProtect", p, size_t{4096}, 0x4u, static_cast<uint32_t*>(nullptr)), 0);
    struct Mbi {
        uint64_t base, alloc_base;
        uint32_t alloc_prot;
        uint16_t part;
        uint64_t size;
        uint32_t state, protect, type;
    } mbi{};
    CHECK_EQ(call<size_t>(kK, "VirtualQuery", p, &mbi, sizeof mbi), sizeof mbi);
    CHECK_EQ(mbi.alloc_base, reinterpret_cast<uintptr_t>(p));
    CHECK_EQ(mbi.state, 0x1000u);
    CHECK_EQ(mbi.type, 0x20000u);
    CHECK_EQ(call<size_t>(kK, "VirtualQuery", p, &mbi, size_t{8}), 0u);
    uint8_t out64[64];
    CHECK_EQ(call<size_t>(kX, "XMemVirtualQuery", static_cast<void*>(nullptr), p, out64), sizeof mbi);
    CHECK_EQ(call<int>(kK, "VirtualFree", p, size_t{4096}, 0x8000u), 0);
    CHECK_EQ(call<int>(kK, "VirtualFree", p, size_t{0}, 0x8000u), 1);

    void* r = call<void*>(kK, "VirtualAlloc", static_cast<void*>(nullptr), size_t{1 << 20}, 0x2000u, 0x1u);
    CHECK(r != nullptr);
    void* c = call<void*>(kK, "VirtualAlloc", static_cast<uint8_t*>(r) + 0x10000, size_t{0x10000}, 0x1000u, 0x4u);
    CHECK(c == static_cast<uint8_t*>(r) + 0x10000);
    static_cast<uint8_t*>(c)[5] = 9;
    CHECK_EQ(call<int>(kK, "VirtualFree", c, size_t{0x10000}, 0x4000u), 1);
    CHECK_EQ(call<int>(kK, "VirtualFree", r, size_t{0}, 0x8000u), 1);
}
