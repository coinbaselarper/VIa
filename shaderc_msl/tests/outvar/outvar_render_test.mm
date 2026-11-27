// Render test for the pixel output-type variants (tools/out_types.py): the synthetic shader of gen_outvar_spv.py,
// built by shaderc_msl.py --output-variants u,ui,fu, drawn into integer targets; every texel must hold the exact
// register dwords (compressed export: the pkrtz inputs; plain export: the dwords), including float-denormal,
// NaN and infinity bit patterns. The base entry with an integer target must be refused by Metal (control).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Push {
    std::uint32_t v[8];
    std::uint32_t scale;
    std::uint32_t pad[3];
};

std::uint16_t pkrtz_half(std::uint32_t a) {
    const std::uint32_t sign = (a >> 16) & 0x8000u, ab = a & 0x7fffffffu;
    const std::uint32_t nor = (ab - 939524096u) >> 13, ex = ab >> 23, mi = 0x800000u | (ab & 0x7fffffu);
    const std::uint32_t sb = 126u - ex, den = mi >> (sb < 25u ? sb : 25u);
    std::uint32_t v = ab >= 947912704u ? nor : den;
    v = ab >= 1199570944u ? 31743u : v;
    v = ab == 2139095040u ? 31744u : v;
    v = ab > 2139095040u ? (((ab >> 13) & 1023u) | 32256u) : v;
    return std::uint16_t(v | sign);
}

bool half_same(std::uint16_t a, std::uint16_t b) {
    const bool na = (a & 0x7c00) == 0x7c00 && (a & 0x3ff), nb = (b & 0x7c00) == 0x7c00 && (b & 0x3ff);
    return na || nb ? na && nb : a == b;
}

const char* kVertex = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 outvar_vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

struct Target {
    MTLPixelFormat format;
    std::uint32_t comps, bytes;
    bool half;
};

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) {
            std::fprintf(stderr, "usage: outvar_render_test <metallib>\n");
            return 2;
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError* err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&err];
        if (!lib) {
            std::fprintf(stderr, "metallib: %s\n", err.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLLibrary> vlib = [dev newLibraryWithSource:@(kVertex) options:nil error:&err];
        id<MTLFunction> vs = [vlib newFunctionWithName:@"outvar_vs"];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const std::uint32_t W = 4;
        const std::vector<std::vector<std::uint32_t>> patterns = {
            {0u, 1u, 5u, 0x7fffffu, 0x800000u, 0x3f800000u, 0x12345678u, 0xffffffffu},
            {0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f800001u, 0x80000001u, 0xdeadbeefu, 0x00010002u, 0x80000000u},
            {0x7fffffffu, 0x00003c00u, 0x0000ffffu, 0xffff0000u, 0x477fe000u, 0x33800000u, 0x7bff7bffu, 0x00000400u},
        };
        struct Config {
            const char* name;
            const char* entry;
            std::vector<Target> targets;
        };
        const std::vector<Config> configs = {
            {"R32Uint <- compressed (code u)", "main0_ou", {{MTLPixelFormatR32Uint, 1, 4, false}}},
            {"RG32Uint <- compressed (code u)", "main0_ou", {{MTLPixelFormatRG32Uint, 2, 8, false}}},
            {"RGBA32Uint <- compressed + R32Sint <- plain (code ui)", "main0_oui",
             {{MTLPixelFormatRGBA32Uint, 4, 16, false}, {MTLPixelFormatR32Sint, 1, 4, false}}},
            {"RGBA16Float <- compressed + R32Uint <- plain (code fu)", "main0_ofu",
             {{MTLPixelFormatRGBA16Float, 4, 8, true}, {MTLPixelFormatR32Uint, 1, 4, false}}},
            {"RG32Uint + RG32Uint (code uu, entry main0_ou: location 1 float)", "main0_ou",
             {{MTLPixelFormatRG32Uint, 2, 8, false}, {MTLPixelFormatRG32Uint, 2, 8, false}}},
        };
        int failures = 0, checked = 0;
        {
            MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = vs;
            pd.fragmentFunction = [lib newFunctionWithName:@"main0"];
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
            NSError* e = nil;
            id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
            std::printf("control: main0 with R32Uint: %s%s\n", p ? "CREATED (unexpected)" : "refused: ",
                        p ? "" : e.localizedDescription.UTF8String);
            if (p) ++failures;
        }
        for (std::size_t ci = 0; ci < configs.size(); ++ci) {
            const Config& c = configs[ci];
            MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = vs;
            pd.fragmentFunction = [lib newFunctionWithName:@(c.entry)];
            for (std::size_t t = 0; t < c.targets.size(); ++t) pd.colorAttachments[t].pixelFormat = c.targets[t].format;
            NSError* e = nil;
            id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
            const bool expect_refused = ci == 4;
            if (!pso) {
                std::printf("%s: pipeline refused: %s%s\n", c.name, e.localizedDescription.UTF8String,
                            expect_refused ? " (expected)" : "");
                if (!expect_refused) ++failures;
                continue;
            }
            if (expect_refused) {
                std::printf("%s: CREATED (unexpected)\n", c.name);
                ++failures;
                continue;
            }
            for (const auto& pat : patterns) {
                std::vector<id<MTLTexture>> tex;
                std::vector<id<MTLBuffer>> rb;
                MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                for (std::size_t t = 0; t < c.targets.size(); ++t) {
                    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.targets[t].format
                                                                                                  width:W height:1 mipmapped:NO];
                    td.usage = MTLTextureUsageRenderTarget;
                    td.storageMode = MTLStorageModePrivate;
                    tex.push_back([dev newTextureWithDescriptor:td]);
                    rb.push_back([dev newBufferWithLength:W * c.targets[t].bytes options:MTLResourceStorageModeShared]);
                    rp.colorAttachments[t].texture = tex.back();
                    rp.colorAttachments[t].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[t].clearColor = MTLClearColorMake(0, 0, 0, 0);
                    rp.colorAttachments[t].storeAction = MTLStoreActionStore;
                }
                Push push{};
                std::memcpy(push.v, pat.data(), sizeof push.v);
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:pso];
                [re setFragmentBytes:&push length:sizeof push atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [re endEncoding];
                id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                for (std::size_t t = 0; t < c.targets.size(); ++t)
                    [be copyFromTexture:tex[t] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                             sourceSize:MTLSizeMake(W, 1, 1) toBuffer:rb[t] destinationOffset:0
                    destinationBytesPerRow:W * c.targets[t].bytes destinationBytesPerImage:W * c.targets[t].bytes];
                [be endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                if (cb.status != MTLCommandBufferStatusCompleted) {
                    std::printf("%s: command buffer failed\n", c.name);
                    ++failures;
                    continue;
                }
                for (std::size_t t = 0; t < c.targets.size(); ++t) {
                    const std::uint32_t base = t == 0 ? 0 : 4;
                    for (std::uint32_t x = 0; x < W; ++x) {
                        for (std::uint32_t k = 0; k < c.targets[t].comps; ++k) {
                            ++checked;
                            const std::uint32_t want32 = pat[base + k];
                            if (c.targets[t].half) {
                                const std::uint16_t got = static_cast<const std::uint16_t*>(rb[t].contents)[x * 4 + k];
                                const std::uint16_t want = pkrtz_half(want32);
                                if (!half_same(got, want)) {
                                    if (++failures <= 20)
                                        std::printf("%s: target %zu texel %u channel %u: half 0x%04x, want 0x%04x (from 0x%08x)\n",
                                                    c.name, t, x, k, got, want, want32);
                                }
                            } else {
                                const std::uint32_t got = static_cast<const std::uint32_t*>(rb[t].contents)[x * c.targets[t].comps + k];
                                if (got != want32 && ++failures <= 20)
                                    std::printf("%s: target %zu texel %u channel %u: 0x%08x, want 0x%08x\n", c.name, t,
                                                x, k, got, want32);
                            }
                        }
                    }
                }
            }
            std::printf("%s: drawn, %zu patterns\n", c.name, patterns.size());
        }
        std::printf("outvar render test: %d channel values checked, %d failures (%s)\n", checked, failures,
                    dev.name.UTF8String);
        return failures ? 1 : 0;
    }
}
