#include <gta6rt/d3d12_backend.h>
#include <gta6rt/shims.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
std::atomic<const char*> g_backend{nullptr};
}

extern "C" gta6rt_status gta6rt_d3d12_backend_register(void) {
    const char* want = std::getenv("GTA6RT_GPU");
    if (!want || !*want || !std::strcmp(want, "vulkan")) {
        g_backend = "vulkan";
        return gta6rt_d3d12_vulkan_register();
    }
    if (!std::strcmp(want, "metal")) {
        g_backend = "metal";
        std::fprintf(stderr, "[host] GTA6RT_GPU=metal: d3d12_xs.dll on gta6rt::gpu (port/metal), no Vulkan driver\n");
        return gta6rt_d3d12_metal_register();
    }
    std::fprintf(stderr, "[host] GTA6RT_GPU=%s: unknown backend (vulkan or metal)\n", want);
    return GTA6RT_E_INVALID_ARG;
}

extern "C" const char* gta6rt_d3d12_backend_name(void) { return g_backend.load(); }

extern "C" gta6rt_status gta6rt_d3d12_backend_at_exit(uint32_t wait_ms) {
    const char* b = g_backend.load();
    if (b && !std::strcmp(b, "metal")) return gta6rt_d3d12_metal_at_exit(wait_ms);
    return gta6rt_d3d12_vulkan_at_exit(wait_ms);
}
