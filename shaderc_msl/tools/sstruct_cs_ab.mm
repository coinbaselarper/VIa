// A/B runner for translated compute kernels (contract bindings): runs library A and library B on the same inputs and
// compares every writable texture and buffer bit for bit. Copy of tmem_ab.mm (sstruct): buffer 29 holds the heap's
// range for kernels built with sstruct.py's safe_cs (every raw heap access outside the heap goes to a sink).
//   sstruct_cs_ab <A.metallib> <B.metallib> <config.json> [--time N]
// config: {"threads":[x,y,z], "groups":[x,y,z], "push":{"index":12, "words":[...]},
//          "heap":{"words":[...], "addr_words":[lo,hi]},          heap address written into push words lo/hi
//          "buffers":[{"index":3, "words":[...]}],                 argument buffer whose slot 0 holds that buffer's address
//          "textures":[{"index":1, "type":"2d"|"2d_array", "write":0|1, "slots":["rgba32sint",...], "size":[w,h,layers]}],
//          "samplers":[{"index":2, "count":n}], "seed":s}
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static id<MTLDevice> d;

static MTLPixelFormat pf(NSString* s) {
    if ([s isEqualToString:@"rgba32sint"]) return MTLPixelFormatRGBA32Sint;
    if ([s isEqualToString:@"rgba32uint"]) return MTLPixelFormatRGBA32Uint;
    return MTLPixelFormatRGBA32Float;
}

struct Run {
    std::vector<id<MTLTexture>> tex;
    std::vector<bool> writable;
    std::vector<id<MTLBuffer>> bufs;
    std::vector<id<MTLBuffer>> args;
    std::vector<int> arg_index;
    id<MTLBuffer> push, heap;
    std::vector<id<MTLSamplerState>> samplers;
};

static uint32_t tex_mask = 0xFFFFFFFFu;

static void fill(id<MTLTexture> t, std::mt19937& rng, bool sentinel) {
    const NSUInteger w = t.width, h = t.height, n = t.arrayLength;
    std::vector<uint32_t> px(w * h * 4);
    for (NSUInteger l = 0; l < n; ++l) {
        for (auto& v : px) v = sentinel ? 0xDEADBEEFu : uint32_t(rng()) & tex_mask;
        [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 slice:l withBytes:px.data() bytesPerRow:w * 16 bytesPerImage:w * h * 16];
    }
}

static bool setup(Run& r, NSDictionary* cfg, uint32_t seed) {
    std::mt19937 rng(seed);
    if (cfg[@"tex_mask"]) tex_mask = uint32_t([cfg[@"tex_mask"] unsignedLongLongValue]);
    NSArray* pw = cfg[@"push"][@"words"];
    NSArray* hw = cfg[@"heap"][@"words"];
    const NSUInteger heap_words = MAX(hw.count, NSUInteger([cfg[@"heap"][@"size_words"] unsignedLongLongValue]));
    r.heap = [d newBufferWithLength:MAX(heap_words * 4, 16) options:MTLResourceStorageModeShared];
    std::memset(r.heap.contents, 0, r.heap.length);
    uint32_t* hp = (uint32_t*)r.heap.contents;
    for (NSUInteger i = 0; i < hw.count; ++i) hp[i] = uint32_t([hw[i] unsignedLongLongValue]);
    r.push = [d newBufferWithLength:MAX(pw.count * 4, 16) options:MTLResourceStorageModeShared];
    uint32_t* pp = (uint32_t*)r.push.contents;
    for (NSUInteger i = 0; i < pw.count; ++i) pp[i] = uint32_t([pw[i] unsignedLongLongValue]);
    NSArray* aw = cfg[@"heap"][@"addr_words"];
    if (aw.count == 2) {
        const uint64_t a = r.heap.gpuAddress;
        pp[[aw[0] intValue]] = uint32_t(a);
        pp[[aw[1] intValue]] = uint32_t(a >> 32);
    }
    {
        id<MTLBuffer> rb = [d newBufferWithLength:64 options:MTLResourceStorageModeShared];
        uint32_t* w = (uint32_t*)rb.contents;
        std::memset(w, 0, 64);
        w[0] = uint32_t(r.heap.gpuAddress);
        w[1] = uint32_t(r.heap.gpuAddress >> 32);
        w[2] = uint32_t(r.heap.length / 4);
        r.args.push_back(rb);
        r.arg_index.push_back(29);
    }
    for (NSDictionary* b in cfg[@"buffers"]) {
        NSArray* w = b[@"words"];
        id<MTLBuffer> buf = [d newBufferWithLength:MAX(w.count * 4, 16) options:MTLResourceStorageModeShared];
        uint32_t* p = (uint32_t*)buf.contents;
        for (NSUInteger i = 0; i < w.count; ++i) p[i] = uint32_t([w[i] unsignedLongLongValue]);
        r.bufs.push_back(buf);
        id<MTLBuffer> ab = [d newBufferWithLength:64 options:MTLResourceStorageModeShared];
        *(uint64_t*)ab.contents = buf.gpuAddress;
        r.args.push_back(ab);
        r.arg_index.push_back([b[@"index"] intValue]);
    }
    for (NSDictionary* t in cfg[@"textures"]) {
        NSArray* slots = t[@"slots"];
        NSArray* size = t[@"size"];
        const bool wr = [t[@"write"] boolValue];
        const bool arr = [t[@"type"] isEqualToString:@"2d_array"];
        id<MTLBuffer> ab = [d newBufferWithLength:MAX(slots.count * 8, 16) options:MTLResourceStorageModeShared];
        for (NSUInteger i = 0; i < slots.count; ++i) {
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf(slots[i]) width:[size[0] intValue]
                                                                                         height:[size[1] intValue] mipmapped:NO];
            if (arr) { td.textureType = MTLTextureType2DArray; td.arrayLength = [size[2] intValue]; }
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            td.storageMode = MTLStorageModeShared;
            id<MTLTexture> tx = [d newTextureWithDescriptor:td];
            fill(tx, rng, wr);
            ((MTLResourceID*)ab.contents)[i] = tx.gpuResourceID;
            r.tex.push_back(tx);
            r.writable.push_back(wr);
        }
        r.args.push_back(ab);
        r.arg_index.push_back([t[@"index"] intValue]);
    }
    for (NSDictionary* s in cfg[@"samplers"]) {
        const int n = [s[@"count"] intValue];
        id<MTLBuffer> ab = [d newBufferWithLength:MAX(n * 8, 16) options:MTLResourceStorageModeShared];
        for (int i = 0; i < n; ++i) {
            MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
            sd.supportArgumentBuffers = YES;
            sd.minFilter = (i & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.magFilter = sd.minFilter;
            sd.sAddressMode = sd.tAddressMode = (i & 2) ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
            id<MTLSamplerState> ss = [d newSamplerStateWithDescriptor:sd];
            ((MTLResourceID*)ab.contents)[i] = ss.gpuResourceID;
            r.samplers.push_back(ss);
        }
        r.args.push_back(ab);
        r.arg_index.push_back([s[@"index"] intValue]);
    }
    return true;
}

static double run(id<MTLComputePipelineState> ps, Run& r, NSDictionary* cfg, int reps, int* status) {
    NSArray* th = cfg[@"threads"];
    NSArray* gr = cfg[@"groups"];
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps];
    for (size_t i = 0; i < r.args.size(); ++i) [ce setBuffer:r.args[i] offset:0 atIndex:r.arg_index[i]];
    [ce setBuffer:r.push offset:0 atIndex:[cfg[@"push"][@"index"] intValue]];
    [ce useResource:r.heap usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    for (auto b : r.bufs) [ce useResource:b usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    for (auto t : r.tex) [ce useResource:t usage:MTLResourceUsageRead | MTLResourceUsageWrite];
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

static std::vector<uint32_t> dump(Run& r, std::vector<size_t>* sizes) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < r.tex.size(); ++i) {
        if (!r.writable[i]) continue;
        id<MTLTexture> t = r.tex[i];
        std::vector<uint32_t> px(t.width * t.height * 4);
        for (NSUInteger l = 0; l < t.arrayLength; ++l) {
            [t getBytes:px.data() bytesPerRow:t.width * 16 bytesPerImage:t.width * t.height * 16
                 fromRegion:MTLRegionMake2D(0, 0, t.width, t.height) mipmapLevel:0 slice:l];
            out.insert(out.end(), px.begin(), px.end());
        }
        if (sizes) sizes->push_back(px.size() * t.arrayLength);
    }
    const uint32_t* hp = (const uint32_t*)r.heap.contents;
    out.insert(out.end(), hp, hp + r.heap.length / 4);
    for (auto b : r.bufs) {
        const uint32_t* p = (const uint32_t*)b.contents;
        out.insert(out.end(), p, p + b.length / 4);
    }
    return out;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 4) return 2;
        int reps = 0;
        for (int i = 4; i + 1 < argc; ++i)
            if (!std::strcmp(argv[i], "--time")) reps = std::atoi(argv[i + 1]);
        d = MTLCreateSystemDefaultDevice();
        NSData* js = [NSData dataWithContentsOfFile:@(argv[3])];
        NSDictionary* cfg = [NSJSONSerialization JSONObjectWithData:js options:0 error:nil];
        if (!cfg) { std::printf("bad config\n"); return 2; }
        NSArray* th = cfg[@"threads"];
        id<MTLComputePipelineState> ps[2];
        for (int k = 0; k < 2; ++k) {
            NSError* e = nil;
            id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1 + k])] error:&e];
            id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
            MTLComputePipelineDescriptor* pd = [MTLComputePipelineDescriptor new];
            pd.computeFunction = fn;
            pd.maxTotalThreadsPerThreadgroup = [th[0] intValue] * [th[1] intValue] * [th[2] intValue];
            ps[k] = fn ? [d newComputePipelineStateWithDescriptor:pd options:MTLPipelineOptionNone reflection:nil error:&e] : nil;
            if (!ps[k]) { std::printf("pipeline %c: %s\n", 'A' + k, e.localizedDescription.UTF8String); return 1; }
        }
        const uint32_t seed = uint32_t([cfg[@"seed"] unsignedIntValue]);
        std::vector<uint32_t> res[2];
        std::vector<size_t> sizes;
        std::vector<uint32_t> before;
        for (int k = 0; k < 2; ++k) {
            Run r;
            setup(r, cfg, seed);
            if (k == 0) before = dump(r, nullptr);
            int st = 0;
            run(ps[k], r, cfg, 1, &st);
            if (st != 4) { std::printf("RESULT status %d for %c\n", st, 'A' + k); return 1; }
            res[k] = dump(r, k == 0 ? &sizes : nullptr);
        }
        size_t diff = 0, changed = 0, nonzero = 0;
        for (size_t i = 0; i < res[0].size(); ++i) {
            diff += res[0][i] != res[1][i];
            changed += res[0][i] != before[i];
            nonzero += res[0][i] != 0 && res[0][i] != 0xDEADBEEFu;
        }
        std::printf("RESULT words %zu differ %zu changed_by_A %zu nonzero_A %zu\n", res[0].size(), diff, changed, nonzero);
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
