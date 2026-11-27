#include <metal_stdlib>
using namespace metal;

struct PushBlock
{
    uint user[32];
    uint heap_lo;
    uint heap_hi;
    uint code_lo;
    uint code_hi;
};

struct spvPerVertex
{
    float4 gl_Position [[position]];
    float4 color [[user(locn0)]];
};

struct spvPerPrimitive
{
    bool gl_CullPrimitiveEXT [[primitive_culled]];
};

using spvMesh_t = mesh<spvPerVertex, spvPerPrimitive, 4, 2, topology::triangle>;

constant uint3 gl_WorkGroupSize [[maybe_unused]] = uint3(4u, 1u, 1u);

struct Payload
{
    uint quad[8];
};

static inline void emit_quad(spvMesh_t m, uint quad, uint lid, constant PushBlock& push)
{
    const float x0 = float(quad * 8u) / 32.0 - 1.0, x1 = float(quad * 8u + 8u) / 32.0 - 1.0;
    const float y0 = 1.0, y1 = 1.0 - 8.0 / 32.0;
    const float2 c[4] = {float2(x0, y0), float2(x1, y0), float2(x1, y1), float2(x0, y1)};
    if (lid < 4u)
    {
        spvPerVertex v;
        v.gl_Position = float4(c[lid], 0.5, 1.0);
        v.color = float4(as_type<float>(push.user[0]), as_type<float>(push.user[1]), as_type<float>(push.user[2]), 1.0);
        m.set_vertex(lid, v);
    }
    if (lid < 2u)
    {
        spvPerPrimitive p;
        p.gl_CullPrimitiveEXT = false;
        m.set_primitive(lid, p);
        m.set_index(lid * 3u + 0u, 0u);
        m.set_index(lid * 3u + 1u, lid == 0u ? 1u : 2u);
        m.set_index(lid * 3u + 2u, lid == 0u ? 2u : 3u);
    }
    if (lid == 0u)
        m.set_primitive_count(2u);
}

[[mesh]] void ms_quad(constant PushBlock& push [[buffer(0)]], uint3 gid [[threadgroup_position_in_grid]],
                      uint lid [[thread_index_in_threadgroup]], spvMesh_t spvMesh)
{
    emit_quad(spvMesh, gid.x, lid, push);
}

[[object]] void os_main(object_data Payload& payload [[payload]], mesh_grid_properties mgp, constant PushBlock& push [[buffer(0)]],
                        uint3 gid [[threadgroup_position_in_grid]], uint lid [[thread_index_in_threadgroup]])
{
    const uint n = push.user[4];
    if (lid < n)
        payload.quad[lid] = gid.x * n + lid;
    if (lid == 0u)
        mgp.set_threadgroups_per_grid(uint3(n, 1u, 1u));
}

[[mesh]] void ms_payload(const object_data Payload& payload [[payload]], constant PushBlock& push [[buffer(0)]],
                         uint3 gid [[threadgroup_position_in_grid]], uint lid [[thread_index_in_threadgroup]], spvMesh_t spvMesh)
{
    emit_quad(spvMesh, payload.quad[gid.x], lid, push);
}

struct main0_in_ps
{
    float4 color [[user(locn0)]];
};

struct main0_out
{
    float4 out_var_SV_Target [[color(0)]];
};

fragment main0_out ps_main(main0_in_ps in [[stage_in]])
{
    main0_out out = {};
    out.out_var_SV_Target = in.color;
    return out;
}
