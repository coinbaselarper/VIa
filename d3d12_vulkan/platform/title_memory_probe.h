#pragma once

#include <cstdint>

namespace rt::d3d12_vulkan {

// The title's memory for the D3D12 layer's title windows (src/title_va_alias.h): an address no allocator reported is
// looked up in os/'s region table (XMemVirtualAlloc, VirtualAlloc), then in the task's VM map (XMemAlloc's malloc).
void install_title_memory_probe();
void title_memory_probe_counts(std::uint64_t* guest, std::uint64_t* host, std::uint64_t* missed);

}  // namespace rt::d3d12_vulkan
