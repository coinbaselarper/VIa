#pragma once

// Set 0 of the translated shaders on Metal: d3d12_vulkan/src/vulkan_bindless.h's binding map and index spaces over 29
// gpu::DescriptorHeaps (one argument buffer per contract binding, the split recipe), and the descriptor hash table of
// binding 2 (descriptor_table.h, unchanged) in a shared buffer. Index 0 of every binding is a fallback of the kind
// the binding declares, as on the Vulkan path.

#include "descriptor_table.h"
#include "metal_context.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
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

// The descriptor hash table written in place, into the buffer the GPU may be reading (MetalBindless::flush). A
// shader matches a slot's 8 key words and then loads its index, with no order between those loads and no pad check,
// so it can see any mix of a slot's old and new words. A change is safe when every mix gives an index the key had or
// 0 (the fallback): a new index for the same key; a key into an empty slot (all zeros); a key removed. One key replaced
// by another is not (a mix can pair the old key with the new index), nor is a refill of a slot emptied in place while
// GPU work that may have read it is in flight (its cached old key can pair with the new index): both go to a new
// generation instead.
class InPlaceRefills {
public:
    void emptied(std::uint32_t slot, std::uint64_t record) { emptied_[slot] = record; }
    bool blocked(std::uint32_t slot, std::uint64_t first_unreleased) const {
        const auto it = emptied_.find(slot);
        return it != emptied_.end() && it->second >= first_unreleased;
    }
    void prune(std::uint64_t first_unreleased) {
        if (emptied_.empty() || first_unreleased == pruned_) return;
        pruned_ = first_unreleased;
        for (auto it = emptied_.begin(); it != emptied_.end();)
            it = it->second < first_unreleased ? emptied_.erase(it) : std::next(it);
    }
    void clear() { emptied_.clear(); }
    std::size_t size() const { return emptied_.size(); }

private:
    std::unordered_map<std::uint32_t, std::uint64_t> emptied_;
    std::uint64_t pruned_ = 0;
};

// A CPU-only placeholder that keeps a slot emptied in place from taking another key until the GPU work that may have
// read the old key has passed (MetalBindless::erase_key / flush). Its key hashes to exactly that slot, so the table
// puts it there; the GPU copy of a tombstone slot stays all zeros (bindless::write_in_place). Key words 0-2 mark it.
inline constexpr std::uint32_t kTombstoneMark[3] = {0x70AB570Eu, 0xDEADDEADu, 0x5107DEADu};

inline bool is_tombstone(const std::uint32_t* key) {
    return key[0] == kTombstoneMark[0] && key[1] == kTombstoneMark[1] && key[2] == kTombstoneMark[2];
}

inline std::uint32_t odd_inverse(std::uint32_t a) {
    std::uint32_t x = a;
    for (int i = 0; i < 5; ++i) x *= 2u - a * x;
    return x;
}

// fmix32 run backwards: the value whose fmix32 is `h`.
inline std::uint32_t unfinalize_descriptor_hash(std::uint32_t h) {
    h ^= h >> 16;
    h *= odd_inverse(0xC2B2AE35u);
    h ^= (h >> 13) ^ (h >> 26);
    h *= odd_inverse(0x85EBCA6Bu);
    h ^= h >> 16;
    return h;
}

// A tombstone key (serial makes each one distinct) whose hash under `kind` is `slot`, so its home slot is `slot`.
inline DescriptorKey tombstone_key(std::uint32_t slot, std::uint64_t serial, DescriptorHashKind kind) {
    DescriptorKey k{};
    k[0] = kTombstoneMark[0];
    k[1] = kTombstoneMark[1];
    k[2] = kTombstoneMark[2];
    k[3] = std::uint32_t(serial);
    k[4] = std::uint32_t(serial >> 32);
    k[5] = 0x7051u;
    k[6] = slot;
    std::uint32_t h = kFnvOffsetBasis;
    for (int i = 0; i < 7; ++i) h = (h ^ k[i]) * kFnvPrime;
    const std::uint32_t want = kind == DescriptorHashKind::fnv1a_fmix32 ? unfinalize_descriptor_hash(slot) : slot;
    k[7] = h ^ (want * odd_inverse(kFnvPrime));
    return k;
}

struct InPlaceResult {
    bool written = false;
    bool emptied = false;  // some slot was emptied: the caller records `record`
    bool rekeyed = false;  // refused: a slot changes from one key to another
};

// `gpu` is the table buffer (word 0 the mask, then 10 words a slot) holding what `table` held before `slots` changed.
// Writes every changed slot when all are safe; writes nothing otherwise.
inline InPlaceResult write_in_place(std::uint32_t* gpu, const DescriptorTable& table,
                                    const std::vector<std::uint32_t>& slots, InPlaceRefills& refills,
                                    std::uint64_t record, std::uint64_t first_unreleased) {
    enum Kind : std::uint8_t { same, index, fill, empty };
    InPlaceResult out;
    std::vector<std::uint32_t> next(slots.size() * 10);
    std::vector<std::uint8_t> kinds(slots.size());
    const std::size_t capacity = table.capacity();
    const auto zero = [](const std::uint32_t* w) {
        for (int i = 0; i < 10; ++i)
            if (w[i]) return false;
        return true;
    };
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i] >= capacity || !table.read_slot(slots[i], &next[i * 10])) return out;
        if (is_tombstone(&next[i * 10])) std::memset(&next[i * 10], 0, 40);
        const std::uint32_t* now = gpu + 1 + std::size_t(slots[i]) * 10;
        const std::uint32_t* to = &next[i * 10];
        if (std::memcmp(now, to, 32) == 0) {
            kinds[i] = std::memcmp(now + 8, to + 8, 8) == 0 ? same : index;
        } else if (zero(to)) {
            kinds[i] = empty;
        } else if (zero(now)) {
            if (refills.blocked(slots[i], first_unreleased)) return out;
            kinds[i] = fill;
        } else {
            out.rekeyed = true;
            return out;
        }
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
        volatile std::uint32_t* dst = gpu + 1 + std::size_t(slots[i]) * 10;
        const std::uint32_t* to = &next[i * 10];
        switch (kinds[i]) {
        case same:
            break;
        case index:
            dst[8] = to[8];
            dst[9] = to[9];
            break;
        case fill:
            for (int w = 9; w >= 1; --w) dst[w] = to[w];
            dst[0] = to[0];
            break;
        case empty:
            dst[0] = 0;
            for (int w = 1; w < 10; ++w) dst[w] = 0;
            refills.emptied(slots[i], record);
            out.emptied = true;
            break;
        }
    }
    std::atomic_thread_fence(std::memory_order_release);
    out.written = true;
    return out;
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
        std::uint64_t full_writes = 0, slot_writes = 0, in_place_flushes = 0, in_place_refused_rekey = 0;
        std::uint64_t in_place_refused_refill = 0;
        std::uint64_t tombstones_live = 0, tombstones_made = 0;
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
    std::uint64_t throttled_flushes_ = 0, throttled_us_ = 0, in_place_flushes_ = 0;
    std::uint64_t in_place_refused_rekey_ = 0, in_place_refused_refill_ = 0;
    // Tombstones by serial (bindless::tombstone_key): `record` is the empty record of the flush that zeroed the slot on
    // the GPU (0: not flushed yet); the tombstone leaves the table once that record is released.
    struct Tombstone {
        DescriptorKey key;
        std::uint64_t record = 0;
    };
    std::unordered_map<std::uint64_t, Tombstone> tombstones_;
    std::uint64_t tombstone_serial_ = 0, tombstones_made_ = 0, tombstones_released_ = 0, tombstones_refused_ = 0;
    void release_tombstones(std::uint64_t first_unreleased, bool all);
    void date_tombstones(std::uint64_t record);
    void free_settled_generations(std::chrono::steady_clock::time_point now);
    bool cluster_refused(const DescriptorKey& key);
    std::uint64_t cluster_refused_ = 0;
    std::chrono::steady_clock::time_point last_free_scan_{};
    bool generation_free(Generation& g, std::chrono::steady_clock::time_point now);
    DescriptorTable::Changes take_all_changes();
    std::uint64_t release_empty_records(std::chrono::steady_clock::time_point now);
    DescriptorTable::Changes carried_;
    struct EmptyRecord {
        std::uint64_t id = 0;
        std::chrono::steady_clock::time_point time{};
        std::vector<std::pair<gpu::Fence*, std::uint64_t>> marks, settle_marks;
        bool settled = false;
    };
    std::deque<EmptyRecord> empty_records_;
    std::uint64_t next_record_ = 1;
    bindless::InPlaceRefills refills_;
    std::chrono::steady_clock::time_point last_report_{};
    void report_in_place(std::chrono::steady_clock::time_point now);
    std::vector<Generation> generations_;
    std::uint64_t generations_freed_ = 0, generations_made_ = 0, generation_bytes_ = 0;
    std::size_t current_ = SIZE_MAX;
    std::uint64_t serial_ = 0;
    std::uint64_t full_ = 0;
    mutable std::mutex mutex_;
};

}  // namespace leonida
