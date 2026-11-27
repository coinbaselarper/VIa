#include "metal_upscaler.h"

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <MetalFX/MetalFX.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace gta6rt::upscale {

const char* mode_name(Mode m) {
    switch (m) {
    case Mode::fsr4: return "fsr4";
    case Mode::metalfx_temporal: return "metalfx-temporal";
    case Mode::metalfx_spatial: return "metalfx-spatial";
    case Mode::bilinear: return "bilinear";
    }
    return "?";
}

Conventions default_conventions() {
    Conventions c;
    auto sign = [](const char* name, float def) {
        const char* v = std::getenv(name);
        if (!v || !*v) return def;
        return std::strtof(v, nullptr) < 0 ? -1.0f : 1.0f;
    };
    c.jitter_sign_x = sign("GTA6RT_UPSCALE_JITTER_SX", -1);
    c.jitter_sign_y = sign("GTA6RT_UPSCALE_JITTER_SY", -1);
    c.motion_sign_x = sign("GTA6RT_UPSCALE_MOTION_SX", 1);
    c.motion_sign_y = sign("GTA6RT_UPSCALE_MOTION_SY", 1);
    return c;
}

namespace {

const char* kKernels = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Scale { float2 uv_scale; uint2 out_size; };
kernel void up_bilinear(texture2d<float, access::sample> src [[texture(0)]],
                        texture2d<float, access::write> dst [[texture(1)]],
                        constant Scale& p [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.out_size.x || gid.y >= p.out_size.y) return;
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
    float2 uv = (float2(gid) + 0.5) / float2(p.out_size) * p.uv_scale;
    dst.write(src.sample(s, uv, level(0)), gid);
}
kernel void up_copy(texture2d<float, access::read> src [[texture(0)]],
                    texture2d<float, access::write> dst [[texture(1)]],
                    constant Scale& p [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.out_size.x || gid.y >= p.out_size.y) return;
    dst.write(src.read(gid), gid);
}
kernel void up_depth_r32(depth2d<float, access::read> src [[texture(0)]],
                         texture2d<float, access::write> dst [[texture(1)]],
                         constant Scale& p [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.out_size.x || gid.y >= p.out_size.y) return;
    dst.write(float4(src.read(gid)), gid);
}
)MSL";

struct ScaleParams {
    float uv_scale[2];
    std::uint32_t out_size[2];
};

bool is_depth_format(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatDepth16Unorm:
    case MTLPixelFormatDepth32Float:
    case MTLPixelFormatDepth32Float_Stencil8:
    case MTLPixelFormatX32_Stencil8:
        return true;
    default:
        return false;
    }
}

}  // namespace

struct MetalUpscaler::Impl {
    id<MTLDevice> device;
    Mode mode;
    Conventions conv = default_conventions();
    id<MTLComputePipelineState> bilinear = nil, copy = nil, depth_r32 = nil;
    id<MTLFXTemporalScaler> temporal = nil;
    id<MTLFXSpatialScaler> spatial = nil;
    std::string key;
    id<MTLTexture> depth_tmp = nil, out_tmp = nil;
    bool depth_convert = false;
    bool first = true;
    std::string error;

    bool kernels(std::string& why) {
        if (bilinear) return true;
        NSError* err = nil;
        MTLCompileOptions* opt = [MTLCompileOptions new];
        id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kKernels] options:opt error:&err];
        if (!lib) {
            why = std::string("upscale kernels did not compile: ") + (err ? err.localizedDescription.UTF8String : "?");
            return false;
        }
        auto make = [&](const char* name) -> id<MTLComputePipelineState> {
            id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:name]];
            NSError* e = nil;
            id<MTLComputePipelineState> ps = fn ? [device newComputePipelineStateWithFunction:fn error:&e] : nil;
            if (!ps) why = std::string("upscale kernel ") + name + ": " + (e ? e.localizedDescription.UTF8String : "no function");
            return ps;
        };
        bilinear = make("up_bilinear");
        copy = make("up_copy");
        depth_r32 = make("up_depth_r32");
        return bilinear && copy && depth_r32;
    }

    void dispatch(id<MTLCommandBuffer> cb, id<MTLComputePipelineState> ps, id<MTLTexture> src, id<MTLTexture> dst,
                  float sx, float sy, std::uint32_t w, std::uint32_t h, const char* label) {
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        enc.label = [NSString stringWithUTF8String:label];
        [enc setComputePipelineState:ps];
        [enc setTexture:src atIndex:0];
        [enc setTexture:dst atIndex:1];
        ScaleParams p{{sx, sy}, {w, h}};
        [enc setBytes:&p length:sizeof p atIndex:0];
        [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [enc endEncoding];
    }

    id<MTLTexture> make_texture(MTLPixelFormat f, std::uint32_t w, std::uint32_t h, MTLTextureUsage usage) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
        d.usage = usage;
        d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [device newTextureWithDescriptor:d];
        t.label = @"gta6rt upscale intermediate";
        return t;
    }

    bool output_target(const MetalInputs& in, MTLTextureUsage need, __strong id<MTLTexture>& target) {
        const bool direct = (in.output.usage & need) == need && in.output.width >= in.output_w && in.output.height >= in.output_h;
        if (direct) {
            target = in.output;
            return true;
        }
        if (!(in.output.usage & MTLTextureUsageShaderWrite)) return false;
        if (!out_tmp || out_tmp.pixelFormat != in.output.pixelFormat || out_tmp.width != in.output_w ||
            out_tmp.height != in.output_h || (out_tmp.usage & need) != need)
            out_tmp = make_texture(in.output.pixelFormat, in.output_w, in.output_h, need | MTLTextureUsageShaderRead);
        target = out_tmp;
        return target != nil;
    }

    bool encode_bilinear(id<MTLCommandBuffer> cb, const MetalInputs& in, std::string& why) {
        if (!kernels(why)) return false;
        if (!(in.output.usage & MTLTextureUsageShaderWrite)) {
            why = "the output texture has no shader-write usage";
            return false;
        }
        const float sx = float(in.render_w) / float(in.color.width), sy = float(in.render_h) / float(in.color.height);
        dispatch(cb, bilinear, in.color, in.output, sx, sy, in.output_w, in.output_h, "gta6rt upscale bilinear");
        return true;
    }

    bool encode_spatial(id<MTLCommandBuffer> cb, const MetalInputs& in, std::string& why) {
        char k[160];
        std::snprintf(k, sizeof k, "s %lu %lu %ux%u %ux%u", (unsigned long)in.color.pixelFormat,
                      (unsigned long)in.output.pixelFormat, in.render_w, in.render_h, in.output_w, in.output_h);
        if (!spatial || key != k) {
            MTLFXSpatialScalerDescriptor* d = [MTLFXSpatialScalerDescriptor new];
            d.colorTextureFormat = in.color.pixelFormat;
            d.outputTextureFormat = in.output.pixelFormat;
            d.inputWidth = in.render_w;
            d.inputHeight = in.render_h;
            d.outputWidth = in.output_w;
            d.outputHeight = in.output_h;
            d.colorProcessingMode = in.hdr ? MTLFXSpatialScalerColorProcessingModeHDR : MTLFXSpatialScalerColorProcessingModeLinear;
            if (![MTLFXSpatialScalerDescriptor supportsDevice:device]) {
                why = "MetalFX spatial is not supported on this device";
                return false;
            }
            spatial = [d newSpatialScalerWithDevice:device];
            if (!spatial) {
                why = std::string("MetalFX refused a spatial scaler for ") + k;
                return false;
            }
            key = k;
            std::fprintf(stderr, "[upscale] MetalFX spatial scaler %s: color usage %#lx, output usage %#lx\n", k,
                         (unsigned long)spatial.colorTextureUsage, (unsigned long)spatial.outputTextureUsage);
        }
        if ((in.color.usage & spatial.colorTextureUsage) != spatial.colorTextureUsage) {
            why = "the colour texture lacks the usage MetalFX spatial needs";
            return false;
        }
        id<MTLTexture> target = nil;
        if (!output_target(in, spatial.outputTextureUsage, target)) {
            why = "no output texture MetalFX spatial can write";
            return false;
        }
        spatial.colorTexture = in.color;
        spatial.outputTexture = target;
        spatial.inputContentWidth = in.render_w;
        spatial.inputContentHeight = in.render_h;
        [spatial encodeToCommandBuffer:cb];
        if (target != in.output) {
            if (!kernels(why)) return false;
            dispatch(cb, copy, target, in.output, 1, 1, in.output_w, in.output_h, "gta6rt upscale copy");
        }
        return true;
    }

    bool encode_temporal(id<MTLCommandBuffer> cb, const MetalInputs& in, std::string& why) {
        MTLPixelFormat depth_format = in.depth.pixelFormat;
        const bool convert = depth_format != MTLPixelFormatDepth32Float && depth_format != MTLPixelFormatR32Float &&
                             depth_format != MTLPixelFormatR16Float && depth_format != MTLPixelFormatDepth16Unorm;
        if (convert) depth_format = MTLPixelFormatR32Float;
        char k[200];
        std::snprintf(k, sizeof k, "t %lu %lu %lu %lu %ux%u %ux%u %d %d", (unsigned long)in.color.pixelFormat,
                      (unsigned long)depth_format, (unsigned long)in.motion.pixelFormat,
                      (unsigned long)in.output.pixelFormat, in.render_w, in.render_h, in.output_w, in.output_h,
                      int(in.auto_exposure), int(convert));
        bool fresh = false;
        if (!temporal || key != k) {
            if (![MTLFXTemporalScalerDescriptor supportsDevice:device]) {
                why = "MetalFX temporal is not supported on this device";
                return false;
            }
            MTLFXTemporalScalerDescriptor* d = [MTLFXTemporalScalerDescriptor new];
            d.colorTextureFormat = in.color.pixelFormat;
            d.depthTextureFormat = depth_format;
            d.motionTextureFormat = in.motion.pixelFormat;
            d.outputTextureFormat = in.output.pixelFormat;
            d.inputWidth = in.render_w;
            d.inputHeight = in.render_h;
            d.outputWidth = in.output_w;
            d.outputHeight = in.output_h;
            d.autoExposureEnabled = in.auto_exposure;
            temporal = [d newTemporalScalerWithDevice:device];
            if (!temporal) {
                why = std::string("MetalFX refused a temporal scaler for ") + k;
                return false;
            }
            key = k;
            fresh = true;
            depth_convert = convert;
            std::fprintf(stderr,
                         "[upscale] MetalFX temporal scaler %s: usage colour %#lx depth %#lx motion %#lx output %#lx%s\n", k,
                         (unsigned long)temporal.colorTextureUsage, (unsigned long)temporal.depthTextureUsage,
                         (unsigned long)temporal.motionTextureUsage, (unsigned long)temporal.outputTextureUsage,
                         convert ? " (depth copied to R32Float)" : "");
        }
        id<MTLTexture> depth = in.depth;
        if (depth_convert) {
            if (!kernels(why)) return false;
            if (!is_depth_format(in.depth.pixelFormat)) {
                why = "depth texture format MetalFX does not take and not a depth format";
                return false;
            }
            if (!depth_tmp || depth_tmp.width != in.render_w || depth_tmp.height != in.render_h)
                depth_tmp = make_texture(MTLPixelFormatR32Float, in.render_w, in.render_h,
                                         MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
            dispatch(cb, depth_r32, in.depth, depth_tmp, 1, 1, in.render_w, in.render_h, "gta6rt upscale depth");
            depth = depth_tmp;
        }
        id<MTLTexture> target = nil;
        if (!output_target(in, temporal.outputTextureUsage, target)) {
            why = "no output texture MetalFX temporal can write";
            return false;
        }
        temporal.colorTexture = in.color;
        temporal.depthTexture = depth;
        temporal.motionTexture = in.motion;
        temporal.outputTexture = target;
        temporal.inputContentWidth = in.render_w;
        temporal.inputContentHeight = in.render_h;
        temporal.jitterOffsetX = conv.jitter_sign_x * in.jitter_x;
        temporal.jitterOffsetY = conv.jitter_sign_y * in.jitter_y;
        temporal.motionVectorScaleX = conv.motion_sign_x * in.motion_scale_x;
        temporal.motionVectorScaleY = conv.motion_sign_y * in.motion_scale_y;
        temporal.preExposure = in.pre_exposure > 0 ? in.pre_exposure : 1.0f;
        temporal.depthReversed = in.depth_inverted;
        temporal.reset = in.reset || fresh;
        [temporal encodeToCommandBuffer:cb];
        if (target != in.output) {
            if (!kernels(why)) return false;
            dispatch(cb, copy, target, in.output, 1, 1, in.output_w, in.output_h, "gta6rt upscale copy");
        }
        return true;
    }
};

MetalUpscaler::MetalUpscaler(id<MTLDevice> device, Mode mode) : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
    impl_->mode = mode;
}

MetalUpscaler::~MetalUpscaler() = default;

Mode MetalUpscaler::mode() const { return impl_->mode; }

void MetalUpscaler::set_conventions(const Conventions& c) { impl_->conv = c; }

bool MetalUpscaler::encode(id<MTLCommandBuffer> cb, const MetalInputs& in, std::string& why) {
    if (!in.color || !in.output) {
        why = "no colour or output texture";
        return false;
    }
    if (!in.render_w || !in.render_h || !in.output_w || !in.output_h || in.render_w > in.color.width ||
        in.render_h > in.color.height || in.output_w > in.output.width || in.output_h > in.output.height) {
        why = "render or output size outside the textures";
        return false;
    }
    switch (impl_->mode) {
    case Mode::bilinear: return impl_->encode_bilinear(cb, in, why);
    case Mode::metalfx_spatial: return impl_->encode_spatial(cb, in, why);
    case Mode::metalfx_temporal:
        if (!in.depth || !in.motion) {
            why = "no depth or motion texture";
            return false;
        }
        return impl_->encode_temporal(cb, in, why);
    case Mode::fsr4: break;
    }
    why = "fsr4 is not a host upscaler";
    return false;
}

std::uint64_t MetalUpscaler::memory_bytes() const {
    std::uint64_t n = 0;
    if (impl_->depth_tmp) n += impl_->depth_tmp.allocatedSize;
    if (impl_->out_tmp) n += impl_->out_tmp.allocatedSize;
    return n;
}

std::string MetalUpscaler::describe() const { return impl_->key; }

id<MTLTexture> MetalUpscaler::encode_readback(id<MTLCommandBuffer> cb, id<MTLTexture> src, std::uint32_t w, std::uint32_t h) {
    static std::mutex mu;
    static id<MTLComputePipelineState> ps = nil;
    id<MTLDevice> dev = cb.device;
    {
        std::lock_guard lock(mu);
        if (!ps) {
            NSError* e = nil;
            id<MTLLibrary> lib = [dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
                                                            "kernel void rb(texture2d<float, access::read> a [[texture(0)]],"
                                                            " texture2d<float, access::write> b [[texture(1)]], uint2 p [[thread_position_in_grid]]) {"
                                                            " if (p.x < b.get_width() && p.y < b.get_height()) b.write(a.read(p), p); }"
                                                   options:nil error:&e];
            if (lib) ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rb"] error:&e];
            if (!ps) return nil;
        }
    }
    if (is_depth_format(src.pixelFormat)) return nil;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                 width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> dst = [dev newTextureWithDescriptor:d];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    enc.label = @"gta6rt upscale readback";
    [enc setComputePipelineState:ps];
    [enc setTexture:src atIndex:0];
    [enc setTexture:dst atIndex:1];
    [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [enc endEncoding];
    return dst;
}

bool MetalUpscaler::write_png(const std::string& path, id<MTLTexture> t) {
    if (!t) return false;
    const std::uint32_t w = std::uint32_t(t.width), h = std::uint32_t(t.height);
    std::vector<float> px(std::size_t(w) * h * 4);
    [t getBytes:px.data() bytesPerRow:w * 16 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    std::vector<std::uint8_t> rgba(std::size_t(w) * h * 4);
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float x = std::max(px[i * 4 + c], 0.0f);
            const float g = std::pow(std::clamp(x / (1.0f + x), 0.0f, 1.0f), 1.0f / 2.2f);
            rgba[i * 4 + c] = std::uint8_t(std::lround(g * 255.0f));
        }
        rgba[i * 4 + 3] = 255;
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, rgba.data(), rgba.size(), nullptr);
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs, CGBitmapInfo(kCGImageAlphaNoneSkipLast), dp, nullptr, false,
                                   kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(), CFIndex(path.size()), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dst) {
        CGImageDestinationAddImage(dst, img, nullptr);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CFRelease(url);
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    return ok;
}

}  // namespace gta6rt::upscale
