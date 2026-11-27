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
    spvDescriptor<texture2d<uint, access::read>> images [[id(0)]][1];
};

// user[0..1]: output address. Copies the 64x64 R32Uint texture in slot 0 of the binding-4 table to the output.
kernel void cs_copy_slot0(const device spvDescriptorSetBuffer5& spvDescriptorSet5 [[buffer(5)]],
                          constant PushBlock& push [[buffer(0)]], uint2 p [[thread_position_in_grid]])
{
    spvDescriptorArray<texture2d<uint, access::read>> images {spvDescriptorSet5.images};
    device uint* out = (device uint*)((ulong(push.user[1]) << 32) | ulong(push.user[0]));
    if (p.x < 64u && p.y < 64u)
        out[p.y * 64u + p.x] = images[0].read(p).x;
}

// user[0..1]: output address, user[2..3]: input address (both raw GPU addresses: the buffers are named only by the
// device residency set, as the translated shaders' V# buffers are). Copies 4096 words.
kernel void cs_copy_address(constant PushBlock& push [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    device uint* out = (device uint*)((ulong(push.user[1]) << 32) | ulong(push.user[0]));
    device const uint* in = (device const uint*)((ulong(push.user[3]) << 32) | ulong(push.user[2]));
    if (i < 4096u)
        out[i] = in[i];
}
