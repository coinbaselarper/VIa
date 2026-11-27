#include "mtl_internal.h"

#include <gta6rt_hud/stats.h>

#import <AppKit/AppKit.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <vector>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace gta6rt::gpu {

const char* status_name(Status s) {
    switch (s) {
    case Status::ok: return "ok";
    case Status::invalid_argument: return "invalid_argument";
    case Status::unsupported: return "unsupported";
    case Status::out_of_memory: return "out_of_memory";
    case Status::compile_failed: return "compile_failed";
    case Status::pipeline_failed: return "pipeline_failed";
    case Status::device_lost: return "device_lost";
    case Status::timeout: return "timeout";
    case Status::not_ready: return "not_ready";
    case Status::not_implemented: return "not_implemented";
    case Status::limit_exceeded: return "limit_exceeded";
    }
    return "unknown";
}

const char* memory_category_name(MemoryCategory c) {
    switch (c) {
    case MemoryCategory::buffer: return "buffer";
    case MemoryCategory::texture: return "texture";
    case MemoryCategory::heap: return "heap";
    case MemoryCategory::descriptor_table: return "descriptor_table";
    case MemoryCategory::host_window: return "host_window";
    case MemoryCategory::internal: return "internal";
    case MemoryCategory::swapchain: return "swapchain";
    case MemoryCategory::count: break;
    }
    return "?";
}

}  // namespace gta6rt::gpu

namespace gta6rt::metal {

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[metal] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

namespace {

const char* kBuiltinSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct ComposeOut { float4 position [[position]]; float2 uv; };
vertex ComposeOut compose_vs(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    ComposeOut o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}
fragment float4 compose_ps(ComposeOut in [[stage_in]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]]) {
    return src.sample(s, in.uv);
}
struct ComposeParams { float4 dst; float4 src; };
vertex ComposeOut compose_plane_vs(uint vid [[vertex_id]], constant ComposeParams& p [[buffer(0)]]) {
    const float2 c = float2(float(vid & 1u), float((vid >> 1) & 1u));
    ComposeOut o;
    o.position = float4(mix(p.dst.xy, p.dst.zw, c), 0.0, 1.0);
    o.uv = mix(p.src.xy, p.src.zw, c);
    return o;
}
kernel void simd_probe(device uint* out [[buffer(0)]], uint w [[threads_per_simdgroup]], uint i [[thread_position_in_grid]]) {
    if (i == 0) out[0] = w;
}
kernel void gta6rt_fill_u32(device uint* o [[buffer(0)]], constant uint2& p [[buffer(1)]],
                           uint i [[thread_position_in_grid]]) {
    if (i < p.y) o[i] = p.x;
}
kernel void gta6rt_nop(device uint* o [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    if (i == 0xffffffffu) o[0] = 0u;
}
struct IndirectCountParams { uint src_word; uint stride_words; uint max_count; uint count_word; uint words; uint has_count; };
kernel void gta6rt_indirect_count(device const uint* args [[buffer(0)]], device const uint* count [[buffer(1)]],
                                  device uint* out [[buffer(2)]], constant IndirectCountParams& p [[buffer(3)]],
                                  uint i [[thread_position_in_grid]]) {
    if (i >= p.max_count) return;
    const uint n = p.has_count != 0u ? min(count[p.count_word], p.max_count) : p.max_count;
    for (uint w = 0; w < p.words; ++w)
        out[i * p.words + w] = i < n ? args[p.src_word + i * p.stride_words + w] : 0u;
}
struct RectIndirectParams { uint src_word; uint stride_words; uint max_count; uint count_word; uint has_count;
                            uint indexed; uint index_bytes; uint params_stride_words; uint params_word;
                            uint strides[8]; };
kernel void gta6rt_rect_indirect(device const uint* args [[buffer(0)]], device const uint* count [[buffer(1)]],
                                 device uint* out [[buffer(2)]], constant RectIndirectParams& p [[buffer(3)]],
                                 uint i [[thread_position_in_grid]]) {
    if (i >= p.max_count) return;
    const uint n = p.has_count != 0u ? min(count[p.count_word], p.max_count) : p.max_count;
    device const uint* a = args + p.src_word + i * p.stride_words;
    const uint vertices = i < n ? a[0] : 0u;
    device uint* draw = out + i * 4u;
    draw[0] = vertices / 3u * 6u;
    draw[1] = i < n ? a[1] : 0u;
    draw[2] = 0u;
    draw[3] = i < n ? (p.indexed != 0u ? a[4] : a[3]) : 0u;
    device uint* w = out + p.params_word + i * p.params_stride_words;
    w[0] = i < n ? a[2] : 0u;
    w[1] = p.indexed != 0u && i < n ? a[3] : 0u;
    w[2] = p.indexed != 0u ? p.index_bytes : 0u;
    w[3] = draw[3];
    for (uint k = 0; k < 8u; ++k) w[4u + k] = p.strides[k];
}
struct IndirectGroupsParams { uint stride_words; uint max_count; uint count_word; uint has_count; uint mesh; uint mesh_max; uint cs_max; uint debug; };
kernel void gta6rt_indirect_groups(device const uint* args [[buffer(0)]], device const uint* count [[buffer(1)]],
                                   device uint* out [[buffer(2)]], constant IndirectGroupsParams& p [[buffer(3)]],
                                   device atomic_uint* dbg [[buffer(4)]], uint i [[thread_position_in_grid]]) {
    if (i >= p.max_count) return;
    const uint raw_count = p.has_count != 0u ? count[p.count_word] : p.max_count;
    const uint n = min(raw_count, p.max_count);
    device const uint* a = args + i * p.stride_words;
    uint3 g = i < n ? uint3(a[0], a[1], a[2]) : uint3(0u);
    const ulong groups = ulong(g.x) * ulong(g.y) * ulong(g.z);
    const bool legal = p.mesh != 0u ? g.x <= 65535u && g.y <= 65535u && g.z <= 65535u && groups <= ulong(p.mesh_max)
                                    : groups <= ulong(p.cs_max);
    if (!legal) g = uint3(0u);
    out[i * 3u + 0u] = g.x;
    out[i * 3u + 1u] = g.y;
    out[i * 3u + 2u] = g.z;
    if (p.debug == 0u) return;
    const uint b = p.mesh != 0u ? 0u : 16u;
    atomic_fetch_add_explicit(&dbg[b + 0u], 1u, memory_order_relaxed);
    if (i < n) atomic_fetch_add_explicit(&dbg[b + 1u], 1u, memory_order_relaxed);
    if (!legal) atomic_fetch_add_explicit(&dbg[b + 3u], 1u, memory_order_relaxed);
    if (legal && groups != 0ul) {
        atomic_fetch_add_explicit(&dbg[b + 2u], 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&dbg[b + 4u], uint(groups), memory_order_relaxed);
        const uint s = atomic_fetch_add_explicit(&dbg[b + 9u], 1u, memory_order_relaxed);
        if (s < 8u) {
            atomic_store_explicit(&dbg[32u + b * 2u + s * 4u + 0u], g.x, memory_order_relaxed);
            atomic_store_explicit(&dbg[32u + b * 2u + s * 4u + 1u], g.y, memory_order_relaxed);
            atomic_store_explicit(&dbg[32u + b * 2u + s * 4u + 2u], g.z, memory_order_relaxed);
            atomic_store_explicit(&dbg[32u + b * 2u + s * 4u + 3u], i, memory_order_relaxed);
        }
    }
    if (i != 0u) return;
    atomic_fetch_add_explicit(&dbg[b + 5u], 1u, memory_order_relaxed);
    if (p.has_count == 0u) return;
    atomic_fetch_add_explicit(&dbg[b + 6u], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&dbg[b + 7u], n, memory_order_relaxed);
    if (n == 0u) atomic_fetch_add_explicit(&dbg[b + 8u], 1u, memory_order_relaxed);
    const uint s = atomic_fetch_add_explicit(&dbg[b + 10u], 1u, memory_order_relaxed);
    if (s < 8u) atomic_store_explicit(&dbg[96u + b / 2u + s], raw_count, memory_order_relaxed);
}
struct QuadParams { uint first_byte; uint index_bytes; uint readable; uint quads; };
kernel void gta6rt_quad_expand(device const uchar* ib [[buffer(0)]], device uint* out [[buffer(1)]],
                               constant QuadParams& p [[buffer(2)]], uint q [[thread_position_in_grid]]) {
    if (q >= p.quads) return;
    uint idx[4];
    for (uint k = 0; k < 4; ++k) {
        const uint n = q * 4u + k;
        if (n >= p.readable) { idx[k] = 0u; continue; }
        const uint off = p.first_byte + n * p.index_bytes;
        idx[k] = p.index_bytes == 4u ? *(device const uint*)(ib + off) : uint(*(device const ushort*)(ib + off));
    }
    out[q * 6u + 0u] = idx[0]; out[q * 6u + 1u] = idx[1]; out[q * 6u + 2u] = idx[2];
    out[q * 6u + 3u] = idx[0]; out[q * 6u + 4u] = idx[2]; out[q * 6u + 5u] = idx[3];
}
)MSL";

MTLResourceOptions storage_options(MemoryKind memory) {
    switch (memory) {
    case MemoryKind::device_local: return MTLResourceStorageModePrivate;
    case MemoryKind::upload: return MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
    case MemoryKind::readback:
    case MemoryKind::shared: return MTLResourceStorageModeShared;
    }
    return MTLResourceStorageModeShared;
}

MTLStorageMode storage_mode(MemoryKind memory) {
    return memory == MemoryKind::device_local ? MTLStorageModePrivate : MTLStorageModeShared;
}

}  // namespace

bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

MetalDevice::MetalDevice(id<MTLDevice> device, const DeviceOptions& options) : device_(device), options_(options) {
    va_.init(options.va_base, options.va_size);
    compiler_.init(options.compile_threads);
    const bool no_residency = env_flag("GTA6RT_METAL_NO_RESIDENCY");
    table_residency_ = !no_residency;
    if (!no_residency && !env_flag("GTA6RT_METAL_NO_DEVICE_RESIDENCY")) {
        if (@available(macOS 15.0, *)) {
            MTLResidencySetDescriptor* rd = [MTLResidencySetDescriptor new];
            rd.label = @"gta6rt device residency";
            rd.initialCapacity = 1024;
            NSError* error = nil;
            resident_ = [device_ newResidencySetWithDescriptor:rd error:&error];
            if (!resident_) log("device residency set: %s", error.localizedDescription.UTF8String);
        }
    }
    listener_queue_ = dispatch_queue_create("gta6rt.metal.events", DISPATCH_QUEUE_SERIAL);
    listener_ = [[MTLSharedEventListener alloc] initWithDispatchQueue:listener_queue_];
    std::string err;
    double secs = 0;
    builtin_library_ = compile_msl(kBuiltinSource, 3, 0, false, &err, &secs);
    if (!builtin_library_) log("builtin library failed: %s", err.c_str());
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterNearest;
    sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    compose_sampler_ = [device_ newSamplerStateWithDescriptor:sd];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    compose_linear_ = [device_ newSamplerStateWithDescriptor:sd];
    for (id<MTLCounterSet> set in device_.counterSets)
        if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) timestamp_set_ = set;
    probe_caps();
    start_pressure_source();
}

MetalDevice::~MetalDevice() {
    if (pressure_source_) dispatch_source_cancel(pressure_source_);
    collect_retired(true);
}

id<MTLComputePipelineState> MetalDevice::builtin_kernel(const char* name) {
    std::lock_guard lock(builtin_mutex_);
    auto it = kernels_.find(name);
    if (it != kernels_.end()) return it->second;
    id<MTLComputePipelineState> p = nil;
    if (builtin_library_) {
        NSError* error = nil;
        MTLComputePipelineDescriptor* d = [MTLComputePipelineDescriptor new];
        d.computeFunction = [builtin_library_ newFunctionWithName:@(name)];
        d.label = @(name);
        p = d.computeFunction ? [device_ newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone
                                                                    reflection:nil error:&error]
                              : nil;
        if (!p) log("builtin kernel %s: %s", name, error.localizedDescription.UTF8String);
    }
    kernels_[name] = p;
    return p;
}

id<MTLBuffer> MetalDevice::quad_pattern(std::uint32_t quads) {
    std::lock_guard lock(builtin_mutex_);
    if (quad_pattern_ && quad_pattern_quads_ >= quads) return quad_pattern_;
    std::uint32_t n = std::max<std::uint32_t>(quads, std::max<std::uint32_t>(4096, quad_pattern_quads_ * 2));
    id<MTLBuffer> b = [device_ newBufferWithLength:std::size_t(n) * 24 options:MTLResourceStorageModeShared];
    auto* p = static_cast<std::uint32_t*>(b.contents);
    for (std::uint32_t q = 0; q < n; ++q) {
        const std::uint32_t v = q * 4;
        const std::uint32_t t[6] = {v, v + 1, v + 2, v, v + 2, v + 3};
        std::memcpy(p + q * 6, t, sizeof(t));
    }
    if (quad_pattern_) {
        retire(quad_pattern_, quad_pattern_);
        ledger_.remove(MemoryCategory::internal, quad_pattern_.allocatedSize);
    }
    make_resident(b);
    ledger_.add(MemoryCategory::internal, b.allocatedSize);
    quad_pattern_ = b;
    quad_pattern_quads_ = n;
    return b;
}

void MetalDevice::make_resident(id<MTLAllocation> allocation) {
    if (!resident_ || !allocation) return;
    std::lock_guard lock(residency_mutex_);
    [resident_ addAllocation:allocation];
    resident_dirty_ = true;
}

void MetalDevice::set_encoder_resource(id<MTLResource> resource, bool on) {
    if (!resource) return;
    std::lock_guard lock(encoder_resource_mutex_);
    auto it = std::find(encoder_resources_.begin(), encoder_resources_.end(), resource);
    if (on == (it != encoder_resources_.end())) return;
    if (on) encoder_resources_.push_back(resource);
    else encoder_resources_.erase(it);
    encoder_resource_version_.fetch_add(1, std::memory_order_acq_rel);
}

void MetalDevice::encoder_resources(std::vector<id<MTLResource>>& out, std::uint64_t& version) {
    std::lock_guard lock(encoder_resource_mutex_);
    out = encoder_resources_;
    version = encoder_resource_version_.load(std::memory_order_acquire);
}

std::uint64_t MetalDevice::encoder_resource_count() {
    std::lock_guard lock(encoder_resource_mutex_);
    return encoder_resources_.size();
}

void MetalDevice::evict(id<MTLAllocation> allocation) {
    if (!resident_ || !allocation) return;
    std::lock_guard lock(residency_mutex_);
    [resident_ removeAllocation:allocation];
    resident_dirty_ = true;
}

void MetalDevice::add_queue(MetalQueue* queue) {
    std::lock_guard lock(retire_mutex_);
    queues_.push_back(queue);
}

void MetalDevice::remove_queue(MetalQueue* queue) {
    std::lock_guard lock(retire_mutex_);
    std::erase(queues_, queue);
}

RetireMarks MetalDevice::retire_marks() {
    RetireMarks marks;
    static const bool now = env_flag("GTA6RT_METAL_RELEASE_AT_ONCE");
    if (now) return marks;
    std::lock_guard lock(retire_mutex_);
    for (MetalQueue* q : queues_) {
        const std::uint64_t mark = q->retire_mark();
        if (q->timeline().signaledValue < mark) marks.emplace_back(q->timeline(), mark);
    }
    return marks;
}

bool MetalDevice::marks_reached(const RetireMarks& marks) {
    for (const auto& [timeline, value] : marks)
        if (timeline.signaledValue < value) return false;
    return true;
}

void MetalDevice::retire(id object, id<MTLAllocation> evict_allocation) {
    if (!object && !evict_allocation) return;
    static const bool now = env_flag("GTA6RT_METAL_RELEASE_AT_ONCE");
    Retired r;
    r.object = object;
    r.evict = evict_allocation;
    if (!now) {
        std::lock_guard lock(retire_mutex_);
        for (MetalQueue* q : queues_) {
            const std::uint64_t mark = q->retire_mark();
            if (q->timeline().signaledValue < mark) r.marks.emplace_back(q->timeline(), mark);
        }
        if (!r.marks.empty()) {
            r.bytes = evict_allocation ? evict_allocation.allocatedSize : 0;
            retired_bytes_ += r.bytes;
            retired_.push_back(std::move(r));
            stats_.releases_deferred++;
            stats_.releases_waiting = retired_.size();
            stats_.releases_waiting_bytes = retired_bytes_;
            if (retired_.size() > stats_.releases_waiting_max) stats_.releases_waiting_max = retired_.size();
            return;
        }
    }
    stats_.releases_immediate++;
    if (r.evict) evict(r.evict);
}

void MetalDevice::collect_retired(bool everything) {
    std::vector<Retired> done;
    {
        std::lock_guard lock(retire_mutex_);
        while (!retired_.empty() && (everything || marks_reached(retired_.front().marks))) {
            retired_bytes_ -= retired_.front().bytes;
            done.push_back(std::move(retired_.front()));
            retired_.pop_front();
        }
        if (done.empty()) return;
        stats_.releases_waiting = retired_.size();
        stats_.releases_waiting_bytes = retired_bytes_;
    }
    for (Retired& r : done)
        if (r.evict) evict(r.evict);
}

void MetalDevice::commit_residency() {
    if (!resident_) return;
    std::lock_guard lock(residency_mutex_);
    if (!resident_dirty_) return;
    [resident_ commit];
    resident_dirty_ = false;
    stats_.residency_commits++;
}

void MetalDevice::probe_caps() {
    caps_.name = device_.name.UTF8String;
#if defined(__x86_64__)
    caps_.process_arch = "x86_64";
#else
    caps_.process_arch = "arm64";
#endif
    caps_.argument_buffers_tier = device_.argumentBuffersSupport == MTLArgumentBuffersTier2 ? 2 : 1;
    if (@available(macOS 15.0, *)) caps_.residency_sets = true;
    caps_.family_apple7 = [device_ supportsFamily:MTLGPUFamilyApple7];
    caps_.family_apple8 = [device_ supportsFamily:MTLGPUFamilyApple8];
    caps_.family_apple9 = [device_ supportsFamily:MTLGPUFamilyApple9];
    caps_.family_metal3 = [device_ supportsFamily:MTLGPUFamilyMetal3];
    if (@available(macOS 26.0, *)) caps_.family_metal4 = [device_ supportsFamily:MTLGPUFamilyMetal4];
    caps_.unified_memory = device_.hasUnifiedMemory;
    caps_.barycentrics = device_.supportsShaderBarycentricCoordinates;
    if (@available(macOS 15.0, *)) caps_.placement_sparse = device_.supportsPlacementSparse;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    caps_.d24s8 = device_.depth24Stencil8PixelFormatSupported;
#pragma clang diagnostic pop
    caps_.mesh_shaders = caps_.family_apple7 || caps_.family_metal3;
    caps_.max_buffer_length = device_.maxBufferLength;
    caps_.max_threadgroup_memory = std::uint32_t(device_.maxThreadgroupMemoryLength);
    caps_.recommended_working_set = device_.recommendedMaxWorkingSetSize;
    caps_.max_threads_per_threadgroup = std::uint32_t(device_.maxThreadsPerThreadgroup.width);
    caps_.max_dispatch_groups = 0xffffffffull;
    caps_.counters_stage_boundary = [device_ supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary];
    caps_.counters_draw_boundary = [device_ supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary];
    caps_.counters_dispatch_boundary = [device_ supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary];
    gta6rt_hud_set_driver(device_.name.UTF8String ? device_.name.UTF8String : "Metal");
    caps_.host_page_size = std::uint64_t(getpagesize());
    caps_.linear_texture_alignment = std::uint32_t([device_ minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatR32Float]);
    if (NSScreen* screen = NSScreen.mainScreen) caps_.display_refresh_hz = double(screen.maximumFramesPerSecond);

    if (builtin_library_) {
        id<MTLFunction> fn = [builtin_library_ newFunctionWithName:@"simd_probe"];
        NSError* error = nil;
        id<MTLComputePipelineState> cps = [device_ newComputePipelineStateWithFunction:fn error:&error];
        if (cps) {
            id<MTLBuffer> out = [device_ newBufferWithLength:16 options:MTLResourceStorageModeShared];
            id<MTLCommandQueue> q = [device_ newCommandQueue];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:cps];
            [enc setBuffer:out offset:0 atIndex:0];
            [enc dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            caps_.simd_width = static_cast<std::uint32_t*>(out.contents)[0];
        }
    }

    MTLTimestamp cpu0 = 0, gpu0 = 0, cpu1 = 0, gpu1 = 0;
    const double w0 = now_seconds();
    [device_ sampleTimestamps:&cpu0 gpuTimestamp:&gpu0];
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    [device_ sampleTimestamps:&cpu1 gpuTimestamp:&gpu1];
    const double wall = now_seconds() - w0;
    if (wall > 0 && gpu1 > gpu0) {
        const double hz = double(gpu1 - gpu0) / wall;
        caps_.timestamp_frequency = std::uint64_t(std::llround(hz / 1e6) * 1000000);
    }
}

DeviceStats MetalDevice::stats() const {
    DeviceStats s;
    s.pipelines_created = stats_.pipelines_created;
    s.pipelines_compiled = stats_.pipelines_compiled;
    s.render_encoders = stats_.render_encoders;
    s.blit_encoders = stats_.blit_encoders;
    s.compute_encoders = stats_.compute_encoders;
    s.barriers_noop = stats_.barriers_noop;
    s.barriers_fenced = stats_.barriers_fenced;
    s.clears_folded = stats_.clears_folded;
    s.command_buffers = stats_.command_buffers;
    s.value_writes = stats_.value_writes;
    s.value_waits = stats_.value_waits;
    s.rect_draws_expanded = stats_.rect_draws_expanded;
    s.stores_discarded = stats_.stores_discarded;
    s.fence_joins = stats_.fence_joins;
    s.target_joins = stats_.target_joins;
    s.residency_commits = stats_.residency_commits;
    s.dispatches = stats_.dispatches;
    s.dispatches_thread_counts = stats_.dispatches_thread_counts;
    s.indirect_commands = stats_.indirect_commands;
    s.indirect_prepasses = stats_.indirect_prepasses;
    s.prepasses_hoisted = stats_.prepasses_hoisted;
    s.render_pass_splits = stats_.render_pass_splits;
    s.barriers_in_encoder = stats_.barriers_in_encoder;
    s.mesh_draws = stats_.mesh_draws;
    s.quad_draws = stats_.quad_draws;
    s.queries_occlusion = stats_.queries_occlusion;
    s.timestamps = stats_.timestamps;
    s.draws_skipped_not_ready = stats_.draws_skipped_not_ready;
    s.d24_fragment_pipelines = stats_.d24_fragment_pipelines;
    s.archive_hits = stats_.archive_hits;
    s.archive_misses = stats_.archive_misses;
    s.pipelines_refused_limit = stats_.pipelines_refused_limit;
    s.presents = stats_.presents;
    s.present_planes = stats_.present_planes;
    s.command_buffer_errors = stats_.command_buffer_errors;
    s.command_buffer_victims = stats_.command_buffer_victims;
    s.event_signals_repaired = stats_.event_signals_repaired;
    s.releases_deferred = stats_.releases_deferred;
    s.releases_immediate = stats_.releases_immediate;
    s.releases_waiting = stats_.releases_waiting;
    s.releases_waiting_bytes = stats_.releases_waiting_bytes;
    s.releases_waiting_max = stats_.releases_waiting_max;
    return s;
}

bool MetalDevice::describe_format(std::uint32_t dxgi, FormatInfo& out) const {
    NativeFormat nf;
    if (!native_format(dxgi, nf)) return false;
    out = nf.info;
    return true;
}

id<MTLLibrary> MetalDevice::compile_msl(const std::string& source, std::uint32_t major, std::uint32_t minor,
                                         bool fast_math, std::string* error, double* seconds) {
    MTLCompileOptions* options = [MTLCompileOptions new];
    MTLLanguageVersion version = MTLLanguageVersion3_0;
    if (major == 3 && minor == 1) version = MTLLanguageVersion3_1;
    if (major == 3 && minor == 2) version = MTLLanguageVersion3_2;
    if (major == 2) version = MTLLanguageVersion2_4;
    options.languageVersion = version;
    if (@available(macOS 15.0, *)) {
        options.mathMode = fast_math ? MTLMathModeFast : MTLMathModeSafe;
    }
    NSError* err = nil;
    const double t0 = now_seconds();
    NSString* text = [[NSString alloc] initWithBytes:source.data() length:source.size() encoding:NSUTF8StringEncoding];
    id<MTLLibrary> lib = text ? [device_ newLibraryWithSource:text options:options error:&err] : nil;
    if (seconds) *seconds = now_seconds() - t0;
    if (!lib && error) *error = err ? err.localizedDescription.UTF8String : "source is not UTF-8";
    return lib;
}

std::unique_ptr<gpu::Queue> MetalDevice::create_queue(QueueType type, const char* label) {
    id<MTLCommandQueue> q = [device_ newCommandQueueWithMaxCommandBufferCount:256];
    if (!q) return nullptr;
    if (label) q.label = @(label);
    if (resident_) {
        if (@available(macOS 15.0, *)) [q addResidencySet:resident_];
    }
    return std::make_unique<MetalQueue>(this, type, q);
}

std::unique_ptr<gpu::Fence> MetalDevice::create_fence(std::uint64_t initial) {
    id<MTLSharedEvent> ev = [device_ newSharedEvent];
    id<MTLBuffer> mirror = [device_ newBufferWithLength:64 options:MTLResourceStorageModeShared];
    if (!ev || !mirror) return nullptr;
    ev.signaledValue = initial;
    return std::make_unique<MetalFence>(this, ev, mirror, initial);
}

AllocationInfo MetalDevice::buffer_allocation_info(const BufferDesc& desc) const {
    MTLSizeAndAlign sa = [device_ heapBufferSizeAndAlignWithLength:desc.size options:storage_options(desc.memory)];
    return {sa.size, sa.align};
}

MTLTextureDescriptor* MetalDevice::texture_descriptor(const TextureDesc& desc, const NativeFormat& nf,
                                                      std::string& err) const {
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    switch (desc.dimension) {
    case TextureDimension::tex1d: td.textureType = MTLTextureType1D; break;
    case TextureDimension::tex2d:
        td.textureType = desc.samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
        break;
    case TextureDimension::tex2d_array:
        td.textureType = desc.samples > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
        break;
    case TextureDimension::tex3d: td.textureType = MTLTextureType3D; break;
    case TextureDimension::cube: td.textureType = MTLTextureTypeCube; break;
    }
    td.pixelFormat = nf.pixel;
    td.width = desc.width;
    td.height = desc.dimension == TextureDimension::tex1d ? 1 : desc.height;
    td.depth = desc.dimension == TextureDimension::tex3d ? desc.depth_or_layers : 1;
    td.arrayLength = desc.dimension == TextureDimension::tex2d_array ? desc.depth_or_layers
                   : desc.dimension == TextureDimension::cube        ? std::max(1u, desc.depth_or_layers / 6)
                                                                     : 1;
    td.mipmapLevelCount = std::max(1u, desc.mip_levels);
    td.sampleCount = std::max(1u, desc.samples);
    MTLTextureUsage u = 0;
    if (desc.usage & usage::shader_read) u |= MTLTextureUsageShaderRead;
    if (desc.usage & (usage::render_target | usage::depth_stencil)) u |= MTLTextureUsageRenderTarget;
    if (desc.usage & usage::unordered_access) u |= MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    if (nf.info.depth || nf.info.stencil || nf.swizzled || (desc.usage & usage::pixel_format_view))
        u |= MTLTextureUsagePixelFormatView;
    td.usage = u ? u : MTLTextureUsageShaderRead;
    td.storageMode = storage_mode(desc.memory);
    if (desc.width == 0 || desc.height == 0) err = "zero extent";
    return td;
}

AllocationInfo MetalDevice::texture_allocation_info(const TextureDesc& desc) const {
    NativeFormat nf;
    if (!native_format(desc.format, nf)) return {};
    std::string err;
    MTLTextureDescriptor* td = texture_descriptor(desc, nf, err);
    if (!err.empty()) return {};
    MTLSizeAndAlign sa = [device_ heapTextureSizeAndAlignWithDescriptor:td];
    return {sa.size, sa.align};
}

std::unique_ptr<gpu::Heap> MetalDevice::create_heap(const HeapDesc& desc) {
    if (!desc.size) return nullptr;
    MTLHeapDescriptor* hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement;
    hd.size = desc.size;
    hd.storageMode = storage_mode(desc.memory);
    hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    if (desc.memory == MemoryKind::upload) hd.cpuCacheMode = MTLCPUCacheModeWriteCombined;
    id<MTLHeap> heap = [device_ newHeapWithDescriptor:hd];
    if (!heap) return nullptr;
    if (desc.label) heap.label = @(desc.label);
    const std::uint64_t va = va_.allocate(heap.size, 1ull << 16);
    if (!va) return nullptr;
    std::unique_ptr<MetalBuffer> arena;
    if (desc.console_arena) {
        const MTLResourceOptions opts = storage_options(desc.memory) | MTLResourceHazardTrackingModeUntracked;
        id<MTLBuffer> b = [heap newBufferWithLength:heap.size options:opts offset:0];
        if (!b) return nullptr;
        b.label = @"console arena";
        arena = std::make_unique<MetalBuffer>(this, b, va, heap.size, true, false);
    }
    return std::make_unique<MetalHeap>(this, heap, va, std::move(arena), desc.memory);
}

std::unique_ptr<gpu::Buffer> MetalDevice::create_buffer(const BufferDesc& desc) {
    if (!desc.size) return nullptr;
    if (desc.va_only) {
        id<MTLBuffer> b = nil;
        if (@available(macOS 26.0, *))
            b = [device_ newBufferWithLength:(desc.size + 0x3fff) & ~std::uint64_t(0x3fff)
                                     options:MTLResourceStorageModePrivate
                     placementSparsePageSize:MTLSparsePageSize16];
        if (!b) return nullptr;
        if (desc.label) b.label = @(desc.label);
        const std::uint64_t va = va_.allocate(desc.size, 1ull << 16);
        return std::make_unique<MetalBuffer>(this, b, va, desc.size, false, true, MemoryCategory::buffer, false);
    }
    id<MTLBuffer> b = [device_ newBufferWithLength:desc.size options:storage_options(desc.memory)];
    if (!b) return nullptr;
    if (desc.label) b.label = @(desc.label);
    const std::uint64_t va = va_.allocate(desc.size, 1ull << 16);
    return std::make_unique<MetalBuffer>(this, b, va, desc.size, false, true, MemoryCategory::buffer, desc.resident);
}

std::unique_ptr<gpu::Texture> MetalDevice::create_texture(const TextureDesc& desc) {
    NativeFormat nf;
    if (!native_format(desc.format, nf)) {
        log("create_texture: DXGI format %u not mapped", desc.format);
        return nullptr;
    }
    std::string err;
    MTLTextureDescriptor* td = texture_descriptor(desc, nf, err);
    if (!err.empty()) return nullptr;
    id<MTLTexture> t = [device_ newTextureWithDescriptor:td];
    if (!t) return nullptr;
    if (desc.label) t.label = @(desc.label);
    return std::make_unique<MetalTexture>(this, t, desc, nf, false);
}

std::unique_ptr<gpu::Buffer> MetalDevice::create_placed_buffer(gpu::Heap* heap, std::uint64_t offset,
                                                               const BufferDesc& desc) {
    auto* h = static_cast<MetalHeap*>(heap);
    if (!h || !desc.size || offset + desc.size > h->size()) return nullptr;
    const MTLResourceOptions opts = storage_options(h->memory()) | MTLResourceHazardTrackingModeUntracked;
    id<MTLBuffer> b = [h->mtl() newBufferWithLength:desc.size options:opts offset:offset];
    if (!b) return nullptr;
    if (desc.label) b.label = @(desc.label);
    return std::make_unique<MetalBuffer>(this, b, h->va_base() + offset, desc.size, true, false);
}

std::unique_ptr<gpu::Texture> MetalDevice::create_placed_texture(gpu::Heap* heap, std::uint64_t offset,
                                                                 const TextureDesc& desc) {
    auto* h = static_cast<MetalHeap*>(heap);
    NativeFormat nf;
    if (!h || !native_format(desc.format, nf)) return nullptr;
    TextureDesc d = desc;
    d.memory = h->memory();
    std::string err;
    MTLTextureDescriptor* td = texture_descriptor(d, nf, err);
    if (!err.empty()) return nullptr;
    td.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    id<MTLTexture> t = [h->mtl() newTextureWithDescriptor:td offset:offset];
    if (!t) return nullptr;
    if (desc.label) t.label = @(desc.label);
    return std::make_unique<MetalTexture>(this, t, d, nf, true);
}

bool MetalDevice::resolve_va(std::uint64_t va, gpu::Buffer** buffer, std::uint64_t* offset) const {
    MetalBuffer* b = nullptr;
    if (!va_.resolve(va, &b, offset)) return false;
    *buffer = b;
    return true;
}

std::unique_ptr<gpu::Sampler> MetalDevice::create_sampler(const SamplerDesc& desc) {
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = desc.min == Filter::linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.magFilter = desc.mag == Filter::linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = desc.mip == Filter::linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    auto mode = [](AddressMode m) {
        switch (m) {
        case AddressMode::wrap: return MTLSamplerAddressModeRepeat;
        case AddressMode::mirror: return MTLSamplerAddressModeMirrorRepeat;
        case AddressMode::clamp: return MTLSamplerAddressModeClampToEdge;
        case AddressMode::border: return MTLSamplerAddressModeClampToBorderColor;
        case AddressMode::mirror_once: return MTLSamplerAddressModeMirrorClampToEdge;
        }
        return MTLSamplerAddressModeRepeat;
    };
    sd.sAddressMode = mode(desc.u);
    sd.tAddressMode = mode(desc.v);
    sd.rAddressMode = mode(desc.w);
    sd.maxAnisotropy = std::clamp<NSUInteger>(desc.max_anisotropy, 1, 16);
    sd.lodMinClamp = desc.min_lod;
    sd.lodMaxClamp = desc.max_lod;
    if (desc.compare != CompareFunc::none) sd.compareFunction = MTLCompareFunction(std::uint32_t(desc.compare) - 1);
    const auto& b = desc.border;
    bool exact = true;
    if (b[3] == 0 && b[0] == 0 && b[1] == 0 && b[2] == 0) {
        sd.borderColor = MTLSamplerBorderColorTransparentBlack;
    } else if (b[3] == 1 && b[0] == 0 && b[1] == 0 && b[2] == 0) {
        sd.borderColor = MTLSamplerBorderColorOpaqueBlack;
    } else if (b[3] == 1 && b[0] == 1 && b[1] == 1 && b[2] == 1) {
        sd.borderColor = MTLSamplerBorderColorOpaqueWhite;
    } else {
        exact = false;
        const float lum = (b[0] + b[1] + b[2]) / 3.0f;
        sd.borderColor = b[3] < 0.5f ? MTLSamplerBorderColorTransparentBlack
                       : lum < 0.5f  ? MTLSamplerBorderColorOpaqueBlack
                                     : MTLSamplerBorderColorOpaqueWhite;
    }
    sd.supportArgumentBuffers = YES;
    id<MTLSamplerState> s = [device_ newSamplerStateWithDescriptor:sd];
    if (!s) return nullptr;
    return std::make_unique<MetalSampler>(s, exact);
}

std::unique_ptr<gpu::DescriptorHeap> MetalDevice::create_descriptor_heap(const DescriptorHeapDesc& desc) {
    if (!desc.count) return nullptr;
    id<MTLBuffer> b = [device_ newBufferWithLength:std::size_t(desc.count) * 8 options:MTLResourceStorageModeShared];
    if (!b) return nullptr;
    if (desc.label) b.label = @(desc.label);
    id<MTLResidencySet> rs = nil;
    if (@available(macOS 15.0, *)) {
        MTLResidencySetDescriptor* rd = [MTLResidencySetDescriptor new];
        rd.initialCapacity = std::min<NSUInteger>(desc.count, 4096);
        if (desc.label) rd.label = @(desc.label);
        NSError* error = nil;
        rs = [device_ newResidencySetWithDescriptor:rd error:&error];
        if (!rs) log("residency set: %s", error.localizedDescription.UTF8String);
    }
    return std::make_unique<MetalDescriptorHeap>(this, desc, b, rs);
}

std::unique_ptr<gpu::RootLayout> MetalDevice::create_root_layout(const RootLayoutDesc& desc) {
    if (desc.push_bytes < desc.user_dwords * 4 || desc.push_bytes > 4096) return nullptr;
    return std::make_unique<MetalRootLayout>(desc);
}

std::unique_ptr<gpu::ShaderLibrary> MetalDevice::create_library_msl(std::string_view source, const MslOptions& options,
                                                                    std::string* error) {
    std::string text(source);
    if (options.deferred) return std::make_unique<MetalLibrary>(this, nil, std::move(text), 0.0, options);
    double secs = 0;
    id<MTLLibrary> lib = compile_msl(text, options.language_major, options.language_minor, options.fast_math, error, &secs);
    if (!lib) return nullptr;
    if (options.label) lib.label = @(options.label);
    return std::make_unique<MetalLibrary>(this, lib, std::move(text), secs, options);
}

std::unique_ptr<gpu::ShaderLibrary> MetalDevice::create_library_metallib(std::span<const std::uint8_t> bytes,
                                                                         std::string* error) {
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* err = nil;
    const double t0 = now_seconds();
    id<MTLLibrary> lib = [device_ newLibraryWithData:data error:&err];
    if (!lib) {
        if (error) *error = err ? err.localizedDescription.UTF8String : "metallib rejected";
        return nullptr;
    }
    auto out = std::make_unique<MetalLibrary>(this, lib, std::string(), now_seconds() - t0, MslOptions{});
    out->set_bytes(bytes.size());
    return out;
}

std::unique_ptr<gpu::CommandList> MetalDevice::create_command_list(QueueType type) {
    return std::make_unique<MetalCommandList>(type);
}

id<MTLRenderPipelineState> MetalDevice::compose_pipeline(MTLPixelFormat format, PlaneBlend blend) {
    std::lock_guard lock(builtin_mutex_);
    auto it = compose_.find({format, blend});
    if (it != compose_.end()) return it->second;
    if (!builtin_library_) return nil;
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [builtin_library_ newFunctionWithName:@"compose_plane_vs"];
    pd.fragmentFunction = [builtin_library_ newFunctionWithName:@"compose_ps"];
    MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
    ca.pixelFormat = format;
    if (blend != PlaneBlend::opaque) {
        ca.blendingEnabled = YES;
        ca.sourceRGBBlendFactor = blend == PlaneBlend::premultiplied ? MTLBlendFactorOne : MTLBlendFactorSourceAlpha;
        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    NSError* error = nil;
    id<MTLRenderPipelineState> pso = [device_ newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pso) log("compose pipeline: %s", error.localizedDescription.UTF8String);
    compose_[{format, blend}] = pso;
    return pso;
}

std::unique_ptr<gpu::Swapchain> MetalDevice::create_swapchain(const SwapchainDesc& desc) {
    CAMetalLayer* layer = (__bridge CAMetalLayer*)desc.layer;
    if (!layer || !desc.width || !desc.height || !desc.buffer_count) return nullptr;
    layer.device = device_;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = desc.keep_last_frame ? NO : YES;
    layer.drawableSize = CGSizeMake(desc.width, desc.height);
    layer.maximumDrawableCount = 3;
    std::vector<std::unique_ptr<MetalTexture>> buffers;
    for (std::uint32_t i = 0; i < desc.buffer_count; ++i) {
        TextureDesc td;
        td.format = desc.format;
        td.width = desc.width;
        td.height = desc.height;
        td.usage = usage::render_target | usage::shader_read;
        NativeFormat nf;
        std::string err;
        if (!native_format(td.format, nf)) return nullptr;
        MTLTextureDescriptor* mtd = texture_descriptor(td, nf, err);
        id<MTLTexture> t = err.empty() ? [device_ newTextureWithDescriptor:mtd] : nil;
        if (!t) return nullptr;
        t.label = @"swapchain buffer";
        buffers.emplace_back(std::make_unique<MetalTexture>(this, t, td, nf, false, MemoryCategory::swapchain));
    }
    for (PlaneBlend b : {PlaneBlend::opaque, PlaneBlend::premultiplied, PlaneBlend::straight})
        if (!compose_pipeline(MTLPixelFormatBGRA8Unorm, b)) return nullptr;
    return std::make_unique<MetalSwapchain>(this, desc, layer, std::move(buffers));
}

std::unique_ptr<gpu::Device> create_device(const DeviceOptions& options) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return nullptr;
    return std::make_unique<MetalDevice>(device, options);
}

}  // namespace gta6rt::metal

namespace gta6rt::metal {

static MTLTextureSwizzle mtl_swizzle(gpu::Swizzle s, MTLTextureSwizzle identity) {
    switch (s) {
    case gpu::Swizzle::zero: return MTLTextureSwizzleZero;
    case gpu::Swizzle::one: return MTLTextureSwizzleOne;
    case gpu::Swizzle::r: return MTLTextureSwizzleRed;
    case gpu::Swizzle::g: return MTLTextureSwizzleGreen;
    case gpu::Swizzle::b: return MTLTextureSwizzleBlue;
    case gpu::Swizzle::a: return MTLTextureSwizzleAlpha;
    case gpu::Swizzle::identity: break;
    }
    return identity;
}

std::unique_ptr<gpu::Texture> MetalDevice::create_texture_view(gpu::Texture* texture, const gpu::TextureViewDesc& desc,
                                                               std::string* error) {
    auto fail = [&](const std::string& why) -> std::unique_ptr<gpu::Texture> {
        if (error) *error = why;
        return nullptr;
    };
    auto* mt = static_cast<MetalTexture*>(texture);
    if (!mt) return fail("no texture");
    id<MTLTexture> base = mt->storage();
    NativeFormat nf = mt->format();
    if (desc.format && desc.format != mt->desc().format && !native_format(desc.format, nf))
        return fail("view format " + std::to_string(desc.format) + " not mapped");
    // Metal aborts the process on a view it cannot make (no error object): a block-compressed texture only views as
    // itself or its sRGB/linear twin, and a view of another format has the texture's texel size.
    if (nf.pixel != base.pixelFormat) {
        const NativeFormat& src = mt->format();
        const bool compressed = src.info.block_dim > 1 || nf.info.block_dim > 1;
        if (compressed) {
            static const std::pair<MTLPixelFormat, MTLPixelFormat> twins[] = {
                {MTLPixelFormatBC1_RGBA, MTLPixelFormatBC1_RGBA_sRGB}, {MTLPixelFormatBC2_RGBA, MTLPixelFormatBC2_RGBA_sRGB},
                {MTLPixelFormatBC3_RGBA, MTLPixelFormatBC3_RGBA_sRGB}, {MTLPixelFormatBC7_RGBAUnorm, MTLPixelFormatBC7_RGBAUnorm_sRGB}};
            bool twin = false;
            for (const auto& [a, b] : twins)
                twin |= (base.pixelFormat == a && nf.pixel == b) || (base.pixelFormat == b && nf.pixel == a);
            if (!twin) return fail("a block-compressed texture views only as its own format or its sRGB twin");
        } else if (!src.info.depth && !nf.info.depth && src.info.block_bytes != nf.info.block_bytes) {
            return fail("a view of another format needs the texture's texel size");
        }
    }
    MTLTextureType type = MTLTextureType2D;
    switch (desc.dimension) {
    case TextureDimension::tex1d: type = base.textureType == MTLTextureType1DArray ? MTLTextureType1DArray : MTLTextureType1D; break;
    case TextureDimension::tex2d: type = base.sampleCount > 1 ? MTLTextureType2DMultisample : MTLTextureType2D; break;
    case TextureDimension::tex2d_array:
        type = base.sampleCount > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
        break;
    case TextureDimension::tex3d: type = MTLTextureType3D; break;
    case TextureDimension::cube: type = MTLTextureTypeCube; break;
    }
    const NSUInteger mips = base.mipmapLevelCount;
    const NSUInteger slices = base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray
                                  ? 6 * base.arrayLength
                              : base.textureType == MTLTextureType3D ? 1
                                                                     : base.arrayLength;
    if (desc.first_mip >= mips) return fail("first mip past the texture");
    const NSUInteger mip_count = desc.mip_count ? std::min<NSUInteger>(desc.mip_count, mips - desc.first_mip)
                                                : mips - desc.first_mip;
    if (desc.first_layer >= slices && type != MTLTextureType3D) return fail("first layer past the texture");
    NSUInteger layer_count = desc.layer_count ? desc.layer_count : slices - desc.first_layer;
    if (type == MTLTextureType3D) layer_count = 1;
    else layer_count = std::min<NSUInteger>(layer_count, slices - desc.first_layer);
    if (type == MTLTextureType2D || type == MTLTextureType1D || type == MTLTextureType2DMultisample) layer_count = 1;
    if (type == MTLTextureTypeCube) layer_count = 6;
    MTLTextureSwizzleChannels sw = nf.swizzled ? nf.swizzle : MTLTextureSwizzleChannelsDefault;
    sw.red = mtl_swizzle(desc.swizzle[0], sw.red);
    sw.green = mtl_swizzle(desc.swizzle[1], sw.green);
    sw.blue = mtl_swizzle(desc.swizzle[2], sw.blue);
    sw.alpha = mtl_swizzle(desc.swizzle[3], sw.alpha);
    id<MTLTexture> view = [base newTextureViewWithPixelFormat:nf.pixel
                                                  textureType:type
                                                       levels:NSMakeRange(desc.first_mip, mip_count)
                                                       slices:NSMakeRange(type == MTLTextureType3D ? 0 : desc.first_layer, layer_count)
                                                      swizzle:sw];
    if (!view) return fail("newTextureViewWithPixelFormat refused the view");
    TextureDesc td = mt->desc();
    td.dimension = desc.dimension;
    td.format = desc.format ? desc.format : td.format;
    td.mip_levels = std::uint32_t(mip_count);
    td.width = std::max(1u, td.width >> desc.first_mip);
    td.height = std::max(1u, td.height >> desc.first_mip);
    td.depth_or_layers = type == MTLTextureType3D ? std::max(1u, td.depth_or_layers >> desc.first_mip)
                                                  : std::uint32_t(layer_count);
    NativeFormat plain = nf;
    plain.swizzled = false;
    auto out = std::make_unique<MetalTexture>(this, view, td, plain, true, MemoryCategory::count);
    out->set_parent(base);
    return out;
}

bool make_resident(gpu::Device* device, gpu::Buffer* buffer) {
    if (!dynamic_cast<MetalDevice*>(device) || !buffer) return false;
    static_cast<MetalBuffer*>(buffer)->make_resident();
    return true;
}

bool set_encoder_resource(gpu::Device* device, gpu::Buffer* buffer, bool on) {
    auto* d = dynamic_cast<MetalDevice*>(device);
    if (!d || !buffer) return false;
    d->set_encoder_resource(static_cast<MetalBuffer*>(buffer)->mtl(), on);
    return true;
}

std::uint64_t encoder_resource_count(gpu::Device* device) {
    auto* d = dynamic_cast<MetalDevice*>(device);
    return d ? d->encoder_resource_count() : 0;
}

std::uint64_t encoder_resource_uses(gpu::Device* device) {
    auto* d = dynamic_cast<MetalDevice*>(device);
    return d ? d->encoder_resource_uses.load(std::memory_order_relaxed) : 0;
}

std::string memory_detail(gpu::Device* device, unsigned max_labels) {
    auto* d = dynamic_cast<MetalDevice*>(device);
    if (!d) return {};
    struct Row {
        std::uint64_t bytes = 0, count = 0;
    };
    std::map<std::string, Row> rows;
    d->va_space().visit([&](MetalBuffer* b) {
        if (b->placed() || b->category() == MemoryCategory::host_window) return;
        NSString* l = b->mtl().label;
        std::string key = l ? l.UTF8String : "(no label)";
        if (!b->resident()) key += " [not resident]";
        Row& r = rows[key];
        r.bytes += b->mtl().allocatedSize;
        ++r.count;
    });
    std::vector<std::pair<std::string, Row>> sorted(rows.begin(), rows.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    std::string out = "buffers by label (MiB, count):";
    std::uint64_t rest = 0, rest_n = 0;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (i >= max_labels) {
            rest += sorted[i].second.bytes, rest_n += sorted[i].second.count;
            continue;
        }
        char t[256];
        std::snprintf(t, sizeof t, "%s %s %.0f (%llu)", i ? "," : "", sorted[i].first.c_str(),
                      double(sorted[i].second.bytes) / 1048576.0, (unsigned long long)sorted[i].second.count);
        out += t;
    }
    const MemoryBudget mb = d->memory_budget();
    std::uint64_t categorised = 0;
    for (std::size_t i = 0; i < std::size_t(MemoryCategory::count); ++i)
        if (MemoryCategory(i) != MemoryCategory::host_window) categorised += mb.bytes[i];
    const std::uint64_t windows = mb.bytes[std::size_t(MemoryCategory::host_window)];
    const std::uint64_t other = mb.device_allocated > categorised + windows ? mb.device_allocated - categorised - windows : 0;
    const DeviceStats st = d->stats();
    char t[256];
    if (rest_n) {
        std::snprintf(t, sizeof t, ", %llu more labels' %.0f (%llu)", (unsigned long long)(sorted.size() - max_labels),
                      double(rest) / 1048576.0, (unsigned long long)rest_n);
        out += t;
    }
    std::snprintf(t, sizeof t,
                  "; Metal allocated outside every backend category %.0f MiB (pipeline code, libraries, encoders; "
                  "pipelines created %llu, compiled %llu)",
                  double(other) / 1048576.0, (unsigned long long)st.pipelines_created,
                  (unsigned long long)st.pipelines_compiled);
    out += t;
    return out;
}

}  // namespace gta6rt::metal
