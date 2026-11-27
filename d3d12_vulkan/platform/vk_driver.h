#pragma once

// The Vulkan driver, loaded directly (no Khronos loader, no ICD JSON): dlopen the driver dylib and take its
// vkGetInstanceProcAddr (or the ICD entry vk_icdGetInstanceProcAddr). spike/s1-vulkan-baseline measured both drivers
// this way on arm64.
//
// GTA6RT_VK_DRIVER picks it: "kosmickrisp", "moltenvk", or a path to a dylib. Unset: KosmicKrisp first, MoltenVK
// second. A named driver is looked for next to the executable, in ../Frameworks, then /opt/homebrew/lib and
// /usr/local/lib. A dylib of the wrong architecture (an arm64 Homebrew driver in an x86_64 process) fails to load
// and the next candidate is tried.

#include <vulkan/vulkan_core.h>

#include <string>

namespace rthost {

enum class VulkanDriverKind { unknown, kosmickrisp, moltenvk };

struct VulkanDriver {
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    std::string path;
    VulkanDriverKind kind = VulkanDriverKind::unknown;
    const char* name() const;
};

// Loaded once per process; null (logged, with every path tried) when no driver loads.
const VulkanDriver* vulkan_driver();

}  // namespace rthost
