#include "metal_present_planes.h"

#include "present_dump.h"

#include <algorithm>
#include <cstdio>

namespace leonida {

namespace {
constexpr std::int32_t kOk = 0;
constexpr std::int32_t kFail = std::int32_t(0x80004005u);           // E_FAIL
constexpr std::int32_t kInvalidArg = std::int32_t(0x80070057u);     // E_INVALIDARG
constexpr std::int32_t kDeviceRemoved = std::int32_t(0x887A0005u);  // DXGI_ERROR_DEVICE_REMOVED
}  // namespace

struct MetalPresenter::Dump {
    present_dump::PresentDump writer;
    std::vector<std::uint8_t> pixels;
};

MetalPresenter::MetalPresenter(MetalContext& context, MetalQueue& queue, void* layer,
                               std::function<bool(std::uint32_t&, std::uint32_t&)> extent)
    : context_(context), queue_(queue), layer_(layer), extent_(std::move(extent)), dump_(std::make_unique<Dump>()) {
    gpu::TextureDesc d;
    d.width = d.height = 1;
    d.format = gpu::format::r8g8b8a8_unorm;
    d.memory = gpu::MemoryKind::shared;
    d.usage = gpu::usage::shader_read;
    d.label = "present black";
    black_ = context_.gpu().create_texture(d);
    if (black_) {
        const std::uint32_t texel = 0xff000000u;
        black_->write_region(0, 0, 0, 0, 1, 1, &texel, 4);
    }
    ensure_swapchain();
}

MetalPresenter::~MetalPresenter() { queue_.idle(2'000'000'000ull); }

bool MetalPresenter::ensure_swapchain() {
    std::uint32_t w = 0, h = 0;
    if (extent_ && !extent_(w, h)) w = h = 0;
    if (!w || !h) {
        if (swapchain_) return true;
        w = 1920, h = 1080;
    }
    if (swapchain_ && w == width_ && h == height_) return true;
    gpu::SwapchainDesc d;
    d.layer = layer_;
    d.width = w;
    d.height = h;
    d.buffer_count = 2;
    d.keep_last_frame = dump_->writer.enabled();
    if (swapchain_) queue_.idle(2'000'000'000ull);
    swapchain_ = context_.gpu().create_swapchain(d);
    width_ = w;
    height_ = h;
    if (!swapchain_) std::fprintf(stderr, "[rt] d3d12-metal: no swapchain on the window's layer (%ux%u)\n", w, h);
    else std::fprintf(stderr, "[rt] d3d12-metal: swapchain %ux%u on the window's CAMetalLayer\n", w, h);
    return swapchain_ != nullptr;
}

std::int32_t MetalPresenter::submit(const gpu::PresentDesc& desc) {
    if (abort_ && abort_()) return kOk;
    if (!ensure_swapchain()) return kFail;
    gpu::Fence* own = nullptr;
    for (const auto& [fence, value] : context_.submitted())
        if (value) queue_.queue()->wait(fence, value), own = fence;
    (void)own;
    const gpu::Status s = queue_.queue()->present_planes(swapchain_.get(), desc);
    if (s == gpu::Status::device_lost) {
        context_.mark_device_lost("present");
        return kDeviceRemoved;
    }
    if (s == gpu::Status::timeout) return kOk;
    if (s != gpu::Status::ok) {
        std::fprintf(stderr, "[rt] d3d12-metal: present failed: %s\n", gpu::status_name(s));
        return kFail;
    }
    ++presented_;
    context_.stats().presents.fetch_add(1, std::memory_order_relaxed);
    submit_phases::totals().presents.fetch_add(1, std::memory_order_relaxed);
    submit_phases::totals().present_serial.fetch_add(1, std::memory_order_relaxed);
    after_present();
    return kOk;
}

void MetalPresenter::after_present() {
    auto& w = dump_->writer;
    if (!w.enabled() || !w.due(presented_ - 1)) return;
    std::uint32_t pw = 0, ph = 0;
    if (swapchain_->read_last_frame(dump_->pixels, pw, ph) != gpu::Status::ok || !pw || !ph) return;
    w.save(presented_, VkExtent2D{pw, ph}, VK_FORMAT_B8G8R8A8_UNORM, false, dump_->pixels.data(), dump_->pixels.size());
}

std::int32_t MetalPresenter::present(ResourceId source, std::uint32_t sync_interval, const PresentRect* rect) {
    MetalBinding b;
    if (!context_.binding(source, b) || !b.texture) return kInvalidArg;
    gpu::PresentDesc d;
    d.plane_count = 1;
    d.sync_interval = sync_interval;
    d.planes[0].source = b.texture;
    d.planes[0].blend = gpu::PlaneBlend::opaque;
    if (rect) d.planes[0].src = {rect->left, rect->top, rect->right, rect->bottom};
    return submit(d);
}

std::int32_t MetalPresenter::present_blank(std::uint32_t sync_interval) {
    if (!black_) return kFail;
    gpu::PresentDesc d;
    d.plane_count = 1;
    d.sync_interval = sync_interval;
    d.planes[0].source = black_.get();
    d.planes[0].blend = gpu::PlaneBlend::opaque;
    d.planes[0].filter = gpu::Filter::point;
    return submit(d);
}

std::int32_t MetalPresenter::present_planes(const PresentPlanes& planes, std::uint32_t sync_interval) {
    if (!planes.count || planes.count > 2) return kInvalidArg;
    if (!ensure_swapchain()) return kFail;
    gpu::PresentDesc d;
    d.sync_interval = sync_interval;
    for (std::uint32_t i = 0; i < planes.count; ++i) {
        const PresentPlane& p = planes.plane[i];
        MetalBinding b;
        if (!context_.binding(p.source, b) || !b.texture) return kInvalidArg;
        if (p.hdr10) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true))
                std::fprintf(stderr, "[rt] d3d12-metal: an HDR10 plane is shown without PQ decoding (not implemented)\n");
        }
        gpu::PresentPlane& out = d.planes[d.plane_count++];
        out.source = b.texture;
        if (p.has_source_rect) out.src = {p.source_rect.left, p.source_rect.top, p.source_rect.right, p.source_rect.bottom};
        const auto px = [](float f, std::uint32_t n) { return std::int32_t(std::clamp(f, 0.0f, 1.0f) * float(n) + 0.5f); };
        const bool whole = p.dest[0] <= 0 && p.dest[1] <= 0 && p.dest[2] >= 1 && p.dest[3] >= 1;
        if (!whole) out.dst = {px(p.dest[0], width_), px(p.dest[1], height_), px(p.dest[2], width_), px(p.dest[3], height_)};
        out.filter = p.point_filter ? gpu::Filter::point : gpu::Filter::linear;
        out.blend = i == 0 ? gpu::PlaneBlend::opaque
                    : p.straight_alpha ? gpu::PlaneBlend::straight
                                       : gpu::PlaneBlend::premultiplied;
    }
    if (d.plane_count > 1) {
        ++composed_;
        context_.stats().composed_presents.fetch_add(1, std::memory_order_relaxed);
    }
    return submit(d);
}

bool MetalPresenter::read_back(std::vector<std::uint8_t>& rgba, std::uint32_t& width, std::uint32_t& height) {
    if (!swapchain_ || swapchain_->read_last_frame(rgba, width, height) != gpu::Status::ok) return false;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
    return true;
}

}  // namespace leonida
