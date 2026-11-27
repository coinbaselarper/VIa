#pragma once

#include "gta6rt_metal/gpu.h"

#include <cstdint>
#include <memory>
#include <string>

namespace gta6rt::metal {

inline constexpr std::uint32_t api_version = 2;

struct DeviceOptions {
    bool validation_labels = false;
    std::uint64_t va_base = 0x0000'0001'0000'0000ull;  // first console VA the backend hands out
    std::uint64_t va_size = 0x0000'0010'0000'0000ull;
    std::uint32_t compile_threads = 3;                  // pipeline compile workers (CompileMode::async, NotReady::skip)
};

std::unique_ptr<gpu::Device> create_device(const DeviceOptions& options = {});

// Compile workers allowed to run at once (<= compile_threads) and of those how many may run Priority::background
// work, so demanded pipelines always find a free worker. 0 keeps a value. GTA6RT_METAL_COMPILE_THREADS and
// GTA6RT_METAL_COMPILE_BACKGROUND set the start values. Returns false for a device of another backend.
bool set_compile_limits(gpu::Device* device, std::uint32_t active, std::uint32_t background);
// Name part of the shards the next flushes write (for example a title phase).
bool set_pipeline_cache_tag(gpu::Device* device, const std::string& tag);

// A window whose content view is a CAMetalLayer, for tests and for hosts without their own window layer.
// Created on the main thread; visible=false gives a layer without a window.
class Window {
public:
    virtual ~Window() = default;
    virtual void* layer() = 0;  // CAMetalLayer*
    virtual bool visible() const = 0;
    virtual std::uint32_t width() const = 0;
    virtual std::uint32_t height() const = 0;
    virtual int pump_events(double seconds) = 0;
};

std::unique_ptr<Window> create_window(const char* title, std::uint32_t width, std::uint32_t height, bool visible);

// Test hook: the next command buffer the queue begins fails in Metal (InvalidResource, raised by the kernel driver for
// this process only) and nothing recorded into it runs, as when Metal discards a command buffer to recover from
// another process's GPU error. wait_idle then reports device_lost once (DESIGN.md 3.12). as_victim counts it in
// DeviceStats::command_buffer_victims as such a discard. The submitting thread waits for the queue to drain before
// and for the command buffer after. Not under MTL_DEBUG_LAYER, which aborts on the released buffer. A command buffer
// that samples GPU counters or copies a texture to a buffer completes without the error (measured).
void inject_command_buffer_failure(gpu::Queue* queue, bool as_victim = false);

// MSL of the D24 depth quantiser the fragment wrapper uses, for an exhaustive GPU-against-CPU check: kernel
// gta6rt_d24_probe writes out[i] = quantise(in[i]) for i < user[2], in at heap_base + user[0], out at heap_base + user[1].
std::string d24_quantize_probe_msl();

// Diagnostics: `buffer` (shared storage, at least 4 KiB) is bound at Metal buffer index 30 of every object, mesh and
// fragment stage (no translated shader uses that index; instrumented debug variants write counters there), and the
// indirect mesh/compute pre-pass adds its record counters to words 0-127. Set before the first command list is
// encoded; null (the default) binds nothing. False for a device of another backend.
bool set_debug_buffer(gpu::Device* device, gpu::Buffer* buffer);

// A buffer made with BufferDesc::resident = false joins the device residency set. False for another backend.
bool make_resident(gpu::Device* device, gpu::Buffer* buffer);

// Diagnostics: live buffers outside heaps by label (MiB and count, resident or not, largest first), and Metal's
// allocated size that no backend category holds (pipeline code, libraries, encoder memory).
std::string memory_detail(gpu::Device* device, unsigned max_labels = 12);

}  // namespace gta6rt::metal
