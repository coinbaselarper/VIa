#include "mtl_internal.h"

#include <cmath>

namespace gta6rt::metal {

namespace {

struct Row {
    std::uint32_t dxgi;
    MTLPixelFormat pixel;
    std::uint32_t bytes;
    std::uint32_t dim;
    bool depth, stencil, emulated;
    const char* name;
    const char* note;
};

const Row kRows[] = {
    {format::r32g32b32a32_float, MTLPixelFormatRGBA32Float, 16, 1, false, false, false, "R32G32B32A32_FLOAT", ""},
    {format::r32g32b32a32_uint, MTLPixelFormatRGBA32Uint, 16, 1, false, false, false, "R32G32B32A32_UINT", ""},
    {format::r16g16b16a16_float, MTLPixelFormatRGBA16Float, 8, 1, false, false, false, "R16G16B16A16_FLOAT", ""},
    {format::r16g16b16a16_unorm, MTLPixelFormatRGBA16Unorm, 8, 1, false, false, false, "R16G16B16A16_UNORM", ""},
    {format::r32g32_float, MTLPixelFormatRG32Float, 8, 1, false, false, false, "R32G32_FLOAT", ""},
    {format::d32_float_s8x24_uint, MTLPixelFormatDepth32Float_Stencil8, 8, 1, true, true, false, "D32_FLOAT_S8X24_UINT", ""},
    {format::r10g10b10a2_unorm, MTLPixelFormatRGB10A2Unorm, 4, 1, false, false, false, "R10G10B10A2_UNORM", ""},
    {format::r11g11b10_float, MTLPixelFormatRG11B10Float, 4, 1, false, false, false, "R11G11B10_FLOAT", ""},
    {format::r8g8b8a8_unorm, MTLPixelFormatRGBA8Unorm, 4, 1, false, false, false, "R8G8B8A8_UNORM", ""},
    {format::r8g8b8a8_unorm_srgb, MTLPixelFormatRGBA8Unorm_sRGB, 4, 1, false, false, false, "R8G8B8A8_UNORM_SRGB", ""},
    {format::r8g8b8a8_uint, MTLPixelFormatRGBA8Uint, 4, 1, false, false, false, "R8G8B8A8_UINT", ""},
    {format::r16g16_float, MTLPixelFormatRG16Float, 4, 1, false, false, false, "R16G16_FLOAT", ""},
    {format::r16g16_unorm, MTLPixelFormatRG16Unorm, 4, 1, false, false, false, "R16G16_UNORM", ""},
    {format::r16g16_uint, MTLPixelFormatRG16Uint, 4, 1, false, false, false, "R16G16_UINT", ""},
    {format::d32_float, MTLPixelFormatDepth32Float, 4, 1, true, false, false, "D32_FLOAT", ""},
    {format::r32_float, MTLPixelFormatR32Float, 4, 1, false, false, false, "R32_FLOAT", ""},
    {format::r32_uint, MTLPixelFormatR32Uint, 4, 1, false, false, false, "R32_UINT", ""},
    {format::d24_unorm_s8_uint, MTLPixelFormatDepth32Float_Stencil8, 8, 1, true, true, true, "D24_UNORM_S8_UINT",
     "stored as Depth32Float_Stencil8; clears quantised to D24 codes (DESIGN.md 5.3)"},
    {format::r8g8_unorm, MTLPixelFormatRG8Unorm, 2, 1, false, false, false, "R8G8_UNORM", ""},
    {format::r16_float, MTLPixelFormatR16Float, 2, 1, false, false, false, "R16_FLOAT", ""},
    {format::d16_unorm, MTLPixelFormatDepth16Unorm, 2, 1, true, false, false, "D16_UNORM", ""},
    {format::r16_unorm, MTLPixelFormatR16Unorm, 2, 1, false, false, false, "R16_UNORM", ""},
    {format::r16_uint, MTLPixelFormatR16Uint, 2, 1, false, false, false, "R16_UINT", ""},
    {format::r8_unorm, MTLPixelFormatR8Unorm, 1, 1, false, false, false, "R8_UNORM", ""},
    {format::r8_uint, MTLPixelFormatR8Uint, 1, 1, false, false, false, "R8_UINT", ""},
    {format::a8_unorm, MTLPixelFormatA8Unorm, 1, 1, false, false, false, "A8_UNORM", ""},
    {format::r9g9b9e5_sharedexp, MTLPixelFormatRGB9E5Float, 4, 1, false, false, false, "R9G9B9E5_SHAREDEXP", ""},
    {format::bc1_unorm, MTLPixelFormatBC1_RGBA, 8, 4, false, false, false, "BC1_UNORM", ""},
    {format::bc2_unorm, MTLPixelFormatBC2_RGBA, 16, 4, false, false, false, "BC2_UNORM", ""},
    {format::bc3_unorm, MTLPixelFormatBC3_RGBA, 16, 4, false, false, false, "BC3_UNORM", ""},
    {format::bc4_unorm, MTLPixelFormatBC4_RUnorm, 8, 4, false, false, false, "BC4_UNORM", ""},
    {format::bc5_unorm, MTLPixelFormatBC5_RGUnorm, 16, 4, false, false, false, "BC5_UNORM", ""},
    {format::b5g6r5_unorm, MTLPixelFormatB5G6R5Unorm, 2, 1, false, false, false, "B5G6R5_UNORM", ""},
    {format::b8g8r8a8_unorm, MTLPixelFormatBGRA8Unorm, 4, 1, false, false, false, "B8G8R8A8_UNORM", ""},
    {format::b8g8r8a8_unorm_srgb, MTLPixelFormatBGRA8Unorm_sRGB, 4, 1, false, false, false, "B8G8R8A8_UNORM_SRGB", ""},
    {format::bc6h_uf16, MTLPixelFormatBC6H_RGBUfloat, 16, 4, false, false, false, "BC6H_UF16", ""},
    {format::bc7_unorm, MTLPixelFormatBC7_RGBAUnorm, 16, 4, false, false, false, "BC7_UNORM", ""},
    // v3 (port/d3d12_metal): the remaining DXGI formats leonida's layer maps, each a native Metal format.
    {4, MTLPixelFormatRGBA32Sint, 16, 1, false, false, false, "R32G32B32A32_SINT", ""},
    {12, MTLPixelFormatRGBA16Uint, 8, 1, false, false, false, "R16G16B16A16_UINT", ""},
    {13, MTLPixelFormatRGBA16Snorm, 8, 1, false, false, false, "R16G16B16A16_SNORM", ""},
    {14, MTLPixelFormatRGBA16Sint, 8, 1, false, false, false, "R16G16B16A16_SINT", ""},
    {17, MTLPixelFormatRG32Uint, 8, 1, false, false, false, "R32G32_UINT", ""},
    {18, MTLPixelFormatRG32Sint, 8, 1, false, false, false, "R32G32_SINT", ""},
    {25, MTLPixelFormatRGB10A2Uint, 4, 1, false, false, false, "R10G10B10A2_UINT", ""},
    {31, MTLPixelFormatRGBA8Snorm, 4, 1, false, false, false, "R8G8B8A8_SNORM", ""},
    {32, MTLPixelFormatRGBA8Sint, 4, 1, false, false, false, "R8G8B8A8_SINT", ""},
    {37, MTLPixelFormatRG16Snorm, 4, 1, false, false, false, "R16G16_SNORM", ""},
    {38, MTLPixelFormatRG16Sint, 4, 1, false, false, false, "R16G16_SINT", ""},
    {43, MTLPixelFormatR32Sint, 4, 1, false, false, false, "R32_SINT", ""},
    {50, MTLPixelFormatRG8Uint, 2, 1, false, false, false, "R8G8_UINT", ""},
    {51, MTLPixelFormatRG8Snorm, 2, 1, false, false, false, "R8G8_SNORM", ""},
    {52, MTLPixelFormatRG8Sint, 2, 1, false, false, false, "R8G8_SINT", ""},
    {58, MTLPixelFormatR16Snorm, 2, 1, false, false, false, "R16_SNORM", ""},
    {59, MTLPixelFormatR16Sint, 2, 1, false, false, false, "R16_SINT", ""},
    {63, MTLPixelFormatR8Snorm, 1, 1, false, false, false, "R8_SNORM", ""},
    {64, MTLPixelFormatR8Sint, 1, 1, false, false, false, "R8_SINT", ""},
    {72, MTLPixelFormatBC1_RGBA_sRGB, 8, 4, false, false, false, "BC1_UNORM_SRGB", ""},
    {75, MTLPixelFormatBC2_RGBA_sRGB, 16, 4, false, false, false, "BC2_UNORM_SRGB", ""},
    {78, MTLPixelFormatBC3_RGBA_sRGB, 16, 4, false, false, false, "BC3_UNORM_SRGB", ""},
    {81, MTLPixelFormatBC4_RSnorm, 8, 4, false, false, false, "BC4_SNORM", ""},
    {84, MTLPixelFormatBC5_RGSnorm, 16, 4, false, false, false, "BC5_SNORM", ""},
    {86, MTLPixelFormatBGR5A1Unorm, 2, 1, false, false, false, "B5G5R5A1_UNORM", ""},
    {96, MTLPixelFormatBC6H_RGBFloat, 16, 4, false, false, false, "BC6H_SF16", ""},
    {99, MTLPixelFormatBC7_RGBAUnorm_sRGB, 16, 4, false, false, false, "BC7_UNORM_SRGB", ""},
    {format::b4g4r4a4_unorm, MTLPixelFormatABGR4Unorm, 2, 1, false, false, true, "B4G4R4A4_UNORM",
     "ABGR4Unorm (R in bits 12-15); sampled through a swizzle view in DXGI order; as a render target the stored bits "
     "are Metal's order (measured, DESIGN.md F17)"},
};

}  // namespace

bool native_format(std::uint32_t dxgi, NativeFormat& out) {
    for (const Row& r : kRows) {
        if (r.dxgi != dxgi) continue;
        out.pixel = r.pixel;
        out.info.dxgi = r.dxgi;
        out.info.native = std::uint32_t(r.pixel);
        out.info.block_bytes = r.bytes;
        out.info.block_dim = r.dim;
        out.info.depth = r.depth;
        out.info.stencil = r.stencil;
        out.info.emulated = r.emulated;
        out.info.name = r.name;
        out.info.note = r.note;
        out.swizzled = r.dxgi == format::b4g4r4a4_unorm;
        if (out.swizzled)
            out.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                                        MTLTextureSwizzleAlpha, MTLTextureSwizzleRed);
        return true;
    }
    return false;
}

MTLVertexFormat vertex_format(std::uint32_t dxgi) {
    switch (dxgi) {
    case format::r32g32b32a32_float: return MTLVertexFormatFloat4;
    case format::r32g32b32a32_uint: return MTLVertexFormatUInt4;
    case format::r32g32b32_float: return MTLVertexFormatFloat3;
    case format::r32g32_float: return MTLVertexFormatFloat2;
    case format::r32_float: return MTLVertexFormatFloat;
    case format::r32_uint: return MTLVertexFormatUInt;
    case format::r16g16b16a16_float: return MTLVertexFormatHalf4;
    case format::r16g16b16a16_unorm: return MTLVertexFormatUShort4Normalized;
    case format::r16g16_float: return MTLVertexFormatHalf2;
    case format::r8g8b8a8_unorm: return MTLVertexFormatUChar4Normalized;
    case format::r8g8b8a8_uint: return MTLVertexFormatUChar4;
    case format::b8g8r8a8_unorm: return MTLVertexFormatUChar4Normalized_BGRA;
    case format::r10g10b10a2_unorm: return MTLVertexFormatUInt1010102Normalized;
    case format::r8g8_unorm: return MTLVertexFormatUChar2Normalized;
    case format::r16_float: return MTLVertexFormatHalf;
    case format::r8_unorm: return MTLVertexFormatUCharNormalized;
    default: return MTLVertexFormatInvalid;
    }
}

float quantize_d24(float depth) {
    if (!(depth > 0.0f)) return 0.0f;
    if (depth >= 1.0f) return 1.0f;
    const float product = depth * 16777215.0f;
    return float(double(std::nearbyint(product)) / 16777215.0);
}

}  // namespace gta6rt::metal
