// Thread-private scratch probe: one compute pipeline on a fresh device, one dispatch, graphics-ledger delta.
//   tmem_probe lib <metallib> <entry> <tx,ty,tz> <mode> [archive...]
//   tmem_probe synth <array_floats> <mode>
//   tmem_probe list <list.tsv> <archive_dir|-> [compile]
//     list rows: metallib \t entry \t tx,ty,tz \t name; one device, kernels in file order, an indirect zero-group
//     dispatch each; prints the graphics ledger after each (the scratch only grows, so a step names a new maximum).
//   tmem_probe rlist <rlist.tsv> <archive_dir|->
//     render rows (one device, indirect zero-count draws, same ledger lines):
//       ps \t fragment metallib|.metal \t entry \t vertex .metal \t color formats (MTLPixelFormat, comma) \t depth format \t name
//       ms \t mesh metallib \t object metallib|- \t mesh threads \t object threads,payload \t - \t name
//       vs \t vertex metallib|.metal \t entry \t - \t - \t - \t name
// mode: indirect0 = indirect dispatch whose argument buffer holds 0 groups (no thread runs);
//       direct1   = one group, every buffer index bound to a zeroed buffer (own kernels only).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static id<MTLDevice> d;
static double gfx0, fp0;

static void sample(double& fp, double& gfx) {
    task_vm_info_data_t v{};
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&v, &n);
    fp = v.phys_footprint / 1048576.0;
    gfx = v.ledger_tag_graphics_footprint / 1048576.0;
}

static void line(const char* what) {
    double fp, gfx;
    sample(fp, gfx);
    std::printf("%-28s footprint %8.1f graphics %8.1f allocated %8.1f MiB\n", what, fp, gfx, d.currentAllocatedSize / 1048576.0);
    std::fflush(stdout);
}

static int list_mode(int argc, char** argv) {
    FILE* f = std::fopen(argv[2], "r");
    if (!f) return 2;
    const bool compile = argc > 4 && !std::strcmp(argv[4], "compile");
    NSError* e = nil;
    NSMutableArray* archives = [NSMutableArray array];
    if (std::strcmp(argv[3], "-")) {
        NSArray* names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@(argv[3]) error:nil];
        for (NSString* n in names) {
            if (![n hasSuffix:@".metalarchive"]) continue;
            MTLBinaryArchiveDescriptor* ad = [MTLBinaryArchiveDescriptor new];
            ad.url = [NSURL fileURLWithPath:[@(argv[3]) stringByAppendingPathComponent:n]];
            id<MTLBinaryArchive> ar = [d newBinaryArchiveWithDescriptor:ad error:&e];
            if (ar) [archives addObject:ar];
        }
    }
    std::printf("archives %lu\n", (unsigned long)archives.count);
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLBuffer> zero = [d newBufferWithLength:1 << 16 options:MTLResourceStorageModeShared];
    std::memset(zero.contents, 0, zero.length);
    id<MTLBuffer> ind = [d newBufferWithLength:16 options:MTLResourceStorageModeShared];
    std::memset(ind.contents, 0, 16);
    { id<MTLCommandBuffer> cb = [q commandBuffer]; [cb commit]; [cb waitUntilCompleted]; }
    line("start");
    double fp, last;
    sample(fp, last);
    char buf[4096];
    while (std::fgets(buf, sizeof buf, f)) {
        @autoreleasepool {
            if (buf[0] == '#') continue;
            char lib_path[2048], entry[256], tg[64], name[1024];
            if (std::sscanf(buf, "%2047[^\t]\t%255[^\t]\t%63[^\t]\t%1023[^\n]", lib_path, entry, tg, name) != 4) continue;
            unsigned x = 1, y = 1, z = 1;
            std::sscanf(tg, "%u,%u,%u", &x, &y, &z);
            id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(lib_path)] error:&e];
            id<MTLFunction> fn = lib ? [lib newFunctionWithName:@(entry)] : nil;
            if (!fn) { std::printf("ROW\t%s\tno_function\n", name); continue; }
            MTLComputePipelineDescriptor* pd = [MTLComputePipelineDescriptor new];
            pd.computeFunction = fn;
            pd.maxTotalThreadsPerThreadgroup = x * y * z;
            const char* how = "hit";
            auto t0 = std::chrono::steady_clock::now();
            id<MTLComputePipelineState> ps = nil;
            if (archives.count) {
                pd.binaryArchives = archives;
                ps = [d newComputePipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&e];
            }
            if (!ps) {
                how = "miss";
                if (compile) {
                    pd.binaryArchives = nil;
                    ps = [d newComputePipelineStateWithDescriptor:pd options:MTLPipelineOptionNone reflection:nil error:&e];
                    how = "compiled";
                }
            }
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (!ps) { std::printf("ROW\t%s\t%s\t%.0f\n", name, how, ms); std::fflush(stdout); continue; }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps];
            for (int i = 0; i < 31; ++i) [ce setBuffer:zero offset:0 atIndex:i];
            [ce dispatchThreadgroupsWithIndirectBuffer:ind indirectBufferOffset:0 threadsPerThreadgroup:MTLSizeMake(x, y, z)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            double g;
            sample(fp, g);
            std::printf("ROW\t%s\t%s\t%.0f\tstatus %d\tmaxthreads %lu\tgraphics %.1f\tstep %.1f\tfootprint %.1f\n", name, how, ms,
                        int(cb.status), (unsigned long)ps.maxTotalThreadsPerThreadgroup, g, g - last, fp);
            std::fflush(stdout);
            last = g;
        }
    }
    line("end");
    return 0;
}

static id<MTLLibrary> load_lib(const char* path, NSError** e) {
    std::string p = path;
    if (p.size() > 6 && p.substr(p.size() - 6) == ".metal") {
        NSString* src = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:e];
        return src ? [d newLibraryWithSource:src options:nil error:e] : nil;
    }
    return [d newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:e];
}

static MTLVertexFormat vformat(MTLDataType t) {
    switch (t) {
        case MTLDataTypeFloat: return MTLVertexFormatFloat;
        case MTLDataTypeFloat2: return MTLVertexFormatFloat2;
        case MTLDataTypeFloat3: return MTLVertexFormatFloat3;
        case MTLDataTypeFloat4: return MTLVertexFormatFloat4;
        case MTLDataTypeHalf: return MTLVertexFormatHalf;
        case MTLDataTypeHalf2: return MTLVertexFormatHalf2;
        case MTLDataTypeHalf3: return MTLVertexFormatHalf3;
        case MTLDataTypeHalf4: return MTLVertexFormatHalf4;
        case MTLDataTypeInt: return MTLVertexFormatInt;
        case MTLDataTypeInt2: return MTLVertexFormatInt2;
        case MTLDataTypeInt3: return MTLVertexFormatInt3;
        case MTLDataTypeInt4: return MTLVertexFormatInt4;
        case MTLDataTypeUInt: return MTLVertexFormatUInt;
        case MTLDataTypeUInt2: return MTLVertexFormatUInt2;
        case MTLDataTypeUInt3: return MTLVertexFormatUInt3;
        case MTLDataTypeUInt4: return MTLVertexFormatUInt4;
        default: return MTLVertexFormatFloat4;
    }
}

static int rlist_mode(int argc, char** argv) {
    FILE* f = std::fopen(argv[2], "r");
    if (!f) return 2;
    NSError* e = nil;
    NSMutableArray* archives = [NSMutableArray array];
    if (std::strcmp(argv[3], "-")) {
        NSArray* names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@(argv[3]) error:nil];
        for (NSString* n in names) {
            if (![n hasSuffix:@".metalarchive"]) continue;
            MTLBinaryArchiveDescriptor* ad = [MTLBinaryArchiveDescriptor new];
            ad.url = [NSURL fileURLWithPath:[@(argv[3]) stringByAppendingPathComponent:n]];
            id<MTLBinaryArchive> ar = [d newBinaryArchiveWithDescriptor:ad error:&e];
            if (ar) [archives addObject:ar];
        }
    }
    std::printf("archives %lu\n", (unsigned long)archives.count);
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLBuffer> zero = [d newBufferWithLength:1 << 16 options:MTLResourceStorageModeShared];
    std::memset(zero.contents, 0, zero.length);
    id<MTLBuffer> ind = [d newBufferWithLength:64 options:MTLResourceStorageModeShared];
    std::memset(ind.contents, 0, 64);
    NSMutableArray* keep = std::getenv("TMEM_KEEP") ? [NSMutableArray array] : nil;
    id<MTLLibrary> aux = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
                                                  "fragment float4 tmem_f() { return float4(0); }\n" options:nil error:&e];
    { id<MTLCommandBuffer> cb = [q commandBuffer]; [cb commit]; [cb waitUntilCompleted]; }
    line("start");
    double fp, last;
    sample(fp, last);
    char buf[8192];
    while (std::fgets(buf, sizeof buf, f)) {
        @autoreleasepool {
            if (buf[0] == '#') continue;
            char kind[8], a[2048], b[2048], c[2048], fm[512], dfm[64], name[1024];
            if (std::sscanf(buf, "%7[^\t]\t%2047[^\t]\t%2047[^\t]\t%2047[^\t]\t%511[^\t]\t%63[^\t]\t%1023[^\n]", kind, a, b, c, fm, dfm, name) != 7) continue;
            std::string k = kind;
            id<MTLRenderPipelineState> ps = nil;
            std::vector<MTLPixelFormat> colors;
            MTLPixelFormat depth = MTLPixelFormatInvalid;
            unsigned mx = 1, my = 1, mz = 1, ox = 1, payload = 0;
            bool raster = true;
            const char* how = "hit";
            auto t0 = std::chrono::steady_clock::now();
            auto make = [&](auto* pd, auto fn) {
                if (archives.count) {
                    pd.binaryArchives = archives;
                    ps = fn(pd, MTLPipelineOptionFailOnBinaryArchiveMiss);
                }
                if (!ps) {
                    how = "compiled";
                    pd.binaryArchives = nil;
                    ps = fn(pd, MTLPipelineOptionNone);
                }
            };
            if (k == "ps" || k == "vs") {
                id<MTLLibrary> lib = load_lib(a, &e);
                id<MTLFunction> fn = lib ? [lib newFunctionWithName:@(b)] : nil;
                if (!fn) { std::printf("ROW\t%s\tno_function\t%s\n", name, e ? e.localizedDescription.UTF8String : ""); std::fflush(stdout); continue; }
                MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
                if (k == "ps") {
                    id<MTLLibrary> vl = load_lib(c, &e);
                    id<MTLFunction> vf = vl ? [vl newFunctionWithName:@"tmem_v"] : nil;
                    if (!vf) { std::printf("ROW\t%s\tno_vertex\t%s\n", name, e ? e.localizedDescription.UTF8String : ""); std::fflush(stdout); continue; }
                    pd.vertexFunction = vf;
                    pd.fragmentFunction = fn;
                    char* save = nullptr;
                    for (char* t = strtok_r(fm, ",", &save); t; t = strtok_r(nullptr, ",", &save)) if (std::atoi(t)) colors.push_back(MTLPixelFormat(std::atoi(t)));
                    depth = MTLPixelFormat(std::atoi(dfm));
                } else {
                    pd.vertexFunction = fn;
                    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
                    NSUInteger off = 0;
                    for (MTLVertexAttribute* va in fn.vertexAttributes) {
                        if (!va.active) continue;
                        vd.attributes[va.attributeIndex].format = vformat(va.attributeType);
                        vd.attributes[va.attributeIndex].offset = off;
                        vd.attributes[va.attributeIndex].bufferIndex = 30;
                        off += 16;
                    }
                    vd.layouts[30].stride = off ? off : 16;
                    pd.vertexDescriptor = vd;
                }
                for (size_t i = 0; i < colors.size(); ++i) pd.colorAttachments[i].pixelFormat = colors[i];
                if (depth != MTLPixelFormatInvalid) {
                    pd.depthAttachmentPixelFormat = depth;
                    if (depth == MTLPixelFormatDepth32Float_Stencil8) pd.stencilAttachmentPixelFormat = depth;
                }
                make(pd, [&](MTLRenderPipelineDescriptor* x, MTLPipelineOption o) {
                    return [d newRenderPipelineStateWithDescriptor:x options:o reflection:nil error:&e];
                });
            } else if (k == "cs") {
                id<MTLLibrary> lib = load_lib(a, &e);
                id<MTLFunction> fn = lib ? [lib newFunctionWithName:@(b)] : nil;
                unsigned x = 1, y = 1, z = 1;
                std::sscanf(c, "%u,%u,%u", &x, &y, &z);
                MTLComputePipelineDescriptor* pd = [MTLComputePipelineDescriptor new];
                pd.computeFunction = fn;
                pd.maxTotalThreadsPerThreadgroup = x * y * z;
                id<MTLComputePipelineState> cp = fn ? [d newComputePipelineStateWithDescriptor:pd options:MTLPipelineOptionNone reflection:nil error:&e] : nil;
                if (!cp) { std::printf("ROW\t%s\tfailed\n", name); std::fflush(stdout); continue; }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:cp];
                [ce dispatchThreadgroupsWithIndirectBuffer:ind indirectBufferOffset:0 threadsPerThreadgroup:MTLSizeMake(x, y, z)];
                [ce endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                double g;
                sample(fp, g);
                std::printf("ROW\t%s\tcompiled\t0\tstatus %d\tkind cs\tgraphics %.1f\tstep %.1f\tfootprint %.1f\n", name, int(cb.status), g, g - last, fp);
                std::fflush(stdout);
                last = g;
                continue;
            } else if (k == "ms") {
                id<MTLLibrary> ml = load_lib(a, &e);
                id<MTLFunction> mf = ml ? [ml newFunctionWithName:@"main0"] : nil;
                id<MTLFunction> of = nil;
                if (std::strcmp(b, "-")) {
                    id<MTLLibrary> ol = load_lib(b, &e);
                    of = ol ? [ol newFunctionWithName:@"main0"] : nil;
                }
                if (!mf) { std::printf("ROW\t%s\tno_function\n", name); std::fflush(stdout); continue; }
                std::sscanf(c, "%u,%u,%u", &mx, &my, &mz);
                std::sscanf(fm, "%u,%u", &ox, &payload);
                MTLMeshRenderPipelineDescriptor* pd = [MTLMeshRenderPipelineDescriptor new];
                pd.meshFunction = mf;
                pd.objectFunction = of;
                pd.fragmentFunction = [aux newFunctionWithName:@"tmem_f"];
                if (of) pd.payloadMemoryLength = payload;
                colors.push_back(MTLPixelFormatRGBA16Float);
                pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
                pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
                depth = MTLPixelFormatDepth32Float;
                pd.binaryArchives = archives.count ? archives : nil;
                if (archives.count) ps = [d newRenderPipelineStateWithMeshDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&e];
                if (!ps) {
                    how = "compiled";
                    pd.binaryArchives = nil;
                    ps = [d newRenderPipelineStateWithMeshDescriptor:pd options:MTLPipelineOptionNone reflection:nil error:&e];
                }
            }
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ps && keep) [keep addObject:ps];
            if (!ps) { std::printf("ROW\t%s\tfailed\t%.0f\t%s\n", name, ms, e ? e.localizedDescription.UTF8String : ""); std::fflush(stdout); continue; }
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            for (size_t i = 0; i < colors.size(); ++i) {
                MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:colors[i] width:4 height:4 mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget;
                td.storageMode = MTLStorageModePrivate;
                rp.colorAttachments[i].texture = [d newTextureWithDescriptor:td];
                rp.colorAttachments[i].loadAction = MTLLoadActionDontCare;
            }
            if (depth != MTLPixelFormatInvalid) {
                MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth width:4 height:4 mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget;
                td.storageMode = MTLStorageModePrivate;
                id<MTLTexture> dt = [d newTextureWithDescriptor:td];
                rp.depthAttachment.texture = dt;
                if (depth == MTLPixelFormatDepth32Float_Stencil8) rp.stencilAttachment.texture = dt;
            }
            if (colors.empty() && depth == MTLPixelFormatInvalid) {
                rp.renderTargetWidth = 4;
                rp.renderTargetHeight = 4;
                rp.defaultRasterSampleCount = 1;
            }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:ps];
            for (int i = 0; i < 31; ++i) {
                [re setVertexBuffer:zero offset:0 atIndex:i];
                [re setFragmentBuffer:zero offset:0 atIndex:i];
                if (k == "ms") { [re setMeshBuffer:zero offset:0 atIndex:i]; [re setObjectBuffer:zero offset:0 atIndex:i]; }
            }
            if (k == "ms") {
                [re drawMeshThreadgroupsWithIndirectBuffer:ind indirectBufferOffset:0 threadsPerObjectThreadgroup:MTLSizeMake(ox, 1, 1)
                                threadsPerMeshThreadgroup:MTLSizeMake(mx, my, mz)];
            } else {
                [re drawPrimitives:raster ? MTLPrimitiveTypeTriangle : MTLPrimitiveTypePoint indirectBuffer:ind indirectBufferOffset:0];
            }
            [re endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            double g;
            sample(fp, g);
            std::printf("ROW\t%s\t%s\t%.0f\tstatus %d\tkind %s\tgraphics %.1f\tstep %.1f\tfootprint %.1f\n", name, how, ms,
                        int(cb.status), kind, g, g - last, fp);
            std::fflush(stdout);
            last = g;
        }
    }
    line("end");
    return 0;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) { std::fprintf(stderr, "usage: see source\n"); return 2; }
        d = MTLCreateSystemDefaultDevice();
        if (!std::strcmp(argv[1], "list")) return argc < 4 ? 2 : list_mode(argc, argv);
        if (!std::strcmp(argv[1], "rlist")) return argc < 4 ? 2 : rlist_mode(argc, argv);
        const bool synth = !std::strcmp(argv[1], "synth");
        NSError* e = nil;
        id<MTLLibrary> lib;
        std::string entry = "k";
        MTLSize tpg = MTLSizeMake(64, 1, 1);
        const char* mode;
        int first_archive = argc;
        if (synth) {
            const int arr = std::atoi(argv[2]);
            mode = argc > 3 ? argv[3] : "indirect0";
            std::string a = std::to_string(arr);
            std::string src = "#include <metal_stdlib>\nusing namespace metal;\n"
                "kernel void k(device float* o [[buffer(0)]], uint t [[thread_position_in_grid]]) {\n";
            if (arr) src += "  float a[" + a + "]; for (int j = 0; j < " + a + "; ++j) a[j] = o[(t * 7 + j) & 1023];"
                " float r = 0; for (int j = 0; j < " + a + "; ++j) r += a[(j * 13 + t) % " + a + "];\n  o[t & 1023] = r;\n";
            else src += "  o[t & 1023] = float(t);\n";
            src += "}\n";
            lib = [d newLibraryWithSource:@(src.c_str()) options:nil error:&e];
        } else {
            if (argc < 6) { std::fprintf(stderr, "usage: see source\n"); return 2; }
            lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&e];
            entry = argv[3];
            unsigned x = 1, y = 1, z = 1;
            std::sscanf(argv[4], "%u,%u,%u", &x, &y, &z);
            tpg = MTLSizeMake(x, y, z);
            mode = argv[5];
            first_archive = 6;
        }
        if (!lib) { std::printf("library: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLFunction> fn = [lib newFunctionWithName:@(entry.c_str())];
        if (!fn) { std::printf("no entry %s\n", entry.c_str()); return 1; }
        line("library");
        MTLComputePipelineDescriptor* pd = [MTLComputePipelineDescriptor new];
        pd.computeFunction = fn;
        NSMutableArray* archives = [NSMutableArray array];
        for (int i = first_archive; i < argc; ++i) {
            MTLBinaryArchiveDescriptor* ad = [MTLBinaryArchiveDescriptor new];
            ad.url = [NSURL fileURLWithPath:@(argv[i])];
            id<MTLBinaryArchive> ar = [d newBinaryArchiveWithDescriptor:ad error:&e];
            if (ar) [archives addObject:ar];
        }
        if (archives.count) pd.binaryArchives = archives;
        auto t0 = std::chrono::steady_clock::now();
        id<MTLComputePipelineState> ps = [d newComputePipelineStateWithDescriptor:pd options:MTLPipelineOptionNone reflection:nil error:&e];
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!ps) { std::printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
        std::printf("pipeline %.0f ms, max threads %lu, width %lu, static tg %lu\n", ms, (unsigned long)ps.maxTotalThreadsPerThreadgroup,
                    (unsigned long)ps.threadExecutionWidth, (unsigned long)ps.staticThreadgroupMemoryLength);
        if (tpg.width * tpg.height * tpg.depth > ps.maxTotalThreadsPerThreadgroup) { std::printf("group too large\n"); return 1; }
        id<MTLCommandQueue> q = [d newCommandQueue];
        id<MTLBuffer> zero = [d newBufferWithLength:1 << 20 options:MTLResourceStorageModeShared];
        std::memset(zero.contents, 0, zero.length);
        id<MTLBuffer> ind = [d newBufferWithLength:16 options:MTLResourceStorageModeShared];
        std::memset(ind.contents, 0, 16);
        { id<MTLCommandBuffer> cb = [q commandBuffer]; [cb commit]; [cb waitUntilCompleted]; }
        line("before dispatch");
        sample(fp0, gfx0);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps];
        for (int i = 0; i < 31; ++i) [ce setBuffer:zero offset:0 atIndex:i];
        if (!std::strcmp(mode, "direct1")) {
            if (!synth) { std::printf("direct1 is for own kernels only\n"); return 1; }
            [ce dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:tpg];
        } else {
            [ce dispatchThreadgroupsWithIndirectBuffer:ind indirectBufferOffset:0 threadsPerThreadgroup:tpg];
        }
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        double fp, gfx;
        sample(fp, gfx);
        char t[96];
        std::snprintf(t, sizeof t, "after %s (status %d)", mode, int(cb.status));
        line(t);
        std::printf("RESULT graphics_delta_mib %.1f footprint_delta_mib %.1f status %d error %s\n", gfx - gfx0, fp - fp0,
                    int(cb.status), cb.error ? cb.error.localizedDescription.UTF8String : "-");
    }
    return 0;
}
