// Early fragment tests on this GPU: a full-screen draw whose fragment function writes a UAV (a device buffer, one word
// per pixel) under a stencil test that passes on half the pixels (a checkerboard written first), with and without
// [[early_fragment_tests]]; then the same with a depth test that fails on half the pixels. Counts the pixels whose UAV
// word was written. The console (DB_SHADER_CONTROL DEPTH_BEFORE_SHADER) writes only where the test passes.
//   speckle_early [size]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <cstdio>
#include <vector>

static const char* kSrc = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; };
vertex VOut vs_full(uint vid [[vertex_id]], constant float& z [[buffer(0)]]) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, z, 1.0);
    return o;
}
fragment float4 fs_mark(VOut in [[stage_in]], constant uint& w [[buffer(0)]]) {
    uint2 c = uint2(in.pos.xy);
    if (((c.x ^ c.y) & 1u) == 0u) discard_fragment();
    return float4(0.0);
}
fragment void fs_uav(VOut in [[stage_in]], device atomic_uint* out [[buffer(1)]], constant uint& w [[buffer(0)]]) {
    uint2 c = uint2(in.pos.xy);
    atomic_fetch_add_explicit(&out[c.y * w + c.x], 1u, memory_order_relaxed);
}
[[early_fragment_tests]] fragment void fs_uav_early(VOut in [[stage_in]], device atomic_uint* out [[buffer(1)]],
                                                    constant uint& w [[buffer(0)]]) {
    uint2 c = uint2(in.pos.xy);
    atomic_fetch_add_explicit(&out[c.y * w + c.x], 1u, memory_order_relaxed);
}
)MSL";

int main(int argc, char** argv) {
    @autoreleasepool {
        const uint32_t w = argc > 1 ? uint32_t(atoi(argv[1])) : 512;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError* err = nil;
        MTLCompileOptions* opt = [MTLCompileOptions new];
        opt.languageVersion = MTLLanguageVersion3_1;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:opt error:&err];
        if (!lib) {
            printf("COMPILE FAILED: %s\n", err.localizedDescription.UTF8String);
            return 2;
        }
        auto pipe = [&](NSString* fs, bool color) {
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = [lib newFunctionWithName:@"vs_full"];
            d.fragmentFunction = [lib newFunctionWithName:fs];
            d.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
            d.colorAttachments[0].writeMask = color ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
            d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            NSError* e = nil;
            id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&e];
            if (!p) printf("PIPELINE FAILED %s: %s\n", fs.UTF8String, e.localizedDescription.UTF8String);
            return p;
        };
        id<MTLRenderPipelineState> mark = pipe(@"fs_mark", false), late = pipe(@"fs_uav", false),
                                   early = pipe(@"fs_uav_early", false);
        if (!mark || !late || !early) return 2;
        MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                      width:w height:w mipmapped:NO];
        cd.usage = MTLTextureUsageRenderTarget;
        cd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> ct = [dev newTextureWithDescriptor:cd];
        MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                                                      width:w height:w mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget;
        dd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> dt = [dev newTextureWithDescriptor:dd];
        id<MTLBuffer> out = [dev newBufferWithLength:size_t(w) * w * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        auto ds = [&](bool stencil_test, bool depth_test, bool mark_pass) {
            MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
            d.depthCompareFunction = depth_test ? MTLCompareFunctionLess : MTLCompareFunctionAlways;
            d.depthWriteEnabled = mark_pass && depth_test;
            MTLStencilDescriptor* s = [MTLStencilDescriptor new];
            s.stencilCompareFunction = stencil_test ? MTLCompareFunctionEqual : MTLCompareFunctionAlways;
            s.depthStencilPassOperation = mark_pass ? MTLStencilOperationReplace : MTLStencilOperationKeep;
            d.frontFaceStencil = s;
            d.backFaceStencil = s;
            return [dev newDepthStencilStateWithDescriptor:d];
        };
        int failures = 0;
        for (int test = 0; test < 2; ++test) {
            const bool stencil = test == 0;
            for (int e = 0; e < 2; ++e) {
                memset(out.contents, 0, out.length);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = ct;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
                rp.depthAttachment.texture = dt;
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = 1.0;
                rp.depthAttachment.storeAction = MTLStoreActionDontCare;
                rp.stencilAttachment.texture = dt;
                rp.stencilAttachment.loadAction = MTLLoadActionClear;
                rp.stencilAttachment.clearStencil = 0;
                rp.stencilAttachment.storeAction = MTLStoreActionDontCare;
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                float zmark = 0.25f, zdraw = 0.5f;
                [enc setRenderPipelineState:mark];
                [enc setDepthStencilState:ds(false, !stencil, true)];
                [enc setStencilReferenceValue:1];
                [enc setVertexBytes:&zmark length:4 atIndex:0];
                [enc setFragmentBytes:&w length:4 atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc setRenderPipelineState:e ? early : late];
                [enc setDepthStencilState:ds(stencil, !stencil, false)];
                [enc setStencilReferenceValue:1];
                [enc setVertexBytes:&zdraw length:4 atIndex:0];
                [enc setFragmentBuffer:out offset:0 atIndex:1];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                const uint32_t* v = static_cast<const uint32_t*>(out.contents);
                size_t written = 0, in_pass = 0, in_fail = 0;
                for (uint32_t y = 0; y < w; ++y)
                    for (uint32_t x = 0; x < w; ++x) {
                        const bool passes = ((x ^ y) & 1u) != 0u ? stencil : !stencil;
                        if (v[y * w + x]) {
                            ++written;
                            (passes ? in_pass : in_fail) += 1;
                        }
                    }
                const bool want_ok = !e || in_fail == 0;
                printf("%-7s test, %-27s: UAV written at %zu of %u pixels (%zu where the test passes, %zu where it fails)%s\n",
                       stencil ? "stencil" : "depth", e ? "[[early_fragment_tests]]" : "no early fragment tests", written,
                       w * w, in_pass, in_fail, want_ok ? "" : "  FAIL");
                failures += !want_ok;
            }
        }
        printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: early tests keep UAV writes to passing pixels");
        return failures ? 1 : 0;
    }
}
