#pragma once

// Text-level edits of SPIRV-Cross MSL: entry signature parsing (shared with rect_list.cpp), threadgroup sizes, mesh
// output estimates and the D24 fragment-depth wrapper (DESIGN.md 5.3). Plain C++, no Metal.

#include "gta6rt_metal/gpu.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gta6rt::msl {

std::string trim(const std::string& s);
std::vector<std::string> split_top_level(const std::string& s);

struct Param {
    std::string decl, attr, name;
};
bool parse_param(const std::string& p, Param& out);

struct Member {
    std::string type, name, attr, array;  // array: "[n]" or empty
};
bool struct_members(const std::string& src, const std::string& name, std::vector<Member>& out, std::string& err,
                    bool allow_arrays = false);
int vector_size(const std::string& type);
std::string base_type(const std::string& type);

struct Entry {
    std::string out_type;
    std::size_t sig_begin = 0, open = 0, close = 0;  // start of the qualifier, '(' and ')'
    std::vector<Param> params;
};
// qualifier: "vertex", "fragment", "kernel", "[[mesh]]", "[[object]]".
bool find_entry(const std::string& msl, const std::string& qualifier, const std::string& entry, Entry& out,
                std::string& err);

std::optional<std::array<std::uint32_t, 3>> workgroup_size(const std::string& msl);

gpu::MeshEstimate estimate_mesh(const std::string& msl, const std::string& entry);
// Metal's unique-scalar count for a mesh vertex struct: SPIRV-Cross's user(clipN) copies of the clip distances
// count once (fitted to the S0.5 / S5.2 measurements: 126, 125, 129).
std::uint32_t mesh_unique_scalars(const std::string& msl, const std::string& vertex_struct);

struct D24Params {
    float constant_bias = 0;  // DepthBias * r, r = 1 / (2^24 - 1)
    float slope = 0;          // SlopeScaledDepthBias
    float clamp = 0;          // DepthBiasClamp
    std::uint32_t flags = 0;  // bit 0: apply the bias
};

struct D24Wrapper {
    bool ok = false;
    std::string source, entry, error;
    std::uint32_t params_index = 0;  // [[buffer(n)]] of D24Params
    bool shader_depth = false;       // the pixel shader writes its own depth (quantised, no bias)
};

// Wraps a SPIRV-Cross fragment function so it outputs [[depth(any)]] = quantise_d24(z + bias); `msl` empty builds a
// depth-only fragment function (pipelines without a pixel shader).
D24Wrapper d24_fragment_wrapper(const std::string& msl, const std::string& entry);
// The MSL of the quantiser alone (kernel gta6rt_d24_probe), for the exhaustive GPU-against-CPU test.
std::string d24_probe_source();

}  // namespace gta6rt::msl
