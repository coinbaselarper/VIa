#include "t.h"

#include <mach/task_info.h>
#include <set>
#include <unistd.h>

static uint64_t resident_bytes() {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &cnt);
    return info.resident_size;
}

static void test_large_reserve() {
    SECTION("2 x 24 GiB reserve-only windows");
    const SIZE_T big = 24ull << 30;
    uint64_t rss0 = resident_bytes();
    char* a = (char*)VirtualAlloc(nullptr, big, MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
    char* b = (char*)VirtualAlloc(nullptr, big, MEM_RESERVE, PAGE_READWRITE);
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    if (!a || !b) return;
    CHECK(((uintptr_t)a & 0xFFFF) == 0);
    CHECK(((uintptr_t)a & 0x1FFFFF) == 0);
    CHECK(((uintptr_t)b & 0xFFFF) == 0);
    uint64_t rss1 = resident_bytes();
    CHECK(rss1 < rss0 + (64ull << 20));
    MEMORY_BASIC_INFORMATION mbi;
    CHECK_EQ(VirtualQuery(a + 12345, &mbi, sizeof mbi), sizeof mbi);
    CHECK_EQ(mbi.State, MEM_RESERVE);
    CHECK(mbi.AllocationBase == a);
    CHECK(mbi.BaseAddress == a + 0x3000);
    CHECK_EQ(mbi.RegionSize, big - 0x3000);
    CHECK_EQ(mbi.Type, MEM_PRIVATE);

    size_t offs[] = {0, 12ull << 30, big - 0x1000};
    for (size_t o : offs) {
        CHECK(VirtualAlloc(a + o, 0x1000, MEM_COMMIT, PAGE_READWRITE) == a + o);
        memset(a + o, 0x5A, 0x1000);
    }
    for (size_t o : offs) CHECK(a[o] == 0x5A && a[o + 0xFFF] == 0x5A);
    CHECK_EQ(VirtualQuery(a + (12ull << 30), &mbi, sizeof mbi), sizeof mbi);
    CHECK_EQ(mbi.State, MEM_COMMIT);
    CHECK_EQ(mbi.Protect, PAGE_READWRITE);
    CHECK_EQ(mbi.RegionSize, 0x1000);
    CHECK_EQ(VirtualQuery(a + (12ull << 30) + 0x1000, &mbi, sizeof mbi), sizeof mbi);
    CHECK_EQ(mbi.State, MEM_RESERVE);
    CHECK_EQ(mbi.RegionSize, big - (12ull << 30) - 0x2000);
    CHECK(VirtualFree(a, 0, MEM_RELEASE));
    CHECK(VirtualFree(b, 0, MEM_RELEASE));
    CHECK_EQ(VirtualQuery(a, &mbi, sizeof mbi), sizeof mbi);
    CHECK_EQ(mbi.State, MEM_FREE);
}

static void test_commit_semantics() {
    SECTION("commit keeps data, decommit zeroes, errors");
    char* r = (char*)VirtualAlloc(nullptr, 1 << 20, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(r != nullptr);
    CHECK(VirtualAlloc(r, 0x8000, MEM_COMMIT, PAGE_READWRITE) == r);
    for (int i = 0; i < 0x8000; i++) r[i] = (char)(i * 7);
    CHECK(VirtualAlloc(r + 0x2000, 0x10000, MEM_COMMIT, PAGE_READWRITE) == r + 0x2000);
    bool same = true;
    for (int i = 0; i < 0x8000; i++) same &= r[i] == (char)(i * 7);
    CHECK(same);
    CHECK(r[0x10000] == 0 && r[0x11FFF] == 0);

    CHECK(VirtualFree(r + 0x1000, 0x1000, MEM_DECOMMIT));
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(r + 0x1000, &mbi, sizeof mbi);
    CHECK_EQ(mbi.State, MEM_RESERVE);
    CHECK_EQ(mbi.RegionSize, 0x1000);
    CHECK(r[0] == 0 && r[0x2000] == (char)(0x2000 * 7) && r[0xFFF] == (char)(0xFFF * 7));
    CHECK(VirtualAlloc(r + 0x1000, 1, MEM_COMMIT, PAGE_READWRITE) == r + 0x1000);
    bool zero = true;
    for (int i = 0x1000; i < 0x2000; i++) zero &= r[i] == 0;
    CHECK(zero);

    CHECK(VirtualAlloc(r + (1 << 20), 0x1000, MEM_COMMIT, PAGE_READWRITE) == nullptr);
    CHECK(VirtualFree(r, 0x1000, MEM_RELEASE) == FALSE);
    CHECK_EQ(GetLastError(), ERROR_INVALID_PARAMETER);
    CHECK(VirtualFree(r + 0x10000, 0, MEM_RELEASE) == FALSE);
    CHECK(VirtualAlloc(r, 0x1000, MEM_RESERVE, PAGE_READWRITE) == nullptr);
    CHECK_EQ(GetLastError(), ERROR_INVALID_ADDRESS);

    DWORD old = 0;
    CHECK(VirtualProtect(r, 0x3000, PAGE_READONLY, &old));
    CHECK_EQ(old, PAGE_READWRITE);
    VirtualQuery(r, &mbi, sizeof mbi);
    CHECK_EQ(mbi.Protect, PAGE_READONLY);
    CHECK_EQ(mbi.RegionSize, 0x3000);
    CHECK(VirtualProtect(r, 0x3000, PAGE_EXECUTE_READWRITE, &old));
    CHECK_EQ(old, PAGE_READONLY);
    CHECK(VirtualProtect(r + 0x20000, 0x1000, PAGE_READONLY, &old) == FALSE);
    CHECK_EQ(GetLastError(), ERROR_INVALID_ADDRESS);

    CHECK(VirtualFree(r, 0, MEM_DECOMMIT));
    VirtualQuery(r, &mbi, sizeof mbi);
    CHECK_EQ(mbi.State, MEM_RESERVE);
    CHECK_EQ(mbi.RegionSize, 1 << 20);
    CHECK(VirtualFree(r, 0, MEM_RELEASE));

    char* c = (char*)VirtualAlloc(nullptr, 5000, MEM_COMMIT, PAGE_READWRITE);
    CHECK(c != nullptr && ((uintptr_t)c & 0xFFFF) == 0);
    VirtualQuery(c, &mbi, sizeof mbi);
    CHECK_EQ(mbi.RegionSize, 0x2000);
    CHECK(VirtualAlloc(c, 0x1000, MEM_RESET, PAGE_READWRITE) == c);
    c[0] = 1;
    CHECK(VirtualFree(c, 0, MEM_RELEASE));
}

static void test_fixed() {
    SECTION("fixed-address reserve");
    char* r = (char*)VirtualAlloc(nullptr, 0x100000, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(VirtualFree(r, 0, MEM_RELEASE));
    char* f = (char*)VirtualAlloc(r + 0x1234, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(f == r);
    if (f) f[0x1234] = 1;
    CHECK(VirtualAlloc(r, 0x1000, MEM_RESERVE, PAGE_READWRITE) == nullptr);
    CHECK_EQ(GetLastError(), ERROR_INVALID_ADDRESS);
    CHECK(VirtualFree(r, 0, MEM_RELEASE));
}

static void test_subpages() {
    SECTION("4 KiB pages inside a host page");
    char* r = (char*)VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(VirtualAlloc(r + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE) == r + 0x1000);
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(r, &mbi, sizeof mbi);
    CHECK_EQ(mbi.State, MEM_RESERVE);
    CHECK_EQ(mbi.RegionSize, 0x1000);
    VirtualQuery(r + 0x1000, &mbi, sizeof mbi);
    CHECK_EQ(mbi.State, MEM_COMMIT);
    CHECK_EQ(mbi.RegionSize, 0x1000);
    VirtualAlloc(r, 0x4000, MEM_COMMIT, PAGE_READWRITE);
    memset(r, 0x11, 0x4000);
    CHECK(VirtualFree(r + 0x1000, 0x1000, MEM_DECOMMIT));
    CHECK(r[0] == 0x11 && r[0x2000] == 0x11 && r[0x3FFF] == 0x11);
    VirtualAlloc(r + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    CHECK(r[0x1000] == 0 && r[0x1FFF] == 0);
    CHECK(VirtualFree(r, 0, MEM_RELEASE));
}

static volatile int g_sink;

static void test_guard_and_write_watch() {
    SECTION("PAGE_GUARD and write watch");
    CHECK_EQ(gta6rt_os_install_fault_handler(), GTA6RT_OK);
    char* g = (char*)VirtualAlloc(nullptr, 0x10000, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD);
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(g, &mbi, sizeof mbi);
    CHECK_EQ(mbi.Protect, PAGE_READWRITE | PAGE_GUARD);
    g_sink = g[0];
    VirtualQuery(g, &mbi, sizeof mbi);
    CHECK_EQ(mbi.Protect & PAGE_GUARD, 0);
    CHECK_EQ(gta6rt_vm_handle_fault(g, 1), GTA6RT_VM_FAULT_NOT_MINE);
    VirtualFree(g, 0, MEM_RELEASE);

    const int n = 16;
    char* w = (char*)VirtualAlloc(nullptr, n * 0x1000, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    CHECK(w != nullptr);
    PVOID addrs[64];
    ULONG_PTR count = 64;
    DWORD gran = 0;
    CHECK_EQ(GetWriteWatch(0, w, n * 0x1000, addrs, &count, &gran), 0);
    CHECK_EQ(count, 0);
    CHECK_EQ(gran, 0x1000);
    g_sink = w[0x3000];
    count = 64;
    GetWriteWatch(0, w, n * 0x1000, addrs, &count, &gran);
    CHECK_EQ(count, 0);
    w[2 * 0x1000 + 5] = 1;
    w[5 * 0x1000] = 2;
    count = 64;
    CHECK_EQ(GetWriteWatch(WRITE_WATCH_FLAG_RESET, w, n * 0x1000, addrs, &count, &gran), 0);
    size_t sub = gta6rt_vm_host_page_size() / 0x1000;
    std::set<size_t> expect;
    for (size_t p : {2, 5})
        for (size_t k = 0; k < sub; k++) expect.insert((p / sub) * sub + k);
    CHECK_EQ(count, expect.size());
    std::set<size_t> got;
    for (ULONG_PTR i = 0; i < count; i++) got.insert(((char*)addrs[i] - w) / 0x1000);
    CHECK(got == expect);
    CHECK(w[2 * 0x1000 + 5] == 1 && w[5 * 0x1000] == 2);
    count = 64;
    GetWriteWatch(0, w, n * 0x1000, addrs, &count, &gran);
    CHECK_EQ(count, 0);
    w[9 * 0x1000] = 3;
    count = 64;
    GetWriteWatch(0, w, n * 0x1000, addrs, &count, &gran);
    CHECK_EQ(count, sub);
    CHECK_EQ(ResetWriteWatch(w, n * 0x1000), 0);
    count = 64;
    GetWriteWatch(0, w, n * 0x1000, addrs, &count, &gran);
    CHECK_EQ(count, 0);
    CHECK(VirtualFree(w, 0, MEM_RELEASE));
}

static void test_placeholders() {
    SECTION("placeholders");
    const SIZE_T G = 0x10000;
    char* p = (char*)VirtualAlloc2(nullptr, nullptr, 4 * G, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                                   nullptr, 0);
    CHECK(p != nullptr);
    if (!p) return;
    CHECK(VirtualAlloc(p, G, MEM_COMMIT, PAGE_READWRITE) == nullptr);
    CHECK(VirtualFree(p, G, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(p, &mbi, sizeof mbi);
    CHECK(mbi.AllocationBase == p);
    CHECK_EQ(mbi.RegionSize, G);
    VirtualQuery(p + G, &mbi, sizeof mbi);
    CHECK(mbi.AllocationBase == p + G);
    CHECK_EQ(mbi.RegionSize, 3 * G);
    CHECK(VirtualAlloc2(nullptr, p, G, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | MEM_COMMIT, PAGE_READWRITE, nullptr, 0) ==
          p);
    p[100] = 42;
    CHECK(p[100] == 42);

    CHECK(VirtualFree(p + G, G, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)G, nullptr);
    CHECK(sec != nullptr);
    char* v = (char*)MapViewOfFile3(sec, GetCurrentProcess(), p + G, 0, G, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                    nullptr, 0);
    CHECK(v == p + G);
    if (v) {
        v[0] = 7;
        VirtualQuery(v, &mbi, sizeof mbi);
        CHECK_EQ(mbi.Type, MEM_MAPPED);
        CHECK(UnmapViewOfFile2(GetCurrentProcess(), v, MEM_PRESERVE_PLACEHOLDER));
        VirtualQuery(v, &mbi, sizeof mbi);
        CHECK_EQ(mbi.State, MEM_RESERVE);
        CHECK(mbi.AllocationBase == v);
    }
    CloseHandle(sec);
    CHECK(VirtualFree(p + G, 3 * G, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS));
    VirtualQuery(p + G, &mbi, sizeof mbi);
    CHECK(mbi.AllocationBase == p + G);
    CHECK_EQ(mbi.RegionSize, 3 * G);
    CHECK(VirtualFree(p, 0, MEM_RELEASE));
    CHECK(VirtualFree(p + G, 0, MEM_RELEASE));
}

static void test_alias_and_sections() {
    SECTION("alias and pagefile sections");
    const size_t n = 0x10000;
    char* src = (char*)VirtualAlloc(nullptr, n, MEM_COMMIT, PAGE_READWRITE);
    char* dst = (char*)VirtualAlloc(nullptr, n, MEM_RESERVE, PAGE_NOACCESS);
    memset(src, 0x33, n);
    CHECK_EQ(gta6rt_vm_alias(dst, src, n, GTA6RT_PROT_READ | GTA6RT_PROT_WRITE), GTA6RT_OK);
    CHECK(dst[0] == 0x33 && dst[n - 1] == 0x33);
    src[10] = 0x44;
    CHECK(dst[10] == 0x44);
    dst[20] = 0x55;
    CHECK(src[20] == 0x55);
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(dst, &mbi, sizeof mbi);
    CHECK_EQ(mbi.State, MEM_COMMIT);
    VirtualFree(src, 0, MEM_RELEASE);
    VirtualFree(dst, 0, MEM_RELEASE);

    HANDLE s = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 1 << 20, u"gta6rt_test_section");
    CHECK(s != nullptr);
    CHECK_EQ(GetLastError(), ERROR_SUCCESS);
    HANDLE s2 = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 1 << 20, u"gta6rt_test_section");
    CHECK_EQ(GetLastError(), ERROR_ALREADY_EXISTS);
    char* v1 = (char*)MapViewOfFile(s, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    char* v2 = (char*)MapViewOfFile(s2, FILE_MAP_WRITE, 0, 0x10000, 0x10000);
    CHECK(v1 && v2);
    CHECK(((uintptr_t)v1 & 0xFFFF) == 0);
    CHECK(MapViewOfFile(s, FILE_MAP_READ, 0, 0x1000, 0x1000) == nullptr);
    if (v1 && v2) {
        v1[0x10000 + 3] = 9;
        CHECK(v2[3] == 9);
        v2[4] = 10;
        CHECK(v1[0x10004] == 10);
        VirtualQuery(v1, &mbi, sizeof mbi);
        CHECK_EQ(mbi.Type, MEM_MAPPED);
        CHECK_EQ(mbi.State, MEM_COMMIT);
        CHECK(UnmapViewOfFile(v1));
        CHECK(UnmapViewOfFile(v2));
        CHECK(UnmapViewOfFile(v2) == FALSE);
    }
    CloseHandle(s);
    CloseHandle(s2);
}

static void test_query_free_and_misc() {
    SECTION("query outside the table, heap");
    MEMORY_BASIC_INFORMATION mbi;
    int local = 0;
    CHECK_EQ(VirtualQuery(&local, &mbi, sizeof mbi), sizeof mbi);
    CHECK_EQ(mbi.State, MEM_COMMIT);
    CHECK(mbi.Protect == PAGE_READWRITE);
    CHECK_EQ(VirtualQuery((void*)0x7F0000000000ull, &mbi, sizeof mbi), sizeof mbi);
    CHECK_EQ(mbi.State, MEM_FREE);
    CHECK_EQ(VirtualQuery(&local, &mbi, 8), 0);
    char buf[16];
    SIZE_T got = 0;
    CHECK(ReadProcessMemory(GetCurrentProcess(), "hello", buf, 6, &got) && got == 6 && !strcmp(buf, "hello"));
    CHECK(ReadProcessMemory(GetCurrentProcess(), (void*)0x10, buf, 6, &got) == FALSE);
    CHECK(IsBadReadPtr(nullptr, 1));
    CHECK(!IsBadReadPtr(buf, sizeof buf));

    HANDLE h = GetProcessHeap();
    char* a = (char*)HeapAlloc(h, HEAP_ZERO_MEMORY, 100);
    CHECK(a && a[99] == 0);
    CHECK(HeapSize(h, 0, a) >= 100);
    a = (char*)HeapReAlloc(h, HEAP_ZERO_MEMORY, a, 1000);
    CHECK(a && a[999] == 0);
    CHECK(HeapFree(h, 0, a));
    bool zero_tail = true;
    for (int round = 0; round < 64; round++) {
        char* junk = (char*)malloc(112);
        memset(junk, 0xEE, 112);
        free(junk);
        char* p = (char*)HeapAlloc(h, 0, 100);
        memset(p, 0xFF, 100);
        p = (char*)HeapReAlloc(h, HEAP_ZERO_MEMORY, p, 1000);
        for (int i = 100; i < 1000; i++)
            if (p[i]) zero_tail = false;
        HeapFree(h, 0, p);
    }
    CHECK(zero_tail);
    void* m = malloc(10);
    CHECK(HeapFree(h, 0, m));
    CHECK(LocalFree(LocalAlloc(LPTR, 10)) == nullptr);
}

static void test_reserve_api() {
    SECTION("gta6rt_vm_* direct");
    void* base = nullptr;
    CHECK_EQ(gta6rt_vm_reserve(nullptr, 0x40000, GTA6RT_VM_IMAGE, &base), GTA6RT_OK);
    CHECK_EQ(gta6rt_vm_commit(base, 0x2000, GTA6RT_PROT_READ | GTA6RT_PROT_WRITE), GTA6RT_OK);
    gta6rt_vm_region r;
    CHECK_EQ(gta6rt_vm_query((char*)base + 0x1000, &r), GTA6RT_OK);
    CHECK_EQ(r.state, GTA6RT_VM_STATE_COMMITTED);
    CHECK_EQ(r.is_image, 1);
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(base, &mbi, sizeof mbi);
    CHECK_EQ(mbi.Type, MEM_IMAGE);
    void* again = nullptr;
    CHECK_EQ(gta6rt_vm_reserve(base, 0x1000, GTA6RT_VM_FIXED, &again), GTA6RT_E_ADDRESS_IN_USE);
    CHECK_EQ(gta6rt_vm_release(base), GTA6RT_OK);
    CHECK_EQ(gta6rt_vm_release(base), GTA6RT_E_NOT_FOUND);
    CHECK(gta6rt_vm_host_page_size() == (size_t)getpagesize());
}

static void test_reserve_range_and_report() {
    SECTION("reserve_range, memory stats and report");
    void* b = nullptr;
    CHECK_EQ(gta6rt_vm_reserve_range(0, 1ull << 46, 0x2000000, 0x1000000, 0, &b), GTA6RT_OK);
    const uint64_t bb = (uint64_t)(uintptr_t)b;
    CHECK(b != nullptr && (bb & 0xffffff) == 0);
    CHECK_EQ(gta6rt_vm_commit(b, 0x100000, GTA6RT_PROT_READ | GTA6RT_PROT_WRITE), GTA6RT_OK);
    if (b) memset(b, 1, 0x100000);
    void* c = nullptr;
    CHECK_EQ(gta6rt_vm_reserve_range(bb, bb + 0x1000000, 0x2000000, 0, 0, &c), GTA6RT_E_NO_MEMORY);
    void* e = nullptr;
    CHECK_EQ(gta6rt_vm_reserve_range(bb, bb + 0x10000000, 0x2000000, 0, 0, &e), GTA6RT_OK);
    const uint64_t ee = (uint64_t)(uintptr_t)e;
    CHECK(ee >= bb + 0x2000000 && ee + 0x2000000 <= bb + 0x10000000);
    if (e) gta6rt_vm_release(e);
    void* d = nullptr;
    const uint64_t lowest = gta6rt_vm_lowest_mappable();
    const bool low_ok = gta6rt_vm_reserve_range(0x10000, 1ull << 32, 0x100000, 0, 0, &d) == GTA6RT_OK;
    fprintf(stderr, "   lowest mappable %#llx, below-4-GiB reservation: %s (%p)\n", (unsigned long long)lowest,
            low_ok ? "ok" : "no room", d);
#if defined(__x86_64__)
    CHECK_EQ(lowest, 0x10000ull);
#else
    CHECK_EQ(lowest, 1ull << 32);
#endif
    CHECK_EQ(low_ok, lowest < (1ull << 32));
    if (low_ok) CHECK((uint64_t)(uintptr_t)d >= lowest && (uint64_t)(uintptr_t)d + 0x100000 <= (1ull << 32));
    if (d) gta6rt_vm_release(d);
    gta6rt_memory_stats st{};
    CHECK_EQ(gta6rt_os_memory_stats(&st), GTA6RT_OK);
    CHECK(st.resident > 0 && st.footprint > 0 && st.regions >= 1);
    CHECK(st.reserved >= 0x2000000 && st.committed >= 0x100000);
    gta6rt_os_memory_report("os_test_vm", 0x1000000, 4);
    CHECK_EQ(gta6rt_vm_release(b), GTA6RT_OK);
}

int main() {
    test_large_reserve();
    test_commit_semantics();
    test_fixed();
    test_subpages();
    test_guard_and_write_watch();
    test_placeholders();
    test_alias_and_sections();
    test_query_free_and_misc();
    test_reserve_api();
    test_reserve_range_and_report();
    return t_finish("os_test_vm");
}
