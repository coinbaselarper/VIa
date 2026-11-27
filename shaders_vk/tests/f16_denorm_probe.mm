// What this GPU does with f16 denormals in the MSL that MoltenVK makes from the translated SPIR-V: every positive and
// negative f16 subnormal through half->float, half arithmetic, float->half and packed-byte reinterpretation, compiled
// with Metal's fast math on and off (MoltenVK: off when a module declares SignedZeroInfNanPreserve).
//   clang++ -std=c++20 -fobjc-arc f16_denorm_probe.mm -framework Metal -framework Foundation -o f16_denorm_probe
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static const char* kSource = R"(
#include <metal_stdlib>
using namespace metal;
kernel void probe(device const ushort* in [[buffer(0)]], device uint* out [[buffer(1)]],
                  uint i [[thread_position_in_grid]]) {
    half h = as_type<half>(in[i]);
    half one = as_type<half>(in[2048]);
    float f = float(h);
    out[i * 5 + 0] = as_type<uint>(f);
    out[i * 5 + 1] = as_type<ushort>(half(h * one));
    out[i * 5 + 2] = as_type<ushort>(half(f * as_type<float>(0x3f800000u + (in[2049] & 0u))));
    out[i * 5 + 3] = as_type<ushort>(half(h + h));
    out[i * 5 + 4] = as_type<ushort>(half(fma(h, one, half(0.0h))));
}
)";

static float half_to_float(uint16_t h) {
    const int e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(1024 + m), e - 25);
    return (h & 0x8000) ? -v : v;
}

static uint16_t float_to_half_rte(float f) {
    const uint16_t sign = std::signbit(f) ? 0x8000 : 0;
    const float a = std::fabs(f);
    uint16_t best = 0;
    float err = INFINITY;
    for (uint16_t h = 0; h < 0x7c00; ++h) {
        const float d = std::fabs(half_to_float(h) - a);
        if (d < err || (d == err && !(h & 1))) { err = d; best = h; }
    }
    return sign | best;
}

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::fprintf(stderr, "no Metal device\n"); return 2; }
        std::vector<uint16_t> input(2050);
        for (int i = 0; i < 1024; ++i) input[i] = uint16_t(i);             // +0 and the 1023 positive subnormals
        for (int i = 0; i < 1024; ++i) input[1024 + i] = uint16_t(0x8000 | i);
        input[2048] = 0x3c00;  // 1.0, read at run time so nothing folds
        input[2049] = 0;
        std::printf("device %s\n", device.name.UTF8String);
        for (int fast = 0; fast < 2; ++fast) {
            MTLCompileOptions* options = [MTLCompileOptions new];
            options.mathMode = fast ? MTLMathModeFast : MTLMathModeSafe;
            NSError* error = nil;
            id<MTLLibrary> lib = [device newLibraryWithSource:@(kSource) options:options error:&error];
            if (!lib) { std::fprintf(stderr, "compile: %s\n", error.description.UTF8String); return 1; }
            id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"probe"]
                                                                                    error:&error];
            id<MTLBuffer> in = [device newBufferWithBytes:input.data() length:input.size() * 2 options:MTLResourceStorageModeShared];
            id<MTLBuffer> out = [device newBufferWithLength:2048 * 5 * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandQueue> queue = [device newCommandQueue];
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:in offset:0 atIndex:0];
            [enc setBuffer:out offset:0 atIndex:1];
            [enc dispatchThreads:MTLSizeMake(2048, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const uint32_t* r = static_cast<const uint32_t*>(out.contents);
            int bad[5] = {}, sub = 0;
            int first[5] = {-1, -1, -1, -1, -1};
            for (int i = 0; i < 2048; ++i) {
                const uint16_t h = input[i];
                if ((h & 0x7fff) == 0) continue;
                ++sub;
                const float exact = half_to_float(h);
                uint32_t fbits;
                std::memcpy(&fbits, &exact, 4);
                const uint16_t twice = float_to_half_rte(exact * 2);
                const uint32_t expect[5] = {fbits, h, h, twice, h};
                for (int k = 0; k < 5; ++k) {
                    const uint32_t got = k == 0 ? r[i * 5] : (r[i * 5 + k] & 0xffff);
                    if (got != expect[k]) {
                        ++bad[k];
                        if (first[k] < 0) first[k] = i;
                    }
                }
            }
            const char* names[5] = {"half->float", "half*1.0", "float->half", "half+half", "fma(half)"};
            std::printf("math mode %s: %d subnormal inputs\n", fast ? "fast" : "safe", sub);
            for (int k = 0; k < 5; ++k) {
                std::printf("  %-11s %4d of %d wrong", names[k], bad[k], sub);
                if (first[k] >= 0) {
                    const int i = first[k];
                    std::printf(" (first: 0x%04x -> 0x%0*x)", input[i], k ? 4 : 8, k ? (r[i * 5 + k] & 0xffff) : r[i * 5]);
                }
                std::printf("\n");
            }
        }
    }
    return 0;
}
