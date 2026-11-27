// DXGI -> MTLVertexFormat: every format port/d3d12_metal's input layouts accept maps to the expected Metal format,
// the rest stay Invalid, and Metal builds a render pipeline for each mapped format with an attribute of its type class.
#include "../src/mtl_internal.h"

#include <cstdio>
#include <string>

using namespace gta6rt::metal;

namespace {

enum Cls { F, U, I };

struct Want {
    std::uint32_t dxgi;
    const char* name;
    MTLVertexFormat vf;
    Cls cls;
};

const Want kWant[] = {
    {2, "R32G32B32A32_FLOAT", MTLVertexFormatFloat4, F},
    {3, "R32G32B32A32_UINT", MTLVertexFormatUInt4, U},
    {4, "R32G32B32A32_SINT", MTLVertexFormatInt4, I},
    {6, "R32G32B32_FLOAT", MTLVertexFormatFloat3, F},
    {7, "R32G32B32_UINT", MTLVertexFormatUInt3, U},
    {8, "R32G32B32_SINT", MTLVertexFormatInt3, I},
    {10, "R16G16B16A16_FLOAT", MTLVertexFormatHalf4, F},
    {11, "R16G16B16A16_UNORM", MTLVertexFormatUShort4Normalized, F},
    {12, "R16G16B16A16_UINT", MTLVertexFormatUShort4, U},
    {13, "R16G16B16A16_SNORM", MTLVertexFormatShort4Normalized, F},
    {14, "R16G16B16A16_SINT", MTLVertexFormatShort4, I},
    {16, "R32G32_FLOAT", MTLVertexFormatFloat2, F},
    {17, "R32G32_UINT", MTLVertexFormatUInt2, U},
    {18, "R32G32_SINT", MTLVertexFormatInt2, I},
    {24, "R10G10B10A2_UNORM", MTLVertexFormatUInt1010102Normalized, F},
    {26, "R11G11B10_FLOAT", MTLVertexFormatFloatRG11B10, F},
    {28, "R8G8B8A8_UNORM", MTLVertexFormatUChar4Normalized, F},
    {30, "R8G8B8A8_UINT", MTLVertexFormatUChar4, U},
    {31, "R8G8B8A8_SNORM", MTLVertexFormatChar4Normalized, F},
    {32, "R8G8B8A8_SINT", MTLVertexFormatChar4, I},
    {34, "R16G16_FLOAT", MTLVertexFormatHalf2, F},
    {35, "R16G16_UNORM", MTLVertexFormatUShort2Normalized, F},
    {36, "R16G16_UINT", MTLVertexFormatUShort2, U},
    {37, "R16G16_SNORM", MTLVertexFormatShort2Normalized, F},
    {38, "R16G16_SINT", MTLVertexFormatShort2, I},
    {41, "R32_FLOAT", MTLVertexFormatFloat, F},
    {42, "R32_UINT", MTLVertexFormatUInt, U},
    {43, "R32_SINT", MTLVertexFormatInt, I},
    {49, "R8G8_UNORM", MTLVertexFormatUChar2Normalized, F},
    {50, "R8G8_UINT", MTLVertexFormatUChar2, U},
    {51, "R8G8_SNORM", MTLVertexFormatChar2Normalized, F},
    {52, "R8G8_SINT", MTLVertexFormatChar2, I},
    {54, "R16_FLOAT", MTLVertexFormatHalf, F},
    {56, "R16_UNORM", MTLVertexFormatUShortNormalized, F},
    {57, "R16_UINT", MTLVertexFormatUShort, U},
    {58, "R16_SNORM", MTLVertexFormatShortNormalized, F},
    {59, "R16_SINT", MTLVertexFormatShort, I},
    {61, "R8_UNORM", MTLVertexFormatUCharNormalized, F},
    {62, "R8_UINT", MTLVertexFormatUChar, U},
    {63, "R8_SNORM", MTLVertexFormatCharNormalized, F},
    {64, "R8_SINT", MTLVertexFormatChar, I},
    {87, "B8G8R8A8_UNORM", MTLVertexFormatUChar4Normalized_BGRA, F},
};

const char* kSource = R"(
#include <metal_stdlib>
using namespace metal;
struct InF { float4 a [[attribute(0)]]; };
struct InU { uint4 a [[attribute(0)]]; };
struct InI { int4 a [[attribute(0)]]; };
vertex float4 vs_f(InF in [[stage_in]]) { return in.a; }
vertex float4 vs_u(InU in [[stage_in]]) { return float4(in.a); }
vertex float4 vs_i(InI in [[stage_in]]) { return float4(in.a); }
fragment float4 fs() { return float4(1); }
)";

}  // namespace

int main() {
    int failures = 0;
    for (std::uint32_t d = 0; d < 192; ++d) {
        const Want* w = nullptr;
        for (const Want& x : kWant)
            if (x.dxgi == d) w = &x;
        const MTLVertexFormat got = vertex_format(d);
        const MTLVertexFormat want = w ? w->vf : MTLVertexFormatInvalid;
        if (got != want) {
            std::printf("FAIL dxgi %u (%s): got %ld, want %ld\n", d, w ? w->name : "unmapped", long(got), long(want));
            ++failures;
        }
    }
    if (vertex_format(25) != MTLVertexFormatInvalid) {
        std::printf("FAIL R10G10B10A2_UINT must stay unmapped\n");
        ++failures;
    }

    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            std::printf("FAIL no Metal device\n");
            return 1;
        }
        NSError* e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSource) options:nil error:&e];
        if (!lib) {
            std::printf("FAIL library: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLFunction> fs = [lib newFunctionWithName:@"fs"];
        id<MTLFunction> vs[3] = {[lib newFunctionWithName:@"vs_f"], [lib newFunctionWithName:@"vs_u"],
                                 [lib newFunctionWithName:@"vs_i"]};
        int built = 0;
        for (const Want& w : kWant) {
            MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[0].format = vertex_format(w.dxgi);
            vd.attributes[0].offset = 0;
            vd.attributes[0].bufferIndex = 0;
            vd.layouts[0].stride = 16;
            MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = vs[w.cls];
            pd.fragmentFunction = fs;
            pd.vertexDescriptor = vd;
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            NSError* pe = nil;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&pe];
            if (!ps) {
                std::printf("FAIL pipeline %s: %s\n", w.name, pe.localizedDescription.UTF8String);
                ++failures;
            } else {
                ++built;
            }
        }
        std::printf("%d of %zu vertex formats built a pipeline\n", built, sizeof(kWant) / sizeof(kWant[0]));
    }
    std::printf("metal_test_vertex_format: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
