#pragma once

#include "downmix.h"

#include <cstdint>
#include <string>

namespace gta6rt::audio_out {

struct DeviceInfo {
    uint32_t id = 0;
    uint32_t channels = 0;
    uint32_t rate = 0;
    uint32_t io_frames = 0;
    uint32_t latency_frames = 0;
    uint32_t max_frames = 0;
    Layout layout;
    std::string name;
};

using RenderFn = void (*)(void* ctx, float* out, uint32_t frames, uint32_t channels);
using ChangeFn = void (*)(void* ctx);

// The default output device through a HAL output unit with a 48 kHz float interleaved client format in the
// device's channel count; the unit's converter does any rate conversion. Every call here is made from the service
// thread only.
class Device {
public:
    Device();
    ~Device();
    // Creates and initialises the unit for the current default device; the render callback is not running yet.
    bool prepare(uint32_t client_rate, RenderFn render, void* ctx, ChangeFn changed, void* change_ctx,
                 DeviceInfo* info, std::string* error);
    bool start(std::string* error);
    // Stops and disposes the unit; returns once no callback runs.
    void close();
    bool open() const;
    // Current default output device, its output channel count and nominal rate.
    static bool query_default(uint32_t* id, uint32_t* channels, uint32_t* rate);

private:
    struct Impl;
    Impl* impl_;
};

uint64_t host_now_ns();

}  // namespace gta6rt::audio_out
