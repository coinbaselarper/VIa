// Mesh-shader speed probe: N groups of one translated mesh pipeline on a synthetic heap, GPU time per group, and an
// optional capture of the outputs (variants built with mspeed.py --capture) for A/B equality checks.
//   mspeed_probe JOB.json
// JOB: {"libs": [metallib, ...], "entry": "main0", "push_index": 8, "push": [36 words], "heap_bytes": N,
//       "heap_image": file of segments (u64 byte offset, u32 byte count, bytes), "hash_words": file of u32 (table),
//       "argbufs": {"1": "tex2d", "2": "sampler", "3": "table", "6": "tex1d", "7": "tex3d"}, "textures": K,
//       "locations": [user locations the mesh writes], "groups": G, "reps": R,
//       "trace_index": 29 (optional), "out_index": 30, "out_bytes": B, "out_prefix": path (optional)}
// Every lib is timed on the same inputs in the same process; lines: TIME <lib> best/median ms, us a group.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static uint32_t mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) { std::fprintf(stderr, "usage: mspeed_probe JOB.json\n"); return 2; }
        NSData* jd = [NSData dataWithContentsOfFile:@(argv[1])];
        NSDictionary* job = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
        if (!job) { std::fprintf(stderr, "bad job\n"); return 2; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError* e = nil;

        const NSUInteger heap_bytes = [job[@"heap_bytes"] unsignedIntegerValue];
        id<MTLBuffer> heap = [dev newBufferWithLength:heap_bytes options:MTLResourceStorageModeShared];
        std::memset(heap.contents, 0, heap_bytes);
        if (NSString* img = job[@"heap_image"]) {
            NSData* d = [NSData dataWithContentsOfFile:img];
            if (!d) { std::fprintf(stderr, "no heap image\n"); return 2; }
            const uint8_t* p = (const uint8_t*)d.bytes;
            const uint8_t* end = p + d.length;
            while (p + 12 <= end) {
                uint64_t off; uint32_t n;
                std::memcpy(&off, p, 8); std::memcpy(&n, p + 8, 4); p += 12;
                if (p + n > end || off + n > heap_bytes) { std::fprintf(stderr, "heap segment out of range\n"); return 2; }
                std::memcpy((uint8_t*)heap.contents + off, p, n);
                p += n;
            }
        }
        std::vector<uint32_t> push;
        for (NSNumber* n in job[@"push"]) push.push_back((uint32_t)n.unsignedLongLongValue);
        const uint64_t hva = heap.gpuAddress;
        if (push.size() < 36) push.resize(36, 0);
        push[32] = uint32_t(hva); push[33] = uint32_t(hva >> 32);
        push[34] = uint32_t(hva); push[35] = uint32_t(hva >> 32);

        id<MTLBuffer> table = nil;
        if (NSString* hw = job[@"hash_words"]) {
            NSData* d = [NSData dataWithContentsOfFile:hw];
            table = [dev newBufferWithBytes:d.bytes length:std::max<NSUInteger>(d.length, 64) options:MTLResourceStorageModeShared];
        } else {
            table = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
            std::memset(table.contents, 0, 64);
        }
        const NSUInteger ntex = job[@"textures"] ? [job[@"textures"] unsignedIntegerValue] : 16;
        NSMutableArray* keep = [NSMutableArray array];
        auto tex = [&](MTLTextureType t, uint32_t seed) {
            MTLTextureDescriptor* td = [MTLTextureDescriptor new];
            td.textureType = t;
            if (t == MTLTextureType2DArray) td.arrayLength = 4;
            const bool special = [job[@"special_textures"] boolValue];
            td.pixelFormat = special ? MTLPixelFormatRGBA32Float : MTLPixelFormatRGBA8Unorm;
            td.width = t == MTLTextureType3D ? 16 : 64;
            td.height = t == MTLTextureType1D ? 1 : (t == MTLTextureType3D ? 16 : 64);
            td.depth = t == MTLTextureType3D ? 16 : 1;
            td.storageMode = MTLStorageModeShared;
            td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> x = [dev newTextureWithDescriptor:td];
            const size_t comps = special ? 4 : 1;
            std::vector<uint32_t> px(td.width * td.height * td.depth * comps);
            static const uint32_t odd[8] = {0x7fc00000u, 0x7f800000u, 0xff800000u, 0x80000000u, 0x00000001u, 0x807fffffu, 0x00000000u, 0x3f800000u};
            for (size_t i = 0; i < px.size(); ++i) {
                const uint32_t h = mix(uint32_t(i) * 2654435761u ^ seed);
                px[i] = !special ? h : ((h & 7u) == 0u ? odd[(h >> 3) & 7u] : (0x3c000000u + (h >> 8) % 0x04000000u) | (h & 0x80000000u));
            }
            for (NSUInteger sl = 0; sl < td.arrayLength; ++sl)
                [x replaceRegion:MTLRegionMake3D(0, 0, 0, td.width, td.height, td.depth) mipmapLevel:0 slice:sl
                       withBytes:px.data() bytesPerRow:td.width * 4 * comps bytesPerImage:td.width * td.height * 4 * comps];
            [keep addObject:x];
            return x;
        };
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.supportArgumentBuffers = YES;
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeRepeat;
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
        struct Arg { NSUInteger index; id<MTLBuffer> b; };
        std::vector<Arg> args;
        NSMutableArray* resources = [NSMutableArray array];
        for (NSString* k in job[@"argbufs"]) {
            NSString* kind = job[@"argbufs"][k];
            id<MTLBuffer> b = [dev newBufferWithLength:std::max<NSUInteger>(ntex * 8, 64) options:MTLResourceStorageModeShared];
            auto* w = (uint64_t*)b.contents;
            std::memset(w, 0, b.length);
            if ([kind isEqualToString:@"table"]) {
                w[0] = table.gpuAddress;
            } else if ([kind isEqualToString:@"sampler"]) {
                for (NSUInteger i = 0; i < ntex; ++i) w[i] = smp.gpuResourceID._impl;
            } else {
                MTLTextureType t = [kind isEqualToString:@"tex1d"] ? MTLTextureType1D
                                 : [kind isEqualToString:@"tex3d"] ? MTLTextureType3D
                                 : [kind isEqualToString:@"tex2darr"] ? MTLTextureType2DArray
                                 : [kind isEqualToString:@"texcube"] ? MTLTextureTypeCube : MTLTextureType2D;
                for (NSUInteger i = 0; i < ntex; ++i) {
                    id<MTLTexture> x = tex(t, uint32_t(i * 977 + k.integerValue));
                    [resources addObject:x];
                    w[i] = x.gpuResourceID._impl;
                }
            }
            args.push_back({(NSUInteger)k.integerValue, b});
        }

        NSMutableString* fsrc = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\nstruct FI {\n"];
        NSArray* locs = job[@"locations"] ?: @[];
        for (NSNumber* l in locs) [fsrc appendFormat:@"    float4 m%@ [[user(locn%@)]];\n", l, l];
        [fsrc appendString:@"};\nfragment float4 mspeed_f(FI i [[stage_in]]) {\n    float4 s = float4(0);\n"];
        for (NSNumber* l in locs) [fsrc appendFormat:@"    s += i.m%@;\n", l];
        [fsrc appendString:@"    return s;\n}\n"];
        id<MTLLibrary> flib = [dev newLibraryWithSource:fsrc options:nil error:&e];
        if (!flib) { std::fprintf(stderr, "fragment: %s\n", e.localizedDescription.UTF8String); return 2; }
        id<MTLFunction> frag = [flib newFunctionWithName:@"mspeed_f"];

        const NSUInteger groups = [job[@"groups"] unsignedIntegerValue];
        const int reps = job[@"reps"] ? [job[@"reps"] intValue] : 7;
        const NSUInteger push_index = [job[@"push_index"] unsignedIntegerValue];
        MTLTextureDescriptor* rtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:64 height:64 mipmapped:NO];
        rtd.usage = MTLTextureUsageRenderTarget;
        rtd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:rtd];

        id<MTLBuffer> trace = nil;
        if (job[@"trace_index"]) {
            const NSUInteger words = heap_bytes / 4;
            trace = [dev newBufferWithLength:256 * 1024 + words / 8 options:MTLResourceStorageModeShared];
            std::memset(trace.contents, 0, trace.length);
            auto* t = (uint32_t*)trace.contents;
            t[0] = uint32_t(hva); t[1] = uint32_t(hva >> 32); t[2] = uint32_t(words);
        }
        id<MTLBuffer> out = nil;
        const NSUInteger out_bytes = [job[@"out_bytes"] unsignedIntegerValue];
        if (out_bytes) out = [dev newBufferWithLength:out_bytes options:MTLResourceStorageModeShared];

        const bool ps_mode = [job[@"mode"] isEqualToString:@"ps"];
        id<MTLFunction> vfn = nil;
        std::vector<id<MTLTexture>> targets;
        const NSUInteger W = job[@"width"] ? [job[@"width"] unsignedIntegerValue] : 256;
        const NSUInteger H = job[@"height"] ? [job[@"height"] unsignedIntegerValue] : 256;
        if (ps_mode) {
            NSString* vs = [NSString stringWithContentsOfFile:job[@"vertex_src"] encoding:NSUTF8StringEncoding error:nil];
            id<MTLLibrary> vl = vs ? [dev newLibraryWithSource:vs options:nil error:&e] : nil;
            vfn = vl ? [vl newFunctionWithName:@"mspeed_v"] : nil;
            if (!vfn) { std::fprintf(stderr, "vertex: %s\n", e ? e.localizedDescription.UTF8String : "no source"); return 2; }
            for (NSNumber* fm in job[@"colors"]) {
                MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)fm.unsignedIntegerValue width:W height:H mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget;
                td.storageMode = MTLStorageModeShared;
                targets.push_back([dev newTextureWithDescriptor:td]);
            }
        }
        for (NSString* path in job[@"libs"]) {
            @autoreleasepool {
                id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:path] error:&e];
                if (ps_mode) {
                    id<MTLFunction> ff = lib ? [lib newFunctionWithName:job[@"entry"] ?: @"main0"] : nil;
                    if (!ff) { std::printf("FAIL\t%s\tno function\n", path.UTF8String); continue; }
                    MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
                    rd.vertexFunction = vfn;
                    rd.fragmentFunction = ff;
                    for (size_t i = 0; i < targets.size(); ++i) rd.colorAttachments[i].pixelFormat = targets[i].pixelFormat;
                    const double c0 = [NSDate timeIntervalSinceReferenceDate];
                    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:rd error:&e];
                    const double c1 = [NSDate timeIntervalSinceReferenceDate];
                    if (!ps) { std::printf("FAIL\t%s\tpso %s\n", path.UTF8String, e.localizedDescription.UTF8String); continue; }
                    const NSUInteger draws = job[@"draws"] ? [job[@"draws"] unsignedIntegerValue] : 1;
                    std::vector<double> times;
                    for (int r = 0; r < reps + 1; ++r) {
                        id<MTLCommandBuffer> cb = [q commandBuffer];
                        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                        for (size_t i = 0; i < targets.size(); ++i) {
                            rp.colorAttachments[i].texture = targets[i];
                            rp.colorAttachments[i].loadAction = MTLLoadActionClear;
                            rp.colorAttachments[i].storeAction = MTLStoreActionStore;
                        }
                        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
                        [en setRenderPipelineState:ps];
                        [en setFragmentBytes:push.data() length:push.size() * 4 atIndex:push_index];
                        for (auto& a : args) [en setFragmentBuffer:a.b offset:0 atIndex:a.index];
                        [en useResource:heap usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
                        [en useResource:table usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
                        for (id<MTLTexture> x in resources) [en useResource:x usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
                        if (trace) [en setFragmentBuffer:trace offset:0 atIndex:[job[@"trace_index"] unsignedIntegerValue]];
                        for (NSUInteger d = 0; d < draws; ++d) [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                        [en endEncoding];
                        [cb commit];
                        [cb waitUntilCompleted];
                        if (cb.status != MTLCommandBufferStatusCompleted) {
                            std::printf("FAIL\t%s\tgpu %s\n", path.UTF8String, cb.error.localizedDescription.UTF8String);
                            return 3;
                        }
                        if (r) times.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e3);
                    }
                    std::sort(times.begin(), times.end());
                    const double px = double(W) * H * draws;
                    std::printf("TIME\t%s\tbest %.3f ms\tmedian %.3f ms\t%.3f ns a pixel (best)\tpso %.1f s\n", path.lastPathComponent.UTF8String,
                                times.front(), times[times.size() / 2], times.front() * 1e6 / px, c1 - c0);
                    std::fflush(stdout);
                    if (NSString* prefix = job[@"out_prefix"]) {
                        NSMutableData* all = [NSMutableData data];
                        for (id<MTLTexture> t : targets) {
                            const NSUInteger bpp = t.pixelFormat == MTLPixelFormatRGBA32Float || t.pixelFormat == MTLPixelFormatRGBA32Uint || t.pixelFormat == MTLPixelFormatRGBA32Sint ? 16 : 8;
                            NSMutableData* d = [NSMutableData dataWithLength:W * H * bpp];
                            [t getBytes:d.mutableBytes bytesPerRow:W * bpp fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
                            [all appendData:d];
                        }
                        [all writeToFile:[NSString stringWithFormat:@"%@.%@.out", prefix, path.lastPathComponent] atomically:YES];
                    }
                    continue;
                }
                id<MTLFunction> f = lib ? [lib newFunctionWithName:job[@"entry"] ?: @"main0"] : nil;
                if (!f) { std::printf("FAIL\t%s\tno function %s\n", path.UTF8String, e ? e.localizedDescription.UTF8String : ""); continue; }
                MTLMeshRenderPipelineDescriptor* pd = [MTLMeshRenderPipelineDescriptor new];
                pd.meshFunction = f;
                pd.fragmentFunction = frag;
                pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
                pd.maxTotalThreadsPerMeshThreadgroup = 128;
                const double c0 = [NSDate timeIntervalSinceReferenceDate];
                id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithMeshDescriptor:pd options:MTLPipelineOptionNone reflection:nil error:&e];
                const double c1 = [NSDate timeIntervalSinceReferenceDate];
                if (!ps) { std::printf("FAIL\t%s\tpso %s\n", path.UTF8String, e.localizedDescription.UTF8String); continue; }
                std::vector<double> times;
                bool failed = false;
                for (int r = 0; r < reps + 1 && !failed; ++r) {
                    if (trace) std::memset((uint8_t*)trace.contents + 12, 0, trace.length - 12);
                    if (out) std::memset(out.contents, 0, out_bytes);
                    id<MTLCommandBuffer> cb = [q commandBuffer];
                    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = rt;
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
                    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
                    [en setRenderPipelineState:ps];
                    [en setMeshBytes:push.data() length:push.size() * 4 atIndex:push_index];
                    for (auto& a : args) [en setMeshBuffer:a.b offset:0 atIndex:a.index];
                    [en useResource:heap usage:MTLResourceUsageRead stages:MTLRenderStageMesh];
                    [en useResource:table usage:MTLResourceUsageRead stages:MTLRenderStageMesh];
                    for (id<MTLTexture> x in resources) [en useResource:x usage:MTLResourceUsageRead stages:MTLRenderStageMesh];
                    if (trace) {
                        [en setMeshBuffer:trace offset:0 atIndex:[job[@"trace_index"] unsignedIntegerValue]];
                    }
                    if (out) [en setMeshBuffer:out offset:0 atIndex:[job[@"out_index"] unsignedIntegerValue]];
                    [en drawMeshThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                        threadsPerMeshThreadgroup:MTLSizeMake(128, 1, 1)];
                    [en endEncoding];
                    [cb commit];
                    [cb waitUntilCompleted];
                    if (cb.status != MTLCommandBufferStatusCompleted) {
                        std::printf("FAIL\t%s\tgpu %s\n", path.UTF8String, cb.error.localizedDescription.UTF8String);
                        failed = true;
                        break;
                    }
                    if (r) times.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e3);
                }
                if (failed) return 3;
                std::sort(times.begin(), times.end());
                const double best = times.empty() ? 0 : times.front(), med = times.empty() ? 0 : times[times.size() / 2];
                std::printf("TIME\t%s\tbest %.3f ms\tmedian %.3f ms\t%.2f us a group (best)\t%.2f (median)\tpso %.1f s\tmesh width %lu\n",
                            path.lastPathComponent.UTF8String, best, med, best * 1e3 / groups, med * 1e3 / groups, c1 - c0,
                            (unsigned long)ps.meshThreadExecutionWidth);
                std::fflush(stdout);
                NSString* prefix = job[@"out_prefix"];
                if (prefix && out) {
                    NSString* p = [NSString stringWithFormat:@"%@.%@.out", prefix, path.lastPathComponent];
                    [[NSData dataWithBytesNoCopy:out.contents length:out_bytes freeWhenDone:NO] writeToFile:p atomically:YES];
                }
                if (prefix && trace) {
                    NSString* p = [NSString stringWithFormat:@"%@.%@.trace", prefix, path.lastPathComponent];
                    [[NSData dataWithBytesNoCopy:trace.contents length:trace.length freeWhenDone:NO] writeToFile:p atomically:YES];
                }
            }
        }
    }
    return 0;
}
