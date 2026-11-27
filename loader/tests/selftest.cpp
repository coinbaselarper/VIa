// Synthetic-PE tests for the loader: relocation (incl. refusal paths), imports of every kind, exports and
// forwarders, TLS, malformed images, package policy. No guest code runs.

#include <gta6rt/loader.h>
#include <gta6rt/winnt_image.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "posix_ops.h"

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    ok ? ++g_pass : ++g_fail;
}

#define CHECK(c) check((c), #c)

constexpr uint32_t kText = 0x1000, kData = 0x2000, kImage = 0x4000, kHdr = 0x400;

struct Imp {
    std::string dll;
    std::vector<std::string> syms;
    bool delay = false;
};
struct Exp {
    std::string name;
    uint32_t rva;
    std::string fwd;
    uint32_t ordinal = 0;
};
struct Spec {
    bool dll = false;
    uint64_t base = 0x150000000ull;
    uint32_t entry_rva = 0;
    uint16_t machine = IMAGE_FILE_MACHINE_AMD64;
    std::vector<Imp> imps;
    std::vector<Exp> exps;
    bool with_relocs = true;
    std::vector<uint32_t> extra_abs64;
    std::vector<uint32_t> highlow;
    bool tls = false;
    uint32_t tls_zero = 8;
    int tls_callbacks = 1;
    uint32_t exc_entries = 0;
    bool exc_unsorted = false;
};

struct Built {
    std::vector<uint8_t> f;
    uint32_t tls_dir = 0, tls_tmpl = 0, tls_index = 0, tls_cb = 0, exc = 0, abs_site = 0, import_desc = 0;
    std::map<std::string, uint32_t> iat;
};

class Pe {
public:
    explicit Pe(const Spec& s) : s_(s), f_(kHdr + 0x1000 + 0x2000, 0) {
        for (uint32_t i = kHdr; i < kHdr + 0x1000; ++i) f_[i] = 0xCC;
    }

    Built build() {
        Built b;
        std::vector<std::pair<uint32_t, int>> relocs;
        uint32_t imp_dir = 0, imp_size = 0, dly_dir = 0, dly_size = 0, exp_dir = 0, exp_size = 0;

        auto n_in = [&](bool delay) {
            size_t n = 0;
            for (auto& i : s_.imps)
                if (i.delay == delay) ++n;
            return n;
        };
        if (n_in(false)) {
            imp_dir = alloc((n_in(false) + 1) * 20, 8);
            imp_size = static_cast<uint32_t>((n_in(false) + 1) * 20);
            b.import_desc = imp_dir;
        }
        if (n_in(true)) {
            dly_dir = alloc((n_in(true) + 1) * 32, 8);
            dly_size = static_cast<uint32_t>((n_in(true) + 1) * 32);
        }
        size_t di = 0, yi = 0;
        for (auto& imp : s_.imps) {
            const size_t n = imp.syms.size();
            const uint32_t ilt = alloc((n + 1) * 8, 8), iat = alloc((n + 1) * 8, 8);
            const uint32_t name = str(imp.dll);
            for (size_t k = 0; k < n; ++k) {
                uint64_t v;
                if (imp.syms[k][0] == '#') v = IMAGE_ORDINAL_FLAG64 | std::strtoul(imp.syms[k].c_str() + 1, nullptr, 10);
                else {
                    const uint32_t ibn = alloc(2 + imp.syms[k].size() + 1, 2);
                    w16(ibn, static_cast<uint16_t>(k));
                    strat(ibn + 2, imp.syms[k]);
                    v = ibn;
                }
                w64(ilt + 8 * k, v);
                w64(iat + 8 * k, v);
                b.iat[imp.dll + "!" + imp.syms[k]] = iat + 8 * static_cast<uint32_t>(k);
            }
            if (!imp.delay) {
                const uint32_t d = imp_dir + 20 * static_cast<uint32_t>(di++);
                w32(d + 0, ilt);
                w32(d + 12, name);
                w32(d + 16, iat);
            } else {
                const uint32_t d = dly_dir + 32 * static_cast<uint32_t>(yi++);
                w32(d + 0, 1);
                w32(d + 4, name);
                w32(d + 12, iat);
                w32(d + 16, ilt);
            }
        }
        if (!s_.exps.empty()) {
            uint32_t maxord = 0;
            std::vector<Exp> ex = s_.exps;
            uint32_t next = 1;
            for (auto& e : ex) {
                if (!e.ordinal) e.ordinal = next;
                next = std::max(next, e.ordinal) + 1;
                maxord = std::max(maxord, e.ordinal);
            }
            const uint32_t dirsz = 40;
            const uint32_t funcs = alloc(maxord * 4, 4);
            size_t named = 0;
            for (auto& e : ex)
                if (!e.name.empty()) ++named;
            const uint32_t names = alloc(named * 4 + 4, 4), ords = alloc(named * 2 + 2, 2);
            exp_dir = alloc(dirsz, 4);
            size_t ni = 0;
            for (auto& e : ex) {
                uint32_t target = e.rva;
                if (!e.fwd.empty()) target = str(e.fwd);
                w32(funcs + 4 * (e.ordinal - 1), target);
                if (!e.name.empty()) {
                    w32(names + 4 * ni, str(e.name));
                    w16(ords + 2 * ni, static_cast<uint16_t>(e.ordinal - 1));
                    ++ni;
                }
            }
            w32(exp_dir + 16, 1);
            w32(exp_dir + 20, maxord);
            w32(exp_dir + 24, static_cast<uint32_t>(named));
            w32(exp_dir + 28, funcs);
            w32(exp_dir + 32, names);
            w32(exp_dir + 36, ords);
            exp_size = cursor_ - exp_dir;
            exp_dir_start_ = exp_dir;
        }
        b.abs_site = alloc(16, 8);
        w64(b.abs_site, s_.base + kText + 0x10);
        w64(b.abs_site + 8, s_.base + kData);
        relocs.push_back({b.abs_site, 10});
        relocs.push_back({b.abs_site + 8, 10});
        for (uint32_t r : s_.extra_abs64) relocs.push_back({r, 10});
        for (uint32_t r : s_.highlow) {
            w32(r, 0x10000000u + kText);
            relocs.push_back({r, 3});
        }

        uint32_t tls_dir = 0;
        if (s_.tls) {
            b.tls_tmpl = alloc(16, 16);
            std::memcpy(&f_[off(b.tls_tmpl)], "TLSTEMPLATE_0123", 16);
            b.tls_index = alloc(4, 4);
            b.tls_cb = alloc(8 * (s_.tls_callbacks + 1), 8);
            tls_dir = alloc(40, 8);
            b.tls_dir = tls_dir;
            w64(tls_dir + 0, s_.base + b.tls_tmpl);
            w64(tls_dir + 8, s_.base + b.tls_tmpl + 16);
            w64(tls_dir + 16, s_.base + b.tls_index);
            w64(tls_dir + 24, s_.base + b.tls_cb);
            w32(tls_dir + 32, s_.tls_zero);
            w32(tls_dir + 36, 4u << 20);
            for (uint32_t o : {0u, 8u, 16u, 24u}) relocs.push_back({tls_dir + o, 10});
            for (int i = 0; i < s_.tls_callbacks; ++i) {
                w64(b.tls_cb + 8 * i, s_.base + kText + 0x30 + 0x10 * i);
                relocs.push_back({b.tls_cb + 8 * i, 10});
            }
        }
        uint32_t exc = 0;
        if (s_.exc_entries) {
            const uint32_t unw = alloc(8, 4);
            w32(unw, 0x00000001);
            exc = alloc(12 * s_.exc_entries, 4);
            b.exc = exc;
            for (uint32_t i = 0; i < s_.exc_entries; ++i) {
                uint32_t idx = s_.exc_unsorted ? s_.exc_entries - 1 - i : i;
                w32(exc + 12 * i, kText + 0x20 * idx);
                w32(exc + 12 * i + 4, kText + 0x20 * idx + 0x20);
                w32(exc + 12 * i + 8, unw);
            }
        }
        uint32_t rel_dir = 0, rel_size = 0;
        if (s_.with_relocs) {
            std::sort(relocs.begin(), relocs.end());
            std::vector<uint8_t> blob;
            size_t i = 0;
            while (i < relocs.size()) {
                const uint32_t page = relocs[i].first & ~0xFFFu;
                std::vector<uint16_t> ents;
                while (i < relocs.size() && (relocs[i].first & ~0xFFFu) == page) {
                    ents.push_back(static_cast<uint16_t>((relocs[i].second << 12) | (relocs[i].first & 0xFFF)));
                    ++i;
                }
                if (ents.size() & 1) ents.push_back(0);
                const uint32_t sz = 8 + 2 * static_cast<uint32_t>(ents.size());
                const size_t at = blob.size();
                blob.resize(at + sz);
                std::memcpy(&blob[at], &page, 4);
                std::memcpy(&blob[at + 4], &sz, 4);
                std::memcpy(&blob[at + 8], ents.data(), 2 * ents.size());
            }
            rel_dir = alloc(blob.size(), 4);
            std::memcpy(&f_[off(rel_dir)], blob.data(), blob.size());
            rel_size = static_cast<uint32_t>(blob.size());
        }

        // headers
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(&f_[0]);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(&f_[0x80]);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = s_.machine;
        nt->FileHeader.NumberOfSections = 2;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE |
                                         (s_.dll ? IMAGE_FILE_DLL : 0) | (s_.with_relocs ? 0 : IMAGE_FILE_RELOCS_STRIPPED);
        auto& oh = nt->OptionalHeader;
        oh.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        oh.AddressOfEntryPoint = s_.entry_rva;
        oh.ImageBase = s_.base;
        oh.SectionAlignment = 0x1000;
        oh.FileAlignment = 0x200;
        oh.SizeOfImage = kImage;
        oh.SizeOfHeaders = kHdr;
        oh.Subsystem = 3;
        oh.NumberOfRvaAndSizes = 16;
        auto setdir = [&](int i, uint32_t rva, uint32_t size) {
            oh.DataDirectory[i].VirtualAddress = rva;
            oh.DataDirectory[i].Size = size;
        };
        setdir(IMAGE_DIRECTORY_ENTRY_IMPORT, imp_dir, imp_size);
        setdir(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT, dly_dir, dly_size);
        if (!s_.exps.empty()) setdir(IMAGE_DIRECTORY_ENTRY_EXPORT, exp_dir_start_, exp_size);
        setdir(IMAGE_DIRECTORY_ENTRY_BASERELOC, rel_dir, rel_size);
        setdir(IMAGE_DIRECTORY_ENTRY_TLS, tls_dir, tls_dir ? 40 : 0);
        setdir(IMAGE_DIRECTORY_ENTRY_EXCEPTION, exc, 12 * s_.exc_entries);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        std::memcpy(sec[0].Name, ".text", 5);
        sec[0].Misc.VirtualSize = 0x1000;
        sec[0].VirtualAddress = kText;
        sec[0].SizeOfRawData = 0x1000;
        sec[0].PointerToRawData = kHdr;
        sec[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
        std::memcpy(sec[1].Name, ".data", 5);
        sec[1].Misc.VirtualSize = 0x2000;
        sec[1].VirtualAddress = kData;
        sec[1].SizeOfRawData = 0x2000;
        sec[1].PointerToRawData = kHdr + 0x1000;
        sec[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
        b.f = f_;
        return b;
    }

private:
    uint32_t off(uint32_t rva) const { return rva < kData ? rva - kText + kHdr : rva - kData + kHdr + 0x1000; }
    uint32_t alloc(size_t n, uint32_t align) {
        cursor_ = (cursor_ + align - 1) & ~(align - 1);
        const uint32_t r = cursor_;
        cursor_ += static_cast<uint32_t>(n);
        return r;
    }
    void strat(uint32_t rva, const std::string& s) { std::memcpy(&f_[off(rva)], s.c_str(), s.size() + 1); }
    uint32_t str(const std::string& s) {
        const uint32_t r = alloc(s.size() + 1, 1);
        strat(r, s);
        return r;
    }
    void w16(uint32_t rva, uint16_t v) { std::memcpy(&f_[off(rva)], &v, 2); }
    void w32(uint32_t rva, uint32_t v) { std::memcpy(&f_[off(rva)], &v, 4); }
    void w64(uint32_t rva, uint64_t v) { std::memcpy(&f_[off(rva)], &v, 8); }

    Spec s_;
    std::vector<uint8_t> f_;
    uint32_t cursor_ = kData;
    uint32_t exp_dir_start_ = 0, exp_size = 0;
};

Spec base_spec() {
    Spec s;
    s.imps = {{"aaa.dll", {"Alpha", "#5", "?v@@3HA"}, false}, {"bbb.dll", {"Beta"}, false}, {"ccc.dll", {"Gamma"}, true}};
    s.exps = {{"ExpA", kText + 0x10, "", 1}, {"", kText + 0x20, "", 2}, {"ExpFwd", 0, "bbb.Beta", 3}};
    s.tls = true;
    s.exc_entries = 2;
    return s;
}

uint64_t u64_at(const gta6rt_image* img, uint32_t rva) {
    gta6rt_image_info i;
    gta6rt_loader_info(img, &i);
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<void*>(i.base + rva), 8);
    return v;
}

gta6rt_status map_spec(const Built& b, const gta6rt_map_options* o, gta6rt_image** out, const char* name = "t.exe") {
    return gta6rt_loader_map_memory(b.f.data(), b.f.size(), name, o, out);
}

int g_data_target = 0;

gta6rt_status test_resolver(void*, const gta6rt_import_ref* r, gta6rt_import_binding* out) {
    if (r->name && !std::strcmp(r->name, "Alpha")) return out->address = 0x1111, GTA6RT_OK;
    if (!r->name && r->ordinal == 5) return out->address = 0x5555, GTA6RT_OK;
    if (r->name && !std::strcmp(r->name, "?v@@3HA")) return out->address = reinterpret_cast<uint64_t>(&g_data_target), GTA6RT_OK;
    if (r->name && !std::strcmp(r->name, "Gamma")) return out->address = 0x3333, GTA6RT_OK;
    return GTA6RT_E_NOT_FOUND;
}

void test_full_image() {
    std::printf("\n== synthetic exe: relocation, imports, exports, TLS, exception table\n");
    const Spec sp = base_spec();
    const Built b = Pe(sp).build();

    gta6rt_image* img = nullptr;
    gta6rt_map_options o{};
    o.flags = GTA6RT_MAP_FORCE_RELOCATE;
    CHECK(map_spec(b, &o, &img) == GTA6RT_OK);
    gta6rt_image_info info;
    gta6rt_loader_info(img, &info);
    CHECK(info.base != sp.base);
    CHECK(info.preferred_base == sp.base);
    CHECK(info.section_count == 2);
    gta6rt_image_stats st;
    gta6rt_loader_stats(img, &st);
    CHECK(st.relocated == 1);
    CHECK(st.import_total == 5 && st.import_dlls == 3 && st.import_by_ordinal == 1 && st.import_delay == 1);
    CHECK(st.export_total == 3 && st.export_forwarders == 1);
    CHECK(st.exception_entries == 2);
    CHECK(gta6rt_loader_tls_index(img) == 0xFFFFFFFFu);

    gta6rt_tls_info ti;
    CHECK(gta6rt_loader_tls_info(img, &ti) == GTA6RT_E_INVALID_ARG);
    CHECK(gta6rt_loader_bind_imports(img, test_resolver, nullptr, nullptr) == GTA6RT_E_INVALID_ARG);
    CHECK(gta6rt_loader_relocate(img) == GTA6RT_OK);
    gta6rt_loader_stats(img, &st);
    CHECK(st.reloc_applied == 2 + 4 + 1);
    CHECK(gta6rt_loader_relocate(img) == GTA6RT_OK);
    const int64_t delta = static_cast<int64_t>(info.base) - static_cast<int64_t>(sp.base);
    CHECK(u64_at(img, b.abs_site) == sp.base + kText + 0x10 + delta);
    CHECK(u64_at(img, b.abs_site + 8) == sp.base + kData + delta);

    CHECK(gta6rt_loader_tls_info(img, &ti) == GTA6RT_OK);
    CHECK(ti.template_start == info.base + b.tls_tmpl && ti.template_size == 16 && ti.zero_fill == 8);
    CHECK(ti.alignment == 8);
    CHECK(ti.callbacks && ti.callbacks[0] == info.base + kText + 0x30 && ti.callbacks[1] == 0);
    uint32_t idx = 77;
    CHECK(gta6rt_loader_tls_assign_index(img, &idx) == GTA6RT_OK && idx == 0);
    uint32_t written;
    std::memcpy(&written, reinterpret_cast<void*>(info.base + b.tls_index), 4);
    CHECK(written == 0);
    CHECK(gta6rt_loader_tls_assign_index(img, &idx) == GTA6RT_OK && idx == 0);
    CHECK(testops::registered_tls_images() == 1);
    void** arr = testops::attach_thread();
    auto* blk = static_cast<uint8_t*>(arr[0]);
    CHECK(blk && std::memcmp(blk, "TLSTEMPLATE_0123", 16) == 0);
    bool z = true;
    for (int i = 16; i < 24; ++i) z &= blk[i] == 0;
    CHECK(z);
    CHECK(reinterpret_cast<uintptr_t>(blk) % 8 == 0);

    gta6rt_bind_stats bs{};
    CHECK(gta6rt_loader_bind_imports(img, test_resolver, nullptr, &bs) == GTA6RT_OK);
    CHECK(bs.total == 5 && bs.trapped == 1 && bs.data == 1 && bs.data_zero_storage == 0 && bs.bound == 4);
    CHECK(u64_at(img, b.iat.at("aaa.dll!Alpha")) == 0x1111);
    CHECK(u64_at(img, b.iat.at("aaa.dll!#5")) == 0x5555);
    CHECK(u64_at(img, b.iat.at("aaa.dll!?v@@3HA")) == reinterpret_cast<uint64_t>(&g_data_target));
    CHECK(u64_at(img, b.iat.at("ccc.dll!Gamma")) == 0x3333);
    const uint64_t beta = u64_at(img, b.iat.at("bbb.dll!Beta"));
    CHECK(testops::is_synthetic_thunk(beta) && testops::thunk_name(beta) == "bbb.dll!Beta");

    CHECK(gta6rt_loader_get_export(img, "ExpA", 0) == info.base + kText + 0x10);
    CHECK(gta6rt_loader_get_export(img, nullptr, 2) == info.base + kText + 0x20);
    CHECK(gta6rt_loader_get_export(img, nullptr, 1) == info.base + kText + 0x10);
    CHECK(gta6rt_loader_get_export(img, "Missing", 0) == 0);
    CHECK(gta6rt_loader_get_export(img, "ExpFwd", 0) == 0);

    const void* tab;
    uint32_t cnt;
    CHECK(gta6rt_loader_exception_table(img, &tab, &cnt) == GTA6RT_OK && cnt == 2);

    uint32_t rva = 0;
    CHECK(gta6rt_loader_file_offset_to_rva(img, kHdr + 0x1010, &rva) == GTA6RT_OK && rva == kData + 0x10);
    CHECK(gta6rt_loader_file_offset_to_rva(img, 0x10, &rva) == GTA6RT_OK && rva == 0x10);
    CHECK(gta6rt_loader_file_offset_to_rva(img, 0x100000, &rva) == GTA6RT_E_NOT_FOUND);
    const uint8_t ret = 0xC3;
    CHECK(gta6rt_loader_patch_rva(img, kText + 0x40, &ret, 1) == GTA6RT_OK);
    CHECK(*reinterpret_cast<uint8_t*>(info.base + kText + 0x40) == 0xC3);
    CHECK(gta6rt_loader_patch_rva(img, kImage - 1, &ret, 2) == GTA6RT_E_INVALID_ARG);
    CHECK(gta6rt_loader_finalize_protections(img) == GTA6RT_OK);
    CHECK(gta6rt_loader_find_image(info.base + 0x100) == img);
    char d[96];
    gta6rt_loader_describe_address(info.base + 0x1234, d, sizeof d);
    CHECK(std::strcmp(d, "t.exe+0x1234") == 0);
    gta6rt_loader_unmap(img);
    CHECK(arr[0] == nullptr && testops::registered_tls_images() == 0);
    testops::detach_thread(arr);
    CHECK(gta6rt_loader_find_image(info.base + 0x100) == nullptr);

    gta6rt_image* img2 = nullptr;
    CHECK(map_spec(b, &o, &img2) == GTA6RT_OK);
    CHECK(gta6rt_loader_relocate(img2) == GTA6RT_OK);
    gta6rt_bind_stats bs2{};
    CHECK(gta6rt_loader_bind_imports(img2, nullptr, nullptr, &bs2) == GTA6RT_OK);
    CHECK(bs2.trapped == 4 && bs2.data_zero_storage == 1 && bs2.data == 1);
    const uint64_t zs = u64_at(img2, b.iat.at("aaa.dll!?v@@3HA"));
    CHECK(zs != 0 && *reinterpret_cast<uint64_t*>(zs) == 0);
    gta6rt_loader_unmap(img2);
}

void test_exe_gets_index_zero_after_dll() {
    std::printf("\n== TLS index policy: a DLL never takes index 0\n");
    Spec dsp = base_spec();
    dsp.dll = true;
    dsp.base = 0x160000000ull;
    const Built bd = Pe(dsp).build();
    const Built be = Pe(base_spec()).build();
    gta6rt_map_options o{};
    o.flags = GTA6RT_MAP_ALLOW_RELOCATE;
    gta6rt_image *dll = nullptr, *exe = nullptr;
    CHECK(map_spec(bd, &o, &dll, "d.dll") == GTA6RT_OK && gta6rt_loader_relocate(dll) == GTA6RT_OK);
    CHECK(map_spec(be, &o, &exe, "e.exe") == GTA6RT_OK && gta6rt_loader_relocate(exe) == GTA6RT_OK);
    uint32_t di = 99, ei = 99;
    CHECK(gta6rt_loader_tls_assign_index(dll, &di) == GTA6RT_OK && di == 1);
    CHECK(gta6rt_loader_tls_assign_index(exe, &ei) == GTA6RT_OK && ei == 0);
    void** arr = testops::attach_thread();
    CHECK(arr[0] && arr[1]);
    gta6rt_image* late = nullptr;
    Spec lsp = base_spec();
    lsp.dll = true;
    lsp.base = 0x170000000ull;
    CHECK(map_spec(Pe(lsp).build(), &o, &late, "late.dll") == GTA6RT_OK && gta6rt_loader_relocate(late) == GTA6RT_OK);
    uint32_t li = 99;
    CHECK(gta6rt_loader_tls_assign_index(late, &li) == GTA6RT_OK && li == 2);
    CHECK(arr[2] != nullptr);
    CHECK(std::memcmp(arr[2], "TLSTEMPLATE_0123", 16) == 0);
    gta6rt_loader_unmap(late);
    CHECK(arr[2] == nullptr);
    testops::detach_thread(arr);
    gta6rt_loader_unmap(exe);
    gta6rt_loader_unmap(dll);
}

void test_malformed() {
    std::printf("\n== malformed images are rejected without crashing\n");
    gta6rt_map_options o{};
    o.flags = GTA6RT_MAP_FORCE_RELOCATE;
    auto expect_map_fail = [&](const char* what, Built b, gta6rt_status want) {
        gta6rt_image* img = nullptr;
        auto st = map_spec(b, &o, &img);
        if (st == GTA6RT_OK) gta6rt_loader_unmap(img);
        check(st == want, what);
    };
    Built good = Pe(base_spec()).build();
    {
        Built b = good;
        b.f.resize(0x200);
        expect_map_fail("truncated file", b, GTA6RT_E_BAD_FORMAT);
    }
    expect_map_fail("not an MZ image", [&] { Built b = good; b.f[0] = 'X'; return b; }(), GTA6RT_E_BAD_FORMAT);
    {
        Spec s = base_spec();
        s.machine = IMAGE_FILE_MACHINE_ARM64;
        expect_map_fail("ARM64 machine", Pe(s).build(), GTA6RT_E_BAD_FORMAT);
    }
    {
        Spec s = base_spec();
        s.entry_rva = 0x10000;
        expect_map_fail("entry point outside the image", Pe(s).build(), GTA6RT_E_BAD_FORMAT);
    }
    {
        Built b = good;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(&b.f[0x80]);
        IMAGE_FIRST_SECTION(nt)[0].VirtualAddress = 0x100;
        expect_map_fail("section overlapping the headers", b, GTA6RT_E_BAD_FORMAT);
    }
    {
        Built b = good;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(&b.f[0x80]);
        IMAGE_FIRST_SECTION(nt)[1].PointerToRawData = 0x1000000;
        expect_map_fail("section reading past the end of the file", b, GTA6RT_E_BAD_FORMAT);
    }
    {
        Built b = good;
        reinterpret_cast<IMAGE_DOS_HEADER*>(&b.f[0])->e_lfanew = 0x7fffff00;
        expect_map_fail("e_lfanew outside the file", b, GTA6RT_E_BAD_FORMAT);
    }
    {
        Built b = good;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(&b.f[0x80]);
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = 0;
        gta6rt_image* img = nullptr;
        CHECK(map_spec(b, &o, &img) == GTA6RT_E_BAD_FORMAT);
    }
    {
        Built b = good;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(&b.f[0x80]);
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = 0x3ff0;
        gta6rt_image* img = nullptr;
        CHECK(map_spec(b, &o, &img) == GTA6RT_OK && gta6rt_loader_relocate(img) == GTA6RT_OK);
        CHECK(gta6rt_loader_bind_imports(img, test_resolver, nullptr, nullptr) == GTA6RT_E_BAD_FORMAT);
        gta6rt_loader_unmap(img);
    }
    {
        Built b = good;
        const uint32_t name_rva_slot = b.import_desc + 12;
        const uint32_t foff = name_rva_slot - kData + kHdr + 0x1000;
        const uint32_t bad = 0x9000;
        std::memcpy(&b.f[foff], &bad, 4);
        gta6rt_image* img = nullptr;
        CHECK(map_spec(b, &o, &img) == GTA6RT_OK && gta6rt_loader_relocate(img) == GTA6RT_OK);
        CHECK(gta6rt_loader_bind_imports(img, test_resolver, nullptr, nullptr) == GTA6RT_E_BAD_FORMAT);
        gta6rt_loader_unmap(img);
    }
    {
        Spec s = base_spec();
        s.highlow = {kData + 0x1800};
        Built b = Pe(s).build();
        gta6rt_image* img = nullptr;
        CHECK(map_spec(b, &o, &img) == GTA6RT_OK);
        CHECK(gta6rt_loader_relocate(img) == GTA6RT_E_BAD_FORMAT);
        gta6rt_loader_unmap(img);
    }
    {
        Built b = good;
        for (size_t i = 0; i + 4 < b.f.size(); ++i) {
            uint16_t e;
            std::memcpy(&e, &b.f[i], 2);
        }
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(&b.f[0x80]);
        const uint32_t rel = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
        uint16_t bogus = static_cast<uint16_t>((5 << 12) | 0x10);
        std::memcpy(&b.f[rel - kData + kHdr + 0x1000 + 8], &bogus, 2);
        gta6rt_image* img = nullptr;
        CHECK(map_spec(b, &o, &img) == GTA6RT_OK);
        CHECK(gta6rt_loader_relocate(img) == GTA6RT_E_BAD_FORMAT);
        gta6rt_loader_unmap(img);
    }
    {
        Built b = good;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(&b.f[0x80]);
        const uint32_t rel = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
        uint32_t page = 0;
        std::memcpy(&b.f[rel - kData + kHdr + 0x1000], &page, 4);
        gta6rt_image* img = nullptr;
        CHECK(map_spec(b, &o, &img) == GTA6RT_OK);
        CHECK(gta6rt_loader_relocate(img) == GTA6RT_E_BAD_FORMAT);
        gta6rt_loader_unmap(img);
    }
    {
        Spec s = base_spec();
        s.exc_unsorted = true;
        gta6rt_image* img = nullptr;
        CHECK(map_spec(Pe(s).build(), &o, &img) == GTA6RT_OK);
        const void* t;
        uint32_t n;
        CHECK(gta6rt_loader_exception_table(img, &t, &n) == GTA6RT_E_BAD_FORMAT);
        gta6rt_loader_unmap(img);
    }
    {
        Built b = good;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(&b.f[0x80]);
        auto& td = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        const uint32_t foff = td.VirtualAddress - kData + kHdr + 0x1000 + 16;
        uint64_t idx = 0x150000000ull + 0x40;
        std::memcpy(&b.f[foff], &idx, 8);
        gta6rt_image* img = nullptr;
        CHECK(map_spec(b, &o, &img) == GTA6RT_OK && gta6rt_loader_relocate(img) == GTA6RT_OK);
        CHECK(gta6rt_loader_tls_assign_index(img, nullptr) == GTA6RT_E_BAD_FORMAT);
        gta6rt_loader_unmap(img);
    }
    {
        Spec s = base_spec();
        s.with_relocs = false;
        Built b = Pe(s).build();
        gta6rt_image *a = nullptr, *c = nullptr;
        gta6rt_map_options po{};
        po.flags = GTA6RT_MAP_ALLOW_RELOCATE;
        CHECK(map_spec(b, &po, &a) == GTA6RT_OK && gta6rt_loader_info(a, nullptr) == GTA6RT_E_INVALID_ARG);
        CHECK(map_spec(b, &po, &c) == GTA6RT_E_BAD_FORMAT);
        po.flags = 0;
        CHECK(map_spec(b, &po, &c) == GTA6RT_E_ADDRESS_IN_USE);
        gta6rt_loader_unmap(a);
    }
}

void write_file(const std::string& p, const std::vector<uint8_t>& d) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

int g_resolver_calls = 0;
std::string g_resolver_seen;

gta6rt_status pkg_resolver(void*, const gta6rt_import_ref* r, gta6rt_import_binding* out) {
    ++g_resolver_calls;
    g_resolver_seen += std::string(r->dll) + "!" + (r->name ? r->name : "#") + ";";
    if (r->name && !std::strcmp(r->name, "Beta") && !strcasecmp(r->dll, "bbb.dll")) return out->address = 0xB0B0, GTA6RT_OK;
    if (r->name && !std::strcmp(r->name, "Kern")) return out->address = 0xCEE0, GTA6RT_OK;
    return GTA6RT_E_NOT_FOUND;
}

void test_package() {
    std::printf("\n== package: inter-DLL imports, forwarders, cycles, init order, policy\n");
    char tmpl[512];
    const char* tmp = std::getenv("TMPDIR");
    std::snprintf(tmpl, sizeof tmpl, "%s/loader_selftest.XXXXXX", tmp ? tmp : "/tmp");
    char* dir = mkdtemp(tmpl);
    if (!dir) {
        check(false, "mkdtemp");
        return;
    }
    const std::string d = dir;

    Spec a;
    a.dll = true;
    a.base = 0x180000000ull;
    a.entry_rva = kText + 0x40;
    a.exps = {{"ExpA", kText + 0x10, "", 1}, {"ExpFwd", 0, "bbb.Beta", 2}, {"ExpOrd", 0, "bbb.#9", 3}};
    a.imps = {{"BBB.dll", {"Beta", "#9"}, false}, {"kernel32.dll", {"Kern"}, false}};
    a.tls = true;
    a.tls_callbacks = 1;
    a.exc_entries = 1;
    Spec bs = a;
    bs.exps = {{"Beta", kText + 0x10, "", 1}, {"", kText + 0x20, "", 9}};
    bs.imps = {{"aaa.dll", {"ExpA"}, false}};
    bs.tls = false;
    bs.entry_rva = kText + 0x50;
    write_file(d + "/Aaa.dll", Pe(a).build().f);
    write_file(d + "/bbb.dll", Pe(bs).build().f);
    write_file(d + "/notadll.txt", {1, 2, 3});

    testops::clear_guest_calls();
    gta6rt_package_desc pd{};
    pd.size = sizeof pd;
    pd.dir_utf8 = d.c_str();
    pd.resolver = pkg_resolver;
    pd.flags = GTA6RT_PKG_NO_MODULE_REGISTRATION | GTA6RT_PKG_NO_SEH_REGISTRATION;
    const char* excl[] = {"aaa.dll"};
    pd.exclusive_dlls = excl;
    pd.exclusive_count = 1;
    CHECK(gta6rt_loader_package_init(&pd) == GTA6RT_OK);
    CHECK(gta6rt_loader_package_init(&pd) == GTA6RT_E_ALREADY_EXISTS);
    CHECK(gta6rt_loader_package_exists("AAA") == 1 && gta6rt_loader_package_exists("bbb.DLL") == 1);
    CHECK(gta6rt_loader_package_exists("notadll") == 0 && gta6rt_loader_package_exists("zzz.dll") == 0);

    Spec es = base_spec();
    es.imps = {{"AAA.dll", {"ExpA", "ExpFwd", "ExpOrd", "Missing"}, false}, {"bbb.dll", {"Beta"}, false}};
    const Built be = Pe(es).build();
    gta6rt_image* exe = nullptr;
    gta6rt_map_options o{};
    o.flags = GTA6RT_MAP_ALLOW_RELOCATE;
    CHECK(map_spec(be, &o, &exe) == GTA6RT_OK && gta6rt_loader_relocate(exe) == GTA6RT_OK);
    gta6rt_bind_stats st{};
    CHECK(gta6rt_loader_bind_imports(exe, gta6rt_loader_package_import_resolver, nullptr, &st) == GTA6RT_OK);

    gta6rt_image* ia = gta6rt_loader_package_find("aaa.dll");
    gta6rt_image* ib = gta6rt_loader_package_find("BBB.DLL");
    CHECK(ia && ib && ia != ib);
    gta6rt_image_info iai, ibi;
    gta6rt_loader_info(ia, &iai);
    gta6rt_loader_info(ib, &ibi);
    CHECK(iai.base != ibi.base);
#if defined(__x86_64__)
    CHECK(iai.base == 0x180000000ull || ibi.base == 0x180000000ull);
#endif
    CHECK(iai.base % 0x10000 == 0 && ibi.base % 0x10000 == 0);

    CHECK(u64_at(exe, Pe(es).build().iat.at("AAA.dll!ExpA")) == iai.base + kText + 0x10);
    CHECK(u64_at(exe, Pe(es).build().iat.at("bbb.dll!Beta")) == 0xB0B0);
    CHECK(u64_at(exe, Pe(es).build().iat.at("AAA.dll!ExpFwd")) == 0xB0B0);
    CHECK(u64_at(exe, Pe(es).build().iat.at("AAA.dll!ExpOrd")) == ibi.base + kText + 0x20);
    CHECK(testops::is_synthetic_thunk(u64_at(exe, Pe(es).build().iat.at("AAA.dll!Missing"))));
    CHECK(st.trapped == 1);
    CHECK(g_resolver_seen.find("AAA.dll!ExpA") == std::string::npos);
    CHECK(g_resolver_seen.find("bbb.dll!Beta") != std::string::npos);
    CHECK(g_resolver_seen.find("kernel32.dll!Kern") != std::string::npos);

    const Built ba = Pe(a).build();
    CHECK(u64_at(ia, ba.iat.at("BBB.dll!Beta")) == 0xB0B0);
    CHECK(u64_at(ia, ba.iat.at("BBB.dll!#9")) == ibi.base + kText + 0x20);
    CHECK(u64_at(ia, ba.iat.at("kernel32.dll!Kern")) == 0xCEE0);
    const Built bb = Pe(bs).build();
    CHECK(u64_at(ib, bb.iat.at("aaa.dll!ExpA")) == iai.base + kText + 0x10);
    CHECK(gta6rt_loader_get_export(ia, "ExpFwd", 0) == 0xB0B0);
    CHECK(gta6rt_loader_get_export(ia, nullptr, 3) == ibi.base + kText + 0x20);

    gta6rt_image* order[8];
    CHECK(gta6rt_loader_package_images(order, 8) == 2);
    CHECK(gta6rt_loader_tls_index(ia) != 0xFFFFFFFFu && gta6rt_loader_tls_index(ia) >= 1);
    CHECK(gta6rt_loader_package_initialize() == GTA6RT_OK);
    const auto& calls = testops::guest_calls();
    int dllmain_a = 0, dllmain_b = 0, tls_a = 0;
    for (auto& c : calls) {
        if (c.target == gta6rt_loader_entry(ia)) ++dllmain_a, CHECK(c.a1 == 1 && c.a2 != 0);
        else if (c.target == gta6rt_loader_entry(ib)) ++dllmain_b;
        else if (c.a1 == 1) ++tls_a;
    }
    CHECK(dllmain_a == 1 && dllmain_b == 1 && tls_a == 1);
    CHECK(gta6rt_loader_package_initialize() == GTA6RT_OK);
    CHECK(testops::guest_calls().size() == calls.size());

    std::printf("  thread notifications\n");
    testops::clear_guest_calls();
    CHECK(gta6rt_loader_notify_thread(DLL_THREAD_ATTACH) == GTA6RT_OK);
    CHECK(testops::guest_calls().size() == 3);
    CHECK(gta6rt_loader_disable_thread_notifications(iai.base) == GTA6RT_OK);
    testops::clear_guest_calls();
    CHECK(gta6rt_loader_notify_thread(DLL_THREAD_DETACH) == GTA6RT_OK);
    size_t a_dllmain = 0, a_tls = 0;
    for (auto& c : testops::guest_calls()) {
        if (c.target == gta6rt_loader_entry(ia)) ++a_dllmain;
        if (c.target == iai.base + kText + 0x30) ++a_tls;
    }
    CHECK(a_dllmain == 0 && a_tls == 1);
    CHECK(gta6rt_loader_notify_thread(7) == GTA6RT_E_INVALID_ARG);

    gta6rt_loader_unmap(exe);
    gta6rt_loader_package_shutdown();
    CHECK(gta6rt_loader_find_image(iai.base + 0x10) == nullptr);

    std::printf("  dynamic load and resolver-first policy\n");
    pd.exclusive_dlls = nullptr;
    pd.exclusive_count = 0;
    g_resolver_seen.clear();
    CHECK(gta6rt_loader_package_init(&pd) == GTA6RT_OK);
    gta6rt_image* dyn = nullptr;
    CHECK(gta6rt_loader_package_load("bbb", GTA6RT_PKG_LOAD_DYNAMIC, &dyn) == GTA6RT_OK && dyn);
    CHECK(gta6rt_loader_package_find("aaa.dll") != nullptr);
    testops::clear_guest_calls();
    CHECK(gta6rt_loader_package_initialize() == GTA6RT_OK);
    gta6rt_image_info dinfo;
    gta6rt_loader_info(dyn, &dinfo);
    bool saw_dyn_null = false;
    for (auto& c : testops::guest_calls())
        if (c.target == dinfo.entry && c.a2 == 0) saw_dyn_null = true;
    CHECK(saw_dyn_null);
    CHECK(gta6rt_loader_package_load("missing.dll", 0, nullptr) == GTA6RT_E_NOT_FOUND);
    gta6rt_loader_package_shutdown();

    std::printf("  a DLL that cannot be loaded degrades to trap thunks\n");
    write_file(d + "/broken.dll", std::vector<uint8_t>(0x300, 0));
    Spec cs = base_spec();
    cs.imps = {{"broken.dll", {"X"}, false}};
    cs.tls = false;
    CHECK(gta6rt_loader_package_init(&pd) == GTA6RT_OK);
    gta6rt_image* ce = nullptr;
    CHECK(map_spec(Pe(cs).build(), &o, &ce) == GTA6RT_OK && gta6rt_loader_relocate(ce) == GTA6RT_OK);
    gta6rt_bind_stats cb{};
    CHECK(gta6rt_loader_bind_imports(ce, gta6rt_loader_package_import_resolver, nullptr, &cb) == GTA6RT_OK && cb.trapped == 1);
    CHECK(gta6rt_loader_package_load("broken.dll", 0, nullptr) == GTA6RT_E_BAD_FORMAT);
    CHECK(gta6rt_loader_package_load("BROKEN.DLL", GTA6RT_PKG_LOAD_DYNAMIC, nullptr) == GTA6RT_E_BAD_FORMAT);
    CHECK(testops::file_map_count("/broken.dll") == 1);
    gta6rt_loader_unmap(ce);
    gta6rt_loader_package_shutdown();

    std::printf("  a DLL that cannot be read is remembered too (no re-read per import)\n");
    write_file(d + "/noread.dll", std::vector<uint8_t>(0x300, 0));
    chmod((d + "/noread.dll").c_str(), 0);
    if (access((d + "/noread.dll").c_str(), R_OK) != 0) {
        Spec rs = base_spec();
        rs.imps = {{"noread.dll", {"X", "Y", "Z"}, false}};
        rs.tls = false;
        CHECK(gta6rt_loader_package_init(&pd) == GTA6RT_OK);
        gta6rt_image* re = nullptr;
        CHECK(map_spec(Pe(rs).build(), &o, &re) == GTA6RT_OK && gta6rt_loader_relocate(re) == GTA6RT_OK);
        gta6rt_bind_stats rb{};
        CHECK(gta6rt_loader_bind_imports(re, gta6rt_loader_package_import_resolver, nullptr, &rb) == GTA6RT_OK &&
              rb.trapped == 3);
        CHECK(gta6rt_loader_package_load("noread.dll", 0, nullptr) != GTA6RT_OK);
        CHECK(testops::file_map_count("/noread.dll") == 1);
        gta6rt_loader_unmap(re);
        gta6rt_loader_package_shutdown();
    } else {
        std::printf("  (skipped: file stays readable, running as root?)\n");
    }

    std::string cmd = "rm -rf '" + d + "'";
    (void)!std::system(cmd.c_str());
}

void test_misc() {
    std::printf("\n== diagnostics and helpers\n");
    uint32_t t[8], n = 0;
    CHECK(gta6rt_loader_parse_stack_times("5,10,5,30", t, 8, &n) == 0 && n == 3 && t[0] == 5 && t[1] == 10 && t[2] == 30);
    CHECK(gta6rt_loader_parse_stack_times("30,x", t, 8, &n) != 0);
    CHECK(gta6rt_loader_parse_stack_times("", t, 8, &n) != 0);
    CHECK(gta6rt_loader_parse_stack_times("1,,2", t, 8, &n) != 0);
    CHECK(gta6rt_loader_guess_import_kind("?_Id_cnt@id@locale@std@@0HA") == GTA6RT_IMPORT_DATA);
    CHECK(gta6rt_loader_guess_import_kind("??_7ios_base@std@@6B@") == GTA6RT_IMPORT_DATA);
    CHECK(gta6rt_loader_guess_import_kind("?id@?$codecvt@DDU_Mbstatet@@@std@@2V0locale@2@A") == GTA6RT_IMPORT_DATA);
    CHECK(gta6rt_loader_guess_import_kind("??0?$basic_iostream@DU?$char_traits@D@std@@@std@@QEAA@PEAV?$basic_streambuf@DU?$char_traits@D@std@@@1@@Z") == GTA6RT_IMPORT_CODE);
    CHECK(gta6rt_loader_guess_import_kind("?tellg@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAA?AV?$fpos@U_Mbstatet@@@2@XZ") == GTA6RT_IMPORT_CODE);
    CHECK(gta6rt_loader_guess_import_kind("memcpy") == GTA6RT_IMPORT_CODE);
    char buf[64];
    gta6rt_loader_describe_address(0x1234, buf, sizeof buf);
    CHECK(std::strcmp(buf, "0x1234") == 0);
    gta6rt_loader_set_stack_provider(nullptr, nullptr);
    CHECK(gta6rt_loader_start_stack_sampler() == GTA6RT_OK);
    gta6rt_loader_stop_stack_sampler();
}

}  // namespace

int main(int argc, char** argv) {
    bool use_os = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--os")) use_os = true;
    if (!use_os) use_os = testops::os_available();
    testops::install(use_os);
    std::printf("loader_selftest: host ops: %s\n", testops::mode());
    test_full_image();
    test_exe_gets_index_zero_after_dll();
    test_malformed();
    test_package();
    test_misc();
    CHECK(testops::reserves_with_image_flag() > 0 && testops::reserves_without_image_flag() == 0);
    std::printf("\nloader_selftest: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
