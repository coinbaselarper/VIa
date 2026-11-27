#pragma once

#include <cstddef>

namespace rt::d3d12_vulkan {

// The module's guest-facing export table (d3d12_xs.dll), the one gta6rt_d3d12_vulkan_register hands to shims_core.
void* find_export(const char* dll, const char* name);
std::size_t export_count();

}  // namespace rt::d3d12_vulkan
