#pragma once

#include <cstdint>

namespace gta6rt::upscale {

enum class Mode : std::uint32_t { fsr4, metalfx_temporal, metalfx_spatial, bilinear };
const char* mode_name(Mode m);

}  // namespace gta6rt::upscale
