#pragma once

// Stands in for d3d12_vulkan/shims/d3d12_pipeline_compile.h in this module (VFS overlay): d3d12_xs_device.cpp uses
// only note_present_for_warmup from it. Pipelines here are built by the backend's compile threads (metal_shaders.cpp).

#include <chrono>

namespace rt::d3d12 {
void note_present_for_warmup();
inline long long compile_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace rt::d3d12
