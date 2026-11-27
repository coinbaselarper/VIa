// 64-bit texture atomics on this GPU: unsigned max of 64-bit keys (high dword decides) into an RG32Uint texture, from
// a compute kernel and from a fragment shader, through a direct texture binding and through an argument buffer of
// resource IDs (as the translated shaders reach their storage images), with and without MTLTextureUsageShaderAtomic.
// Each case is checked texel by texel against the CPU maximum.
//   speckle_atomic64 [texels per side] [keys per texel]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

static const char* kSrc = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Params { uint w; uint n; };
template<typename T> struct Desc { T value; };

kernel void cs_direct(texture2d<ulong, access::read_write> t [[texture(0)]], device const ulong* keys [[buffer(0)]],
                      constant Params& p [[buffer(1)]], uint gid [[thread_position_in_grid]]) {
    uint texel = gid / p.n;
    t.atomic_max(uint2(texel % p.w, texel / p.w), keys[gid]);
}

kernel void cs_table(device const void* table [[buffer(2)]], device const ulong* keys [[buffer(0)]],
                     constant Params& p [[buffer(1)]], uint gid [[thread_position_in_grid]]) {
    const device Desc<texture2d<ulong, access::read_write>>* d =
        static_cast<const device Desc<texture2d<ulong, access::read_write>>*>(table);
    uint texel = gid / p.n;
    d[3].value.atomic_max(uint2(texel % p.w, texel / p.w), keys[gid]);
}

struct VOut { float4 pos [[position]]; uint layer [[flat]] [[user(locn0)]]; };
vertex VOut vs_main(uint vid [[vertex_id]], uint iid [[instance_id]]) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.layer = iid;
    return o;
}
fragment float4 fs_table(VOut in [[stage_in]], device const void* table [[buffer(2)]],
                         device const ulong* keys [[buffer(0)]], constant Params& p [[buffer(1)]]) {
    const device Desc<texture2d<ulong, access::read_write>>* d =
        static_cast<const device Desc<texture2d<ulong, access::read_write>>*>(table);
    uint2 c = uint2(in.pos.xy);
    uint texel = c.y * p.w + c.x;
    d[3].value.atomic_max(c, keys[texel * p.n + in.layer]);
    return float4(0.0);
}
)MSL";

static uint64_t make_key(std::mt19937_64& r) {
    // high dword: a depth-like float in [0,1) (as bits); low dword: arbitrary payload. The low dword is drawn so that
    // it is often larger for the smaller high dword: a low-dword max picks a different key than the 64-bit max.
    uint64_t hi = r() & 0x3f7fffffull;
    uint64_t lo = r() & 0xffffffffull;
    return (hi << 32) | lo;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        const uint32_t w = argc > 1 ? uint32_t(atoi(argv[1])) : 256;
        const uint32_t n = argc > 2 ? uint32_t(atoi(argv[2])) : 64;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("device %s apple9 %d apple8 %d metal3 %d\n", dev.name.UTF8String,
               (int)[dev supportsFamily:MTLGPUFamilyApple9], (int)[dev supportsFamily:MTLGPUFamilyApple8],
               (int)[dev supportsFamily:MTLGPUFamilyMetal3]);
        NSError* err = nil;
        MTLCompileOptions* opt = [MTLCompileOptions new];
        opt.languageVersion = MTLLanguageVersion3_1;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:opt error:&err];
        if (!lib) {
            printf("COMPILE FAILED: %s\n", err.localizedDescription.UTF8String);
            return 2;
        }
        id<MTLComputePipelineState> cs_direct =
            [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs_direct"] error:&err];
        id<MTLComputePipelineState> cs_table =
            [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs_table"] error:&err];
        MTLRenderPipelineDescriptor* rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"vs_main"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"fs_table"];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
        rpd.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
        id<MTLRenderPipelineState> fs = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!cs_direct || !cs_table || !fs) {
            printf("PIPELINE FAILED: %s\n", err.localizedDescription.UTF8String);
            return 2;
        }
        const size_t texels = size_t(w) * w, total = texels * n;
        std::mt19937_64 rng(12345);
        std::vector<uint64_t> keys(total), want(texels, 0);
        for (size_t i = 0; i < total; ++i) {
            keys[i] = make_key(rng);
            want[i / n] = std::max(want[i / n], keys[i]);
        }
        size_t low_differs = 0;
        for (size_t t = 0; t < texels; ++t) {
            uint64_t lo_best = 0, lo_key = 0;
            for (size_t k = 0; k < n; ++k) {
                uint64_t v = keys[t * n + k];
                if ((v & 0xffffffffull) >= lo_best) { lo_best = v & 0xffffffffull; lo_key = v; }
            }
            low_differs += lo_key != want[t];
        }
        printf("%ux%u texels, %u keys each: a low-dword max would pick another key in %zu texels\n", w, w, n, low_differs);
        id<MTLBuffer> kb = [dev newBufferWithBytes:keys.data() length:total * 8 options:MTLResourceStorageModeShared];
        struct { uint32_t w, n; } p{w, n};
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int failures = 0;
        for (int atomic_usage = 0; atomic_usage < 2; ++atomic_usage) {
            for (int mode = 0; mode < 3; ++mode) {
                MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Uint
                                                                                              width:w height:w mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                if (atomic_usage) td.usage |= MTLTextureUsageShaderAtomic;
                id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
                std::vector<uint64_t> zero(texels, 0);
                [tex replaceRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:0 withBytes:zero.data() bytesPerRow:w * 8];
                id<MTLBuffer> table = [dev newBufferWithLength:8 * 8 options:MTLResourceStorageModeShared];
                ((MTLResourceID*)table.contents)[3] = tex.gpuResourceID;
                id<MTLCommandBuffer> cb = [q commandBuffer];
                const char* what = mode == 0 ? "compute, direct binding" : mode == 1 ? "compute, argument table" : "fragment, argument table";
                if (mode < 2) {
                    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
                    [e setComputePipelineState:mode == 0 ? cs_direct : cs_table];
                    [e setTexture:tex atIndex:0];
                    [e setBuffer:kb offset:0 atIndex:0];
                    [e setBytes:&p length:sizeof p atIndex:1];
                    [e setBuffer:table offset:0 atIndex:2];
                    if (mode == 1) [e useResource:tex usage:MTLResourceUsageRead | MTLResourceUsageWrite];
                    [e dispatchThreads:MTLSizeMake(total, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                    [e endEncoding];
                } else {
                    MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                                  width:w height:w mipmapped:NO];
                    cd.usage = MTLTextureUsageRenderTarget;
                    cd.storageMode = MTLStorageModePrivate;
                    id<MTLTexture> ct = [dev newTextureWithDescriptor:cd];
                    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = ct;
                    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                    rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
                    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
                    [e setRenderPipelineState:fs];
                    [e setFragmentBuffer:kb offset:0 atIndex:0];
                    [e setFragmentBytes:&p length:sizeof p atIndex:1];
                    [e setFragmentBuffer:table offset:0 atIndex:2];
                    [e useResource:tex usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageFragment];
                    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:n];
                    [e endEncoding];
                }
                [cb commit];
                [cb waitUntilCompleted];
                if (cb.status != MTLCommandBufferStatusCompleted) {
                    printf("%-26s usage atomic %d: command buffer status %ld %s\n", what, atomic_usage, (long)cb.status,
                           cb.error ? cb.error.localizedDescription.UTF8String : "");
                    ++failures;
                    continue;
                }
                std::vector<uint64_t> got(texels);
                [tex getBytes:got.data() bytesPerRow:w * 8 fromRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:0];
                size_t bad = 0, lo_only = 0;
                for (size_t t = 0; t < texels; ++t) {
                    bad += got[t] != want[t];
                    lo_only += got[t] != want[t] && (got[t] >> 32) != (want[t] >> 32);
                }
                printf("%-26s usage atomic %d: %zu of %zu texels differ from the 64-bit max (%zu with another high dword)%s\n",
                       what, atomic_usage, bad, texels, lo_only, bad ? "  FAIL" : "  ok");
                failures += bad != 0;
            }
        }
        printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: all cases exact");
        return failures ? 1 : 0;
    }
}
