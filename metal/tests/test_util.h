#pragma once

#include "gta6rt_metal/gpu.h"
#include "gta6rt_metal/metal_backend.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace test {

extern int failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++::test::failures;                                             \
            std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);     \
            std::printf(__VA_ARGS__);                                       \
            std::printf("\n");                                              \
        }                                                                   \
    } while (0)

std::string read_file(const std::string& path);
std::string shader_path(const char* name);
bool headless();
void print_caps(const gta6rt::gpu::DeviceCaps& caps);
void print_stats(const gta6rt::gpu::DeviceStats& s);

// Copies mip 0 of an RGBA8/BGRA8-sized texture into CPU memory through the backend (copy + readback buffer).
// The readback buffer is filled with kReadbackFill first, so texels the copy did not write are visible
// (GTA6RT_TEST_NO_READBACK_FILL=1 leaves it as Metal allocated it, untouched by the CPU before the copy).
constexpr std::uint8_t kReadbackFill = 0xee;

// On failure *status (if given) holds the failing call's status and out is left empty.
bool read_texture(gta6rt::gpu::Device& device, gta6rt::gpu::Queue& queue, gta6rt::gpu::Texture& texture,
                  std::uint32_t bytes_per_pixel, std::vector<std::uint8_t>& out,
                  gta6rt::gpu::Status* status = nullptr);

int finish(const char* name);

}  // namespace test
