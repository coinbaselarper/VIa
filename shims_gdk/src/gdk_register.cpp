#include <windows.h>

#include <gta6rt/abi.h>
#include <gta6rt/shims.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "com_shim.h"
#include "shims.h"
#include "silent_audio.h"

namespace rt::gdk {

namespace {
struct Registered {
    const ::rt::shims::Entry* entries;
    size_t count;
};

std::vector<Registered>& tables() {
    static auto* t = new std::vector<Registered>;
    return *t;
}
}  // namespace

TableRegistration::TableRegistration(const ::rt::shims::Entry* entries, size_t count) {
    tables().push_back({entries, count});
}

void for_each_entry(void (*fn)(void* user, const ::rt::shims::Entry& entry), void* user) {
    for (const Registered& r : tables())
        for (size_t i = 0; i < r.count; ++i) fn(user, r.entries[i]);
}

}  // namespace rt::gdk

namespace rt::gdk {

namespace {
int32_t create_audio_enumerator(uint32_t cls_context, const ::rt::com_shim::Guid* riid, void** ppv) {
    if (!riid || !ppv) return static_cast<int32_t>(E_POINTER);
    return silent_audio::create_enumerator(cls_context, *reinterpret_cast<const GUID*>(riid), ppv);
}
}  // namespace

void register_com_classes() {
    static_assert(sizeof(GUID) == sizeof(::rt::com_shim::Guid));
    ::rt::com_shim::register_class(*reinterpret_cast<const ::rt::com_shim::Guid*>(&silent_audio::kClsidMMDeviceEnumerator),
                                   &create_audio_enumerator);
}

gta6rt_status register_all() {
    static std::once_flag once;
    static gta6rt_status result = GTA6RT_OK;
    std::call_once(once, [] {
        register_com_classes();
        struct Dll {
            std::vector<std::string> names;
            std::vector<gta6rt_shim_export> exports;
        };
        static auto* dlls = new std::map<std::string, Dll>;
        for (const auto& t : tables())
            for (size_t i = 0; i < t.count; ++i) {
                const auto& e = t.entries[i];
                Dll& d = (*dlls)[e.dll];
                d.names.emplace_back(e.name);
                gta6rt_shim_export x{};
                x.ordinal = 0;
                if (e.name[0] == '#') x.ordinal = static_cast<uint16_t>(std::strtoul(e.name + 1, nullptr, 10));
                x.kind = GTA6RT_SHIM_CODE_NATIVE;
                x.target = e.fn;
                d.exports.push_back(x);
            }
        for (auto& [name, d] : *dlls) {
            for (size_t i = 0; i < d.exports.size(); ++i) d.exports[i].name = d.names[i].c_str();
            d.exports.push_back({});
        }
        for (auto& [name, d] : *dlls) {
            const gta6rt_status s = gta6rt_shims_register_dll(name.c_str(), d.exports.data());
            if (s != GTA6RT_OK && result == GTA6RT_OK) result = s;
        }
    });
    return result;
}

}  // namespace rt::gdk

// The tables hold ms_abi function pointers, which only the x86_64 build can hand out as CODE_NATIVE exports. arm64
// needs a frame adapter per entry (see STATUS.md) and reports GTA6RT_E_UNSUPPORTED.
extern "C" gta6rt_status gta6rt_shims_gdk_register(void) {
#if defined(__x86_64__)
    return rt::gdk::register_all();
#else
    rt::gdk::register_com_classes();
    return GTA6RT_E_UNSUPPORTED;
#endif
}
