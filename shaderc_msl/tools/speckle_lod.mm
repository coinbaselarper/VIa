// Implicit-LOD samples in compute (translator fork --cs-implicit-lod): a compute kernel whose four lanes 4k..4k+3 hold
// one 2x2 pixel quad takes the gradients of its coordinates from the quad (quad_broadcast of lanes 1 and 2 minus lane 0,
// the console's coarse derivative, times 2^bias) and samples with them; a fragment shader samples the same coordinates
// with implicit LOD (and bias). Both write the mip level they read (each level of the texture holds its own index).
// Counts the pixels whose level differs; also the per-lane (fine) variant and what LOD 0 (the store's lowering) gives.
//   speckle_lod [size]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static const char* kSrc = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct P { uint w; float bias; float warp; uint pad; };
static float2 coords(float2 p, constant P& c) {
    float2 q = p / float(c.w);
    if (c.warp < 0.0) return float2(3.1 * q.x + 0.7 * q.y, -0.4 * q.x + 2.3 * q.y);
    return q * (0.5 + 7.5 * q.yx) + c.warp * float2(q.y * q.y, q.x * q.x);
}
struct VOut { float4 pos [[position]]; };
vertex VOut vs_full(uint vid [[vertex_id]]) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
fragment float4 fs_implicit(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],
                            constant P& c [[buffer(0)]]) {
    return t.sample(s, coords(in.pos.xy, c), bias(c.bias));
}
kernel void cs_quad(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant P& c [[buffer(0)]],
                    device float* out [[buffer(1)]], uint tid [[thread_index_in_threadgroup]],
                    uint2 tg [[threadgroup_position_in_grid]]) {
    uint quad = tid >> 2, lane = tid & 3;
    uint2 px = tg * 8 + uint2((quad & 3) * 2 + (lane & 1), (quad >> 2) * 2 + (lane >> 1));
    float2 uv = coords(float2(px) + 0.5, c);
    float2 dx = quad_broadcast(uv, 1) - quad_broadcast(uv, 0);
    float2 dy = quad_broadcast(uv, 2) - quad_broadcast(uv, 0);
    float k = exp2(c.bias);
    out[px.y * c.w + px.x] = t.sample(s, uv, gradient2d(dx * k, dy * k)).r;
}
kernel void cs_quad_fine(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant P& c [[buffer(0)]],
                         device float* out [[buffer(1)]], uint tid [[thread_index_in_threadgroup]],
                         uint2 tg [[threadgroup_position_in_grid]]) {
    uint quad = tid >> 2, lane = tid & 3;
    uint2 px = tg * 8 + uint2((quad & 3) * 2 + (lane & 1), (quad >> 2) * 2 + (lane >> 1));
    float2 uv = coords(float2(px) + 0.5, c);
    float2 dx = quad_shuffle_xor(uv, 1) - uv;
    float2 dy = quad_shuffle_xor(uv, 2) - uv;
    if (lane & 1) dx = -dx;
    if (lane & 2) dy = -dy;
    float k = exp2(c.bias);
    out[px.y * c.w + px.x] = t.sample(s, uv, gradient2d(dx * k, dy * k)).r;
}
kernel void cs_lod0(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant P& c [[buffer(0)]],
                    device float* out [[buffer(1)]], uint2 px [[thread_position_in_grid]]) {
    out[px.y * c.w + px.x] = t.sample(s, coords(float2(px) + 0.5, c), level(0.0)).r;
}
)MSL";

int main(int argc, char** argv) {
    @autoreleasepool {
        const uint32_t w = argc > 1 ? uint32_t(atoi(argv[1])) : 256;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError* err = nil;
        MTLCompileOptions* opt = [MTLCompileOptions new];
        opt.languageVersion = MTLLanguageVersion3_1;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:opt error:&err];
        if (!lib) {
            printf("COMPILE FAILED: %s\n", err.localizedDescription.UTF8String);
            return 2;
        }
        const uint32_t tw = 1024, levels = 11;
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                                                      width:tw height:tw mipmapped:YES];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        for (uint32_t l = 0; l < levels; ++l) {
            const uint32_t s = std::max(1u, tw >> l);
            std::vector<float> v(size_t(s) * s, float(l));
            [tex replaceRegion:MTLRegionMake2D(0, 0, s, s) mipmapLevel:l withBytes:v.data() bytesPerRow:s * 4];
        }
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterNearest;
        sd.magFilter = MTLSamplerMinMagFilterNearest;
        sd.mipFilter = MTLSamplerMipFilterNearest;
        sd.sAddressMode = MTLSamplerAddressModeRepeat;
        sd.tAddressMode = MTLSamplerAddressModeRepeat;
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
        MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs_full"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fs_implicit"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
        id<MTLRenderPipelineState> rp = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        id<MTLComputePipelineState> cq = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs_quad"] error:&err];
        id<MTLComputePipelineState> c0 = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs_lod0"] error:&err];
        id<MTLComputePipelineState> cf = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs_quad_fine"] error:&err];
        if (!rp || !cq || !c0 || !cf) {
            printf("PIPELINE FAILED: %s\n", err.localizedDescription.UTF8String);
            return 2;
        }
        MTLTextureDescriptor* od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                                                      width:w height:w mipmapped:NO];
        od.usage = MTLTextureUsageRenderTarget;
        od.storageMode = MTLStorageModeShared;
        id<MTLTexture> ot = [dev newTextureWithDescriptor:od];
        id<MTLBuffer> qb = [dev newBufferWithLength:size_t(w) * w * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> zb = [dev newBufferWithLength:size_t(w) * w * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> fb = [dev newBufferWithLength:size_t(w) * w * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        printf("device %s, simd width %lu\n", dev.name.UTF8String, (unsigned long)cq.threadExecutionWidth);
        int failures = 0;
        const float biases[] = {0.0f, 1.0f, -1.5f};
        const float warps[] = {-1.0f, 0.0f, 3.0f};
        for (float warp : warps)
            for (float bias : biases) {
                struct { uint32_t w; float bias; float warp; uint32_t pad; } p{w, bias, warp, 0};
                id<MTLCommandBuffer> cb = [q commandBuffer];
                MTLRenderPassDescriptor* pd = [MTLRenderPassDescriptor renderPassDescriptor];
                pd.colorAttachments[0].texture = ot;
                pd.colorAttachments[0].loadAction = MTLLoadActionClear;
                pd.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pd];
                [re setRenderPipelineState:rp];
                [re setFragmentTexture:tex atIndex:0];
                [re setFragmentSamplerState:smp atIndex:0];
                [re setFragmentBytes:&p length:sizeof p atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:cq];
                [ce setTexture:tex atIndex:0];
                [ce setSamplerState:smp atIndex:0];
                [ce setBytes:&p length:sizeof p atIndex:0];
                [ce setBuffer:qb offset:0 atIndex:1];
                [ce dispatchThreadgroups:MTLSizeMake(w / 8, w / 8, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [ce setComputePipelineState:cf];
                [ce setBuffer:fb offset:0 atIndex:1];
                [ce dispatchThreadgroups:MTLSizeMake(w / 8, w / 8, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [ce setComputePipelineState:c0];
                [ce setBuffer:zb offset:0 atIndex:1];
                [ce dispatchThreads:MTLSizeMake(w, w, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                [ce endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                std::vector<float> ref(size_t(w) * w);
                [ot getBytes:ref.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:0];
                const float* vq = static_cast<const float*>(qb.contents);
                const float* vz = static_cast<const float*>(zb.contents);
                const float* vf = static_cast<const float*>(fb.contents);
                size_t dq = 0, dz = 0, d1 = 0, df = 0;
                double lsum = 0;
                for (size_t i = 0; i < ref.size(); ++i) {
                    dq += vq[i] != ref[i];
                    d1 += std::fabs(vq[i] - ref[i]) > 1.0f;
                    dz += vz[i] != ref[i];
                    df += vf[i] != ref[i];
                    lsum += ref[i];
                }
                const bool affine = warp < 0.0f;
                const bool ok = d1 == 0 && (!affine || (dq == 0 && df == 0)) &&
                                (affine || std::max(dq, df) <= std::min(dq, df) + ref.size() / 100);
                printf("%s bias %+.1f: implicit LOD mean level %.2f; coarse quad gradients (the fork) differ at %zu of %zu "
                       "pixels (%zu by more than one level), fine quad gradients at %zu; LOD 0 (the store) at %zu%s\n",
                       affine ? "affine     " : warp == 0.0f ? "quadratic  " : "quadratic+w", bias, lsum / double(ref.size()), dq,
                       ref.size(), d1, df, dz, ok ? "" : "  FAIL");
                failures += !ok;
            }
        printf("%s\n", failures ? "RESULT: FAIL"
                                 : "RESULT: lanes 4k..4k+3 are the quad; quad gradients give the implicit LOD on affine maps "
                                   "and stay within one level of it otherwise (coarse as close as fine)");
        return failures ? 1 : 0;
    }
}
