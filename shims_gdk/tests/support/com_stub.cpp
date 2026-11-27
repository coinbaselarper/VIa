// Test-only stand-in for shims_core's COM allocator exports, which the silent endpoint calls.
#include <windows.h>
#include <objbase.h>

#include <cstdlib>

extern "C" {
void* WINAPI CoTaskMemAlloc(SIZE_T n) { return std::malloc(n ? n : 1); }
void WINAPI CoTaskMemFree(void* p) { std::free(p); }
}

#include <map>
#include <mutex>

#include "com_shim.h"

namespace rt::com_shim {
namespace {
std::mutex g_mu;
std::map<std::string, ClassFactoryFn>& classes() {
    static auto* m = new std::map<std::string, ClassFactoryFn>;
    return *m;
}
std::string key(const Guid& g) { return std::string(reinterpret_cast<const char*>(&g), sizeof g); }
}  // namespace

void register_class(const Guid& clsid, ClassFactoryFn factory) {
    std::lock_guard lock(g_mu);
    classes()[key(clsid)] = factory;
}

ClassFactoryFn find_class(const Guid& clsid) {
    std::lock_guard lock(g_mu);
    const auto it = classes().find(key(clsid));
    return it == classes().end() ? nullptr : it->second;
}
}  // namespace rt::com_shim
