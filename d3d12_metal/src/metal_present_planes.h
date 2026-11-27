#pragma once

// Stands in for d3d12_vulkan/src/vulkan_present.h in this module (clang VFS overlay, CMakeLists.txt): the plane
// structs xbox_present.cpp fills, copied unchanged from that header (ORIGIN.md), and the Metal presenter instead of
// VulkanPresenter.

#include "metal_context.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace leonida {

struct PresentRect {
    std::int32_t left, top, right, bottom;
};

struct PresentPlane {
    ResourceId source{};
    PresentRect source_rect{};
    bool has_source_rect = false;
    float dest[4] = {0, 0, 1, 1};
    bool point_filter = false;
    bool hdr10 = false;
    bool straight_alpha = false;
};

struct PresentPlanes {
    PresentPlane plane[2];
    std::uint32_t count = 1;
    float sdr_white_nits = 203.0f;
    enum class SdrEotf : std::uint32_t { srgb = 0, gamma22 = 1 };
    SdrEotf sdr_eotf = SdrEotf::srgb;
    float hdr_peak_nits = 1000.0f;
};

// Puts textures of the device on screen through a gpu::Swapchain on the window's CAMetalLayer (ADAPTER.md section 3).
class MetalPresenter {
public:
    MetalPresenter(MetalContext& context, MetalQueue& queue, void* layer, std::function<bool(std::uint32_t&, std::uint32_t&)> extent);
    ~MetalPresenter();
    bool ok() const { return swapchain_ != nullptr; }

    // `source` is a colour texture; rect (when not null) is the part of it shown. Returns a D3D12/DXGI HRESULT value.
    std::int32_t present(ResourceId source, std::uint32_t sync_interval, const PresentRect* rect);
    std::int32_t present_blank(std::uint32_t sync_interval);
    std::int32_t present_planes(const PresentPlanes& planes, std::uint32_t sync_interval);
    void set_abort(std::function<bool()> abort) { abort_ = std::move(abort); }
    bool read_back(std::vector<std::uint8_t>& rgba, std::uint32_t& width, std::uint32_t& height);
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint64_t presented() const { return presented_; }
    std::uint64_t composed() const { return composed_; }

private:
    bool ensure_swapchain();
    std::int32_t submit(const gpu::PresentDesc& desc);
    void after_present();

    MetalContext& context_;
    MetalQueue& queue_;
    void* layer_;
    std::function<bool(std::uint32_t&, std::uint32_t&)> extent_;
    std::function<bool()> abort_;
    std::unique_ptr<gpu::Swapchain> swapchain_;
    std::unique_ptr<gpu::Texture> black_;
    std::uint32_t width_ = 0, height_ = 0;
    std::uint64_t presented_ = 0, composed_ = 0;
    struct Dump;
    std::unique_ptr<Dump> dump_;
    struct Check;
    std::unique_ptr<Check> check_;
    void check_plane(gpu::Texture* source);
};

}  // namespace leonida
