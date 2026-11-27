// Checks on the GPU what --vs-lane-from-ballot computes in a Metal vertex function (no thread_index_in_simdgroup
// there): the lane picked from the entry ballot by the invocation's rank among active lanes. For every vertex the
// derived lane must be the index simd_shuffle reads it at: shuffle(vid, lane) == vid, and for every active lane k,
// shuffle(lane, k) == k. Vertex counts that leave the last simdgroup partial are included.
//   clang++ -std=c++20 -fobjc-arc vs_lane_probe.mm -framework Metal -framework Foundation -o vs_lane_probe
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static const char* kSource = R"(
#include <metal_stdlib>
using namespace metal;
static uint nth_bit(uint w, uint r) {
    uint pos = 0;
    for (uint step = 16; step >= 1; step >>= 1) {
        uint c = popcount(w & ((1u << step) - 1u));
        bool up = r >= c;
        r = up ? r - c : r;
        w = up ? w >> step : w;
        pos = up ? pos + step : pos;
    }
    return pos;
}
vertex void probe(uint vid [[vertex_id]], device uint4* out [[buffer(0)]]) {
    uint2 ballot = as_type<uint2>((simd_vote::vote_t)simd_ballot(true));
    uint rank = simd_prefix_exclusive_sum(1u);
    uint n0 = popcount(ballot.x);
    bool hi = rank >= n0;
    uint lane = hi ? 32u + nth_bit(ballot.y, rank - n0) : nth_bit(ballot.x, rank);
    uint ok = simd_shuffle(vid, ushort(lane)) == vid ? 1u : 0u;
    uint bad = 0;
    for (uint k = 0; k < 32; ++k) {
        uint got = simd_shuffle(lane, ushort(k));
        if ((ballot.x >> k) & 1u) bad += got != k ? 1u : 0u;
    }
    out[vid] = uint4(lane, ok, bad, popcount(ballot.x) + popcount(ballot.y));
}
)";

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSError* error = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@(kSource) options:[MTLCompileOptions new] error:&error];
        if (!lib) { std::fprintf(stderr, "compile: %s\n", error.description.UTF8String); return 1; }
        MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction = [lib newFunctionWithName:@"probe"];
        desc.rasterizationEnabled = NO;
        id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!pso) { std::fprintf(stderr, "pipeline: %s\n", error.description.UTF8String); return 1; }
        std::printf("device %s\n", device.name.UTF8String);
        int failures = 0;
        for (uint32_t count : {1u, 3u, 31u, 32u, 33u, 100u, 1000u, 65537u}) {
            id<MTLBuffer> out = [device newBufferWithLength:count * 16 options:MTLResourceStorageModeShared];
            std::memset(out.contents, 0xff, count * 16);
            id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.renderTargetWidth = 1;
            pass.renderTargetHeight = 1;
            pass.defaultRasterSampleCount = 1;
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
            [enc setRenderPipelineState:pso];
            [enc setVertexBuffer:out offset:0 atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:count];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const uint32_t* r = static_cast<const uint32_t*>(out.contents);
            uint32_t not_self = 0, inconsistent = 0, max_lane = 0, partial = 0, unwritten = 0;
            for (uint32_t v = 0; v < count; ++v) {
                const uint32_t* e = r + v * 4;
                if (e[0] == 0xffffffffu) { ++unwritten; continue; }
                not_self += e[1] != 1;
                inconsistent += e[2] != 0;
                if (e[0] > max_lane) max_lane = e[0];
                partial += e[3] < 32;
            }
            const bool ok = !not_self && !inconsistent && !unwritten;
            failures += !ok;
            std::printf("vertices %6u: shuffle(vid, lane) != vid %u, shuffle(lane, k) != k %u, unwritten %u, "
                        "largest lane %u, vertices in partial simdgroups %u: %s\n",
                        count, not_self, inconsistent, unwritten, max_lane, partial, ok ? "ok" : "FAIL");
        }
        return failures ? 1 : 0;
    }
}
