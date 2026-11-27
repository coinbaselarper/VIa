// Cost of render-pass attachment load/store actions on this GPU: N passes a command buffer over the same targets,
// GPU time (GPUStartTime..GPUEndTime) / N, median of R command buffers, per configuration.
// build: clang++ -std=c++17 -O2 -fobjc-arc gbudget_tileprobe.mm -framework Metal -framework Foundation -o gta6rt_gbudget_tile
// run:   shaderc_msl/tools/tmem_slot.sh ./gta6rt_gbudget_tile [W H]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static const char* kSrc = R"(
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; };
vertex V vs(uint i [[vertex_id]]) { V v; float2 q[3] = {float2(-1,-1), float2(-0.999,-1), float2(-1,-0.999)}; v.p = float4(q[i], 0.5, 1); return v; }
struct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; float4 c4 [[color(4)]]; float4 c5 [[color(5)]]; };
fragment O fs() { O o; o.c0 = o.c1 = o.c2 = o.c3 = o.c4 = o.c5 = float4(0.5); return o; }
fragment O fs_full(V v [[stage_in]]) {
    uint2 p = uint2(v.p.xy); uint h = p.x * 73856093u ^ p.y * 19349663u; O o;
    o.c0 = unpack_unorm4x8_to_float(h * 2654435761u); o.c1 = unpack_unorm4x8_to_float(h * 2246822519u);
    o.c2 = unpack_unorm4x8_to_float(h * 3266489917u); o.c3 = unpack_unorm4x8_to_float(h * 668265263u);
    o.c4 = unpack_unorm4x8_to_float(h * 374761393u); o.c5 = float4(unpack_unorm4x8_to_float(h * 2654435769u)) * 7.3; return o; }
vertex V vs_full(uint i [[vertex_id]]) { V v; float2 q[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; v.p = float4(q[i], 0.5, 1); return v; }
)";

struct Config {
    std::string name;
    int colors;
    bool depth;
    MTLLoadAction load;
    MTLStoreAction store;
    int draw;
};

int main(int argc, char** argv) {
    @autoreleasepool {
        const int W = argc > 2 ? std::atoi(argv[1]) : 1280, H = argc > 2 ? std::atoi(argv[2]) : 720;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError* e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:nil error:&e];
        if (!lib) {
            std::fprintf(stderr, "library: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        const MTLPixelFormat fmts[6] = {MTLPixelFormatRGBA8Unorm_sRGB, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGB10A2Unorm,
                                        MTLPixelFormatRG16Float, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA16Float};
        const int bpp[6] = {4, 4, 4, 4, 4, 8};
        const int sets = std::getenv("TILE_SETS") ? std::max(1, std::atoi(std::getenv("TILE_SETS"))) : 1;
        std::vector<id<MTLTexture>> rts;
        for (int i = 0; i < 6 * sets; ++i) {
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmts[i % 6] width:W height:H mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModePrivate;
            rts.push_back([dev newTextureWithDescriptor:d]);
        }
        MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                                                      width:W height:H mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget;
        dd.storageMode = MTLStorageModePrivate;
        std::vector<id<MTLTexture>> dss_;
        for (int k = 0; k < sets; ++k) dss_.push_back([dev newTextureWithDescriptor:dd]);
        const bool noise = std::getenv("TILE_NOISE") != nullptr;
        if (noise) {
            id<MTLBuffer> rnd = [dev newBufferWithLength:size_t(W) * H * 8 options:MTLResourceStorageModeShared];
            auto* w = static_cast<uint32_t*>(rnd.contents);
            uint32_t x = 2463534242u;
            for (size_t i = 0; i < size_t(W) * H * 2; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; w[i] = x; }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            for (int i = 0; i < 6 * sets; ++i)
                [b copyFromBuffer:rnd sourceOffset:0 sourceBytesPerRow:size_t(W) * bpp[i % 6] sourceBytesPerImage:size_t(W) * H * bpp[i % 6]
                       sourceSize:MTLSizeMake(W, H, 1) toTexture:rts[i] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            std::printf("targets filled with noise; full-screen draws write noise\n");
        }
        auto pso = [&](int colors, bool depth, bool full) {
            MTLRenderPipelineDescriptor* p = [MTLRenderPipelineDescriptor new];
            p.vertexFunction = [lib newFunctionWithName:full ? @"vs_full" : @"vs"];
            p.fragmentFunction = [lib newFunctionWithName:full ? @"fs_full" : @"fs"];
            for (int i = 0; i < colors; ++i) p.colorAttachments[i].pixelFormat = fmts[i];
            if (depth) {
                p.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
                p.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            }
            NSError* pe = nil;
            id<MTLRenderPipelineState> s = [dev newRenderPipelineStateWithDescriptor:p error:&pe];
            if (!s) std::fprintf(stderr, "pso: %s\n", pe.localizedDescription.UTF8String);
            return s;
        };
        std::vector<Config> cfgs;
        const MTLLoadAction L = MTLLoadActionLoad, C = MTLLoadActionClear, D = MTLLoadActionDontCare;
        const MTLStoreAction S = MTLStoreActionStore, X = MTLStoreActionDontCare;
        for (int draw : {0, 1, 2}) {
            cfgs.push_back({"6rt+ds load/store", 6, true, L, S, draw});
            cfgs.push_back({"6rt+ds clear/store", 6, true, C, S, draw});
            cfgs.push_back({"6rt+ds dontcare/store", 6, true, D, S, draw});
            cfgs.push_back({"6rt+ds load/dontcare", 6, true, L, X, draw});
            cfgs.push_back({"6rt+ds dontcare/dontcare", 6, true, D, X, draw});
            cfgs.push_back({"3rt+ds load/store", 3, true, L, S, draw});
            cfgs.push_back({"1rt load/store", 1, false, L, S, draw});
            cfgs.push_back({"1rt dontcare/dontcare", 1, false, D, X, draw});
            cfgs.push_back({"ds only load/store", 0, true, L, S, draw});
        }
        const int N = 40, R = 7;
        std::printf("%d target sets rotated; ", sets);
        std::printf("%dx%d, %d passes a command buffer, median of %d; draw 0 = none, 1 = one 1-pixel triangle, 2 = one full-screen triangle\n", W, H, N, R);
        std::printf("%-28s draw  ms/pass  MiB moved/pass  GB/s\n", "config");
        for (const Config& c : cfgs) {
            id<MTLRenderPipelineState> ps = c.draw ? pso(c.colors, c.depth, c.draw == 2) : nil;
            id<MTLDepthStencilState> dss = nil;
            if (c.depth) {
                MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
                dsd.depthCompareFunction = MTLCompareFunctionAlways;
                dsd.depthWriteEnabled = YES;
                dss = [dev newDepthStencilStateWithDescriptor:dsd];
            }
            std::vector<double> t;
            for (int r = 0; r < R + 1; ++r) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                for (int n = 0; n < N; ++n) {
                    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    const int set = n % sets;
                    for (int i = 0; i < c.colors; ++i) {
                        rp.colorAttachments[i].texture = rts[set * 6 + i];
                        rp.colorAttachments[i].loadAction = c.load;
                        rp.colorAttachments[i].storeAction = c.store;
                        rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
                    }
                    if (c.depth) {
                        rp.depthAttachment.texture = dss_[set];
                        rp.depthAttachment.loadAction = c.load;
                        rp.depthAttachment.storeAction = c.store;
                        rp.stencilAttachment.texture = dss_[set];
                        rp.stencilAttachment.loadAction = c.load;
                        rp.stencilAttachment.storeAction = c.store;
                    }
                    rp.renderTargetWidth = W;
                    rp.renderTargetHeight = H;
                    if (!c.colors && !c.depth) rp.defaultRasterSampleCount = 1;
                    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                    if (ps) {
                        [enc setRenderPipelineState:ps];
                        if (dss) [enc setDepthStencilState:dss];
                        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                    }
                    [enc endEncoding];
                }
                [cb commit];
                [cb waitUntilCompleted];
                if (r) t.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e3 / N);
            }
            std::sort(t.begin(), t.end());
            int px_bytes = c.depth ? 5 : 0;
            for (int i = 0; i < c.colors; ++i) px_bytes += bpp[i];
            const double mib = double(px_bytes) * W * H / 1048576.0 *
                               ((c.load == L ? 1 : 0) + (c.store == S ? 1 : 0));
            const double ms = t[t.size() / 2];
            std::printf("%-28s %d    %7.3f  %8.1f       %6.1f\n", c.name.c_str(), c.draw, ms, mib,
                        ms > 0 ? mib * 1048576.0 / (ms * 1e-3) / 1e9 : 0.0);
        }
    }
    return 0;
}
