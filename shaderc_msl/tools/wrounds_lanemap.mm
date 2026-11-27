// Lane-map probe: in a mesh function and a compute kernel, is thread i of a threadgroup lane i % 32 of SIMD group
// i / 32 (the mapping the fork's --half-shuffle assumes: console lane = 32 x SIMD group + SIMD lane), and does a
// simd_shuffle inside one emulated wave read the lane it names, for every source pattern of the in-half ops
// (DPP rows/DPP8/quads, ds_swizzle bit masks, ds_bpermute) with random EXEC patterns?
//   wrounds_lanemap [--groups N]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const char* kSrc = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; };
using M = metal::mesh<V, void, 3, 1, topology::triangle>;

static uint pat_src(uint pat, uint lane)
{
    uint sel = pat & 0xffu, kind = pat >> 8;
    uint h = lane & 32u, l = lane & 31u;
    if (kind == 0u) return h | (l & ~15u) | ((l + sel) & 15u);          // row rotate
    if (kind == 1u) return h | (l & ~7u) | ((l ^ sel) & 7u);            // DPP8-like
    if (kind == 2u) return h | (l & ~3u) | ((sel >> ((l & 3u) * 2u)) & 3u); // quad perm
    if (kind == 3u) return h | ((((l & (sel & 31u)) | ((sel >> 3) & 31u)) ^ ((sel * 7u) & 31u)) & 31u); // bitmask
    return h | ((l * 13u + sel) & 31u);                                  // bpermute-like
}

static void body(uint li, uint sg, uint ln, uint gid, device uint* out, device atomic_uint* bad, uint stride)
{
    out[(gid * stride + li) * 3u + 0u] = li;
    out[(gid * stride + li) * 3u + 1u] = sg;
    out[(gid * stride + li) * 3u + 2u] = ln;
    if (li != sg * 32u + ln) atomic_fetch_add_explicit(&bad[0], 1u, memory_order_relaxed);
    uint wl = li & 63u;
    for (uint pat = 0u; pat < 5u * 256u; ++pat) {
        uint seed = gid * 2654435761u + pat * 40503u;
        uint v = (wl * 2246822519u) ^ seed;
        bool on = ((((wl + 1u) * 3266489917u) ^ seed) >> 7 & 3u) != 0u;
        uint s = pat_src(pat, wl);
        uint got = simd_shuffle(v, ushort(s & 31u));
        uint want = (s * 2246822519u) ^ seed;
        ulong bits = ulong(static_cast<simd_vote::vote_t>(simd_ballot(on)));
        bool src_on = ((bits >> (s & 31u)) & 1ul) != 0ul;
        bool want_on = ((((s + 1u) * 3266489917u) ^ seed) >> 7 & 3u) != 0u;
        if (got != want || src_on != want_on) atomic_fetch_add_explicit(&bad[1], 1u, memory_order_relaxed);
    }
}

[[mesh]] void lanemap_mesh(M m, device uint* out [[buffer(0)]], device atomic_uint* bad [[buffer(1)]],
                           uint li [[thread_index_in_threadgroup]], uint sg [[simdgroup_index_in_threadgroup]],
                           uint ln [[thread_index_in_simdgroup]], uint gid [[threadgroup_position_in_grid]])
{
    body(li, sg, ln, gid, out, bad, 128u);
    if (li == 0u) m.set_primitive_count(0);
}

kernel void lanemap_cs(device uint* out [[buffer(0)]], device atomic_uint* bad [[buffer(1)]],
                       uint li [[thread_index_in_threadgroup]], uint sg [[simdgroup_index_in_threadgroup]],
                       uint ln [[thread_index_in_simdgroup]], uint gid [[threadgroup_position_in_grid]])
{
    body(li, sg, ln, gid, out, bad, 256u);
}

fragment float4 lanemap_ps() { return float4(0); }
)MSL";

int main(int argc, char** argv) {
    @autoreleasepool {
        int groups = 4096;
        for (int i = 1; i + 1 < argc; i += 2)
            if (!std::strcmp(argv[i], "--groups")) groups = std::atoi(argv[i + 1]);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError* err = nil;
        MTLCompileOptions* o = [MTLCompileOptions new];
        o.languageVersion = MTLLanguageVersion3_1;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:o error:&err];
        if (!lib) { std::fprintf(stderr, "compile: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLBuffer> out = [dev newBufferWithLength:(size_t)groups * 256 * 12 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bad = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
        // compute, 256 threads (4 emulated waves)
        {
            std::memset(bad.contents, 0, 16);
            id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"lanemap_cs"] error:&err];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:p];
            [e setBuffer:out offset:0 atIndex:0];
            [e setBuffer:bad offset:0 atIndex:1];
            [e dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [e endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const uint32_t* b = (const uint32_t*)bad.contents;
            std::printf("COMPUTE\tgroups %d x 256\tlane-map errors %u\tshuffle/ballot errors %u of %llu\t%s\n", groups,
                        b[0], b[1], (unsigned long long)groups * 256 * 1280, cb.error ? cb.error.localizedDescription.UTF8String : "ok");
        }
        // mesh, 128 threads (2 emulated waves), rasterization off
        {
            std::memset(bad.contents, 0, 16);
            MTLMeshRenderPipelineDescriptor* d = [MTLMeshRenderPipelineDescriptor new];
            d.meshFunction = [lib newFunctionWithName:@"lanemap_mesh"];
            d.fragmentFunction = [lib newFunctionWithName:@"lanemap_ps"];
            d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            d.rasterizationEnabled = NO;
            id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithMeshDescriptor:d options:0 reflection:nil error:&err];
            if (!p) { std::fprintf(stderr, "mesh pipeline: %s\n", err.localizedDescription.UTF8String); return 1; }
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:p];
            [e setMeshBuffer:out offset:0 atIndex:0];
            [e setMeshBuffer:bad offset:0 atIndex:1];
            [e drawMeshThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                threadsPerMeshThreadgroup:MTLSizeMake(128, 1, 1)];
            [e endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const uint32_t* b = (const uint32_t*)bad.contents;
            std::printf("MESH\tgroups %d x 128\tlane-map errors %u\tshuffle/ballot errors %u of %llu\t%s\n", groups,
                        b[0], b[1], (unsigned long long)groups * 128 * 1280, cb.error ? cb.error.localizedDescription.UTF8String : "ok");
        }
    }
    return 0;
}
