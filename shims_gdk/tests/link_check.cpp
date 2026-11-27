// Links gta6rt_shims_gdk against the real shims_core, isa and os, registers the GDK tables with the real registry and
// resolves imports through gta6rt_shims_import_resolver. Resolution only mints thunks; nothing here runs guest code.
#include <gta6rt/isa.h>
#include <gta6rt/shims.h>

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <string>

#include "gdk_register.h"
#include "silent_audio.h"

namespace {
struct Probe {
    const char* dll;
    const char* name;
};
const Probe kProbes[] = {
    {"xgameruntime.dll", "QueryApiImpl"},
    {"xgameruntime.dll", "XAsyncBegin"},
    {"xgameruntime.dll", "XTaskQueueCreate"},
    {"dstorage.dll", "DStorageGetFactory"},
    {"acphal.dll", "AcpHalCreate"},
    {"xapu.dll", "XApuConnect"},
    {"chad.dll", "XDspActivate"},
    {"gameinput.dll", "GameInputCreate"},
    {"gameinput.dll", "#1"},
    {"gameplatformservices.dll", "QueryApiImpl"},
};
}  // namespace

int main() {
    int failures = 0;
    gta6rt_isa_config cfg{};
    gta6rt_status s = gta6rt_isa_init(&cfg);
    std::printf("isa_init: %d (%s)\n", s, gta6rt_isa_backend_name());
    s = gta6rt_shims_init();
    std::printf("shims_init: %d\n", s);
    std::map<std::string, int> per_dll;
    rt::gdk::for_each_entry([](void* u, const rt::shims::Entry& e) { ++(*static_cast<std::map<std::string, int>*>(u))[e.dll]; },
                            &per_dll);
    for (const auto& [dll, n] : per_dll) std::printf("table %s: %d exports\n", dll.c_str(), n);
    s = gta6rt_shims_gdk_register();
    std::printf("gdk_register: %d\n", s);
    using CoCreate = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    auto co_create = reinterpret_cast<CoCreate>(rt::shims::find("ole32.dll", "CoCreateInstance"));
    void* enumerator = nullptr;
    const HRESULT co_hr = co_create ? co_create(rt::silent_audio::kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                                __uuidof(IMMDeviceEnumerator), &enumerator)
                                    : E_FAIL;
    std::printf("CoCreateInstance(MMDeviceEnumerator) -> %#x\n", static_cast<unsigned>(co_hr));
    if (co_hr != S_OK || !enumerator) ++failures;
    if (enumerator) static_cast<IUnknown*>(enumerator)->Release();
    if (s == GTA6RT_E_UNSUPPORTED) {
        std::printf("gdk_link_check: %s (arm64: the GDK tables hold ms_abi entries, no frame adapters yet)\n", failures ? "FAIL" : "OK");
        return failures ? 1 : 0;
    }
    for (const Probe& p : kProbes) {
        gta6rt_import_ref ref{};
        ref.dll = p.dll;
        ref.name = p.name;
        ref.kind_guess = GTA6RT_IMPORT_CODE;
        gta6rt_import_binding b{};
        const gta6rt_status r = gta6rt_shims_import_resolver(nullptr, &ref, &b);
        std::printf("%s!%s -> status %d address %#llx\n", p.dll, p.name, r, static_cast<unsigned long long>(b.address));
        if (r != GTA6RT_OK || b.address == 0) ++failures;
    }
    std::printf("gdk_link_check: %s (%zu imports resolved)\n", failures ? "FAIL" : "OK", std::size(kProbes));
    return failures ? 1 : 0;
}
