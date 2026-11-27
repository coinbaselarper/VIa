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

struct main0_out
{
    float4 m_color [[user(locn0)]];
    float2 m_uv [[user(locn1)]];
    float4 gl_Position [[position]];
};

struct main0_in
{
    float4 m_position [[attribute(0)]];
    float2 m_uv [[attribute(1)]];
};

vertex main0_out main0(main0_in in [[stage_in]], constant PushBlock& _17 [[buffer(0)]], uint gl_VertexIndex [[vertex_id]], uint gl_InstanceIndex [[instance_id]], uint gl_BaseVertex [[base_vertex]], uint gl_BaseInstance [[base_instance]])
{
    main0_out out = {};
    out.gl_Position = in.m_position;
    out.m_uv = in.m_uv;
    out.m_color = float4(as_type<float>(_17.user[4]));
    return out;
}

vertex main0_out main0_nl(main0_in in [[stage_in]], constant PushBlock& _17 [[buffer(0)]], uint gl_VertexIndex [[vertex_id]], uint gl_InstanceIndex [[instance_id]], uint gl_BaseVertex [[base_vertex]], uint gl_BaseInstance [[base_instance]])
{
    main0_out out = {};
    out.gl_Position = in.m_position;
    out.m_uv = float2(in.m_uv.x * in.m_uv.y, in.m_uv.y);
    out.m_color = float4(as_type<float>(_17.user[4]));
    return out;
}

struct ps_in
{
    float4 m_color [[user(locn0)]];
    float2 m_uv [[user(locn1)]];
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
    out.color = images[push.user[0]].sample(samplers[push.user[1]], in.m_uv) * in.m_color;
    return out;
}
