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

struct main0_in
{
    float2 pos [[attribute(0)]];
};

struct main0_out_vs
{
    float4 color [[user(locn0)]];
    float4 gl_Position [[position]];
};

vertex main0_out_vs vs_main(main0_in in [[stage_in]], constant PushBlock& push [[buffer(0)]], uint gl_InstanceIndex [[instance_id]])
{
    main0_out_vs out = {};
    const float dx = float(gl_InstanceIndex) * as_type<float>(push.user[5]);
    out.gl_Position = float4(in.pos.x + dx, in.pos.y, as_type<float>(push.user[4]), 1.0);
    out.color = float4(as_type<float>(push.user[0]), as_type<float>(push.user[1]), as_type<float>(push.user[2]),
                       as_type<float>(push.user[3]));
    return out;
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

struct dual_out
{
    float4 out_var_SV_Target0 [[color(0), index(0)]];
    float4 out_var_SV_Target1 [[color(0), index(1)]];
};

fragment dual_out ps_dual(main0_in_ps in [[stage_in]], constant PushBlock& push [[buffer(1)]])
{
    dual_out out = {};
    out.out_var_SV_Target0 = in.color;
    out.out_var_SV_Target1 = float4(as_type<float>(push.user[8]), as_type<float>(push.user[9]),
                                    as_type<float>(push.user[10]), as_type<float>(push.user[11]));
    return out;
}

struct depth_out
{
    float4 out_var_SV_Target [[color(0)]];
    float gl_FragDepth [[depth(any)]];
};

fragment depth_out ps_depthwrite(main0_in_ps in [[stage_in]], constant PushBlock& push [[buffer(1)]])
{
    depth_out out = {};
    out.out_var_SV_Target = in.color;
    out.gl_FragDepth = as_type<float>(push.user[6]);
    return out;
}

fragment main0_out ps_heavy(main0_in_ps in [[stage_in]], float4 gl_FragCoord [[position]], constant PushBlock& push [[buffer(1)]])
{
    float a = gl_FragCoord.x;
    for (uint i = 0; i < push.user[7]; ++i)
        a = fma(a, 0.9999f, 0.0001f);
    main0_out out = {};
    out.out_var_SV_Target = in.color;
    if (a == 12345.678f)
        out.out_var_SV_Target.x = 0.0;
    return out;
}

fragment main0_out ps_texel(main0_in_ps in [[stage_in]], const device spvDescriptorSetBuffer1& spvDescriptorSet1 [[buffer(1)]],
                            constant PushBlock& push [[buffer(2)]])
{
    spvDescriptorArray<texture2d<float>> images {spvDescriptorSet1.images};
    main0_out out = {};
    out.out_var_SV_Target = images[push.user[12]].read(uint2(0, 0));
    return out;
}
