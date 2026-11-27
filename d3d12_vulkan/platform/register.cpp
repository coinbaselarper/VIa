#include "register.h"

#include "d3d12_compile_exit.h"
#include "shims.h"
#include "title_memory_probe.h"

#include <gta6rt/shims.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace rt::d3d12_vulkan {
extern const rt::shims::Entry kD3d12XsExports[];
extern const std::size_t kD3d12XsExportCount;
}  // namespace rt::d3d12_vulkan

namespace rt::d3d12_vulkan {

void* find_export(const char* dll, const char* name) {
    for (std::size_t i = 0; i < kD3d12XsExportCount; ++i) {
        const rt::shims::Entry& e = kD3d12XsExports[i];
        if (!std::strcmp(e.dll, dll) && !std::strcmp(e.name, name)) return e.fn;
    }
    return nullptr;
}

std::size_t export_count() { return kD3d12XsExportCount; }

}  // namespace rt::d3d12_vulkan

// The table holds ms_abi functions, which only x86_64 can hand to the guest as CODE_NATIVE exports. arm64 needs a
// frame adapter per entry (STATUS.md) and reports GTA6RT_E_UNSUPPORTED, as shims_gdk does.
extern "C" gta6rt_status gta6rt_d3d12_vulkan_register(void) {
#if defined(__x86_64__)
    static std::once_flag once;
    static gta6rt_status result = GTA6RT_OK;
    std::call_once(once, [] {
        rt::d3d12_vulkan::install_title_memory_probe();
        static auto* exports = new std::vector<gta6rt_shim_export>;
        for (std::size_t i = 0; i < rt::d3d12_vulkan::kD3d12XsExportCount; ++i) {
            const rt::shims::Entry& e = rt::d3d12_vulkan::kD3d12XsExports[i];
            gta6rt_shim_export x{};
            x.name = e.name;
            if (e.name[0] == '#') x.ordinal = static_cast<uint16_t>(std::strtoul(e.name + 1, nullptr, 10));
            x.kind = GTA6RT_SHIM_CODE_NATIVE;
            x.target = e.fn;
            exports->push_back(x);
        }
        exports->push_back({});
        result = gta6rt_shims_register_dll("d3d12_xs.dll", exports->data());
    });
    return result;
#else
    return GTA6RT_E_UNSUPPORTED;
#endif
}

// A process that ends with its device alive (the host's watchdog or the title's exit, both _exit): the compiler threads
// are stopped and the session pipeline cache written, as ~Device would. GTA6RT_E_NOT_FOUND when no cache was written.
extern "C" gta6rt_status gta6rt_d3d12_vulkan_at_exit(uint32_t wait_ms) {
    rt::d3d12::stop_pipeline_compiles(wait_ms);
    return rt::d3d12::save_pipeline_caches_at_exit(wait_ms) ? GTA6RT_OK : GTA6RT_E_NOT_FOUND;
}
