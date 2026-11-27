#pragma once
// port/d3d12_vulkan: a module's float-controls execution modes against VkPhysicalDeviceFloatControlsProperties.
// Vulkan forbids a mode whose property the device lacks (VUID-RuntimeSpirv-shaderDenormPreserveFloat16-06297 and
// its siblings); MoltenVK 1.4.2 and KosmicKrisp 26.3.1 report no shaderDenormPreserveFloat16, which every module the
// unmodified translator writes declares. Such a pipeline is refused by name instead of being handed to the driver.

#include <cstdint>
#include <span>

namespace leonida::float_controls {

enum Mode : unsigned { denorm_preserve, denorm_flush, signed_zero_inf_nan, rte, rtz, mode_count };

constexpr unsigned bit(Mode mode, unsigned width) {
    return 1u << (unsigned(mode) * 3 + (width == 16 ? 0 : width == 32 ? 1 : 2));
}

inline const char* property_name(unsigned b) {
    static const char* const names[] = {
        "shaderDenormPreserveFloat16", "shaderDenormPreserveFloat32", "shaderDenormPreserveFloat64",
        "shaderDenormFlushToZeroFloat16", "shaderDenormFlushToZeroFloat32", "shaderDenormFlushToZeroFloat64",
        "shaderSignedZeroInfNanPreserveFloat16", "shaderSignedZeroInfNanPreserveFloat32",
        "shaderSignedZeroInfNanPreserveFloat64", "shaderRoundingModeRTEFloat16", "shaderRoundingModeRTEFloat32",
        "shaderRoundingModeRTEFloat64", "shaderRoundingModeRTZFloat16", "shaderRoundingModeRTZFloat32",
        "shaderRoundingModeRTZFloat64"};
    for (unsigned i = 0; i < 15; ++i)
        if (b == (1u << i)) return names[i];
    return "?";
}

// The property bits the module's OpExecutionMode instructions need. Scans the preamble only: execution modes come
// before the first function.
inline unsigned required(std::span<const std::uint32_t> words) {
    unsigned need = 0;
    for (std::size_t i = 5; i < words.size();) {
        const std::uint32_t op = words[i] & 0xffff, count = words[i] >> 16;
        if (!count || i + count > words.size()) break;
        if (op == 54) break;  // OpFunction
        if (op == 16 && count == 4) {  // OpExecutionMode %entry Mode Width
            const std::uint32_t mode = words[i + 2], width = words[i + 3];
            if (mode >= 4459 && mode <= 4463 && (width == 16 || width == 32 || width == 64))
                need |= bit(Mode(mode - 4459), width);
        }
        i += count;
    }
    return need;
}

}  // namespace leonida::float_controls
