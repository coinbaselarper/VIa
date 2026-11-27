// GPU cost of compute encoder boundaries on this device: N one-group dispatches of a trivial kernel encoded as
// (a) one concurrent encoder, (b) one concurrent encoder with a buffer+texture memory barrier between dispatches,
// (c) one encoder per dispatch, each waiting for the previous encoder's fence (the adapter's join chain).
//   cspeed_encprobe [N]   prints the command buffer GPU time of each, best of 7.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <algorithm>
#include <cstdlib>

static const char* kSrc =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void k(device uint* b [[buffer(0)]], uint i [[thread_position_in_grid]]) { b[i] = b[i] * 3u + 1u; }\n";

int main(int argc, char** argv) {
    @autoreleasepool {
        const int n = argc > 1 ? std::atoi(argv[1]) : 200;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError* e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSrc) options:nil error:&e];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&e];
        id<MTLBuffer> buf = [dev newBufferWithLength:64 * 4 options:MTLResourceStorageModePrivate];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLFence> fences[2] = {[dev newFence], [dev newFence]};
        const char* names[3] = {"one encoder", "one encoder + barrier each", "encoder each + fence chain"};
        for (int mode = 0; mode < 3; ++mode) {
            double best = 1e9;
            for (int rep = 0; rep < 7; ++rep) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = nil;
                for (int i = 0; i < n; ++i) {
                    if (!ce || mode == 2) {
                        if (ce) {
                            [ce updateFence:fences[i & 1]];
                            [ce endEncoding];
                        }
                        ce = [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
                        if (mode == 2 && i) [ce waitForFence:fences[(i - 1) & 1]];
                        [ce setComputePipelineState:ps];
                        [ce setBuffer:buf offset:0 atIndex:0];
                    }
                    if (mode == 1 && i) [ce memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
                    [ce dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                }
                [ce endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                if (cb.error) {
                    std::printf("command buffer error: %s\n", cb.error.localizedDescription.UTF8String);
                    return 1;
                }
                best = std::min(best, (cb.GPUEndTime - cb.GPUStartTime) * 1000.0);
            }
            std::printf("%-28s N=%d: %.3f ms (%.2f us a dispatch)\n", names[mode], n, best, best * 1000.0 / n);
        }
    }
    return 0;
}
