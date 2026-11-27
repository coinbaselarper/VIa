#include "mtl_internal.h"

#include <cstring>

namespace gta6rt::metal {

std::uint64_t VaSpace::allocate(std::uint64_t size, std::uint64_t align) {
    std::lock_guard lock(mutex_);
    if (!align) align = 1;
    std::uint64_t va = (next_ + align - 1) & ~(align - 1);
    if (va + size > end_ || va + size < va) return 0;
    next_ = va + size;
    return va;
}

void VaSpace::add(std::uint64_t va, std::uint64_t size, MetalBuffer* buffer) {
    std::lock_guard lock(mutex_);
    ranges_.emplace(va, Entry{size, buffer});
}

void VaSpace::remove(std::uint64_t va, MetalBuffer* buffer) {
    std::lock_guard lock(mutex_);
    auto [lo, hi] = ranges_.equal_range(va);
    for (auto it = lo; it != hi; ++it) {
        if (it->second.buffer == buffer) {
            ranges_.erase(it);
            return;
        }
    }
}

bool VaSpace::resolve(std::uint64_t va, MetalBuffer** buffer, std::uint64_t* offset) const {
    std::lock_guard lock(mutex_);
    auto it = ranges_.upper_bound(va);
    while (it != ranges_.begin()) {
        --it;
        if (va - it->first < it->second.size) {
            *buffer = it->second.buffer;
            *offset = va - it->first;
            return true;
        }
    }
    return false;
}

MetalBuffer::MetalBuffer(MetalDevice* device, id<MTLBuffer> buffer, std::uint64_t va, std::uint64_t size, bool placed,
                         bool tracked, MemoryCategory category, bool resident)
    : device_(device), buffer_(buffer), va_(va), size_(size), placed_(placed), tracked_(tracked), category_(category) {
    if (va_) device_->va_space().add(va_, size_, this);
    if (!placed_) {
        resident_ = resident;
        if (resident) device_->make_resident(buffer_);
        accounted_ = category_ == MemoryCategory::host_window ? size_ : buffer_.allocatedSize;
        device_->ledger().add(category_, accounted_);
    }
}

void MetalBuffer::make_resident() {
    if (placed_ || resident_.exchange(true)) return;
    device_->make_resident(buffer_);
}

MetalBuffer::~MetalBuffer() {
    if (va_) device_->va_space().remove(va_, this);
    device_->retire(buffer_, placed_ ? nil : buffer_);
    if (!placed_) device_->ledger().remove(category_, accounted_);
}

void* MetalBuffer::cpu_pointer() {
    return buffer_.storageMode == MTLStorageModePrivate ? nullptr : buffer_.contents;
}

MetalTexture::MetalTexture(MetalDevice* device, id<MTLTexture> texture, const TextureDesc& desc,
                           const NativeFormat& format, bool placed, MemoryCategory category)
    : device_(device), texture_(texture), desc_(desc), format_(format), placed_(placed), category_(category) {
    desc_.label = nullptr;
    if (format_.swizzled) {
        const NSUInteger slices = texture_.textureType == MTLTextureTypeCube ? 6 * texture_.arrayLength
                                  : texture_.textureType == MTLTextureType3D  ? 1
                                                                              : texture_.arrayLength;
        view_ = [texture_ newTextureViewWithPixelFormat:texture_.pixelFormat
                                            textureType:texture_.textureType
                                                 levels:NSMakeRange(0, texture_.mipmapLevelCount)
                                                 slices:NSMakeRange(0, slices)
                                                swizzle:format_.swizzle];
    }
    if (!placed_) {
        device_->make_resident(texture_);
        if (category_ != MemoryCategory::count) {
            accounted_ = texture_.allocatedSize;
            device_->ledger().add(category_, accounted_);
        }
    }
}

MetalTexture::~MetalTexture() {
    device_->retire(texture_, placed_ ? nil : texture_);
    if (view_) device_->retire(view_);
    if (!placed_ && category_ != MemoryCategory::count) device_->ledger().remove(category_, accounted_);
}

Status MetalTexture::write_region(std::uint32_t mip, std::uint32_t layer, std::uint32_t x, std::uint32_t y,
                                  std::uint32_t width, std::uint32_t height, const void* data, std::uint32_t row_pitch) {
    if (texture_.storageMode == MTLStorageModePrivate) return Status::unsupported;
    if (!data || !width || !height) return Status::invalid_argument;
    if (texture_.textureType == MTLTextureType3D) {
        [texture_ replaceRegion:MTLRegionMake3D(x, y, layer, width, height, 1)
                    mipmapLevel:mip
                          slice:0
                      withBytes:data
                    bytesPerRow:row_pitch
                  bytesPerImage:std::size_t(row_pitch) * height];
        return Status::ok;
    }
    [texture_ replaceRegion:MTLRegionMake2D(x, y, width, height)
                mipmapLevel:mip
                      slice:layer
                  withBytes:data
                bytesPerRow:row_pitch
              bytesPerImage:0];
    return Status::ok;
}

MetalHeap::MetalHeap(MetalDevice* device, id<MTLHeap> heap, std::uint64_t va_base, std::unique_ptr<MetalBuffer> arena,
                     MemoryKind memory)
    : device_(device), heap_(heap), va_base_(va_base), arena_(std::move(arena)), memory_(memory) {
    device_->make_resident(heap_);
    accounted_ = heap_.size;
    device_->ledger().add(MemoryCategory::heap, accounted_);
}

MetalHeap::~MetalHeap() {
    arena_.reset();
    device_->retire(heap_, heap_);
    device_->ledger().remove(MemoryCategory::heap, accounted_);
}

MetalDescriptorHeap::MetalDescriptorHeap(MetalDevice* device, const DescriptorHeapDesc& desc, id<MTLBuffer> buffer,
                                         id<MTLResidencySet> residency)
    : device_(device), kind_(desc.kind), count_(desc.count), buffer_(buffer), residency_(residency),
      kept_(desc.count, nil), slot_allocation_(desc.count, nil) {
    std::memset(buffer_.contents, 0, std::size_t(desc.count) * 8);
    accounted_ = buffer_.allocatedSize;
    device_->ledger().add(MemoryCategory::descriptor_table, accounted_);
}

MetalDescriptorHeap::~MetalDescriptorHeap() {
    device_->retire(buffer_);
    if (residency_) device_->retire(residency_);
    for (id object : kept_)
        if (object) device_->retire(object);
    device_->ledger().remove(MemoryCategory::descriptor_table, accounted_);
}

void MetalDescriptorHeap::set_slot(std::uint32_t slot, std::uint64_t raw, id object, id<MTLAllocation> allocation) {
    std::lock_guard lock(mutex_);
    static_cast<std::uint64_t*>(buffer_.contents)[slot] = raw;
    if (kept_[slot] != object) {
        if (kept_[slot]) device_->retire(kept_[slot]);
        kept_[slot] = object;
    }
    id<MTLAllocation> old = slot_allocation_[slot];
    if (old == allocation) return;
    if (old) {
        auto it = allocation_refs_.find((__bridge void*)old);
        if (it != allocation_refs_.end() && --it->second == 0) {
            allocation_refs_.erase(it);
            if (residency_) pending_removals_.push_back({old, device_->retire_marks()});
        }
    }
    slot_allocation_[slot] = allocation;
    if (allocation) {
        if (allocation_refs_[(__bridge void*)allocation]++ == 0) {
            if (residency_) [residency_ addAllocation:allocation];
            dirty_ = true;
        }
    }
}

static id<MTLAllocation> allocation_of(id<MTLResource> resource) {
    if (resource.heap) return resource.heap;
    return resource;
}

Status MetalDescriptorHeap::write_texture(std::uint32_t slot, gpu::Texture* texture) {
    if (slot >= count_ || kind_ != DescriptorKind::texture || !texture) return Status::invalid_argument;
    auto* mt = static_cast<MetalTexture*>(texture);
    id<MTLTexture> t = mt->storage();
    id<MTLTexture> v = mt->sample_view();
    set_slot(slot, v.gpuResourceID._impl, v, allocation_of(t));
    return Status::ok;
}

Status MetalDescriptorHeap::write_sampler(std::uint32_t slot, gpu::Sampler* sampler) {
    if (slot >= count_ || kind_ != DescriptorKind::sampler || !sampler) return Status::invalid_argument;
    id<MTLSamplerState> s = static_cast<MetalSampler*>(sampler)->mtl();
    set_slot(slot, s.gpuResourceID._impl, s, nil);
    return Status::ok;
}

Status MetalDescriptorHeap::write_buffer(std::uint32_t slot, gpu::Buffer* buffer, std::uint64_t offset) {
    if (slot >= count_ || kind_ != DescriptorKind::buffer || !buffer) return Status::invalid_argument;
    id<MTLBuffer> b = static_cast<MetalBuffer*>(buffer)->mtl();
    if (offset >= b.length) return Status::invalid_argument;
    set_slot(slot, b.gpuAddress + offset, b, allocation_of(b));
    return Status::ok;
}

Status MetalDescriptorHeap::clear(std::uint32_t slot) {
    if (slot >= count_) return Status::invalid_argument;
    set_slot(slot, 0, nil, nil);
    return Status::ok;
}

Status MetalDescriptorHeap::copy(std::uint32_t dst_slot, const gpu::DescriptorHeap& src, std::uint32_t src_slot,
                                 std::uint32_t n) {
    const auto& s = static_cast<const MetalDescriptorHeap&>(src);
    if (s.kind_ != kind_ || dst_slot + n > count_ || src_slot + n > s.count_) return Status::invalid_argument;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint64_t raw;
        id object;
        id<MTLAllocation> allocation;
        {
            std::lock_guard lock(s.mutex_);
            raw = static_cast<const std::uint64_t*>(s.buffer_.contents)[src_slot + i];
            object = s.kept_[src_slot + i];
            allocation = s.slot_allocation_[src_slot + i];
        }
        set_slot(dst_slot + i, raw, object, allocation);
    }
    return Status::ok;
}

std::uint64_t MetalDescriptorHeap::read_raw(std::uint32_t slot) const {
    if (slot >= count_) return 0;
    std::lock_guard lock(mutex_);
    return static_cast<const std::uint64_t*>(buffer_.contents)[slot];
}

id<MTLResidencySet> MetalDescriptorHeap::residency_for_submit() {
    std::lock_guard lock(mutex_);
    if (!residency_) return nil;
    while (!pending_removals_.empty() && MetalDevice::marks_reached(pending_removals_.front().marks)) {
        id<MTLAllocation> a = pending_removals_.front().allocation;
        pending_removals_.pop_front();
        if (!allocation_refs_.count((__bridge void*)a)) {
            [residency_ removeAllocation:a];
            dirty_ = true;
        }
    }
    if (dirty_) {
        [residency_ commit];
        dirty_ = false;
    }
    return residency_;
}

MetalQueryHeap::MetalQueryHeap(MetalDevice* device, const QueryHeapDesc& desc, id<MTLBuffer> results,
                               id<MTLCounterSampleBuffer> samples)
    : device_(device), type_(desc.type), count_(desc.count), results_(results), samples_(samples) {
    if (results_) {
        std::memset(results_.contents, 0, results_.length);
        accounted_ = results_.allocatedSize;
        device_->make_resident(results_);
    } else {
        accounted_ = std::uint64_t(count_) * 8;
    }
    device_->ledger().add(MemoryCategory::internal, accounted_);
}

MetalQueryHeap::~MetalQueryHeap() {
    if (results_) device_->retire(results_, results_);
    if (samples_) device_->retire(samples_);
    device_->ledger().remove(MemoryCategory::internal, accounted_);
}

}  // namespace gta6rt::metal
