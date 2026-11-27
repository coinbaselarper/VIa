#pragma once

// The Metal path's split of the GPU memory the layer holds, in the Vulkan path's categories and names
// (d3d12_vulkan/src/gpu_ledger.h), so both paths print "[rt] gpu-memory" lines that compare line for line. The sites
// that make a Metal object add its bytes under a category and remove them when it goes; Metal's own total
// (currentAllocatedSize) is printed beside the sum, and what no site tagged is "other". Title pages wrapped in place
// (slot-133 windows, buffer-backed title textures) add no memory and are counted apart, as host-pointer imports are
// on the Vulkan path.

#include <array>
#include <atomic>
#include <cstdint>

namespace leonida::metal_ledger {

enum Category : std::uint8_t {
    title_copy,     // Metal textures holding a copy of a texture the title keeps in its memory
    render_target,  // textures the GPU writes: render targets, depth, UAV textures
    staging,        // upload buffers
    title_shadow,   // buffers that mirror title memory (slot-133 shadows, title windows outside the arena)
    descriptor,     // the GPU address arena, bindless tables and the descriptor hash table
    buffer,         // D3D12 buffers outside the arena
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

struct State {
    std::array<std::atomic<std::int64_t>, kCount> bytes{}, count{};
    std::atomic<std::int64_t> imported_bytes{0}, imported_count{0};
    std::atomic<std::int64_t> deferred_bytes{0}, deferred_count{0};  // title textures with no Metal texture yet
};

inline State& state() {
    static State s;
    return s;
}

inline void add(Category c, std::uint64_t bytes) {
    state().bytes[c].fetch_add(std::int64_t(bytes), std::memory_order_relaxed);
    state().count[c].fetch_add(1, std::memory_order_relaxed);
}
inline void remove(Category c, std::uint64_t bytes) {
    state().bytes[c].fetch_sub(std::int64_t(bytes), std::memory_order_relaxed);
    state().count[c].fetch_sub(1, std::memory_order_relaxed);
}
inline void add_imported(std::uint64_t bytes) {
    state().imported_bytes.fetch_add(std::int64_t(bytes), std::memory_order_relaxed);
    state().imported_count.fetch_add(1, std::memory_order_relaxed);
}
inline void remove_imported(std::uint64_t bytes) {
    state().imported_bytes.fetch_sub(std::int64_t(bytes), std::memory_order_relaxed);
    state().imported_count.fetch_sub(1, std::memory_order_relaxed);
}
inline void add_deferred(std::uint64_t bytes, int sign) {
    state().deferred_bytes.fetch_add(sign * std::int64_t(bytes), std::memory_order_relaxed);
    state().deferred_count.fetch_add(sign, std::memory_order_relaxed);
}

inline double mib(std::int64_t bytes) { return double(bytes) / 1048576.0; }

}  // namespace leonida::metal_ledger
