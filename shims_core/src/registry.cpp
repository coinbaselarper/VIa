#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unistd.h>
#include <vector>

#include <gta6rt/isa.h>
#include <gta6rt/os.h>

#include "guest_address.h"
#include "console_registry.h"
#include "shims.h"
#include "util.h"

namespace rt::shims {
namespace {

struct Registry {
    std::shared_mutex mutex;
    std::map<std::string, Found> rows;  // "dll!name"
    std::map<std::string, size_t> per_dll;
};

Registry& registry() {
    static auto* r = new Registry;
    return *r;
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

const char* family_of(const std::string& dll) {
    if (starts_with(dll, "api-ms-win-crt-") || dll == "ucrtbase.dll" || dll == "ucrtbased.dll") return "ucrtbase.dll";
    if (starts_with(dll, "api-ms-win-") || starts_with(dll, "ext-ms-win-") || dll == "kernelbase.dll") return "kernel32.dll";
    return nullptr;
}

std::mutex g_called_mutex;
std::set<std::string> g_called;
std::atomic<int> g_policy{-1};

constexpr int kStopExitCode = 3;

struct Lookup {
    void* ctx;
    ModuleLookup fn;
};
std::mutex g_lookup_mutex;
std::vector<Lookup> g_lookups;

const char* const kNotHResult[] = {
    "XAsyncCancel", "XAsyncComplete",
    "XTaskQueueCloseHandle", "XTaskQueueDispatch", "XTaskQueueGetCurrentProcessTaskQueue",
    "XTaskQueueSetCurrentProcessTaskQueue", "XTaskQueueUnregisterMonitor", "XTaskQueueUnregisterWaiter",
    "XUserCloseHandle", "XUserCloseSignOutDeferralHandle", "XUserCompare", "XUserIsStoreUser",
    "XUserUnregisterForChangeEvent", "XUserUnregisterForDefaultAudioEndpointUtf16Changed",
    "XUserUnregisterForDeviceAssociationChanged",
    "XStoreCloseContextHandle", "XStoreCloseLicenseHandle", "XStoreCloseProductsQueryHandle", "XStoreIsLicenseValid",
    "XStoreProductsQueryHasMorePages", "XStoreUnregisterGameLicenseChanged", "XStoreUnregisterPackageLicenseLost",
    "XGameSaveCloseContainer", "XGameSaveCloseProvider", "XGameSaveCloseUpdate",
    "XPackageCloseInstallationMonitorHandle", "XPackageCloseMountHandle", "XPackageGetInstallationProgress",
    "XPackageIsPackagedProcess", "XPackageUninstallPackage", "XPackageUnregisterInstallationProgressChanged",
    "XPackageUnregisterPackageInstalled", "XPackageUpdateInstallationMonitor",
};
const char* const kXFamilies[] = {"XAsync", "XTaskQueue", "XUser", "XStore", "XGameSave", "XPackage", "XGameUi"};

bool is_audio_hal_hresult(const std::string& dll, const std::string& name) {
    if (dll == "chad.dll") return starts_with(name, "XDsp");
    if (dll == "xapu.dll") return name != "XApuGetResultCount" && starts_with(name, "XApu");
    if (dll == "acphal.dll")
        return name == "AcpHalAllocateShapeContexts" || name == "AcpHalCreate" || name == "AcpHalReleaseShapeContexts" ||
               name == "ApuAlloc" || name == "ApuCreateHeap";
    return false;
}

ReturnClass classify(const std::string& dll, const std::string& name) {
    if (dll == "d3d12.dll" || dll == "d3d12_xs.dll") {
        if (starts_with(name, "D3D12Create") || starts_with(name, "D3D12Serialize") ||
            name == "D3D12GetDebugInterface" || name == "D3D12EnableExperimentalFeatures" ||
            name == "D3D12XboxCreateDevice" || name == "D3D12XboxCreateSyntheticDevice")
            return ReturnClass::HResult;
    } else if (dll == "dxgi.dll") {
        if (starts_with(name, "CreateDXGIFactory") || name == "DXGIGetDebugInterface1" ||
            name == "DXGIDeclareAdapterRemovalSupport")
            return ReturnClass::HResult;
    } else if (dll == "xaudio2_9.dll" || dll == "xaudio2_9redist.dll") {
        if (name == "XAudio2Create" || name == "#1") return ReturnClass::HResult;
    } else if (dll == "gameinput.dll") {
        if (name == "GameInputCreate" || name == "#1") return ReturnClass::HResult;
    } else if (dll == "mfplat.dll" || dll == "mfreadwrite.dll" || dll == "mf.dll") {
        if (starts_with(name, "MF") && name != "MFGetSystemTime") return ReturnClass::HResult;
    } else if (starts_with(dll, "api-ms-win-crt-") || dll == "ucrtbase.dll") {
        const bool counted = name.find("printf") != std::string::npos || name.find("scanf") != std::string::npos ||
                             starts_with(name, "strnlen") || starts_with(name, "wcsnlen");
        if (name.size() > 2 && name.compare(name.size() - 2, 2, "_s") == 0 && !counted) return ReturnClass::Errno;
    } else if (dll == "dstorage.dll" || dll == "dstorage_xs.dll" || dll == "xcrdapi.dll") {
        if (name == "DStorageGetFactory") return ReturnClass::HResult;
    } else if (is_audio_hal_hresult(dll, name)) {
        return ReturnClass::HResult;
    } else if (dll == "xgameruntime.dll") {
        if (name == "XGameRuntimeInitialize" || name == "XGameEventWrite") return ReturnClass::HResult;
        for (const char* n : kNotHResult)
            if (name == n) return ReturnClass::Other;
        for (const char* f : kXFamilies)
            if (starts_with(name, f)) return ReturnClass::HResult;
    }
    return ReturnClass::Other;
}

struct UnimplCtx {
    std::string what;
    ReturnClass cls;
    std::atomic<uint64_t> calls{0};
};

std::atomic<size_t> g_unimpl_calls{0};
std::mutex g_unimpl_list_mutex;
std::vector<const UnimplCtx*> g_unimpl_called;

constexpr uint32_t kErrorCallNotImplemented = 120;

std::string describe_address(uint64_t addr) {
    char head[32];
    std::snprintf(head, sizeof head, "0x%016llx", static_cast<unsigned long long>(addr));
    const std::string off = module_offset(addr);
    if (off.rfind("0x", 0) == 0) return std::string(head) + " = not inside any module";
    return std::string(head) + " = " + off;
}

const char* policy_effect(UnimplementedPolicy policy, ReturnClass cls) {
    if (policy == UnimplementedPolicy::Log) return "policy log: returns 0, last error untouched";
    switch (cls) {
    case ReturnClass::HResult: return "policy fail: returns E_NOTIMPL, last error 120 ERROR_CALL_NOT_IMPLEMENTED";
    case ReturnClass::Errno: return "policy fail: returns EINVAL (errno_t), last error 120 ERROR_CALL_NOT_IMPLEMENTED";
    case ReturnClass::Other: break;
    }
    return "policy fail: returns 0 (NULL/FALSE), last error 120 ERROR_CALL_NOT_IMPLEMENTED";
}

const char* class_name(ReturnClass cls) {
    return cls == ReturnClass::HResult ? "hresult" : cls == ReturnClass::Errno ? "errno_t" : "other";
}

uint64_t fail_value(ReturnClass cls) {
    return cls == ReturnClass::HResult ? kENotImpl : cls == ReturnClass::Errno ? kEinval : 0;
}

const char* fail_value_name(ReturnClass cls) {
    return cls == ReturnClass::HResult ? "E_NOTIMPL" : cls == ReturnClass::Errno ? "EINVAL" : "0";
}

void unimplemented_frame(gta6rt_guest_frame* f, void* user) {
    auto* ctx = static_cast<UnimplCtx*>(user);
    const UnimplementedPolicy policy = unimplemented_policy();
    uint64_t ret = 0;
    if (f->rsp) std::memcpy(&ret, reinterpret_cast<const void*>(static_cast<uintptr_t>(f->rsp)), 8);
    ++g_unimpl_calls;
    const uint64_t n = ctx->calls.fetch_add(1) + 1;
    if (n == 1) {
        std::lock_guard lock(g_called_mutex);
        g_called.insert(ctx->what);
        std::lock_guard l2(g_unimpl_list_mutex);
        g_unimpl_called.push_back(ctx);
    }
    if (policy == UnimplementedPolicy::Stop) {
        std::fprintf(stderr,
                     "[rt] STOP: unimplemented import %s called\n"
                     "[rt]   caller   %s\n"
                     "[rt]   rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx\n"
                     "[rt]   return class: %s (LEONIDA_UNIMPLEMENTED=fail returns %s)\n"
                     "[rt]   terminating the process (exit code %d)\n",
                     ctx->what.c_str(), describe_address(ret).c_str(), static_cast<unsigned long long>(f->rcx),
                     static_cast<unsigned long long>(f->rdx), static_cast<unsigned long long>(f->r8),
                     static_cast<unsigned long long>(f->r9), class_name(ctx->cls), fail_value_name(ctx->cls), kStopExitCode);
        std::fflush(stderr);
        _exit(kStopExitCode);
    }
    if ((n & (n - 1)) == 0)
        gta6rt_log(GTA6RT_LOG_WARN, "shims", "unimplemented: %s called from %s (call %llu; %s)", ctx->what.c_str(),
                   module_offset(ret).c_str(), static_cast<unsigned long long>(n), policy_effect(policy, ctx->cls));
    if (policy == UnimplementedPolicy::Fail) {
        f->rax = fail_value(ctx->cls);
        gta6rt_last_error_set(kErrorCallNotImplemented);
    } else {
        f->rax = 0;
    }
    f->xmm0_ret = 0;
    f->ret_is_float = 0;
}

struct BundledLoaderEntry {
    void* ctx;
    const BundledModuleOps* ops;
};
struct XMemHooksEntry {
    const void* owner;
    const XMemAllocationHooks* hooks;
};

std::mutex& bundled_loader_mutex() {
    static auto* m = new std::mutex;
    return *m;
}
std::vector<BundledLoaderEntry>& bundled_loaders() {
    static auto* v = new std::vector<BundledLoaderEntry>;
    return *v;
}
std::mutex& xmem_hooks_mutex() {
    static auto* m = new std::mutex;
    return *m;
}
std::vector<XMemHooksEntry>& xmem_hooks_entries() {
    static auto* v = new std::vector<XMemHooksEntry>;
    return *v;
}
std::vector<BundledLoaderEntry> snapshot_bundled_loaders() {
    std::lock_guard lock(bundled_loader_mutex());
    return bundled_loaders();
}

std::mutex g_thunk_mutex;
std::map<std::string, gta6rt_gva>& thunk_cache() {
    static auto* m = new std::map<std::string, gta6rt_gva>;
    return *m;
}

}  // namespace

void register_entry(const std::string& dll, const std::string& name, const Found& f) {
    auto& r = registry();
    std::unique_lock lock(r.mutex);
    const std::string d = lower_ascii(dll);
    auto [it, inserted] = r.rows.insert_or_assign(d + "!" + name, f);
    (void)it;
    if (inserted) ++r.per_dll[d];
}

void register_entry_if_absent(const std::string& dll, const std::string& name, const Found& f) {
    auto& r = registry();
    std::unique_lock lock(r.mutex);
    const std::string d = lower_ascii(dll);
    if (r.rows.emplace(d + "!" + name, f).second) ++r.per_dll[d];
}

Table::Table(const Entry* entries, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        Found f;
        f.fn = entries[i].fn;
        f.frame = entries[i].frame;
        f.data = entries[i].data;
        register_entry(entries[i].dll, entries[i].name, f);
    }
}

Found find_entry(const std::string& dll_lower, const std::string& name) {
    auto& r = registry();
    std::shared_lock lock(r.mutex);
    auto it = r.rows.find(dll_lower + "!" + name);
    if (it != r.rows.end()) return it->second;
    if (const char* fam = family_of(dll_lower)) {
        it = r.rows.find(std::string(fam) + "!" + name);
        if (it != r.rows.end()) return it->second;
    }
    return {};
}

void* find(const std::string& dll_lower, const std::string& name) {
    const Found f = find_entry(dll_lower, name);
    return f.fn;
}

size_t count_rows(const std::string& dll_lower) {
    auto& r = registry();
    std::shared_lock lock(r.mutex);
    auto it = r.per_dll.find(dll_lower);
    return it == r.per_dll.end() ? 0 : it->second;
}

size_t total_rows() {
    auto& r = registry();
    std::shared_lock lock(r.mutex);
    return r.rows.size();
}

bool owns_dll(const std::string& dll_lower) {
    if (count_rows(dll_lower) > 0) return true;
    if (const char* fam = family_of(dll_lower)) return count_rows(fam) > 0;
    return false;
}

UnimplementedPolicy parse_unimplemented_policy(const char* text, bool* valid) {
    if (valid) *valid = true;
    if (!text || !*text) return UnimplementedPolicy::Fail;
    const std::string v = lower_ascii(text);
    if (v == "log") return UnimplementedPolicy::Log;
    if (v == "fail") return UnimplementedPolicy::Fail;
    if (v == "stop") return UnimplementedPolicy::Stop;
    if (valid) *valid = false;
    return UnimplementedPolicy::Fail;
}

UnimplementedPolicy unimplemented_policy() {
    int p = g_policy.load();
    if (p < 0) {
        const char* env = gta6rt_env_get("LEONIDA_UNIMPLEMENTED");
        bool valid = true;
        const UnimplementedPolicy parsed = parse_unimplemented_policy(env, &valid);
        if (!valid) RTLOG(WARN, "LEONIDA_UNIMPLEMENTED: expected log, fail or stop; using fail");
        int expected = -1;
        g_policy.compare_exchange_strong(expected, static_cast<int>(parsed));
        p = g_policy.load();
    }
    return static_cast<UnimplementedPolicy>(p);
}

void set_unimplemented_policy(UnimplementedPolicy p) { g_policy.store(static_cast<int>(p)); }

ReturnClass classify_import(const std::string& dll, const std::string& name) { return classify(lower_ascii(dll), name); }

void add_module_lookup(void* ctx, ModuleLookup fn) {
    std::lock_guard lock(g_lookup_mutex);
    g_lookups.push_back({ctx, fn});
}

void remove_module_lookup(void* ctx) {
    std::lock_guard lock(g_lookup_mutex);
    for (auto it = g_lookups.begin(); it != g_lookups.end(); ++it)
        if (it->ctx == ctx) {
            g_lookups.erase(it);
            break;
        }
}

bool mapped_image_base(uint64_t address, uint64_t& base) {
    std::string module;
    uint32_t rva = 0;
    {
        std::lock_guard lock(g_lookup_mutex);
        for (auto& l : g_lookups)
            if (l.fn(l.ctx, address, module, rva)) {
                base = address - rva;
                return true;
            }
    }
    if (gta6rt_module* m = gta6rt_module_find_by_address(address)) {
        base = gta6rt_module_info(m)->base;
        return true;
    }
    return false;
}

std::string module_offset(uint64_t address) {
    char b[48];
    std::string module;
    uint32_t rva = 0;
    {
        std::lock_guard lock(g_lookup_mutex);
        for (auto& l : g_lookups)
            if (l.fn(l.ctx, address, module, rva)) {
                std::snprintf(b, sizeof b, "+0x%x", rva);
                return module + b;
            }
    }
    if (gta6rt_module* m = gta6rt_module_find_by_address(address)) {
        const gta6rt_module_desc* d = gta6rt_module_info(m);
        std::snprintf(b, sizeof b, "+0x%llx", static_cast<unsigned long long>(address - d->base));
        return std::string(d->name ? d->name : "?") + b;
    }
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(address));
    return b;
}

void add_bundled_loader(void* ctx, const BundledModuleOps* ops) {
    if (!ctx || !ops) return;
    std::lock_guard lock(bundled_loader_mutex());
    bundled_loaders().push_back({ctx, ops});
}

void remove_bundled_loader(void* ctx) {
    std::lock_guard lock(bundled_loader_mutex());
    auto& list = bundled_loaders();
    for (auto it = list.begin(); it != list.end(); ++it)
        if (it->ctx == ctx) {
            list.erase(it);
            break;
        }
}

void* find_loaded_bundled_module(const std::string& dll_lower) {
    for (const auto& e : snapshot_bundled_loaders())
        if (e.ops->find_loaded)
            if (void* h = e.ops->find_loaded(e.ctx, dll_lower)) return h;
    return nullptr;
}

void* load_bundled_module(const std::string& dll_lower, uint32_t flags, bool& handled) {
    handled = false;
    for (const auto& e : snapshot_bundled_loaders()) {
        if (!e.ops->load_bundled) continue;
        bool h = false;
        void* m = e.ops->load_bundled(e.ctx, dll_lower, flags, h);
        if (h) {
            handled = true;
            return m;
        }
    }
    return nullptr;
}

void* find_loaded_bundled_module_w(const std::u16string& basename) {
    for (const auto& e : snapshot_bundled_loaders())
        if (e.ops->find_loaded_w)
            if (void* h = e.ops->find_loaded_w(e.ctx, basename)) return h;
    return nullptr;
}

void* load_bundled_module_w(const std::u16string& basename, uint32_t flags, bool& handled) {
    handled = false;
    for (const auto& e : snapshot_bundled_loaders()) {
        if (!e.ops->load_bundled_w) continue;
        bool h = false;
        void* m = e.ops->load_bundled_w(e.ctx, basename, flags, h);
        if (h) {
            handled = true;
            return m;
        }
    }
    return nullptr;
}

void* get_bundled_proc_address(void* module, const std::string& sym, bool& owns_handle) {
    owns_handle = false;
    for (const auto& e : snapshot_bundled_loaders()) {
        bool owns = false;
        void* fn = e.ops->get_proc(e.ctx, module, sym, owns);
        if (owns) {
            owns_handle = true;
            return fn;
        }
    }
    return nullptr;
}

bool owns_bundled_module_handle(void* module) {
    for (const auto& e : snapshot_bundled_loaders())
        if (e.ops->owns_handle && e.ops->owns_handle(e.ctx, module)) return true;
    return false;
}

void add_xmem_allocation_hooks(const void* owner, const XMemAllocationHooks* hooks) {
    if (!owner || !hooks) return;
    std::lock_guard lock(xmem_hooks_mutex());
    xmem_hooks_entries().push_back({owner, hooks});
}

void remove_xmem_allocation_hooks(const void* owner) {
    std::lock_guard lock(xmem_hooks_mutex());
    auto& list = xmem_hooks_entries();
    for (auto it = list.begin(); it != list.end();) it = it->owner == owner ? list.erase(it) : it + 1;
}

const XMemAllocationHooks* xmem_allocation_hooks() {
    std::lock_guard lock(xmem_hooks_mutex());
    const auto& list = xmem_hooks_entries();
    return list.empty() ? nullptr : list.back().hooks;
}

gta6rt_status make_unimplemented_thunk(const std::string& dll, const std::string& name, gta6rt_gva* out) {
    const std::string key = lower_ascii(dll) + "!" + name;
    std::lock_guard lock(g_thunk_mutex);
    auto& cache = thunk_cache();
    if (auto it = cache.find(key); it != cache.end()) {
        *out = it->second;
        return GTA6RT_OK;
    }
    auto* ctx = new UnimplCtx{key, classify(lower_ascii(dll), name)};
    ctx->what = key;
    gta6rt_thunk_desc desc{};
    desc.kind = GTA6RT_THUNK_TRAP;
    desc.fn = reinterpret_cast<const void*>(&unimplemented_frame);
    desc.user = ctx;
    desc.debug_name = ctx->what.c_str();
    gta6rt_gva addr = 0;
    const gta6rt_status st = gta6rt_isa_make_thunk(&desc, &addr);
    if (st != GTA6RT_OK) {
        delete ctx;
        return st;
    }
    cache.emplace(key, addr);
    *out = addr;
    return GTA6RT_OK;
}

gta6rt_status make_guest_thunk(void* native, gta6rt_frame_handler frame, const char* debug_name, gta6rt_gva* out) {
    gta6rt_thunk_desc desc{};
    desc.debug_name = strdup(debug_name ? debug_name : "host");
#if defined(__x86_64__)
    if (native) {
        desc.kind = GTA6RT_THUNK_NATIVE;
        desc.fn = native;
    } else {
        desc.kind = GTA6RT_THUNK_FRAME;
        desc.fn = reinterpret_cast<const void*>(frame);
    }
#else
    (void)native;
    desc.kind = GTA6RT_THUNK_FRAME;
    desc.fn = reinterpret_cast<const void*>(frame);
#endif
    if (!desc.fn) return GTA6RT_E_UNSUPPORTED;
    return gta6rt_isa_make_thunk(&desc, out);
}

void report_called_unimplemented() {
    std::lock_guard lock(g_unimpl_list_mutex);
    const UnimplementedPolicy policy = unimplemented_policy();
    for (const UnimplCtx* c : g_unimpl_called)
        std::fprintf(stderr, "  called: %s  %llu call(s), %s\n", c->what.c_str(),
                     static_cast<unsigned long long>(c->calls.load()), policy_effect(policy, c->cls));
}

size_t unimplemented_call_count() { return g_unimpl_calls.load(); }

gta6rt_status guest_address(const std::string& dll_lower, const std::string& name, gta6rt_gva* out, bool serve_stub,
                            bool* was_stub) {
    if (was_stub) *was_stub = false;
    const Found f = find_entry(dll_lower, name);
    const std::string key = dll_lower + "!" + name;
    if (f) {
        if (f.data) {
            *out = reinterpret_cast<uintptr_t>(f.fn);
            return GTA6RT_OK;
        }
        std::lock_guard lock(g_thunk_mutex);
        auto& cache = thunk_cache();
        if (auto it = cache.find(key); it != cache.end()) {
            *out = it->second;
            return GTA6RT_OK;
        }
        gta6rt_thunk_desc desc{};
        desc.user = nullptr;
        desc.debug_name = nullptr;
        char* dbg = strdup(key.c_str());
        desc.debug_name = dbg;
#if defined(__x86_64__)
        if (f.fn) {
            desc.kind = GTA6RT_THUNK_NATIVE;
            desc.fn = f.fn;
        } else {
            desc.kind = GTA6RT_THUNK_FRAME;
            desc.fn = reinterpret_cast<const void*>(f.frame);
        }
#else
        if (f.frame) {
            desc.kind = GTA6RT_THUNK_FRAME;
            desc.fn = reinterpret_cast<const void*>(f.frame);
        } else {
            free(dbg);
            goto stub;
        }
#endif
        {
            gta6rt_gva addr = 0;
            const gta6rt_status st = gta6rt_isa_make_thunk(&desc, &addr);
            if (st != GTA6RT_OK) {
                free(dbg);
                return st;
            }
            cache.emplace(key, addr);
            *out = addr;
            return GTA6RT_OK;
        }
    }
#if !defined(__x86_64__)
stub:
#endif
    if (!serve_stub) return GTA6RT_E_NOT_FOUND;
    if (was_stub) *was_stub = true;
    return make_unimplemented_thunk(dll_lower, name, out);
}

}  // namespace rt::shims

namespace {
std::atomic<uint32_t> g_stat_served{0}, g_stat_stub{0};
}

extern "C" {

gta6rt_status gta6rt_shims_register_dll(const char* dll_name, const gta6rt_shim_export* exports) {
    if (!dll_name || !exports) return GTA6RT_E_INVALID_ARG;
    for (const gta6rt_shim_export* e = exports; e->name; ++e) {
        rt::shims::Found f;
        switch (e->kind) {
        case GTA6RT_SHIM_CODE_NATIVE: f.fn = const_cast<void*>(e->target); break;
        case GTA6RT_SHIM_CODE_FRAME: f.frame = reinterpret_cast<gta6rt_frame_handler>(const_cast<void*>(e->target)); break;
        case GTA6RT_SHIM_DATA: f.fn = const_cast<void*>(e->target); f.data = true; break;
        default: return GTA6RT_E_INVALID_ARG;
        }
        rt::shims::register_entry(dll_name, e->name, f);
        if (e->ordinal) rt::shims::register_entry(dll_name, "#" + std::to_string(e->ordinal), f);
    }
    return GTA6RT_OK;
}

gta6rt_status gta6rt_shims_init(void) {
    static std::once_flag once;
    std::call_once(once, [] {
        RTLOG(INFO, "shim registry: %zu rows", rt::shims::total_rows());
        rt::shims::apply_console_profile();
    });
    return GTA6RT_OK;
}

gta6rt_status gta6rt_shims_import_resolver(void* user, const gta6rt_import_ref* ref, gta6rt_import_binding* out) {
    (void)user;
    if (!ref || !ref->dll || !out) return GTA6RT_E_INVALID_ARG;
    const std::string dll = rt::lower_ascii(ref->dll);
    const std::string name = ref->name ? std::string(ref->name) : "#" + std::to_string(ref->ordinal);
    bool stub = false;
    const bool data_guess = ref->kind_guess == GTA6RT_IMPORT_DATA;
    gta6rt_gva addr = 0;
    const gta6rt_status st = rt::shims::guest_address(dll, name, &addr, !data_guess, &stub);
    if (st != GTA6RT_OK) return st;
    out->address = addr;
    if (stub) ++g_stat_stub;
    else ++g_stat_served;
    return GTA6RT_OK;
}

}  // extern "C"
