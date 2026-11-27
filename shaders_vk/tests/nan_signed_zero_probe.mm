// NaN, infinity and -0 through Metal's safe and fast math (MoltenVK compiles a module with fast math unless it declares
// SignedZeroInfNanPreserve; --no-float-controls drops that mode, --apple-float-controls keeps it).
//   clang++ -std=c++20 -fobjc-arc nan_signed_zero_probe.mm -framework Metal -framework Foundation -o nan_signed_zero_probe
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <cstdio>

static const char* kSource = R"(
#include <metal_stdlib>
using namespace metal;
kernel void probe(device const float* in [[buffer(0)]], device uint* out [[buffer(1)]]) {
    float nan_ = in[0], one = in[1], zero = in[2], inf_ = in[3], neg = in[4];
    out[0] = isnan(nan_) ? 1u : 0u;
    out[1] = (nan_ != nan_) ? 1u : 0u;
    out[2] = isinf(inf_) ? 1u : 0u;
    out[3] = as_type<uint>(neg * zero);
    out[4] = as_type<uint>(fmax(nan_, one));
    out[5] = (nan_ < one || nan_ >= one) ? 1u : 0u;
    out[6] = as_type<uint>(zero / zero);
    out[7] = as_type<uint>(one / zero);
    out[8] = as_type<uint>(-zero + zero);
    out[9] = as_type<uint>(select(one, zero, nan_ == nan_));
}
)";

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const float input[5] = {__builtin_nanf(""), 1.0f, 0.0f, __builtin_inff(), -1.0f};
        const char* names[10] = {"isnan(NaN)", "NaN != NaN", "isinf(inf)", "bits(-1*0)", "bits(fmax(NaN,1))",
                                 "NaN<1 || NaN>=1", "bits(0/0)", "bits(1/0)", "bits(-0+0)", "NaN==NaN ? 0 : 1"};
        const uint32_t expect[10] = {1, 1, 1, 0x80000000u, 0x3f800000u, 0, 0x7fc00000u, 0x7f800000u, 0, 0x3f800000u};
        std::printf("device %s\n", device.name.UTF8String);
        for (int fast = 0; fast < 2; ++fast) {
            MTLCompileOptions* options = [MTLCompileOptions new];
            options.mathMode = fast ? MTLMathModeFast : MTLMathModeSafe;
            NSError* error = nil;
            id<MTLLibrary> lib = [device newLibraryWithSource:@(kSource) options:options error:&error];
            if (!lib) { std::fprintf(stderr, "compile: %s\n", error.description.UTF8String); return 1; }
            id<MTLComputePipelineState> pso =
                [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"probe"] error:&error];
            id<MTLBuffer> in = [device newBufferWithBytes:input length:sizeof(input) options:MTLResourceStorageModeShared];
            id<MTLBuffer> out = [device newBufferWithLength:40 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:in offset:0 atIndex:0];
            [enc setBuffer:out offset:0 atIndex:1];
            [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const uint32_t* r = static_cast<const uint32_t*>(out.contents);
            int wrong = 0;
            std::printf("math mode %s:\n", fast ? "fast" : "safe");
            for (int k = 0; k < 10; ++k) {
                const bool nan_ok = (k == 6) && ((r[k] & 0x7f800000u) == 0x7f800000u) && (r[k] & 0x7fffffu);
                const bool ok = r[k] == expect[k] || nan_ok;
                wrong += !ok;
                std::printf("  %-20s 0x%08x %s\n", names[k], r[k], ok ? "ok" : "WRONG");
            }
            std::printf("  %d of 10 wrong\n", wrong);
        }
    }
    return 0;
}
