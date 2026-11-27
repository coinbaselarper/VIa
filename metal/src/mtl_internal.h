#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gta6rt_metal/gpu.h"
#include "gta6rt_metal/metal_backend.h"
#include "gta6rt_metal/rect_list.h"
#include "msl_rewrite.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace gta6rt::metal {

using namespace gta6rt::gpu;

class MetalDevice;
class MetalBuffer;
class MetalTexture;

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

struct NativeFormat {
    MTLPixelFormat pixel = MTLPixelFormatInvalid;
    FormatInfo info;
    bool swizzled = false;  // sampled through a swizzle view (B4G4R4A4 stored as ABGR4Unorm)
    MTLTextureSwizzleChannels swizzle = MTLTextureSwizzleChannelsDefault;
};
bool native_format(std::uint32_t dxgi, NativeFormat& out);
MTLVertexFormat vertex_format(std::uint32_t dxgi);
float quantize_d24(float depth);

// Queue timelines and the values they must reach (MetalDevice::retire).
using RetireMarks = std::vector<std::pair<id<MTLSharedEvent>, std::uint64_t>>;

struct AtomicStats {
    std::atomic<std::uint64_t> pipelines_created{0}, pipelines_compiled{0}, render_encoders{0}, blit_encoders{0},
        compute_encoders{0}, barriers_noop{0}, barriers_fenced{0}, clears_folded{0}, command_buffers{0},
        value_writes{0}, value_waits{0}, rect_draws_expanded{0}, stores_discarded{0}, fence_joins{0},
        target_joins{0}, residency_commits{0};
    std::atomic<std::uint64_t> dispatches{0}, dispatches_thread_counts{0}, indirect_commands{0}, indirect_prepasses{0},
        prepasses_hoisted{0}, render_pass_splits{0}, barriers_in_encoder{0}, mesh_draws{0}, quad_draws{0},
        queries_occlusion{0}, timestamps{0}, draws_skipped_not_ready{0}, d24_fragment_pipelines{0}, archive_hits{0},
        archive_misses{0}, pipelines_refused_limit{0}, presents{0}, present_planes{0};
    std::atomic<std::uint64_t> command_buffer_errors{0}, command_buffer_victims{0}, event_signals_repaired{0};
    std::atomic<std::uint64_t> releases_deferred{0}, releases_immediate{0}, releases_waiting{0},
        releases_waiting_bytes{0}, releases_waiting_max{0};
};

struct QueueErrors {
    std::atomic<std::uint64_t> unreported{0};
    std::atomic<bool> any_failed{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t completed = 0;  // command buffers whose completed handler ran
    std::uint64_t logged = 0;
    std::uint64_t slow_logged = 0;
    std::string last;
};

using EventSignal = std::pair<id<MTLSharedEvent>, std::uint64_t>;

// Per-category accounting of what the backend allocates (DESIGN.md 8).
class MemoryLedger {
public:
    void add(MemoryCategory c, std::uint64_t bytes) {
        bytes_[std::size_t(c)] += bytes;
        objects_[std::size_t(c)] += 1;
    }
    void remove(MemoryCategory c, std::uint64_t bytes) {
        bytes_[std::size_t(c)] -= bytes;
        objects_[std::size_t(c)] -= 1;
    }
    std::uint64_t bytes(MemoryCategory c) const { return bytes_[std::size_t(c)].load(); }
    std::uint64_t objects(MemoryCategory c) const { return objects_[std::size_t(c)].load(); }

private:
    std::array<std::atomic<std::uint64_t>, std::size_t(MemoryCategory::count)> bytes_{}, objects_{};
};

class VaSpace {
public:
    void init(std::uint64_t base, std::uint64_t size) { next_ = base; end_ = base + size; }
    std::uint64_t allocate(std::uint64_t size, std::uint64_t align);
    void add(std::uint64_t va, std::uint64_t size, MetalBuffer* buffer);
    void remove(std::uint64_t va, MetalBuffer* buffer);
    bool resolve(std::uint64_t va, MetalBuffer** buffer, std::uint64_t* offset) const;

private:
    struct Entry {
        std::uint64_t size;
        MetalBuffer* buffer;
    };
    mutable std::mutex mutex_;
    std::multimap<std::uint64_t, Entry> ranges_;
    std::uint64_t next_ = 0, end_ = 0;
};

class MetalBuffer final : public gpu::Buffer {
public:
    MetalBuffer(MetalDevice* device, id<MTLBuffer> buffer, std::uint64_t va, std::uint64_t size, bool placed,
                bool tracked, MemoryCategory category = MemoryCategory::buffer);
    ~MetalBuffer() override;
    std::uint64_t gpu_va() const override { return va_; }
    std::uint64_t host_gpu_address() const override { return buffer_.gpuAddress; }
    void* cpu_pointer() override;
    std::uint64_t size() const override { return size_; }
    bool placed() const override { return placed_; }
    id<MTLBuffer> mtl() const { return buffer_; }
    bool tracked() const { return tracked_; }

private:
    MetalDevice* device_;
    id<MTLBuffer> buffer_;
    std::uint64_t va_, size_;
    bool placed_, tracked_;
    MemoryCategory category_;
    std::uint64_t accounted_ = 0;
};

class MetalTexture final : public gpu::Texture {
public:
    MetalTexture(MetalDevice* device, id<MTLTexture> texture, const TextureDesc& desc, const NativeFormat& format,
                 bool placed, MemoryCategory category = MemoryCategory::texture);
    ~MetalTexture() override;
    std::uint64_t gpu_va() const override { return 0; }
    std::uint64_t host_gpu_address() const override { return 0; }
    void* cpu_pointer() override { return nullptr; }
    std::uint64_t size() const override { return texture_.allocatedSize; }
    bool placed() const override { return placed_; }
    const TextureDesc& desc() const override { return desc_; }
    std::uint32_t native_format() const override { return std::uint32_t(format_.pixel); }
    Status write_region(std::uint32_t mip, std::uint32_t layer, std::uint32_t x, std::uint32_t y, std::uint32_t width,
                        std::uint32_t height, const void* data, std::uint32_t row_pitch) override;
    id<MTLTexture> mtl() const { return texture_; }
    id<MTLTexture> sample_view() const { return view_ ? view_ : texture_; }
    // v3: a view made by create_texture_view: the texture whose memory it shares (residency is the parent's).
    void set_parent(id<MTLTexture> parent) { parent_ = parent; }
    id<MTLTexture> storage() const { return parent_ ? parent_ : texture_; }
    const NativeFormat& format() const { return format_; }
    bool tracked() const { return !placed_; }

private:
    MetalDevice* device_;
    id<MTLTexture> texture_;
    id<MTLTexture> view_ = nil;
    id<MTLTexture> parent_ = nil;
    TextureDesc desc_;
    NativeFormat format_;
    bool placed_;
    MemoryCategory category_;
    std::uint64_t accounted_ = 0;
};

class MetalHeap final : public gpu::Heap {
public:
    MetalHeap(MetalDevice* device, id<MTLHeap> heap, std::uint64_t va_base, std::unique_ptr<MetalBuffer> arena,
              MemoryKind memory);
    ~MetalHeap() override;
    std::uint64_t size() const override { return heap_.size; }
    std::uint64_t va_base() const override { return va_base_; }
    std::uint64_t host_gpu_address() const override { return arena_ ? arena_->host_gpu_address() : 0; }
    gpu::Buffer* arena() override { return arena_.get(); }
    id<MTLHeap> mtl() const { return heap_; }
    MemoryKind memory() const { return memory_; }

private:
    MetalDevice* device_;
    id<MTLHeap> heap_;
    std::uint64_t va_base_;
    std::unique_ptr<MetalBuffer> arena_;
    MemoryKind memory_;
    std::uint64_t accounted_ = 0;
};

class MetalSampler final : public gpu::Sampler {
public:
    MetalSampler(id<MTLSamplerState> state, bool exact) : state_(state), exact_(exact) {}
    bool border_exact() const override { return exact_; }
    id<MTLSamplerState> mtl() const { return state_; }

private:
    id<MTLSamplerState> state_;
    bool exact_;
};

class MetalDescriptorHeap final : public gpu::DescriptorHeap {
public:
    MetalDescriptorHeap(MetalDevice* device, const DescriptorHeapDesc& desc, id<MTLBuffer> buffer,
                        id<MTLResidencySet> residency);
    ~MetalDescriptorHeap() override;
    std::uint32_t count() const override { return count_; }
    std::uint32_t stride() const override { return 8; }
    DescriptorKind kind() const override { return kind_; }
    Status write_texture(std::uint32_t slot, gpu::Texture* texture) override;
    Status write_sampler(std::uint32_t slot, gpu::Sampler* sampler) override;
    Status write_buffer(std::uint32_t slot, gpu::Buffer* buffer, std::uint64_t offset) override;
    Status clear(std::uint32_t slot) override;
    Status copy(std::uint32_t dst_slot, const gpu::DescriptorHeap& src, std::uint32_t src_slot, std::uint32_t n) override;
    std::uint64_t read_raw(std::uint32_t slot) const override;

    id<MTLBuffer> mtl() const { return buffer_; }
    id<MTLResidencySet> residency_for_submit();

private:
    void set_slot(std::uint32_t slot, std::uint64_t raw, id object, id<MTLAllocation> allocation);
    MetalDevice* device_;
    DescriptorKind kind_;
    std::uint32_t count_;
    id<MTLBuffer> buffer_;
    id<MTLResidencySet> residency_;
    mutable std::mutex mutex_;
    std::vector<id> kept_;
    std::vector<id<MTLAllocation>> slot_allocation_;
    std::unordered_map<void*, std::uint32_t> allocation_refs_;
    struct PendingRemoval {
        id<MTLAllocation> allocation;
        RetireMarks marks;
    };
    std::deque<PendingRemoval> pending_removals_;
    bool dirty_ = false;
    std::uint64_t accounted_ = 0;
};

class MetalRootLayout final : public gpu::RootLayout {
public:
    explicit MetalRootLayout(const RootLayoutDesc& desc) : desc_(desc) {}
    const RootLayoutDesc& desc() const override { return desc_; }

private:
    RootLayoutDesc desc_;
};

class MetalLibrary final : public gpu::ShaderLibrary {
public:
    MetalLibrary(MetalDevice* device, id<MTLLibrary> library, std::string source, double seconds, MslOptions options);
    std::vector<std::string> function_names() const override;
    const std::string& source() const override { return source_; }
    double compile_seconds() const override { return seconds_; }
    // Compiles a deferred library (once, on whichever thread asks first); nil and an error if it fails.
    id<MTLLibrary> mtl(std::string* error = nullptr);
    bool deferred_pending() const { return !library_ && error_.empty(); }
    // Size of what the device compiler is given (metallib bytes, else MSL text): the compile queue's cost estimate.
    void set_bytes(std::size_t n) { bytes_ = n; }
    std::size_t bytes() const { return bytes_ ? bytes_ : source_.size(); }

private:
    std::size_t bytes_ = 0;
    MetalDevice* device_;
    std::mutex mutex_;
    id<MTLLibrary> library_;
    std::string source_;
    double seconds_;
    MslOptions options_;
    std::string error_;
};

struct StagePlan {
    int push_index = -1;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tables;  // (contract binding, buffer index)
};

// Compile state shared by render, compute and mesh pipelines. Whoever claims a pipeline builds it: a compile
// worker that pops it from the queue, or a caller of wait() (lazy, or taken off the queue). DESIGN.md 3.7.
class PipelineCore {
public:
    PipelineCore(MetalDevice* device, CompileMode mode, NotReady not_ready, Priority priority);
    virtual ~PipelineCore() = default;
    void start();
    Status core_wait();
    // Encoding: ready -> true; failed -> false; not ready -> wait or skip (queueing a lazy one).
    bool ready_for_encode(Status* why);
    PipelineState core_state() const;
    Status core_status() const { return status_.load(); }
    bool claim() { bool f = false; return claimed_.compare_exchange_strong(f, true); }
    bool claimed_ref() const { return claimed_.load(); }
    void run_build();
    void detach_from_queue();  // destructors of derived classes call this first
    Priority priority() const { return priority_; }
    std::atomic<bool> queued{false};
    std::size_t cost_bytes = 0;  // library bytes of its stages (CompileQueue: large builds are capped)
    std::string label_text;  // the description's label (Metal pipeline states carry none): encoder labels name it

protected:
    virtual void build() = 0;
    void finish_ok(double seconds);
    void finish_fail(Status status, const std::string& error, double seconds);
    MetalDevice* device_;
    CompileMode mode_;
    NotReady not_ready_;
    Priority priority_;
    std::atomic<bool> claimed_{false};
    std::atomic<PipelineState> state_{PipelineState::deferred};
    std::atomic<Status> status_{Status::not_ready};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string error_;
    PipelineLimit limit_;
    double seconds_ = 0;
    bool from_archive_ = false;
    std::atomic<bool> skip_logged_{false};
};

class MetalRenderPipeline final : public gpu::RenderPipeline, public PipelineCore {
public:
    MetalRenderPipeline(MetalDevice* device, const RenderPipelineDesc& desc);
    ~MetalRenderPipeline() override;
    PipelineState state() const override { return core_state(); }
    Status wait() override { return core_wait(); }
    const std::string& error() const override { return error_; }
    const PipelineReflection& reflection() const override { return reflection_; }
    double compile_seconds() const override { return seconds_; }
    const PipelineLimit& limit() const override { return limit_; }
    Status status() const override { return core_status(); }
    bool from_archive() const override { return from_archive_; }
    RectMode rect_mode() const override { return rect_mode_; }
    D24Mode d24_mode() const override { return d24_mode_; }

    void prepare();
    const RenderPipelineDesc& desc() const { return desc_; }
    id<MTLRenderPipelineState> pso() const { return pso_; }
    id<MTLRenderPipelineState> rect_pso() const { return rect_pso_; }
    id<MTLDepthStencilState> depth_state() const { return depth_state_; }
    const StagePlan& vertex_plan() const { return vertex_plan_; }
    const StagePlan& fragment_plan() const { return fragment_plan_; }
    std::uint32_t vertex_buffer_index(std::uint32_t slot) const { return 30 - slot; }
    const rect::WrapperResult& rect_wrapper() const { return rect_wrapper_; }
    id<MTLBuffer> rect_params_dummy() const { return rect_dummy_; }
    int d24_params_index() const { return d24_mode_ == D24Mode::fragment ? int(d24_wrapper_.params_index) : -1; }

private:
    void build() override;
    MTLRenderPipelineDescriptor* make_descriptor(bool for_rect, std::string& err);
    void build_plan(MTLRenderPipelineReflection* refl);
    void build_rect();
    void build_d24();

    RenderPipelineDesc desc_;
    PipelineReflection reflection_;
    id<MTLRenderPipelineState> pso_ = nil;
    id<MTLRenderPipelineState> rect_pso_ = nil;
    id<MTLDepthStencilState> depth_state_ = nil;
    StagePlan vertex_plan_, fragment_plan_;
    RectMode rect_mode_ = RectMode::none;
    rect::WrapperResult rect_wrapper_;
    id<MTLBuffer> rect_dummy_ = nil;
    D24Mode d24_mode_ = D24Mode::rasterizer;
    msl::D24Wrapper d24_wrapper_;
};

class MetalComputePipeline final : public gpu::ComputePipeline, public PipelineCore {
public:
    MetalComputePipeline(MetalDevice* device, const ComputePipelineDesc& desc);
    ~MetalComputePipeline() override;
    PipelineState state() const override { return core_state(); }
    Status wait() override { return core_wait(); }
    const std::string& error() const override { return error_; }
    const PipelineReflection& reflection() const override { return reflection_; }
    double compile_seconds() const override { return seconds_; }
    const PipelineLimit& limit() const override { return limit_; }
    Status status() const override { return core_status(); }
    bool from_archive() const override { return from_archive_; }
    std::array<std::uint32_t, 3> threads() const override { return threads_; }
    std::uint32_t threadgroup_memory() const override { return pso_ ? std::uint32_t(pso_.staticThreadgroupMemoryLength) : 0; }
    std::uint32_t max_total_threads() const override { return pso_ ? std::uint32_t(pso_.maxTotalThreadsPerThreadgroup) : 0; }
    std::uint32_t execution_width() const override { return pso_ ? std::uint32_t(pso_.threadExecutionWidth) : 0; }

    id<MTLComputePipelineState> pso() const { return pso_; }
    const RootLayout* root() const { return desc_.root; }
    const StagePlan& plan() const { return plan_; }
    MTLSize group_size() const { return MTLSizeMake(threads_[0], threads_[1], threads_[2]); }

private:
    void build() override;
    ComputePipelineDesc desc_;
    PipelineReflection reflection_;
    id<MTLComputePipelineState> pso_ = nil;
    StagePlan plan_;
    std::array<std::uint32_t, 3> threads_{1, 1, 1};
};

class MetalMeshPipeline final : public gpu::MeshPipeline, public PipelineCore {
public:
    MetalMeshPipeline(MetalDevice* device, const MeshPipelineDesc& desc);
    ~MetalMeshPipeline() override;
    PipelineState state() const override { return core_state(); }
    Status wait() override { return core_wait(); }
    const std::string& error() const override { return error_; }
    const PipelineReflection& reflection() const override { return reflection_; }
    double compile_seconds() const override { return seconds_; }
    const PipelineLimit& limit() const override { return limit_; }
    Status status() const override { return core_status(); }
    bool from_archive() const override { return from_archive_; }
    const MeshEstimate& estimate() const override { return estimate_; }
    std::array<std::uint32_t, 3> mesh_threads() const override { return mesh_threads_; }
    std::array<std::uint32_t, 3> object_threads() const override { return object_threads_; }

    void prepare();
    const MeshPipelineDesc& desc() const { return desc_; }
    id<MTLRenderPipelineState> pso() const { return pso_; }
    id<MTLDepthStencilState> depth_state() const { return depth_state_; }
    const StagePlan& object_plan() const { return object_plan_; }
    const StagePlan& mesh_plan() const { return mesh_plan_; }
    const StagePlan& fragment_plan() const { return fragment_plan_; }

private:
    void build() override;
    MeshPipelineDesc desc_;
    PipelineReflection reflection_;
    MeshEstimate estimate_;
    id<MTLRenderPipelineState> pso_ = nil;
    id<MTLDepthStencilState> depth_state_ = nil;
    StagePlan object_plan_, mesh_plan_, fragment_plan_;
    std::array<std::uint32_t, 3> mesh_threads_{1, 1, 1}, object_threads_{1, 1, 1};
};

class MetalQueryHeap final : public gpu::QueryHeap {
public:
    MetalQueryHeap(MetalDevice* device, const QueryHeapDesc& desc, id<MTLBuffer> results, id<MTLCounterSampleBuffer> samples);
    ~MetalQueryHeap() override;
    QueryType type() const override { return type_; }
    std::uint32_t count() const override { return count_; }
    id<MTLBuffer> results() const { return results_; }
    id<MTLCounterSampleBuffer> samples() const { return samples_; }

private:
    MetalDevice* device_;
    QueryType type_;
    std::uint32_t count_;
    id<MTLBuffer> results_;
    id<MTLCounterSampleBuffer> samples_;
    std::uint64_t accounted_ = 0;
};

// ---- command-list IR ----

struct DrawState {
    MetalRenderPipeline* pipeline = nullptr;
    const MetalRootLayout* root = nullptr;
    std::array<std::uint32_t, 32> user_data{};
    std::uint64_t heap_base = 0, code_base = 0;
    std::array<MetalDescriptorHeap*, 32> tables{};
    std::array<VertexBufferView, 8> vertex_buffers{};
    IndexBufferView index{};
    std::uint32_t topology = topology::triangle_list;
    Viewport viewport{};
    bool viewport_set = false;
    Rect scissor{};
    bool scissor_set = false;
    std::array<float, 4> blend_factor{1, 1, 1, 1};
    std::uint32_t stencil_ref = 0;
    std::uint32_t stencil_back_ref = 0;
    bool separate_stencil_refs = false;
    MetalComputePipeline* compute = nullptr;
    MetalMeshPipeline* mesh = nullptr;
};

struct CmdBeginPass { RenderPassDesc desc; };
struct CmdEndPass {};
struct CmdSetTargets {
    std::array<TargetView, 8> targets{};
    std::uint32_t count = 0;
    TargetView depth;
    bool has_depth = false;
};
struct CmdClearTarget { TargetView target; std::array<float, 4> color; };
struct CmdClearDepth { TargetView target; bool depth; float depth_value; bool stencil; std::uint8_t stencil_value; };
struct CmdDiscard { gpu::Resource* resource; };
struct CmdDraw {
    std::shared_ptr<const DrawState> state;
    bool indexed = false;
    std::uint32_t count = 0, instances = 1, first = 0, first_instance = 0;
    std::int32_t base_vertex = 0;
};
struct CmdCopyBuffer { MetalBuffer* dst; std::uint64_t dst_offset; MetalBuffer* src; std::uint64_t src_offset; std::uint64_t size; };
struct CmdCopyBufferTexture { BufferTextureCopy copy; bool to_texture; };
struct CmdCopyTexture { TextureCopy copy; };
struct CmdFillU32 { MetalBuffer* buffer; std::uint64_t offset; std::uint64_t size; std::uint32_t value; };
struct CmdBarrier { std::vector<Barrier> barriers; };
struct CmdWriteValue { MetalBuffer* buffer; std::uint64_t offset; std::uint64_t value; std::uint32_t bytes; };
struct CmdWaitValue { MetalBuffer* buffer; std::uint64_t offset; std::uint64_t value; std::uint32_t bytes; WaitCompare compare; };
struct CmdBeginEvent { std::string label; };
struct CmdEndEvent {};
struct CmdDispatch { std::shared_ptr<const DrawState> state; std::uint32_t x, y, z, flags; };
struct CmdDrawMesh { std::shared_ptr<const DrawState> state; std::uint32_t x, y, z; };
struct CmdIndirect { std::shared_ptr<const DrawState> state; IndirectDesc desc; };
struct CmdQuery { MetalQueryHeap* heap; std::uint32_t index; bool begin; };
struct CmdResolveQueries { MetalQueryHeap* heap; std::uint32_t first, count; MetalBuffer* dst; std::uint64_t offset; };
struct CmdWriteTimestamp { MetalBuffer* buffer; std::uint64_t offset; };

using Cmd = std::variant<CmdBeginPass, CmdEndPass, CmdSetTargets, CmdClearTarget, CmdClearDepth, CmdDiscard, CmdDraw,
                         CmdCopyBuffer, CmdCopyBufferTexture, CmdBarrier, CmdWriteValue, CmdWaitValue, CmdBeginEvent,
                         CmdEndEvent, CmdDispatch, CmdDrawMesh, CmdIndirect, CmdQuery, CmdResolveQueries,
                         CmdWriteTimestamp, CmdCopyTexture, CmdFillU32>;

class MetalCommandList final : public gpu::CommandList {
public:
    explicit MetalCommandList(QueueType type) : type_(type) {}
    QueueType type() const override { return type_; }
    void reset() override;
    Status close() override;
    void begin_render_pass(const RenderPassDesc& desc) override;
    void end_render_pass() override;
    void set_render_targets(std::span<const TargetView> targets, const TargetView* depth) override;
    void clear_render_target(const TargetView& target, const std::array<float, 4>& color) override;
    void clear_depth_stencil(const TargetView& target, bool depth, float depth_value, bool stencil,
                             std::uint8_t stencil_value) override;
    void discard(gpu::Resource* resource) override;
    void set_pipeline(gpu::RenderPipeline* pipeline) override;
    void set_root_layout(const gpu::RootLayout* layout) override;
    void set_user_data(std::uint32_t first, std::span<const std::uint32_t> dwords) override;
    void set_heap_base(std::uint64_t value) override;
    void set_code_base(std::uint64_t value) override;
    void set_descriptor_table(std::uint32_t binding, gpu::DescriptorHeap* heap) override;
    void set_vertex_buffers(std::uint32_t first_slot, std::span<const VertexBufferView> views) override;
    void set_index_buffer(const IndexBufferView& view) override;
    void set_topology(std::uint32_t d3d_topology) override;
    void set_viewport(const Viewport& viewport) override;
    void set_scissor(const Rect& rect) override;
    void set_blend_factor(const std::array<float, 4>& factor) override;
    void set_stencil_ref(std::uint32_t ref) override;
    void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex,
              std::uint32_t first_instance) override;
    void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                      std::int32_t base_vertex, std::uint32_t first_instance) override;
    void copy_buffer(gpu::Buffer* dst, std::uint64_t dst_offset, gpu::Buffer* src, std::uint64_t src_offset,
                     std::uint64_t size) override;
    void copy_buffer_to_texture(const BufferTextureCopy& copy) override;
    void copy_texture_to_buffer(const BufferTextureCopy& copy) override;
    void copy_texture(const TextureCopy& copy) override;
    void fill_buffer_u32(gpu::Buffer* buffer, std::uint64_t offset, std::uint64_t size, std::uint32_t value) override;
    void barrier(std::span<const Barrier> barriers) override;
    void write_value(gpu::Buffer* buffer, std::uint64_t offset, std::uint64_t value, std::uint32_t bytes) override;
    void wait_value(gpu::Buffer* buffer, std::uint64_t offset, std::uint64_t value, std::uint32_t bytes,
                    WaitCompare compare) override;
    void begin_event(const char* label) override;
    void end_event() override;
    void set_compute_pipeline(gpu::ComputePipeline* pipeline) override;
    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t flags) override;
    void set_mesh_pipeline(gpu::MeshPipeline* pipeline) override;
    void draw_mesh(std::uint32_t x, std::uint32_t y, std::uint32_t z) override;
    void execute_indirect(const IndirectDesc& desc) override;
    void set_stencil_refs(std::uint32_t front, std::uint32_t back) override;
    void begin_query(gpu::QueryHeap* heap, std::uint32_t index) override;
    void end_query(gpu::QueryHeap* heap, std::uint32_t index) override;
    void resolve_queries(gpu::QueryHeap* heap, std::uint32_t first, std::uint32_t count, gpu::Buffer* dst,
                         std::uint64_t offset) override;
    void write_timestamp(gpu::Buffer* buffer, std::uint64_t offset) override;

    const std::vector<Cmd>& commands() const { return commands_; }
    bool closed() const { return closed_; }
    const std::string& record_error() const { return error_; }

private:
    void push(Cmd&& cmd);
    DrawState& edit_state();
    std::shared_ptr<const DrawState> snapshot();
    QueueType type_;
    std::vector<Cmd> commands_;
    std::shared_ptr<DrawState> state_ = std::make_shared<DrawState>();
    bool state_shared_ = false;
    bool closed_ = false;
    bool in_pass_ = false;
    std::string error_;
};

class MetalFence final : public gpu::Fence {
public:
    MetalFence(MetalDevice* device, id<MTLSharedEvent> event, id<MTLBuffer> mirror, std::uint64_t initial);
    std::uint64_t completed_value() const override { return event_.signaledValue; }
    Status signal_cpu(std::uint64_t value) override;
    Status wait_cpu(std::uint64_t value, std::uint64_t timeout_ns) override;
    Status on_completion(std::uint64_t value, std::function<void()> callback) override;
    const volatile std::uint64_t* value_address() const override {
        return static_cast<const volatile std::uint64_t*>(mirror_.contents);
    }
    id<MTLSharedEvent> mtl() const { return event_; }
    void track_mirror(std::uint64_t value);

private:
    MetalDevice* device_;
    id<MTLSharedEvent> event_;
    id<MTLBuffer> mirror_;
};

class MetalSwapchain;

// Fences one encoder waits on before it runs and the fence it updates when it is done (DESIGN.md 3.3).
struct EncoderFences {
    std::array<id<MTLFence>, 9> wait{};
    std::uint32_t wait_count = 0;
    id<MTLFence> update = nil;
    bool joined = false;  // this encoder runs after every earlier encoder of the queue
};

class MetalQueue final : public gpu::Queue {
public:
    MetalQueue(MetalDevice* device, QueueType type, id<MTLCommandQueue> queue);
    ~MetalQueue() override;
    QueueType type() const override { return type_; }
    Status execute(std::span<gpu::CommandList* const> lists) override;
    Status signal(gpu::Fence* fence, std::uint64_t value) override;
    Status wait(gpu::Fence* fence, std::uint64_t value) override;
    Status present(gpu::Swapchain* swapchain, std::uint32_t sync_interval) override;
    Status wait_idle(std::uint64_t timeout_ns) override;
    std::uint64_t timestamp_frequency() const override;
    Status present_planes(gpu::Swapchain* swapchain, const PresentDesc& desc) override;
    ClockCalibration clock_calibration() const override;

    id<MTLCommandBuffer> begin_command_buffer();
    // Encodes the given event signals and the timeline signal, then commits. A command buffer that fails skips its
    // remaining event signals; its completed handler does them on the CPU (DESIGN.md 3.12).
    void end_command_buffer(id<MTLCommandBuffer> cb, std::span<const EventSignal> signals = {},
                            std::string summary = {});
    void inject_failure(bool as_victim) {
        inject_victim_ = as_victim;
        inject_failure_ = true;
    }
    // Called with the submit lock held, once per encoder, in encoding order (DESIGN.md 3.3). Every encoder waits
    // for the work before the last barrier; join (a barrier) starts a new epoch, observe also waits for the current
    // epoch without starting one. Untracked render targets already attached in this epoch force a join.
    enum class Join : std::uint32_t { none, join, observe };
    EncoderFences open_encoder(Join mode, std::span<const void* const> untracked_targets);
    MetalDevice* device() const { return device_; }
    id<MTLSharedEvent> timeline() const { return timeline_; }
    std::uint64_t last_serial() {
        std::lock_guard lock(mutex_);
        return serial_;
    }
    // The timeline value every command buffer committed or being encoded now signals once it is done.
    std::uint64_t retire_mark() {
        std::lock_guard lock(mutex_);
        return serial_ + open_;
    }

private:
    static constexpr std::uint32_t kFencePool = 9;
    static constexpr std::uint32_t kMaxLive = kFencePool - 1;
    MetalDevice* device_;
    QueueType type_;
    id<MTLCommandQueue> queue_;
    id<MTLSharedEvent> timeline_;
    std::array<id<MTLFence>, kFencePool> fences_{};
    std::uint32_t waits_ = 0;  // fences of the encoders before the last join (every encoder of this epoch waits)
    std::uint32_t epoch_ = 0;  // fences of the encoders since the last join
    std::uint32_t next_fence_ = 0;
    std::unordered_set<const void*> epoch_targets_;
    std::mutex mutex_;
    std::mutex submit_mutex_;
    std::uint64_t serial_ = 0;
    std::uint64_t open_ = 0;
    std::vector<std::pair<id<MTLSharedEvent>, std::uint64_t>> pending_waits_;
    std::shared_ptr<QueueErrors> errors_ = std::make_shared<QueueErrors>();
    std::atomic<bool> inject_failure_{false}, inject_victim_{false};
    id<MTLCommandBuffer> injected_ = nil;
    bool injected_victim_ = false;
};

class FramePacer;

class MetalSwapchain final : public gpu::Swapchain {
public:
    MetalSwapchain(MetalDevice* device, const SwapchainDesc& desc, CAMetalLayer* layer,
                   std::vector<std::unique_ptr<MetalTexture>> buffers);
    ~MetalSwapchain() override;
    std::uint32_t buffer_count() const override { return std::uint32_t(buffers_.size()); }
    gpu::Texture* buffer(std::uint32_t index) override { return index < buffers_.size() ? buffers_[index].get() : nullptr; }
    std::uint32_t current_index() const override { return index_; }
    std::uint64_t presented() const override { return presented_; }
    Status read_last_frame(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height) override;
    FrameStats frame_stats() const override;
    Status wait_frame_event(std::uint64_t timeout_ns) override;
    Status encode_present(MetalQueue* queue, const PresentDesc& desc, bool advance);

private:
    void on_presented(double t);
    MetalDevice* device_;
    SwapchainDesc desc_;
    CAMetalLayer* layer_;
    std::vector<std::unique_ptr<MetalTexture>> buffers_;
    std::uint32_t index_ = 0;
    std::uint64_t presented_ = 0;
    id<MTLBuffer> last_frame_ = nil;
    std::uint32_t last_w_ = 0, last_h_ = 0;
    id<MTLCommandBuffer> last_cb_ = nil;
    mutable std::mutex stats_mutex_;
    std::uint64_t displayed_ = 0;
    double last_presented_time_ = 0;
    std::deque<double> intervals_;
    std::shared_ptr<FramePacer> pacer_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

// Polls CPU-visible values for wait_value and releases the gate events (DESIGN.md 5.2).
class ValuePump {
public:
    ~ValuePump();
    void add(const volatile void* address, std::uint32_t bytes, std::uint64_t value, WaitCompare compare,
             id<MTLSharedEvent> gate, std::uint64_t gate_value);

private:
    void run();
    struct Wait {
        const volatile void* address;
        std::uint32_t bytes;
        std::uint64_t value;
        WaitCompare compare;
        id<MTLSharedEvent> gate;
        std::uint64_t gate_value;
    };
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Wait> waits_;
    std::thread thread_;
    bool stop_ = false;
};

// Pipeline compile workers with three priority FIFOs (DESIGN.md 3.7).
class CompileQueue {
public:
    ~CompileQueue();
    void init(std::uint32_t threads);
    void enqueue(PipelineCore* p, Priority priority);
    bool steal(PipelineCore* p);   // a caller claimed it: drop it from the queue
    void forget(PipelineCore* p);  // destruction: drop it, or wait while a worker builds it
    CompileQueueStats stats() const;
    void set_limits(std::uint32_t active, std::uint32_t background);

private:
    void run();
    bool runnable_locked(std::size_t* priority, std::size_t* index) const;
    void erase_locked(PipelineCore* p);
    void publish_pending_locked() const;
    std::uint32_t threads_n_ = 3, active_limit_ = 3, background_limit_ = 2;
    std::uint32_t running_background_ = 0, running_large_ = 0, large_limit_ = 2;
    std::size_t large_bytes_ = 512 * 1024;
    std::uint64_t large_deferred_ = 0;
    int qos_[3] = {0, 0, 0};
    mutable std::mutex mutex_;
    std::condition_variable cv_, done_cv_;
    std::array<std::deque<PipelineCore*>, 3> queues_;
    std::unordered_set<PipelineCore*> running_;
    std::vector<std::thread> threads_;
    std::array<std::uint64_t, 3> completed_{};
    std::uint64_t stolen_ = 0;
    bool stop_ = false;
};

class MetalDevice final : public gpu::Device {
public:
    MetalDevice(id<MTLDevice> device, const DeviceOptions& options);
    ~MetalDevice() override;
    const DeviceCaps& caps() const override { return caps_; }
    DeviceStats stats() const override;
    bool describe_format(std::uint32_t dxgi, FormatInfo& out) const override;
    std::unique_ptr<gpu::Queue> create_queue(QueueType type, const char* label) override;
    std::unique_ptr<gpu::Fence> create_fence(std::uint64_t initial) override;
    AllocationInfo buffer_allocation_info(const BufferDesc& desc) const override;
    AllocationInfo texture_allocation_info(const TextureDesc& desc) const override;
    std::unique_ptr<gpu::Heap> create_heap(const HeapDesc& desc) override;
    std::unique_ptr<gpu::Buffer> create_buffer(const BufferDesc& desc) override;
    std::unique_ptr<gpu::Texture> create_texture(const TextureDesc& desc) override;
    std::unique_ptr<gpu::Buffer> create_placed_buffer(gpu::Heap* heap, std::uint64_t offset, const BufferDesc& desc) override;
    std::unique_ptr<gpu::Texture> create_placed_texture(gpu::Heap* heap, std::uint64_t offset, const TextureDesc& desc) override;
    bool resolve_va(std::uint64_t va, gpu::Buffer** buffer, std::uint64_t* offset) const override;
    std::unique_ptr<gpu::Sampler> create_sampler(const SamplerDesc& desc) override;
    std::unique_ptr<gpu::DescriptorHeap> create_descriptor_heap(const DescriptorHeapDesc& desc) override;
    std::unique_ptr<gpu::RootLayout> create_root_layout(const RootLayoutDesc& desc) override;
    std::unique_ptr<gpu::ShaderLibrary> create_library_msl(std::string_view source, const MslOptions& options,
                                                           std::string* error) override;
    std::unique_ptr<gpu::ShaderLibrary> create_library_metallib(std::span<const std::uint8_t> bytes,
                                                                std::string* error) override;
    std::unique_ptr<gpu::RenderPipeline> create_render_pipeline(const RenderPipelineDesc& desc) override;
    std::unique_ptr<gpu::CommandList> create_command_list(QueueType type) override;
    std::unique_ptr<gpu::Swapchain> create_swapchain(const SwapchainDesc& desc) override;
    std::unique_ptr<gpu::ComputePipeline> create_compute_pipeline(const ComputePipelineDesc& desc) override;
    std::unique_ptr<gpu::MeshPipeline> create_mesh_pipeline(const MeshPipelineDesc& desc) override;
    std::unique_ptr<gpu::QueryHeap> create_query_heap(const QueryHeapDesc& desc) override;
    std::unique_ptr<gpu::Buffer> create_host_buffer(const HostBufferDesc& desc, std::string* error) override;
    std::unique_ptr<gpu::Texture> create_buffer_texture(gpu::Buffer* buffer, std::uint64_t offset, std::uint32_t row_pitch,
                                                        const TextureDesc& desc, std::string* error) override;
    MemoryBudget memory_budget() const override;
    std::uint64_t add_memory_pressure_handler(std::function<void(MemoryPressure)> handler) override;
    void remove_memory_pressure_handler(std::uint64_t token) override;
    void simulate_memory_pressure(MemoryPressure level) override;
    Status open_pipeline_cache(const PipelineCacheDesc& desc, std::string* error) override;
    Status flush_pipeline_cache(std::string* error) override;
    PipelineCacheStats pipeline_cache_stats() const override;
    void precompile(std::span<gpu::Pipeline* const> pipelines, Priority priority) override;
    CompileQueueStats compile_queue_stats() const override { return compiler_.stats(); }
    MeshEstimate estimate_mesh(gpu::ShaderLibrary* library, const std::string& entry) const override;
    std::unique_ptr<gpu::Texture> create_texture_view(gpu::Texture* texture, const gpu::TextureViewDesc& desc,
                                                      std::string* error) override;

    id<MTLDevice> mtl() const { return device_; }
    AtomicStats& counters() { return stats_; }
    MemoryLedger& ledger() { return ledger_; }
    VaSpace& va_space() { return va_; }
    ValuePump& pump() { return pump_; }
    CompileQueue& compiler() { return compiler_; }
    MTLSharedEventListener* listener() const { return listener_; }
    id<MTLRenderPipelineState> compose_pipeline(MTLPixelFormat format, PlaneBlend blend);
    id<MTLSamplerState> compose_sampler(Filter filter) const { return filter == Filter::linear ? compose_linear_ : compose_sampler_; }
    id<MTLSamplerState> compose_sampler() const { return compose_sampler_; }
    id<MTLLibrary> compile_msl(const std::string& source, std::uint32_t major, std::uint32_t minor, bool fast_math,
                               std::string* error, double* seconds);
    id<MTLComputePipelineState> builtin_kernel(const char* name);
    id<MTLBuffer> quad_pattern(std::uint32_t quads);
    id<MTLCounterSet> timestamp_counters() const { return timestamp_set_; }

    // Binary-archive-backed builds (DESIGN.md 3.7): try the archive first, compile and add on a miss.
    id<MTLRenderPipelineState> build_render(MTLRenderPipelineDescriptor* pd, MTLRenderPipelineReflection** refl,
                                            NSError** error, bool* hit);
    id<MTLRenderPipelineState> build_mesh(MTLMeshRenderPipelineDescriptor* pd, MTLRenderPipelineReflection** refl,
                                          NSError** error, bool* hit);
    id<MTLComputePipelineState> build_compute(MTLComputePipelineDescriptor* pd, MTLComputePipelineReflection** refl,
                                              NSError** error, bool* hit);

    // Device-wide residency (DESIGN.md 3.5): every heap and committed resource is resident from creation until
    // it is destroyed, as in D3D12. The set is attached to every queue; changes are committed before a submit.
    void make_resident(id<MTLAllocation> allocation);
    void evict(id<MTLAllocation> allocation);
    void commit_residency();
    bool table_residency() const { return table_residency_; }

    // Releases wait for the GPU (spike/r7-device-loss): Metal fails a command buffer with InvalidResource when an
    // allocation it names (bound, in an argument buffer, in a residency set it uses) is released before the command
    // buffer was scheduled, e.g. while it waits at an event. retire() keeps `object` and leaves `evict_allocation` in
    // the device residency set until every command buffer committed or being encoded on any queue at the call has
    // completed (its queue's timeline reached it); collect_retired() releases what has.
    RetireMarks retire_marks();
    static bool marks_reached(const RetireMarks& marks);
    void retire(id object, id<MTLAllocation> evict_allocation = nil);
    void collect_retired(bool everything = false);
    void add_queue(MetalQueue* queue);
    void remove_queue(MetalQueue* queue);

    // Set by a completed handler when Metal stopped running this process's work: a GPU timeout, submissions
    // ignored, device removed. Every later execute answers device_lost.
    std::atomic<bool>& gpu_fatal() { return gpu_fatal_; }

private:
    std::atomic<bool> gpu_fatal_{false};
    void probe_caps();
    void start_pressure_source();
    void dispatch_pressure(MemoryPressure level);
    MTLTextureDescriptor* texture_descriptor(const TextureDesc& desc, const NativeFormat& format, std::string& err) const;

    id<MTLDevice> device_;
    DeviceOptions options_;
    DeviceCaps caps_;
    AtomicStats stats_;
    MemoryLedger ledger_;
    VaSpace va_;
    ValuePump pump_;
    CompileQueue compiler_;
    MTLSharedEventListener* listener_ = nil;
    dispatch_queue_t listener_queue_ = nullptr;
    std::mutex builtin_mutex_;
    id<MTLLibrary> builtin_library_ = nil;
    std::map<std::pair<MTLPixelFormat, PlaneBlend>, id<MTLRenderPipelineState>> compose_;
    std::map<std::string, id<MTLComputePipelineState>> kernels_;
    id<MTLBuffer> quad_pattern_ = nil;
    std::uint32_t quad_pattern_quads_ = 0;
    id<MTLSamplerState> compose_sampler_ = nil;
    id<MTLSamplerState> compose_linear_ = nil;
    id<MTLCounterSet> timestamp_set_ = nil;
    std::mutex residency_mutex_;
    id<MTLResidencySet> resident_ = nil;
    bool resident_dirty_ = false;
    struct Retired {
        id object = nil;
        id<MTLAllocation> evict = nil;
        std::uint64_t bytes = 0;
        RetireMarks marks;
    };
    std::mutex retire_mutex_;
    std::vector<MetalQueue*> queues_;
    std::deque<Retired> retired_;
    std::uint64_t retired_bytes_ = 0;
    bool table_residency_ = true;
    // Shards (open_pipeline_cache): archive_mutex_ guards the lists and stats, archive_add_mutex_ the adds into the
    // writable archive_ (a seal takes both, briefly), archive_flush_mutex_ one flush at a time.
    NSArray* archive_lookup_locked() const;
    std::string next_shard_path_locked();
    template <class P, class Make, class Add>
    P archive_build(id descriptor, bool* hit, NSError** error, Make&& make, Add&& add);
    mutable std::mutex archive_mutex_;
    std::mutex archive_add_mutex_, archive_flush_mutex_;
    id<MTLBinaryArchive> archive_ = nil;
    std::vector<id<MTLBinaryArchive>> archive_shards_;
    std::vector<std::string> archive_files_;
    std::string archive_dir_, archive_prefix_, archive_tag_ = "run";
    std::uint64_t archive_shard_cap_ = 150'000'000;
    std::uint64_t archive_bytes_per_pipeline_ = 512 * 1024;
    std::uint32_t archive_seq_ = 0;
    PipelineCacheStats archive_stats_;
    bool archive_add_first_ = false;

public:
    void set_archive_tag(const std::string& tag);
    void set_compile_limits(std::uint32_t active, std::uint32_t background) { compiler_.set_limits(active, background); }
    // set_debug_buffer (metal_backend.h): set before the first command list is encoded, never changed after.
    void set_debug_buffer(id<MTLBuffer> buffer) { debug_buffer_ = buffer; }
    id<MTLBuffer> debug_buffer() const { return debug_buffer_; }

private:
    id<MTLBuffer> debug_buffer_ = nil;
    mutable std::mutex pressure_mutex_;
    std::map<std::uint64_t, std::function<void(MemoryPressure)>> pressure_handlers_;
    std::uint64_t next_pressure_token_ = 1;
    std::atomic<std::uint32_t> pressure_level_{0};
    std::atomic<std::uint64_t> pressure_events_{0};
    dispatch_source_t pressure_source_ = nullptr;
    dispatch_queue_t pressure_queue_ = nullptr;
};

double now_seconds();
bool env_flag(const char* name);

}  // namespace gta6rt::metal
