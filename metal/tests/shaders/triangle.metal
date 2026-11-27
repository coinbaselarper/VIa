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

struct spvDescriptorSetBuffer1
{
    spvDescriptor<texture2d<float>> images [[id(0)]][1];
};

struct spvDescriptorSetBuffer2
{
    spvDescriptor<sampler> samplers [[id(0)]][1];
};

struct main0_in
{
    float2 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct main0_out
{
    float2 uv [[user(locn0)]];
    float4 gl_Position [[position]];
};

vertex main0_out main0(main0_in in [[stage_in]], constant PushBlock& push [[buffer(0)]])
{
    main0_out out = {};
    const float2 offset = float2(as_type<float>(push.user[2]), as_type<float>(push.user[3]));
    out.gl_Position = float4(in.position + offset, 0.0, 1.0);
    out.uv = in.uv;
    return out;
}

struct ps_in
{
    float2 uv [[user(locn0)]];
};

struct ps_out
{
    float4 color [[color(0)]];
};

fragment ps_out ps_main(ps_in in [[stage_in]], const device spvDescriptorSetBuffer1& spvDescriptorSet1 [[buffer(1)]],
                        const device spvDescriptorSetBuffer2& spvDescriptorSet2 [[buffer(2)]],
                        constant PushBlock& push [[buffer(3)]])
{
    spvDescriptorArray<texture2d<float>> images {spvDescriptorSet1.images};
    spvDescriptorArray<sampler> samplers {spvDescriptorSet2.samplers};
    ps_out out = {};
    out.color = images[push.user[0]].sample(samplers[push.user[1]], in.uv);
    return out;
}
