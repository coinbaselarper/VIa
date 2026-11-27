// Maps the real exe and every bundled DLL and prints section / relocation / import / TLS statistics.
// No guest code runs: thunks are synthetic addresses and call_guest only records what it would have called.

#include <gta6rt/loader.h>
#include <gta6rt/winnt_image.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "posix_ops.h"

namespace {

int g_fail = 0;
int g_pass = 0;
bool g_require_preferred = true;

void check(bool ok, const char* what, const char* detail = "") {
    std::printf("  %s  %s%s%s\n", ok ? "ok  " : "FAIL", what, *detail ? "  " : "", detail);
    ok ? ++g_pass : ++g_fail;
}

#define CHECK_EQ(a, b, what)                                                                          \
    do {                                                                                              \
        const unsigned long long va_ = static_cast<unsigned long long>(a), vb_ = static_cast<unsigned long long>(b); \
        char d_[128];                                                                                 \
        std::snprintf(d_, sizeof d_, "got %llu expected %llu", va_, vb_);                             \
        check(va_ == vb_, what, d_);                                                                  \
    } while (0)

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string flags_str(uint32_t c) {
    std::string s;
    s += (c & IMAGE_SCN_MEM_READ) ? 'r' : '-';
    s += (c & IMAGE_SCN_MEM_WRITE) ? 'w' : '-';
    s += (c & IMAGE_SCN_MEM_EXECUTE) ? 'x' : '-';
    return s;
}

void print_image(gta6rt_image* img, bool sections) {
    gta6rt_image_info info;
    gta6rt_loader_info(img, &info);
    gta6rt_image_stats st;
    gta6rt_loader_stats(img, &st);
    std::printf("  base 0x%llx  preferred 0x%llx  size 0x%llx  %s  entry %s\n", (unsigned long long)info.base,
                (unsigned long long)info.preferred_base, (unsigned long long)info.size,
                st.relocated ? "RELOCATED" : "at preferred base", info.entry ? "yes" : "none");
    std::printf("  %s  sections %u  stack reserve 0x%llx commit 0x%llx  machine 0x%x subsystem %u\n",
                (info.characteristics & IMAGE_FILE_DLL) ? "DLL" : "EXE", st.section_count,
                (unsigned long long)info.stack_reserve, (unsigned long long)info.stack_commit, info.machine,
                info.subsystem);
    if (sections) {
        for (uint32_t i = 0; i < st.section_count; ++i) {
            gta6rt_section_info s;
            gta6rt_loader_section(img, i, &s);
            std::printf("    %-8s rva 0x%08x vsize 0x%08x raw 0x%08x @0x%08x %s\n", s.name, s.rva, s.virtual_size,
                        s.raw_size, s.raw_offset, flags_str(s.characteristics).c_str());
        }
    }
    std::printf("  relocs: %llu entries (DIR64 %llu, HIGHLOW %llu, padding %llu), applied %llu\n",
                (unsigned long long)st.reloc_entries, (unsigned long long)st.reloc_dir64,
                (unsigned long long)st.reloc_highlow, (unsigned long long)st.reloc_padding,
                (unsigned long long)st.reloc_applied);
    std::printf("  imports: %u in %u DLLs (%u by ordinal, %u delay)  exports: %u (%u forwarders)\n", st.import_total,
                st.import_dlls, st.import_by_ordinal, st.import_delay, st.export_total, st.export_forwarders);
    std::printf("  exception entries: %u  TLS: %s callbacks %u index %s\n", st.exception_entries,
                st.tls_present ? "yes" : "no", st.tls_callbacks,
                st.tls_index == 0xFFFFFFFFu ? "-" : std::to_string(st.tls_index).c_str());
}

struct Served {
    std::map<std::string, std::pair<int, int>> per_dll;  // dll -> (shim-served, total)
};

gta6rt_status shim_resolver(void* user, const gta6rt_import_ref* ref, gta6rt_import_binding* out) {
    std::string d = ref->dll;
    for (auto& c : d) c = static_cast<char>(tolower(c));
    if (d == "xboxbvhbuilder.dll") {
        static uint64_t next = 0xFFFE000000000000ull;
        next += 16;
        out->address = next;
        ++*static_cast<int*>(user);
        return GTA6RT_OK;
    }
    return GTA6RT_E_NOT_FOUND;
}

std::string root_dir() {
    const char* r = std::getenv("GTA6RT_PROJECT_ROOT");
    return r ? r : GTA6RT_LOADER_TEST_ROOT;
}

bool in_image(const gta6rt_image* img, uint64_t a) {
    gta6rt_image_info i;
    gta6rt_loader_info(img, &i);
    return a >= i.base && a < i.base + i.size;
}

int exe_checks(gta6rt_image* exe, bool primary) {
    gta6rt_image_stats st;
    gta6rt_loader_stats(exe, &st);
    CHECK_EQ(st.section_count, 10, "exe: 10 sections");
    CHECK_EQ(st.reloc_entries, 598282, "exe: 598,282 relocations");
    CHECK_EQ(st.reloc_dir64, 598282, "exe: all relocations are DIR64");
    CHECK_EQ(st.reloc_highlow, 0, "exe: no HIGHLOW relocations");
    CHECK_EQ(st.import_total, 587, "exe: 587 imports");
    CHECK_EQ(st.import_dlls, 68, "exe: 68 imported DLLs");
    CHECK_EQ(st.import_by_ordinal, 24, "exe: 24 imports by ordinal");
    CHECK_EQ(st.import_delay, 0, "exe: no delay imports");
    CHECK_EQ(st.exception_entries, 299257, "exe: 299,257 RUNTIME_FUNCTION entries");
    CHECK_EQ(st.tls_present, 1, "exe: TLS directory present");
    CHECK_EQ(st.tls_callbacks, 2, "exe: 2 TLS callbacks");
    if (primary) {
        gta6rt_image_info info;
        gta6rt_loader_info(exe, &info);
        if (g_require_preferred) {
            CHECK_EQ(info.base, 0x140000000ull, "exe: mapped at the preferred base 0x140000000");
            CHECK_EQ(st.relocated, 0, "exe: relocation was not needed");
        } else if (info.base != 0x140000000ull) {
            std::printf("  note  preferred base 0x140000000 was not available here; the exe was mapped at 0x%llx and relocated\n",
                        (unsigned long long)info.base);
        }
        CHECK_EQ(info.entry - info.base, 0x56a1ec8, "exe: entry RVA 0x56a1ec8");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool use_os = false, os_probe = true;
    std::string exe_path = root_dir() + "/exe/game_scarlett_master.exe";
    std::string pkg_dir = root_dir() + "/xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK";
    bool skip_force = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--os")) use_os = true, os_probe = false;
        else if (!std::strcmp(argv[i], "--posix")) use_os = false, os_probe = false;
        else if (!std::strcmp(argv[i], "--exe") && i + 1 < argc) exe_path = argv[++i];
        else if (!std::strcmp(argv[i], "--package") && i + 1 < argc) pkg_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--skip-force-reloc")) skip_force = true;
        else if (!std::strcmp(argv[i], "--no-require-preferred")) g_require_preferred = false;
        else {
            std::fprintf(stderr, "usage: loader_pe_stats [--os|--posix] [--exe PATH] [--package DIR] [--skip-force-reloc]\n");
            return 2;
        }
    }
#if !defined(__x86_64__)
    g_require_preferred = false;
#endif
    if (os_probe) use_os = testops::os_available();
    testops::install(use_os);
    std::printf("loader_pe_stats: host ops: %s\n", testops::mode());
#if defined(__x86_64__)
    std::printf("arch x86_64\n");
#else
    std::printf("arch arm64\n");
#endif

    std::printf("\n== exe: %s\n", exe_path.c_str());
    gta6rt_image* exe = nullptr;
    gta6rt_map_options mo{};
    mo.flags = GTA6RT_MAP_ALLOW_RELOCATE;
    double t0 = now_ms();
    auto st = gta6rt_loader_map_file(exe_path.c_str(), &mo, &exe);
    if (st != GTA6RT_OK) {
        std::printf("  FAIL map_file: %s\n", gta6rt_status_name(st));
        return 1;
    }
    std::printf("  map_file %.1f ms\n", now_ms() - t0);
    t0 = now_ms();
    check(gta6rt_loader_relocate(exe) == GTA6RT_OK, "relocate");
    print_image(exe, true);
    exe_checks(exe, true);

    uint32_t rva = 0;
    check(gta6rt_loader_file_offset_to_rva(exe, 0x5646540, &rva) == GTA6RT_OK, "file offset 0x5646540 (__chkstk) maps to an RVA");
    std::printf("      file offset 0x5646540 -> rva 0x%x\n", rva);

    const void* table = nullptr;
    uint32_t count = 0;
    check(gta6rt_loader_exception_table(exe, &table, &count) == GTA6RT_OK, "exception table validated (sorted, in bounds, unwind data in image)");
    CHECK_EQ(count, 299257, "exception table count");

    gta6rt_tls_info ti;
    check(gta6rt_loader_tls_info(exe, &ti) == GTA6RT_OK, "tls_info");
    std::printf("      TLS template 0x%llx size %llu zero-fill %llu align %u index@0x%llx callbacks:", (unsigned long long)ti.template_start,
                (unsigned long long)ti.template_size, (unsigned long long)ti.zero_fill, ti.alignment, (unsigned long long)ti.index_address);
    for (int i = 0; ti.callbacks && ti.callbacks[i]; ++i) std::printf(" 0x%llx", (unsigned long long)ti.callbacks[i]);
    std::printf("\n");
    uint32_t tidx = 99;
    check(gta6rt_loader_tls_assign_index(exe, &tidx) == GTA6RT_OK && tidx == 0, "exe gets TLS index 0");
    {
        uint32_t written;
        std::memcpy(&written, reinterpret_cast<void*>(ti.index_address), 4);
        CHECK_EQ(written, 0, "_tls_index in the image holds 0");
    }

    std::printf("\n== package: %s\n", pkg_dir.c_str());
    int shim_served = 0;
    gta6rt_package_desc pd{};
    pd.size = sizeof pd;
    pd.dir_utf8 = pkg_dir.c_str();
    pd.resolver = shim_resolver;
    pd.resolver_user = &shim_served;
    pd.flags = use_os ? 0 : (GTA6RT_PKG_NO_MODULE_REGISTRATION | GTA6RT_PKG_NO_SEH_REGISTRATION);
    check(gta6rt_loader_package_init(&pd) == GTA6RT_OK, "package_init");
    check(gta6rt_loader_package_exists("VCRUNTIME140.dll") == 1, "package finds VCRUNTIME140.dll case-insensitively");

    gta6rt_bind_stats bs{};
    t0 = now_ms();
    st = gta6rt_loader_bind_imports(exe, gta6rt_loader_package_import_resolver, nullptr, &bs);
    check(st == GTA6RT_OK, "bind exe imports through the package resolver");
    std::printf("      bind %.1f ms: total %u bound %u trapped %u data %u (zero-storage %u); shim-served %d\n",
                now_ms() - t0, bs.total, bs.bound, bs.trapped, bs.data, bs.data_zero_storage, shim_served);
    CHECK_EQ(bs.total, 587, "bind: 587 imports visited");
    CHECK_EQ(bs.bound + bs.trapped + bs.data_zero_storage, 587, "bind: every import bound or trapped");

    std::map<std::string, std::array<int, 5>> per_dll;
    {
        struct Ctx {
            gta6rt_image* exe;
            std::map<std::string, std::array<int, 5>>* m;
            std::vector<gta6rt_image*> pk;
        } c{exe, &per_dll, {}};
        gta6rt_image* imgs[64];
        uint32_t n = gta6rt_loader_package_images(imgs, 64);
        for (uint32_t i = 0; i < n; ++i) c.pk.push_back(imgs[i]);
        gta6rt_loader_enumerate_imports(
            exe,
            [](void* u, const gta6rt_import_ref* r, uint64_t iat_rva) {
                auto* c = static_cast<Ctx*>(u);
                gta6rt_image_info info;
                gta6rt_loader_info(c->exe, &info);
                uint64_t v;
                std::memcpy(&v, reinterpret_cast<void*>(info.base + iat_rva), 8);
                auto& row = (*c->m)[r->dll];
                ++row[0];
                bool pkg = false;
                for (auto* p : c->pk)
                    if (in_image(p, v)) pkg = true;
                if (pkg) ++row[1];
                else if (testops::is_synthetic_thunk(v)) ++row[2];
                else if (v >= 0xFFFE000000000000ull) ++row[3];
                else ++row[4];
                return 0;
            },
            &c);
    }
    std::printf("      exe imports by DLL: total / package-DLL export / trap thunk / shim-served / other(data storage)\n");
    int tot_pkg = 0, tot_trap = 0, tot_shim = 0, tot_other = 0;
    for (auto& [d, r] : per_dll) {
        if (r[1] || r[3] || r[4])
            std::printf("        %-28s %3d  %3d  %3d  %3d  %3d\n", d.c_str(), r[0], r[1], r[2], r[3], r[4]);
        tot_pkg += r[1], tot_trap += r[2], tot_shim += r[3], tot_other += r[4];
    }
    std::printf("      other DLLs (all trapped): %d imports in %zu DLLs\n", tot_trap,
                per_dll.size() - [&] { size_t k = 0; for (auto& [d, r] : per_dll) if (r[1] || r[3] || r[4]) ++k; return k; }());
    std::printf("      sum: package %d, trap %d, shim %d, other %d\n", tot_pkg, tot_trap, tot_shim, tot_other);
    CHECK_EQ(per_dll["MSVCP140.dll"][1] + per_dll["MSVCP140.dll"][4], 113, "all 113 MSVCP140 imports served by the package DLL (incl. data)");
    CHECK_EQ(per_dll["VCRUNTIME140.dll"][1], 23, "all 23 VCRUNTIME140 imports served by the package DLL");
    CHECK_EQ(per_dll["VCRUNTIME140_1.dll"][1], 1, "VCRUNTIME140_1 import served by the package DLL");
    CHECK_EQ(per_dll["Bink2Scarlett.dll"][1], 23, "all 23 Bink2Scarlett imports served by the package DLL");
    CHECK_EQ(per_dll["libHttpClient.GDK.dll"][1], 38, "all 38 libHttpClient.GDK imports served by the package DLL");
    CHECK_EQ(per_dll["amd_fidelityfx_loader_xbox.dll"][1], 4, "all 4 amd_fidelityfx_loader imports served by the package DLL");
    CHECK_EQ(per_dll["XboxBvhBuilder.dll"][3], 7, "XboxBvhBuilder.dll imports served by the shim resolver (7)");
    CHECK_EQ(per_dll["XboxBvhBuilder.dll"][1], 0, "XboxBvhBuilder.dll not loaded from the package while a shim serves it");
    CHECK_EQ(bs.data_zero_storage, 0, "no data import fell back to zero storage");

    {
        int guessed = 0, truth = 0, disagree = 0;
        struct Ctx {
            gta6rt_image* exe;
            int *guessed, *truth, *disagree;
        } c{exe, &guessed, &truth, &disagree};
        gta6rt_image* pk[64];
        uint32_t npk = gta6rt_loader_package_images(pk, 64);
        gta6rt_loader_enumerate_imports(
            exe,
            [](void* u, const gta6rt_import_ref* r, uint64_t iat_rva) {
                auto* c = static_cast<Ctx*>(u);
                gta6rt_image_info info;
                gta6rt_loader_info(c->exe, &info);
                uint64_t v;
                std::memcpy(&v, reinterpret_cast<void*>(info.base + iat_rva), 8);
                const bool guess = r->kind_guess == GTA6RT_IMPORT_DATA;
                bool is_data = false, resolved = false;
                gta6rt_image* imgs[64];
                uint32_t n = gta6rt_loader_package_images(imgs, 64);
                for (uint32_t i = 0; i < n; ++i) {
                    gta6rt_image_info ii;
                    gta6rt_loader_info(imgs[i], &ii);
                    if (v >= ii.base && v < ii.base + ii.size) {
                        resolved = true;
                        is_data = (gta6rt_loader_rva_section_flags(imgs[i], static_cast<uint32_t>(v - ii.base)) & IMAGE_SCN_MEM_EXECUTE) == 0;
                    }
                }
                *c->guessed += guess;
                if (resolved) {
                    *c->truth += is_data;
                    if (is_data != guess) {
                        ++*c->disagree;
                        std::printf("      DISAGREE %s!%s guess=%s section=%s\n", r->dll, r->name ? r->name : "#", guess ? "data" : "code", is_data ? "data" : "code");
                    }
                }
                if (guess) std::printf("      data import: %s!%s\n", r->dll, r->name);
                return 0;
            },
            &c);
        (void)npk;
        std::printf("      %d flagged data, %d package-resolved imports land in non-executable sections, %d disagreements\n", guessed, truth, disagree);
        CHECK_EQ(guessed, 6, "exe: 6 data imports flagged (3 locale ids + 3 ios vftables; S0.3 found only 3)");
        CHECK_EQ(truth, 6, "exe: 6 package-resolved imports are data by section flags");
        CHECK_EQ(disagree, 0, "kind_guess agrees with section flags for every package-resolved exe import");
    }

    std::printf("\n== load every package DLL\n");
    std::vector<std::string> dlls;
    {
        std::string cmd = "ls '" + pkg_dir + "'";
        FILE* p = popen(cmd.c_str(), "r");
        char line[512];
        while (p && std::fgets(line, sizeof line, p)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.size() > 4 && (s.substr(s.size() - 4) == ".dll" || s.substr(s.size() - 4) == ".DLL")) dlls.push_back(s);
        }
        if (p) pclose(p);
    }
    for (auto& d : dlls) {
        gta6rt_image* img = nullptr;
        t0 = now_ms();
        st = gta6rt_loader_package_load(d.c_str(), 0, &img);
        std::printf("\n-- %s (%.1f ms)\n", d.c_str(), now_ms() - t0);
        if (st != GTA6RT_OK) {
            check(false, "package_load", gta6rt_status_name(st));
            continue;
        }
        print_image(img, false);
        gta6rt_image_stats s;
        gta6rt_loader_stats(img, &s);
        check(s.import_total > 0, "imports parsed");
        const void* t;
        uint32_t n;
        auto es = gta6rt_loader_exception_table(img, &t, &n);
        check(es == GTA6RT_OK || es == GTA6RT_E_NOT_FOUND, "exception table valid");
    }

    {
        gta6rt_image* imgs[64];
        uint32_t n = gta6rt_loader_package_images(imgs, 64);
        std::printf("\n== dependency / initialisation order (%u DLLs)\n", n);
        for (uint32_t i = 0; i < n; ++i) {
            gta6rt_image_info info;
            gta6rt_loader_info(imgs[i], &info);
            std::printf("  %2u  %-34s base 0x%llx  tls index %s\n", i, std::strrchr(info.path_utf8, '/') + 1,
                        (unsigned long long)info.base,
                        gta6rt_loader_tls_index(imgs[i]) == 0xFFFFFFFFu ? "-" : std::to_string(gta6rt_loader_tls_index(imgs[i])).c_str());
        }
        auto pos = [&](const char* nm) {
            for (uint32_t i = 0; i < n; ++i)
                if (imgs[i] == gta6rt_loader_package_find(nm)) return static_cast<int>(i);
            return -1;
        };
        check(pos("vcruntime140.dll") < pos("vcruntime140_1.dll"), "vcruntime140 initialises before vcruntime140_1 (which imports it)");
        check(pos("vcruntime140.dll") < pos("msvcp140.dll"), "vcruntime140 initialises before msvcp140");
        check(pos("msvcp140.dll") < pos("libHttpClient.GDK.dll"), "msvcp140 initialises before libHttpClient.GDK");
        check(pos("msvcp140.dll") < pos("amd_fidelityfx_upscaler_xbox.dll"), "msvcp140 initialises before the upscaler");

        std::set<uint64_t> bases;
        bool overlap = false;
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        for (uint32_t i = 0; i < n; ++i) {
            gta6rt_image_info info;
            gta6rt_loader_info(imgs[i], &info);
            for (auto& r : ranges)
                if (info.base < r.second && info.base + info.size > r.first) overlap = true;
            ranges.push_back({info.base, info.base + info.size});
        }
        check(!overlap, "no two mapped images overlap");

        testops::clear_guest_calls();
        check(gta6rt_loader_package_initialize() == GTA6RT_OK, "package_initialize (recording call_guest, no guest code runs)");
        std::printf("  recorded guest calls (target is an image address; arg1 = reason):\n");
        size_t dllmain_calls = 0, tls_calls = 0;
        for (auto& c : testops::guest_calls()) {
            char d[160];
            gta6rt_loader_describe_address(c.target, d, sizeof d);
            std::printf("    %-46s a0=0x%llx a1=%llu a2=0x%llx\n", d, (unsigned long long)c.a0, (unsigned long long)c.a1,
                        (unsigned long long)c.a2);
            gta6rt_image* ii = nullptr;
            for (uint32_t i = 0; i < n; ++i)
                if (in_image(imgs[i], c.target)) ii = imgs[i];
            if (ii && c.target == gta6rt_loader_entry(ii)) ++dllmain_calls;
            else ++tls_calls;
        }
        CHECK_EQ(dllmain_calls, n, "one DllMain(DLL_PROCESS_ATTACH) per DLL");
        check(testops::guest_calls().size() >= n, "TLS callbacks and DllMain recorded");
        (void)tls_calls;
    }

    std::printf("\n== per-thread static TLS (exe index 0 + DLL indices)\n");
    {
        gta6rt_tls_info ti2;
        gta6rt_loader_tls_info(exe, &ti2);
        void** arr = testops::attach_thread();
        check(arr != nullptr, "thread attached after every image registered its TLS with isa");
        void** slots = arr;
        check(slots[0] != nullptr, "slot 0 (exe) has a block");
        bool eq = slots[0] && std::memcmp(slots[0], reinterpret_cast<void*>(ti2.template_start), ti2.template_size) == 0;
        check(eq, "exe block starts with the TLS template bytes");
        bool zero = true;
        for (uint64_t i = 0; slots[0] && i < ti2.zero_fill; ++i)
            if (static_cast<uint8_t*>(slots[0])[ti2.template_size + i]) zero = false;
        check(zero, "exe block zero-fill area is zero");
        gta6rt_image* imgs[64];
        uint32_t n = gta6rt_loader_package_images(imgs, 64);
        int with_tls = 0;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t ix = gta6rt_loader_tls_index(imgs[i]);
            if (ix != 0xFFFFFFFFu) {
                ++with_tls;
                check(ix >= 1 && slots[ix] != nullptr, "DLL slot has a block (index >= 1)");
            }
        }
        std::printf("      %d package DLLs have static TLS\n", with_tls);
        void** arr2 = testops::attach_thread();
        check(arr2 != arr && arr2[0] != slots[0], "a second thread gets its own blocks");
        std::memset(slots[0], 0xAB, 8);
        check(std::memcmp(arr2[0], reinterpret_cast<void*>(ti2.template_start), ti2.template_size) == 0,
              "writes in one thread's block do not reach another's");
        std::vector<std::thread> th;
        std::atomic<int> okc{0};
        for (int i = 0; i < 8; ++i)
            th.emplace_back([&] {
                for (int k = 0; k < 200; ++k) {
                    testops::detach_thread(testops::attach_thread());
                }
                ++okc;
            });
        for (auto& t : th) t.join();
        CHECK_EQ(okc.load(), 8, "8 threads x 200 alloc/free cycles");
        testops::detach_thread(arr);
        testops::detach_thread(arr2);
    }

    std::printf("\n== kind_guess against the real export sections (msvcp140.dll)\n");
    {
        gta6rt_image* mp = gta6rt_loader_package_find("msvcp140.dll");
        struct Ctx {
            gta6rt_image* img;
            int data_true = 0, data_guessed = 0, mism = 0, total = 0;
        } c{mp};
        gta6rt_loader_enumerate_exports(
            mp,
            [](void* u, const char* name, uint32_t, uint32_t rva, const char* fwd) {
                auto* c = static_cast<Ctx*>(u);
                if (!name || fwd || name[0] != '?') return 0;
                ++c->total;
                const bool data = (gta6rt_loader_rva_section_flags(c->img, rva) & IMAGE_SCN_MEM_EXECUTE) == 0;
                const bool guess = gta6rt_loader_guess_import_kind(name) == GTA6RT_IMPORT_DATA;
                c->data_true += data;
                c->data_guessed += guess;
                if (data != guess) {
                    ++c->mism;
                    if (c->mism <= 5) std::printf("      mismatch: %s real=%s guess=%s\n", name, data ? "data" : "code", guess ? "data" : "code");
                }
                return 0;
            },
            &c);
        std::printf("      %d decorated exports, %d data by section flags, %d guessed data, %d mismatches\n", c.total,
                    c.data_true, c.data_guessed, c.mism);
        CHECK_EQ(c.mism, 0, "heuristic agrees with section flags for every msvcp140 export");
    }

    if (!skip_force) {
        std::printf("\n== forced relocation of a second copy of the exe\n");
        gta6rt_image* e2 = nullptr;
        gta6rt_map_options fo{};
        fo.flags = GTA6RT_MAP_FORCE_RELOCATE;
        t0 = now_ms();
        st = gta6rt_loader_map_file(exe_path.c_str(), &fo, &e2);
        check(st == GTA6RT_OK, "map_file with FORCE_RELOCATE");
        if (st == GTA6RT_OK) {
            gta6rt_image_info i1, i2;
            gta6rt_loader_info(exe, &i1);
            gta6rt_loader_info(e2, &i2);
            std::printf("      second copy at 0x%llx (delta 0x%llx)\n", (unsigned long long)i2.base,
                        (unsigned long long)(i2.base - i1.base));
            check(i2.base != 0x140000000ull, "second copy is away from the preferred base");
            check(gta6rt_loader_relocate(e2) == GTA6RT_OK, "relocate");
            std::printf("      relocate %.1f ms\n", now_ms() - t0);
            gta6rt_image_stats s2;
            gta6rt_loader_stats(e2, &s2);
            CHECK_EQ(s2.reloc_applied, 598282, "598,282 relocations applied");
            // Every DIR64 target of the relocated copy must equal the fixed-base copy's value plus delta.
            gta6rt_section_info sec;
            const int64_t delta = static_cast<int64_t>(i2.base) - static_cast<int64_t>(i1.base);
            uint64_t checked = 0, bad = 0;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(i2.base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(i2.base + dos->e_lfanew);
            const auto& rd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            const auto* blk = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(i2.base + rd.VirtualAddress);
            const auto* end = reinterpret_cast<const uint8_t*>(blk) + rd.Size;
            while (reinterpret_cast<const uint8_t*>(blk) < end) {
                const auto* e = reinterpret_cast<const uint16_t*>(blk + 1);
                for (size_t k = 0; k < (blk->SizeOfBlock - 8) / 2; ++k) {
                    if ((e[k] >> 12) != IMAGE_REL_BASED_DIR64) continue;
                    const uint32_t r = blk->VirtualAddress + (e[k] & 0xFFF);
                    uint64_t a, b;
                    std::memcpy(&a, reinterpret_cast<void*>(i1.base + r), 8);
                    std::memcpy(&b, reinterpret_cast<void*>(i2.base + r), 8);
                    ++checked;
                    if (b - a != static_cast<uint64_t>(delta)) ++bad;
                }
                blk = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(reinterpret_cast<const uint8_t*>(blk) + blk->SizeOfBlock);
            }
            (void)sec;
            CHECK_EQ(checked, 598282, "DIR64 sites compared against the fixed-base copy");
            CHECK_EQ(bad, 0, "every relocated value = fixed-base value + delta");
            gta6rt_loader_unmap(e2);
        }
    }

    std::printf("\n== finalize protections\n");
    check(gta6rt_loader_finalize_protections(exe) == GTA6RT_OK, "exe finalize_protections");
    check(testops::reserves_without_image_flag() == 0, "every image reserve carried GTA6RT_VM_IMAGE");
    std::printf("\nsummary: %d checks passed, %d failed\n", g_pass, g_fail);
    gta6rt_loader_package_shutdown();
    gta6rt_loader_unmap(exe);
    return g_fail ? 1 : 0;
}
