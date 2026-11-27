#include <metal_stdlib>
using namespace metal;

template<typename T>
struct spvDescriptor
{
    T value;
};

template<typename T>
struct spvDescriptorArray
{
    spvDescriptorArray(const device spvDescriptor<T>* ptr_) : ptr(&ptr_->value) {}
    const device T& operator [] (size_t i) const { return ptr[i]; }
    const device T* ptr;
};

struct PushBlock
{
    uint user[32];
    uint heap_lo;
    uint heap_hi;
    uint code_lo;
    uint code_hi;
};

struct spvDescriptorSetBuffer5
{
    spvDescriptor<texture2d<float, access::write>> images [[id(0)]][1];
};

struct spvDescriptorSetBuffer8
{
    spvDescriptor<texture2d<uint, access::write>> uimages [[id(0)]][1];
};

static inline ulong addr(constant PushBlock& p, uint lo)
{
    return (ulong(p.user[lo + 1]) << 32) | ulong(p.user[lo]);
}

// user[0..1]: constants address, user[2]: block bytes, user[3..4]: output address. One group per draw's constants.
kernel void cs_read_constants(constant PushBlock& push [[buffer(0)]], uint3 g [[threadgroup_position_in_grid]],
                              uint l [[thread_index_in_threadgroup]])
{
    if (l != 0u)
        return;
    device const uint4* blk = (device const uint4*)(addr(push, 0) + ulong(g.x) * ulong(push.user[2]));
    device uint4* out = (device uint4*)addr(push, 3);
    out[g.x * 2u] = blk[0];
    out[g.x * 2u + 1u] = blk[push.user[2] / 16u - 1u];
}

// user[0..1]: address of a uint array to increment in place (GPU writes into host-owned pages).
kernel void cs_bump(constant PushBlock& push [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    device uint* p = (device uint*)addr(push, 0);
    if (i < push.user[2])
        p[i] = p[i] * 3u + 1u;
}

// The ocean kinds the title's CPU physics reads (fork assessment): R32F 256^2, RG32F 256^2, RG32F 64^2, RG16UI 128^2.
// user[8]: frame. Float textures are slots 0..2 of the binding-4 table, the uint one slot 0 of binding 7.
kernel void cs_ocean(const device spvDescriptorSetBuffer5& spvDescriptorSet5 [[buffer(5)]],
                     const device spvDescriptorSetBuffer8& spvDescriptorSet8 [[buffer(8)]],
                     constant PushBlock& push [[buffer(0)]], uint2 p [[thread_position_in_grid]])
{
    spvDescriptorArray<texture2d<float, access::write>> images {spvDescriptorSet5.images};
    spvDescriptorArray<texture2d<uint, access::write>> uimages {spvDescriptorSet8.uimages};
    const float f = float(push.user[8]);
    if (p.x < 256u && p.y < 256u)
    {
        images[0].write(float4(float(p.x) + f * 1000.0, 0, 0, 0), p);
        images[1].write(float4(float(p.y), f, 0, 0), p);
    }
    if (p.x < 64u && p.y < 64u)
        images[2].write(float4(float(p.x * p.y), -f, 0, 0), p);
    if (p.x < 128u && p.y < 128u)
        uimages[0].write(uint4(p.x + p.y * 128u, push.user[8], 0, 0), p);
}
