#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "loader_internal.h"

using ldr::Image;

namespace {

constexpr uint64_t kDllBase = 0x180000000ull;
constexpr uint64_t kBaseAlign = 0x10000;
alignas(16) uint8_t g_static_reserved[1232];

struct Package {
    bool inited = false;
    std::string dir;
    gta6rt_import_resolver resolver = nullptr;
    void* resolver_user = nullptr;
    uint32_t flags = 0;
    bool explicit_exclusive = false;
    std::set<std::string> exclusive;
    std::unordered_map<std::string, std::string> files;
    std::unordered_map<std::string, Image*> loaded;
    std::map<std::string, gta6rt_status> failed;
    std::vector<Image*> init_order;
    int dynamic_depth = 0;
    uint64_t highest_end = 0;
};

std::recursive_mutex g_mu;
Package g_pkg;
thread_local int t_forward_depth = 0;

std::string norm(const std::string& in) {
    size_t cut = in.find_last_of("/\\");
    std::string s = ldr::lower(cut == std::string::npos ? in : in.substr(cut + 1));
    if (s.find('.') == std::string::npos) s += ".dll";
    return s;
}

bool starts_with(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

bool is_exclusive(const std::string& n) {
    if (g_pkg.explicit_exclusive) return g_pkg.exclusive.count(n) != 0;
    return starts_with(n, "vcruntime140") || starts_with(n, "msvcp140") || starts_with(n, "concrt140") ||
           starts_with(n, "vccorlib140") || starts_with(n, "vcomp140") || starts_with(n, "bink2scarlett") ||
           starts_with(n, "libhttpclient.gdk") || starts_with(n, "xcurl") || starts_with(n, "amd_fidelityfx_");
}

void scan_dir() {
    g_pkg.files.clear();
    DIR* d = opendir(g_pkg.dir.c_str());
    if (!d) {
        ldr::log_error("package: cannot open %s", g_pkg.dir.c_str());
        return;
    }
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        const std::string l = ldr::lower(n);
        if (l.size() > 4 && l.compare(l.size() - 4, 4, ".dll") == 0) g_pkg.files[l] = n;
    }
    closedir(d);
}

struct DynamicScope {
    bool on;
    explicit DynamicScope(bool d) : on(d) {
        if (on) ++g_pkg.dynamic_depth;
    }
    ~DynamicScope() {
        if (on) --g_pkg.dynamic_depth;
    }
};

gta6rt_gva export_cb(void* user, const char* name, uint32_t ordinal) {
    return gta6rt_loader_get_export(static_cast<Image*>(user), name, ordinal);
}

gta6rt_status load_locked(const std::string& n, bool dynamic, Image** out);

gta6rt_status resolve_locked(const gta6rt_import_ref* ref, gta6rt_import_binding* out) {
    const std::string n = norm(ref->dll);
    const bool pkg_has = g_pkg.files.count(n) != 0 || g_pkg.loaded.count(n) != 0;
    if (!(pkg_has && is_exclusive(n)) && g_pkg.resolver) {
        gta6rt_status st = g_pkg.resolver(g_pkg.resolver_user, ref, out);
        if (st == GTA6RT_OK && out->address) return GTA6RT_OK;
        if (st != GTA6RT_OK && st != GTA6RT_E_NOT_FOUND) return st;
    }
    if (!pkg_has) return GTA6RT_E_NOT_FOUND;
    Image* dep = nullptr;
    if (auto it = g_pkg.loaded.find(n); it != g_pkg.loaded.end()) dep = it->second;
    else if (load_locked(n, false, &dep) != GTA6RT_OK) return GTA6RT_E_NOT_FOUND;

    const ldr::Export* e = ldr::find_export(dep, ref->name, ref->name ? 0 : ref->ordinal);
    if (!e) {
        ldr::log_warn("package copy of %s has no export %s%s", dep->name.c_str(), ref->name ? "" : "#",
                      ref->name ? ref->name : std::to_string(ref->ordinal).c_str());
        return GTA6RT_E_NOT_FOUND;
    }
    gta6rt_gva addr = e->forwarder.empty() ? reinterpret_cast<gta6rt_gva>(dep->base) + e->rva
                                           : ldr::resolve_forwarder(dep, e->forwarder);
    if (!addr) return GTA6RT_E_NOT_FOUND;
    out->address = addr;
    return GTA6RT_OK;
}

gta6rt_status finish_load(Image* img, bool dynamic) {
    const auto& flags = g_pkg.flags;
    if (auto st = gta6rt_loader_relocate(img); st != GTA6RT_OK) return st;
    if (auto st = gta6rt_loader_tls_assign_index(img, nullptr); st != GTA6RT_OK && st != GTA6RT_E_NOT_FOUND) return st;
    if (auto st = ldr::parse_exports(img); st != GTA6RT_OK) return st;
    img->in_package = true;
    img->dynamic_load = dynamic;
    g_pkg.loaded[ldr::lower(img->name)] = img;
    g_pkg.highest_end = std::max<uint64_t>(g_pkg.highest_end, reinterpret_cast<uint64_t>(img->base) + img->size);

    if (!(flags & GTA6RT_PKG_NO_MODULE_REGISTRATION)) {
        gta6rt_module_desc md{};
        md.name = img->name.c_str();
        md.path_utf8 = img->path.c_str();
        md.kind = GTA6RT_MODULE_GUEST_IMAGE;
        md.base = reinterpret_cast<gta6rt_gva>(img->base);
        md.size = img->size;
        md.get_export = export_cb;
        md.user = img;
        if (auto st = gta6rt_module_register(&md, &img->module); st != GTA6RT_OK) {
            ldr::log_error("%s: module registration failed (%s)", img->name.c_str(), gta6rt_status_name(st));
            return st;
        }
    }
    gta6rt_bind_stats bs{};
    if (auto st = gta6rt_loader_bind_imports(img, gta6rt_loader_package_import_resolver, nullptr, &bs); st != GTA6RT_OK)
        return st;
    ldr::log_info("%s: base %p%s, %u imports (%u bound, %u trapped, %u data)", img->name.c_str(),
                  static_cast<void*>(img->base), img->relocated && img->reloc_applied ? " (relocated)" : "", bs.total,
                  bs.bound, bs.trapped, bs.data);
    if (auto st = gta6rt_loader_finalize_protections(img); st != GTA6RT_OK) return st;
    if (!(flags & GTA6RT_PKG_NO_SEH_REGISTRATION)) {
        const gta6rt_runtime_function* t = nullptr;
        uint32_t n = 0;
        auto st = ldr::exception_table(img, &t, &n);
        if (st == GTA6RT_OK) {
            if (auto rs = gta6rt_seh_add_function_table(reinterpret_cast<uint64_t>(img->base), t, n, img->size);
                rs != GTA6RT_OK) {
                ldr::log_error("%s: seh table registration failed (%s)", img->name.c_str(), gta6rt_status_name(rs));
                return rs;
            }
            img->seh_registered = true;
            img->seh_table = t;
        } else if (st != GTA6RT_E_NOT_FOUND) {
            return st;
        }
    }
    g_pkg.init_order.push_back(img);
    return GTA6RT_OK;
}

gta6rt_status load_locked(const std::string& n, bool dynamic, Image** out) {
    if (auto f = g_pkg.failed.find(n); f != g_pkg.failed.end()) return f->second;
    auto fit = g_pkg.files.find(n);
    if (fit == g_pkg.files.end()) {
        scan_dir();
        fit = g_pkg.files.find(n);
        if (fit == g_pkg.files.end()) return GTA6RT_E_NOT_FOUND;
    }
    const std::string path = g_pkg.dir + "/" + fit->second;
    DynamicScope scope(dynamic);
    const bool dyn = g_pkg.dynamic_depth > 0;

    const void* data = nullptr;
    size_t size = 0;
    if (auto st = ldr::ops().file_map_readonly(path.c_str(), &data, &size); st != GTA6RT_OK) {
        ldr::log_error("package: cannot read %s (%s)", path.c_str(), gta6rt_status_name(st));
        g_pkg.failed[n] = st;
        return st;
    }
    Image* img = nullptr;
    gta6rt_status st = GTA6RT_E_ADDRESS_IN_USE;
    const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(data);
    uint64_t preferred = kDllBase;
    if (size > sizeof(IMAGE_DOS_HEADER) && dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
        static_cast<uint64_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) <= size)
        preferred = reinterpret_cast<const IMAGE_NT_HEADERS64*>(static_cast<const uint8_t*>(data) + dos->e_lfanew)
                        ->OptionalHeader.ImageBase;
    uint64_t next = std::max<uint64_t>(g_pkg.highest_end, kDllBase);
    next = ((next + kBaseAlign - 1) & ~(kBaseAlign - 1)) + kBaseAlign;
    const uint64_t candidates[] = {preferred, next, next + (1ull << 30), next + (2ull << 30), next + (3ull << 30)};
    for (uint64_t cand : candidates) {
        gta6rt_map_options opt{};
        opt.base_override = cand;
        st = gta6rt_loader_map_memory(data, size, fit->second.c_str(), &opt, &img);
        if (st != GTA6RT_E_ADDRESS_IN_USE) break;
    }
    if (st == GTA6RT_E_ADDRESS_IN_USE) {
        gta6rt_map_options opt{};
        opt.flags = GTA6RT_MAP_ALLOW_RELOCATE;
        st = gta6rt_loader_map_memory(data, size, fit->second.c_str(), &opt, &img);
    }
    ldr::ops().file_unmap(data, size);
    if (st != GTA6RT_OK) {
        ldr::log_error("package: mapping %s failed (%s); its imports will be trapped", fit->second.c_str(),
                       gta6rt_status_name(st));
        g_pkg.failed[n] = st;
        return st;
    }
    img->path = path;

    st = finish_load(img, dyn);
    if (st != GTA6RT_OK) {
        ldr::log_error("package: loading %s failed (%s); its imports will be trapped", fit->second.c_str(),
                       gta6rt_status_name(st));
        gta6rt_loader_unmap(img);
        g_pkg.failed[n] = st;
        return st;
    }
    *out = img;
    return GTA6RT_OK;
}

gta6rt_status init_one(Image* img) {
    img->init_started = true;
    if (auto st = gta6rt_loader_run_tls_callbacks(img, DLL_PROCESS_ATTACH); st != GTA6RT_OK) return st;
    if (img->is_dll() && img->entry_rva) {
        gta6rt_guest_args a{};
        a.args[0] = reinterpret_cast<uint64_t>(img->base);
        a.args[1] = DLL_PROCESS_ATTACH;
        a.args[2] = img->dynamic_load ? 0 : reinterpret_cast<uint64_t>(g_static_reserved);
        a.count = 3;
        gta6rt_guest_ret r{};
        if (auto st = ldr::ops().call_guest(reinterpret_cast<gta6rt_gva>(img->base) + img->entry_rva, &a, &r);
            st != GTA6RT_OK)
            return st;
        img->dllmain_ran = true;
        if (static_cast<uint32_t>(r.rax) == 0) {
            ldr::log_error("%s: DllMain(DLL_PROCESS_ATTACH) returned FALSE", img->name.c_str());
            img->initialised = true;
            return GTA6RT_E_IO;
        }
    }
    img->initialised = true;
    return GTA6RT_OK;
}

}  // namespace

namespace ldr {

gta6rt_gva resolve_forwarder(Image* img, const std::string& spec) {
    std::lock_guard l(g_mu);
    if (!g_pkg.inited || t_forward_depth >= 8) return 0;
    const size_t dot = spec.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= spec.size()) return 0;
    std::string module = spec.substr(0, dot);
    const std::string sym = spec.substr(dot + 1);
    if (module.find('.') == std::string::npos) module += ".dll";
    gta6rt_import_ref ref{};
    ref.dll = module.c_str();
    uint32_t ord = 0;
    if (sym.size() > 1 && sym[0] == '#') {
        for (size_t i = 1; i < sym.size(); ++i) {
            if (sym[i] < '0' || sym[i] > '9') return 0;
            ord = ord * 10 + static_cast<uint32_t>(sym[i] - '0');
            if (ord > 0xFFFF) return 0;
        }
        ref.ordinal = static_cast<uint16_t>(ord);
    } else {
        ref.name = sym.c_str();
    }
    struct Depth {
        Depth() { ++t_forward_depth; }
        ~Depth() { --t_forward_depth; }
    } depth;
    gta6rt_import_binding b{};
    if (resolve_locked(&ref, &b) != GTA6RT_OK) {
        log_warn("%s: forwarder %s did not resolve", img->name.c_str(), spec.c_str());
        return 0;
    }
    return b.address;
}

void package_unmap_hook(Image* img) {
    std::lock_guard l(g_mu);
    if (img->seh_registered) {
        gta6rt_seh_remove_function_table(static_cast<const gta6rt_runtime_function*>(img->seh_table));
        img->seh_registered = false;
    }
    if (img->module) {
        gta6rt_module_unregister(img->module);
        img->module = nullptr;
    }
    if (img->in_package) {
        g_pkg.loaded.erase(lower(img->name));
        g_pkg.init_order.erase(std::remove(g_pkg.init_order.begin(), g_pkg.init_order.end(), img),
                               g_pkg.init_order.end());
        img->in_package = false;
    }
}

}  // namespace ldr

extern "C" {

gta6rt_status gta6rt_loader_package_init(const gta6rt_package_desc* desc) {
    if (!desc || !desc->dir_utf8) return GTA6RT_E_INVALID_ARG;
    std::lock_guard l(g_mu);
    if (g_pkg.inited) return GTA6RT_E_ALREADY_EXISTS;
    g_pkg = Package{};
    g_pkg.dir = desc->dir_utf8;
    while (g_pkg.dir.size() > 1 && g_pkg.dir.back() == '/') g_pkg.dir.pop_back();
    g_pkg.resolver = desc->resolver;
    g_pkg.resolver_user = desc->resolver_user;
    g_pkg.flags = desc->flags;
    if (desc->exclusive_dlls) {
        g_pkg.explicit_exclusive = true;
        for (uint32_t i = 0; i < desc->exclusive_count; ++i) g_pkg.exclusive.insert(norm(desc->exclusive_dlls[i]));
    }
    scan_dir();
    g_pkg.inited = true;
    ldr::log_info("package: %s, %zu DLLs", g_pkg.dir.c_str(), g_pkg.files.size());
    return GTA6RT_OK;
}

void gta6rt_loader_package_shutdown(void) {
    std::lock_guard l(g_mu);
    if (!g_pkg.inited) return;
    auto order = g_pkg.init_order;
    for (auto it = order.rbegin(); it != order.rend(); ++it) gta6rt_loader_unmap(*it);
    std::vector<Image*> rest;
    for (auto& [k, v] : g_pkg.loaded) rest.push_back(v);
    for (auto* i : rest) gta6rt_loader_unmap(i);
    g_pkg = Package{};
}

gta6rt_status gta6rt_loader_package_load(const char* dll_name, uint32_t flags, gta6rt_image** out) {
    if (!dll_name) return GTA6RT_E_INVALID_ARG;
    std::lock_guard l(g_mu);
    if (!g_pkg.inited) return GTA6RT_E_INVALID_ARG;
    const std::string n = norm(dll_name);
    if (auto it = g_pkg.loaded.find(n); it != g_pkg.loaded.end()) {
        if (out) *out = it->second;
        return GTA6RT_OK;
    }
    Image* img = nullptr;
    auto st = load_locked(n, (flags & GTA6RT_PKG_LOAD_DYNAMIC) != 0, &img);
    if (st == GTA6RT_OK && out) *out = img;
    return st;
}

gta6rt_image* gta6rt_loader_package_find(const char* dll_name) {
    if (!dll_name) return nullptr;
    std::lock_guard l(g_mu);
    auto it = g_pkg.loaded.find(norm(dll_name));
    return it == g_pkg.loaded.end() ? nullptr : it->second;
}

int gta6rt_loader_package_exists(const char* dll_name) {
    if (!dll_name) return 0;
    std::lock_guard l(g_mu);
    if (!g_pkg.inited) return 0;
    const std::string n = norm(dll_name);
    if (g_pkg.loaded.count(n) || g_pkg.files.count(n)) return 1;
    scan_dir();
    return g_pkg.files.count(n) != 0;
}

gta6rt_status gta6rt_loader_package_initialize(void) {
    std::lock_guard l(g_mu);
    for (size_t i = 0; i < g_pkg.init_order.size(); ++i) {
        Image* img = g_pkg.init_order[i];
        if (img->initialised || img->init_started) continue;
        if (auto st = init_one(img); st != GTA6RT_OK) return st;
    }
    return GTA6RT_OK;
}

gta6rt_status gta6rt_loader_package_detach(void) {
    std::lock_guard l(g_mu);
    gta6rt_status first = GTA6RT_OK;
    for (auto it = g_pkg.init_order.rbegin(); it != g_pkg.init_order.rend(); ++it) {
        Image* img = *it;
        if (!img->initialised) continue;
        if (img->is_dll() && img->entry_rva && img->dllmain_ran) {
            gta6rt_guest_args a{};
            a.args[0] = reinterpret_cast<uint64_t>(img->base);
            a.args[1] = DLL_PROCESS_DETACH;
            a.args[2] = reinterpret_cast<uint64_t>(g_static_reserved);
            a.count = 3;
            gta6rt_guest_ret r{};
            auto st = ldr::ops().call_guest(reinterpret_cast<gta6rt_gva>(img->base) + img->entry_rva, &a, &r);
            if (st != GTA6RT_OK && first == GTA6RT_OK) first = st;
        }
        if (auto st = gta6rt_loader_run_tls_callbacks(img, DLL_PROCESS_DETACH); st != GTA6RT_OK && first == GTA6RT_OK)
            first = st;
        img->initialised = false;
        img->init_started = false;
    }
    return first;
}

uint32_t gta6rt_loader_package_images(gta6rt_image** out, uint32_t max) {
    std::lock_guard l(g_mu);
    const uint32_t n = static_cast<uint32_t>(g_pkg.init_order.size());
    for (uint32_t i = 0; out && i < n && i < max; ++i) out[i] = g_pkg.init_order[i];
    return n;
}

gta6rt_status gta6rt_loader_package_import_resolver(void* user, const gta6rt_import_ref* ref,
                                                    gta6rt_import_binding* out) {
    (void)user;
    if (!ref || !out || !ref->dll) return GTA6RT_E_INVALID_ARG;
    std::lock_guard l(g_mu);
    if (!g_pkg.inited) return GTA6RT_E_NOT_FOUND;
    return resolve_locked(ref, out);
}

gta6rt_status gta6rt_loader_disable_thread_notifications(gta6rt_gva base) {
    for (auto* img : ldr::registry_snapshot())
        if (reinterpret_cast<gta6rt_gva>(img->base) == base) {
            img->thread_calls_disabled = true;
            return GTA6RT_OK;
        }
    return GTA6RT_E_NOT_FOUND;
}

gta6rt_status gta6rt_loader_notify_thread(uint32_t reason) {
    if (reason != DLL_THREAD_ATTACH && reason != DLL_THREAD_DETACH) return GTA6RT_E_INVALID_ARG;
    std::vector<Image*> list;
    {
        std::lock_guard l(g_mu);
        for (auto* img : ldr::registry_snapshot())
            if (!img->in_package && img->thread_notify) list.push_back(img);
        for (auto* img : g_pkg.init_order)
            if (img->initialised) list.push_back(img);
    }
    if (reason == DLL_THREAD_DETACH) std::reverse(list.begin(), list.end());
    gta6rt_status first = GTA6RT_OK;
    for (auto* img : list) {
        gta6rt_tls_info ti;
        if (gta6rt_loader_tls_info(img, &ti) == GTA6RT_OK && ti.callbacks) {
            for (uint64_t i = 0; ti.callbacks[i]; ++i) {
                gta6rt_guest_args a{};
                a.args[0] = reinterpret_cast<uint64_t>(img->base);
                a.args[1] = reason;
                a.count = 3;
                gta6rt_guest_ret r{};
                if (auto st = ldr::ops().call_guest(ti.callbacks[i], &a, &r); st != GTA6RT_OK && first == GTA6RT_OK)
                    first = st;
            }
        }
        if (img->is_dll() && img->entry_rva && img->dllmain_ran && !img->thread_calls_disabled) {
            gta6rt_guest_args a{};
            a.args[0] = reinterpret_cast<uint64_t>(img->base);
            a.args[1] = reason;
            a.count = 3;
            gta6rt_guest_ret r{};
            if (auto st = ldr::ops().call_guest(reinterpret_cast<gta6rt_gva>(img->base) + img->entry_rva, &a, &r);
                st != GTA6RT_OK && first == GTA6RT_OK)
                first = st;
        }
    }
    return first;
}

}  // extern "C"
