#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const char *kSrc = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float psize [[point_size]]; };
static inline void body(uint vid, uint iid, device uint4 *out, constant uint &stride, device atomic_uint *calls)
{
    uint lane = simd_prefix_exclusive_sum(1u);
    uint n = simd_sum(1u);
    simd_vote::vote_t b = (simd_vote::vote_t)simd_ballot(true);
    uint lo = uint(b), want = n >= 32u ? 0xFFFFFFFFu : ((1u << n) - 1u);
    uint self = simd_shuffle(vid, ushort(lane));
    uint first = simd_broadcast_first(lane);
    uint slot = iid * stride + vid;
    atomic_fetch_add_explicit(&calls[0], 1u, memory_order_relaxed);
    uint flags = (lo == want ? 1u : 0u) | (self == vid ? 2u : 0u) | (first == 0u ? 4u : 0u) | (lane < n ? 8u : 0u);
    out[slot] = uint4(lane, n, flags, 1u);
}
vertex VOut lane_vs(uint vid [[vertex_id]], uint iid [[instance_id]], device uint4 *out [[buffer(0)]],
                    constant uint &stride [[buffer(1)]], device atomic_uint *calls [[buffer(2)]])
{
    body(vid, iid, out, stride, calls);
    VOut o;
    o.pos = float4(float(vid % 64u) / 32.0 - 1.0, float((vid / 64u) % 64u) / 32.0 - 1.0, 0.5, 1.0);
    o.psize = 1.0;
    return o;
}
vertex void lane_vs_noraster(uint vid [[vertex_id]], uint iid [[instance_id]], device uint4 *out [[buffer(0)]],
                             constant uint &stride [[buffer(1)]], device atomic_uint *calls [[buffer(2)]])
{
    body(vid, iid, out, stride, calls);
}
fragment float4 fs() { return float4(1); }
)";

struct Case {
    const char *name;
    MTLPrimitiveType prim;
    uint32_t verts, instances;
    int index_mode;
    bool raster;
};

int main() {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("vs_lane_identity: no Metal device (SKIP)\n"); return 77; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        MTLCompileOptions *co = [MTLCompileOptions new];
        co.languageVersion = MTLLanguageVersion3_1;
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSrc) options:co error:&e];
        if (!lib) { printf("compile: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLRenderPipelineState> pso[2];
        for (int r = 0; r < 2; r++) {
            MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lib newFunctionWithName:r ? @"lane_vs" : @"lane_vs_noraster"];
            rd.fragmentFunction = r ? [lib newFunctionWithName:@"fs"] : nil;
            rd.rasterizationEnabled = r ? YES : NO;
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            rd.inputPrimitiveTopology = MTLPrimitiveTopologyClassUnspecified;
            pso[r] = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
            if (!pso[r]) { printf("pso: %s\n", e.localizedDescription.UTF8String); return 1; }
        }
        MTLTextureDescriptor *rtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:256 height:256 mipmapped:NO];
        rtd.usage = MTLTextureUsageRenderTarget;
        rtd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:rtd];
        const Case cases[] = {
            {"tri_3000", MTLPrimitiveTypeTriangle, 3000, 1, 0, true},
            {"tri_3001_partial", MTLPrimitiveTypeTriangle, 3001, 1, 0, true},
            {"tri_37", MTLPrimitiveTypeTriangle, 37, 1, 0, true},
            {"tri_3000_noraster", MTLPrimitiveTypeTriangle, 3000, 1, 0, false},
            {"strip_4099", MTLPrimitiveTypeTriangleStrip, 4099, 1, 0, true},
            {"points_5000", MTLPrimitiveTypePoint, 5000, 1, 0, true},
            {"lines_998", MTLPrimitiveTypeLine, 998, 1, 0, true},
            {"tri_96_x_70_instances", MTLPrimitiveTypeTriangle, 96, 70, 0, true},
            {"tri_33_x_9_instances", MTLPrimitiveTypeTriangle, 33, 9, 0, true},
            {"indexed_shuffled_3000", MTLPrimitiveTypeTriangle, 3000, 1, 1, true},
            {"indexed_reuse_6000_of_1000", MTLPrimitiveTypeTriangle, 6000, 1, 2, true},
            {"indexed_sparse_3000", MTLPrimitiveTypeTriangle, 3000, 1, 3, true},
        };
        int bad_cases = 0, passed = 0;
        for (const Case &c : cases) {
            const uint32_t stride = 8192;
            id<MTLBuffer> out = [dev newBufferWithLength:16ull * stride * c.instances options:MTLResourceStorageModeShared];
            memset(out.contents, 0, out.length);
            id<MTLBuffer> calls = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
            memset(calls.contents, 0, 4);
            std::vector<uint32_t> idx;
            if (c.index_mode == 1) {
                for (uint32_t i = 0; i < c.verts; i++) idx.push_back(i);
                uint32_t s = 12345;
                for (uint32_t i = c.verts - 1; i > 0; i--) { s = s * 1664525u + 1013904223u; std::swap(idx[i], idx[s % (i + 1)]); }
            } else if (c.index_mode == 2) {
                uint32_t s = 777;
                for (uint32_t i = 0; i < c.verts; i++) { s = s * 1664525u + 1013904223u; idx.push_back((s >> 8) % 1000u); }
            } else if (c.index_mode == 3) {
                for (uint32_t i = 0; i < c.verts; i++) idx.push_back((i * 7u) % 8191u);
            }
            id<MTLBuffer> ib = idx.empty() ? nil : [dev newBufferWithBytes:idx.data() length:4 * idx.size() options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = rt;
            p.colorAttachments[0].loadAction = MTLLoadActionClear;
            p.colorAttachments[0].storeAction = MTLStoreActionDontCare;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
            [re setRenderPipelineState:pso[c.raster ? 1 : 0]];
            [re setVertexBuffer:out offset:0 atIndex:0];
            [re setVertexBytes:&stride length:4 atIndex:1];
            [re setVertexBuffer:calls offset:0 atIndex:2];
            if (ib)
                [re drawIndexedPrimitives:c.prim indexCount:c.verts indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0 instanceCount:c.instances];
            else
                [re drawPrimitives:c.prim vertexStart:0 vertexCount:c.verts instanceCount:c.instances];
            [re endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) { printf("case %s GPU_ERROR\n", c.name); bad_cases++; continue; }
            const uint32_t *o = (const uint32_t *)out.contents;
            uint64_t inv = 0, ballot_bad = 0, shuffle_bad = 0, first_bad = 0, range_bad = 0, partial = 0;
            uint32_t nmin = 99, nmax = 0;
            for (uint32_t inst = 0; inst < c.instances; inst++)
                for (uint32_t v = 0; v < stride; v++) {
                    const uint32_t *r = o + 4ull * (inst * stride + v);
                    if (r[3] != 1u) continue;
                    inv++;
                    uint32_t n = r[1], f = r[2];
                    nmin = std::min(nmin, n); nmax = std::max(nmax, n);
                    if (n < 32) partial++;
                    if (!(f & 1)) ballot_bad++;
                    if (!(f & 2)) shuffle_bad++;
                    if (!(f & 4)) first_bad++;
                    if (!(f & 8)) range_bad++;
                }
            uint32_t ncalls = *(uint32_t *)calls.contents;
            bool ok = inv && !ballot_bad && !shuffle_bad && !first_bad && !range_bad;
            if (!ok) bad_cases++; else passed++;
            printf("case %-28s %s distinct_vertices %llu invocations %u active_lanes %u..%u invocations_in_partial_groups %llu "
                   "noncontiguous_ballot %llu shuffle_mismatch %llu first_not_0 %llu lane_ge_n %llu\n",
                   c.name, ok ? "OK" : "FAIL", inv, ncalls, nmin, nmax, partial, ballot_bad, shuffle_bad, first_bad, range_bad);
        }
        const int ncases = (int)(sizeof(cases) / sizeof(cases[0]));
        printf("vs_lane_identity: %d cases, %d passing, %d failing (lane = simd_prefix_exclusive_sum(1) is the SIMD lane index "
               "iff the active lanes of every vertex SIMD group are 0..n-1); Metal API validation %s, shader validation %s\n",
               ncases, passed, bad_cases, getenv("MTL_DEBUG_LAYER") ? "on" : "off", getenv("MTL_SHADER_VALIDATION") ? "on" : "off");
        return (ncases == 12 && passed == 12 && !bad_cases) ? 0 : 1;
    }
}
