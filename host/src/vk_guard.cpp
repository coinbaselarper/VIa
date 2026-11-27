// A pass-through Vulkan driver that reports image views Metal would abort on.
//
// The D3D12 layer loads GTA6RT_VK_DRIVER with dlopen and takes vkGetInstanceProcAddr from it; the host points that
// variable here and GTA6RT_VK_GUARD_DRIVER at the real driver. Every command goes to the real driver unchanged; only
// vkCreateImage/vkDestroyImage (to learn each image's format) and vkCreateImageView are seen first. A view whose format
// Metal cannot reinterpret the image as (a different texel size, a compressed format of another family, compressed
// against uncompressed) aborts the process in MTLTexture newTextureViewWithPixelFormat (s3r4-c: a BC1_SRGB view of a
// BC7 image). It used to be refused here; the D3D12 layer now refuses such views itself and gives a T# that names
// another texture at a placement's address an image of its own (round 5), so one reaching the driver is a layer bug:
// it is logged and counted, then passed on, and Metal's assertion ends the run.
#include <vulkan/vulkan.h>

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

namespace {

PFN_vkGetInstanceProcAddr g_real_gipa = nullptr;
PFN_vkGetDeviceProcAddr g_real_gdpa = nullptr;
PFN_vkCreateImage g_real_create_image = nullptr;
PFN_vkDestroyImage g_real_destroy_image = nullptr;
PFN_vkCreateImageView g_real_create_view = nullptr;

std::mutex g_mu;
std::unordered_map<VkImage, VkFormat> g_images;
std::map<std::pair<int, int>, unsigned long long> g_refused;
std::atomic<unsigned long long> g_refusals{0};

bool load_real() {
    static const bool ok = [] {
        const char* path = std::getenv("GTA6RT_VK_GUARD_DRIVER");
        if (!path || !*path) {
            std::fprintf(stderr, "[host] vk_guard: GTA6RT_VK_GUARD_DRIVER is not set; no driver to forward to\n");
            return false;
        }
        void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            std::fprintf(stderr, "[host] vk_guard: dlopen %s: %s\n", path, dlerror());
            return false;
        }
        g_real_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vkGetInstanceProcAddr"));
        if (!g_real_gipa) g_real_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vk_icdGetInstanceProcAddr"));
        std::fprintf(stderr, "[host] vk_guard: forwarding to %s%s\n", path, g_real_gipa ? "" : " (no vkGetInstanceProcAddr)");
        return g_real_gipa != nullptr;
    }();
    return ok;
}

// Bytes per texel of the uncompressed core formats; 0 for formats this table does not know.
unsigned texel_bytes(VkFormat f) {
    const int v = int(f);
    if (v == 1) return 1;
    if (v >= 2 && v <= 8) return 2;
    if (v >= 9 && v <= 15) return 1;
    if (v >= 16 && v <= 22) return 2;
    if (v >= 23 && v <= 36) return 3;
    if (v >= 37 && v <= 69) return 4;
    if (v >= 70 && v <= 76) return 2;
    if (v >= 77 && v <= 83) return 4;
    if (v >= 84 && v <= 90) return 6;
    if (v >= 91 && v <= 97) return 8;
    if (v >= 98 && v <= 100) return 4;
    if (v >= 101 && v <= 103) return 8;
    if (v >= 104 && v <= 106) return 12;
    if (v >= 107 && v <= 109) return 16;
    if (v >= 110 && v <= 112) return 8;
    if (v >= 113 && v <= 115) return 16;
    if (v >= 116 && v <= 118) return 24;
    if (v >= 119 && v <= 121) return 32;
    if (v == 122 || v == 123) return 4;
    if (v == VK_FORMAT_A4R4G4B4_UNORM_PACK16 || v == VK_FORMAT_A4B4G4R4_UNORM_PACK16) return 2;
    return 0;
}

// BC family (BC1 RGB/RGBA share Metal's BC1_RGBA), or -1 for a format that is not BC.
int bc_family(VkFormat f) {
    const int v = int(f);
    if (v < VK_FORMAT_BC1_RGB_UNORM_BLOCK || v > VK_FORMAT_BC7_SRGB_BLOCK) return -1;
    if (v <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK) return 1;
    return 2 + (v - VK_FORMAT_BC2_UNORM_BLOCK) / 2;
}

bool other_compressed(VkFormat f) {
    const int v = int(f);
    return (v >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && v <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ||
           (v >= 1000066000 && v <= 1000066013) || (v >= 1000054000 && v <= 1000054007);
}

bool depth_stencil(VkFormat f) { return int(f) >= VK_FORMAT_D16_UNORM && int(f) <= VK_FORMAT_D32_SFLOAT_S8_UINT; }

// Why Metal cannot view an image of `image` format as `view`; nullptr when it can (or when this table cannot tell).
const char* incompatible(VkFormat image, VkFormat view) {
    if (image == view || image == VK_FORMAT_UNDEFINED || view == VK_FORMAT_UNDEFINED) return nullptr;
    if (depth_stencil(image) || depth_stencil(view)) return nullptr;
    const int bi = bc_family(image), bv = bc_family(view);
    if (bi >= 0 || bv >= 0) {
        if (bi >= 0 && bv >= 0) return bi == bv ? nullptr : "compressed formats of different families";
        return "a compressed and an uncompressed format";
    }
    if (other_compressed(image) || other_compressed(view)) return nullptr;
    const unsigned a = texel_bytes(image), b = texel_bytes(view);
    if (a && b && a != b) return "texel sizes differ";
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL guard_create_image(VkDevice device, const VkImageCreateInfo* info,
                                                  const VkAllocationCallbacks* alloc, VkImage* image) {
    const VkResult r = g_real_create_image(device, info, alloc, image);
    if (r == VK_SUCCESS && info && image) {
        std::lock_guard lock(g_mu);
        g_images[*image] = info->format;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL guard_destroy_image(VkDevice device, VkImage image, const VkAllocationCallbacks* alloc) {
    {
        std::lock_guard lock(g_mu);
        g_images.erase(image);
    }
    g_real_destroy_image(device, image, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL guard_create_view(VkDevice device, const VkImageViewCreateInfo* info,
                                                 const VkAllocationCallbacks* alloc, VkImageView* view) {
    if (info) {
        VkFormat image_format = VK_FORMAT_UNDEFINED;
        {
            std::lock_guard lock(g_mu);
            if (auto it = g_images.find(info->image); it != g_images.end()) image_format = it->second;
        }
        if (const char* why = incompatible(image_format, info->format)) {
            const unsigned long long total = ++g_refusals;
            unsigned long long n;
            {
                std::lock_guard lock(g_mu);
                n = ++g_refused[{int(image_format), int(info->format)}];
            }
            std::fprintf(stderr,
                         "[host] vk_guard: a view in VkFormat %d of an image in VkFormat %d reached the driver (%s; Metal "
                         "aborts on it; the D3D12 layer should have refused it): %llu such, %llu in all\n",
                         int(info->format), int(image_format), why, n, total);
        }
    }
    return g_real_create_view(device, info, alloc, view);
}

PFN_vkVoidFunction intercept(const char* name, PFN_vkVoidFunction real);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL guard_gdpa(VkDevice device, const char* name) {
    if (!g_real_gdpa || !name) return nullptr;
    return intercept(name, g_real_gdpa(device, name));
}

PFN_vkVoidFunction intercept(const char* name, PFN_vkVoidFunction real) {
    if (!real) return real;
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) {
        g_real_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&guard_gdpa);
    }
    if (!std::strcmp(name, "vkCreateImage")) {
        g_real_create_image = reinterpret_cast<PFN_vkCreateImage>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&guard_create_image);
    }
    if (!std::strcmp(name, "vkDestroyImage")) {
        g_real_destroy_image = reinterpret_cast<PFN_vkDestroyImage>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&guard_destroy_image);
    }
    if (!std::strcmp(name, "vkCreateImageView")) {
        g_real_create_view = reinterpret_cast<PFN_vkCreateImageView>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&guard_create_view);
    }
    return real;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!load_real() || !name) return nullptr;
    if (!std::strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
    return intercept(name, g_real_gipa(instance, name));
}

extern "C" __attribute__((visibility("default"))) unsigned long long gta6rt_vk_guard_refusals() { return g_refusals.load(); }
