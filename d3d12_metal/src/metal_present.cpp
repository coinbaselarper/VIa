#include "metal_present_planes.h"

#include "present_dump.h"
#include "upscale.h"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace leonida {

namespace {
constexpr std::int32_t kOk = 0;
constexpr std::int32_t kFail = std::int32_t(0x80004005u);           // E_FAIL
constexpr std::int32_t kInvalidArg = std::int32_t(0x80070057u);     // E_INVALIDARG
constexpr std::int32_t kDeviceRemoved = std::int32_t(0x887A0005u);  // DXGI_ERROR_DEVICE_REMOVED
}  // namespace

// GTA6RT_METAL_WORLD_PRESENT_DUMP=dir[,every[,count[,first]]]: once the scene probe has seen an upscaled frame (the
// world; with `first`, that many upscaled frames), every `every`-th presented frame (default 10) is written as
// dir/world_present_<frame>.png, `count` times (default 6).
struct MetalPresenter::Dump {
    present_dump::PresentDump writer;
    std::vector<std::uint8_t> pixels;
    std::string world_dir;
    std::uint64_t world_every = 10, world_count = 6, world_done = 0, world_seen = 0, world_first = 1;
    Dump() {
        const char* v = std::getenv("GTA6RT_METAL_WORLD_PRESENT_DUMP");
        if (!v || !*v) return;
        std::string s = v;
        const std::size_t c = s.find(',');
        world_dir = s.substr(0, c);
        if (c != std::string::npos) {
            unsigned long long e = 0, n = 0, f = 0;
            const int got = std::sscanf(s.c_str() + c + 1, "%llu,%llu,%llu", &e, &n, &f);
            if (got >= 1 && e) world_every = e;
            if (got >= 2) world_count = n;
            if (got >= 3 && f) world_first = f;
        }
        ::mkdir(world_dir.c_str(), 0755);
    }
    bool world_due(SceneProbe* scene) {
        if (world_dir.empty() || world_done >= world_count || !scene || scene->upscaled_frames() < world_first)
            return false;
        return world_seen++ % world_every == 0;
    }
};

// GTA6RT_METAL_PRESENT_CHECK=1[,dir]: once the host upscaler has run (the world), plane 0 of every present is copied
// to a shared buffer behind the present and inspected when that copy's fence completes, so the present path never
// waits for the GPU. A present is black when under 5% of its texels have a channel >= 4, dark when its mean luma < 8.
struct MetalPresenter::Check {
    struct Slot {
        std::unique_ptr<gpu::Buffer> buffer;
        std::unique_ptr<gpu::CommandList> list;
        std::uint32_t width = 0, height = 0, pitch = 0;
        std::uint64_t present = 0, upscaled = 0;
        std::atomic<bool> busy{false};
    };
    struct State {
        std::mutex mutex;
        std::string dir;
        std::uint64_t checked = 0, black = 0, dark = 0, run = 0, longest = 0, missed = 0, dumped_black = 0, idle = 0;
        std::array<Slot, 3> slots;
    };
    std::shared_ptr<State> state;
    std::unique_ptr<gpu::Fence> fence;
    std::uint64_t value = 0, last_upscaled = 0;
    bool told_format = false;

    static std::unique_ptr<Check> create(gpu::Device& device) {
        const char* v = std::getenv("GTA6RT_METAL_PRESENT_CHECK");
        if (!v || !*v || *v == '0') return nullptr;
        auto c = std::make_unique<Check>();
        c->state = std::make_shared<State>();
        if (const char* comma = std::strchr(v, ',')) {
            c->state->dir = comma + 1;
            ::mkdir(c->state->dir.c_str(), 0755);
        }
        c->fence = device.create_fence(0);
        if (!c->fence) return nullptr;
        std::fprintf(stderr, "[rt] d3d12-metal present check: plane 0 of every world present is inspected%s%s\n",
                     c->state->dir.empty() ? "" : "; black ones and every 20th written to ", c->state->dir.c_str());
        return c;
    }

    static void inspect(State& s, Slot& slot) {
        const auto* px = static_cast<const std::uint8_t*>(slot.buffer->cpu_pointer());
        const std::uint64_t n = std::uint64_t(slot.width) * slot.height;
        std::uint64_t lit = 0;
        double sum[3] = {0, 0, 0};
        for (std::uint32_t y = 0; y < slot.height; ++y) {
            const std::uint8_t* row = px + std::size_t(y) * slot.pitch;
            for (std::uint32_t x = 0; x < slot.width; ++x) {
                const std::uint8_t* p = row + std::size_t(x) * 4;
                lit += std::max({p[0], p[1], p[2]}) >= 4;
                sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
            }
        }
        const double share = n ? double(lit) / double(n) : 0.0;
        const double m0 = sum[0] / double(n ? n : 1), m1 = sum[1] / double(n ? n : 1), m2 = sum[2] / double(n ? n : 1);
        const double luma = 0.2126 * m0 + 0.7152 * m1 + 0.0722 * m2;
        const bool black = share < 0.05, dark = !black && luma < 8.0;
        std::string file;
        {
            std::lock_guard lock(s.mutex);
            ++s.checked;
            s.black += black;
            s.dark += dark;
            s.run = black ? 0 : s.run + 1;
            s.longest = std::max(s.longest, s.run);
            if (!s.dir.empty() && ((black && s.dumped_black < 8) || s.checked % 20 == 1)) {
                s.dumped_black += black;
                char name[80];
                std::snprintf(name, sizeof name, "/present_check_%06llu%s.png",
                              static_cast<unsigned long long>(slot.present), black ? "_black" : "");
                file = s.dir + name;
            }
            std::fprintf(stderr,
                         "[rt] d3d12-metal present check %llu (present %llu, upscaled %llu): %.1f%% lit, mean %.1f %.1f "
                         "%.1f%s; non-black run %llu (longest %llu), black %llu, dark %llu, missed %llu\n",
                         static_cast<unsigned long long>(s.checked), static_cast<unsigned long long>(slot.present),
                         static_cast<unsigned long long>(slot.upscaled),
                         100.0 * share, m0, m1, m2, black ? " BLACK" : dark ? " dark" : "",
                         static_cast<unsigned long long>(s.run), static_cast<unsigned long long>(s.longest),
                         static_cast<unsigned long long>(s.black), static_cast<unsigned long long>(s.dark),
                         static_cast<unsigned long long>(s.missed));
        }
        if (!file.empty()) {
            std::vector<std::uint8_t> rgba(std::size_t(n) * 4);
            for (std::uint32_t y = 0; y < slot.height; ++y)
                std::memcpy(rgba.data() + std::size_t(y) * slot.width * 4, px + std::size_t(y) * slot.pitch,
                            std::size_t(slot.width) * 4);
            for (std::size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            if (std::FILE* f = std::fopen(file.c_str(), "wb")) {
                present_dump::write_png(f, VkExtent2D{slot.width, slot.height}, VK_FORMAT_R8G8B8A8_UNORM, false, rgba.data());
                std::fclose(f);
            }
        }
    }
};

void MetalPresenter::check_plane(gpu::Texture* source) {
    if (!check_ || !source) return;
    // A world frame is a present after which the upscaler ran again: presents with no new world render (the title's
    // own fades and menus) are counted apart and leave the run of non-black world frames as it is.
    const std::uint64_t upscaled = gta6rt::upscale::frames_upscaled();
    if (!upscaled) return;
    if (upscaled == check_->last_upscaled) {
        std::lock_guard lock(check_->state->mutex);
        if (++check_->state->idle <= 3 || !(check_->state->idle % 50))
            std::fprintf(stderr, "[rt] d3d12-metal present check: present %llu has no new world render (%llu so far); not "
                                 "counted\n",
                         static_cast<unsigned long long>(presented_), static_cast<unsigned long long>(check_->state->idle));
        return;
    }
    check_->last_upscaled = upscaled;
    const gpu::TextureDesc& td = source->desc();
    gpu::FormatInfo info;
    if (!context_.gpu().describe_format(td.format, info) || info.block_bytes != 4 || info.block_dim != 1 || !td.width ||
        !td.height) {
        if (!check_->told_format) {
            check_->told_format = true;
            std::fprintf(stderr, "[rt] d3d12-metal present check: plane 0 format %u is not 4-byte texels; not checked\n",
                         td.format);
        }
        return;
    }
    auto state = check_->state;
    Check::Slot& slot = state->slots[check_->value % state->slots.size()];
    if (slot.busy.exchange(true)) {
        std::lock_guard lock(state->mutex);
        ++state->missed;
        return;
    }
    slot.width = td.width;
    slot.height = td.height;
    slot.pitch = td.width * 4;
    slot.present = presented_;
    slot.upscaled = gta6rt::upscale::frames_upscaled();
    const std::uint64_t bytes = std::uint64_t(slot.pitch) * slot.height;
    if (slot.buffer && slot.buffer->size() < bytes) slot.buffer.reset();
    if (!slot.buffer || !slot.list) {
        gpu::BufferDesc bd;
        bd.size = bytes;
        bd.memory = gpu::MemoryKind::shared;
        bd.usage = gpu::usage::copy;
        bd.label = "present check";
        if (!slot.buffer) slot.buffer = context_.gpu().create_buffer(bd);
        if (!slot.list) slot.list = context_.gpu().create_command_list(queue_.queue()->type());
    }
    if (!slot.buffer || !slot.buffer->cpu_pointer() || !slot.list) {
        slot.busy = false;
        return;
    }
    slot.list->reset();
    gpu::BufferTextureCopy k;
    k.buffer = slot.buffer.get();
    k.row_pitch = slot.pitch;
    k.texture = source;
    k.width = slot.width;
    k.height = slot.height;
    slot.list->copy_texture_to_buffer(k);
    if (slot.list->close() != gpu::Status::ok) {
        slot.busy = false;
        return;
    }
    gpu::CommandList* lists[] = {slot.list.get()};
    if (queue_.queue()->execute(lists) != gpu::Status::ok ||
        queue_.queue()->signal(check_->fence.get(), ++check_->value) != gpu::Status::ok) {
        slot.busy = false;
        return;
    }
    Check::Slot* sp = &slot;
    check_->fence->on_completion(check_->value, [state, sp] {
        Check::inspect(*state, *sp);
        sp->busy = false;
    });
}

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
    check_ = Check::create(context_.gpu());
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
    d.keep_last_frame = dump_->writer.enabled() || !dump_->world_dir.empty();
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
    for (const auto& [fence, value] : context_.submitted_only())
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
    check_plane(desc.planes[0].source);
    if (SceneProbe* scene = context_.scene()) scene->end_frame();
    context_.stats().presents.fetch_add(1, std::memory_order_relaxed);
    submit_phases::totals().presents.fetch_add(1, std::memory_order_relaxed);
    submit_phases::totals().present_serial.fetch_add(1, std::memory_order_relaxed);
    after_present();
    return kOk;
}

void MetalPresenter::after_present() {
    auto& w = dump_->writer;
    const bool world = dump_->world_due(context_.scene());
    const bool due = w.enabled() && w.due(presented_ - 1);
    if (!world && !due) return;
    std::uint32_t pw = 0, ph = 0;
    if (swapchain_->read_last_frame(dump_->pixels, pw, ph) != gpu::Status::ok || !pw || !ph) return;
    // The window's CAMetalLayer is opaque, so the screen shows these texels with alpha ignored; the PNGs say the same.
    for (std::size_t i = 3; i < dump_->pixels.size(); i += 4) dump_->pixels[i] = 255;
    if (world) {
        ++dump_->world_done;
        std::uint64_t lit = 0;
        const std::uint64_t n = std::uint64_t(pw) * ph;
        for (std::uint64_t i = 0; i < n && 4 * i + 3 < dump_->pixels.size(); ++i)
            lit += (dump_->pixels[4 * i] | dump_->pixels[4 * i + 1] | dump_->pixels[4 * i + 2]) != 0;
        char name[64];
        std::snprintf(name, sizeof name, "/world_present_%06llu.png", static_cast<unsigned long long>(presented_));
        const std::string path = dump_->world_dir + name;
        bool written = false;
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            written = present_dump::write_png(f, VkExtent2D{pw, ph}, VK_FORMAT_B8G8R8A8_UNORM, false, dump_->pixels.data());
            std::fclose(f);
        }
        std::fprintf(stderr, "[rt] d3d12-metal world present %llu (%ux%u): %.1f%% of pixels not black; %s %s\n",
                     static_cast<unsigned long long>(presented_), pw, ph, n ? 100.0 * double(lit) / double(n) : 0.0,
                     written ? "written to" : "NOT written to", path.c_str());
    }
    if (due)
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
    if (SceneProbe* scene = context_.scene(); scene && scene->present_mode()) {
        float exposure = 1.0f;
        if (gpu::Texture* colour = scene->present_source(exposure)) {
            gpu::PresentPlane under;
            under.source = colour;
            under.blend = gpu::PlaneBlend::opaque;
            under.tone_exposure = exposure;
            if (scene->present_mode() == 2 || d.plane_count < 2) {
                d.planes[0] = under;
                d.plane_count = 1;
            } else {
                d.planes[0] = under;
            }
        }
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
