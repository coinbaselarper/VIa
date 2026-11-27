// What a compute pipeline made from a binary archive costs: one device, the archives of a directory opened, then
// every row's pipeline created with FailOnBinaryArchiveMiss and KEPT (no dispatch: no thread runs), with Metal's
// allocated size, the graphics ledger, the process footprint and machine wired after each row.
//   wired2_pipemem <list.tsv> <archive_dir> [release]
//     rows: metallib \t entry \t tx,ty,tz \t name   (tmem_probe list rows)
//   release: after all rows, drop every pipeline and the libraries, then report again; release-all: the archives too.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <cstdio>
#include <cstring>

static double wired_mib() {
    vm_statistics64_data_t vs{};
    mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &n);
    return double(vs.wire_count) * vm_kernel_page_size / 1048576.0;
}

static void sample(double& fp, double& gfx) {
    task_vm_info_data_t v{};
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&v, &n);
    fp = v.phys_footprint / 1048576.0;
    gfx = v.ledger_tag_graphics_footprint / 1048576.0;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) return 2;
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        NSError* e = nil;
        NSMutableArray* archives = [NSMutableArray array];
        NSArray* names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@(argv[2]) error:nil];
        for (NSString* n in names) {
            if (![n hasSuffix:@".metalarchive"]) continue;
            MTLBinaryArchiveDescriptor* ad = [MTLBinaryArchiveDescriptor new];
            ad.url = [NSURL fileURLWithPath:[@(argv[2]) stringByAppendingPathComponent:n]];
            id<MTLBinaryArchive> ar = [d newBinaryArchiveWithDescriptor:ad error:&e];
            if (ar) [archives addObject:ar];
        }
        double fp, gfx;
        sample(fp, gfx);
        const double a0 = d.currentAllocatedSize / 1048576.0, g0 = gfx, f0 = fp, w0 = wired_mib();
        std::printf("start archives %lu allocated %.1f graphics %.1f footprint %.1f wired %.0f MiB\n",
                    (unsigned long)archives.count, a0, g0, f0, w0);
        NSMutableArray* keep = [NSMutableArray array];
        NSMutableArray* libs = [NSMutableArray array];
        FILE* f = std::fopen(argv[1], "r");
        if (!f) return 2;
        char buf[4096];
        int hits = 0, misses = 0;
        while (std::fgets(buf, sizeof buf, f)) {
            @autoreleasepool {
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
                pd.binaryArchives = archives;
                const double before = d.currentAllocatedSize / 1048576.0;
                id<MTLComputePipelineState> ps =
                    [d newComputePipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&e];
                if (!ps) { ++misses; std::printf("ROW\t%s\tmiss\n", name); continue; }
                ++hits;
                [keep addObject:ps];
                [libs addObject:lib];
                sample(fp, gfx);
                std::printf("ROW\t%s\thit\tallocated %.3f\tstep %.3f\tgraphics %.1f\tfootprint %.1f\n", name,
                            d.currentAllocatedSize / 1048576.0, d.currentAllocatedSize / 1048576.0 - before, gfx, fp);
            }
        }
        sample(fp, gfx);
        const double a1 = d.currentAllocatedSize / 1048576.0, w1 = wired_mib();
        std::printf("kept %d pipelines (%d misses): allocated +%.1f MiB (%.3f a pipeline), graphics +%.1f, footprint +%.1f, "
                    "machine wired +%.0f MiB\n", hits, misses, a1 - a0, hits ? (a1 - a0) / hits : 0.0, gfx - g0, fp - f0, w1 - w0);
        if (argc > 3 && (!std::strcmp(argv[3], "release") || !std::strcmp(argv[3], "release-all"))) {
            [keep removeAllObjects];
            [libs removeAllObjects];
            if (!std::strcmp(argv[3], "release-all")) [archives removeAllObjects];
            sample(fp, gfx);
            std::printf("released: allocated +%.1f MiB, graphics +%.1f, footprint +%.1f, machine wired +%.0f MiB\n",
                        d.currentAllocatedSize / 1048576.0 - a0, gfx - g0, fp - f0, wired_mib() - w0);
        }
    }
    return 0;
}
