#pragma once

// Console rectangle lists (D3D topology 17, pipeline type 19) without a geometry stage (DESIGN.md F25).
// Three corners per rectangle; the corner opposite the longest side is the right angle vc, the fourth corner is
// va + vb - vc for the position and every float output. Both triangles start at the rectangle's first vertex and
// keep its winding (leonida rect_list_geometry.h), so flat values are the first vertex's.
//
// vertex_expand: generate_rect_wrapper rewrites a SPIRV-Cross vertex function into a callable body plus a
// wrapper that pulls the three corners' inputs, runs the body three times and emits the expanded vertex: exact
// after the vertex stage. cpu_rewrite: expand_rect_vertices builds the fourth vertex from vertex inputs, exact
// only for shaders linear in their inputs (VIctory primitive.rs).

#include "gta6rt_metal/gpu.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gta6rt::rect {

// 0, 1 or 2: the corner opposite the longest side; ties, NaN and degenerate triangles take 1 (VIctory rect.rs).
int right_angle(const std::array<std::array<float, 2>, 3>& xy);

// The six corner indices (0..3, 3 = the made corner) of the two triangles for right angle `corner`.
std::array<std::uint8_t, 6> triangle_corners(int corner);

struct CpuElement {
    std::uint32_t offset = 0;
    std::uint32_t format = gpu::format::unknown;
};

// Reads `rects` rectangles of three `stride`-byte vertices (optionally through 16/32-bit indices) and writes four
// vertices per rectangle plus six 16/32-bit indices per rectangle. The position is the first float element with
// two or more components. Unknown formats keep the first corner's bytes.
struct CpuExpansion {
    std::vector<std::uint8_t> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<int> corners;
};
bool expand_rect_vertices(std::span<const std::uint8_t> src, std::uint32_t stride, std::span<const CpuElement> elements,
                          std::uint32_t rects, std::span<const std::uint32_t> src_indices, CpuExpansion& out);

struct WrapperInput {
    std::uint32_t location = 0;
    std::uint32_t format = gpu::format::unknown;
    std::uint32_t slot = 0;
    std::uint32_t offset = 0;
};

struct WrapperResult {
    bool ok = false;
    std::string source;
    std::string entry;
    std::string error;
    std::uint32_t params_index = 0;              // [[buffer(n)]] of the rect parameters
    std::vector<std::uint32_t> vertex_buffer_index;  // per input slot
    std::uint32_t index_buffer_index = 0;
};

// The draw parameters the wrapper reads (constant buffer at params_index).
struct WrapperParams {
    std::uint32_t first = 0;         // first vertex or first index
    std::int32_t base_vertex = 0;
    std::uint32_t indexed = 0;       // 0, 2 or 4 bytes per index
    std::uint32_t first_instance = 0;
    std::array<std::uint32_t, 8> strides{};
};

// `msl` is a SPIRV-Cross vertex function source with entry `entry` and [[stage_in]] inputs; `inputs` gives each
// attribute location's format, slot and offset (the pipeline's input layout).
WrapperResult generate_rect_wrapper(const std::string& msl, const std::string& entry,
                                    std::span<const WrapperInput> inputs);

}  // namespace gta6rt::rect
