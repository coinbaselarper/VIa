// COM lifecycle and helper shims for api-ms-win-core-com-l1-1-0..3.dll.
//
// game_scarlett_master.exe imports CoInitializeEx, CoUninitialize, CoCreateInstance, CoCreateGuid, CoTaskMemFree,
// PropVariantClear, and StringFromGUID2 through api-ms-win-core-com-l1-1-0.dll. There is no host COM on macOS, so the
// guest's COM is bookkeeping only: each thread tracks its nesting depth and apartment type. CoInitializeEx returns
// S_OK the first time, S_FALSE for a matching later call and RPC_E_CHANGED_MODE for the other apartment type;
// CoUninitialize unwinds. CoCreateInstance serves the classes registered with register_class (the audio device
// enumerator, by shims_gdk) and answers REGDB_E_CLASSNOTREG for every other class, logged once.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <gta6rt/os.h>

#include <objbase.h>

#include "com_shim.h"
#include "shims.h"
#include "util.h"

namespace rt::com_shim {
namespace {

constexpr int32_t kSOk = 0, kSFalse = 1;
constexpr int32_t kEPointer = static_cast<int32_t>(0x80004003u), kEInvalidArg = static_cast<int32_t>(0x80070057u),
                  kChangedMode = static_cast<int32_t>(0x80010106u),
                  kClassNotReg = static_cast<int32_t>(0x80040154u), kNoAggregation = static_cast<int32_t>(0x80040110u);
constexpr uint32_t kApartmentThreaded = 0x2;

struct ThreadState {
    uint32_t depth = 0;
    uint32_t coinit = 0;
};
thread_local ThreadState t_state;

std::mutex g_class_mutex;
std::map<std::string, ClassFactoryFn>& classes() {
    static auto* m = new std::map<std::string, ClassFactoryFn>;
    return *m;
}

std::string guid_key(const Guid& g) { return std::string(reinterpret_cast<const char*>(&g), sizeof g); }

int32_t WINAPI co_initialize_ex(void* reserved, uint32_t coinit) {
    if (reserved) return kEInvalidArg;
    if (t_state.depth == 0) {
        t_state.depth = 1;
        t_state.coinit = coinit;
        return kSOk;
    }
    if ((t_state.coinit & kApartmentThreaded) != (coinit & kApartmentThreaded)) return kChangedMode;
    ++t_state.depth;
    return kSFalse;
}

void WINAPI co_uninitialize() {
    if (t_state.depth == 0) return;
    if (--t_state.depth == 0) t_state.coinit = 0;
}

int32_t WINAPI co_create_instance(const Guid* clsid, void* outer, uint32_t cls_context, const Guid* riid, void** ppv) {
    if (!ppv) return kEPointer;
    *ppv = nullptr;
    if (!clsid || !riid) return kEInvalidArg;
    ClassFactoryFn factory = nullptr;
    {
        std::lock_guard lock(g_class_mutex);
        auto it = classes().find(guid_key(*clsid));
        if (it != classes().end()) factory = it->second;
    }
    if (factory) return outer ? kNoAggregation : factory(cls_context, riid, ppv);
    static std::mutex m;
    static std::set<std::string> seen;
    {
        std::lock_guard lock(m);
        if (seen.insert(guid_key(*clsid)).second)
            RTLOG(WARN, "CoCreateInstance: class {%08X-%04X-%04X-...} is not served (REGDB_E_CLASSNOTREG)", clsid->data1,
                  clsid->data2, clsid->data3);
    }
    return kClassNotReg;
}

int32_t WINAPI co_create_guid(Guid* g) {
    if (!g) return kEInvalidArg;
    gta6rt_random_bytes(g, sizeof *g);
    g->data3 = static_cast<uint16_t>((g->data3 & 0x0FFF) | 0x4000);
    g->data4[0] = static_cast<uint8_t>((g->data4[0] & 0x3F) | 0x80);
    return kSOk;
}

void* WINAPI co_task_mem_alloc(size_t size) { return std::malloc(size ? size : 1); }
void WINAPI co_task_mem_free(void* p) { std::free(p); }

// PROPVARIANT: vt at +0, the value at +8. Only the types that own memory matter.
int32_t WINAPI prop_variant_clear(uint8_t* pv) {
    if (!pv) return kEInvalidArg;
    uint16_t vt;
    std::memcpy(&vt, pv, 2);
    constexpr uint16_t kVtVector = 0x1000;
    void* owned = nullptr;
    switch (vt & ~kVtVector) {
    case 30: case 31: case 72:
        if (!(vt & kVtVector)) std::memcpy(&owned, pv + 8, 8);
        break;
    case 65:
        std::memcpy(&owned, pv + 16, 8);
        break;
    default:
        break;
    }
    if (owned) std::free(owned);
    std::memset(pv, 0, 24);
    return kSOk;
}

int32_t WINAPI co_get_apartment_type(int32_t* type, int32_t* qualifier) {
    if (!type || !qualifier) return kEInvalidArg;
    if (t_state.depth == 0) return static_cast<int32_t>(0x800401F0u);
    *type = (t_state.coinit & kApartmentThreaded) ? 0 : 1;
    *qualifier = 0;
    return kSOk;
}

int WINAPI string_from_guid2(const Guid* g, char16_t* out, int cch_max) {
    if (!g || !out || cch_max < 39) return 0;
    char buf[40];
    std::snprintf(buf, sizeof buf, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->data1, g->data2, g->data3,
                  g->data4[0], g->data4[1], g->data4[2], g->data4[3], g->data4[4], g->data4[5], g->data4[6], g->data4[7]);
    for (int i = 0; i < 39; ++i) out[i] = static_cast<char16_t>(static_cast<unsigned char>(buf[i]));
    return 39;
}

}  // namespace

void register_class(const Guid& clsid, ClassFactoryFn factory) {
    std::lock_guard lock(g_class_mutex);
    classes()[guid_key(clsid)] = factory;
}

uint32_t thread_depth() { return t_state.depth; }

}  // namespace rt::com_shim

namespace {
using namespace rt::com_shim;
#define COM_L1_API(dll)                                                                                         \
    SHIM(dll, "CoInitializeEx", co_initialize_ex), SHIM(dll, "CoUninitialize", co_uninitialize),                \
    SHIM(dll, "CoCreateInstance", co_create_instance), SHIM(dll, "CoCreateGuid", co_create_guid),               \
    SHIM(dll, "CoTaskMemFree", co_task_mem_free), SHIM(dll, "CoTaskMemAlloc", co_task_mem_alloc),               \
    SHIM(dll, "PropVariantClear", prop_variant_clear), SHIM(dll, "StringFromGUID2", string_from_guid2),            \
    SHIM(dll, "CoGetApartmentType", co_get_apartment_type)
}  // namespace

SHIM_TABLE(com_shim,
    COM_L1_API("api-ms-win-core-com-l1-1-0.dll"),
    COM_L1_API("api-ms-win-core-com-l1-1-1.dll"),
    COM_L1_API("api-ms-win-core-com-l1-1-2.dll"),
    COM_L1_API("api-ms-win-core-com-l1-1-3.dll"),
    COM_L1_API("ole32.dll"),
);

// Host-side entry points declared in compat/objbase.h for code that calls COM directly (the guest reaches the same
// functions through the table above).
HRESULT WINAPI CoInitializeEx(LPVOID reserved, DWORD coinit) { return rt::com_shim::co_initialize_ex(reserved, coinit); }
void WINAPI CoUninitialize(void) { rt::com_shim::co_uninitialize(); }
HRESULT WINAPI CoGetApartmentType(APTTYPE* type, APTTYPEQUALIFIER* qualifier) {
    return rt::com_shim::co_get_apartment_type(reinterpret_cast<int32_t*>(type), reinterpret_cast<int32_t*>(qualifier));
}
HRESULT WINAPI CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID riid, LPVOID* ppv) {
    return rt::com_shim::co_create_instance(reinterpret_cast<const rt::com_shim::Guid*>(&clsid), outer, context,
                                            reinterpret_cast<const rt::com_shim::Guid*>(&riid), ppv);
}
LPVOID WINAPI CoTaskMemAlloc(SIZE_T size) { return rt::com_shim::co_task_mem_alloc(size); }
void WINAPI CoTaskMemFree(LPVOID p) { rt::com_shim::co_task_mem_free(p); }
HRESULT WINAPI CoCreateGuid(GUID* guid) { return rt::com_shim::co_create_guid(reinterpret_cast<rt::com_shim::Guid*>(guid)); }
int WINAPI StringFromGUID2(REFGUID guid, LPOLESTR out, int cch) {
    return rt::com_shim::string_from_guid2(reinterpret_cast<const rt::com_shim::Guid*>(&guid), out, cch);
}
