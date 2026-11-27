// Offline proof of the upscale stage's conventions and cost: a synthetic moving-camera sequence rendered the way the
// title feeds FidelityFX (1280x720 HDR linear colour, inverted depth, render-resolution motion vectors in pixels,
// previous minus current; Halton jitter from the stage's own ffxQuery answer, projection offset +j, dispatch jitter -j),
// upscaled to 1920x1080 by every mode and every sign mapping onto MetalFX, against a 4x4-supersampled truth.
//
// usage: upscale_offline [--frames N] [--out DIR] [--mode all|...]

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>

#include "../src/ffx_types.h"
#include "../src/metal_upscaler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace gta6rt::upscale;

namespace {

const char* kScene = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Frame { float2 cam; float2 cam_prev; float2 disk; float2 disk_prev; float2 jitter; float radius; float scale; uint2 size; };
float3 background(float2 s) {
    float2 c = floor(s / 3.0);
    float check = fmod(abs(c.x + c.y), 2.0);
    float rings = 0.5 + 0.5 * sin(length(s - float2(400.0, 300.0)) * 0.9);
    float3 col = mix(float3(0.08, 0.10, 0.14), float3(0.75, 0.62, 0.40), check) * (0.6 + 0.4 * rings);
    float2 g = fract(s / 37.0);
    if (g.x < 0.06 || g.y < 0.06) col = float3(3.5, 3.2, 2.6);
    return col;
}
float3 diskcol(float2 l) {
    float a = atan2(l.y, l.x);
    float spokes = step(0.0, sin(a * 12.0));
    return mix(float3(0.9, 0.15, 0.1), float3(0.1, 0.7, 0.95), spokes) * (1.0 + 2.0 * step(length(l), 6.0));
}
float3 shade(constant Frame& f, float2 screen, thread float& depth, thread bool& on_disk) {
    float2 l = screen - f.disk;
    on_disk = length(l) < f.radius;
    depth = on_disk ? 0.7 : 0.1;
    return on_disk ? diskcol(l) : background(screen + f.cam);
}
kernel void render(constant Frame& f [[buffer(0)]], texture2d<float, access::write> color [[texture(0)]],
                   texture2d<float, access::write> depth [[texture(1)]], texture2d<float, access::write> motion [[texture(2)]],
                   uint2 p [[thread_position_in_grid]]) {
    if (p.x >= f.size.x || p.y >= f.size.y) return;
    float2 screen = float2(p) + 0.5 - f.jitter;
    float d; bool on;
    float3 c = shade(f, screen, d, on);
    color.write(float4(c, 1.0), p);
    depth.write(float4(d), p);
    float2 unjittered = float2(p) + 0.5;
    float2 mv = on ? -(f.disk - f.disk_prev) : (f.cam - f.cam_prev);
    (void)unjittered;
    motion.write(float4(mv, 0, 0), p);
}
kernel void truth(constant Frame& f [[buffer(0)]], texture2d<float, access::write> out [[texture(0)]],
                  uint2 q [[thread_position_in_grid]]) {
    if (q.x >= f.size.x || q.y >= f.size.y) return;
    float3 acc = 0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            float2 s = (float2(q) + (float2(i, j) + 0.5) / 4.0) / f.scale;
            float d; bool on;
            acc += shade(f, s, d, on);
        }
    out.write(float4(acc / 16.0, 1.0), q);
}
)MSL";

struct FrameParams {
    float cam[2], cam_prev[2], disk[2], disk_prev[2], jitter[2];
    float radius, scale;
    std::uint32_t size[2];
};

id<MTLTexture> make(id<MTLDevice> dev, MTLPixelFormat f, std::uint32_t w, std::uint32_t h, MTLTextureUsage u) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = u;
    d.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:d];
}

std::vector<float> readback(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> t) {
    const std::uint32_t w = std::uint32_t(t.width), h = std::uint32_t(t.height);
    id<MTLTexture> s = nil;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                 width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    s = [dev newTextureWithDescriptor:d];
    static id<MTLComputePipelineState> conv = nil;
    if (!conv) {
        NSError* e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
                                                        "kernel void cv(texture2d<float, access::read> a [[texture(0)]],"
                                                        " texture2d<float, access::write> b [[texture(1)]], uint2 p [[thread_position_in_grid]]) {"
                                                        " if (p.x < b.get_width() && p.y < b.get_height()) b.write(a.read(p), p); }"
                                               options:nil error:&e];
        conv = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cv"] error:&e];
    }
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:conv];
    [enc setTexture:t atIndex:0];
    [enc setTexture:s atIndex:1];
    [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    std::vector<float> px(std::size_t(w) * h * 4);
    [s getBytes:px.data() bytesPerRow:w * 16 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    return px;
}

float tonemap(float x) { return x / (1.0f + std::max(x, 0.0f)); }

double psnr(const std::vector<float>& a, const std::vector<float>& b, std::size_t margin_w = 0, std::uint32_t w = 0,
            std::uint32_t h = 0) {
    double se = 0;
    std::size_t n = 0;
    for (std::uint32_t y = std::uint32_t(margin_w); y + margin_w < h; ++y)
        for (std::uint32_t x = std::uint32_t(margin_w); x + margin_w < w; ++x)
            for (int c = 0; c < 3; ++c) {
                const std::size_t i = (std::size_t(y) * w + x) * 4 + c;
                const double d = tonemap(a[i]) - tonemap(b[i]);
                se += d * d;
                ++n;
            }
    const double mse = se / double(n ? n : 1);
    return mse > 0 ? 10.0 * std::log10(1.0 / mse) : 99.0;
}

bool write_png(const std::string& path, const std::vector<float>& px, std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> rgba(std::size_t(w) * h * 4);
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float t = tonemap(px[i * 4 + c]);
            const float g = std::pow(std::clamp(t, 0.0f, 1.0f), 1.0f / 2.2f);
            rgba[i * 4 + c] = std::uint8_t(std::lround(g * 255.0f));
        }
        rgba[i * 4 + 3] = 255;
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, rgba.data(), rgba.size(), nullptr);
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs, CGBitmapInfo(kCGImageAlphaNoneSkipLast), dp,
                                   nullptr, false, kCGRenderingIntentDefault);
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

struct Candidate {
    std::string name;
    Mode mode;
    Conventions conv;
    std::unique_ptr<MetalUpscaler> up;
    id<MTLTexture> out = nil;
    double psnr_sum = 0;
    int psnr_n = 0;
    std::vector<double> gpu_ms;
    bool failed = false;
    std::string why;
};

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        int frames = 64;
        std::string out_dir;
        std::string only = "all";
        float speed_x = 2.3f, speed_y = -1.1f;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
            else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
            else if (a == "--mode" && i + 1 < argc) only = argv[++i];
            else if (a == "--speed" && i + 2 < argc) {
                speed_x = std::strtof(argv[++i], nullptr);
                speed_y = std::strtof(argv[++i], nullptr);
            }
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError* err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kScene] options:nil error:&err];
        if (!lib) {
            std::fprintf(stderr, "scene: %s\n", err.localizedDescription.UTF8String);
            return 2;
        }
        id<MTLComputePipelineState> render = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"render"] error:&err];
        id<MTLComputePipelineState> truth = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"truth"] error:&err];
        const std::uint32_t rw = 1280, rh = 720, ow = 1920, oh = 1080;
        const MTLTextureUsage rd = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        id<MTLTexture> color = make(dev, MTLPixelFormatRGBA16Float, rw, rh, rd | MTLTextureUsageRenderTarget);
        id<MTLTexture> depth = make(dev, MTLPixelFormatR32Float, rw, rh, rd);
        id<MTLTexture> motion = make(dev, MTLPixelFormatRG16Float, rw, rh, rd);
        id<MTLTexture> gt = make(dev, MTLPixelFormatRGBA16Float, ow, oh, rd);

        std::vector<Candidate> cands;
        auto add = [&](const std::string& name, Mode m, Conventions c) {
            Candidate k;
            k.name = name;
            k.mode = m;
            k.conv = c;
            k.up = std::make_unique<MetalUpscaler>(dev, m);
            k.up->set_conventions(c);
            k.out = make(dev, MTLPixelFormatRGBA16Float, ow, oh, rd | MTLTextureUsageRenderTarget);
            cands.push_back(std::move(k));
        };
        if (only == "all" || only == "bilinear") add("bilinear", Mode::bilinear, {});
        if (only == "all" || only == "metalfx-spatial") add("metalfx-spatial", Mode::metalfx_spatial, {});
        if (only == "all" || only == "metalfx-temporal") {
            for (float jx : {1.0f, -1.0f})
                for (float jy : {1.0f, -1.0f})
                    for (int mv = 0; mv < 3; ++mv) {
                        Conventions c;
                        c.jitter_sign_x = jx;
                        c.jitter_sign_y = jy;
                        c.motion_sign_x = mv == 1 ? -1.0f : 1.0f;
                        c.motion_sign_y = mv == 0 ? 1.0f : -1.0f;
                        char n[96];
                        std::snprintf(n, sizeof n, "temporal j(%+.0f,%+.0f) mv(%+.0f,%+.0f)", jx, jy, c.motion_sign_x,
                                      c.motion_sign_y);
                        add(n, Mode::metalfx_temporal, c);
                    }
        }
        const std::int32_t phases = ffx::jitter_phase_count(rw, ow);
        std::printf("upscale_offline: %s, %ux%u -> %ux%u, %d frames, camera %.2f,%.2f px/frame, jitter phases %d\n",
                    dev.name.UTF8String, rw, rh, ow, oh, frames, speed_x, speed_y, phases);
        std::vector<float> last_input;
        for (int f = 0; f < frames; ++f) {
            FrameParams p{};
            float jx, jy;
            ffx::jitter_offset(f, phases, jx, jy);
            auto cam = [&](int t, float* o) {
                o[0] = speed_x * t;
                o[1] = speed_y * t;
            };
            auto disk = [&](int t, float* o) {
                o[0] = 640.0f + 220.0f * std::cos(t * 0.05f);
                o[1] = 360.0f + 140.0f * std::sin(t * 0.07f);
            };
            cam(f, p.cam);
            cam(f - 1, p.cam_prev);
            disk(f, p.disk);
            disk(f - 1, p.disk_prev);
            p.jitter[0] = jx;
            p.jitter[1] = jy;
            p.radius = 90.0f;
            p.scale = float(ow) / float(rw);
            p.size[0] = rw;
            p.size[1] = rh;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:render];
            [enc setBytes:&p length:sizeof p atIndex:0];
            [enc setTexture:color atIndex:0];
            [enc setTexture:depth atIndex:1];
            [enc setTexture:motion atIndex:2];
            [enc dispatchThreads:MTLSizeMake(rw, rh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            FrameParams pt = p;
            pt.size[0] = ow;
            pt.size[1] = oh;
            [enc setComputePipelineState:truth];
            [enc setBytes:&pt length:sizeof pt atIndex:0];
            [enc setTexture:gt atIndex:0];
            [enc dispatchThreads:MTLSizeMake(ow, oh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            [enc endEncoding];
            [cb commit];
            MetalInputs in;
            in.color = color;
            in.depth = depth;
            in.motion = motion;
            in.render_w = rw;
            in.render_h = rh;
            in.output_w = ow;
            in.output_h = oh;
            in.jitter_x = -jx;
            in.jitter_y = -jy;
            in.reset = f == 0;
            for (Candidate& k : cands) {
                if (k.failed) continue;
                in.output = k.out;
                id<MTLCommandBuffer> ucb = [q commandBuffer];
                if (!k.up->encode(ucb, in, k.why)) {
                    k.failed = true;
                    continue;
                }
                [ucb commit];
                [ucb waitUntilCompleted];
                if (ucb.error) {
                    k.failed = true;
                    k.why = ucb.error.localizedDescription.UTF8String;
                    continue;
                }
                if (f >= 8) k.gpu_ms.push_back((ucb.GPUEndTime - ucb.GPUStartTime) * 1000.0);
            }
            if (f >= frames - 16 || f == frames - 1) {
                const std::vector<float> t = readback(dev, q, gt);
                for (Candidate& k : cands) {
                    if (k.failed) continue;
                    const std::vector<float> o = readback(dev, q, k.out);
                    k.psnr_sum += psnr(o, t, 8, ow, oh);
                    ++k.psnr_n;
                    if (f == frames - 1 && !out_dir.empty()) {
                        std::string n = k.name;
                        for (char& ch : n)
                            ch = ch == '+' ? 'p' : ch == '-' ? 'm' : std::isalnum((unsigned char)ch) ? ch : '_';
                        if (k.mode != Mode::metalfx_temporal || k.name == "temporal j(+1,+1) mv(+1,+1)" ||
                            k.name == "temporal j(-1,-1) mv(+1,+1)")
                            write_png(out_dir + "/offline-" + n + ".png", o, ow, oh);
                    }
                }
                if (f == frames - 1 && !out_dir.empty()) {
                    write_png(out_dir + "/offline-truth.png", t, ow, oh);
                    write_png(out_dir + "/offline-input.png", readback(dev, q, color), rw, rh);
                }
            }
        }
        std::printf("%-34s %10s %12s %12s\n", "upscaler", "PSNR dB", "GPU ms p50", "GPU ms max");
        for (Candidate& k : cands) {
            if (k.failed) {
                std::printf("%-34s FAILED: %s\n", k.name.c_str(), k.why.c_str());
                continue;
            }
            std::vector<double> g = k.gpu_ms;
            std::sort(g.begin(), g.end());
            std::printf("%-34s %10.2f %12.3f %12.3f\n", k.name.c_str(), k.psnr_sum / std::max(1, k.psnr_n),
                        g.empty() ? 0.0 : g[g.size() / 2], g.empty() ? 0.0 : g.back());
        }
        for (Candidate& k : cands)
            if (!k.failed && !k.up->describe().empty()) std::printf("scaler %s: %s, scratch %llu bytes\n", k.name.c_str(),
                                                                     k.up->describe().c_str(),
                                                                     (unsigned long long)k.up->memory_bytes());
    }
    return 0;
}
