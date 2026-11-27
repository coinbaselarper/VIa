#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static uint32_t mix(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

static double now() { return [NSDate timeIntervalSinceReferenceDate]; }

struct Ctx {
    NSMutableDictionary *psos = [NSMutableDictionary dictionary];
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLBuffer> heap;
    uint64_t heap_bytes = 0;
    std::string pattern;
    uint32_t seed = 0;
};

static void fill_heap(Ctx &c, NSString *pattern, uint32_t seed, uint64_t bytes) {
    std::string p = pattern.UTF8String;
    if (c.heap && c.pattern == p && c.seed == seed && c.heap_bytes == bytes) return;
    if (!c.heap || c.heap_bytes != bytes) {
        c.heap = [c.dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        c.heap_bytes = bytes;
    }
    uint32_t *hp = (uint32_t *)c.heap.contents;
    uint64_t n = bytes / 4;
    if (p == "rand") {
        for (uint64_t i = 0; i < n; i++) hp[i] = mix((uint32_t)i ^ seed);
    } else if (p == "small") {
        for (uint64_t i = 0; i < n; i++) {
            uint32_t h = mix((uint32_t)i ^ seed);
            hp[i] = (h & 0x7F) | (((h >> 8) & 0x7F) << 10) | (((h >> 16) & 0x7F) << 20) | ((h >> 24) << 27 & 0x78000000u);
        }
    } else if (p == "bytes7f") {
        for (uint64_t i = 0; i < n; i++) hp[i] = mix((uint32_t)i ^ seed) & 0x7F7F7F7Fu;
    } else if (p == "bytes3f") {
        for (uint64_t i = 0; i < n; i++) hp[i] = mix((uint32_t)i ^ seed) & 0x3F3F3F3Fu;
    } else if (p == "hi") {
        for (uint64_t i = 0; i < n; i++) hp[i] = (mix((uint32_t)i ^ seed) & 0x7F7F7F7Fu) | 0x40404040u;
    } else if (p == "strip") {
        for (uint64_t i = 0; i < n; i++) {
            uint32_t a = (uint32_t)(i & 127), b = (a + 1) & 127, d = (a + 2) & 127;
            hp[i] = a | (b << 10) | (d << 20);
        }
    } else if (p.rfind("const:", 0) == 0) {
        uint32_t w = (uint32_t)strtoul(p.c_str() + 6, nullptr, 16);
        for (uint64_t i = 0; i < n; i++) hp[i] = w;
    } else if (p.rfind("rngkb:", 0) == 0) {
        unsigned long lo = 0, hi = 0;
        sscanf(p.c_str() + 6, "%lu:%lu", &lo, &hi);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t kb = (i * 4) >> 10;
            hp[i] = (kb >= lo && kb < hi) ? 0xFFFFFFFFu : mix((uint32_t)i ^ seed);
        }
    } else if (p.rfind("sparse:", 0) == 0) {
        uint32_t m = (uint32_t)strtoul(p.c_str() + 7, nullptr, 10);
        for (uint64_t i = 0; i < n; i++) {
            uint32_t a = mix((uint32_t)i ^ seed);
            hp[i] = (a % m == 0) ? mix(a ^ 0x9e3779b9u) : 0xFFFFFFFFu;
        }
    } else if (p.rfind("or:", 0) == 0) {
        uint32_t m = (uint32_t)strtoul(p.c_str() + 3, nullptr, 16);
        for (uint64_t i = 0; i < n; i++) hp[i] = mix((uint32_t)i ^ seed) | m;
    } else if (p.rfind("modi:", 0) == 0) {
        uint32_t m = (uint32_t)strtoul(p.c_str() + 5, nullptr, 16);
        for (uint64_t i = 0; i < n; i++) hp[i] = (uint32_t)(i % m);
    } else {
        for (uint64_t i = 0; i < n; i++) hp[i] = (0x3F00u + (mix((uint32_t)i ^ seed) % 0xC0u)) << 16;
    }
    c.pattern = p;
    c.seed = seed;
}

static NSDictionary *run(Ctx &c, NSDictionary *job) {
    NSMutableDictionary *r = [NSMutableDictionary dictionary];
    NSString *mode = job[@"mode"];
    NSError *e = nil;
    id<MTLLibrary> lib = [c.dev newLibraryWithURL:[NSURL fileURLWithPath:job[@"lib"]] error:&e];
    if (!lib) { r[@"result"] = @"LIB_FAIL"; r[@"error"] = e.localizedDescription ?: @""; return r; }
    id<MTLFunction> f = [lib newFunctionWithName:job[@"fn"] ?: @"main0"];
    if (!f) { r[@"result"] = @"NO_FUNCTION"; return r; }
    NSArray *g = job[@"groups"];
    NSUInteger gx = [g[0] unsignedIntegerValue], gy = [g[1] unsignedIntegerValue], gz = [g[2] unsignedIntegerValue];
    NSUInteger heap_bytes = [job[@"heap_mb"] unsignedIntegerValue] << 20;
    if (![job[@"warm"] boolValue]) fill_heap(c, job[@"pattern"] ?: @"rand", (uint32_t)[job[@"seed"] unsignedIntegerValue], heap_bytes);
    else if (!c.heap) c.heap = [c.dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    if (job[@"inject_file"]) {
        NSData *blob = [NSData dataWithContentsOfFile:job[@"inject_file"]];
        NSUInteger off = [job[@"inject_off"] unsignedIntegerValue];
        if (!blob || off + blob.length > c.heap_bytes) { r[@"result"] = @"INJECT_FAIL"; return r; }
        memcpy((uint8_t *)c.heap.contents + off, blob.bytes, blob.length);
        c.pattern = "";
    }
    uint64_t heap_va = c.heap.gpuAddress;
    NSUInteger push_bytes = [job[@"push_bytes"] unsignedIntegerValue];
    std::vector<uint32_t> push(push_bytes / 4, 0);
    for (int i = 0; i < 32 && i < (int)push.size(); i++) push[i] = 0x01000000u + 0x40u * (uint32_t)i;
    push[32] = (uint32_t)heap_va; push[33] = (uint32_t)(heap_va >> 32);
    push[34] = (uint32_t)heap_va; push[35] = (uint32_t)(heap_va >> 32);
    id<MTLBuffer> out = nil;
    NSUInteger out_bytes = [job[@"out_bytes"] unsignedIntegerValue];
    if (out_bytes) {
        out = [c.dev newBufferWithLength:out_bytes options:MTLResourceStorageModeShared];
        memset(out.contents, 0, out_bytes);
    }
    if ([mode isEqualToString:@"ref"] && push.size() >= 38) {
        uint64_t va = out.gpuAddress;
        push[36] = (uint32_t)va; push[37] = (uint32_t)(va >> 32);
    }
    NSMutableArray<id<MTLBuffer>> *keep = [NSMutableArray array];
    NSDictionary *dummy = job[@"dummy_buffers"];
    auto bind = [&](auto setter) {
        for (NSString *k in dummy) {
            NSUInteger bytes = [dummy[k] unsignedIntegerValue];
            id<MTLBuffer> b = [c.dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            memset(b.contents, 0, bytes);
            [keep addObject:b];
            setter(b, (NSUInteger)k.integerValue);
        }
    };
    NSUInteger push_index = [job[@"push_index"] unsignedIntegerValue];
    id<MTLCommandBuffer> cb = [c.q commandBuffer];
    double t0 = now();
    if ([mode isEqualToString:@"cs"]) {
        NSString *ck = [@"k:" stringByAppendingString:job[@"lib"]];
        id<MTLComputePipelineState> ps = c.psos[ck];
        if (!ps) {
            MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
            d.computeFunction = f;
            ps = [c.dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&e];
            if (!ps) { r[@"result"] = @"PSO_FAIL"; r[@"error"] = e.localizedDescription ?: @""; return r; }
            c.psos[ck] = ps;
        }
        r[@"pso_s"] = @(now() - t0);
        r[@"static_tg_mem"] = @(ps.staticThreadgroupMemoryLength);
        if ([job[@"warm"] boolValue]) { r[@"result"] = @"OK"; return r; }
        auto digest = [&]() {
            uint64_t hsh = 1469598103934665603ull;
            const uint64_t *w = (const uint64_t *)c.heap.contents;
            for (uint64_t i = 0; i < c.heap_bytes / 8; i++) hsh = (hsh ^ w[i]) * 1099511628211ull;
            return hsh;
        };
        uint64_t before = digest();
        id<MTLBuffer> xb = nil;
        if (job[@"xb_index"]) {
            xb = [c.dev newBufferWithLength:[job[@"xb_bytes"] unsignedIntegerValue] options:MTLResourceStorageModeShared];
            memset(xb.contents, 0, xb.length);
        }
        id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
        [en setComputePipelineState:ps];
        [en setBytes:push.data() length:push_bytes atIndex:push_index];
        if (xb) { [en setBuffer:xb offset:0 atIndex:[job[@"xb_index"] unsignedIntegerValue]]; [en useResource:xb usage:MTLResourceUsageRead | MTLResourceUsageWrite]; }
        [en useResource:c.heap usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        NSArray *th = job[@"threads"];
        [en dispatchThreadgroups:MTLSizeMake(gx, gy, gz) threadsPerThreadgroup:MTLSizeMake([th[0] unsignedIntegerValue], [th[1] unsignedIntegerValue], [th[2] unsignedIntegerValue])];
        [en endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        r[@"gpu_ms"] = @((cb.GPUEndTime - cb.GPUStartTime) * 1e3);
        if (cb.status != MTLCommandBufferStatusCompleted) { r[@"result"] = @"GPU_ERROR"; r[@"error"] = cb.error ? cb.error.localizedDescription : @""; return r; }
        uint64_t after = digest();
        r[@"heap_before"] = [NSString stringWithFormat:@"%016llx", before];
        r[@"heap_after"] = [NSString stringWithFormat:@"%016llx", after];
        r[@"result"] = @"OK";
        return r;
    }
    if ([mode isEqualToString:@"ref"]) {
        NSString *ck = [@"c:" stringByAppendingString:job[@"lib"]];
        id<MTLComputePipelineState> ps = c.psos[ck];
        if (!ps) {
            MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
            d.computeFunction = f;
            ps = [c.dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&e];
            if (!ps) { r[@"result"] = @"PSO_FAIL"; r[@"error"] = e.localizedDescription ?: @""; return r; }
            c.psos[ck] = ps;
        }
        r[@"pso_s"] = @(now() - t0);
        if ([job[@"warm"] boolValue]) { r[@"result"] = @"OK"; return r; }
        id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
        [en setComputePipelineState:ps];
        [en setBytes:push.data() length:push_bytes atIndex:push_index];
        bind([&](id<MTLBuffer> b, NSUInteger idx) { [en setBuffer:b offset:0 atIndex:idx]; [en useResource:b usage:MTLResourceUsageRead]; });
        [en useResource:c.heap usage:MTLResourceUsageRead];
        [en useResource:out usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [en dispatchThreadgroups:MTLSizeMake(gx, gy, gz) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        [en endEncoding];
    } else {
        id<MTLFunction> of = nil;
        if (job[@"obj_lib"]) {
            id<MTLLibrary> ol = [c.dev newLibraryWithURL:[NSURL fileURLWithPath:job[@"obj_lib"]] error:&e];
            of = ol ? [ol newFunctionWithName:job[@"obj_fn"] ?: @"main0"] : nil;
            if (!of) { r[@"result"] = @"OBJ_FAIL"; r[@"error"] = e.localizedDescription ?: @""; return r; }
        }
        NSString *ck = [NSString stringWithFormat:@"m:%@:%@", job[@"lib"], job[@"obj_lib"] ?: @""];
        id<MTLRenderPipelineState> ps = c.psos[ck];
        if (!ps) {
            MTLMeshRenderPipelineDescriptor *d = [MTLMeshRenderPipelineDescriptor new];
            if (of) { d.objectFunction = of; d.maxTotalThreadsPerObjectThreadgroup = 1; d.payloadMemoryLength = 24; }
            d.meshFunction = f;
            d.rasterizationEnabled = NO;
            d.maxTotalThreadsPerMeshThreadgroup = 128;
            ps = [c.dev newRenderPipelineStateWithMeshDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&e];
            if (!ps) { r[@"result"] = @"PSO_FAIL"; r[@"error"] = e.localizedDescription ?: @""; return r; }
            c.psos[ck] = ps;
        }
        r[@"pso_s"] = @(now() - t0);
        if ([job[@"warm"] boolValue]) { r[@"result"] = @"OK"; return r; }
        r[@"mesh_width"] = @(ps.meshThreadExecutionWidth);
        if (![mode isEqualToString:@"split"]) return r;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = [c.dev newTextureWithDescriptor:td];
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps];
        [en setMeshBytes:push.data() length:push_bytes atIndex:push_index];
        id<MTLBuffer> capab = nil;
        if (job[@"cap_index"]) {
            if ([job[@"cap_direct"] boolValue]) {
                capab = out;
            } else {
                capab = [c.dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
                *(uint64_t *)capab.contents = out.gpuAddress;
            }
            [keep addObject:capab];
            [en setMeshBuffer:capab offset:0 atIndex:[job[@"cap_index"] unsignedIntegerValue]];
            [en useResource:capab usage:MTLResourceUsageRead stages:MTLRenderStageMesh];
        }
        bind([&](id<MTLBuffer> b, NSUInteger idx) { [en setMeshBuffer:b offset:0 atIndex:idx]; [en useResource:b usage:MTLResourceUsageRead stages:MTLRenderStageMesh]; });
        [en useResource:c.heap usage:MTLResourceUsageRead stages:MTLRenderStageMesh];
        if (out) [en useResource:out usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageMesh];
        [en drawMeshThreadgroups:MTLSizeMake(gx, gy, gz) threadsPerObjectThreadgroup:MTLSizeMake(of ? 1 : 0, of ? 1 : 0, of ? 1 : 0) threadsPerMeshThreadgroup:MTLSizeMake(128, 1, 1)];
        [en endEncoding];
    }
    [cb commit];
    [cb waitUntilCompleted];
    r[@"gpu_ms"] = @((cb.GPUEndTime - cb.GPUStartTime) * 1e3);
    if (cb.status != MTLCommandBufferStatusCompleted) {
        r[@"result"] = @"GPU_ERROR";
        r[@"error"] = cb.error ? cb.error.localizedDescription : @"command buffer failed";
        return r;
    }
    if (out && job[@"out_path"]) {
        NSData *d = [NSData dataWithBytesNoCopy:out.contents length:out_bytes freeWhenDone:NO];
        [d writeToFile:job[@"out_path"] atomically:YES];
    }
    r[@"result"] = @"OK";
    return r;
}

int main() {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        Ctx c;
        c.dev = MTLCreateSystemDefaultDevice();
        if (!c.dev) { printf("{\"result\":\"NO_DEVICE\"}\n"); return 1; }
        c.q = [c.dev newCommandQueue];
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&line, &cap, stdin)) > 0) {
            @autoreleasepool {
                NSData *data = [NSData dataWithBytes:line length:(NSUInteger)n];
                NSDictionary *job = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
                NSDictionary *r = job ? run(c, job) : @{@"result": @"BAD_JOB"};
                NSData *j = [NSJSONSerialization dataWithJSONObject:r options:0 error:nil];
                fwrite(j.bytes, 1, j.length, stdout);
                fputc('\n', stdout);
                fflush(stdout);
            }
        }
        free(line);
    }
    return 0;
}
