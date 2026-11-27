#include "metal_bindless.h"
#include "metal_ledger.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

// GTA6RT_METAL_TABLE_HASH=fnv1a (default) | fnv1a_fmix32: the hash the table places keys with. Every module the store
// and its overlays hand out must probe with the same kind (shaderc_msl `--hash-kind`, d3d12_metal/tools/rehash_world.sh);
// the Metal store of round 14 is all bare FNV-1a.
DescriptorHashKind table_hash_kind() {
    const char* v = std::getenv("GTA6RT_METAL_TABLE_HASH");
    const std::string k = v ? v : "";
    if (k.empty() || k == "fnv1a") return DescriptorHashKind::fnv1a;
    if (k == "fnv1a_fmix32" || k == "fmix32") {
        std::fprintf(stderr, "[rt] d3d12-metal: descriptor hash table: fnv1a_fmix32 (GTA6RT_METAL_TABLE_HASH)\n");
        return DescriptorHashKind::fnv1a_fmix32;
    }
    std::fprintf(stderr, "[rt] d3d12-metal: GTA6RT_METAL_TABLE_HASH=%s is not a hash kind (fnv1a, fnv1a_fmix32)\n", v);
    std::abort();
}

}  // namespace

MetalBindless::MetalBindless(gpu::Device& device, MetalContext& context)
    : device_(device), context_(context), table_(16, table_hash_kind()) {
    counts_ = kCounts;
    // GTA6RT_METAL_BINDLESS_WIDE=1 (off by default): the 1D bindings and the 2D storage bindings get the 2D sampled
    // count. A 2D index with a storage view must lie below the storage count, so 4,096 title UAV T#s filled the 2D
    // index space for them about 40 s into the world, and 4,096 1D T#s filled the 1D one before it.
    if (const char* v = std::getenv("GTA6RT_METAL_BINDLESS_WIDE"); v && std::strcmp(v, "0") != 0) {
        for (const std::uint32_t b : {4u, 5u, 8u, 9u, 10u, 17u, 18u, 19u}) counts_[b] = kCounts[0];
        std::fprintf(stderr, "[rt] d3d12-metal: bindless 1D and 2D storage bindings widened to %u entries "
                             "(GTA6RT_METAL_BINDLESS_WIDE)\n", kCounts[0]);
    }
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

// GTA6RT_METAL_TABLE_CLUSTER_CAP_MB (default 160 since round 15 with bare FNV-1a, 0 with fnv1a_fmix32; 0: off): once
// the table is that large, a key whose 8 probe slots are all taken (no relocation possible) is refused instead of
// doubling the table, when the table is under 1/8 full. The shaders' bare FNV-1a keeps keys that differ only in high
// bits on one home slot at any table size below 2^(that bit), so such clusters, not load, grow it: 30,488 keys in
// 4,194,304 slots (160 MiB a generation) in ten-a; ten-final's doubling to 320 MiB ended it on the footprint cap.
bool MetalBindless::cluster_refused(const DescriptorKey& key) {
    static const std::uint64_t cap = [this] {
        const char* v = std::getenv("GTA6RT_METAL_TABLE_CLUSTER_CAP_MB");
        if (v && *v) return std::uint64_t(std::strtoull(v, nullptr, 10)) << 20;
        return table_.hash_kind() == DescriptorHashKind::fnv1a ? std::uint64_t(160) << 20 : 0;
    }();
    if (!cap || table_.size_bytes() < cap) return false;
    const std::size_t capacity = table_.capacity();
    if ((table_.size() + 1) * 8 > capacity) return false;
    const DescriptorHashKind kind = table_.hash_kind();
    const std::uint32_t mask = table_.mask();
    const std::uint32_t h = hash_descriptor_key(key, kind);
    std::uint32_t w[10];
    for (std::uint32_t k = 0; k < kDescriptorProbes; ++k) {
        if (!table_.read_slot((h + k) & mask, w)) return false;
        if (w[9] != kSlotOccupiedMarker || std::memcmp(w, key.data(), 32) == 0) return false;
    }
    for (std::uint32_t offset = kDescriptorProbes; offset < 2 * kDescriptorProbes - 1; ++offset) {
        const std::uint32_t hole = (h + offset) & mask;
        if (!table_.read_slot(hole, w)) return false;
        if (w[9] == kSlotOccupiedMarker) continue;
        for (std::uint32_t k = kDescriptorProbes; k > 0; --k) {
            if (!table_.read_slot((h + k - 1) & mask, w)) return false;
            if (((hole - hash_descriptor_key(w, kind)) & mask) < kDescriptorProbes) return false;
        }
        break;
    }
    const std::uint64_t n = ++cluster_refused_;
    if (n <= 16 || !(n & (n - 1)))
        std::fprintf(stderr, "[rt] d3d12-metal: descriptor hash table: key %08x %08x %08x %08x %08x %08x %08x %08x refused "
                             "(its 8 probe slots from %u are taken; %zu keys in %zu slots; %llu refused): it reads the "
                             "fallback\n",
                     key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7], h & mask, table_.size(), capacity,
                     static_cast<unsigned long long>(n));
    return true;
}

bool MetalBindless::insert_key(const DescriptorKey& key, std::uint32_t index) {
    std::lock_guard lock(mutex_);
    if (cluster_refused(key)) return false;
    const auto result = table_.insert_or_update(key.data(), index);
    table_dirty_ |= result != DescriptorTable::InsertResult::unchanged;
    return true;
}

namespace {
// GTA6RT_METAL_TABLE_TOMBSTONES (default on, 0: off): an erased key's slot holds a tombstone until the GPU passed the
// flush that emptied it, so no other key is written into it in place meanwhile (bindless::tombstone_key).
bool tombstones_on() {
    static const bool on = [] {
        const char* v = std::getenv("GTA6RT_METAL_TABLE_TOMBSTONES");
        return !v || std::strcmp(v, "0") != 0;
    }();
    return on;
}
}  // namespace

bool MetalBindless::erase_key(const DescriptorKey& key) {
    std::lock_guard lock(mutex_);
    std::uint32_t slot = UINT32_MAX;
    if (tombstones_on()) {
        const std::uint32_t mask = table_.mask();
        const std::uint32_t h = hash_descriptor_key(key, table_.hash_kind());
        std::uint32_t words[10];
        for (std::uint32_t k = 0; k < kDescriptorProbes && slot == UINT32_MAX; ++k)
            if (table_.read_slot((h + k) & mask, words) && words[9] == kSlotOccupiedMarker &&
                std::memcmp(words, key.data(), 32) == 0)
                slot = (h + k) & mask;
    }
    if (!table_.erase(key)) return false;
    table_dirty_ = true;
    if (slot != UINT32_MAX) {
        const std::uint64_t serial = ++tombstone_serial_;
        const DescriptorKey t = bindless::tombstone_key(slot, serial, table_.hash_kind());
        table_.insert_or_update(t.data(), 0);
        std::uint32_t words[10];
        if (table_.read_slot(slot, words) && std::memcmp(words, t.data(), 32) == 0) {
            tombstones_.emplace(serial, Tombstone{t, 0});
            ++tombstones_made_;
        } else {
            table_.erase(t);
            ++tombstones_refused_;
        }
    }
    return true;
}

void MetalBindless::release_tombstones(std::uint64_t first_unreleased, bool all) {
    for (auto it = tombstones_.begin(); it != tombstones_.end();) {
        if (!all && (!it->second.record || it->second.record >= first_unreleased)) {
            ++it;
            continue;
        }
        table_.erase(it->second.key);
        ++tombstones_released_;
        it = tombstones_.erase(it);
    }
}

void MetalBindless::date_tombstones(std::uint64_t record) {
    for (auto& [serial, t] : tombstones_)
        if (!t.record) t.record = record;
}

// A retired generation the queues have passed is freed at once (the generation path reused or freed one only when it
// next ran, so 2-3 retired 80 MiB generations stayed live between its runs). Every 100 ms at most.
void MetalBindless::free_settled_generations(std::chrono::steady_clock::time_point now) {
    if (now - last_free_scan_ < std::chrono::milliseconds(100)) return;
    last_free_scan_ = now;
    for (std::size_t i = 0; i < generations_.size();) {
        Generation& g = generations_[i];
        if (i == current_ || !generation_free(g, now)) {
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
    }
}

bool MetalBindless::generation_free(Generation& g, std::chrono::steady_clock::time_point now) {
    return g.retired && context_.marks().passed(g.retired_marks, g.retired_time, now, g.settled, g.settle_marks);
}

// The table's changes since the last take, with any an in-place attempt took and left to the generation path.
DescriptorTable::Changes MetalBindless::take_all_changes() {
    DescriptorTable::Changes changes = table_.take_changes();
    if (carried_.all || !carried_.slots.empty()) {
        changes.all |= carried_.all;
        changes.slots.insert(changes.slots.begin(), carried_.slots.begin(), carried_.slots.end());
        carried_ = {};
    }
    return changes;
}

// Records of in-place flushes that emptied slots: released once the queues passed what they had submitted then and,
// 100 ms later, again (as generation_free). The first record not released yet.
std::uint64_t MetalBindless::release_empty_records(std::chrono::steady_clock::time_point now) {
    while (!empty_records_.empty()) {
        EmptyRecord& r = empty_records_.front();
        if (!context_.marks().passed(r.marks, r.time, now, r.settled, r.settle_marks)) break;
        empty_records_.pop_front();
    }
    const std::uint64_t first = empty_records_.empty() ? next_record_ : empty_records_.front().id;
    refills_.prune(first);
    return first;
}

void MetalBindless::report_in_place(std::chrono::steady_clock::time_point now) {
    if (now - last_report_ < std::chrono::seconds(10)) return;
    last_report_ = now;
    // GTA6RT_METAL_TABLE_DUMP=path (diagnostics, off by default): every 10 s the table's keys and indices (9 words each,
    // tombstones left out) to that file, for offline sizing.
    static const char* dump = std::getenv("GTA6RT_METAL_TABLE_DUMP");
    if (dump && *dump) {
        const std::vector<std::uint32_t> keys = table_.export_keys();
        const std::string tmp = std::string(dump) + ".tmp";
        if (std::FILE* f = std::fopen(tmp.c_str(), "wb")) {
            for (std::size_t i = 0; i + 9 <= keys.size(); i += 9)
                if (!bindless::is_tombstone(&keys[i])) std::fwrite(&keys[i], 4, 9, f);
            std::fclose(f);
            std::rename(tmp.c_str(), dump);
        }
    }
    std::fprintf(stderr, "[rt] d3d12-metal: descriptor hash table: %llu flushes in place, %llu to a generation (%llu "
                         "rekeyed, %llu refills too soon), %zu live generations (%llu MiB), %zu slots emptied in flight; "
                         "tombstones %zu live, %llu made, %llu released, %llu misplaced; %zu keys in %zu slots\n",
                 static_cast<unsigned long long>(in_place_flushes_),
                 static_cast<unsigned long long>(serial_),
                 static_cast<unsigned long long>(in_place_refused_rekey_),
                 static_cast<unsigned long long>(in_place_refused_refill_), generations_.size(),
                 static_cast<unsigned long long>(generation_bytes_ >> 20), refills_.size(), tombstones_.size(),
                 static_cast<unsigned long long>(tombstones_made_), static_cast<unsigned long long>(tombstones_released_),
                 static_cast<unsigned long long>(tombstones_refused_), table_.size(), table_.capacity());
}

bool MetalBindless::flush() {
    std::unique_lock lock(mutex_);
    free_settled_generations(std::chrono::steady_clock::now());
    if (!table_dirty_ && current_ != SIZE_MAX) return true;
    // GTA6RT_METAL_TABLE_IN_PLACE (default on, 0: off): while the table keeps its size, changed slots are written into
    // the generation the GPU reads when bindless::write_in_place finds every change safe for a racing reader; else
    // the generation path below takes the same changes.
    static const bool in_place = [] {
        const char* v = std::getenv("GTA6RT_METAL_TABLE_IN_PLACE");
        return !v || std::strcmp(v, "0") != 0;
    }();
    if (in_place && current_ != SIZE_MAX) {
        Generation& g = generations_[current_];
        auto* gpu_words = static_cast<std::uint32_t*>(g.buffer->cpu_pointer());
        if (gpu_words && g.written && g.epoch == table_epoch_ && g.synced == journal_seq_ &&
            g.buffer->size() >= table_.size_bytes()) {
            const auto now = std::chrono::steady_clock::now();
            const std::uint64_t first = release_empty_records(now);
            release_tombstones(first, false);
            DescriptorTable::Changes changes = take_all_changes();
            if (!changes.all) {
                const bindless::InPlaceResult r =
                    bindless::write_in_place(gpu_words, table_, changes.slots, refills_, next_record_, first);
                if (r.written) {
                    const bool undated = std::any_of(tombstones_.begin(), tombstones_.end(),
                                                     [](const auto& t) { return !t.second.record; });
                    if (r.emptied || undated) {
                        date_tombstones(next_record_);
                        empty_records_.push_back({next_record_++, now, context_.submitted()});
                    }
                    journal_.insert(journal_.end(), changes.slots.begin(), changes.slots.end());
                    journal_seq_ += changes.slots.size();
                    g.synced = journal_seq_;
                    // Only a generation that may be reused reads the journal; with the current one alone it is empty.
                    if (generations_.size() == 1) {
                        journal_.clear();
                        journal_base_ = journal_seq_;
                    }
                    slot_writes_ += changes.slots.size();
                    ++in_place_flushes_;
                    table_dirty_ = false;
                    report_in_place(now);
                    return true;
                }
                ++(r.rekeyed ? in_place_refused_rekey_ : in_place_refused_refill_);
            }
            carried_ = std::move(changes);
        }
    }
    std::size_t bytes = table_.size_bytes();
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
            if (g.buffer && g.buffer->size() >= bytes && (pick == SIZE_MAX || !in_place)) {
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
        bytes = table_.size_bytes();
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
        refills_.clear();
        empty_records_.clear();
        release_tombstones(0, true);
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
    DescriptorTable::Changes changes = take_all_changes();
    if (changes.all) {
        ++table_epoch_;
        journal_.clear();
        journal_base_ = journal_seq_;
    } else {
        journal_.insert(journal_.end(), changes.slots.begin(), changes.slots.end());
        journal_seq_ += changes.slots.size();
    }
    Generation& g = generations_[pick];
    if (g.buffer->size() < table_.size_bytes()) {
        std::fprintf(stderr, "[rt] d3d12-metal: descriptor hash table: generation of %llu bytes picked for a table of "
                             "%zu bytes; not written\n",
                     static_cast<unsigned long long>(g.buffer->size()), table_.size_bytes());
        return false;
    }
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
    if (in_place) report_in_place(now);
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
    s.in_place_flushes = in_place_flushes_;
    s.in_place_refused_rekey = in_place_refused_rekey_;
    s.in_place_refused_refill = in_place_refused_refill_;
    s.tombstones_live = tombstones_.size();
    s.tombstones_made = tombstones_made_;
    return s;
}

}  // namespace leonida
