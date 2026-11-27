#include "display_info.h"

#include <CoreGraphics/CoreGraphics.h>

#include <cmath>

namespace rthost {
namespace {

std::uint32_t hz_of(CGDisplayModeRef mode) {
    const double hz = CGDisplayModeGetRefreshRate(mode);
    return hz >= 1.0 ? std::uint32_t(std::lround(hz)) : 60;
}

}  // namespace

std::vector<Display> displays() {
    std::vector<Display> out;
    CGDirectDisplayID ids[32];
    std::uint32_t count = 0;
    if (CGGetActiveDisplayList(32, ids, &count) != kCGErrorSuccess) return out;
    const CGDirectDisplayID main = CGMainDisplayID();
    for (std::uint32_t i = 0; i < count; ++i) {
        Display d;
        d.id = ids[i];
        d.primary = ids[i] == main;
        const CGRect bounds = CGDisplayBounds(ids[i]);
        d.x = std::int32_t(bounds.origin.x);
        d.y = std::int32_t(bounds.origin.y);
        if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i])) {
            d.width = std::uint32_t(CGDisplayModeGetPixelWidth(mode));
            d.height = std::uint32_t(CGDisplayModeGetPixelHeight(mode));
            d.refresh_hz = hz_of(mode);
            CGDisplayModeRelease(mode);
        } else {
            d.width = std::uint32_t(CGDisplayPixelsWide(ids[i]));
            d.height = std::uint32_t(CGDisplayPixelsHigh(ids[i]));
        }
        d.rotation_degrees = std::uint32_t(CGDisplayRotation(ids[i]));
        if (d.primary) out.insert(out.begin(), d);
        else out.push_back(d);
    }
    return out;
}

std::vector<std::array<std::uint32_t, 3>> display_modes(std::uint32_t id) {
    std::vector<std::array<std::uint32_t, 3>> out;
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, nullptr);
    if (!modes) return out;
    for (CFIndex i = 0; i < CFArrayGetCount(modes); ++i) {
        auto mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        out.push_back({std::uint32_t(CGDisplayModeGetPixelWidth(mode)), std::uint32_t(CGDisplayModeGetPixelHeight(mode)),
                       hz_of(mode)});
    }
    CFRelease(modes);
    return out;
}

std::uint32_t display_of_window(const void*) {
    // Windows are centred on the main screen when made (metal_window.mm); per-window tracking is not done.
    return CGMainDisplayID();
}

}  // namespace rthost
