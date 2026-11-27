#include "image_decode.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cstring>

namespace rthost {

bool decode_image_bgra_fit(const char* path, std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>& out) {
    out.clear();
    if (!path || !width || !height) return false;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path),
                                                           CFIndex(std::strlen(path)), false);
    if (!url) return false;
    CGImageSourceRef source = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    if (!source) return false;
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (!image) return false;
    const std::uint64_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
    bool ok = false;
    if (w && h) {
        const std::uint32_t draw_w = std::uint32_t(std::clamp<std::uint64_t>(std::uint64_t(height) * w / h, 1, width));
        const std::uint32_t draw_h = std::uint32_t(std::clamp<std::uint64_t>(std::uint64_t(width) * h / w, 1, height));
        out.assign(std::size_t(width) * height * 4, 0);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(out.data(), width, height, 8, std::size_t(width) * 4, space,
                                                     kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(space);
        if (context) {
            CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
            // CoreGraphics' origin is bottom-left; the centred rectangle is symmetric, so rows come out top-down.
            CGContextDrawImage(context, CGRectMake((width - draw_w) / 2.0, (height - draw_h) / 2.0, draw_w, draw_h),
                               image);
            CGContextRelease(context);
            ok = true;
        }
    }
    CGImageRelease(image);
    if (!ok) out.clear();
    return ok;
}

}  // namespace rthost
