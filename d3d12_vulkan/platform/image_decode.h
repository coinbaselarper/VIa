#pragma once

#include <cstdint>
#include <vector>

namespace rthost {
// Decodes an image file (ImageIO: PNG, JPEG, ...) into width x height premultiplied BGRA8, the image scaled to fit at
// its own aspect ratio and centred on transparent black. False when the file cannot be decoded.
bool decode_image_bgra_fit(const char* path, std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>& out);
}  // namespace rthost
