#include "mtl_internal.h"

#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include <gta6rt_hud/stats.h>

namespace gta6rt::metal {

// ---- fences ----

MetalFence::MetalFence(MetalDevice* device, id<MTLSharedEvent> event, id<MTLBuffer> mirror, std::uint64_t initial)
    : device_(device), event_(event), mirror_(mirror) {
    *static_cast<volatile std::uint64_t*>(mirror_.contents) = initial;
}

Status MetalFence::signal_cpu(std::uint64_t value) {
    event_.signaledValue = value;
    *static_cast<volatile std::uint64_t*>(mirror_.contents) = value;
    return Status::ok;
}

Status MetalFence::wait_cpu(std::uint64_t value, std::uint64_t timeout_ns) {
    if (event_.signaledValue >= value) return Status::ok;
    const std::uint64_t ms = timeout_ns == ~0ull ? ~0ull : (timeout_ns + 999999) / 1000000;
    return [event_ waitUntilSignaledValue:value timeoutMS:ms] ? Status::ok : Status::timeout;
}

Status MetalFence::on_completion(std::uint64_t value, std::function<void()> callback) {
    if (!callback) return Status::invalid_argument;
    auto shared = std::make_shared<std::function<void()>>(std::move(callback));
    [event_ notifyListener:device_->listener()
                   atValue:value
                     block:^(id<MTLSharedEvent>, uint64_t) {
                       (*shared)();
                     }];
    return Status::ok;
}

void MetalFence::track_mirror(std::uint64_t value) {
    id<MTLBuffer> mirror = mirror_;
    [event_ notifyListener:device_->listener()
                   atValue:value
                     block:^(id<MTLSharedEvent> e, uint64_t) {
                       auto* p = static_cast<volatile std::uint64_t*>(mirror.contents);
                       const std::uint64_t v = e.signaledValue;
                       if (v > *p) *p = v;
                     }];
}

// ---- value pump ----

ValuePump::~ValuePump() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ValuePump::add(const volatile void* address, std::uint32_t bytes, std::uint64_t value, WaitCompare compare,
                    id<MTLSharedEvent> gate, std::uint64_t gate_value) {
    {
        std::lock_guard lock(mutex_);
        waits_.push_back({address, bytes, value, compare, gate, gate_value});
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    }
    cv_.notify_all();
}

void ValuePump::run() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        if (waits_.empty()) {
            cv_.wait(lock, [&] { return stop_ || !waits_.empty(); });
            continue;
        }
        for (std::size_t i = 0; i < waits_.size();) {
            const Wait& w = waits_[i];
            const std::uint64_t v = w.bytes == 8 ? *static_cast<const volatile std::uint64_t*>(w.address)
                                                 : *static_cast<const volatile std::uint32_t*>(w.address);
            const bool met = w.compare == WaitCompare::greater_equal ? v >= w.value
                           : w.compare == WaitCompare::equal         ? v == w.value
                                                                     : v != w.value;
            if (met) {
                w.gate.signaledValue = w.gate_value;
                waits_.erase(waits_.begin() + std::ptrdiff_t(i));
            } else {
                ++i;
            }
        }
        cv_.wait_for(lock, std::chrono::microseconds(100));
    }
}

// ---- queue ----

MetalQueue::MetalQueue(MetalDevice* device, QueueType type, id<MTLCommandQueue> queue)
    : device_(device), type_(type), queue_(queue) {
    timeline_ = [device->mtl() newSharedEvent];
    for (auto& f : fences_) f = [device->mtl() newFence];
    device_->add_queue(this);
}

MetalQueue::~MetalQueue() {
    wait_idle(5'000'000'000ull);
    device_->remove_queue(this);
    device_->collect_retired();
}

EncoderFences MetalQueue::open_encoder(Join mode, std::span<const void* const> untracked_targets) {
    static const bool no_target_join = [] {
        const char* v = std::getenv("GTA6RT_METAL_NO_TARGET_JOIN");
        return v && *v && *v != '0';
    }();
    EncoderFences out;
    bool target_join = false;
    if (!no_target_join)
        for (const void* t : untracked_targets) target_join |= epoch_targets_.count(t) != 0;
    const std::uint32_t all = (1u << kFencePool) - 1;
    const bool join = mode == Join::join || target_join;
    if (join || (~(waits_ | epoch_) & all) == 0) {
        if (epoch_) {
            waits_ = epoch_;
            device_->counters().fence_joins++;
            if (target_join && mode != Join::join) device_->counters().target_joins++;
        }
        epoch_ = 0;
        epoch_targets_.clear();
        out.joined = true;
    }
    const std::uint32_t wait = waits_ | (mode == Join::observe ? epoch_ : 0u);
    for (std::uint32_t i = 0; i < kFencePool; ++i)
        if (wait & (1u << i)) out.wait[out.wait_count++] = fences_[i];
    std::uint32_t slot = 0;
    for (std::uint32_t k = 0; k < kFencePool; ++k) {
        slot = (next_fence_ + k) % kFencePool;
        if (!((waits_ | epoch_) & (1u << slot))) break;
    }
    next_fence_ = (slot + 1) % kFencePool;
    epoch_ |= 1u << slot;
    out.update = fences_[slot];
    for (const void* t : untracked_targets) epoch_targets_.insert(t);
    return out;
}

namespace {

// inject_command_buffer_failure: a copy from a buffer released before commit, in a command buffer without retained
// references. Metal fails the command buffer there with InvalidResource (raised by the kernel driver for this process
// only, measured) and skips the rest of it, as it does with a command buffer it discards to recover from another
// process's GPU error. The failure is reliable only when the command buffer runs on an idle queue and completes before
// the next one is committed (measured 8 of 8; with work queued around it most were not failed), so the injected
// command buffer waits for both, on the submitting thread. Measured exceptions: a command buffer that also samples GPU
// counters (0 of 5) or copies a texture to a buffer later on (0 of 6) completes without the error.
void encode_invalid_resource(id<MTLDevice> device, id<MTLCommandBuffer> cb) {
    id<MTLBuffer> dst = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    @autoreleasepool {
        id<MTLBuffer> src = [device newBufferWithLength:64ull << 20 options:MTLResourceStorageModePrivate];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromBuffer:src sourceOffset:0 toBuffer:dst destinationOffset:0 size:256];
        [b endEncoding];
    }
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
      (void)dst;
    }];
}

}  // namespace

id<MTLCommandBuffer> MetalQueue::begin_command_buffer() {
    device_->collect_retired();
    {
        std::lock_guard lock(mutex_);
        ++open_;
    }
    device_->commit_residency();
    const bool fail = inject_failure_.exchange(false);
    if (fail) {
        std::uint64_t committed;
        {
            std::lock_guard lock(mutex_);
            committed = serial_;
        }
        std::unique_lock l(errors_->mutex);
        errors_->cv.wait(l, [&] { return errors_->completed >= committed; });
    }
    static const bool encoder_status = [] {
        const char* v = std::getenv("GTA6RT_METAL_ENCODER_STATUS");
        return !v || std::strcmp(v, "0") != 0;
    }();
    id<MTLCommandBuffer> cb = nil;
    if (encoder_status) {
        MTLCommandBufferDescriptor* d = [MTLCommandBufferDescriptor new];
        d.retainedReferences = !fail;
        d.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
        cb = [queue_ commandBufferWithDescriptor:d];
    } else {
        cb = fail ? [queue_ commandBufferWithUnretainedReferences] : [queue_ commandBuffer];
    }
    if (fail) {
        injected_ = cb;
        injected_victim_ = inject_victim_;
    }
    {
        std::lock_guard lock(mutex_);
        for (auto& [ev, v] : pending_waits_) [cb encodeWaitForEvent:ev value:v];
        pending_waits_.clear();
    }
    if (fail) encode_invalid_resource(device_->mtl(), cb);
    device_->counters().command_buffers++;
    return cb;
}

// A failed command buffer skips the event signals after the point where it failed, so the timeline and any fence it
// signals would stay below their values and every later GPU or CPU wait on them would block (measured). Its
// completed handler does those signals on the CPU; after any failure every completed handler also raises the
// timeline to its own serial, which undoes a CPU write that raced past a later GPU signal (DESIGN.md 3.12).
// GTA6RT_METAL_SLOW_CB_MS (default 1000, 0: off): a command buffer that ran at least this long on the GPU is logged
// with its encoders, so the work behind a GPU timeout (the kernel stops a command buffer after a few seconds) is named
// before it happens.
static double slow_command_buffer_ms() {
    static const double ms = [] {
        const char* v = std::getenv("GTA6RT_METAL_SLOW_CB_MS");
        return v && *v ? std::strtod(v, nullptr) : 1000.0;
    }();
    return ms;
}

static const char* encoder_state_name(MTLCommandEncoderErrorState s) {
    switch (s) {
    case MTLCommandEncoderErrorStateCompleted: return "completed";
    case MTLCommandEncoderErrorStateAffected: return "affected";
    case MTLCommandEncoderErrorStatePending: return "pending";
    case MTLCommandEncoderErrorStateFaulted: return "FAULTED";
    default: return "unknown";
    }
}

void MetalQueue::end_command_buffer(id<MTLCommandBuffer> cb, std::span<const EventSignal> signals, std::string summary) {
    static const bool no_repair = env_flag("GTA6RT_METAL_NO_ERROR_REPAIR");
    std::shared_ptr<QueueErrors> errors = errors_;
    AtomicStats* stats = &device_->counters();
    std::atomic<bool>* fatal = &device_->gpu_fatal();
    std::vector<EventSignal> extra(signals.begin(), signals.end());
    id<MTLSharedEvent> timeline = timeline_;
    const bool as_victim = cb == injected_ && injected_victim_;
    const double slow_ms = slow_command_buffer_ms();
    std::lock_guard lock(mutex_);
    const std::uint64_t serial = ++serial_;
    if (open_) --open_;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
      const double gpu = done.GPUStartTime > 0 && done.GPUEndTime > done.GPUStartTime ? done.GPUEndTime - done.GPUStartTime : 0;
      if (gta6rt_hud_active() && gpu > 0) gta6rt_hud_note_gpu_ns(std::uint64_t(gpu * 1e9));
      if (slow_ms > 0 && gpu * 1000.0 >= slow_ms) {
          std::lock_guard l(errors->mutex);
          if (errors->slow_logged++ < 32)
              log("command buffer %llu ran %.0f ms on the GPU (status %d): %s", (unsigned long long)serial,
                  gpu * 1000.0, int(done.status), summary.empty() ? "(no encoder summary)" : summary.c_str());
      }
      const bool failed = done.status == MTLCommandBufferStatusError;
      if (failed) {
          NSError* e = done.error;
          const std::string text = e ? e.localizedDescription.UTF8String : "no error object";
          const bool victim = as_victim || (e && (e.code == 5 || text.find("InnocentVictim") != std::string::npos));
          const bool lost = !victim && e &&
                            (e.code == MTLCommandBufferErrorTimeout || e.code == MTLCommandBufferErrorAccessRevoked ||
                             e.code == MTLCommandBufferErrorDeviceRemoved);
          stats->command_buffer_errors++;
          if (victim) stats->command_buffer_victims++;
          errors->unreported.fetch_add(1);
          errors->any_failed = true;
          std::lock_guard l(errors->mutex);
          errors->last = "code " + std::to_string(e ? long(e.code) : 0) + ": " + text;
          if (errors->logged++ < 16) {
              log("command buffer %llu failed (%s): %s; GPU time %.0f ms", (unsigned long long)serial,
                  victim ? "discarded by GPU recovery" : "own error", errors->last.c_str(), gpu * 1000.0);
              NSArray* infos = e ? e.userInfo[MTLCommandBufferEncoderInfoErrorKey] : nil;
              unsigned shown = 0, completed = 0;
              for (id<MTLCommandBufferEncoderInfo> info in infos) {
                  if (info.errorState == MTLCommandEncoderErrorStateCompleted) {
                      ++completed;
                      continue;
                  }
                  if (shown++ >= 12) continue;
                  std::string signposts;
                  for (NSString* s in info.debugSignposts) {
                      if (signposts.size() > 300) break;
                      signposts += signposts.empty() ? "" : " / ";
                      signposts += s.UTF8String;
                  }
                  log("  encoder %s: %s%s%s", encoder_state_name(info.errorState),
                      info.label ? info.label.UTF8String : "(no label)", signposts.empty() ? "" : "; signposts ",
                      signposts.c_str());
              }
              if (infos) log("  %lu encoders reported, %u completed", (unsigned long)infos.count, completed);
              if (!summary.empty()) log("  encoded: %s", summary.c_str());
          }
          if (lost && !fatal->exchange(true))
              log("Metal stopped running this process's work (code %ld); every later submission answers device_lost",
                  long(e.code));
      }
      if (!no_repair && errors->any_failed.load()) {
          if (failed)
              for (const auto& [ev, v] : extra)
                  if (ev.signaledValue < v) {
                      ev.signaledValue = v;
                      stats->event_signals_repaired++;
                  }
          if (timeline.signaledValue < serial) {
              timeline.signaledValue = serial;
              if (failed) stats->event_signals_repaired++;
          }
      }
      {
          std::lock_guard l(errors->mutex);
          errors->completed++;
      }
      errors->cv.notify_all();
    }];
    for (const auto& [ev, v] : signals) [cb encodeSignalEvent:ev value:v];
    [cb encodeSignalEvent:timeline_ value:serial];
    [cb commit];
    if (cb == injected_) {
        injected_ = nil;
        [cb waitUntilCompleted];
    }
}

Status MetalQueue::signal(gpu::Fence* fence, std::uint64_t value) {
    auto* f = static_cast<MetalFence*>(fence);
    if (!f) return Status::invalid_argument;
    std::lock_guard submit(submit_mutex_);
    id<MTLCommandBuffer> cb = begin_command_buffer();
    {
        std::lock_guard lock(mutex_);
        [cb encodeWaitForEvent:timeline_ value:serial_];
    }
    f->track_mirror(value);
    const EventSignal s{f->mtl(), value};
    end_command_buffer(cb, std::span<const EventSignal>(&s, 1));
    return Status::ok;
}

Status MetalQueue::wait(gpu::Fence* fence, std::uint64_t value) {
    auto* f = static_cast<MetalFence*>(fence);
    if (!f) return Status::invalid_argument;
    std::lock_guard lock(mutex_);
    pending_waits_.emplace_back(f->mtl(), value);
    return Status::ok;
}

Status MetalQueue::present(gpu::Swapchain* swapchain, std::uint32_t sync_interval) {
    auto* s = static_cast<MetalSwapchain*>(swapchain);
    if (!s) return Status::invalid_argument;
    PresentDesc d;
    d.plane_count = 1;
    d.planes[0].source = s->buffer(s->current_index());
    d.planes[0].filter = Filter::point;
    d.planes[0].blend = PlaneBlend::opaque;
    d.sync_interval = sync_interval;
    std::lock_guard submit(submit_mutex_);
    return s->encode_present(this, d, true);
}

Status MetalQueue::present_planes(gpu::Swapchain* swapchain, const PresentDesc& desc) {
    auto* s = static_cast<MetalSwapchain*>(swapchain);
    if (!s || !desc.plane_count || desc.plane_count > 8) return Status::invalid_argument;
    for (std::uint32_t i = 0; i < desc.plane_count; ++i)
        if (!desc.planes[i].source) return Status::invalid_argument;
    std::lock_guard submit(submit_mutex_);
    return s->encode_present(this, desc, false);
}

ClockCalibration MetalQueue::clock_calibration() const {
    MTLTimestamp cpu = 0, gpu = 0;
    [device_->mtl() sampleTimestamps:&cpu gpuTimestamp:&gpu];
    ClockCalibration c;
    c.gpu_ticks = gpu;
    c.cpu_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    return c;
}

Status MetalQueue::wait_idle(std::uint64_t timeout_ns) {
    std::uint64_t target;
    {
        std::lock_guard lock(mutex_);
        target = serial_;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
    if (timeline_.signaledValue < target) {
        const std::uint64_t ms = (timeout_ns + 999999) / 1000000;
        if (![timeline_ waitUntilSignaledValue:target timeoutMS:ms]) return Status::timeout;
    }
    {
        std::unique_lock l(errors_->mutex);
        if (!errors_->cv.wait_until(l, deadline, [&] { return errors_->completed >= target; })) return Status::timeout;
    }
    return errors_->unreported.exchange(0) ? Status::device_lost : Status::ok;
}

std::uint64_t MetalQueue::timestamp_frequency() const { return device_->caps().timestamp_frequency; }

void inject_command_buffer_failure(gpu::Queue* queue, bool as_victim) {
    if (queue) static_cast<MetalQueue*>(queue)->inject_failure(as_victim);
}

// ---- encoding ----

namespace {

bool same_view(const TargetView& a, const TargetView& b) {
    return a.texture == b.texture && a.mip == b.mip && a.layer == b.layer;
}

MTLPrimitiveType primitive(std::uint32_t topo) {
    switch (topo) {
    case topology::point_list: return MTLPrimitiveTypePoint;
    case topology::line_list: return MTLPrimitiveTypeLine;
    case topology::line_strip: return MTLPrimitiveTypeLineStrip;
    case topology::triangle_strip: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}

constexpr MTLRenderStages kAllRenderStages = MTLRenderStageVertex | MTLRenderStageFragment;
constexpr MTLRenderStages kWaitStages =
    MTLRenderStageVertex | MTLRenderStageFragment | MTLRenderStageObject | MTLRenderStageMesh;

// D3D12_RESOURCE_STATES that only read: vertex/constant, index, depth read, non-pixel and pixel shader resource,
// indirect argument/predication, copy source, resolve source, shading-rate source.
constexpr std::uint32_t kReadOnlyStates = 0x1u | 0x2u | 0x20u | 0x40u | 0x80u | 0x200u | 0x800u | 0x2000u | 0x1000000u;

bool barrier_is_noop(const Barrier& b) {
    if (b.type != BarrierType::transition || !b.resource) return false;
    const auto read_only = [](std::uint32_t s) { return s != 0 && (s & ~kReadOnlyStates) == 0; };
    return read_only(b.before) && read_only(b.after);
}

constexpr std::uint32_t kPacked[] = {16, 20, 12, 12};

struct PendingClear {
    TargetView view;
    bool color = false;
    std::array<float, 4> rgba{};
    bool depth = false;
    float depth_value = 1.0f;
    bool stencil = false;
    std::uint8_t stencil_value = 0;
};

struct Prepared {
    id<MTLBuffer> buffer = nil;
    std::uint64_t offset = 0;
};

struct ActiveQuery {
    MetalQueryHeap* heap = nullptr;
    std::uint32_t index = 0;
};

class Encoder {
public:
    Encoder(MetalQueue* queue) : q_(queue), d_(queue->device()) {}
    Status run(std::span<gpu::CommandList* const> lists);

private:
    void visit(const Cmd& cmd);
    void end_encoder();
    void close_command_buffer();
    void flush_clears();
    void open_explicit(const RenderPassDesc& desc);
    void open_implicit();
    void attach(MTLRenderPassDescriptor* rp, const TargetView& v, std::uint32_t i, MTLLoadAction load,
                MTLStoreAction store, const std::array<float, 4>& clear);
    void note_target(MetalTexture* t) {
        if (t && !t->tracked()) rp_targets_.push_back(t);
    }
    void begin_render(MTLRenderPassDescriptor* rp, std::uint32_t w, std::uint32_t h);
    id<MTLBlitCommandEncoder> blit();
    id<MTLComputeCommandEncoder> compute();
    bool ensure_render();
    void draw(const CmdDraw& d);
    void draw_rect(const CmdDraw& d, const DrawState& s, MetalRenderPipeline* p);
    void draw_quads(const CmdDraw& d, const DrawState& s, MetalRenderPipeline* p);
    void set_fixed_state(const DrawState& s, const RasterDesc& r, id<MTLDepthStencilState> ds, bool d24_fragment);
    bool bind_common(const DrawState& s, MetalRenderPipeline* p, bool rect);
    bool bind_tables(const DrawState& s, const StagePlan& plan, int stage);
    std::vector<std::uint8_t> push_block(const DrawState& s, const RootLayout* fallback) const;
    bool discard_follows(const gpu::Resource* r) const;
    void dispatch(const CmdDispatch& c);
    bool bind_compute(const DrawState& s, MetalComputePipeline* p);
    void draw_mesh(const CmdDrawMesh& c);
    bool bind_mesh(const DrawState& s, MetalMeshPipeline* p);
    void indirect(const CmdIndirect& c);
    void query(const CmdQuery& c);
    void resolve(const CmdResolveQueries& c);
    void sample_timestamp(id<MTLCounterSampleBuffer> buffer, std::uint32_t index);
    void write_timestamp(const CmdWriteTimestamp& c);
    void split_after_completion();
    void barrier(const CmdBarrier& c);
    void scan_prepasses(std::size_t from, bool explicit_pass);
    bool needs_prepass(const Cmd& c) const;
    Prepared run_prepass(const Cmd& c);
    Prepared count_prepass(const IndirectDesc& d);
    Prepared groups_prepass(const IndirectDesc& d);
    Prepared rect_indirect_prepass(const IndirectDesc& d, const DrawState& s);
    void indirect_rect(const CmdIndirect& c, MetalRenderPipeline* p);
    Prepared quad_prepass(const CmdDraw& d, const DrawState& s, bool* ok);
    id<MTLBuffer> scratch(std::uint64_t bytes, std::uint64_t* offset);
    void keep(id object) {
        if (object) [kept_ addObject:object];
    }
    // Encoder labels and the command buffer's summary (GTA6RT_METAL_ENCODER_STATUS, on by default): what a failed
    // command buffer's encoder infos and the slow-command-buffer line name.
    static bool labels_on() {
        static const bool on = [] {
            const char* v = std::getenv("GTA6RT_METAL_ENCODER_STATUS");
            return !v || std::strcmp(v, "0") != 0;
        }();
        return on;
    }
    void begin_label(const char* kind) {
        label_kind_ = kind;
        label_work_ = 0;
        label_first_ = label_last_ = nil;
    }
    void note_work(id pso = nil) {
        ++label_work_;
        if (!pso) return;
        if (!label_first_) label_first_ = pso;
        label_last_ = pso;
    }
    void finish_label(id<MTLCommandEncoder> enc) {
        if (!labels_on() || !label_kind_) return;
        NSString* first = label_first_ ? [(id)label_first_ label] : nil;
        NSString* last = label_last_ && label_last_ != label_first_ ? [(id)label_last_ label] : nil;
        char text[512];
        std::snprintf(text, sizeof text, "e%u %s x%u%s%s%s%s", label_index_, label_kind_, label_work_,
                      first ? " " : "", first ? first.UTF8String : "", last ? " .. " : "", last ? last.UTF8String : "");
        enc.label = @(text);
        if (summary_.size() < 1800) {
            summary_ += summary_.empty() ? "" : "; ";
            summary_ += text;
        }
        ++label_index_;
        label_kind_ = nullptr;
    }

    MetalQueue* q_;
    const char* label_kind_ = nullptr;
    std::uint32_t label_work_ = 0, label_index_ = 0;
    id label_first_ = nil, label_last_ = nil;
    std::string summary_;
    MetalDevice* d_;
    id<MTLCommandBuffer> cb_ = nil;
    id<MTLRenderCommandEncoder> render_ = nil;
    id<MTLBlitCommandEncoder> blit_ = nil;
    id<MTLComputeCommandEncoder> compute_ = nil;
    bool compute_joined_ = false;
    bool render_mesh_ = false;
    bool explicit_ = false;
    CmdSetTargets bound_{};
    std::array<TargetView, 8> pass_targets_{};
    std::uint32_t pass_count_ = 0;
    TargetView pass_depth_{};
    bool pass_has_depth_ = false, pass_depth_stencil_ = false;
    std::uint32_t pass_w_ = 0, pass_h_ = 0;
    std::vector<PendingClear> pending_;
    std::unordered_set<const gpu::Resource*> discarded_;
    bool join_ = true;
    id<MTLFence> update_ = nil;
    id<MTLFence> prepass_fence_ = nil;
    std::vector<const void*> rp_targets_;
    std::vector<MetalDescriptorHeap*> cb_tables_;
    NSMutableArray* kept_ = [NSMutableArray new];
    id<MTLBuffer> constants_ = nil;
    std::uint32_t next_constant_ = 0;
    id<MTLCounterSampleBuffer> stamps_ = nil;
    std::uint32_t next_stamp_ = 0;
    std::vector<std::pair<MetalBuffer*, std::uint64_t>> stamp_targets_;
    id<MTLBuffer> scratch_ = nil;
    std::uint64_t scratch_used_ = 0;
    std::uint64_t scratch_bytes_ = 0;
    std::unordered_map<const void*, Prepared> prepared_;
    MetalQueryHeap* pass_vis_ = nullptr;
    ActiveQuery active_;
    bool span_logged_ = false;
    const std::vector<Cmd>* cmds_ = nullptr;
    std::size_t pos_ = 0;
    std::vector<bool> event_on_cb_;
    Status status_ = Status::ok;
};

Status Encoder::run(std::span<gpu::CommandList* const> lists) {
    std::size_t values = 0, stamps = 0;
    for (gpu::CommandList* l : lists) {
        auto* ml = static_cast<MetalCommandList*>(l);
        if (!ml || !ml->closed()) return Status::invalid_argument;
        for (const Cmd& c : ml->commands()) {
            values += std::holds_alternative<CmdWriteValue>(c);
            stamps += std::holds_alternative<CmdWriteTimestamp>(c);
        }
    }
    if (values) constants_ = [d_->mtl() newBufferWithLength:values * 8 options:MTLResourceStorageModeShared];
    if (stamps && d_->timestamp_counters()) {
        MTLCounterSampleBufferDescriptor* sd = [MTLCounterSampleBufferDescriptor new];
        sd.counterSet = d_->timestamp_counters();
        sd.sampleCount = stamps;
        sd.storageMode = MTLStorageModeShared;
        NSError* e = nil;
        stamps_ = [d_->mtl() newCounterSampleBufferWithDescriptor:sd error:&e];
        if (!stamps_) log("write_timestamp: %s", e.localizedDescription.UTF8String);
    }
    cb_ = q_->begin_command_buffer();
    for (gpu::CommandList* l : lists) {
        auto* ml = static_cast<MetalCommandList*>(l);
        cmds_ = &ml->commands();
        for (pos_ = 0; pos_ < cmds_->size(); ++pos_) visit((*cmds_)[pos_]);
        end_encoder();
        flush_clears();
        explicit_ = false;
        bound_ = {};
        active_ = {};
        prepared_.clear();
    }
    if (!stamp_targets_.empty()) {
        split_after_completion();
        for (std::size_t k = 0; k < stamp_targets_.size(); ++k)
            [blit() resolveCounters:stamps_ inRange:NSMakeRange(k, 1) destinationBuffer:stamp_targets_[k].first->mtl()
                  destinationOffset:stamp_targets_[k].second];
    }
    close_command_buffer();
    return status_;
}

// Counter samples reach memory some time after their encoder completes, and an MTLFence does not cover them
// (measured: a resolve in the same command buffer read 0 for the last samples, with or without a fence). A resolve
// therefore goes into a new command buffer that waits for the previous one to complete (DESIGN.md 3.8).
void Encoder::split_after_completion() {
    end_encoder();
    flush_clears();
    close_command_buffer();
    const std::uint64_t serial = q_->last_serial();
    cb_ = q_->begin_command_buffer();
    for (std::size_t i = 0; i < event_on_cb_.size(); ++i)
        if (event_on_cb_[i]) [cb_ pushDebugGroup:@"(continued)"];
    [cb_ encodeWaitForEvent:q_->timeline() value:serial];
    join_ = true;
}

void Encoder::close_command_buffer() {
    end_encoder();
    for (std::size_t i = event_on_cb_.size(); i-- > 0;)
        if (event_on_cb_[i]) [cb_ popDebugGroup];
    if (d_->table_residency()) {
        std::unordered_set<MetalDescriptorHeap*> seen;
        for (MetalDescriptorHeap* h : cb_tables_) {
            if (!seen.insert(h).second) continue;
            if (id<MTLResidencySet> rs = h->residency_for_submit()) [cb_ useResidencySet:rs];
        }
    }
    cb_tables_.clear();
    if (scratch_) {
        const std::uint64_t bytes = scratch_.allocatedSize;
        d_->ledger().add(MemoryCategory::internal, bytes);
        MetalDevice* dev = d_;
        [cb_ addCompletedHandler:^(id<MTLCommandBuffer>) {
          dev->ledger().remove(MemoryCategory::internal, bytes);
        }];
        scratch_ = nil;
        scratch_used_ = scratch_bytes_ = 0;
    }
    if (kept_.count) {
        NSMutableArray* hold = kept_;
        [cb_ addCompletedHandler:^(id<MTLCommandBuffer>) {
          [hold removeAllObjects];
        }];
        kept_ = [NSMutableArray new];
    }
    q_->end_command_buffer(cb_, {}, std::move(summary_));
    summary_.clear();
    label_index_ = 0;
    cb_ = nil;
}

id<MTLBuffer> Encoder::scratch(std::uint64_t bytes, std::uint64_t* offset) {
    bytes = (bytes + 255) & ~std::uint64_t(255);
    if (!scratch_ || scratch_used_ + bytes > scratch_bytes_) {
        if (scratch_) {
            const std::uint64_t old = scratch_.allocatedSize;
            d_->ledger().add(MemoryCategory::internal, old);
            MetalDevice* dev = d_;
            [cb_ addCompletedHandler:^(id<MTLCommandBuffer>) {
              dev->ledger().remove(MemoryCategory::internal, old);
            }];
        }
        scratch_bytes_ = std::max<std::uint64_t>(bytes, 1ull << 20);
        scratch_ = [d_->mtl() newBufferWithLength:scratch_bytes_ options:MTLResourceStorageModeShared];
        scratch_used_ = 0;
        keep(scratch_);
    }
    *offset = scratch_used_;
    scratch_used_ += bytes;
    return scratch_;
}

void Encoder::barrier(const CmdBarrier& c) {
    bool fenced = false;
    for (const Barrier& b : c.barriers) {
        if (barrier_is_noop(b)) {
            d_->counters().barriers_noop++;
        } else {
            d_->counters().barriers_fenced++;
            fenced = true;
        }
    }
    if (!fenced) return;
    if (compute_ && compute_joined_ && pending_.empty()) {
        [compute_ memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
        d_->counters().barriers_in_encoder++;
        join_ = true;
        return;
    }
    end_encoder();
    flush_clears();
    join_ = true;
}

void Encoder::visit(const Cmd& cmd) {
    if (auto* c = std::get_if<CmdBeginPass>(&cmd)) {
        end_encoder();
        flush_clears();
        scan_prepasses(pos_ + 1, true);
        open_explicit(c->desc);
    } else if (std::holds_alternative<CmdEndPass>(cmd)) {
        end_encoder();
        explicit_ = false;
    } else if (auto* c = std::get_if<CmdSetTargets>(&cmd)) {
        if (render_ && !explicit_) {
            bool same = c->count == pass_count_ && c->has_depth == pass_has_depth_ &&
                        (!c->has_depth || same_view(c->depth, pass_depth_));
            for (std::uint32_t i = 0; same && i < c->count; ++i) same = same_view(c->targets[i], pass_targets_[i]);
            if (!same) end_encoder();
        }
        bound_ = *c;
    } else if (auto* c = std::get_if<CmdClearTarget>(&cmd)) {
        if (render_) end_encoder();
        std::erase_if(pending_, [&](const PendingClear& p) { return p.color && same_view(p.view, c->target); });
        PendingClear p;
        p.view = c->target;
        p.color = true;
        p.rgba = c->color;
        pending_.push_back(p);
    } else if (auto* c = std::get_if<CmdClearDepth>(&cmd)) {
        if (render_) end_encoder();
        PendingClear p;
        p.view = c->target;
        p.depth = c->depth;
        p.depth_value = c->depth_value;
        p.stencil = c->stencil;
        p.stencil_value = c->stencil_value;
        auto* t = static_cast<MetalTexture*>(c->target.texture);
        if (t && t->format().info.emulated && t->format().info.depth) p.depth_value = quantize_d24(p.depth_value);
        std::erase_if(pending_, [&](const PendingClear& o) { return !o.color && same_view(o.view, c->target); });
        pending_.push_back(p);
    } else if (auto* c = std::get_if<CmdDiscard>(&cmd)) {
        bool attached = pass_has_depth_ && pass_depth_.texture == c->resource;
        for (std::uint32_t i = 0; i < pass_count_; ++i) attached |= pass_targets_[i].texture == c->resource;
        if (render_ && !explicit_ && attached) end_encoder();
        discarded_.insert(c->resource);
    } else if (auto* c = std::get_if<CmdDraw>(&cmd)) {
        draw(*c);
    } else if (auto* c = std::get_if<CmdCopyBuffer>(&cmd)) {
        if (render_) end_encoder();
        flush_clears();
        [blit() copyFromBuffer:c->src->mtl()
                   sourceOffset:c->src_offset
                       toBuffer:c->dst->mtl()
              destinationOffset:c->dst_offset
                           size:c->size];
    } else if (auto* c = std::get_if<CmdCopyBufferTexture>(&cmd)) {
        if (render_) end_encoder();
        flush_clears();
        const BufferTextureCopy& k = c->copy;
        auto* t = static_cast<MetalTexture*>(k.texture);
        auto* b = static_cast<MetalBuffer*>(k.buffer);
        const std::uint32_t w = k.width ? k.width : std::max(1u, t->desc().width >> k.mip);
        const std::uint32_t h = k.height ? k.height : std::max(1u, t->desc().height >> k.mip);
        const FormatInfo& fi = t->format().info;
        std::uint32_t pitch = k.row_pitch;
        if (!pitch) pitch = (k.depth_plane ? 4 : fi.block_bytes) * ((w + fi.block_dim - 1) / fi.block_dim);
        const std::uint32_t rows = (h + fi.block_dim - 1) / fi.block_dim;
        const MTLBlitOption opt = k.depth_plane ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
        const bool volume = t->mtl().textureType == MTLTextureType3D;
        const NSUInteger slice = volume ? 0 : k.layer;
        const NSUInteger z = volume ? k.layer : 0;
        if (c->to_texture) {
            discarded_.erase(k.texture);
            [blit() copyFromBuffer:b->mtl()
                       sourceOffset:k.offset
                  sourceBytesPerRow:pitch
                sourceBytesPerImage:std::size_t(pitch) * rows
                         sourceSize:MTLSizeMake(w, h, 1)
                          toTexture:t->mtl()
                   destinationSlice:slice
                   destinationLevel:k.mip
                  destinationOrigin:MTLOriginMake(k.x, k.y, z)
                            options:opt];
        } else {
            [blit() copyFromTexture:t->mtl()
                             sourceSlice:slice
                             sourceLevel:k.mip
                            sourceOrigin:MTLOriginMake(k.x, k.y, z)
                              sourceSize:MTLSizeMake(w, h, 1)
                                toBuffer:b->mtl()
                       destinationOffset:k.offset
                  destinationBytesPerRow:pitch
                destinationBytesPerImage:std::size_t(pitch) * rows
                                 options:opt];
        }
    } else if (auto* c = std::get_if<CmdCopyTexture>(&cmd)) {
        if (render_) end_encoder();
        flush_clears();
        const TextureCopy& k = c->copy;
        auto* src = static_cast<MetalTexture*>(k.src);
        auto* dst = static_cast<MetalTexture*>(k.dst);
        const bool src3 = src->mtl().textureType == MTLTextureType3D, dst3 = dst->mtl().textureType == MTLTextureType3D;
        discarded_.erase(k.dst);
        [blit() copyFromTexture:src->mtl()
                     sourceSlice:src3 ? 0 : k.src_layer
                     sourceLevel:k.src_mip
                    sourceOrigin:MTLOriginMake(k.src_x, k.src_y, src3 ? k.src_layer : 0)
                      sourceSize:MTLSizeMake(k.width, k.height, k.depth)
                       toTexture:dst->mtl()
                destinationSlice:dst3 ? 0 : k.dst_layer
                destinationLevel:k.dst_mip
               destinationOrigin:MTLOriginMake(k.dst_x, k.dst_y, dst3 ? k.dst_layer : 0)];
        keep(src->mtl());
        keep(dst->mtl());
    } else if (auto* c = std::get_if<CmdFillU32>(&cmd)) {
        if (render_) end_encoder();
        flush_clears();
        const std::uint32_t byte = c->value & 0xffu;
        if (c->value == byte * 0x01010101u) {
            [blit() fillBuffer:c->buffer->mtl() range:NSMakeRange(c->offset, c->size) value:std::uint8_t(byte)];
        } else {
            id<MTLComputeCommandEncoder> e = compute();
            id<MTLComputePipelineState> k = d_->builtin_kernel("gta6rt_fill_u32");
            [e setComputePipelineState:k];
            note_work(k);
            const std::uint32_t words = std::uint32_t(c->size / 4);
            const std::uint32_t params[2] = {c->value, words};
            [e setBuffer:c->buffer->mtl() offset:c->offset atIndex:0];
            [e setBytes:params length:sizeof params atIndex:1];
            [e dispatchThreads:MTLSizeMake(words, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(std::min<std::uint32_t>(64, words), 1, 1)];
        }
    } else if (auto* c = std::get_if<CmdBarrier>(&cmd)) {
        barrier(*c);
    } else if (auto* c = std::get_if<CmdWriteValue>(&cmd)) {
        if (render_) end_encoder();
        flush_clears();
        const std::uint32_t slot = next_constant_++;
        static_cast<std::uint64_t*>(constants_.contents)[slot] = c->value;
        [blit() copyFromBuffer:constants_
                   sourceOffset:slot * 8
                       toBuffer:c->buffer->mtl()
              destinationOffset:c->offset
                           size:c->bytes];
        d_->counters().value_writes++;
    } else if (auto* c = std::get_if<CmdWaitValue>(&cmd)) {
        end_encoder();
        flush_clears();
        close_command_buffer();
        id<MTLSharedEvent> gate = [d_->mtl() newSharedEvent];
        cb_ = q_->begin_command_buffer();
        for (std::size_t i = 0; i < event_on_cb_.size(); ++i)
            if (event_on_cb_[i]) [cb_ pushDebugGroup:@"(continued)"];
        [cb_ encodeWaitForEvent:gate value:1];
        auto* base = static_cast<const volatile std::uint8_t*>(c->buffer->cpu_pointer());
        d_->pump().add(base + c->offset, c->bytes, c->value, c->compare, gate, 1);
        d_->counters().value_waits++;
        join_ = true;
    } else if (auto* c = std::get_if<CmdBeginEvent>(&cmd)) {
        NSString* label = @(c->label.c_str());
        if (render_) {
            [render_ insertDebugSignpost:label];
            event_on_cb_.push_back(false);
        } else if (blit_) {
            [blit_ insertDebugSignpost:label];
            event_on_cb_.push_back(false);
        } else if (compute_) {
            [compute_ insertDebugSignpost:label];
            event_on_cb_.push_back(false);
        } else {
            [cb_ pushDebugGroup:label];
            event_on_cb_.push_back(true);
        }
    } else if (std::holds_alternative<CmdEndEvent>(cmd)) {
        if (!event_on_cb_.empty()) {
            const bool on_cb = event_on_cb_.back();
            event_on_cb_.pop_back();
            if (on_cb) {
                end_encoder();
                [cb_ popDebugGroup];
            }
        }
    } else if (auto* c = std::get_if<CmdDispatch>(&cmd)) {
        dispatch(*c);
    } else if (auto* c = std::get_if<CmdDrawMesh>(&cmd)) {
        draw_mesh(*c);
    } else if (auto* c = std::get_if<CmdIndirect>(&cmd)) {
        indirect(*c);
    } else if (auto* c = std::get_if<CmdQuery>(&cmd)) {
        query(*c);
    } else if (auto* c = std::get_if<CmdResolveQueries>(&cmd)) {
        resolve(*c);
    } else if (auto* c = std::get_if<CmdWriteTimestamp>(&cmd)) {
        write_timestamp(*c);
    }
}

id<MTLBlitCommandEncoder> Encoder::blit() {
    if (render_ || compute_) end_encoder();
    if (!blit_) {
        blit_ = [cb_ blitCommandEncoder];
        const EncoderFences f = q_->open_encoder(join_ ? MetalQueue::Join::join : MetalQueue::Join::none, {});
        join_ = false;
        for (std::uint32_t i = 0; i < f.wait_count; ++i) [blit_ waitForFence:f.wait[i]];
        update_ = f.update;
        d_->counters().blit_encoders++;
        begin_label("blit");
    }
    note_work();
    return blit_;
}

id<MTLComputeCommandEncoder> Encoder::compute() {
    if (render_ || blit_) end_encoder();
    if (!compute_) {
        compute_ = [cb_ computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
        const EncoderFences f = q_->open_encoder(join_ ? MetalQueue::Join::join : MetalQueue::Join::none, {});
        join_ = false;
        for (std::uint32_t i = 0; i < f.wait_count; ++i) [compute_ waitForFence:f.wait[i]];
        update_ = f.update;
        compute_joined_ = f.joined;
        d_->counters().compute_encoders++;
        begin_label("compute");
    }
    return compute_;
}

bool Encoder::discard_follows(const gpu::Resource* r) const {
    if (!cmds_) return false;
    for (std::size_t i = pos_; i < cmds_->size(); ++i) {
        const Cmd& c = (*cmds_)[i];
        if (auto* d = std::get_if<CmdDiscard>(&c)) {
            if (d->resource == r) return true;
            continue;
        }
        if (std::holds_alternative<CmdSetTargets>(c) || std::holds_alternative<CmdBarrier>(c) ||
            std::holds_alternative<CmdBeginEvent>(c) || std::holds_alternative<CmdEndEvent>(c) ||
            std::holds_alternative<CmdEndPass>(c))
            continue;
        return false;
    }
    return false;
}

void Encoder::end_encoder() {
    if (render_) {
        if (!explicit_) {
            for (std::uint32_t i = 0; i < pass_count_; ++i) {
                if (!pass_targets_[i].texture) continue;
                const bool drop = discard_follows(pass_targets_[i].texture);
                if (drop) d_->counters().stores_discarded++;
                [render_ setColorStoreAction:drop ? MTLStoreActionDontCare : MTLStoreActionStore atIndex:i];
            }
            if (pass_has_depth_) {
                const bool drop = discard_follows(pass_depth_.texture);
                if (drop) d_->counters().stores_discarded++;
                [render_ setDepthStoreAction:drop ? MTLStoreActionDontCare : MTLStoreActionStore];
                if (pass_depth_stencil_)
                    [render_ setStencilStoreAction:drop ? MTLStoreActionDontCare : MTLStoreActionStore];
            }
        }
        if (active_.heap && pass_vis_ == active_.heap && !span_logged_) {
            log("occlusion query open across render passes: the result counts the last pass only");
            span_logged_ = true;
        }
        [render_ updateFence:update_
                 afterStages:kAllRenderStages | (render_mesh_ ? MTLRenderStageObject | MTLRenderStageMesh : 0)];
        finish_label(render_);
        [render_ endEncoding];
        render_mesh_ = false;
        render_ = nil;
        update_ = nil;
        pass_vis_ = nullptr;
        prepass_fence_ = nil;
    }
    if (blit_) {
        [blit_ updateFence:update_];
        finish_label(blit_);
        [blit_ endEncoding];
        blit_ = nil;
        update_ = nil;
    }
    if (compute_) {
        [compute_ updateFence:update_];
        finish_label(compute_);
        [compute_ endEncoding];
        compute_ = nil;
        update_ = nil;
        compute_joined_ = false;
    }
}

void Encoder::attach(MTLRenderPassDescriptor* rp, const TargetView& v, std::uint32_t i, MTLLoadAction load,
                     MTLStoreAction store, const std::array<float, 4>& clear) {
    auto* t = static_cast<MetalTexture*>(v.texture);
    MTLRenderPassColorAttachmentDescriptor* ca = rp.colorAttachments[i];
    ca.texture = t->mtl();
    ca.level = v.mip;
    ca.slice = v.layer;
    ca.loadAction = load;
    ca.storeAction = store;
    ca.clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
    keep(t->mtl());
    note_target(t);
}

void Encoder::begin_render(MTLRenderPassDescriptor* rp, std::uint32_t w, std::uint32_t h) {
    if (blit_ || compute_) end_encoder();
    MetalQueryHeap* vis = active_.heap ? active_.heap : pass_vis_;
    if (vis) {
        rp.visibilityResultBuffer = vis->results();
        keep(vis->results());
    }
    id<MTLFence> extra = prepass_fence_;
    render_ = [cb_ renderCommandEncoderWithDescriptor:rp];
    pass_vis_ = vis;
    pass_w_ = w;
    pass_h_ = h;
    const EncoderFences f = q_->open_encoder(join_ ? MetalQueue::Join::join : MetalQueue::Join::none, rp_targets_);
    rp_targets_.clear();
    join_ = false;
    for (std::uint32_t i = 0; i < f.wait_count; ++i) [render_ waitForFence:f.wait[i] beforeStages:kWaitStages];
    if (extra) [render_ waitForFence:extra beforeStages:kWaitStages];
    prepass_fence_ = nil;
    update_ = f.update;
    d_->counters().render_encoders++;
    begin_label("render");
    if (active_.heap && pass_vis_ == active_.heap)
        [render_ setVisibilityResultMode:active_.heap->type() == QueryType::binary_occlusion ? MTLVisibilityResultModeBoolean
                                                                                             : MTLVisibilityResultModeCounting
                                  offset:active_.index * 8];
}

static MTLLoadAction load_action(LoadOp op) {
    switch (op) {
    case LoadOp::clear: return MTLLoadActionClear;
    case LoadOp::preserve: return MTLLoadActionLoad;
    default: return MTLLoadActionDontCare;
    }
}

static MTLStoreAction store_action(StoreOp op) {
    switch (op) {
    case StoreOp::preserve:
    case StoreOp::resolve: return MTLStoreActionStore;
    default: return MTLStoreActionDontCare;
    }
}

void Encoder::open_explicit(const RenderPassDesc& desc) {
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    std::uint32_t w = 0, h = 0;
    pass_count_ = desc.target_count;
    for (std::uint32_t i = 0; i < desc.target_count; ++i) {
        const RenderPassTarget& t = desc.targets[i];
        pass_targets_[i] = t.view;
        if (!t.view.texture) continue;
        if (t.store == StoreOp::resolve) {
            log("render pass: StoreOp::resolve has no resolve target in gpu.h; stored instead");
            status_ = Status::not_implemented;
        }
        discarded_.erase(t.view.texture);
        attach(rp, t.view, i, load_action(t.load), store_action(t.store), t.clear);
        w = std::max(1u, t.view.texture->desc().width >> t.view.mip);
        h = std::max(1u, t.view.texture->desc().height >> t.view.mip);
    }
    pass_has_depth_ = desc.depth.texture != nullptr;
    pass_depth_ = desc.depth;
    if (desc.depth.texture) {
        auto* t = static_cast<MetalTexture*>(desc.depth.texture);
        if (desc.depth_store == StoreOp::resolve) {
            log("render pass: depth StoreOp::resolve has no resolve target in gpu.h; stored instead");
            status_ = Status::not_implemented;
        }
        discarded_.erase(t);
        note_target(t);
        rp.depthAttachment.texture = t->mtl();
        rp.depthAttachment.loadAction = load_action(desc.depth_load);
        rp.depthAttachment.storeAction = store_action(desc.depth_store);
        rp.depthAttachment.clearDepth =
            t->format().info.emulated ? quantize_d24(desc.clear_depth) : desc.clear_depth;
        pass_depth_stencil_ = t->format().info.stencil;
        if (pass_depth_stencil_) {
            rp.stencilAttachment.texture = t->mtl();
            rp.stencilAttachment.loadAction = load_action(desc.depth_load);
            rp.stencilAttachment.storeAction = store_action(desc.depth_store);
            rp.stencilAttachment.clearStencil = desc.clear_stencil;
        }
        w = t->desc().width;
        h = t->desc().height;
        keep(t->mtl());
    }
    explicit_ = true;
    begin_render(rp, w, h);
}

void Encoder::open_implicit() {
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    std::uint32_t w = 0, h = 0;
    auto take_clear = [&](const TargetView& v, bool color) -> std::optional<PendingClear> {
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->color == color && same_view(it->view, v)) {
                PendingClear p = *it;
                pending_.erase(it);
                d_->counters().clears_folded++;
                return p;
            }
        }
        return std::nullopt;
    };
    pass_count_ = bound_.count;
    for (std::uint32_t i = 0; i < bound_.count; ++i) {
        const TargetView& v = bound_.targets[i];
        pass_targets_[i] = v;
        if (!v.texture) continue;
        MTLLoadAction load = MTLLoadActionLoad;
        std::array<float, 4> rgba{};
        const bool was_discarded = discarded_.erase(v.texture) != 0;
        if (auto p = take_clear(v, true)) {
            load = MTLLoadActionClear;
            rgba = p->rgba;
        } else if (was_discarded) {
            load = MTLLoadActionDontCare;
        }
        attach(rp, v, i, load, MTLStoreActionUnknown, rgba);
        w = std::max(1u, v.texture->desc().width >> v.mip);
        h = std::max(1u, v.texture->desc().height >> v.mip);
    }
    pass_has_depth_ = bound_.has_depth;
    pass_depth_ = bound_.depth;
    pass_depth_stencil_ = false;
    if (bound_.has_depth) {
        auto* t = static_cast<MetalTexture*>(bound_.depth.texture);
        note_target(t);
        const bool was_discarded = discarded_.erase(t) != 0;
        rp.depthAttachment.texture = t->mtl();
        rp.depthAttachment.storeAction = MTLStoreActionUnknown;
        rp.depthAttachment.loadAction = MTLLoadActionLoad;
        pass_depth_stencil_ = t->format().info.stencil;
        if (pass_depth_stencil_) {
            rp.stencilAttachment.texture = t->mtl();
            rp.stencilAttachment.storeAction = MTLStoreActionUnknown;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad;
        }
        if (auto p = take_clear(bound_.depth, false)) {
            if (p->depth) {
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = p->depth_value;
            }
            if (p->stencil && pass_depth_stencil_) {
                rp.stencilAttachment.loadAction = MTLLoadActionClear;
                rp.stencilAttachment.clearStencil = p->stencil_value;
            }
        } else if (was_discarded) {
            rp.depthAttachment.loadAction = MTLLoadActionDontCare;
            if (pass_depth_stencil_) rp.stencilAttachment.loadAction = MTLLoadActionDontCare;
        }
        w = t->desc().width;
        h = t->desc().height;
        keep(t->mtl());
    }
    explicit_ = false;
    begin_render(rp, w, h);
}

void Encoder::flush_clears() {
    if (pending_.empty()) return;
    std::vector<PendingClear> clears = std::move(pending_);
    pending_.clear();
    const ActiveQuery saved = active_;
    active_ = {};
    for (const PendingClear& p : clears) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        auto* t = static_cast<MetalTexture*>(p.view.texture);
        discarded_.erase(t);
        if (p.color) {
            attach(rp, p.view, 0, MTLLoadActionClear, MTLStoreActionStore, p.rgba);
        } else {
            note_target(t);
            rp.depthAttachment.texture = t->mtl();
            rp.depthAttachment.loadAction = p.depth ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = p.depth_value;
            if (t->format().info.stencil) {
                rp.stencilAttachment.texture = t->mtl();
                rp.stencilAttachment.loadAction = p.stencil ? MTLLoadActionClear : MTLLoadActionLoad;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
                rp.stencilAttachment.clearStencil = p.stencil_value;
            }
            keep(t->mtl());
        }
        const bool was_explicit = explicit_;
        explicit_ = true;
        pass_count_ = 0;
        pass_has_depth_ = false;
        begin_render(rp, t->desc().width, t->desc().height);
        end_encoder();
        explicit_ = was_explicit;
    }
    active_ = saved;
}

std::vector<std::uint8_t> Encoder::push_block(const DrawState& s, const RootLayout* fallback) const {
    const MetalRootLayout* root = s.root ? s.root : static_cast<const MetalRootLayout*>(fallback);
    if (!root) return {};
    const RootLayoutDesc& rd = root->desc();
    std::vector<std::uint8_t> block(rd.push_bytes, 0);
    std::memcpy(block.data(), s.user_data.data(), std::min<std::size_t>(rd.user_dwords * 4, rd.push_bytes));
    const std::size_t at = rd.user_dwords * 4;
    if (at + 16 <= block.size()) {
        std::memcpy(block.data() + at, &s.heap_base, 8);
        std::memcpy(block.data() + at + 8, &s.code_base, 8);
    }
    return block;
}

bool Encoder::bind_tables(const DrawState& s, const StagePlan& plan, int stage) {
    for (auto [binding, index] : plan.tables) {
        MetalDescriptorHeap* h = s.tables[binding];
        if (!h) {
            log("table binding %u is not set", binding);
            return false;
        }
        switch (stage) {
        case 0: [render_ setVertexBuffer:h->mtl() offset:0 atIndex:index]; break;
        case 1: [render_ setFragmentBuffer:h->mtl() offset:0 atIndex:index]; break;
        case 2: [compute_ setBuffer:h->mtl() offset:0 atIndex:index]; break;
        case 3: [render_ setObjectBuffer:h->mtl() offset:0 atIndex:index]; break;
        case 4: [render_ setMeshBuffer:h->mtl() offset:0 atIndex:index]; break;
        }
        cb_tables_.push_back(h);
    }
    return true;
}

void Encoder::set_fixed_state(const DrawState& s, const RasterDesc& r, id<MTLDepthStencilState> ds, bool d24_fragment) {
    [render_ setDepthStencilState:ds];
    [render_ setCullMode:r.cull == CullMode::none ? MTLCullModeNone
                         : r.cull == CullMode::front ? MTLCullModeFront
                                                     : MTLCullModeBack];
    [render_ setFrontFacingWinding:r.front_ccw ? MTLWindingCounterClockwise : MTLWindingClockwise];
    [render_ setTriangleFillMode:r.wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    if (d24_fragment)
        [render_ setDepthBias:0 slopeScale:0 clamp:0];
    else
        [render_ setDepthBias:float(r.depth_bias) slopeScale:r.slope_scaled_depth_bias clamp:r.depth_bias_clamp];
    [render_ setDepthClipMode:r.depth_clip ? MTLDepthClipModeClip : MTLDepthClipModeClamp];
    MTLViewport vp{0, 0, double(pass_w_), double(pass_h_), 0, 1};
    if (s.viewport_set)
        vp = {s.viewport.x, s.viewport.y, s.viewport.width, s.viewport.height, s.viewport.min_depth, s.viewport.max_depth};
    [render_ setViewport:vp];
    MTLScissorRect sc{0, 0, pass_w_, pass_h_};
    if (s.scissor_set) {
        const std::int64_t l = std::clamp<std::int64_t>(s.scissor.left, 0, pass_w_);
        const std::int64_t t = std::clamp<std::int64_t>(s.scissor.top, 0, pass_h_);
        const std::int64_t rr = std::clamp<std::int64_t>(s.scissor.right, l, pass_w_);
        const std::int64_t b = std::clamp<std::int64_t>(s.scissor.bottom, t, pass_h_);
        sc = {NSUInteger(l), NSUInteger(t), NSUInteger(rr - l), NSUInteger(b - t)};
    }
    [render_ setScissorRect:sc];
    [render_ setBlendColorRed:s.blend_factor[0] green:s.blend_factor[1] blue:s.blend_factor[2] alpha:s.blend_factor[3]];
    if (s.separate_stencil_refs)
        [render_ setStencilFrontReferenceValue:s.stencil_ref backReferenceValue:s.stencil_back_ref];
    else
        [render_ setStencilReferenceValue:s.stencil_ref];
}

bool Encoder::bind_common(const DrawState& s, MetalRenderPipeline* p, bool rect) {
    const bool d24 = p->d24_mode() == D24Mode::fragment;
    [render_ setRenderPipelineState:rect ? p->rect_pso() : p->pso()];
    note_work(p->pso());
    set_fixed_state(s, p->desc().raster, p->depth_state(), d24);
    if (d24) {
        const RasterDesc& r = p->desc().raster;
        msl::D24Params dp;
        dp.constant_bias = float(r.depth_bias) * float(1.0 / 16777215.0);
        dp.slope = r.slope_scaled_depth_bias;
        dp.clamp = r.depth_bias_clamp;
        dp.flags = (r.depth_bias != 0 || r.slope_scaled_depth_bias != 0.0f) ? 1u : 0u;
        [render_ setFragmentBytes:&dp length:sizeof(dp) atIndex:std::uint32_t(p->d24_params_index())];
    }
    const std::vector<std::uint8_t> block = push_block(s, p->desc().root);
    if (!block.empty()) {
        if (p->vertex_plan().push_index >= 0)
            [render_ setVertexBytes:block.data() length:block.size() atIndex:p->vertex_plan().push_index];
        if (p->fragment_plan().push_index >= 0)
            [render_ setFragmentBytes:block.data() length:block.size() atIndex:p->fragment_plan().push_index];
    }
    return bind_tables(s, p->vertex_plan(), 0) && bind_tables(s, p->fragment_plan(), 1);
}

bool Encoder::ensure_render() {
    if (render_) return true;
    if (explicit_) {
        log("draw outside a render pass");
        status_ = Status::invalid_argument;
        return false;
    }
    auto bound = [&](const PendingClear& c) {
        for (std::uint32_t i = 0; i < bound_.count; ++i)
            if (c.color && same_view(bound_.targets[i], c.view)) return true;
        return !c.color && bound_.has_depth && same_view(bound_.depth, c.view);
    };
    std::vector<PendingClear> fold, other;
    for (const PendingClear& c : pending_) (bound(c) ? fold : other).push_back(c);
    if (!other.empty()) {
        pending_ = std::move(other);
        flush_clears();
    }
    pending_ = std::move(fold);
    scan_prepasses(pos_, false);
    open_implicit();
    return true;
}

bool Encoder::needs_prepass(const Cmd& c) const {
    if (auto* d = std::get_if<CmdDraw>(&c)) return d->indexed && d->state->topology == topology::quad_list;
    if (auto* i = std::get_if<CmdIndirect>(&c))
        return i->desc.kind != IndirectKind::dispatch &&
               (i->desc.count_buffer || i->desc.kind == IndirectKind::dispatch_mesh ||
                (i->state && i->state->topology == topology::rect_list));
    return false;
}

// Runs the compute pre-passes of the draws a render pass is about to hold before the pass opens, so the pass is
// not split (DESIGN.md 3.6). Inputs are stable over that range: anything that writes ends the pass first.
void Encoder::scan_prepasses(std::size_t from, bool explicit_pass) {
    if (!cmds_) return;
    std::vector<const Cmd*> found;
    for (std::size_t i = from; i < cmds_->size(); ++i) {
        const Cmd& c = (*cmds_)[i];
        if (explicit_pass) {
            if (std::holds_alternative<CmdEndPass>(c)) break;
        } else {
            if (auto* t = std::get_if<CmdSetTargets>(&c)) {
                bool same = t->count == bound_.count && t->has_depth == bound_.has_depth &&
                            (!t->has_depth || same_view(t->depth, bound_.depth));
                for (std::uint32_t k = 0; same && k < t->count; ++k) same = same_view(t->targets[k], bound_.targets[k]);
                if (!same) break;
                continue;
            }
            const bool in_pass = std::holds_alternative<CmdDraw>(c) || std::holds_alternative<CmdDrawMesh>(c) ||
                                 std::holds_alternative<CmdBeginEvent>(c) || std::holds_alternative<CmdEndEvent>(c) ||
                                 (std::holds_alternative<CmdIndirect>(c) &&
                                  std::get<CmdIndirect>(c).desc.kind != IndirectKind::dispatch) ||
                                 (std::holds_alternative<CmdQuery>(c) &&
                                  std::get<CmdQuery>(c).heap->type() != QueryType::timestamp);
            if (!in_pass) break;
        }
        if (auto* q = std::get_if<CmdQuery>(&c)) {
            if (q->begin && !active_.heap && !pass_vis_ &&
                (q->heap->type() == QueryType::occlusion || q->heap->type() == QueryType::binary_occlusion))
                pass_vis_ = q->heap;
        }
        if (needs_prepass(c) && !prepared_.count(&c)) found.push_back(&c);
    }
    if (found.empty()) return;
    for (const Cmd* c : found) {
        prepared_[c] = run_prepass(*c);
        d_->counters().prepasses_hoisted++;
    }
    prepass_fence_ = update_;
    MetalQueryHeap* keep_vis = pass_vis_;
    end_encoder();
    pass_vis_ = keep_vis;
}

Prepared Encoder::run_prepass(const Cmd& c) {
    if (auto* d = std::get_if<CmdDraw>(&c)) {
        bool ok = false;
        return quad_prepass(*d, *d->state, &ok);
    }
    const CmdIndirect& i = std::get<CmdIndirect>(c);
    if (i.desc.kind == IndirectKind::dispatch_mesh) return groups_prepass(i.desc);
    if (i.state && i.state->topology == topology::rect_list) return rect_indirect_prepass(i.desc, *i.state);
    return count_prepass(i.desc);
}

// Mesh and compute indirect records copied with the count applied and any record past the limits zeroed (a mesh
// grid: D3D12's, each dimension at most 65,535 and 2^22 groups; a compute grid: 2^26 groups, Metal grids are 32-bit
// and the title's may pass 65,535): a record the title never wrote (its producer pass skipped while its pipeline
// compiled) otherwise launches billions of groups, and the kernel stops the command buffer
// (kIOGPUCommandBufferCallbackErrorTimeout, d3m-s4-g). GTA6RT_METAL_INDIRECT_LIMITS=0: records as written.
static bool indirect_limits() {
    static const bool on = [] {
        const char* v = std::getenv("GTA6RT_METAL_INDIRECT_LIMITS");
        return !v || std::strcmp(v, "0") != 0;
    }();
    return on;
}

Prepared Encoder::groups_prepass(const IndirectDesc& d) {
    if (!indirect_limits()) return d.count_buffer ? count_prepass(d) : Prepared{};
    Prepared out;
    id<MTLComputePipelineState> k = d_->builtin_kernel("gta6rt_indirect_groups");
    if (!k || !d.max_count) return out;
    out.buffer = scratch(std::uint64_t(d.max_count) * 12, &out.offset);
    struct {
        std::uint32_t stride_words, max_count, count_word, has_count, mesh;
    } params{d.stride / 4, d.max_count, 0, d.count_buffer ? 1u : 0u, d.kind == IndirectKind::dispatch_mesh ? 1u : 0u};
    id<MTLComputeCommandEncoder> e = compute();
    auto* args = static_cast<MetalBuffer*>(d.arguments);
    auto* count = static_cast<MetalBuffer*>(d.count_buffer);
    [e setComputePipelineState:k];
    note_work(k);
    [e setBuffer:args->mtl() offset:d.argument_offset atIndex:0];
    [e setBuffer:count ? count->mtl() : args->mtl() offset:count ? d.count_offset : 0 atIndex:1];
    [e setBuffer:out.buffer offset:out.offset atIndex:2];
    [e setBytes:&params length:sizeof(params) atIndex:3];
    [e dispatchThreads:MTLSizeMake(d.max_count, 1, 1) threadsPerThreadgroup:MTLSizeMake(std::min<std::uint32_t>(64, d.max_count), 1, 1)];
    d_->counters().indirect_prepasses++;
    return out;
}

// Indirect rectangle lists: per record, a Metal draw of (vertex count / 3) * 6 vertices for the rect wrapper and the
// wrapper's parameters (first vertex or index, base vertex, first instance) from the D3D12 record, written by the
// GPU before the pass; the wrapper reads its parameters at params_stride bytes per record.
constexpr std::uint32_t kRectParamsStride = 256;

Prepared Encoder::rect_indirect_prepass(const IndirectDesc& d, const DrawState& s) {
    Prepared out;
    id<MTLComputePipelineState> k = d_->builtin_kernel("gta6rt_rect_indirect");
    if (!k || !d.max_count) return out;
    const std::uint64_t params_at = (std::uint64_t(d.max_count) * 16 + kRectParamsStride - 1) / kRectParamsStride * kRectParamsStride;
    out.buffer = scratch(params_at + std::uint64_t(d.max_count) * kRectParamsStride, &out.offset);
    struct {
        std::uint32_t src_word, stride_words, max_count, count_word, has_count, indexed, index_bytes, params_stride_words;
        std::uint32_t params_word;
        std::uint32_t strides[8];
    } params{};
    params.params_word = std::uint32_t(params_at / 4);
    params.stride_words = d.stride / 4;
    params.max_count = d.max_count;
    params.has_count = d.count_buffer ? 1u : 0u;
    params.indexed = d.kind == IndirectKind::draw_indexed ? 1u : 0u;
    params.index_bytes = s.index.format == format::r32_uint ? 4u : 2u;
    params.params_stride_words = kRectParamsStride / 4;
    for (std::uint32_t slot = 0; slot < 8; ++slot) params.strides[slot] = s.vertex_buffers[slot].stride;
    id<MTLComputeCommandEncoder> e = compute();
    auto* args = static_cast<MetalBuffer*>(d.arguments);
    auto* count = static_cast<MetalBuffer*>(d.count_buffer);
    [e setComputePipelineState:k];
    note_work(k);
    [e setBuffer:args->mtl() offset:d.argument_offset atIndex:0];
    [e setBuffer:count ? count->mtl() : args->mtl() offset:count ? d.count_offset : 0 atIndex:1];
    [e setBuffer:out.buffer offset:out.offset atIndex:2];
    [e setBytes:&params length:sizeof(params) atIndex:3];
    [e dispatchThreads:MTLSizeMake(d.max_count, 1, 1) threadsPerThreadgroup:MTLSizeMake(std::min<std::uint32_t>(64, d.max_count), 1, 1)];
    d_->counters().indirect_prepasses++;
    return out;
}

void Encoder::indirect_rect(const CmdIndirect& c, MetalRenderPipeline* p) {
    const IndirectDesc& d = c.desc;
    const DrawState& s = *c.state;
    if (p->rect_mode() != RectMode::vertex_expand || !p->rect_pso()) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) log("indirect rect-list draw skipped: the pipeline has no vertex-stage rect expansion");
        return;
    }
    const Prepared& pr = prepared_[&(*cmds_)[pos_]];
    if (!pr.buffer) {
        status_ = Status::invalid_argument;
        return;
    }
    if (!bind_common(s, p, true)) return;
    const rect::WrapperResult& w = p->rect_wrapper();
    for (std::uint32_t slot = 0; slot < w.vertex_buffer_index.size(); ++slot) {
        const VertexBufferView& v = s.vertex_buffers[slot];
        gpu::Buffer* b = nullptr;
        std::uint64_t off = 0;
        if (!d_->resolve_va(v.va, &b, &off)) continue;
        [render_ setVertexBuffer:static_cast<MetalBuffer*>(b)->mtl() offset:off atIndex:w.vertex_buffer_index[slot]];
    }
    id<MTLBuffer> ib = p->rect_params_dummy();
    std::uint64_t ib_off = 0;
    if (d.kind == IndirectKind::draw_indexed) {
        gpu::Buffer* b = nullptr;
        if (!d_->resolve_va(s.index.va, &b, &ib_off)) {
            status_ = Status::invalid_argument;
            return;
        }
        ib = static_cast<MetalBuffer*>(b)->mtl();
    }
    [render_ setVertexBuffer:ib offset:ib_off atIndex:w.index_buffer_index];
    const std::uint64_t params_base =
        pr.offset + (std::uint64_t(d.max_count) * 16 + kRectParamsStride - 1) / kRectParamsStride * kRectParamsStride;
    for (std::uint32_t i = 0; i < d.max_count; ++i) {
        [render_ setVertexBuffer:pr.buffer offset:params_base + std::uint64_t(i) * kRectParamsStride atIndex:w.params_index];
        [render_ drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:pr.buffer indirectBufferOffset:pr.offset + i * 16];
    }
    d_->counters().indirect_commands += d.max_count;
    d_->counters().rect_draws_expanded += d.max_count;
    gta6rt_hud_add_draws(d.max_count);
}

Prepared Encoder::count_prepass(const IndirectDesc& d) {
    Prepared out;
    id<MTLComputePipelineState> k = d_->builtin_kernel("gta6rt_indirect_count");
    if (!k) return out;
    const std::uint32_t words = kPacked[std::uint32_t(d.kind) & 3] / 4;
    out.buffer = scratch(std::uint64_t(d.max_count) * words * 4, &out.offset);
    struct {
        std::uint32_t src_word, stride_words, max_count, count_word, words, has_count;
    } params{0, d.stride / 4, d.max_count, 0, words, d.count_buffer ? 1u : 0u};
    id<MTLComputeCommandEncoder> e = compute();
    auto* args = static_cast<MetalBuffer*>(d.arguments);
    auto* count = static_cast<MetalBuffer*>(d.count_buffer);
    [e setComputePipelineState:k];
    note_work(k);
    [e setBuffer:args->mtl() offset:d.argument_offset atIndex:0];
    [e setBuffer:count ? count->mtl() : args->mtl() offset:count ? d.count_offset : 0 atIndex:1];
    [e setBuffer:out.buffer offset:out.offset atIndex:2];
    [e setBytes:&params length:sizeof(params) atIndex:3];
    [e dispatchThreads:MTLSizeMake(d.max_count, 1, 1) threadsPerThreadgroup:MTLSizeMake(std::min<std::uint32_t>(64, d.max_count), 1, 1)];
    d_->counters().indirect_prepasses++;
    return out;
}

Prepared Encoder::quad_prepass(const CmdDraw& d, const DrawState& s, bool* ok) {
    Prepared out;
    *ok = false;
    id<MTLComputePipelineState> k = d_->builtin_kernel("gta6rt_quad_expand");
    gpu::Buffer* ib = nullptr;
    std::uint64_t off = 0;
    if (!k || !d_->resolve_va(s.index.va, &ib, &off)) {
        log("quad list: index buffer VA 0x%llx not resolved", (unsigned long long)s.index.va);
        return out;
    }
    const std::uint32_t bytes = s.index.format == format::r32_uint ? 4 : 2;
    const std::uint32_t quads = d.count / 4;
    if (!quads) return out;
    const std::uint64_t first = off + std::uint64_t(d.first) * bytes;
    const std::uint64_t bind = first & ~std::uint64_t(3);
    std::uint64_t readable = std::uint64_t(quads) * 4;
    if (s.index.size) {
        const std::uint64_t view_end = std::uint64_t(s.index.size);
        const std::uint64_t used = std::uint64_t(d.first) * bytes;
        readable = used >= view_end ? 0 : std::min<std::uint64_t>(readable, (view_end - used) / bytes);
    }
    struct {
        std::uint32_t first_byte, index_bytes, readable, quads;
    } params{std::uint32_t(first - bind), bytes, std::uint32_t(readable), quads};
    out.buffer = scratch(std::uint64_t(quads) * 24, &out.offset);
    id<MTLComputeCommandEncoder> e = compute();
    [e setComputePipelineState:k];
    note_work(k);
    [e setBuffer:static_cast<MetalBuffer*>(ib)->mtl() offset:bind atIndex:0];
    [e setBuffer:out.buffer offset:out.offset atIndex:1];
    [e setBytes:&params length:sizeof(params) atIndex:2];
    [e dispatchThreads:MTLSizeMake(quads, 1, 1) threadsPerThreadgroup:MTLSizeMake(std::min<std::uint32_t>(64, quads), 1, 1)];
    d_->counters().indirect_prepasses++;
    *ok = true;
    return out;
}

void Encoder::draw(const CmdDraw& d) {
    gta6rt_hud_add_draws(1);
    const DrawState& s = *d.state;
    MetalRenderPipeline* p = s.pipeline;
    const bool quads = s.topology == topology::quad_list;
    if (quads && d.indexed && render_ && !prepared_.count(&(*cmds_)[pos_])) {
        end_encoder();
        d_->counters().render_pass_splits++;
    }
    if (!ensure_render()) return;
    Status why = Status::ok;
    if (!p) {
        log("draw skipped: pipeline not set");
        status_ = Status::pipeline_failed;
        return;
    }
    if (!p->ready_for_encode(&why)) {
        if (why != Status::not_ready) {
            log("draw skipped: pipeline %s", p->error().c_str());
            status_ = why;
        }
        return;
    }
    if (s.topology == topology::rect_list) {
        if (p->rect_mode() == RectMode::none) {
            log("draw skipped: rectangle list on a pipeline whose topology type is not rectangle");
            status_ = Status::unsupported;
            return;
        }
        draw_rect(d, s, p);
        return;
    }
    if (!quads && s.topology > topology::triangle_strip) {
        log("draw skipped: topology %u is not implemented", s.topology);
        status_ = Status::not_implemented;
        return;
    }
    if (!bind_common(s, p, false)) {
        status_ = Status::invalid_argument;
        return;
    }
    std::vector<bool> slot_done(8, false);
    for (const InputElement& e : p->desc().inputs) {
        if (slot_done[e.slot]) continue;
        slot_done[e.slot] = true;
        const VertexBufferView& v = s.vertex_buffers[e.slot];
        gpu::Buffer* b = nullptr;
        std::uint64_t off = 0;
        if (!d_->resolve_va(v.va, &b, &off)) {
            log("draw: vertex buffer slot %u VA 0x%llx not resolved", e.slot, (unsigned long long)v.va);
            status_ = Status::invalid_argument;
            return;
        }
        [render_ setVertexBuffer:static_cast<MetalBuffer*>(b)->mtl()
                          offset:off
                 attributeStride:v.stride
                         atIndex:p->vertex_buffer_index(e.slot)];
    }
    if (quads) {
        draw_quads(d, s, p);
        return;
    }
    const MTLPrimitiveType type = primitive(s.topology);
    if (d.indexed) {
        gpu::Buffer* b = nullptr;
        std::uint64_t off = 0;
        if (!d_->resolve_va(s.index.va, &b, &off)) {
            log("draw: index buffer VA 0x%llx not resolved", (unsigned long long)s.index.va);
            status_ = Status::invalid_argument;
            return;
        }
        const bool wide = s.index.format == format::r32_uint;
        [render_ drawIndexedPrimitives:type
                            indexCount:d.count
                             indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                           indexBuffer:static_cast<MetalBuffer*>(b)->mtl()
                     indexBufferOffset:off + std::uint64_t(d.first) * (wide ? 4 : 2)
                         instanceCount:d.instances
                            baseVertex:d.base_vertex
                          baseInstance:d.first_instance];
    } else {
        [render_ drawPrimitives:type
                    vertexStart:d.first
                    vertexCount:d.count
                  instanceCount:d.instances
                   baseInstance:d.first_instance];
    }
}

void Encoder::draw_quads(const CmdDraw& d, const DrawState& s, MetalRenderPipeline* p) {
    const std::uint32_t quads = d.count / 4;
    if (!quads) return;
    d_->counters().quad_draws++;
    if (!d.indexed) {
        id<MTLBuffer> pattern = d_->quad_pattern(quads);
        keep(pattern);
        [render_ drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:std::uint64_t(quads) * 6
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:pattern
                     indexBufferOffset:0
                         instanceCount:d.instances
                            baseVertex:std::int64_t(d.first)
                          baseInstance:d.first_instance];
        return;
    }
    auto it = prepared_.find(&(*cmds_)[pos_]);
    if (it == prepared_.end() || !it->second.buffer) {
        log("quad list: index expansion missing");
        status_ = Status::invalid_argument;
        return;
    }
    [render_ drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:std::uint64_t(quads) * 6
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:it->second.buffer
                 indexBufferOffset:it->second.offset
                     instanceCount:d.instances
                        baseVertex:d.base_vertex
                      baseInstance:d.first_instance];
}

void Encoder::draw_rect(const CmdDraw& d, const DrawState& s, MetalRenderPipeline* p) {
    const std::uint32_t rects = d.count / 3;
    if (!rects) return;
    d_->counters().rect_draws_expanded++;
    if (p->rect_mode() == RectMode::vertex_expand && p->rect_pso()) {
        if (!bind_common(s, p, true)) return;
        const rect::WrapperResult& w = p->rect_wrapper();
        rect::WrapperParams params;
        params.first = d.first;
        params.base_vertex = d.base_vertex;
        params.first_instance = d.first_instance;
        for (std::uint32_t slot = 0; slot < w.vertex_buffer_index.size(); ++slot) {
            const VertexBufferView& v = s.vertex_buffers[slot];
            gpu::Buffer* b = nullptr;
            std::uint64_t off = 0;
            if (!d_->resolve_va(v.va, &b, &off)) continue;
            params.strides[slot] = v.stride;
            [render_ setVertexBuffer:static_cast<MetalBuffer*>(b)->mtl() offset:off atIndex:w.vertex_buffer_index[slot]];
        }
        id<MTLBuffer> ib = nil;
        std::uint64_t ib_off = 0;
        if (d.indexed) {
            gpu::Buffer* b = nullptr;
            if (!d_->resolve_va(s.index.va, &b, &ib_off)) return;
            params.indexed = s.index.format == format::r32_uint ? 4 : 2;
            ib = static_cast<MetalBuffer*>(b)->mtl();
        } else {
            ib = p->rect_params_dummy();
        }
        [render_ setVertexBuffer:ib offset:ib_off atIndex:w.index_buffer_index];
        [render_ setVertexBytes:&params length:sizeof(params) atIndex:w.params_index];
        [render_ drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:rects * 6
                  instanceCount:d.instances
                   baseInstance:d.first_instance];
        return;
    }
    const auto& inputs = p->desc().inputs;
    std::vector<rect::CpuElement> elements;
    for (const InputElement& e : inputs) {
        if (e.slot != 0 || e.per_instance) {
            log("rect cpu_rewrite: only per-vertex inputs in slot 0 are expanded");
            status_ = Status::unsupported;
            return;
        }
        elements.push_back({e.offset, e.format});
    }
    const VertexBufferView& v = s.vertex_buffers[0];
    gpu::Buffer* vb = nullptr;
    std::uint64_t voff = 0;
    if (!d_->resolve_va(v.va, &vb, &voff) || !vb->cpu_pointer()) {
        log("rect cpu_rewrite: vertex buffer is not CPU visible");
        status_ = Status::unsupported;
        return;
    }
    std::vector<std::uint32_t> indices;
    if (d.indexed) {
        gpu::Buffer* ib = nullptr;
        std::uint64_t ioff = 0;
        if (!d_->resolve_va(s.index.va, &ib, &ioff) || !ib->cpu_pointer()) {
            status_ = Status::unsupported;
            return;
        }
        const auto* base = static_cast<const std::uint8_t*>(ib->cpu_pointer()) + ioff;
        for (std::uint32_t i = 0; i < rects * 3; ++i) {
            const std::uint32_t k = d.first + i;
            const std::uint32_t idx = s.index.format == format::r32_uint
                                          ? reinterpret_cast<const std::uint32_t*>(base)[k]
                                          : reinterpret_cast<const std::uint16_t*>(base)[k];
            indices.push_back(std::uint32_t(std::int64_t(idx) + d.base_vertex));
        }
    } else {
        for (std::uint32_t i = 0; i < rects * 3; ++i) indices.push_back(d.first + i);
    }
    const auto* src = static_cast<const std::uint8_t*>(vb->cpu_pointer()) + voff;
    const std::size_t avail = std::size_t(vb->size() - voff);
    rect::CpuExpansion ex;
    if (!rect::expand_rect_vertices({src, avail}, v.stride, elements, rects, indices, ex)) {
        status_ = Status::invalid_argument;
        return;
    }
    if (!bind_common(s, p, false)) return;
    id<MTLBuffer> nvb = [d_->mtl() newBufferWithBytes:ex.vertices.data() length:ex.vertices.size() options:MTLResourceStorageModeShared];
    id<MTLBuffer> nib = [d_->mtl() newBufferWithBytes:ex.indices.data() length:ex.indices.size() * 4 options:MTLResourceStorageModeShared];
    keep(nvb);
    keep(nib);
    [render_ setVertexBuffer:nvb offset:0 attributeStride:v.stride atIndex:p->vertex_buffer_index(0)];
    [render_ drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:ex.indices.size()
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:nib
                 indexBufferOffset:0
                     instanceCount:d.instances
                        baseVertex:0
                      baseInstance:d.first_instance];
}

bool Encoder::bind_compute(const DrawState& s, MetalComputePipeline* p) {
    [compute_ setComputePipelineState:p->pso()];
    note_work(p->pso());
    const std::vector<std::uint8_t> block = push_block(s, p->root());
    if (!block.empty() && p->plan().push_index >= 0)
        [compute_ setBytes:block.data() length:block.size() atIndex:p->plan().push_index];
    return bind_tables(s, p->plan(), 2);
}

void Encoder::dispatch(const CmdDispatch& c) {
    if (render_) end_encoder();
    flush_clears();
    const DrawState& s = *c.state;
    MetalComputePipeline* p = s.compute;
    if (!p) {
        log("dispatch skipped: no compute pipeline");
        status_ = Status::invalid_argument;
        return;
    }
    Status why = Status::ok;
    if (!p->ready_for_encode(&why)) {
        if (why != Status::not_ready) {
            log("dispatch skipped: pipeline %s", p->error().c_str());
            status_ = why;
        }
        return;
    }
    compute();
    if (!bind_compute(s, p)) {
        status_ = Status::invalid_argument;
        return;
    }
    if (c.flags & dispatch_flag::thread_counts) {
        [compute_ dispatchThreads:MTLSizeMake(c.x, c.y, c.z) threadsPerThreadgroup:p->group_size()];
        d_->counters().dispatches_thread_counts++;
    } else {
        [compute_ dispatchThreadgroups:MTLSizeMake(c.x, c.y, c.z) threadsPerThreadgroup:p->group_size()];
    }
    d_->counters().dispatches++;
    gta6rt_hud_add_dispatches(1);
}

bool Encoder::bind_mesh(const DrawState& s, MetalMeshPipeline* p) {
    [render_ setRenderPipelineState:p->pso()];
    note_work(p->pso());
    set_fixed_state(s, p->desc().raster, p->depth_state(), false);
    const std::vector<std::uint8_t> block = push_block(s, p->desc().root);
    if (!block.empty()) {
        if (p->object_plan().push_index >= 0)
            [render_ setObjectBytes:block.data() length:block.size() atIndex:p->object_plan().push_index];
        if (p->mesh_plan().push_index >= 0)
            [render_ setMeshBytes:block.data() length:block.size() atIndex:p->mesh_plan().push_index];
        if (p->fragment_plan().push_index >= 0)
            [render_ setFragmentBytes:block.data() length:block.size() atIndex:p->fragment_plan().push_index];
    }
    return bind_tables(s, p->object_plan(), 3) && bind_tables(s, p->mesh_plan(), 4) &&
           bind_tables(s, p->fragment_plan(), 1);
}

void Encoder::draw_mesh(const CmdDrawMesh& c) {
    if (!ensure_render()) return;
    const DrawState& s = *c.state;
    MetalMeshPipeline* p = s.mesh;
    if (!p) {
        log("mesh draw skipped: no mesh pipeline");
        status_ = Status::invalid_argument;
        return;
    }
    Status why = Status::ok;
    if (!p->ready_for_encode(&why)) {
        if (why != Status::not_ready) {
            log("mesh draw skipped: pipeline %s", p->error().c_str());
            status_ = why;
        }
        return;
    }
    if (!bind_mesh(s, p)) {
        status_ = Status::invalid_argument;
        return;
    }
    render_mesh_ = true;
    const auto o = p->object_threads();
    const auto m = p->mesh_threads();
    [render_ drawMeshThreadgroups:MTLSizeMake(c.x, c.y, c.z)
        threadsPerObjectThreadgroup:MTLSizeMake(o[0], o[1], o[2])
          threadsPerMeshThreadgroup:MTLSizeMake(m[0], m[1], m[2])];
    d_->counters().mesh_draws++;
}

void Encoder::indirect(const CmdIndirect& c) {
    const IndirectDesc& d = c.desc;
    const DrawState& s = *c.state;
    auto* args = static_cast<MetalBuffer*>(d.arguments);
    id<MTLBuffer> src = args->mtl();
    std::uint64_t base = d.argument_offset;
    std::uint64_t stride = d.stride;
    const std::uint32_t packed = kPacked[std::uint32_t(d.kind) & 3];
    if (d.kind == IndirectKind::dispatch) {
        if (render_) end_encoder();
        flush_clears();
        MetalComputePipeline* p = s.compute;
        Status why = Status::ok;
        if (!p || !p->ready_for_encode(&why)) {
            if (!p || why != Status::not_ready) {
                log("indirect dispatch skipped: %s", p ? p->error().c_str() : "no compute pipeline");
                status_ = p ? why : Status::invalid_argument;
            }
            return;
        }
        if (d.count_buffer || indirect_limits()) {
            const Prepared pr = indirect_limits() ? groups_prepass(d) : count_prepass(d);
            if (!pr.buffer) {
                status_ = Status::invalid_argument;
                return;
            }
            [compute_ memoryBarrierWithScope:MTLBarrierScopeBuffers];
            src = pr.buffer;
            base = pr.offset;
            stride = packed;
        }
        compute();
        if (!bind_compute(s, p)) {
            status_ = Status::invalid_argument;
            return;
        }
        for (std::uint32_t i = 0; i < d.max_count; ++i)
            [compute_ dispatchThreadgroupsWithIndirectBuffer:src
                                        indirectBufferOffset:base + i * stride
                                       threadsPerThreadgroup:p->group_size()];
        d_->counters().indirect_commands += d.max_count;
        d_->counters().dispatches += d.max_count;
        gta6rt_hud_add_dispatches(d.max_count);
        return;
    }
    const Cmd* self = &(*cmds_)[pos_];
    const bool rect = s.topology == topology::rect_list && d.kind != IndirectKind::dispatch_mesh;
    const bool mesh_limits = d.kind == IndirectKind::dispatch_mesh && indirect_limits();
    if ((d.count_buffer || rect || mesh_limits) && render_ && !prepared_.count(self)) {
        end_encoder();
        d_->counters().render_pass_splits++;
    }
    if (!ensure_render()) return;
    if (rect) {
        MetalRenderPipeline* p = s.pipeline;
        Status why = Status::ok;
        if (!p || !p->ready_for_encode(&why)) {
            if (!p || why != Status::not_ready) {
                log("indirect draw skipped: %s", p ? p->error().c_str() : "no pipeline");
                status_ = p ? why : Status::invalid_argument;
            }
            return;
        }
        indirect_rect(c, p);
        return;
    }
    if (d.count_buffer || mesh_limits) {
        const Prepared& pr = prepared_[self];
        if (!pr.buffer) {
            status_ = Status::invalid_argument;
            return;
        }
        src = pr.buffer;
        base = pr.offset;
        stride = packed;
    }
    if (d.kind == IndirectKind::dispatch_mesh) {
        MetalMeshPipeline* p = s.mesh;
        Status why = Status::ok;
        if (!p || !p->ready_for_encode(&why)) {
            if (!p || why != Status::not_ready) {
                log("indirect mesh draw skipped: %s", p ? p->error().c_str() : "no mesh pipeline");
                status_ = p ? why : Status::invalid_argument;
            }
            return;
        }
        if (!bind_mesh(s, p)) {
            status_ = Status::invalid_argument;
            return;
        }
        render_mesh_ = true;
        const auto o = p->object_threads();
        const auto m = p->mesh_threads();
        for (std::uint32_t i = 0; i < d.max_count; ++i)
            [render_ drawMeshThreadgroupsWithIndirectBuffer:src
                                       indirectBufferOffset:base + i * stride
                                threadsPerObjectThreadgroup:MTLSizeMake(o[0], o[1], o[2])
                                  threadsPerMeshThreadgroup:MTLSizeMake(m[0], m[1], m[2])];
        d_->counters().indirect_commands += d.max_count;
        d_->counters().mesh_draws += d.max_count;
        return;
    }
    MetalRenderPipeline* p = s.pipeline;
    Status why = Status::ok;
    if (!p || !p->ready_for_encode(&why)) {
        if (!p || why != Status::not_ready) {
            log("indirect draw skipped: %s", p ? p->error().c_str() : "no pipeline");
            status_ = p ? why : Status::invalid_argument;
        }
        return;
    }
    if (s.topology == topology::quad_list || s.topology == topology::rect_list || s.topology > topology::triangle_strip) {
        log("indirect draw skipped: topology %u is not drawn indirectly", s.topology);
        status_ = Status::not_implemented;
        return;
    }
    if (!bind_common(s, p, false)) {
        status_ = Status::invalid_argument;
        return;
    }
    std::vector<bool> slot_done(8, false);
    for (const InputElement& e : p->desc().inputs) {
        if (slot_done[e.slot]) continue;
        slot_done[e.slot] = true;
        const VertexBufferView& v = s.vertex_buffers[e.slot];
        gpu::Buffer* b = nullptr;
        std::uint64_t off = 0;
        if (!d_->resolve_va(v.va, &b, &off)) {
            status_ = Status::invalid_argument;
            return;
        }
        [render_ setVertexBuffer:static_cast<MetalBuffer*>(b)->mtl() offset:off attributeStride:v.stride
                         atIndex:p->vertex_buffer_index(e.slot)];
    }
    const MTLPrimitiveType type = primitive(s.topology);
    if (d.kind == IndirectKind::draw_indexed) {
        gpu::Buffer* ib = nullptr;
        std::uint64_t ioff = 0;
        if (!d_->resolve_va(s.index.va, &ib, &ioff)) {
            status_ = Status::invalid_argument;
            return;
        }
        const bool wide = s.index.format == format::r32_uint;
        for (std::uint32_t i = 0; i < d.max_count; ++i)
            [render_ drawIndexedPrimitives:type
                                 indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                               indexBuffer:static_cast<MetalBuffer*>(ib)->mtl()
                         indexBufferOffset:ioff
                            indirectBuffer:src
                      indirectBufferOffset:base + i * stride];
    } else {
        for (std::uint32_t i = 0; i < d.max_count; ++i)
            [render_ drawPrimitives:type indirectBuffer:src indirectBufferOffset:base + i * stride];
    }
    d_->counters().indirect_commands += d.max_count;
    gta6rt_hud_add_draws(d.max_count);
}

void Encoder::query(const CmdQuery& c) {
    MetalQueryHeap* h = c.heap;
    if (h->type() == QueryType::timestamp) {
        if (c.begin) return;
        sample_timestamp(h->samples(), c.index);
        return;
    }
    if (h->type() == QueryType::pipeline_statistics) {
        static bool logged = false;
        if (!logged) log("pipeline statistics queries resolve to zeros (no Metal equivalent)");
        logged = true;
        return;
    }
    if (c.begin) {
        if (render_ && pass_vis_ != h) {
            if (explicit_) {
                log("occlusion query: a second query heap inside one explicit render pass is not supported");
                status_ = Status::not_implemented;
                return;
            }
            end_encoder();
        }
        active_ = {h, c.index};
        d_->counters().queries_occlusion++;
        if (render_)
            [render_ setVisibilityResultMode:h->type() == QueryType::binary_occlusion ? MTLVisibilityResultModeBoolean
                                                                                      : MTLVisibilityResultModeCounting
                                      offset:c.index * 8];
    } else {
        if (render_ && pass_vis_ == h) [render_ setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        if (active_.heap == h && active_.index == c.index) active_ = {};
    }
}

void Encoder::resolve(const CmdResolveQueries& c) {
    if (render_ || compute_) end_encoder();
    flush_clears();
    join_ = true;
    MetalQueryHeap* h = c.heap;
    keep(h->results());
    switch (h->type()) {
    case QueryType::occlusion:
    case QueryType::binary_occlusion:
        [blit() copyFromBuffer:h->results() sourceOffset:std::uint64_t(c.first) * 8 toBuffer:c.dst->mtl()
             destinationOffset:c.offset size:std::uint64_t(c.count) * 8];
        break;
    case QueryType::timestamp:
        split_after_completion();
        [blit() resolveCounters:h->samples() inRange:NSMakeRange(c.first, c.count) destinationBuffer:c.dst->mtl()
              destinationOffset:c.offset];
        break;
    case QueryType::pipeline_statistics:
        [blit() fillBuffer:c.dst->mtl() range:NSMakeRange(c.offset, std::size_t(c.count) * 88) value:0];
        break;
    }
}

// A timestamp is the end of a one-thread compute encoder that waits for every earlier encoder of the queue:
// this GPU samples counters at stage boundaries only (S0.5), and an empty encoder takes no sample (measured).
void Encoder::sample_timestamp(id<MTLCounterSampleBuffer> buffer, std::uint32_t index) {
    end_encoder();
    flush_clears();
    id<MTLComputePipelineState> nop = d_->builtin_kernel("gta6rt_nop");
    if (!buffer || !nop) {
        status_ = Status::unsupported;
        return;
    }
    MTLComputePassDescriptor* cpd = [MTLComputePassDescriptor computePassDescriptor];
    cpd.dispatchType = MTLDispatchTypeSerial;
    cpd.sampleBufferAttachments[0].sampleBuffer = buffer;
    cpd.sampleBufferAttachments[0].startOfEncoderSampleIndex = index;
    cpd.sampleBufferAttachments[0].endOfEncoderSampleIndex = MTLCounterDontSample;
    id<MTLComputeCommandEncoder> enc = [cb_ computeCommandEncoderWithDescriptor:cpd];
    const EncoderFences f = q_->open_encoder(join_ ? MetalQueue::Join::join : MetalQueue::Join::observe, {});
    join_ = false;
    for (std::uint32_t i = 0; i < f.wait_count; ++i) [enc waitForFence:f.wait[i]];
    [enc setComputePipelineState:nop];
    if (!constants_) constants_ = [d_->mtl() newBufferWithLength:64 options:MTLResourceStorageModeShared];
    [enc setBuffer:constants_ offset:0 atIndex:0];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc updateFence:f.update];
    [enc endEncoding];
    keep(buffer);
    d_->counters().compute_encoders++;
    d_->counters().timestamps++;
}

void Encoder::write_timestamp(const CmdWriteTimestamp& c) {
    if (!stamps_) {
        status_ = Status::unsupported;
        return;
    }
    sample_timestamp(stamps_, next_stamp_++);
    stamp_targets_.emplace_back(c.buffer, c.offset);
}

}  // namespace

Status MetalQueue::execute(std::span<gpu::CommandList* const> lists) {
    if (lists.empty()) return Status::ok;
    if (device_->gpu_fatal().load(std::memory_order_relaxed)) return Status::device_lost;
    std::lock_guard submit(submit_mutex_);
    const std::uint64_t hud_start = gta6rt_hud_active() ? gta6rt_hud_now_ns() : 0;
    Status result;
    @autoreleasepool {
        Encoder e(this);
        result = e.run(lists);
    }
    if (hud_start) gta6rt_hud_note_submission(gta6rt_hud_now_ns() - hud_start);
    return result;
}

}  // namespace gta6rt::metal
