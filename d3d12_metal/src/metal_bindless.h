#pragma once

// Set 0 of the translated shaders on Metal: d3d12_vulkan/src/vulkan_bindless.h's binding map and index spaces over 29
// gpu::DescriptorHeaps (one argument buffer per contract binding, the split recipe), and the descriptor hash table of
// binding 2 (descriptor_table.h, unchanged) in a shared buffer. Index 0 of every binding is a fallback of the kind
// the binding declares, as on the Vulkan path.

#include "descriptor_table.h"
#include "metal_context.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace leonida {

enum class ImageDimension : std::uint32_t { dim_2d, dim_2d_array, dim_3d, dim_1d, dim_1d_array, dim_2d_ms, dim_2d_ms_array };

struct StorageViews {
    gpu::Texture* as_float = nullptr;
    gpu::Texture* as_uint = nullptr;
    gpu::Texture* as_sint = nullptr;
    gpu::Texture* r32ui = nullptr;
};

namespace bindless {
inline constexpr std::uint32_t first_ms_binding = 20;
inline constexpr std::uint32_t ms_binding_count = 6;
inline constexpr std::uint32_t first_1d_array_storage_binding = 26;
inline constexpr std::uint32_t no_binding = 0xffffffffu;

constexpr bool multisampled(ImageDimension d) {
    return d == ImageDimension::dim_2d_ms || d == ImageDimension::dim_2d_ms_array;
}
constexpr std::uint32_t sampled_binding(ImageDimension d, std::uint32_t component_class = 0) {
    constexpr std::uint32_t by_dimension[] = {0, 3, 6, 5, no_binding, first_ms_binding, first_ms_binding + 1};
    return by_dimension[std::uint32_t(d)] + (multisampled(d) ? component_class * 2 : 0);
}
constexpr std::uint32_t storage_binding(ImageDimension d, std::uint32_t component_class) {
    if (d == ImageDimension::dim_1d_array) return first_1d_array_storage_binding + component_class;
    return multisampled(d) ? no_binding : 8 + 3 * std::uint32_t(d) + component_class;
}
constexpr std::uint32_t r32ui_binding(ImageDimension d) {
    return d == ImageDimension::dim_2d ? 4 : d == ImageDimension::dim_3d ? 7 : no_binding;
}
}  // namespace bindless

class MetalBindless {
public:
    MetalBindless(gpu::Device& device, MetalContext& context);
    bool ok() const { return ok_; }

    // 0 when the dimension's array (or the shorter storage arrays, for an image with storage views) is full.
    std::uint32_t add_image(ImageDimension dimension, gpu::Texture* sampled, const StorageViews& storage = {},
                            std::uint32_t sampled_class = 0);
    void remove_image(ImageDimension dimension, std::uint32_t index);
    std::uint32_t add_sampler(gpu::Sampler* sampler);
    void remove_sampler(std::uint32_t index);
    bool insert_key(const DescriptorKey& key, std::uint32_t index);
    bool erase_key(const DescriptorKey& key);
    const DescriptorTable& table() const { return table_; }

    std::uint32_t image_capacity(ImageDimension dimension) const;
    std::uint32_t storage_capacity(ImageDimension dimension) const;
    std::uint32_t sampler_capacity() const { return counts_[bindless::sampler_binding]; }
    bool supports_ms_class(std::uint32_t component_class) const { return component_class < 3; }

    // Before a submission: a changed hash table goes to a new buffer (the GPU may still read the one before) and
    // binding 2 names it; every binding's heap is published to the context.
    bool flush();
    struct Stats {
        std::uint64_t generations = 0, table_bytes = 0, images = 0, samplers = 0, full = 0;
        std::uint64_t live_generations = 0, generation_bytes = 0, generations_made = 0, generations_freed = 0;
        std::uint64_t full_writes = 0, slot_writes = 0;
    };
    Stats stats() const;

private:
    void write_image(ImageDimension dimension, std::uint32_t index, gpu::Texture* sampled, const StorageViews& storage,
                     std::uint32_t sampled_class);
    static std::uint32_t allocate(std::vector<std::uint8_t>& used, std::uint32_t from, std::uint32_t to);
    gpu::Texture* make_fallback(gpu::TextureDimension dimension, std::uint32_t format, std::uint32_t samples,
                                bool storage);

    gpu::Device& device_;
    MetalContext& context_;
    bool ok_ = false;
    std::array<std::uint32_t, bindless::binding_count> counts_{};
    std::array<std::unique_ptr<gpu::DescriptorHeap>, bindless::binding_count> heaps_{};
    std::array<gpu::Texture*, bindless::binding_count> fallback_{};
    std::vector<std::unique_ptr<gpu::Texture>> fallback_textures_;
    std::unique_ptr<gpu::Sampler> fallback_sampler_;
    std::array<std::vector<std::uint8_t>, 7> image_used_;
    std::vector<std::uint8_t> sampler_used_;
    DescriptorTable table_;
    bool table_dirty_ = true;
    // A generation the table left may still be read by submissions in flight: it is reused, or freed when it is too
    // small, once the queues' progress passed what they had submitted when it was left and again what they had
    // submitted 100 ms later (a queue that was replaying with it then has submitted by that time).
    struct Generation {
        std::unique_ptr<gpu::Buffer> buffer;
        std::unique_ptr<gpu::DescriptorHeap> binding;
        std::uint64_t serial = 0;
        bool retired = false, settled = false;
        std::chrono::steady_clock::time_point retired_time{};
        std::vector<std::pair<gpu::Fence*, std::uint64_t>> retired_marks, settle_marks;
        bool written = false;
        std::uint64_t epoch = 0, synced = 0;  // the table epoch and journal position its contents match
    };
    std::vector<std::uint32_t> journal_;  // changed slots, journal_base_ .. journal_seq_
    std::uint64_t journal_base_ = 0, journal_seq_ = 0, table_epoch_ = 0, full_writes_ = 0, slot_writes_ = 0;
    std::uint64_t throttled_flushes_ = 0, throttled_us_ = 0;
    bool generation_free(Generation& g, std::chrono::steady_clock::time_point now);
    std::vector<Generation> generations_;
    std::uint64_t generations_freed_ = 0, generations_made_ = 0, generation_bytes_ = 0;
    std::size_t current_ = SIZE_MAX;
    std::uint64_t serial_ = 0;
    std::uint64_t full_ = 0;
    mutable std::mutex mutex_;
};

}  // namespace leonida
