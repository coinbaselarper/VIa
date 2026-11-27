#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gta6rt::shaderc_msl {

constexpr int kApiVersion = 3;

enum class Entry { unknown, vertex, fragment, kernel, mesh };

// mesh_route "split64": the mesh stage emits at most vmax vertices and pmax primitives per mesh threadgroup and is
// launched by an object stage (MeshPipelineDesc::object, payload 24 bytes: the original WorkgroupId and NumWorkgroups
// as two uint3, TaskPayloadWorkgroupEXT). The object function launches `chunks` mesh threadgroups per original
// group (dispatch the ORIGINAL grid; object threadgroup size 1, mesh threadgroup size 128). Primitives beyond the last
// chunk are dropped (see STATUS.md: chunk bound). Buffer indices are those of the mesh function.
struct SplitMesh {
    bool present = false;
    std::string object_metallib;
    std::string mesh_metallib;
    int chunks = 0, vmax = 0, pmax = 0;
    int push_buffer = -1;
    std::vector<std::pair<int, int>> tables;
};

// mesh_route "mesh_as_compute": dispatch `cs` with the ORIGINAL mesh grid (threadgroup size local_x), writing one
// slot of slot_bytes per workgroup (linear workgroup index x + y*nx + z*nx*ny) to a buffer whose 64-bit address is in
// push dwords address_dword and address_dword + 1 (push block push_bytes); then draw with `vs` non-indexed,
// groups * max_primitives * 3 vertices, same push block. Slot layout: 16-byte header (vertex count, primitive count),
// vertices at `vertices` (stride vertex_stride: position xyzw, then params at params[loc] offsets, each 4 floats),
// primitives at `primitives` (stride primitive_stride: three uint indices, null flag). A primitive past the
// count, or a null one, is clipped by the vertex shader.
struct MeshAsCompute {
    bool present = false;
    std::string cs_metallib, vs_metallib;
    int cs_push = -1, vs_push = -1;
    std::vector<std::pair<int, int>> cs_tables, vs_tables;
    int slot_bytes = 0, vertices = 0, vertex_stride = 0, primitives = 0, primitive_stride = 0;
    int max_vertices = 0, max_primitives = 0, address_dword = 0, push_bytes = 0, local_x = 0;
    std::vector<std::pair<int, int>> params;
};

struct ShaderEntry {
    std::string name;
    std::string key;
    std::string stage;
    Entry entry = Entry::unknown;
    std::string metallib;
    int push_buffer = -1;
    std::vector<std::pair<int, int>> tables;
    std::vector<std::pair<int, int>> descriptor_arrays;
    std::string mesh_route;
    bool vertex_as_kernel = false;
    std::vector<std::string> passes;
    // cs_route "x_buf_device": a compute shader whose wave64 exchange buffer lives in a storage buffer at contract
    // binding 29 (a table, see `tables`); bind x_buf_words * 4 * (groups x * y * z) bytes for every dispatch
    // (contents need not survive); an indirect dispatch needs the largest group count it can issue.
    std::string cs_route;
    int x_buf_words = 0;
    int x_buf_buffer = -1;  // [[buffer(n)]] of the exchange buffer in the compute function
    SplitMesh split;
    MeshAsCompute msc;
};

class Catalog {
public:
    bool load(const std::string &manifest_tsv, std::string *error = nullptr);
    const ShaderEntry *find(const std::string &name) const;
    size_t size() const { return entries_.size(); }
    const std::string &root() const { return root_; }
    std::string library_path(const ShaderEntry &e) const;
    std::string path(const std::string &relative) const { return root_ + "/" + relative; }

private:
    std::string root_;
    std::vector<ShaderEntry> entries_;
    std::unordered_map<std::string, size_t> by_name_;
};

}
