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

static inline ulong addr(constant PushBlock& p, uint lo)
{
    return (ulong(p.user[lo + 1]) << 32) | ulong(p.user[lo]);
}

kernel void cs_bump(constant PushBlock& push [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    device uint* p = (device uint*)addr(push, 0);
    if (i < push.user[2])
        p[i] = p[i] * 3u + 1u;
}

kernel void cs_gather(constant PushBlock& push [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    device const uint* s = (device const uint*)addr(push, 0);
    device uint* d = (device uint*)addr(push, 3);
    if (i < push.user[2])
        d[i] = s[ulong(i) * ulong(push.user[5])];
}

kernel void cs_spin(constant PushBlock& push [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    device uint* p = (device uint*)addr(push, 0);
    uint v = p[i];
    for (uint k = 0; k < push.user[2]; ++k)
        v = v * 1664525u + 1013904223u;
    p[i] = v;
}
