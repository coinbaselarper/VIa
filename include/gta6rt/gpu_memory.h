#ifndef GTA6RT_GPU_MEMORY_H
#define GTA6RT_GPU_MEMORY_H

/*
 * GPU memory of the D3D12 layer (d3d12_vulkan/), for the host's governor and reports.
 *
 * Both symbols live in the gta6rt executable (gta6rt_d3d12_vulkan, force-loaded). They are weak imports so a module
 * that links without the layer still links: test `if (gta6rt_gpu_memory_report)` before calling, or look them up
 * with dlsym(RTLD_DEFAULT, ...). Neither blocks on a title thread; both are safe from any thread once the process
 * has started, and answer 0 / valid = 0 before the layer has a device.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTA6RT_GPU_MEMORY_API 1

typedef struct gta6rt_gpu_bytes {
    uint32_t api;   /* GTA6RT_GPU_MEMORY_API, set by the layer */
    uint32_t valid; /* 0: no device yet; the rest is zero */
    /* VkDeviceMemory the layer allocated, by purpose (bytes). Their sum is driver_total. */
    uint64_t title_copies;   /* private images of textures the title keeps in its own memory */
    uint64_t render_targets; /* images the GPU writes: render targets, depth, UAV textures */
    uint64_t staging;        /* upload/readback rings and staged copies */
    uint64_t title_shadows;  /* buffers that mirror title memory */
    uint64_t descriptors;    /* GPU address arena (descriptor heaps, arena buffers), bindless tables */
    uint64_t buffers;        /* D3D12 buffers outside the arena */
    uint64_t textures;       /* the layer's other textures */
    uint64_t present;        /* swapchain, compose, capture */
    uint64_t other;
    uint64_t driver_total;
    uint64_t driver_allocations;
    /* Title pages the GPU reads in place (host-pointer imports): mapped for the GPU, not extra memory. */
    uint64_t imported;
    /* Title copies that gta6rt_gpu_memory_release can drop now (the title's bytes stay the source). */
    uint64_t evictable;
    /* VK_EXT_memory_budget, summed over heaps (0 when the driver lacks it). */
    uint64_t heap_usage;
    uint64_t heap_budget;
    /* The Metal device's currentAllocatedSize for this process (0 when unknown), recommendedMaxWorkingSetSize. */
    uint64_t metal_allocated;
    uint64_t metal_working_set;
    uint64_t pipelines;          /* live VkPipelines */
    uint64_t failed_allocations; /* vkAllocateMemory failures so far */
} gta6rt_gpu_bytes;

/* Fills *out; returns 0, or -1 when out is null. */
__attribute__((weak_import)) int gta6rt_gpu_memory_report(gta6rt_gpu_bytes* out);

/* Drops up to max_bytes of evictable title copies now (least recently used first; 0 = all evictable) and returns the
 * bytes released. The images go through the layer's deferred deletion, so the driver frees them once no submission
 * in flight can read them (a few frames). A released texture is made again from title memory when next used. */
__attribute__((weak_import)) uint64_t gta6rt_gpu_memory_release(uint64_t max_bytes);

#ifdef __cplusplus
}
#endif

#endif
