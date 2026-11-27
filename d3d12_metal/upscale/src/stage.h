#pragma once

#include "mode.h"
#include "upscale.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace gta6rt::upscale {

Mode selected_mode();
gpu::Device* stage_device();

// One ffxDispatch of the upscale effect, as the title described it (ResourceIds are the layer's, rt::d3d12::resource_id).
struct Frame {
    std::uint64_t id = 0;
    std::uint64_t context = 0;
    std::uint32_t create_flags = 0;
    std::uintptr_t color = 0, depth = 0, motion = 0, exposure = 0, reactive = 0, transparency = 0, output = 0;
    std::uint32_t color_state = 0, output_state = 0;
    float jitter_x = 0, jitter_y = 0, motion_scale_x = 1, motion_scale_y = 1;
    std::uint32_t render_w = 0, render_h = 0, upscale_w = 0, upscale_h = 0;
    bool sharpen = false, reset = false;
    float sharpness = 0, frame_ms = 0, pre_exposure = 1, camera_near = 0, camera_far = 0, fov_y = 0;
    std::uint32_t flags = 0;
};

bool frame_for(std::uint64_t id, Frame& out);
bool marker_frame(std::string_view label, std::uint64_t& id);

struct Counters {
    std::atomic<std::uint64_t> contexts{0}, dispatches{0}, markers{0}, queries{0}, refused{0}, replayed{0}, ran{0},
        failed{0}, over_budget{0};
};
Counters& counters();

}  // namespace gta6rt::upscale
