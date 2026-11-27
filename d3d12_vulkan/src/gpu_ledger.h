#pragma once

// port/: GPU memory the layer holds, by what it is for. Every vkAllocateMemory of the runtime's function table is
// recorded under the category of the scope it runs in (Scope, thread-local; "other" outside any), so the sum is
// exact and the split is as good as the scopes. Host-pointer imports (title pages the GPU reads in place) are kept
// apart: they add no memory. Also live pipelines, and allocation failures by category.

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace leonida::gpu_ledger {

enum Category : std::uint8_t {
    title_copy,     // private images of textures the title keeps in its memory (slot-70 images, sampled slot-133)
    render_target,  // images the GPU writes: render targets, depth, UAV textures (slot 133 and ordinary)
    staging,        // upload and readback rings, staged copies
    title_shadow,   // buffers that mirror title memory (slot-133 shadows, title windows)
    descriptor,     // the GPU address arena (descriptor heaps, arena buffers), bindless tables
    buffer,         // D3D12 buffers outside the arena (upload/readback heaps, committed buffers)
    texture,        // the layer's other textures
    present,        // swapchain, compose, capture
    other,
    kCount
};

inline const char* name(int c) {
    static const char* names[kCount] = {"title copies", "render targets", "staging", "title shadows",
                                        "descriptors/arena", "buffers", "textures", "present", "other"};
    return c >= 0 && c < kCount ? names[c] : "?";
}

struct Totals {
    std::array<std::uint64_t, kCount> bytes{}, count{}, failed{}, failed_bytes{};
    std::uint64_t imported_bytes = 0, imported_count = 0, pipelines = 0, pipelines_made = 0;
    std::uint64_t driver_bytes() const {
        std::uint64_t n = 0;
        for (auto b : bytes) n += b;
        return n;
    }
    std::uint64_t driver_count() const {
        std::uint64_t n = 0;
        for (auto b : count) n += b;
        return n;
    }
};

struct State {
    PFN_vkAllocateMemory allocate = nullptr;
    PFN_vkFreeMemory free = nullptr;
    PFN_vkCreateGraphicsPipelines create_graphics = nullptr;
    PFN_vkCreateComputePipelines create_compute = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    std::mutex mutex;
    struct Allocation {
        std::uint64_t bytes;
        Category category;
        bool imported;
    };
    std::unordered_map<VkDeviceMemory, Allocation> allocations;
    Totals totals;
    bool installed = false;
};

inline State& state() {
    static State s;
    return s;
}

inline thread_local Category t_category = other;

class Scope {
public:
    explicit Scope(Category c) : saved_(t_category) { t_category = c; }
    ~Scope() { t_category = saved_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Category saved_;
};

inline bool imported_memory(const VkMemoryAllocateInfo* info) {
    for (auto* p = static_cast<const VkBaseInStructure*>(info->pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT ||
            p->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR)
            return true;
    return false;
}

inline VKAPI_ATTR VkResult VKAPI_CALL allocate_memory(VkDevice device, const VkMemoryAllocateInfo* info,
                                                      const VkAllocationCallbacks* callbacks, VkDeviceMemory* out) {
    State& s = state();
    const VkResult result = s.allocate(device, info, callbacks, out);
    const Category c = t_category;
    const bool imported = imported_memory(info);
    std::lock_guard lock(s.mutex);
    if (result == VK_SUCCESS && out && *out) {
        s.allocations[*out] = {info->allocationSize, c, imported};
        if (imported) {
            s.totals.imported_bytes += info->allocationSize;
            ++s.totals.imported_count;
        } else {
            s.totals.bytes[c] += info->allocationSize;
            ++s.totals.count[c];
        }
    } else if (!imported) {
        ++s.totals.failed[c];
        s.totals.failed_bytes[c] += info->allocationSize;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 24)
            std::fprintf(stderr, "[rt] gpu-memory: vkAllocateMemory of %llu bytes (memory type %u) for %s failed (%d)\n",
                         static_cast<unsigned long long>(info->allocationSize), info->memoryTypeIndex, name(c),
                         int(result));
    }
    return result;
}

inline VKAPI_ATTR void VKAPI_CALL free_memory(VkDevice device, VkDeviceMemory memory,
                                              const VkAllocationCallbacks* callbacks) {
    State& s = state();
    if (memory) {
        std::lock_guard lock(s.mutex);
        if (auto it = s.allocations.find(memory); it != s.allocations.end()) {
            if (it->second.imported) {
                s.totals.imported_bytes -= it->second.bytes;
                --s.totals.imported_count;
            } else {
                s.totals.bytes[it->second.category] -= it->second.bytes;
                --s.totals.count[it->second.category];
            }
            s.allocations.erase(it);
        }
    }
    s.free(device, memory, callbacks);
}

inline void note_pipelines(std::uint32_t count, const VkPipeline* made) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    for (std::uint32_t i = 0; i < count; ++i)
        if (made[i]) {
            ++s.totals.pipelines;
            ++s.totals.pipelines_made;
        }
}

inline VKAPI_ATTR VkResult VKAPI_CALL create_graphics_pipelines(VkDevice device, VkPipelineCache cache,
                                                                std::uint32_t count,
                                                                const VkGraphicsPipelineCreateInfo* infos,
                                                                const VkAllocationCallbacks* callbacks,
                                                                VkPipeline* out) {
    const VkResult result = state().create_graphics(device, cache, count, infos, callbacks, out);
    if (result >= 0 && out) note_pipelines(count, out);
    return result;
}

inline VKAPI_ATTR VkResult VKAPI_CALL create_compute_pipelines(VkDevice device, VkPipelineCache cache,
                                                               std::uint32_t count,
                                                               const VkComputePipelineCreateInfo* infos,
                                                               const VkAllocationCallbacks* callbacks,
                                                               VkPipeline* out) {
    const VkResult result = state().create_compute(device, cache, count, infos, callbacks, out);
    if (result >= 0 && out) note_pipelines(count, out);
    return result;
}

inline VKAPI_ATTR void VKAPI_CALL destroy_pipeline(VkDevice device, VkPipeline pipeline,
                                                   const VkAllocationCallbacks* callbacks) {
    State& s = state();
    if (pipeline) {
        std::lock_guard lock(s.mutex);
        if (s.totals.pipelines) --s.totals.pipelines;
    }
    s.destroy_pipeline(device, pipeline, callbacks);
}

// Wraps the runtime's function table before anything allocates. Once per process: the first device wins.
template<typename Functions>
void install(Functions& vk) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.installed || !vk.vkAllocateMemory || !vk.vkFreeMemory || !vk.vkCreateGraphicsPipelines ||
        !vk.vkCreateComputePipelines || !vk.vkDestroyPipeline)
        return;
    s.installed = true;
    s.allocate = vk.vkAllocateMemory, vk.vkAllocateMemory = allocate_memory;
    s.free = vk.vkFreeMemory, vk.vkFreeMemory = free_memory;
    s.create_graphics = vk.vkCreateGraphicsPipelines, vk.vkCreateGraphicsPipelines = create_graphics_pipelines;
    s.create_compute = vk.vkCreateComputePipelines, vk.vkCreateComputePipelines = create_compute_pipelines;
    s.destroy_pipeline = vk.vkDestroyPipeline, vk.vkDestroyPipeline = destroy_pipeline;
}

inline Totals totals() {
    State& s = state();
    std::lock_guard lock(s.mutex);
    return s.totals;
}

inline double mib(std::uint64_t bytes) { return double(bytes) / 1048576.0; }

}  // namespace leonida::gpu_ledger
