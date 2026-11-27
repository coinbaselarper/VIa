#pragma once

// The subset of the FidelityFX API (FFX SDK, MIT: spike/r6-fsr4/ref/amd) the title uses through
// amd_fidelityfx_loader_xbox.dll. Layouts are the x64 ones; the offsets were checked against the title's call sites
// (game_scarlett_master.exe 0x1428ef1d0 create, 0x1428ef560 jitter, 0x1428ef700 dispatch; upscale/STATUS.md).

#include <cstddef>
#include <cstdint>

namespace gta6rt::upscale::ffx {

enum : std::uint32_t {
    ok = 0,
    error = 1,
    unknown_desctype = 2,
    runtime_error = 3,
    no_provider = 4,
    error_memory = 5,
    error_parameter = 6,
};

enum : std::uint64_t {
    create_upscale = 0x00010000,
    dispatch_upscale = 0x00010001,
    query_ratio_from_quality = 0x00010002,
    query_render_resolution = 0x00010003,
    query_jitter_phase_count = 0x00010004,
    query_jitter_offset = 0x00010005,
    dispatch_reactive_mask = 0x00010006,
    configure_key_value = 0x00010007,
    query_gpu_memory = 0x00010008,
    query_gpu_memory_v2 = 0x00010009,
    query_resource_requirements = 0x0001000a,
    create_upscale_version = 0x0001000b,
    configure_global_debug = 7,
    query_get_versions = 4,
    query_provider_version = 6,
};

enum : std::uint32_t {
    flag_hdr = 1u << 0,
    flag_display_res_mv = 1u << 1,
    flag_mv_jitter_cancel = 1u << 2,
    flag_depth_inverted = 1u << 3,
    flag_depth_infinite = 1u << 4,
    flag_auto_exposure = 1u << 5,
    flag_dynamic_resolution = 1u << 6,
};

struct Header {
    std::uint64_t type;
    Header* next;
};

struct Dimensions {
    std::uint32_t width, height;
};

struct Float2 {
    float x, y;
};

struct ResourceDescription {
    std::uint32_t type, format, width, height, depth, mips, flags, usage;
};

struct Resource {
    void* resource;
    ResourceDescription description;
    std::uint32_t state;
};

struct CreateUpscale {
    Header header;
    std::uint32_t flags;
    Dimensions max_render;
    Dimensions max_upscale;
    void* message;
};

struct DispatchUpscale {
    Header header;
    void* command_list;
    Resource color, depth, motion, exposure, reactive, transparency, output;
    Float2 jitter;
    Float2 motion_scale;
    Dimensions render;
    Dimensions upscale;
    bool sharpen;
    float sharpness;
    float frame_ms;
    float pre_exposure;
    bool reset;
    float camera_near, camera_far, fov_y, meters;
    std::uint32_t flags;
};

struct QueryPhaseCount {
    Header header;
    std::uint32_t render_width, display_width;
    std::int32_t* out_count;
};

struct QueryJitterOffset {
    Header header;
    std::int32_t index, phase_count;
    float* out_x;
    float* out_y;
};

struct QueryRatio {
    Header header;
    std::uint32_t quality;
    float* out_ratio;
};

struct QueryRenderResolution {
    Header header;
    std::uint32_t display_width, display_height, quality;
    std::uint32_t* out_width;
    std::uint32_t* out_height;
};

struct MemoryUsage {
    std::uint64_t total, aliasable;
};

struct QueryGpuMemory {
    Header header;
    MemoryUsage* out;
};

static_assert(sizeof(Resource) == 48);
static_assert(offsetof(CreateUpscale, flags) == 0x10 && offsetof(CreateUpscale, max_render) == 0x14 &&
              offsetof(CreateUpscale, message) == 0x28);
static_assert(offsetof(DispatchUpscale, command_list) == 0x10 && offsetof(DispatchUpscale, color) == 0x18 &&
              offsetof(DispatchUpscale, depth) == 0x48 && offsetof(DispatchUpscale, motion) == 0x78 &&
              offsetof(DispatchUpscale, exposure) == 0xa8 && offsetof(DispatchUpscale, output) == 0x138 &&
              offsetof(DispatchUpscale, jitter) == 0x168 && offsetof(DispatchUpscale, motion_scale) == 0x170 &&
              offsetof(DispatchUpscale, render) == 0x178 && offsetof(DispatchUpscale, upscale) == 0x180 &&
              offsetof(DispatchUpscale, sharpen) == 0x188 && offsetof(DispatchUpscale, sharpness) == 0x18c &&
              offsetof(DispatchUpscale, frame_ms) == 0x190 && offsetof(DispatchUpscale, pre_exposure) == 0x194 &&
              offsetof(DispatchUpscale, reset) == 0x198 && offsetof(DispatchUpscale, camera_near) == 0x19c &&
              offsetof(DispatchUpscale, fov_y) == 0x1a4 && offsetof(DispatchUpscale, flags) == 0x1ac &&
              sizeof(DispatchUpscale) == 0x1b0);
static_assert(offsetof(QueryPhaseCount, out_count) == 0x18 && offsetof(QueryJitterOffset, out_x) == 0x18 &&
              offsetof(QueryJitterOffset, out_y) == 0x20 && offsetof(QueryGpuMemory, out) == 0x10);

inline float halton(std::int32_t index, std::int32_t base) {
    float f = 1.0f, result = 0.0f;
    for (std::int32_t i = index; i > 0;) {
        f /= float(base);
        result += f * float(i % base);
        i = std::int32_t(float(i) / float(base));
    }
    return result;
}

inline std::int32_t jitter_phase_count(std::uint32_t render_width, std::uint32_t display_width) {
    if (!render_width) return 1;
    const float ratio = float(display_width) / float(render_width);
    const auto n = std::int32_t(8.0f * ratio * ratio);
    return n > 0 ? n : 1;
}

inline void jitter_offset(std::int32_t index, std::int32_t phase_count, float& x, float& y) {
    if (phase_count <= 0) phase_count = 1;
    const std::int32_t i = ((index % phase_count) + phase_count) % phase_count + 1;
    x = halton(i, 2) - 0.5f;
    y = halton(i, 3) - 0.5f;
}

}  // namespace gta6rt::upscale::ffx
