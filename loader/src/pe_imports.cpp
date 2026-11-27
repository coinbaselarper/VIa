#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "loader_internal.h"

using ldr::Image;

namespace {

const IMAGE_DATA_DIRECTORY* dir(const Image* img, unsigned index) {
    static const IMAGE_DATA_DIRECTORY none{};
    if (index >= img->data_dir_count) return &none;
    return &img->dirs[index];
}

bool read_cstr(const Image* img, uint64_t rva, std::string& out) {
    if (rva >= img->size) return false;
    const char* s = reinterpret_cast<const char*>(img->base + rva);
    const void* end = std::memchr(s, 0, img->size - rva);
    if (!end) return false;
    out.assign(s, static_cast<const char*>(end));
    return true;
}

// MSVC mangling puts the storage class of a variable right after the scope list ("?x@@3HA", "?id@?$ctype@_W@std@@2V0locale@2@A").
// Functions have a letter there ("@@YA", "@@QEAA"). A digit 0-3 followed by the type code (never '@') marks data;
// "@@2@" is a back-reference inside a name. vftables/vbtables ("??_7") and function-local statics ("?x@?1??f@@...") are data too. Undecorated data exports ("_Nan") cannot be told from code by name.
gta6rt_import_kind guess_kind(const char* name) {
    if (!name || name[0] != '?') return GTA6RT_IMPORT_CODE;
    if (name[1] == '?') return (name[2] == '_' && (name[3] == '7' || name[3] == '8')) ? GTA6RT_IMPORT_DATA : GTA6RT_IMPORT_CODE;
    if (const char* ls = std::strstr(name, "@?"); ls && ls[2] >= '0' && ls[2] <= '9' && ls[3] == '?' && ls[4] == '?')
        return GTA6RT_IMPORT_DATA;
    for (const char* p = std::strstr(name, "@@"); p; p = std::strstr(p + 1, "@@"))
        if (p[2] >= '0' && p[2] <= '3' && p[3] && p[3] != '@') return GTA6RT_IMPORT_DATA;
    return GTA6RT_IMPORT_CODE;
}

std::mutex g_thunk_mutex;
std::deque<std::string> g_names;
std::unordered_map<std::string, gta6rt_gva> g_trap_cache;
std::deque<void*> g_zero_blocks;

struct Walk {
    const Image* img;
    gta6rt_import_visitor fn;
    void* user;
    bool stop = false;
};

gta6rt_status walk_thunks(Walk& w, const std::string& dll, uint32_t delay, uint64_t lookup_rva, uint64_t iat_rva) {
    const Image* img = w.img;
    for (;; lookup_rva += sizeof(IMAGE_THUNK_DATA64), iat_rva += sizeof(IMAGE_THUNK_DATA64)) {
        if (!ldr::range_ok(img, lookup_rva, 8) || !ldr::range_ok(img, iat_rva, 8)) {
            ldr::log_error("%s: an import table for %s runs past the image", img->name.c_str(), dll.c_str());
            return GTA6RT_E_BAD_FORMAT;
        }
        uint64_t v;
        std::memcpy(&v, img->base + lookup_rva, 8);
        if (!v) return GTA6RT_OK;
        if (iat_rva < img->headers_size) {
            ldr::log_error("%s: an import address table lands in the headers", img->name.c_str());
            return GTA6RT_E_BAD_FORMAT;
        }
        gta6rt_import_ref ref{};
        std::string name;
        ref.dll = dll.c_str();
        ref.delay = delay;
        if (IMAGE_SNAP_BY_ORDINAL64(v)) {
            ref.ordinal = static_cast<uint16_t>(IMAGE_ORDINAL64(v));
            ref.name = nullptr;
            ref.kind_guess = GTA6RT_IMPORT_CODE;
        } else {
            if (!ldr::range_ok(img, v, 2) || !read_cstr(img, (v & 0x7fffffff) + 2, name)) {
                ldr::log_error("%s: an import name is outside the image", img->name.c_str());
                return GTA6RT_E_BAD_FORMAT;
            }
            uint16_t hint;
            std::memcpy(&hint, img->base + (v & 0x7fffffff), 2);
            ref.name = name.c_str();
            ref.hint = hint;
            ref.kind_guess = guess_kind(ref.name);
        }
        if (w.fn(w.user, &ref, iat_rva)) {
            w.stop = true;
            return GTA6RT_OK;
        }
    }
}

}  // namespace

namespace ldr {

gta6rt_gva trap_thunk(const std::string& dll, const std::string& sym) {
    const std::string key = dll + "!" + sym;
    std::lock_guard l(g_thunk_mutex);
    if (auto it = g_trap_cache.find(key); it != g_trap_cache.end()) return it->second;
    g_names.push_back(key);
    gta6rt_thunk_desc d{};
    d.kind = GTA6RT_THUNK_TRAP;
    d.fn = nullptr;
    d.user = nullptr;
    d.debug_name = g_names.back().c_str();
    gta6rt_gva addr = 0;
    if (auto st = ops().make_thunk(&d, &addr); st != GTA6RT_OK) {
        log_error("cannot create trap thunk for %s (%s)", key.c_str(), gta6rt_status_name(st));
        return 0;
    }
    g_trap_cache.emplace(key, addr);
    return addr;
}

void* zero_storage(size_t size) {
    void* p = nullptr;
    if (posix_memalign(&p, 64, size)) return nullptr;
    std::memset(p, 0, size);
    std::lock_guard l(g_thunk_mutex);
    g_zero_blocks.push_back(p);
    return p;
}

gta6rt_status enumerate_imports(const Image* img, gta6rt_import_visitor fn, void* user) {
    Walk w{img, fn, user};
    if (const auto* d = dir(img, IMAGE_DIRECTORY_ENTRY_IMPORT); d->Size) {
        for (uint64_t i = 0;; ++i) {
            const uint64_t rva = d->VirtualAddress + i * sizeof(IMAGE_IMPORT_DESCRIPTOR);
            if (!range_ok(img, rva, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
                log_error("%s: the import table runs past the image", img->name.c_str());
                return GTA6RT_E_BAD_FORMAT;
            }
            IMAGE_IMPORT_DESCRIPTOR desc;
            std::memcpy(&desc, img->base + rva, sizeof desc);
            if (!desc.Name) break;
            std::string dll;
            if (!read_cstr(img, desc.Name, dll)) {
                log_error("%s: an imported DLL name is outside the image", img->name.c_str());
                return GTA6RT_E_BAD_FORMAT;
            }
            const uint32_t lookup = desc.OriginalFirstThunk ? desc.OriginalFirstThunk : desc.FirstThunk;
            if (auto st = walk_thunks(w, dll, 0, lookup, desc.FirstThunk); st != GTA6RT_OK) return st;
            if (w.stop) return GTA6RT_OK;
        }
    }
    if (const auto* d = dir(img, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT); d->Size) {
        for (uint64_t i = 0;; ++i) {
            const uint64_t rva = d->VirtualAddress + i * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
            if (!range_ok(img, rva, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR))) {
                log_error("%s: the delay-import table runs past the image", img->name.c_str());
                return GTA6RT_E_BAD_FORMAT;
            }
            IMAGE_DELAYLOAD_DESCRIPTOR desc;
            std::memcpy(&desc, img->base + rva, sizeof desc);
            if (!desc.DllNameRVA) break;
            if (!(desc.Attributes & 1) || !desc.ImportAddressTableRVA || !desc.ImportNameTableRVA) {
                log_error("%s: unusable delay-import descriptor (attributes 0x%x)", img->name.c_str(), desc.Attributes);
                return GTA6RT_E_BAD_FORMAT;
            }
            std::string dll;
            if (!read_cstr(img, desc.DllNameRVA, dll)) {
                log_error("%s: a delay-imported DLL name is outside the image", img->name.c_str());
                return GTA6RT_E_BAD_FORMAT;
            }
            if (auto st = walk_thunks(w, dll, 1, desc.ImportNameTableRVA, desc.ImportAddressTableRVA); st != GTA6RT_OK)
                return st;
            if (w.stop) return GTA6RT_OK;
        }
    }
    return GTA6RT_OK;
}

}  // namespace ldr

extern "C" {

gta6rt_import_kind gta6rt_loader_guess_import_kind(const char* name) { return guess_kind(name); }

gta6rt_status gta6rt_loader_enumerate_imports(const gta6rt_image* img, gta6rt_import_visitor fn, void* user) {
    if (!img || !fn) return GTA6RT_E_INVALID_ARG;
    return ldr::enumerate_imports(img, fn, user);
}

gta6rt_status gta6rt_loader_bind_imports(gta6rt_image* img, gta6rt_import_resolver resolver, void* user,
                                         gta6rt_bind_stats* out_stats) {
    if (!img) return GTA6RT_E_INVALID_ARG;
    if (!img->relocated) {
        ldr::log_error("%s: bind_imports before relocate", img->name.c_str());
        return GTA6RT_E_INVALID_ARG;
    }
    if (img->finalized) return GTA6RT_E_INVALID_ARG;
    struct Ctx {
        Image* img;
        gta6rt_import_resolver resolver;
        void* user;
        gta6rt_bind_stats st;
        gta6rt_status err;
    } c{img, resolver, user, {}, GTA6RT_OK};

    auto st = ldr::enumerate_imports(
        img,
        [](void* u, const gta6rt_import_ref* ref, uint64_t iat_rva) {
            auto* c = static_cast<Ctx*>(u);
            ++c->st.total;
            gta6rt_import_binding b{};
            gta6rt_status rs = GTA6RT_E_NOT_FOUND;
            if (c->resolver) rs = c->resolver(c->user, ref, &b);
            std::string sym = ref->name ? ref->name : "#" + std::to_string(ref->ordinal);
            if (rs == GTA6RT_OK && b.address) {
                ++c->st.bound;
                if (ref->kind_guess == GTA6RT_IMPORT_DATA) ++c->st.data;
            } else if (rs != GTA6RT_OK && rs != GTA6RT_E_NOT_FOUND) {
                ldr::log_error("%s: resolver failed for %s!%s (%s)", c->img->name.c_str(), ref->dll, sym.c_str(),
                               gta6rt_status_name(rs));
                c->err = rs;
                return 1;
            } else if (ref->kind_guess == GTA6RT_IMPORT_DATA) {
                void* p = ldr::zero_storage(64);
                if (!p) {
                    c->err = GTA6RT_E_NO_MEMORY;
                    return 1;
                }
                b.address = reinterpret_cast<gta6rt_gva>(p);
                ++c->st.data;
                ++c->st.data_zero_storage;
                ldr::log_warn("%s: data import %s!%s not served; bound to zeroed host storage", c->img->name.c_str(),
                              ref->dll, sym.c_str());
            } else {
                b.address = ldr::trap_thunk(ref->dll, sym);
                if (!b.address) {
                    c->err = GTA6RT_E_NOT_IMPLEMENTED;
                    return 1;
                }
                ++c->st.trapped;
            }
            const uint64_t addr = b.address;
            std::memcpy(c->img->base + iat_rva, &addr, 8);
            return 0;
        },
        &c);
    if (out_stats) *out_stats = c.st;
    if (st != GTA6RT_OK) return st;
    if (c.err != GTA6RT_OK) return c.err;
    img->imports_bound = true;
    return GTA6RT_OK;
}

}  // extern "C"
