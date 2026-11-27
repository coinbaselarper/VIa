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

struct rect_out
{
    float2 uv [[user(locn0)]];
    float4 gl_Position [[position]];
};

vertex rect_out vs_rect(uint gl_VertexIndex [[vertex_id]], constant PushBlock& push [[buffer(0)]])
{
    const float2 corner[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1) };
    const float4 r = float4(as_type<float>(push.user[8]), as_type<float>(push.user[9]),
                            as_type<float>(push.user[10]), as_type<float>(push.user[11]));
    const float2 ndc = mix(r.xy, r.zw, corner[gl_VertexIndex % 6]);
    rect_out out = {};
    out.gl_Position = float4(ndc, 0.0, 1.0);
    out.uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
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

fragment ps_out ps_heavy(ps_in in [[stage_in]], constant PushBlock& push [[buffer(1)]])
{
    float a = in.uv.x;
    for (uint i = 0; i < push.user[4]; ++i)
        a = fma(a, 0.9999f, 0.0001f);
    ps_out out = {};
    out.color = float4(as_type<float>(push.user[0]), as_type<float>(push.user[1]), as_type<float>(push.user[2]), 1.0);
    if (a == 12345.678f)
        out.color.x = 0.0;
    return out;
}

fragment ps_out ps_sample(ps_in in [[stage_in]], const device spvDescriptorSetBuffer1& spvDescriptorSet1 [[buffer(1)]],
                          const device spvDescriptorSetBuffer2& spvDescriptorSet2 [[buffer(2)]],
                          constant PushBlock& push [[buffer(3)]])
{
    spvDescriptorArray<texture2d<float>> images {spvDescriptorSet1.images};
    spvDescriptorArray<sampler> samplers {spvDescriptorSet2.samplers};
    ps_out out = {};
    out.color = images[push.user[0]].sample(samplers[push.user[1]], in.uv);
    return out;
}

fragment ps_out ps_heap(ps_in in [[stage_in]], constant PushBlock& push [[buffer(1)]])
{
    const ulong base = (ulong(push.heap_hi) << 32) | ulong(push.heap_lo);
    device const float4* p = (device const float4*)(base + ulong(push.user[5]));
    ps_out out = {};
    out.color = p[0];
    return out;
}
