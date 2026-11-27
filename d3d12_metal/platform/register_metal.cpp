#include <gta6rt/d3d12_backend.h>
#include <gta6rt/shims.h>

// platform/register.cpp of d3d12_vulkan, compiled into this module, defines these two over this module's device; the
// prelink keeps them private, so these are the only names the host sees.
extern "C" gta6rt_status gta6rt_d3d12_metal_register(void) { return gta6rt_d3d12_vulkan_register(); }
extern "C" gta6rt_status gta6rt_d3d12_metal_at_exit(uint32_t wait_ms) { return gta6rt_d3d12_vulkan_at_exit(wait_ms); }
