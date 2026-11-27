// Thread-private scratch probe: one compute pipeline on a fresh device, one dispatch, graphics-ledger delta.
//   tmem_probe lib <metallib> <entry> <tx,ty,tz> <mode> [archive...]
//   tmem_probe synth <array_floats> <mode>
//   tmem_probe list <list.tsv> <archive_dir|-> [compile]
//     list rows: metallib \t entry \t tx,ty,tz \t name; one device, kernels in file order, an indirect zero-group
//     dispatch each; prints the graphics ledger after each (the scratch only grows, so a step names a new maximum).
// mode: indirect0 = indirect dispatch whose argument buffer holds 0 groups (no thread runs);
//       direct1   = one group, every buffer index bound to a zeroed buffer (own kernels only).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

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

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) { std::fprintf(stderr, "usage: see source\n"); return 2; }
        d = MTLCreateSystemDefaultDevice();
        if (!std::strcmp(argv[1], "list")) return argc < 4 ? 2 : list_mode(argc, argv);
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
