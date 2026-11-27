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

constant uint3 gl_WorkGroupSize [[maybe_unused]] = uint3(64u, 1u, 1u);

static inline device uint* at(constant PushBlock& p, uint offset)
{
    const ulong base = (ulong(p.heap_hi) << 32) | ulong(p.heap_lo);
    return (device uint*)(base + ulong(offset));
}

kernel void cs_mark(constant PushBlock& push [[buffer(0)]], uint3 g [[threadgroup_position_in_grid]],
                    uint3 ng [[threadgroups_per_grid]], uint l [[thread_index_in_threadgroup]])
{
    device atomic_uint* counter = (device atomic_uint*)at(push, push.user[1]);
    atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
    if (l == 0u)
    {
        device atomic_uint* mark = (device atomic_uint*)at(push, push.user[0]);
        const uint idx = g.x + g.y * ng.x + g.z * ng.x * ng.y;
        if (idx < push.user[2])
            atomic_fetch_add_explicit(&mark[idx], 1u, memory_order_relaxed);
    }
}

kernel void cs_step(constant PushBlock& push [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    const uint n = push.user[2];
    if (i >= n)
        return;
    device const uint* src = at(push, push.user[0]);
    device uint* dst = at(push, push.user[1]);
    const uint v = src[(i + 1u) % n];
    uint h = v ^ i;
    for (uint k = 0; k < push.user[3]; ++k)
        h = h * 1664525u + 1013904223u;
    dst[i] = v + 1u + ((h == 0x9e3779b9u && push.user[3] == 7777u) ? 1u : 0u);
}

kernel void cs_write_args(constant PushBlock& push [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    if (i != 0u)
        return;
    device uint* a = at(push, push.user[0]);
    a[0] = push.user[4]; a[1] = 1u; a[2] = 1u;
    a[4] = push.user[5]; a[5] = 1u; a[6] = 1u;
    a[8] = push.user[6]; a[9] = 1u; a[10] = 1u;
    device uint* c = at(push, push.user[1]);
    c[0] = push.user[7];
}

kernel void cs_tg32k(constant PushBlock& push [[buffer(0)]], uint l [[thread_index_in_threadgroup]])
{
    threadgroup float data[8192];
    for (uint k = l; k < 8192u; k += 64u)
        data[k] = float(k);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    device float* o = (device float*)at(push, push.user[0]);
    o[l] = data[(l * 97u + 1u) % 8192u];
}

kernel void cs_tg40k(constant PushBlock& push [[buffer(0)]], uint l [[thread_index_in_threadgroup]])
{
    threadgroup float data[10240];
    for (uint k = l; k < 10240u; k += 64u)
        data[k] = float(k);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    device float* o = (device float*)at(push, push.user[0]);
    o[l] = data[(l * 97u + 1u) % 10240u];
}
