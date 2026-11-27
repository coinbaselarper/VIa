#include "metal_bindless.h"
#include "metal_ledger.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace leonida {

namespace {

constexpr std::uint8_t kInUse = 1;

// Index counts by binding (vulkan_bindless.h's layout at the sizes its size_bindings() grows to on a large GPU; a
// Metal argument buffer of tier 2 holds them all).
constexpr std::array<std::uint32_t, bindless::binding_count> kCounts = {
    65536, 2048, 1, 16384, 4096, 4096, 4096, 4096,   // 0 2D, 1 samplers, 2 table, 3 2D array, 4 2D r32ui, 5 1D, 6 3D, 7 3D r32ui
    4096, 4096, 4096,                                // 8-10 2D storage
    4096, 4096, 4096,                                // 11-13 2D-array storage
    4096, 4096, 4096,                                // 14-16 3D storage
    4096, 4096, 4096,                                // 17-19 1D storage
    256, 256, 256, 256, 256, 256,                    // 20-25 MS by class
    256, 256, 256};                                  // 26-28 1D-array storage

gpu::DescriptorKind kind_of(std::uint32_t binding) {
    if (binding == bindless::sampler_binding) return gpu::DescriptorKind::sampler;
    if (binding == bindless::table_binding) return gpu::DescriptorKind::buffer;
    return gpu::DescriptorKind::texture;
}

}  // namespace

MetalBindless::MetalBindless(gpu::Device& device, MetalContext& context) : device_(device), context_(context) {
    counts_ = kCounts;
    for (std::uint32_t b = 0; b < bindless::binding_count; ++b) {
        if (b == bindless::table_binding) continue;
        gpu::DescriptorHeapDesc d;
        d.kind = kind_of(b);
        d.count = counts_[b];
        d.label = "set0";
        heaps_[b] = device_.create_descriptor_heap(d);
        if (!heaps_[b]) {
            std::fprintf(stderr, "[rt] d3d12-metal: bindless binding %u (%u entries) could not be created\n", b, d.count);
            return;
        }
    }
    using TD = gpu::TextureDimension;
    namespace f = gpu::format;
    gpu::Texture* sampled2d = make_fallback(TD::tex2d, f::r8g8b8a8_unorm, 1, false);
    gpu::Texture* sampled2da = make_fallback(TD::tex2d_array, f::r8g8b8a8_unorm, 1, false);
    gpu::Texture* sampled1d = make_fallback(TD::tex1d, f::r8g8b8a8_unorm, 1, false);
    gpu::Texture* sampled3d = make_fallback(TD::tex3d, f::r8g8b8a8_unorm, 1, false);
    fallback_[0] = sampled2d;
    fallback_[3] = sampled2da;
    fallback_[5] = sampled1d;
    fallback_[6] = sampled3d;
    fallback_[4] = make_fallback(TD::tex2d, f::r32_uint, 1, true);
    fallback_[7] = make_fallback(TD::tex3d, f::r32_uint, 1, true);
    const TD dims[4] = {TD::tex2d, TD::tex2d_array, TD::tex3d, TD::tex1d};
    const std::uint32_t classes[3] = {f::r32_float, f::r32_uint, 43};  // R32_SINT
    for (std::uint32_t d = 0; d < 4; ++d)
        for (std::uint32_t c = 0; c < 3; ++c) fallback_[8 + 3 * d + c] = make_fallback(dims[d], classes[c], 1, true);
    for (std::uint32_t c = 0; c < 3; ++c) {
        const std::uint32_t fmt = c == 0 ? f::r8g8b8a8_unorm : classes[c];
        fallback_[bindless::first_ms_binding + 2 * c] = make_fallback(TD::tex2d, fmt, 4, false);
        fallback_[bindless::first_ms_binding + 2 * c + 1] = make_fallback(TD::tex2d_array, fmt, 4, false);
        fallback_[bindless::first_1d_array_storage_binding + c] = make_fallback(TD::tex1d, classes[c], 1, true);
    }
    gpu::SamplerDesc sd;
    sd.min = sd.mag = sd.mip = gpu::Filter::point;
    sd.u = sd.v = sd.w = gpu::AddressMode::clamp;
    fallback_sampler_ = device_.create_sampler(sd);
    for (std::uint32_t b = 0; b < bindless::binding_count; ++b) {
        if (!heaps_[b]) continue;
        if (b == bindless::sampler_binding) {
            if (fallback_sampler_) heaps_[b]->write_sampler(0, fallback_sampler_.get());
        } else if (fallback_[b]) {
            heaps_[b]->write_texture(0, fallback_[b]);
        } else {
            std::fprintf(stderr, "[rt] d3d12-metal: bindless binding %u has no fallback texture\n", b);
        }
    }
    for (std::uint32_t d = 0; d < 7; ++d) image_used_[d].assign(image_capacity(ImageDimension(d)), 0);
    sampler_used_.assign(counts_[bindless::sampler_binding], 0);
    for (auto& used : image_used_)
        if (!used.empty()) used[0] = kInUse;
    if (!sampler_used_.empty()) sampler_used_[0] = kInUse;
    ok_ = fallback_sampler_ != nullptr;
    for (std::uint32_t b = 0; b < bindless::binding_count; ++b)
        if (heaps_[b]) context_.set_table(b, heaps_[b].get());
}

gpu::Texture* MetalBindless::make_fallback(gpu::TextureDimension dimension, std::uint32_t format, std::uint32_t samples,
                                           bool storage) {
    gpu::TextureDesc d;
    d.dimension = dimension;
    d.format = format;
    d.width = d.height = d.depth_or_layers = 1;
    d.samples = samples;
    d.usage = gpu::usage::shader_read | (storage ? gpu::usage::unordered_access : 0) |
              (samples > 1 ? gpu::usage::render_target : 0);
    d.memory = samples > 1 ? gpu::MemoryKind::device_local : gpu::MemoryKind::shared;
    d.label = "set0 fallback";
    auto t = device_.create_texture(d);
    if (!t) return nullptr;
    if (samples == 1) {
        const std::uint8_t zero[16] = {};
        t->write_region(0, 0, 0, 0, 1, 1, zero, 16);
    }
    gpu::Texture* out = t.get();
    fallback_textures_.push_back(std::move(t));
    return out;
}

std::uint32_t MetalBindless::image_capacity(ImageDimension dimension) const {
    const std::uint32_t b = bindless::sampled_binding(dimension);
    return b == bindless::no_binding ? storage_capacity(dimension) : counts_[b];
}

std::uint32_t MetalBindless::storage_capacity(ImageDimension dimension) const {
    const std::uint32_t b = bindless::storage_binding(dimension, 0);
    return b == bindless::no_binding ? 0 : counts_[b];
}

std::uint32_t MetalBindless::allocate(std::vector<std::uint8_t>& used, std::uint32_t from, std::uint32_t to) {
    for (std::uint32_t i = std::max(from, 1u); i < to && i < used.size(); ++i)
        if (!used[i]) {
            used[i] = kInUse;
            return i;
        }
    return 0;
}

void MetalBindless::write_image(ImageDimension dimension, std::uint32_t index, gpu::Texture* sampled,
                                const StorageViews& storage, std::uint32_t sampled_class) {
    const std::uint32_t sb = bindless::sampled_binding(dimension, sampled_class);
    if (sb != bindless::no_binding && heaps_[sb] && index < counts_[sb])
        heaps_[sb]->write_texture(index, sampled ? sampled : fallback_[sb]);
    if (bindless::multisampled(dimension)) return;
    gpu::Texture* by_class[3] = {storage.as_float, storage.as_uint, storage.as_sint};
    for (std::uint32_t c = 0; c < 3; ++c) {
        const std::uint32_t b = bindless::storage_binding(dimension, c);
        if (b != bindless::no_binding && heaps_[b] && index < counts_[b])
            heaps_[b]->write_texture(index, by_class[c] ? by_class[c] : fallback_[b]);
    }
    const std::uint32_t rb = bindless::r32ui_binding(dimension);
    if (rb != bindless::no_binding && heaps_[rb] && index < counts_[rb])
        heaps_[rb]->write_texture(index, storage.r32ui ? storage.r32ui : fallback_[rb]);
}

std::uint32_t MetalBindless::add_image(ImageDimension dimension, gpu::Texture* sampled, const StorageViews& storage,
                                       std::uint32_t sampled_class) {
    std::lock_guard lock(mutex_);
    const bool stores = storage.as_float || storage.as_uint || storage.as_sint || storage.r32ui;
    if (!stores && bindless::sampled_binding(dimension) == bindless::no_binding) return 0;
    if (bindless::multisampled(dimension) && (stores || !supports_ms_class(sampled_class))) return 0;
    auto& used = image_used_[std::uint32_t(dimension)];
    const std::uint32_t low_end = std::min(std::uint32_t(used.size()), storage_capacity(dimension));
    std::uint32_t index = stores ? 0 : allocate(used, low_end, std::uint32_t(used.size()));
    if (!index) index = allocate(used, 1, stores ? low_end : std::uint32_t(used.size()));
    if (!index) {
        ++full_;
        return 0;
    }
    write_image(dimension, index, sampled, storage, sampled_class);
    return index;
}

void MetalBindless::remove_image(ImageDimension dimension, std::uint32_t index) {
    std::lock_guard lock(mutex_);
    auto& used = image_used_[std::uint32_t(dimension)];
    if (!index || index >= used.size() || !used[index]) return;
    write_image(dimension, index, nullptr, {}, 0);
    used[index] = 0;
}

std::uint32_t MetalBindless::add_sampler(gpu::Sampler* sampler) {
    std::lock_guard lock(mutex_);
    const std::uint32_t index = allocate(sampler_used_, 1, std::uint32_t(sampler_used_.size()));
    if (index && heaps_[bindless::sampler_binding]) heaps_[bindless::sampler_binding]->write_sampler(index, sampler);
    if (!index) ++full_;
    return index;
}

void MetalBindless::remove_sampler(std::uint32_t index) {
    std::lock_guard lock(mutex_);
    if (!index || index >= sampler_used_.size() || !sampler_used_[index]) return;
    if (heaps_[bindless::sampler_binding])
        heaps_[bindless::sampler_binding]->write_sampler(index, fallback_sampler_.get());
    sampler_used_[index] = 0;
}

bool MetalBindless::insert_key(const DescriptorKey& key, std::uint32_t index) {
    std::lock_guard lock(mutex_);
    const auto result = table_.insert_or_update(key.data(), index);
    table_dirty_ |= result != DescriptorTable::InsertResult::unchanged;
    return true;
}

bool MetalBindless::erase_key(const DescriptorKey& key) {
    std::lock_guard lock(mutex_);
    if (!table_.erase(key)) return false;
    table_dirty_ = true;
    return true;
}

bool MetalBindless::generation_free(Generation& g, std::chrono::steady_clock::time_point now) {
    if (!g.retired) return false;
    if (!g.settled) {
        if (now - g.retired_time < std::chrono::milliseconds(100)) return false;
        g.settle_marks = context_.submitted();
        g.settled = true;
    }
    return context_.reached(g.retired_marks) && context_.reached(g.settle_marks);
}

bool MetalBindless::flush() {
    std::unique_lock lock(mutex_);
    if (!table_dirty_ && current_ != SIZE_MAX) return true;
    const std::size_t bytes = table_.size_bytes();
    auto now = std::chrono::steady_clock::now();
    std::size_t pick = SIZE_MAX;
    // GTA6RT_METAL_TABLE_GENERATIONS (default 8, 0: no cap): at that many live generations a flush waits (up to 2 s)
    // for one the queues have passed instead of making another: while command buffers take a second each on the GPU
    // a generation per dirty submission piled up to 48 (1.9 GiB, d3m-s7-b) and ended the run at the footprint cap.
    static const std::size_t cap = [] {
        const char* v = std::getenv("GTA6RT_METAL_TABLE_GENERATIONS");
        return std::size_t(v && *v ? std::strtoul(v, nullptr, 10) : 8);
    }();
    const auto wait_start = now;
    for (;;) {
        for (std::size_t i = 0; i < generations_.size();) {
            Generation& g = generations_[i];
            if (i == current_ || !generation_free(g, now)) {
                ++i;
                continue;
            }
            if (g.buffer && g.buffer->size() >= bytes) {
                if (pick == SIZE_MAX) pick = i;
                ++i;
                continue;
            }
            if (g.buffer) {
                generation_bytes_ -= g.buffer->size();
                metal_ledger::remove(metal_ledger::descriptor, g.buffer->size());
            }
            generations_.erase(generations_.begin() + std::ptrdiff_t(i));
            ++generations_freed_;
            if (current_ != SIZE_MAX && current_ > i) --current_;
            if (pick != SIZE_MAX && pick > i) --pick;
        }
        if (pick != SIZE_MAX || !cap || generations_.size() < cap || now - wait_start > std::chrono::seconds(2)) break;
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
        if (!table_dirty_ && current_ != SIZE_MAX) return true;
        now = std::chrono::steady_clock::now();
    }
    if (now != wait_start) {
        ++throttled_flushes_;
        throttled_us_ += std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - wait_start).count());
        if (throttled_flushes_ <= 4 || !(throttled_flushes_ & (throttled_flushes_ - 1)))
            std::fprintf(stderr, "[rt] d3d12-metal: descriptor hash table: %zu live generations, a flush waited %.1f ms for one "
                                 "the GPU passed%s (%llu waits, %.1f s in all)\n",
                         generations_.size(), double(std::chrono::duration_cast<std::chrono::microseconds>(now - wait_start).count()) / 1000.0,
                         pick == SIZE_MAX ? " and made another" : "",
                         static_cast<unsigned long long>(throttled_flushes_), double(throttled_us_) / 1e6);
    }
    if (current_ != SIZE_MAX && pick != current_) {
        Generation& old = generations_[current_];
        old.retired = true;
        old.settled = false;
        old.retired_time = now;
        old.retired_marks = context_.submitted();
        old.settle_marks.clear();
    }
    if (pick == SIZE_MAX) {
        Generation g;
        gpu::BufferDesc bd;
        // The table's size changes only when it doubles: a generation of its size is reused until then.
        bd.size = std::max<std::uint64_t>((bytes + 0xffff) & ~std::uint64_t(0xffff), 64 * 1024);
        bd.memory = gpu::MemoryKind::shared;
        bd.usage = gpu::usage::shader_read;
        bd.label = "descriptor hash table";
        g.buffer = device_.create_buffer(bd);
        gpu::DescriptorHeapDesc hd;
        hd.kind = gpu::DescriptorKind::buffer;
        hd.count = 1;
        hd.label = "set0 binding 2";
        g.binding = device_.create_descriptor_heap(hd);
        if (!g.buffer || !g.binding) {
            std::fprintf(stderr, "[rt] d3d12-metal: descriptor hash table: no buffer for %zu bytes\n", bytes);
            return false;
        }
        g.binding->write_buffer(0, g.buffer.get(), 0);
        generation_bytes_ += g.buffer->size();
        ++generations_made_;
        metal_ledger::add(metal_ledger::descriptor, g.buffer->size());
        generations_.push_back(std::move(g));
        pick = generations_.size() - 1;
    }
    // The slots changed since a reused generation was last written are rewritten in it (the change journal), the
    // whole table only for a new generation, after the table grew, or when the journal no longer reaches back.
    DescriptorTable::Changes changes = table_.take_changes();
    if (changes.all) {
        ++table_epoch_;
        journal_.clear();
        journal_base_ = journal_seq_;
    } else {
        journal_.insert(journal_.end(), changes.slots.begin(), changes.slots.end());
        journal_seq_ += changes.slots.size();
    }
    Generation& g = generations_[pick];
    g.retired = g.settled = false;
    auto* words = static_cast<std::uint32_t*>(g.buffer->cpu_pointer());
    if (g.epoch != table_epoch_ || !g.written || g.synced < journal_base_) {
        table_.write_to_buffer(words);
        ++full_writes_;
    } else {
        for (std::uint64_t k = g.synced - journal_base_; k < journal_.size(); ++k)
            table_.read_slot(journal_[k], words + 1 + std::size_t(journal_[k]) * 10);
        slot_writes_ += journal_.size() - (g.synced - journal_base_);
    }
    g.written = true;
    g.epoch = table_epoch_;
    g.synced = journal_seq_;
    // The journal keeps what the oldest live generation still needs, up to 4M slots.
    std::uint64_t oldest = journal_seq_;
    for (const Generation& other : generations_)
        if (other.written && other.epoch == table_epoch_) oldest = std::min(oldest, other.synced);
    if (journal_.size() > (4u << 20)) oldest = journal_seq_;
    if (oldest > journal_base_) {
        journal_.erase(journal_.begin(), journal_.begin() + std::ptrdiff_t(oldest - journal_base_));
        journal_base_ = oldest;
    }
    g.serial = ++serial_;
    current_ = pick;
    table_dirty_ = false;
    context_.set_table(bindless::table_binding, g.binding.get());
    return true;
}

MetalBindless::Stats MetalBindless::stats() const {
    std::lock_guard lock(mutex_);
    Stats s;
    s.generations = serial_;
    s.table_bytes = table_.size_bytes();
    for (const auto& used : image_used_) s.images += std::count(used.begin(), used.end(), kInUse);
    s.samplers = std::count(sampler_used_.begin(), sampler_used_.end(), kInUse);
    s.full = full_;
    s.live_generations = generations_.size();
    s.generation_bytes = generation_bytes_;
    s.generations_made = generations_made_;
    s.generations_freed = generations_freed_;
    s.full_writes = full_writes_;
    s.slot_writes = slot_writes_;
    return s;
}

}  // namespace leonida
