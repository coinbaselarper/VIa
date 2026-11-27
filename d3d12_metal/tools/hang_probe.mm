// Bounded mesh-shader probe for a GPU hang: shaderc_msl/tools/mspeed_probe.mm's mesh path (same job JSON) with one
// draw per library, no repetitions, and a CB abort: a command buffer still running HANG_PROBE_ABORT_MS (default 2000)
// after its commit ends the process at once (exit 5), before the GPU watchdog fires. Run it only through
// shaderc_msl/tools/tmem_slot.sh (nothing else on the GPU). Lines: TIME <lib> <ms>, LOOPS <lib> <max trips of each
// counted loop> (record 0, words 3.. of group 0 when the library was built with the loop census), FAIL, ABORT.
//   hang_probe JOB.json
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static uint32_t mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

static std::atomic<int64_t> g_deadline{0};
static std::atomic<const char*> g_current{""};

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) { std::fprintf(stderr, "usage: hang_probe JOB.json\n"); return 2; }
        const char* ab = std::getenv("HANG_PROBE_ABORT_MS");
        const int64_t abort_ms = ab && *ab ? std::atoll(ab) : 2000;
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                const int64_t d = g_deadline.load();
                if (d && now_ms() > d) {
                    std::printf("ABORT\t%s\tcommand buffer still running past the CB abort; process ended\n", g_current.load());
                    std::fflush(stdout);
                    std::_Exit(5);
                }
            }
        }).detach();

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
        NSMutableArray* resources = [NSMutableArray array];
        auto tex = [&](MTLTextureType t, uint32_t seed) {
            MTLTextureDescriptor* td = [MTLTextureDescriptor new];
            td.textureType = t;
            if (t == MTLTextureType2DArray) td.arrayLength = 4;
            td.pixelFormat = MTLPixelFormatRGBA8Unorm;
            td.width = t == MTLTextureType3D ? 16 : 64;
            td.height = t == MTLTextureType1D ? 1 : (t == MTLTextureType3D ? 16 : 64);
            td.depth = t == MTLTextureType3D ? 16 : 1;
            td.storageMode = MTLStorageModeShared;
            td.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> x = [dev newTextureWithDescriptor:td];
            std::vector<uint32_t> px(td.width * td.height * td.depth);
            for (size_t i = 0; i < px.size(); ++i) px[i] = mix(uint32_t(i) * 2654435761u ^ seed);
            for (NSUInteger sl = 0; sl < td.arrayLength; ++sl)
                [x replaceRegion:MTLRegionMake3D(0, 0, 0, td.width, td.height, td.depth) mipmapLevel:0 slice:sl
                       withBytes:px.data() bytesPerRow:td.width * 4 bytesPerImage:td.width * td.height * 4];
            [resources addObject:x];
            return x;
        };
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.supportArgumentBuffers = YES;
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressModeRepeat;
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
        struct Arg { NSUInteger index; id<MTLBuffer> b; };
        std::vector<Arg> args;
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
                for (NSUInteger i = 0; i < ntex; ++i) w[i] = tex(t, uint32_t(i * 977 + k.integerValue)).gpuResourceID._impl;
            }
            args.push_back({(NSUInteger)k.integerValue, b});
        }

        NSMutableString* fsrc = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\nstruct FI {\n"];
        NSArray* locs = job[@"locations"] ?: @[];
        for (NSNumber* l in locs) [fsrc appendFormat:@"    float4 m%@ [[user(locn%@)]];\n", l, l];
        [fsrc appendString:@"};\nfragment float4 hang_f(FI i [[stage_in]]) {\n    float4 s = float4(0);\n"];
        for (NSNumber* l in locs) [fsrc appendFormat:@"    s += i.m%@;\n", l];
        [fsrc appendString:@"    return s;\n}\n"];
        id<MTLLibrary> flib = [dev newLibraryWithSource:fsrc options:nil error:&e];
        if (!flib) { std::fprintf(stderr, "fragment: %s\n", e.localizedDescription.UTF8String); return 2; }
        id<MTLFunction> frag = [flib newFunctionWithName:@"hang_f"];

        const NSUInteger groups = [job[@"groups"] unsignedIntegerValue];
        const NSUInteger push_index = [job[@"push_index"] unsignedIntegerValue];
        const NSUInteger rec = job[@"record_words"] ? [job[@"record_words"] unsignedIntegerValue] : 0;
        const NSUInteger loops = job[@"loops"] ? [job[@"loops"] unsignedIntegerValue] : 0;
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
        const NSUInteger out_bytes = [job[@"out_bytes"] unsignedIntegerValue];
        id<MTLBuffer> out = out_bytes ? [dev newBufferWithLength:out_bytes options:MTLResourceStorageModeShared] : nil;

        for (NSString* path in job[@"libs"]) {
            @autoreleasepool {
                id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:path] error:&e];
                id<MTLFunction> f = lib ? [lib newFunctionWithName:job[@"entry"] ?: @"main0"] : nil;
                if (!f) { std::printf("FAIL\t%s\tno function\n", path.UTF8String); continue; }
                MTLMeshRenderPipelineDescriptor* pd = [MTLMeshRenderPipelineDescriptor new];
                pd.meshFunction = f;
                pd.fragmentFunction = frag;
                pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
                pd.maxTotalThreadsPerMeshThreadgroup = 128;
                id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithMeshDescriptor:pd options:MTLPipelineOptionNone reflection:nil error:&e];
                if (!ps) { std::printf("FAIL\t%s\tpso %s\n", path.UTF8String, e.localizedDescription.UTF8String); continue; }
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
                if (trace) [en setMeshBuffer:trace offset:0 atIndex:[job[@"trace_index"] unsignedIntegerValue]];
                if (out) [en setMeshBuffer:out offset:0 atIndex:[job[@"out_index"] unsignedIntegerValue]];
                [en drawMeshThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                    threadsPerMeshThreadgroup:MTLSizeMake(128, 1, 1)];
                [en endEncoding];
                g_current.store(path.lastPathComponent.UTF8String);
                [cb commit];
                g_deadline.store(now_ms() + abort_ms);
                [cb waitUntilCompleted];
                g_deadline.store(0);
                if (cb.status != MTLCommandBufferStatusCompleted) {
                    std::printf("FAIL\t%s\tgpu %s\n", path.UTF8String, cb.error.localizedDescription.UTF8String);
                    return 3;
                }
                std::printf("TIME\t%s\t%.3f ms\t%lu groups\n", path.lastPathComponent.UTF8String,
                            (cb.GPUEndTime - cb.GPUStartTime) * 1e3, (unsigned long)groups);
                if (out && loops && rec > 3) {
                    const auto* w = (const uint32_t*)out.contents;
                    std::printf("LOOPS\t%s", path.lastPathComponent.UTF8String);
                    for (NSUInteger k = 0; k < loops && 3 + k < rec; ++k) {
                        uint32_t m = 0;
                        for (NSUInteger g = 0; g < groups; ++g) m = std::max(m, w[g * 129 * rec + 3 + k]);
                        std::printf("\t%u", m);
                    }
                    std::printf("\n");
                }
                std::fflush(stdout);
                if (NSString* prefix = job[@"out_prefix"]; prefix && out) {
                    NSString* p = [NSString stringWithFormat:@"%@.%@.out", prefix, path.lastPathComponent];
                    [[NSData dataWithBytesNoCopy:out.contents length:out_bytes freeWhenDone:NO] writeToFile:p atomically:YES];
                }
            }
        }
    }
    return 0;
}
