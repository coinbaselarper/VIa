#include "vk_driver.h"

#include <dlfcn.h>
#include <mach-o/dyld.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

namespace rthost {
namespace {

struct Candidate {
    std::string path;
    VulkanDriverKind kind;
};

const char* file_name(VulkanDriverKind kind) {
    switch (kind) {
    case VulkanDriverKind::kosmickrisp: return "libvulkan_kosmickrisp.dylib";
    case VulkanDriverKind::moltenvk: return "libMoltenVK.dylib";
    default: return nullptr;
    }
}

std::filesystem::path executable_dir() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(std::filesystem::path(path.c_str()), ec);
    return ec ? std::filesystem::path(path.c_str()).parent_path() : resolved.parent_path();
}

void add_named(std::vector<Candidate>& out, VulkanDriverKind kind) {
    const char* name = file_name(kind);
    const auto exe = executable_dir();
    if (!exe.empty()) {
        out.push_back({(exe / name).string(), kind});
        out.push_back({(exe / ".." / "Frameworks" / name).string(), kind});
    }
    out.push_back({std::string("/opt/homebrew/lib/") + name, kind});
    out.push_back({std::string("/usr/local/lib/") + name, kind});
}

VulkanDriverKind kind_of(const std::string& path) {
    if (path.find("kosmickrisp") != std::string::npos) return VulkanDriverKind::kosmickrisp;
    if (path.find("MoltenVK") != std::string::npos) return VulkanDriverKind::moltenvk;
    return VulkanDriverKind::unknown;
}

VulkanDriver load() {
    std::vector<Candidate> candidates;
    const char* choice = std::getenv("GTA6RT_VK_DRIVER");
    if (choice && *choice) {
        if (!std::strcmp(choice, "kosmickrisp")) add_named(candidates, VulkanDriverKind::kosmickrisp);
        else if (!std::strcmp(choice, "moltenvk")) add_named(candidates, VulkanDriverKind::moltenvk);
        else candidates.push_back({choice, kind_of(choice)});
    } else {
        add_named(candidates, VulkanDriverKind::kosmickrisp);
        add_named(candidates, VulkanDriverKind::moltenvk);
    }
    std::string tried;
    for (const Candidate& c : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(c.path, ec)) continue;
        void* handle = dlopen(c.path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            const char* error = dlerror();
            tried += "\n  " + c.path + ": " + (error ? error : "dlopen failed");
            continue;
        }
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
        if (!gipa) gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vk_icdGetInstanceProcAddr"));
        if (!gipa) {
            tried += "\n  " + c.path + ": no vkGetInstanceProcAddr or vk_icdGetInstanceProcAddr";
            dlclose(handle);
            continue;
        }
        VulkanDriver driver;
        driver.get_instance_proc = gipa;
        driver.path = c.path;
        driver.kind = c.kind;
        std::fprintf(stderr, "[rt] vk_driver: %s (%s)\n", driver.name(), c.path.c_str());
        return driver;
    }
    std::fprintf(stderr, "[rt] vk_driver: no Vulkan driver loaded (GTA6RT_VK_DRIVER=%s)%s\n",
                 choice && *choice ? choice : "<unset: kosmickrisp, then moltenvk>",
                 tried.empty() ? "; no candidate file exists" : tried.c_str());
    return {};
}

}  // namespace

const char* VulkanDriver::name() const {
    switch (kind) {
    case VulkanDriverKind::kosmickrisp: return "KosmicKrisp";
    case VulkanDriverKind::moltenvk: return "MoltenVK";
    default: return "Vulkan driver";
    }
}

const VulkanDriver* vulkan_driver() {
    static const VulkanDriver driver = load();
    return driver.get_instance_proc ? &driver : nullptr;
}

}  // namespace rthost
