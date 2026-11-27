#pragma once

// The GPU side of leonida's D3D12 front end on gta6rt::gpu (port/metal): what VulkanContext, VulkanQueue and
// VulkanFence are on the Vulkan path (d3d12_vulkan/src/vulkan_backend.h), for the cut ADAPTER.md section 1 draws.
// Resources by ResourceId, pipelines by id, query heaps by id, the 29 bindless tables, the cross-queue hold model
// (queue_holds.h, unchanged) and the replay of recorded command lists (metal_replay.cpp).

#include "command_list.h"
#include "indirect_draw_stats.h"
#include "local_diagnostics.h"
#include "pipeline_workers.h"
#include "queue_holds.h"
#include "resource_tiling.h"
#include "submit_phases.h"
#include "title_va_alias.h"
#include "uav_clear.h"
#include "vk_run_counters.h"

#include <gta6rt_metal/gpu.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace leonida {

namespace gpu = gta6rt::gpu;

enum class ReplayResult {
    success,
    list_open,
    unresolved_resource,
    invalid_subresource,
    invalid_clear,
    invalid_copy,
    invalid_predication,
    invalid_query,
    invalid_indirect_arguments,
    invalid_draw,
    unsupported
};

namespace bindless {
inline constexpr std::uint32_t binding_count = 29;
inline constexpr std::uint32_t table_binding = 2;
inline constexpr std::uint32_t sampler_binding = 1;
}  // namespace bindless

// Copied from vulkan_backend.h (neutral): tile ranges d3d12_com.cpp's CopyTiles recording builds.
struct TileRegion {
    std::uint64_t first = 0;
    std::uint64_t count = 0;
};

// What replay needs to know of a resource (vulkan_replay.h ResourceBinding without Vulkan handles).
struct MetalBinding {
    gpu::Buffer* buffer = nullptr;       // a buffer, or the arena for a buffer placed in it
    std::uint64_t buffer_offset = 0;     // where the resource starts in `buffer`
    std::uint64_t size = 0;
    std::uint64_t va = 0;                // console VA (GetGPUVirtualAddress)
    const std::uint8_t* host = nullptr;  // CPU-written bytes (upload heaps), for host-side reads
    gpu::Texture* texture = nullptr;
    std::uint32_t format = 0;            // DXGI format of the resource
    std::uint32_t width = 1, height = 1, depth = 1, mip_levels = 1, array_layers = 1, samples = 1;
    bool volume = false;
    bool depth_stencil = false;
    bool stencil = false;
    bool depth24 = false;
    bool presentable = false;
    std::uint32_t block_bytes = 0, block_dim = 1;  // texel or 4x4-block size, for footprint copies
    // A texture whose Metal texture is made at its first use (a deferred title texture): ensure(owner) makes it and
    // registers the binding again; false when it cannot be made.
    void* owner = nullptr;
    bool (*ensure)(void*) = nullptr;
};

// A pipeline whose Metal object is made at first use: `ensure(owner)` (the PSO) makes it and registers it again
// with the pipeline set; false while it cannot be drawn with yet.
struct MetalComputePipeline {
    gpu::ComputePipeline* pipeline = nullptr;
    std::uint64_t code_base = 0;
    const char* name = "";
    void* owner = nullptr;
    bool (*ensure)(void*) = nullptr;
    bool refused = false;
};

struct MetalGraphicsPipeline {
    gpu::RenderPipeline* render = nullptr;
    gpu::MeshPipeline* mesh = nullptr;
    bool split = false;  // mesh: the split64 route (object stage + 64-vertex-chunk mesh stage)
    // The rectangle-list variant (topology type rectangle, vertex-stage expansion) of a triangle PSO the title draws
    // rect lists with: console PSOs say TRIANGLE for those. Built at the first rect-list draw (ensure_rect).
    gpu::RenderPipeline* rect = nullptr;
    bool (*ensure_rect)(void*) = nullptr;
    std::uint64_t code_base = 0;
    std::uint32_t topology_type = 0;  // D3D12_PRIMITIVE_TOPOLOGY_TYPE; 19 rectangle, 32 quad (console)
    const char* name = "";
    void* owner = nullptr;
    bool (*ensure)(void*) = nullptr;
    bool refused = false;
};

struct QueryHeapEntry {
    gpu::QueryHeap* heap = nullptr;
    std::uint32_t d3d12_type = 0;  // D3D12_QUERY_HEAP_TYPE
    std::uint32_t count = 0;
};

class MetalQueue;

class MetalContext {
public:
    using PinFn = std::function<std::shared_ptr<void>()>;

    explicit MetalContext(gpu::Device& device);
    ~MetalContext();
    MetalContext(const MetalContext&) = delete;
    MetalContext& operator=(const MetalContext&) = delete;

    gpu::Device& gpu() { return device_; }
    QueueHolds<gpu::Fence*>& holds() { return holds_; }

    void add_resource(ResourceId id, const MetalBinding& binding);
    void remove_resource(ResourceId id);
    bool binding(ResourceId id, MetalBinding& out) const;
    bool has_resource(ResourceId id) const;
    // A console VA to the buffer holding it: the resource that covers [va, va + bytes) (bytes cut at its end).
    bool buffer_at(std::uint64_t va, std::uint64_t& bytes, gpu::Buffer*& buffer, std::uint64_t& offset) const;
    bool host_at(std::uint64_t va, std::uint64_t bytes, const std::uint8_t*& host) const;
    // A resource (not the arena fallback) covers all of [va, va + bytes).
    bool knows_buffer_va(std::uint64_t va, std::uint64_t bytes = 1) const;
    bool buffer_resource_at(std::uint64_t va, std::uint64_t bytes, ResourceId& id, std::uint64_t& start,
                            std::uint64_t& size, const std::function<bool(ResourceId)>& accept) const;
    // Extra VA ranges replay must resolve that are no D3D12 resource: title windows and host buffers.
    void add_va_range(std::uint64_t va, std::uint64_t bytes, gpu::Buffer* buffer, std::uint64_t offset);
    void remove_va_range(std::uint64_t va, gpu::Buffer* buffer);

    void add_pipeline(std::uintptr_t id, const MetalComputePipeline& pipeline);
    void add_graphics_pipeline(std::uintptr_t id, const MetalGraphicsPipeline& pipeline);
    void remove_pipeline(std::uintptr_t id);
    bool compute_pipeline(std::uintptr_t id, MetalComputePipeline& out) const;
    bool graphics_pipeline(std::uintptr_t id, MetalGraphicsPipeline& out) const;

    void add_query_heap(QueryHeapId id, const QueryHeapEntry& entry);
    void remove_query_heap(QueryHeapId id);
    bool query_heap(QueryHeapId id, QueryHeapEntry& out) const;
    std::uint32_t query_heap_type(QueryHeapId id) const;

    // The contract every translated shader shares (ADAPTER.md section 4): one root layout, heap_base, 29 tables.
    void set_root_layout(const gpu::RootLayout* layout) { root_ = layout; }
    const gpu::RootLayout* root_layout() const { return root_; }
    void set_heap_base(std::uint64_t heap_base, std::uint64_t arena_va, std::uint64_t arena_size) {
        heap_base_ = heap_base;
        arena_va_ = arena_va;
        arena_size_ = arena_size;
    }
    std::uint64_t heap_base() const { return heap_base_; }
    // The arena's one buffer: a console VA below arena_size that no resource covers is that offset in it.
    void set_arena(gpu::Buffer* buffer, std::uint64_t size) {
        arena_buffer_ = buffer;
        arena_size_ = size;
    }
    gpu::Buffer* arena_buffer() const { return arena_buffer_; }
    std::uint64_t arena_va() const { return arena_va_; }
    void set_table(std::uint32_t binding, gpu::DescriptorHeap* table) { tables_[binding] = table; }
    gpu::DescriptorHeap* table(std::uint32_t binding) const { return tables_[binding]; }

    // Hooks the device installs, as on the Vulkan path (VulkanContext::set_before_execute and the others).
    void set_before_execute(std::function<void()> fn) { before_execute_ = std::move(fn); }
    void run_before_execute() const {
        if (before_execute_) before_execute_();
    }
    using TableScanner = std::function<bool(std::uint64_t, std::uint32_t)>;
    void set_table_scanner(TableScanner scan) { table_scanner_ = std::move(scan); }
    bool scan_table(std::uint64_t va, std::uint32_t descriptors) const {
        return table_scanner_ && table_scanner_(va, descriptors);
    }
    void set_sample_table_resolver(std::function<bool(std::uint64_t, std::uint32_t)> f) { sample_table_ = std::move(f); }
    bool prepare_sample_table(std::uint64_t address, std::uint32_t count) const {
        return !sample_table_ || sample_table_(address, count);
    }
    void set_cpu_texture_publisher(std::function<bool()> f) { cpu_texture_publisher_ = std::move(f); }
    bool publish_cpu_textures() const { return !cpu_texture_publisher_ || cpu_texture_publisher_(); }
    void set_bindless_prepare(std::function<bool()> f) { bindless_prepare_ = std::move(f); }
    bool bindless_prepare() const { return !bindless_prepare_ || bindless_prepare_(); }
    // Resolves a console VA a list names for a GPU write (WriteBufferImmediate, labels) to a buffer and offset.
    void set_write_resolver(std::function<bool(std::uint64_t, gpu::Buffer*&, std::uint64_t&)> f) {
        write_resolver_ = std::move(f);
    }
    bool resolve_write(std::uint64_t va, gpu::Buffer*& buffer, std::uint64_t& offset) const;

    // Shared 1x1 fallbacks the replay binds where a list names nothing (unbound slots stay unbound).
    void set_zero_buffer(gpu::Buffer* buffer) { zero_buffer_ = buffer; }
    gpu::Buffer* zero_buffer() const { return zero_buffer_; }

    void set_pipeline_workers(std::unique_ptr<PipelineWorkers> workers) { workers_ = std::move(workers); }
    PipelineWorkers* pipeline_workers() { return workers_.get(); }

    void mark_device_lost(const char* where);
    bool is_device_lost() const { return device_lost_.load(std::memory_order_acquire); }
    // Called once, by the first mark_device_lost (the device ends the run there: Metal runs nothing of a process that
    // caused a GPU timeout).
    void set_on_device_lost(std::function<void(const char*)> f) { on_lost_ = std::move(f); }

    // Counters for the run report and STATUS.md measurements.
    struct Stats {
        std::atomic<std::uint64_t> executes{0}, lists{0}, commands{0}, draws{0}, draws_skipped{0}, dispatches{0},
            dispatches_skipped{0}, copies{0}, clears{0}, barriers{0}, unsupported{0}, replay_us{0}, execute_us{0},
            before_us{0}, holds{0}, presents{0}, composed_presents{0}, deferred_backed{0}, format_copies{0};
    };
    Stats& stats() { return stats_; }
    void note_unsupported(const char* what);
    // GTA6RT_METAL_DRAW_CENSUS=1: draws and dispatches by pipeline (and skipped ones by why), taken and printed by
    // the device's guard thread; off by default (a lock per draw).
    static bool census_on();
    void note_census(const char* pipeline, const char* what);
    std::string take_census();

    // Every draw and dispatch replay sees, by class and by what became of it (atomics; always on). `why` names the
    // refusal of a skipped one (the refused PSO's reason, Metal's error), kept by count.
    enum class Work : std::uint8_t {
        draw, draw_indexed, draw_rect, mesh, mesh_split, indirect_draw, indirect_draw_indexed, indirect_rect,
        indirect_mesh, indirect_mesh_split, dispatch, indirect_dispatch, indirect_uncaptured, count
    };
    enum class Fate : std::uint8_t {
        recorded, pso_refused, pso_unknown, library_pending, compiling, metal_refused, target_unknown,
        vertex_unknown, index_unknown, arguments_unknown, not_implemented, producer_not_ready, count
    };
    // A dispatch skipped because its pipeline or library is not ready yet: GPU-driven indirect work in the next
    // GTA6RT_METAL_INDIRECT_GUARD_MS (default 2000, 0: off) is skipped too, since its records may come from that
    // dispatch and were never written (garbage grids: GPU timeouts, d3m-s4-g/-h).
    void note_compute_not_ready();
    bool producers_settling() const;
    static const char* work_name(Work w);
    static const char* fate_name(Fate f);
    void note_work(Work w, Fate f, const char* why = nullptr);
    // Counts since the last call, per presented frame, and the top skip reasons; empty when nothing was seen.
    std::string take_work_report(std::uint64_t presents);

    // The latest progress value each queue signalled, so a present (on its own queue) waits for the work before it.
    void note_submitted(gpu::Fence* progress, std::uint64_t value);
    void forget_queue(gpu::Fence* progress);
    std::vector<std::pair<gpu::Fence*, std::uint64_t>> submitted() const;
    // Every queue of `marks` that still exists has completed its value (a queue that went counts as done).
    bool reached(const std::vector<std::pair<gpu::Fence*, std::uint64_t>>& marks) const;

private:
    friend class MetalQueue;
    struct VaRange {
        std::uint64_t end;
        gpu::Buffer* buffer;
        std::uint64_t offset;
        ResourceId id;
    };

    gpu::Device& device_;
    QueueHolds<gpu::Fence*> holds_;
    mutable std::shared_mutex resource_mutex_;
    std::unordered_map<ResourceId, MetalBinding> resources_;
    std::multimap<std::uint64_t, VaRange> vas_;
    std::uint64_t max_range_ = 0;
    mutable std::shared_mutex pipeline_mutex_;
    std::unordered_map<std::uintptr_t, MetalComputePipeline> compute_;
    std::unordered_map<std::uintptr_t, MetalGraphicsPipeline> graphics_;
    std::unordered_map<QueryHeapId, QueryHeapEntry> queries_;
    const gpu::RootLayout* root_ = nullptr;
    std::uint64_t heap_base_ = 0, arena_va_ = 0, arena_size_ = 0;
    gpu::Buffer* arena_buffer_ = nullptr;
    std::array<gpu::DescriptorHeap*, bindless::binding_count> tables_{};
    std::function<void()> before_execute_;
    TableScanner table_scanner_;
    std::function<bool(std::uint64_t, std::uint32_t)> sample_table_;
    std::function<bool()> cpu_texture_publisher_;
    std::function<bool()> bindless_prepare_;
    std::function<bool(std::uint64_t, gpu::Buffer*&, std::uint64_t&)> write_resolver_;
    gpu::Buffer* zero_buffer_ = nullptr;
    std::unique_ptr<PipelineWorkers> workers_;
    std::atomic<bool> device_lost_{false};
    std::function<void(const char*)> on_lost_;
    Stats stats_;
    std::mutex unsupported_mutex_;
    std::map<std::string, std::uint64_t> unsupported_;
    std::mutex census_mutex_;
    std::map<std::pair<std::string, std::string>, std::uint64_t> census_;
    static constexpr std::size_t kWork = std::size_t(Work::count) * std::size_t(Fate::count);
    std::array<std::atomic<std::uint64_t>, kWork> work_{};
    std::array<std::uint64_t, kWork> work_seen_{};
    std::mutex work_mutex_;
    std::map<std::string, std::uint64_t> work_why_;
    std::atomic<std::int64_t> last_compute_skip_ns_{0};
    mutable std::mutex submitted_mutex_;
    std::map<gpu::Fence*, std::uint64_t> submitted_;
};

// Replays one closed IR list into `out` (ADAPTER.md section 4). Commands the backend cannot do yet are counted and
// named once (MetalContext::note_unsupported); a draw or dispatch whose pipeline is not ready is skipped by the
// backend (NotReady::skip) and counted there.
// `labels`: the GPU-label writes of the list (LEONIDA_GPU_LABELS), stored by the CPU once the submission completes.
// `temps`: buffers the replay made for this submission (a copy between two texture formats goes through one); the
// caller keeps them until the lists are executed (the backend holds their release until the GPU is done).
ReplayResult metal_replay(const CommandList& list, gpu::CommandList& out, MetalContext& context, gpu::QueueType queue,
                          std::vector<label::Write>* labels = nullptr,
                          std::vector<std::unique_ptr<gpu::Buffer>>* temps = nullptr);

// A D3D12 queue: QueueHolds holder over a gpu::Queue (the Vulkan path's VulkanQueue minus layouts).
class MetalQueue : private QueueHolds<gpu::Fence*>::Holder {
public:
    using PinFn = MetalContext::PinFn;
    MetalQueue(MetalContext& context, gpu::QueueType type, const char* label);
    ~MetalQueue();
    MetalQueue(const MetalQueue&) = delete;
    MetalQueue& operator=(const MetalQueue&) = delete;

    bool ok() const { return queue_ != nullptr; }
    gpu::Queue* queue() { return queue_.get(); }
    gpu::QueueType type() const { return type_; }

    ReplayResult execute(std::span<const CommandList* const> lists, const PinFn& pin = {});
    bool signal(gpu::Fence* fence, std::uint64_t value, const PinFn& pin = {});
    void wait(gpu::Fence* fence, std::uint64_t value, const PinFn& pin = {});
    void marker(DebugMarker::Kind kind, const char* label);
    ReplayResult take_held_failure() {
        return static_cast<ReplayResult>(held_failure_.exchange(static_cast<int>(ReplayResult::success)));
    }
    bool idle(std::uint64_t timeout_ns = 10'000'000'000ull);
    std::uint64_t completed() const;

private:
    struct Deferred {
        enum class Kind : std::uint8_t { batch, signal, marker } kind = Kind::batch;
        std::uint64_t seq = 0;
        std::vector<CommandList> lists;
        gpu::Fence* fence = nullptr;
        std::uint64_t value = 0;
        DebugMarker::Kind marker_kind = DebugMarker::Kind::set;
        std::string label;
        std::vector<gpu::Fence*> waits;
        std::vector<std::uint64_t> wait_values;
        std::vector<std::shared_ptr<void>> wait_pins, pins;
    };
    struct Pinned {
        std::uint64_t ticket;
        std::shared_ptr<void> pin;
    };

    ReplayResult execute_now(std::span<const CommandList* const> lists);
    bool submit_signal(gpu::Fence* fence, std::uint64_t value);
    void consume_waits();
    bool must_hold() const;
    bool waits_pending(const std::vector<gpu::Fence*>& fences, const std::vector<std::uint64_t>& values) const;
    void hold(Deferred&& op);
    void run(Deferred& op);
    void retire_pins(std::vector<std::shared_ptr<void>>& pins, std::uint64_t ticket);
    void drain_pins(std::vector<std::shared_ptr<void>>& out);
    std::uint64_t mark_progress();
    gpu::CommandList* list_slot(std::size_t index);

    bool release_ready() override;
    void discard_waiting_on(gpu::Fence* fence) override;
    std::uint64_t oldest_held_seq() override;

    MetalContext& context_;
    gpu::QueueType type_;
    std::unique_ptr<gpu::Queue> queue_;
    std::unique_ptr<gpu::Fence> progress_;
    std::uint64_t submitted_ = 0;
    std::mutex state_mutex_;
    std::deque<Deferred> held_;
    std::vector<gpu::Fence*> waits_;
    std::vector<std::uint64_t> wait_values_;
    std::vector<std::shared_ptr<void>> wait_pins_;
    std::deque<Pinned> pin_queue_;
    std::vector<std::shared_ptr<void>> retired_pins_;
    std::vector<std::unique_ptr<gpu::CommandList>> lists_;
    std::vector<std::string> open_events_;
    std::atomic<int> held_failure_{static_cast<int>(ReplayResult::success)};
};

}  // namespace leonida
