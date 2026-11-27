#pragma once

#include <cstdint>
#include <memory>

namespace rt::xa2 {

// Where the XAudio2 engine's mastering voice goes. The engine thread loops `wait_ready` / `write` one quantum at a
// time; the backend's clock decides when the next quantum is due, so voice consumption follows the output clock.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    virtual bool start(uint32_t rate, uint32_t channels, uint32_t quantum_frames) = 0;
    virtual void stop() = 0;
    // Blocks until the next quantum is due. False once stop() was called.
    virtual bool wait_ready() = 0;
    virtual void write(const float* interleaved, uint32_t frames) = 0;
    // Frames between the end of the last write and the moment it is heard (estimate).
    virtual uint32_t latency_frames() const = 0;
    virtual uint32_t glitches() const = 0;
    virtual const char* name() const = 0;
};

// Paced by the host monotonic clock, output discarded.
std::unique_ptr<AudioOutput> make_silent_output();
// CoreAudio default output device (AudioUnit). Null with a log line if the device cannot be opened.
std::unique_ptr<AudioOutput> make_coreaudio_output();

struct CoreAudioInfo {
    double device_rate;
    uint32_t io_buffer_frames;
    uint32_t device_latency_frames;
    uint32_t safety_offset_frames;
    uint32_t ring_target_frames;
    uint64_t callbacks;
    uint64_t frames_rendered;
    uint64_t first_callback_ns;
    uint64_t last_callback_ns;
};
// Diagnostics of a CoreAudio output (tests). False for other backends.
bool coreaudio_info(const AudioOutput* out, CoreAudioInfo* info);

}  // namespace rt::xa2
