#pragma once

// The Metal upscalers behind the FidelityFX stage: MetalFX temporal, MetalFX spatial and a bilinear compute pass,
// on plain MTLTextures and an MTLCommandBuffer (no gta6rt::gpu types), so the offline harness drives the same code.
// Objective-C++ only.

#import <Metal/Metal.h>

#include <cstdint>
#include <memory>
#include <string>

#include "mode.h"

namespace gta6rt::upscale {

// The title's conventions (FidelityFX): jitter is the dispatch's jitterOffset (render pixels, the negated projection
// jitter), motion vectors are previous minus current position in render pixels times motion_scale, y down.
struct MetalInputs {
    id<MTLTexture> color = nil, depth = nil, motion = nil, output = nil;
    std::uint32_t render_w = 0, render_h = 0, output_w = 0, output_h = 0;
    float jitter_x = 0, jitter_y = 0, motion_scale_x = 1, motion_scale_y = 1;
    float pre_exposure = 1;
    bool reset = false, depth_inverted = true, auto_exposure = true, hdr = true;
};

// How FidelityFX's conventions map onto MetalFX's: MetalFX jitter = -(FFX dispatch jitter), motion unchanged
// (tests/upscale_offline.mm, measured; STATUS.md). The env overrides exist for the sweep only.
struct Conventions {
    float jitter_sign_x = 1, jitter_sign_y = 1, motion_sign_x = 1, motion_sign_y = 1;
};
Conventions default_conventions();

class MetalUpscaler {
public:
    MetalUpscaler(id<MTLDevice> device, Mode mode);
    ~MetalUpscaler();
    Mode mode() const;
    // Encodes the upscale into `cb`. False (with why) when the inputs cannot be used; nothing was encoded then.
    bool encode(id<MTLCommandBuffer> cb, const MetalInputs& in, std::string& why);
    void set_conventions(const Conventions& c);
    std::uint64_t memory_bytes() const;
    std::string describe() const;

    // A shared RGBA32Float copy of `src` (w x h from the origin) encoded into `cb`, and a tone-mapped sRGB PNG of it
    // once `cb` completed (x / (1 + x), gamma 2.2: the dumps compare inputs and outputs, not the title's grading).
    static id<MTLTexture> encode_readback(id<MTLCommandBuffer> cb, id<MTLTexture> src, std::uint32_t w, std::uint32_t h);
    static bool write_png(const std::string& path, id<MTLTexture> rgba32f, std::string* stats = nullptr);
    // GTA6RT_UPSCALE_PROBE: colour, motion (render size) and output sampled on a kProbeW x kProbeH grid into a shared
    // buffer encoded into `cb`; probe_stats() reads it once `cb` completed.
    static constexpr std::uint32_t kProbeW = 160, kProbeH = 90;
    static id<MTLBuffer> encode_probe(id<MTLCommandBuffer> cb, id<MTLTexture> color, id<MTLTexture> motion,
                                      id<MTLTexture> output, std::uint32_t rw, std::uint32_t rh, std::uint32_t ow,
                                      std::uint32_t oh);
    static std::string probe_stats(id<MTLBuffer> probe);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gta6rt::upscale
