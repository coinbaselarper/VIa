// Test-only stand-in for shims_core's registry: the export table behind gta6rt_shims_register_dll and the
// rt::shims lookups the GDK shims call. Names are matched as leonida's registry did (lower-case DLL, exact name).
#include <windows.h>

#include <gta6rt/shims.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <string>

#include "shims.h"

namespace {
std::mutex g_mu;
std::map<std::string, void*>& table() {
    static auto* t = new std::map<std::string, void*>;
    return *t;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

extern "C" gta6rt_status gta6rt_shims_register_dll(const char* dll_name, const gta6rt_shim_export* exports) {
    std::lock_guard lock(g_mu);
    for (; exports->name; ++exports) {
        if (exports->kind != GTA6RT_SHIM_CODE_NATIVE) continue;
        table()[lower(dll_name) + "!" + exports->name] = const_cast<void*>(exports->target);
    }
    return GTA6RT_OK;
}

extern "C" gta6rt_status gta6rt_shims_init(void) { return GTA6RT_OK; }

namespace rt::shims {

Table::Table(const Entry*, size_t) {}

void* find(const std::string& dll_lower, const std::string& name) {
    std::lock_guard lock(g_mu);
    const auto it = table().find(dll_lower + "!" + name);
    return it == table().end() ? nullptr : it->second;
}

bool owns_dll(const std::string& dll_lower) {
    std::lock_guard lock(g_mu);
    const std::string prefix = dll_lower + "!";
    const auto it = table().lower_bound(prefix);
    return it != table().end() && it->first.compare(0, prefix.size(), prefix) == 0;
}

std::string module_offset(uint64_t address) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(address));
    return b;
}

}  // namespace rt::shims
