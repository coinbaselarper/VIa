#pragma once

#include <array>
#include <cstdint>
#include <vector>

// The host's displays through CoreGraphics, for the DXGI outputs (no windows.h here).
namespace rthost {

struct Display {
    std::uint32_t id = 0;  // CGDirectDisplayID; HMONITOR on this host is this value
    bool primary = false;
    std::int32_t x = 0, y = 0;            // desktop position in points, top-left origin as on Windows
    std::uint32_t width = 0, height = 0;  // current mode in pixels
    std::uint32_t refresh_hz = 60;
    std::uint32_t rotation_degrees = 0;
};

// Primary display first. Empty when CoreGraphics reports none (headless).
std::vector<Display> displays();
// (width, height, refresh Hz) of every mode the display lists, current one included.
std::vector<std::array<std::uint32_t, 3>> display_modes(std::uint32_t id);
// The display showing most of a gta6rt_mac_window, else the main display; 0 when there is none.
std::uint32_t display_of_window(const void* mac_window);

}  // namespace rthost
