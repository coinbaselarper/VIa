// library_loader: LoadLibrary / GetModuleHandle / GetProcAddress for the modules the shims serve and the package DLLs
// the host maps.
//
// GDK code does not always import the console runtime. The package's libHttpClient.GDK.dll and XCurl.dll have no import
// entry for xgameruntime.dll at all: their statically linked xgameruntime.lib thunks hold the wide string
// "xgameruntime.dll" and look names up at run time. There is no host Windows to fall back to on macOS, so a name is
// resolved in this order:
//   1. a module the shim registry serves (xgameruntime.dll, d3d12_xs.dll, kernel32.dll, ucrtbase.dll, ...): a
//      pseudo-handle, and GetProcAddress on it resolves names (and "#ordinal") to guest thunks;
//   2. a package DLL the host's loader maps (the BundledModuleOps the host registers);
//   3. a module in the os module table (the title exe and any image registered there);
//   otherwise ERROR_MOD_NOT_FOUND. A name the registry does not serve is NULL with ERROR_PROC_NOT_FOUND, never a stub.
// GetModuleFileName answers G:\... for package files, so the title's own path logic lands in the console's drive.
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <gta6rt/os.h>

#include "file_system.h"
#include "guest_address.h"
#include "shims.h"
#include "util.h"

namespace rt::shims::library_loader {

struct Pseudo {
    char tag[8] = {'L', 'E', 'O', 'S', 'H', 'I', 'M', 0};
    std::string dll;  // lower case, with .dll
};

namespace {

constexpr uint32_t kErrorModNotFound = 126, kErrorProcNotFound = 127, kErrorInvalidHandle = 6, kErrorInvalidParameter = 87,
                   kErrorInsufficientBuffer = 122;
constexpr uint32_t kDataFileFlags = 0x2 | 0x40 | 0x20;

std::mutex& loader_mutex() {
    static auto* m = new std::mutex;
    return *m;
}

std::deque<Pseudo>& pseudos() {
    static auto* d = new std::deque<Pseudo>;
    return *d;
}

bool is_ascii(const char16_t* w) {
    for (; w && *w; ++w)
        if (*w >= 0x80) return false;
    return true;
}

std::string module_name(std::string path) {
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) path.erase(0, slash + 1);
    path = lower_ascii(path);
    if (!path.empty() && path.back() == '.') path.pop_back();
    else if (path.find('.') == std::string::npos) path += ".dll";
    return path;
}

std::u16string module_name_w(std::u16string path) {
    const size_t slash = path.find_last_of(u"\\/");
    if (slash != std::u16string::npos) path.erase(0, slash + 1);
    if (!path.empty() && path.back() == u'.') path.pop_back();
    else if (path.find(u'.') == std::u16string::npos) path += u".dll";
    return path;
}

const Pseudo* pseudo(uint64_t h) {
    std::lock_guard lock(loader_mutex());
    for (auto& p : pseudos())
        if (reinterpret_cast<uintptr_t>(&p) == h) return &p;
    return nullptr;
}

uint64_t owned_handle(const std::string& path) {
    const std::string dll = module_name(path);
    if (dll.empty() || !owns_dll(dll)) return 0;
    std::lock_guard lock(loader_mutex());
    for (auto& p : pseudos())
        if (p.dll == dll) return reinterpret_cast<uintptr_t>(&p);
    pseudos().push_back(Pseudo{});
    pseudos().back().dll = dll;
    RTLOG(INFO, "loader: %s is served by the shims (run-time load)", dll.c_str());
    return reinterpret_cast<uintptr_t>(&pseudos().back());
}

uint64_t module_table_handle(const std::string& dll) {
    if (gta6rt_module* m = gta6rt_module_find_by_name(dll.c_str())) return gta6rt_module_info(m)->base;
    return 0;
}

uint64_t main_module_base() {
    for (size_t i = 0; gta6rt_module* m = gta6rt_module_at(i); ++i)
        if (gta6rt_module_info(m)->kind == GTA6RT_MODULE_GUEST_IMAGE) return gta6rt_module_info(m)->base;
    return 0;
}

uint64_t load_by_utf8(const std::string& path, uint32_t flags) {
    if (uint64_t h = owned_handle(path)) return h;
    const std::string dll = module_name(path);
    if (!dll.empty()) {
        bool handled = false;
        void* h = load_bundled_module(dll, flags, handled);
        if (handled) return reinterpret_cast<uintptr_t>(h);
        if (uint64_t b = module_table_handle(dll)) return b;
    }
    RTLOG(WARN, "LoadLibrary(%s): module not found", path.c_str());
    gta6rt_last_error_set(kErrorModNotFound);
    return 0;
}

uint64_t load_by_wide(const char16_t* name, uint32_t flags) {
    if (is_ascii(name) || !name) return load_by_utf8(narrow(name), flags);
    const std::u16string base = module_name_w(name);
    bool handled = false;
    void* h = load_bundled_module_w(base, flags, handled);
    if (handled) return reinterpret_cast<uintptr_t>(h);
    RTLOG(WARN, "LoadLibrary(%s): module not found", narrow(name).c_str());
    gta6rt_last_error_set(kErrorModNotFound);
    return 0;
}

uint64_t WINAPI load_library_a(const char* name) {
    if (!name) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    return load_by_utf8(name, 0);
}
uint64_t WINAPI load_library_w(const char16_t* name) {
    if (!name) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    return load_by_wide(name, 0);
}
uint64_t WINAPI load_library_ex_a(const char* name, void* file, uint32_t flags) {
    (void)file;
    if (!name) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    if (flags & kDataFileFlags) {
        gta6rt_last_error_set(kErrorModNotFound);
        return 0;
    }
    return load_by_utf8(name, flags);
}
uint64_t WINAPI load_library_ex_w(const char16_t* name, void* file, uint32_t flags) {
    (void)file;
    if (!name) {
        gta6rt_last_error_set(kErrorInvalidParameter);
        return 0;
    }
    if (flags & kDataFileFlags) {
        gta6rt_last_error_set(kErrorModNotFound);
        return 0;
    }
    return load_by_wide(name, flags);
}

uint64_t get_handle_utf8(const std::string& path) {
    if (uint64_t h = owned_handle(path)) return h;
    const std::string dll = module_name(path);
    if (dll.empty()) return 0;
    if (void* h = find_loaded_bundled_module(dll)) return reinterpret_cast<uintptr_t>(h);
    return module_table_handle(dll);
}

uint64_t WINAPI get_module_handle_a(const char* name) {
    if (!name) return main_module_base();
    const uint64_t h = get_handle_utf8(name);
    if (!h) gta6rt_last_error_set(kErrorModNotFound);
    return h;
}

uint64_t WINAPI get_module_handle_w(const char16_t* name) {
    if (!name) return main_module_base();
    uint64_t h;
    if (is_ascii(name)) {
        h = get_handle_utf8(narrow(name));
    } else {
        void* b = find_loaded_bundled_module_w(module_name_w(name));
        h = reinterpret_cast<uintptr_t>(b);
    }
    if (!h) gta6rt_last_error_set(kErrorModNotFound);
    return h;
}

uint64_t WINAPI get_proc_address(uint64_t module, const char* name) {
    if (!name) {
        gta6rt_last_error_set(kErrorProcNotFound);
        return 0;
    }
    const std::string what = reinterpret_cast<uintptr_t>(name) < 0x10000 ? "#" + std::to_string(reinterpret_cast<uintptr_t>(name)) : name;
    if (const Pseudo* p = pseudo(module)) {
        gta6rt_gva addr = 0;
        if (guest_address(p->dll, what, &addr, false) == GTA6RT_OK) return addr;
        static std::mutex m;
        static std::set<std::string> seen;
        std::lock_guard lock(m);
        if (seen.insert(p->dll + "!" + what).second) RTLOG(WARN, "GetProcAddress(%s, %s): not served", p->dll.c_str(), what.c_str());
        gta6rt_last_error_set(kErrorProcNotFound);
        return 0;
    }
    bool owns = false;
    void* fn = get_bundled_proc_address(reinterpret_cast<void*>(static_cast<uintptr_t>(module)), what, owns);
    if (owns) {
        if (fn) return reinterpret_cast<uintptr_t>(fn);
        gta6rt_last_error_set(kErrorProcNotFound);
        return 0;
    }
    if (gta6rt_module* m = gta6rt_module_find_by_base(module)) {
        const bool ordinal = what[0] == '#';
        const gta6rt_gva a = gta6rt_module_get_export(m, ordinal ? nullptr : what.c_str(), ordinal ? static_cast<uint32_t>(std::stoul(what.substr(1))) : 0);
        if (a) return a;
        gta6rt_last_error_set(kErrorProcNotFound);
        return 0;
    }
    gta6rt_last_error_set(kErrorInvalidHandle);
    return 0;
}

int WINAPI free_library(uint64_t module) {
    if (pseudo(module) || owns_bundled_module_handle(reinterpret_cast<void*>(static_cast<uintptr_t>(module))) ||
        gta6rt_module_find_by_base(module))
        return 1;
    gta6rt_last_error_set(kErrorInvalidHandle);
    return 0;
}

int WINAPI disable_thread_calls(uint64_t module) { return free_library(module); }

std::string console_view_of(const std::string& host_path) {
    const std::string game = fs::game_dir();
    if (!game.empty() && host_path.size() > game.size() && host_path.compare(0, game.size(), game) == 0 && host_path[game.size()] == '/') {
        std::string rel = host_path.substr(game.size() + 1);
        for (auto& c : rel)
            if (c == '/') c = '\\';
        return "G:\\" + rel;
    }
    return host_path;
}

uint32_t module_file_name(uint64_t module, std::string& out) {
    if (const Pseudo* p = pseudo(module)) {
        out = "C:\\Windows\\System32\\" + p->dll;
        return 0;
    }
    gta6rt_module* m = module ? gta6rt_module_find_by_base(module) : gta6rt_module_find_by_base(main_module_base());
    if (!m) return kErrorInvalidHandle;
    const gta6rt_module_desc* d = gta6rt_module_info(m);
    out = d->path_utf8 && *d->path_utf8 ? console_view_of(d->path_utf8) : std::string(d->name ? d->name : "");
    return 0;
}

uint32_t WINAPI get_module_file_name_w(uint64_t module, char16_t* buf, uint32_t size) {
    std::string path;
    if (const uint32_t e = module_file_name(module, path)) {
        gta6rt_last_error_set(e);
        return 0;
    }
    const std::u16string wide = widen(path.c_str());
    if (!buf || size == 0) {
        gta6rt_last_error_set(kErrorInsufficientBuffer);
        return 0;
    }
    const size_t n = wide.size() < size ? wide.size() : size - 1;
    std::memcpy(buf, wide.data(), n * sizeof(char16_t));
    buf[n] = 0;
    if (n < wide.size()) {
        gta6rt_last_error_set(kErrorInsufficientBuffer);
        return size;
    }
    gta6rt_last_error_set(0);
    return static_cast<uint32_t>(n);
}

uint32_t WINAPI get_module_file_name_a(uint64_t module, char* buf, uint32_t size) {
    std::string path;
    if (const uint32_t e = module_file_name(module, path)) {
        gta6rt_last_error_set(e);
        return 0;
    }
    if (!buf || size == 0) {
        gta6rt_last_error_set(kErrorInsufficientBuffer);
        return 0;
    }
    const size_t n = path.size() < size ? path.size() : size - 1;
    std::memcpy(buf, path.data(), n);
    buf[n] = 0;
    if (n < path.size()) {
        gta6rt_last_error_set(kErrorInsufficientBuffer);
        return size;
    }
    gta6rt_last_error_set(0);
    return static_cast<uint32_t>(n);
}

}  // namespace
}  // namespace rt::shims::library_loader

namespace {
using namespace rt::shims::library_loader;
}

SHIM_TABLE(library_loader,
    SHIM("kernel32.dll", "LoadLibraryA", load_library_a),
    SHIM("kernel32.dll", "LoadLibraryW", load_library_w),
    SHIM("kernel32.dll", "LoadLibraryExA", load_library_ex_a),
    SHIM("kernel32.dll", "LoadLibraryExW", load_library_ex_w),
    SHIM("kernel32.dll", "GetModuleHandleA", get_module_handle_a),
    SHIM("kernel32.dll", "GetModuleHandleW", get_module_handle_w),
    SHIM("kernel32.dll", "GetProcAddress", get_proc_address),
    SHIM("kernel32.dll", "FreeLibrary", free_library),
    SHIM("kernel32.dll", "DisableThreadLibraryCalls", disable_thread_calls),
    SHIM("kernel32.dll", "GetModuleFileNameA", get_module_file_name_a),
    SHIM("kernel32.dll", "GetModuleFileNameW", get_module_file_name_w),
);
