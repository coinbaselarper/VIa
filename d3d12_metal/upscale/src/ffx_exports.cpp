// The five amd_fidelityfx_loader_xbox.dll exports served by the host (upscale.h). Backend-neutral: the only layer
// calls are the list proxy lookup, ID3D12GraphicsCommandList::SetMarker and rt::d3d12::resource_id.

#include "d3d12_com.h"
#include "d3d12_xs_device.h"

#include "ffx_types.h"
#include "stage.h"

#include <gta6rt/isa.h>
#include <gta6rt/loader.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace gta6rt::upscale {

namespace {

constexpr const char* kMarker = "gta6rt-upscale#";

Mode parse_mode() {
    const char* v = std::getenv("GTA6RT_UPSCALER");
    if (!v || !*v) return Mode::metalfx_temporal;
    const std::string s = v;
    if (s == "fsr4") return Mode::fsr4;
    if (s == "metalfx-temporal" || s == "temporal") return Mode::metalfx_temporal;
    if (s == "metalfx-spatial" || s == "spatial") return Mode::metalfx_spatial;
    if (s == "bilinear") return Mode::bilinear;
    std::fprintf(stderr, "[upscale] GTA6RT_UPSCALER=%s is not fsr4|metalfx-temporal|metalfx-spatial|bilinear; "
                         "using metalfx-temporal\n", v);
    return Mode::metalfx_temporal;
}

struct Context {
    std::uint64_t magic = 0x6774613672747570ull;
    std::uint64_t id = 0;
    std::uint32_t flags = 0;
    ffx::Dimensions max_render{}, max_upscale{};
    std::uint64_t dispatches = 0;
};

std::atomic<std::uint64_t> g_next_context{1};
std::atomic<std::uint64_t> g_next_frame{1};
std::mutex g_frames_mutex;
std::array<Frame, 256> g_frames{};
gpu::Device* g_device = nullptr;
Mode g_mode = Mode::metalfx_temporal;
Counters g_counters;

void once(std::atomic<bool>& flag, const char* fmt, auto... args) {
    if (!flag.exchange(true)) std::fprintf(stderr, fmt, args...);
}

Context* context_of(void** context) {
    if (!context || !*context) return nullptr;
    auto* c = static_cast<Context*>(*context);
    return c->magic == 0x6774613672747570ull ? c : nullptr;
}

const ffx::Header* find(const ffx::Header* h, std::uint64_t type) {
    for (int n = 0; h && n < 32; h = h->next, ++n)
        if (h->type == type) return h;
    return nullptr;
}

ID3D12GraphicsCommandList* real_list(void* list) {
    if (!list) return nullptr;
    if (rt::d3d12_xs::is_command_list_proxy(list)) return rt::d3d12_xs::command_list_proxy_real_list(list);
    if (rt::d3d12_xs::is_list_proxy(list)) {
        ID3D12CommandList* real = rt::d3d12_xs::list_proxy_real_list(list);
        ID3D12GraphicsCommandList* g = nullptr;
        if (real && SUCCEEDED(real->QueryInterface(IID_PPV_ARGS(&g)))) {
            g->Release();
            return g;
        }
    }
    return nullptr;
}

std::uintptr_t id_of(const ffx::Resource& r) {
    return r.resource ? rt::d3d12::resource_id(static_cast<const ID3D12Resource*>(r.resource)) : 0;
}

GTA6RT_GUEST_ABI std::uint32_t up_create(void** context, ffx::Header* desc, const void*) {
    if (!context || !desc) return ffx::error_parameter;
    *context = nullptr;
    const auto* up = reinterpret_cast<const ffx::CreateUpscale*>(find(desc, ffx::create_upscale));
    if (!up) {
        std::fprintf(stderr, "[upscale] ffxCreateContext type %#llx: not the upscale effect; refused\n",
                     static_cast<unsigned long long>(desc->type));
        g_counters.refused.fetch_add(1);
        return ffx::unknown_desctype;
    }
    auto* c = new Context;
    c->id = g_next_context.fetch_add(1);
    c->flags = up->flags;
    c->max_render = up->max_render;
    c->max_upscale = up->max_upscale;
    *context = c;
    g_counters.contexts.fetch_add(1);
    std::string chain;
    for (const ffx::Header* h = desc; h && chain.size() < 200; h = h->next) {
        char t[24];
        std::snprintf(t, sizeof t, "%s%#llx", chain.empty() ? "" : " ", static_cast<unsigned long long>(h->type));
        chain += t;
    }
    std::fprintf(stderr,
                 "[upscale] ffxCreateContext #%llu: %s, max render %ux%u, display %ux%u, flags %#x (hdr %d, depth "
                 "inverted %d, infinite %d, auto exposure %d, display-res mv %d, jitter-cancelled mv %d); chain %s\n",
                 static_cast<unsigned long long>(c->id), mode_name(g_mode), c->max_render.width, c->max_render.height,
                 c->max_upscale.width, c->max_upscale.height, c->flags, !!(c->flags & ffx::flag_hdr),
                 !!(c->flags & ffx::flag_depth_inverted), !!(c->flags & ffx::flag_depth_infinite),
                 !!(c->flags & ffx::flag_auto_exposure), !!(c->flags & ffx::flag_display_res_mv),
                 !!(c->flags & ffx::flag_mv_jitter_cancel), chain.c_str());
    return ffx::ok;
}

GTA6RT_GUEST_ABI std::uint32_t up_destroy(void** context, const void*) {
    Context* c = context_of(context);
    if (!c) return ffx::error_parameter;
    std::fprintf(stderr, "[upscale] ffxDestroyContext #%llu after %llu dispatches\n",
                 static_cast<unsigned long long>(c->id), static_cast<unsigned long long>(c->dispatches));
    c->magic = 0;
    delete c;
    *context = nullptr;
    return ffx::ok;
}

GTA6RT_GUEST_ABI std::uint32_t up_configure(void** context, const ffx::Header* desc) {
    if (!desc) return ffx::error_parameter;
    static std::atomic<bool> said{false};
    once(said, "[upscale] ffxConfigure type %#llx accepted and ignored (first call)\n",
         static_cast<unsigned long long>(desc->type));
    return ffx::ok;
}

GTA6RT_GUEST_ABI std::uint32_t up_query(void** context, ffx::Header* desc) {
    if (!desc) return ffx::error_parameter;
    g_counters.queries.fetch_add(1);
    switch (desc->type) {
    case ffx::query_jitter_phase_count: {
        auto* q = reinterpret_cast<ffx::QueryPhaseCount*>(desc);
        const std::int32_t n = ffx::jitter_phase_count(q->render_width, q->display_width);
        if (q->out_count) *q->out_count = n;
        static std::atomic<bool> said{false};
        once(said, "[upscale] ffxQuery jitter phase count %ux%u -> %d\n", q->render_width, q->display_width, n);
        return ffx::ok;
    }
    case ffx::query_jitter_offset: {
        auto* q = reinterpret_cast<ffx::QueryJitterOffset*>(desc);
        float x = 0, y = 0;
        ffx::jitter_offset(q->index, q->phase_count, x, y);
        if (q->out_x) *q->out_x = x;
        if (q->out_y) *q->out_y = y;
        return ffx::ok;
    }
    case ffx::query_ratio_from_quality: {
        auto* q = reinterpret_cast<ffx::QueryRatio*>(desc);
        static constexpr float ratio[] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
        if (q->quality > 4) return ffx::error_parameter;
        if (q->out_ratio) *q->out_ratio = ratio[q->quality];
        return ffx::ok;
    }
    case ffx::query_render_resolution: {
        auto* q = reinterpret_cast<ffx::QueryRenderResolution*>(desc);
        static constexpr float ratio[] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
        if (q->quality > 4) return ffx::error_parameter;
        if (q->out_width) *q->out_width = std::uint32_t(float(q->display_width) / ratio[q->quality]);
        if (q->out_height) *q->out_height = std::uint32_t(float(q->display_height) / ratio[q->quality]);
        return ffx::ok;
    }
    case ffx::query_gpu_memory: {
        auto* q = reinterpret_cast<ffx::QueryGpuMemory*>(desc);
        if (q->out) *q->out = {32ull << 20, 0};
        return ffx::ok;
    }
    default: {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 8)
            std::fprintf(stderr, "[upscale] ffxQuery type %#llx not served (FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE)\n",
                         static_cast<unsigned long long>(desc->type));
        return ffx::unknown_desctype;
    }
    }
}

GTA6RT_GUEST_ABI std::uint32_t up_dispatch(void** context, const ffx::Header* desc) {
    Context* c = context_of(context);
    if (!c || !desc) return ffx::error_parameter;
    if (desc->type != ffx::dispatch_upscale) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 8)
            std::fprintf(stderr, "[upscale] ffxDispatch type %#llx not served (refused)\n",
                         static_cast<unsigned long long>(desc->type));
        g_counters.refused.fetch_add(1);
        return ffx::unknown_desctype;
    }
    const auto* d = reinterpret_cast<const ffx::DispatchUpscale*>(desc);
    ID3D12GraphicsCommandList* list = real_list(d->command_list);
    if (!list) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 8)
            std::fprintf(stderr, "[upscale] ffxDispatch: command list %p is not one of the layer's; nothing recorded\n",
                         d->command_list);
        g_counters.refused.fetch_add(1);
        return ffx::error_parameter;
    }
    Frame f;
    f.id = g_next_frame.fetch_add(1);
    f.context = c->id;
    f.create_flags = c->flags;
    f.color = id_of(d->color);
    f.depth = id_of(d->depth);
    f.motion = id_of(d->motion);
    f.exposure = id_of(d->exposure);
    f.reactive = id_of(d->reactive);
    f.transparency = id_of(d->transparency);
    f.output = id_of(d->output);
    f.color_state = d->color.state;
    f.output_state = d->output.state;
    f.jitter_x = d->jitter.x;
    f.jitter_y = d->jitter.y;
    f.motion_scale_x = d->motion_scale.x;
    f.motion_scale_y = d->motion_scale.y;
    f.render_w = d->render.width;
    f.render_h = d->render.height;
    f.upscale_w = d->upscale.width ? d->upscale.width : c->max_upscale.width;
    f.upscale_h = d->upscale.height ? d->upscale.height : c->max_upscale.height;
    f.sharpen = d->sharpen;
    f.sharpness = d->sharpness;
    f.frame_ms = d->frame_ms;
    f.pre_exposure = d->pre_exposure;
    f.reset = d->reset;
    f.camera_near = d->camera_near;
    f.camera_far = d->camera_far;
    f.fov_y = d->fov_y;
    f.flags = d->flags;
    {
        std::lock_guard lock(g_frames_mutex);
        g_frames[f.id % g_frames.size()] = f;
    }
    char label[48];
    const int n = std::snprintf(label, sizeof label, "%s%llu", kMarker, static_cast<unsigned long long>(f.id));
    list->SetMarker(1, label, UINT(n + 1));
    ++c->dispatches;
    g_counters.dispatches.fetch_add(1);
    const std::uint64_t k = g_counters.markers.fetch_add(1) + 1;
    if (k <= 3 || !(k & (k - 1)))
        std::fprintf(stderr,
                     "[upscale] ffxDispatch #%llu (ctx %llu): colour %#lx (%ux%u fmt %u state %#x) depth %#lx (fmt %u) "
                     "motion %#lx (fmt %u) exposure %#lx reactive %#lx t&c %#lx -> output %#lx (%ux%u fmt %u state %#x); "
                     "render %ux%u -> %ux%u, jitter %.4f,%.4f, mv scale %.3f,%.3f, reset %d, sharpen %d %.2f, "
                     "dt %.2f ms, pre-exposure %.3f, near %.3f far %.1f fov %.3f, flags %#x\n",
                     static_cast<unsigned long long>(f.id), static_cast<unsigned long long>(c->id),
                     static_cast<unsigned long>(f.color), d->color.description.width, d->color.description.height,
                     d->color.description.format, d->color.state, static_cast<unsigned long>(f.depth),
                     d->depth.description.format, static_cast<unsigned long>(f.motion), d->motion.description.format,
                     static_cast<unsigned long>(f.exposure), static_cast<unsigned long>(f.reactive),
                     static_cast<unsigned long>(f.transparency), static_cast<unsigned long>(f.output),
                     d->output.description.width, d->output.description.height, d->output.description.format,
                     d->output.state, f.render_w, f.render_h, f.upscale_w, f.upscale_h, f.jitter_x, f.jitter_y,
                     f.motion_scale_x, f.motion_scale_y, int(f.reset), int(f.sharpen), f.sharpness, f.frame_ms,
                     f.pre_exposure, f.camera_near, f.camera_far, f.fov_y, f.flags);
    return ffx::ok;
}

struct Export {
    const char* name;
    const void* fn;
};

void patch_loader() {
#if defined(__x86_64__)
    gta6rt_image* img = gta6rt_loader_package_find("amd_fidelityfx_loader_xbox.dll");
    if (!img) {
        std::fprintf(stderr, "[upscale] amd_fidelityfx_loader_xbox.dll is not loaded at device creation; the "
                             "title's upscaler is NOT replaced (the FSR4 DLL will run)\n");
        return;
    }
    gta6rt_image_info info{};
    if (gta6rt_loader_info(img, &info) != GTA6RT_OK) {
        std::fprintf(stderr, "[upscale] loader image info failed; the title's upscaler is NOT replaced\n");
        return;
    }
    static const Export exports[] = {
        {"ffxCreateContext", reinterpret_cast<const void*>(&up_create)},
        {"ffxDestroyContext", reinterpret_cast<const void*>(&up_destroy)},
        {"ffxConfigure", reinterpret_cast<const void*>(&up_configure)},
        {"ffxQuery", reinterpret_cast<const void*>(&up_query)},
        {"ffxDispatch", reinterpret_cast<const void*>(&up_dispatch)},
    };
    struct Found {
        std::uint32_t rva[5] = {};
    } found;
    gta6rt_loader_enumerate_exports(
        img,
        [](void* user, const char* name, std::uint32_t, std::uint32_t rva, const char* fwd) -> int {
            auto* f = static_cast<Found*>(user);
            if (!name || fwd) return 0;
            for (int i = 0; i < 5; ++i)
                if (!std::strcmp(name, exports[i].name)) f->rva[i] = rva;
            return 0;
        },
        &found);
    int patched = 0;
    for (int i = 0; i < 5; ++i) {
        if (!found.rva[i]) {
            std::fprintf(stderr, "[upscale] amd_fidelityfx_loader_xbox.dll has no export %s\n", exports[i].name);
            continue;
        }
        gta6rt_thunk_desc td{};
        td.kind = GTA6RT_THUNK_NATIVE;
        td.fn = exports[i].fn;
        char dbg[96];
        std::snprintf(dbg, sizeof dbg, "gta6rt-upscale!%s", exports[i].name);
        static std::array<std::string, 5> names;
        names[i] = dbg;
        td.debug_name = names[i].c_str();
        gta6rt_gva thunk = 0;
        if (gta6rt_isa_make_thunk(&td, &thunk) != GTA6RT_OK || !thunk) {
            std::fprintf(stderr, "[upscale] no thunk for %s; the title's upscaler is NOT replaced\n", exports[i].name);
            return;
        }
        std::uint8_t code[12] = {0x48, 0xb8};
        std::memcpy(code + 2, &thunk, 8);
        code[10] = 0xff;
        code[11] = 0xe0;
        const gta6rt_status st = gta6rt_isa_patch_code(info.base + found.rva[i], code, sizeof code);
        if (st != GTA6RT_OK) {
            std::fprintf(stderr, "[upscale] patching %s failed (%d)\n", exports[i].name, int(st));
            continue;
        }
        ++patched;
    }
    std::fprintf(stderr, "[upscale] %d of 5 amd_fidelityfx_loader_xbox.dll exports (at %#llx) now go to the host "
                         "upscaler (%s); the FSR4 DLL is not loaded\n",
                 patched, static_cast<unsigned long long>(info.base), mode_name(g_mode));
#else
    std::fprintf(stderr, "[upscale] arm64: the loader exports are not patched (no guest thunks); FSR4 runs\n");
#endif
}

}  // namespace

Mode selected_mode() { return g_mode; }
gpu::Device* stage_device() { return g_device; }
Counters& counters() { return g_counters; }
std::uint64_t frames_upscaled() { return g_counters.ran.load(std::memory_order_relaxed); }

bool frame_for(std::uint64_t id, Frame& out) {
    std::lock_guard lock(g_frames_mutex);
    const Frame& f = g_frames[id % g_frames.size()];
    if (f.id != id) return false;
    out = f;
    return true;
}

bool is_marker(std::string_view label) { return label.starts_with(kMarker); }

bool marker_frame(std::string_view label, std::uint64_t& id) {
    if (!is_marker(label)) return false;
    label.remove_prefix(std::strlen(kMarker));
    std::uint64_t v = 0;
    for (char ch : label) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + std::uint64_t(ch - '0');
    }
    id = v;
    return v != 0;
}

void install(gpu::Device& device) {
    static std::once_flag flag;
    std::call_once(flag, [&] {
        g_device = &device;
        g_mode = parse_mode();
        if (g_mode == Mode::fsr4) {
            std::fprintf(stderr, "[upscale] GTA6RT_UPSCALER=fsr4: the title's FSR4 DLL runs (29 compute passes)\n");
            return;
        }
        patch_loader();
    });
}

}  // namespace gta6rt::upscale
