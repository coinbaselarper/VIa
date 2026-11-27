// A/B and timing of translated compute kernels on a dispatch captured in the title (cspeed_capcfg.py output):
// both libraries run on identical copies of the captured heap, hash table, textures and samplers; every writable
// texture and the whole heap are compared bit for bit afterwards.
//   cspeed_ab <A.metallib> <B.metallib> <cfg.json> [--time N] [--seed S] [--shared-writes]
// --shared-writes: one large texture per writable binding, bound at every slot (title-sized targets: writes at real
// coordinates land), and A runs twice first so a non-deterministic result (slots racing on one texel) is reported.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static id<MTLDevice> dev;
static bool shared_writes = false;

struct Tex {
    id<MTLTexture> t;
    bool write;
};

struct Run {
    id<MTLBuffer> heap, table, push;
    std::vector<id<MTLBuffer>> args;
    std::vector<int> arg_index;
    std::vector<Tex> tex;
    std::vector<id<MTLSamplerState>> samplers;
};

static MTLPixelFormat format_of(int klass) {
    return klass == 0 ? MTLPixelFormatRGBA32Sint : klass == 1 ? MTLPixelFormatRGBA32Uint : MTLPixelFormatRGBA32Float;
}

static int class_of(NSString* elem) {
    if ([elem isEqualToString:@"int"] || [elem isEqualToString:@"short"]) return 0;
    if ([elem isEqualToString:@"uint"] || [elem isEqualToString:@"ushort"]) return 1;
    return 2;
}

static id<MTLTexture> make_texture(NSString* type, int klass, bool write, std::mt19937& rng) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.pixelFormat = format_of(klass);
    d.width = 64, d.height = 64, d.depth = 1, d.arrayLength = 1;
    if ([type isEqualToString:@"texture1d"]) d.textureType = MTLTextureType1D, d.width = 256, d.height = 1;
    else if ([type isEqualToString:@"texture1d_array"]) d.textureType = MTLTextureType1DArray, d.width = 256, d.height = 1, d.arrayLength = 4;
    else if ([type isEqualToString:@"texture2d_array"]) d.textureType = MTLTextureType2DArray, d.arrayLength = 4;
    else if ([type isEqualToString:@"texture3d"]) d.textureType = MTLTextureType3D, d.width = d.height = d.depth = 16;
    else if ([type isEqualToString:@"texturecube"]) d.textureType = MTLTextureTypeCube, d.width = d.height = 32;
    else if ([type isEqualToString:@"texturecube_array"]) d.textureType = MTLTextureTypeCubeArray, d.width = d.height = 32, d.arrayLength = 2;
    else d.textureType = MTLTextureType2D;
    if (write && shared_writes) {
        if (d.textureType == MTLTextureType3D) d.width = d.height = d.depth = 160;
        else if (d.textureType == MTLTextureType1D || d.textureType == MTLTextureType1DArray) d.width = 16384;
        else if (d.textureType == MTLTextureType2DArray) d.width = d.height = 1024;
        else if (d.textureType == MTLTextureType2D) d.width = 2048, d.height = 2560;
    } else if (write) {
        if (d.textureType == MTLTextureType3D) d.width = d.height = d.depth = 8;
        else if (d.height > 1) d.width = d.height = 32;
    }
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    const NSUInteger slices = d.textureType == MTLTextureTypeCube ? 6 : d.textureType == MTLTextureTypeCubeArray ? 6 * d.arrayLength : d.arrayLength;
    std::vector<uint32_t> px(d.width * d.height * d.depth * 4);
    for (NSUInteger s = 0; s < slices; ++s) {
        for (auto& v : px) {
            if (write) v = 0xDEADBEEFu;
            else if (klass == 2) {
                const float f = float(rng() % 2001) / 1000.0f - 0.5f;
                std::memcpy(&v, &f, 4);
            } else v = rng() & 0xFFFF;
        }
        [t replaceRegion:MTLRegionMake3D(0, 0, 0, d.width, d.height, d.depth) mipmapLevel:0 slice:s withBytes:px.data()
             bytesPerRow:d.width * 16 bytesPerImage:d.width * d.height * 16];
    }
    return t;
}

static NSData* file(NSString* path) { return [NSData dataWithContentsOfFile:path]; }

static bool setup(Run& r, NSDictionary* cfg, uint32_t seed) {
    std::mt19937 rng(seed);
    NSData* heap = file(cfg[@"heap"][@"file"]);
    NSData* table = file(cfg[@"table"][@"file"]);
    if (!heap || !table) return false;
    r.heap = [dev newBufferWithBytes:heap.bytes length:heap.length options:MTLResourceStorageModeShared];
    r.table = [dev newBufferWithBytes:table.bytes length:table.length options:MTLResourceStorageModeShared];
    NSArray* pw = cfg[@"push"][@"words"];
    r.push = [dev newBufferWithLength:MAX(pw.count * 4, 16) options:MTLResourceStorageModeShared];
    uint32_t* pp = (uint32_t*)r.push.contents;
    for (NSUInteger i = 0; i < pw.count; ++i) pp[i] = uint32_t([pw[i] unsignedLongLongValue]);
    NSArray* aw = cfg[@"push"][@"addr_words"];
    if (aw.count == 2) {
        const uint64_t base = r.heap.gpuAddress - [cfg[@"heap"][@"va0"] unsignedLongLongValue];
        pp[[aw[0] intValue]] = uint32_t(base);
        pp[[aw[1] intValue]] = uint32_t(base >> 32);
    }
    NSArray* cls = cfg[@"slot_class"];
    const NSUInteger slots = cls.count;
    for (NSDictionary* b in cfg[@"bindings"]) {
        NSString* kind = b[@"kind"];
        id<MTLBuffer> ab = nil;
        if ([kind isEqualToString:@"table"]) {
            ab = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
            *(uint64_t*)ab.contents = r.table.gpuAddress;
        } else if ([kind isEqualToString:@"sampler"]) {
            const int n = [cfg[@"samplers"] intValue];
            ab = [dev newBufferWithLength:n * 8 options:MTLResourceStorageModeShared];
            for (int i = 0; i < n; ++i) {
                MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
                sd.supportArgumentBuffers = YES;
                sd.minFilter = sd.magFilter = (i & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = (i & 2) ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
                if (i & 4) sd.compareFunction = MTLCompareFunctionLessEqual;
                id<MTLSamplerState> ss = [dev newSamplerStateWithDescriptor:sd];
                ((MTLResourceID*)ab.contents)[i] = ss.gpuResourceID;
                r.samplers.push_back(ss);
            }
        } else {
            NSString* type = b[@"type"];
            const bool wr = [b[@"write"] boolValue];
            ab = [dev newBufferWithLength:slots * 8 options:MTLResourceStorageModeShared];
            if (wr && shared_writes) {
                id<MTLTexture> t = make_texture(type, class_of(b[@"elem"]), true, rng);
                for (NSUInteger s = 0; s < slots; ++s) ((MTLResourceID*)ab.contents)[s] = t.gpuResourceID;
                r.tex.push_back({t, true});
            } else if (wr) {
                for (NSUInteger s = 0; s < slots; ++s) {
                    id<MTLTexture> t = make_texture(type, class_of(b[@"elem"]), true, rng);
                    ((MTLResourceID*)ab.contents)[s] = t.gpuResourceID;
                    r.tex.push_back({t, true});
                }
            } else {
                id<MTLTexture> per[3];
                for (int k = 0; k < 3; ++k) {
                    per[k] = make_texture(type, k, false, rng);
                    r.tex.push_back({per[k], false});
                }
                for (NSUInteger s = 0; s < slots; ++s) ((MTLResourceID*)ab.contents)[s] = per[[cls[s] intValue]].gpuResourceID;
            }
        }
        r.args.push_back(ab);
        r.arg_index.push_back([b[@"index"] intValue]);
    }
    return true;
}

static double run(id<MTLComputePipelineState> ps, Run& r, NSDictionary* cfg, int reps, int* status) {
    NSArray* th = cfg[@"threads"];
    NSArray* gr = cfg[@"groups"];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps];
    for (size_t i = 0; i < r.args.size(); ++i) [ce setBuffer:r.args[i] offset:0 atIndex:r.arg_index[i]];
    [ce setBuffer:r.push offset:0 atIndex:[cfg[@"push"][@"index"] intValue]];
    [ce useResource:r.heap usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [ce useResource:r.table usage:MTLResourceUsageRead];
    for (auto& t : r.tex) [ce useResource:t.t usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    for (int i = 0; i < reps; ++i)
        [ce dispatchThreadgroups:MTLSizeMake([gr[0] intValue], [gr[1] intValue], [gr[2] intValue])
            threadsPerThreadgroup:MTLSizeMake([th[0] intValue], [th[1] intValue], [th[2] intValue])];
    [ce endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    *status = int(cb.status);
    if (cb.error) std::printf("command buffer error: %s\n", cb.error.localizedDescription.UTF8String);
    return (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
}

static std::vector<uint32_t> dump(Run& r, size_t* tex_words) {
    std::vector<uint32_t> out;
    for (auto& x : r.tex) {
        if (!x.write) continue;
        id<MTLTexture> t = x.t;
        const NSUInteger slices = t.textureType == MTLTextureTypeCube ? 6 : t.textureType == MTLTextureTypeCubeArray ? 6 * t.arrayLength : t.arrayLength;
        std::vector<uint32_t> px(t.width * t.height * t.depth * 4);
        for (NSUInteger s = 0; s < slices; ++s) {
            [t getBytes:px.data() bytesPerRow:t.width * 16 bytesPerImage:t.width * t.height * 16
                 fromRegion:MTLRegionMake3D(0, 0, 0, t.width, t.height, t.depth) mipmapLevel:0 slice:s];
            out.insert(out.end(), px.begin(), px.end());
        }
    }
    if (tex_words) *tex_words = out.size();
    const uint32_t* hp = (const uint32_t*)r.heap.contents;
    out.insert(out.end(), hp, hp + r.heap.length / 4);
    return out;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 4) return 2;
        int reps = 0;
        uint32_t seed = 1;
        for (int i = 4; i + 1 < argc; ++i) {
            if (!std::strcmp(argv[i], "--time")) reps = std::atoi(argv[i + 1]);
            if (!std::strcmp(argv[i], "--seed")) seed = uint32_t(std::atoi(argv[i + 1]));
        }
        for (int i = 4; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--shared-writes")) shared_writes = true;
        }
        dev = MTLCreateSystemDefaultDevice();
        NSDictionary* cfg = [NSJSONSerialization JSONObjectWithData:file(@(argv[3])) options:0 error:nil];
        if (!cfg) { std::printf("bad config\n"); return 2; }
        NSArray* th = cfg[@"threads"];
        id<MTLComputePipelineState> ps[2];
        for (int k = 0; k < 2; ++k) {
            NSError* e = nil;
            id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1 + k])] error:&e];
            id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
            MTLComputePipelineDescriptor* pd = [MTLComputePipelineDescriptor new];
            pd.computeFunction = fn;
            pd.maxTotalThreadsPerThreadgroup = [th[0] intValue] * [th[1] intValue] * [th[2] intValue];
            ps[k] = fn ? [dev newComputePipelineStateWithDescriptor:pd options:MTLPipelineOptionNone reflection:nil error:&e] : nil;
            if (!ps[k]) { std::printf("pipeline %c: %s\n", 'A' + k, e.localizedDescription.UTF8String); return 1; }
        }
        std::vector<uint32_t> res[2], before;
        size_t tex_words = 0;
        for (int k = 0; k < 2; ++k) {
            Run r;
            if (!setup(r, cfg, seed)) { std::printf("setup failed\n"); return 2; }
            if (k == 0) before = dump(r, nullptr);
            int st = 0;
            const double ms = run(ps[k], r, cfg, 1, &st);
            if (st != 4) { std::printf("RESULT status %d for %c\n", st, 'A' + k); return 1; }
            std::printf("RUN %c %.3f ms\n", 'A' + k, ms);
            res[k] = dump(r, &tex_words);
        }
        if (shared_writes) {
            Run r;
            setup(r, cfg, seed);
            int st = 0;
            run(ps[0], r, cfg, 1, &st);
            const std::vector<uint32_t> again = dump(r, nullptr);
            size_t nd = 0;
            for (size_t i = 0; i < again.size(); ++i) nd += again[i] != res[0][i];
            std::printf("DETERMINISM A vs A: %zu words differ\n", nd);
        }
        size_t diff = 0, changed = 0, tex_changed = 0, heap_changed = 0;
        for (size_t i = 0; i < res[0].size(); ++i) {
            diff += res[0][i] != res[1][i];
            if (res[0][i] != before[i]) {
                ++changed;
                ++(i < tex_words ? tex_changed : heap_changed);
            }
        }
        std::printf("RESULT words %zu differ %zu changed_by_A %zu (textures %zu, heap %zu)\n", res[0].size(), diff, changed,
                    tex_changed, heap_changed);
        if (reps) {
            for (int k = 0; k < 2; ++k) {
                Run r;
                setup(r, cfg, seed);
                int st = 0;
                double best = 1e9;
                for (int t = 0; t < 5; ++t) best = std::min(best, run(ps[k], r, cfg, reps, &st));
                std::printf("TIME %c %.3f ms for %d dispatches (best of 5)\n", 'A' + k, best, reps);
            }
        }
    }
    return 0;
}
