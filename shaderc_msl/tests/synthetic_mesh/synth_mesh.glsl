#version 460
#extension GL_EXT_mesh_shader : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(local_size_x = 128) in;
layout(triangles, max_vertices = 128, max_primitives = 128) out;

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Word { uint v; };
layout(push_constant) uniform PC { uint s[32]; uvec2 heap; uvec2 code; } pc;

layout(location = 0) out vec4 o_a[];
layout(location = 1) out vec4 o_b[];

uint rd(uint64_t block, uint idx) { return Word(block + uint64_t(idx) * 4ul).v; }

void main() {
    uint lane = gl_LocalInvocationIndex;
    uint64_t base = uint64_t(pc.heap.x) | (uint64_t(pc.heap.y) << 32);
    uint64_t blk = base + 0x1000000ul + uint64_t(gl_WorkGroupID.x) * 65536ul;
    uint nv = min(rd(blk, 0u), 128u);
    uint np = min(rd(blk, 1u), 128u);
    SetMeshOutputsEXT(nv, np);
    if (lane < nv) {
        vec4 p = vec4(uintBitsToFloat(rd(blk, 512u + lane * 4u)), uintBitsToFloat(rd(blk, 513u + lane * 4u)),
                      uintBitsToFloat(rd(blk, 514u + lane * 4u)), 1.0);
        gl_MeshVerticesEXT[lane].gl_Position = p;
        o_a[lane] = p * 2.0;
        o_b[lane] = vec4(float(lane), float(gl_WorkGroupID.x), p.x + p.y, 7.0);
    }
    if (lane < np) {
        uint w = rd(blk, 128u + lane);
        gl_PrimitiveTriangleIndicesEXT[lane] = uvec3(w & 255u, (w >> 8u) & 255u, (w >> 16u) & 255u);
        gl_MeshPrimitivesEXT[lane].gl_CullPrimitiveEXT = ((w >> 24u) & 1u) != 0u;
    }
}
