#include "mtl_internal.h"

#include <mach/mach.h>
#include <malloc/malloc.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace gta6rt::metal {

std::string d24_quantize_probe_msl() { return msl::d24_probe_source(); }

MemoryBudget MetalDevice::memory_budget() const {
    MemoryBudget b;
    b.recommended_working_set = device_.recommendedMaxWorkingSetSize;
    b.device_allocated = device_.currentAllocatedSize;
    b.physical_memory = NSProcessInfo.processInfo.physicalMemory;
    task_vm_info_data_t info{};
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &n) == KERN_SUCCESS)
        b.process_footprint = info.phys_footprint;
    for (std::size_t i = 0; i < std::size_t(MemoryCategory::count); ++i) {
        b.bytes[i] = const_cast<MemoryLedger&>(ledger_).bytes(MemoryCategory(i));
        b.objects[i] = const_cast<MemoryLedger&>(ledger_).objects(MemoryCategory(i));
    }
    b.pressure = MemoryPressure(pressure_level_.load());
    b.pressure_events = pressure_events_.load();
    return b;
}

void MetalDevice::start_pressure_source() {
    pressure_queue_ = dispatch_queue_create("gta6rt.metal.memory-pressure", DISPATCH_QUEUE_SERIAL);
    pressure_source_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                                              DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
                                                  DISPATCH_MEMORYPRESSURE_CRITICAL,
                                              pressure_queue_);
    if (!pressure_source_) {
        log("memory pressure source unavailable");
        return;
    }
    dispatch_source_t src = pressure_source_;
    MetalDevice* self = this;
    dispatch_source_set_event_handler(src, ^{
      const unsigned long data = dispatch_source_get_data(src);
      const MemoryPressure level = (data & DISPATCH_MEMORYPRESSURE_CRITICAL) ? MemoryPressure::critical
                                   : (data & DISPATCH_MEMORYPRESSURE_WARN)   ? MemoryPressure::warning
                                                                             : MemoryPressure::normal;
      self->dispatch_pressure(level);
    });
    dispatch_resume(src);
    start_machine_guard();
}

namespace {
// GTA6RT_METAL_MALLOC_TRACE=1: once a second, every malloc zone's bytes in use and allocated, and the machine's wired and
// compressor pages (diagnostics for the footprint's growth).
void malloc_zone_trace(std::uint64_t wired_mb, std::uint64_t compressor_mb, std::uint64_t compressions) {
    vm_address_t* zones = nullptr;
    unsigned count = 0;
    if (malloc_get_all_zones(mach_task_self(), nullptr, &zones, &count) != KERN_SUCCESS) return;
    std::string text;
    std::uint64_t in_use = 0;
    for (unsigned i = 0; i < count; ++i) {
        auto* zone = reinterpret_cast<malloc_zone_t*>(zones[i]);
        malloc_statistics_t st{};
        malloc_zone_statistics(zone, &st);
        in_use += st.size_in_use;
        if (st.size_allocated < (1u << 20)) continue;
        const char* name = malloc_get_zone_name(zone);
        char t[160];
        std::snprintf(t, sizeof t, "%s%s %.0f in use / %.0f allocated (%u blocks)", text.empty() ? "" : ", ",
                      name ? name : "?", double(st.size_in_use) / 1048576.0, double(st.size_allocated) / 1048576.0,
                      st.blocks_in_use);
        text += t;
    }
    std::fprintf(stderr, "[metal] malloc trace t=%.0fs: in use %.0f MiB: %s; machine wired %llu MiB, compressor %llu MiB "
                         "(%llu compressions)\n",
                 now_seconds(), double(in_use) / 1048576.0, text.c_str(), (unsigned long long)wired_mb,
                 (unsigned long long)compressor_mb, (unsigned long long)compressions);
}
}  // namespace

// GTA6RT_METAL_MACHINE_GUARD_MB (default 15360, 0: off): 4 times a second, the machine's wired pages plus the pages the
// compressor occupies; over the limit the process ends (exit 6). Wired GPU memory cannot be compressed or paged, and the
// 03:33 kernel panic came at 10.7 GiB wired + 5.3 GiB compressed (16.0 GiB) with 96 MiB free; the worst world run
// before this guard reached 9.37 + 4.73 = 14.1 GiB.
void MetalDevice::start_machine_guard() {
    const char* v = std::getenv("GTA6RT_METAL_MACHINE_GUARD_MB");
    const std::uint64_t limit_mb = v && *v ? std::strtoull(v, nullptr, 10) : 15360;
    if (!limit_mb || !pressure_queue_) return;
    machine_guard_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, pressure_queue_);
    if (!machine_guard_) {
        log("machine guard unavailable");
        return;
    }
    dispatch_source_set_timer(machine_guard_, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), 250 * NSEC_PER_MSEC,
                              20 * NSEC_PER_MSEC);
    const host_t host = mach_host_self();
    const std::uint64_t page = vm_kernel_page_size;
    const bool malloc_trace = env_flag("GTA6RT_METAL_MALLOC_TRACE");
    __block unsigned ticks = 0;
    dispatch_source_set_event_handler(machine_guard_, ^{
      vm_statistics64_data_t vs{};
      mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
      if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vs), &n) != KERN_SUCCESS) return;
      const std::uint64_t wired = std::uint64_t(vs.wire_count) * page >> 20;
      const std::uint64_t compressor = std::uint64_t(vs.compressor_page_count) * page >> 20;
      if (malloc_trace && ++ticks % 4 == 0) malloc_zone_trace(wired, compressor, std::uint64_t(vs.compressions));
      if (wired + compressor <= limit_mb) return;
      std::fprintf(stderr, "[metal] machine guard: wired %llu MiB + compressor %llu MiB = %llu MiB over "
                           "GTA6RT_METAL_MACHINE_GUARD_MB %llu: ending the run (exit 6)\n",
                   (unsigned long long)wired, (unsigned long long)compressor, (unsigned long long)(wired + compressor),
                   (unsigned long long)limit_mb);
      std::fflush(stderr);
      std::_Exit(6);
    });
    dispatch_resume(machine_guard_);
}

void MetalDevice::dispatch_pressure(MemoryPressure level) {
    pressure_level_ = std::uint32_t(level);
    pressure_events_++;
    std::vector<std::function<void(MemoryPressure)>> handlers;
    {
        std::lock_guard lock(pressure_mutex_);
        for (auto& [token, h] : pressure_handlers_) handlers.push_back(h);
    }
    for (auto& h : handlers) h(level);
}

std::uint64_t MetalDevice::add_memory_pressure_handler(std::function<void(MemoryPressure)> handler) {
    std::lock_guard lock(pressure_mutex_);
    const std::uint64_t token = next_pressure_token_++;
    pressure_handlers_[token] = std::move(handler);
    return token;
}

void MetalDevice::remove_memory_pressure_handler(std::uint64_t token) {
    std::lock_guard lock(pressure_mutex_);
    pressure_handlers_.erase(token);
}

void MetalDevice::simulate_memory_pressure(MemoryPressure level) {
    if (!pressure_queue_) {
        dispatch_pressure(level);
        return;
    }
    MetalDevice* self = this;
    dispatch_sync(pressure_queue_, ^{
      self->dispatch_pressure(level);
    });
}

std::unique_ptr<gpu::Buffer> MetalDevice::create_host_buffer(const HostBufferDesc& desc, std::string* error) {
    const std::uint64_t page = caps_.host_page_size ? caps_.host_page_size : 4096;
    auto fail = [&](const std::string& e) -> std::unique_ptr<gpu::Buffer> {
        if (error) *error = e;
        return nullptr;
    };
    if (!desc.cpu || !desc.size) return fail("no pages");
    if (reinterpret_cast<std::uintptr_t>(desc.cpu) % page || desc.size % page)
        return fail("pointer and size must be multiples of the " + std::to_string(page) + "-byte page");
    if (desc.size > caps_.max_buffer_length) return fail("larger than maxBufferLength");
    const MTLResourceOptions opts = MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked;
    id<MTLBuffer> b = [device_ newBufferWithBytesNoCopy:desc.cpu length:desc.size options:opts deallocator:nil];
    if (!b) return fail("newBufferWithBytesNoCopy refused the range");
    if (desc.label) b.label = @(desc.label);
    const std::uint64_t va = desc.va ? desc.va : va_.allocate(desc.size, 1ull << 16);
    auto out = std::make_unique<MetalBuffer>(this, b, va, desc.size, false, false, MemoryCategory::host_window, desc.resident);
    if (!desc.va) out->own_va();
    return out;
}

std::unique_ptr<gpu::Texture> MetalDevice::create_buffer_texture(gpu::Buffer* buffer, std::uint64_t offset,
                                                                 std::uint32_t row_pitch, const TextureDesc& desc,
                                                                 std::string* error) {
    auto fail = [&](const std::string& e) -> std::unique_ptr<gpu::Texture> {
        if (error) *error = e;
        return nullptr;
    };
    auto* b = static_cast<MetalBuffer*>(buffer);
    NativeFormat nf;
    if (!b || !native_format(desc.format, nf)) return fail("buffer and a mapped format");
    if (desc.dimension != TextureDimension::tex2d || desc.mip_levels > 1 || desc.samples > 1 || nf.info.block_dim > 1 ||
        nf.info.depth)
        return fail("linear textures are 2D, one mip, uncompressed colour");
    const NSUInteger align = [device_ minimumLinearTextureAlignmentForPixelFormat:nf.pixel];
    if (row_pitch % align || offset % align) return fail("row pitch and offset must be multiples of " + std::to_string(align));
    if (row_pitch < desc.width * nf.info.block_bytes) return fail("row pitch below the row size");
    if (offset + std::uint64_t(row_pitch) * desc.height > b->size()) return fail("texture does not fit the buffer");
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:nf.pixel
                                                                                  width:desc.width
                                                                                 height:desc.height
                                                                              mipmapped:NO];
    td.storageMode = b->mtl().storageMode;
    td.cpuCacheMode = b->mtl().cpuCacheMode;
    td.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    MTLTextureUsage u = MTLTextureUsageShaderRead;
    if (desc.usage & usage::unordered_access) u |= MTLTextureUsageShaderWrite;
    if (desc.usage & usage::render_target) u |= MTLTextureUsageRenderTarget;
    if (desc.usage & usage::pixel_format_view) u |= MTLTextureUsagePixelFormatView;
    td.usage = u;
    id<MTLTexture> t = [b->mtl() newTextureWithDescriptor:td offset:offset bytesPerRow:row_pitch];
    if (!t) return fail("newTextureWithDescriptor:offset:bytesPerRow: refused");
    if (desc.label) t.label = @(desc.label);
    TextureDesc d = desc;
    d.memory = b->cpu_pointer() ? MemoryKind::shared : MemoryKind::device_local;
    return std::make_unique<MetalTexture>(this, t, d, nf, false, MemoryCategory::count);
}

std::unique_ptr<gpu::QueryHeap> MetalDevice::create_query_heap(const QueryHeapDesc& desc) {
    if (!desc.count) return nullptr;
    id<MTLBuffer> results = nil;
    id<MTLCounterSampleBuffer> samples = nil;
    if (desc.type == QueryType::timestamp) {
        if (!timestamp_set_ || !caps_.counters_stage_boundary) {
            log("timestamp queries: no timestamp counter set with stage-boundary sampling");
            return nullptr;
        }
        MTLCounterSampleBufferDescriptor* sd = [MTLCounterSampleBufferDescriptor new];
        sd.counterSet = timestamp_set_;
        sd.sampleCount = desc.count;
        sd.storageMode = MTLStorageModeShared;
        if (desc.label) sd.label = @(desc.label);
        NSError* e = nil;
        samples = [device_ newCounterSampleBufferWithDescriptor:sd error:&e];
        if (!samples) {
            log("counter sample buffer: %s", e.localizedDescription.UTF8String);
            return nullptr;
        }
    } else {
        const std::size_t per = desc.type == QueryType::pipeline_statistics ? 88 : 8;
        results = [device_ newBufferWithLength:std::size_t(desc.count) * per options:MTLResourceStorageModeShared];
        if (!results) return nullptr;
        if (desc.label) results.label = @(desc.label);
    }
    return std::make_unique<MetalQueryHeap>(this, desc, results, samples);
}

}  // namespace gta6rt::metal
