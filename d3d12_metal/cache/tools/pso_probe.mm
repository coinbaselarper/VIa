// pso_probe: what a slow world pipeline is and where its creation time goes, one compute shader at a time.
//   pso_probe [--catalog M] [--variants reflect,none,archive] NAME...
// For each store shader: metallib size, functions and function constants, then the compute pipeline created with
// each variant: "reflect" = the backend's options (BindingInfo | BufferTypeInfo), "none" = no reflection,
// "archive" = addComputePipelineFunctionsWithDescriptor into an empty archive, then a pipeline from it with
// FailOnBinaryArchiveMiss (the add-first order), "file" = a pipeline from --archive-file F (for example metal-tt's
// output) with FailOnBinaryArchiveMiss. Run with MTL_SHADER_CACHE_SIZE=0 for cold numbers.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <gta6rt_shaderc_msl/catalog.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace sc = gta6rt::shaderc_msl;

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    @autoreleasepool {
        std::string catalog_path = std::string(GTA6RT_PORT_ROOT) + "/build/shaderc_msl/runs/main/manifest.tsv";
        std::vector<std::string> variants{"reflect", "none"};
        std::vector<std::string> names;
        std::string archive_file;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--catalog" && i + 1 < argc) catalog_path = argv[++i];
            else if (a == "--archive-file" && i + 1 < argc) archive_file = argv[++i];
            else if (a == "--variants" && i + 1 < argc) {
                variants.clear();
                std::stringstream ss(argv[++i]);
                for (std::string v; std::getline(ss, v, ',');) variants.push_back(v);
            } else names.push_back(a);
        }
        sc::Catalog catalog;
        std::string error;
        if (!catalog.load(catalog_path, &error)) {
            std::fprintf(stderr, "catalog: %s\n", error.c_str());
            return 1;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const char* cache = std::getenv("MTL_SHADER_CACHE_SIZE");
        std::printf("device %s, MTL_SHADER_CACHE_SIZE=%s\n", device.name.UTF8String, cache ? cache : "(unset: on)");
        for (const std::string& name : names) {
            const sc::ShaderEntry* e = catalog.find(name);
            if (!e) {
                std::printf("%s: not in the store\n", name.c_str());
                continue;
            }
            const std::string path = catalog.library_path(*e);
            std::ifstream in(path, std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            NSError* err = nil;
            double t0 = now_s();
            id<MTLLibrary> lib = [device newLibraryWithData:data error:&err];
            const double lib_s = now_s() - t0;
            if (!lib) {
                std::printf("%s: library: %s\n", name.c_str(), err.localizedDescription.UTF8String);
                continue;
            }
            NSString* entry = lib.functionNames.firstObject;
            for (NSString* n in lib.functionNames)
                if ([n isEqualToString:@"main0"]) entry = n;
            t0 = now_s();
            id<MTLFunction> fn = [lib newFunctionWithName:entry];
            const double fn_s = now_s() - t0;
            std::printf("%s: metallib %zu B (%s), %lu functions, entry %s, %lu function constants, library %.3f s, "
                        "function %.3f s\n",
                        name.c_str(), bytes.size(), path.c_str(), (unsigned long)lib.functionNames.count, entry.UTF8String,
                        (unsigned long)fn.functionConstantsDictionary.count, lib_s, fn_s);
            for (const std::string& v : variants) {
                MTLComputePipelineDescriptor* pd = [MTLComputePipelineDescriptor new];
                pd.computeFunction = fn;
                MTLComputePipelineReflection* refl = nil;
                id<MTLComputePipelineState> pso = nil;
                double add_s = 0;
                t0 = now_s();
                if (v == "file") {
                    MTLBinaryArchiveDescriptor* ad = [MTLBinaryArchiveDescriptor new];
                    ad.url = [NSURL fileURLWithPath:@(archive_file.c_str())];
                    const double a0 = now_s();
                    id<MTLBinaryArchive> ar = [device newBinaryArchiveWithDescriptor:ad error:&err];
                    add_s = now_s() - a0;
                    if (ar) {
                        pd.binaryArchives = @[ ar ];
                        pso = [device newComputePipelineStateWithDescriptor:pd
                                                                    options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo |
                                                                            MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                 reflection:&refl
                                                                      error:&err];
                    }
                } else if (v == "archive") {
                    id<MTLBinaryArchive> ar = [device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&err];
                    const double a0 = now_s();
                    const BOOL ok = [ar addComputePipelineFunctionsWithDescriptor:pd error:&err];
                    add_s = now_s() - a0;
                    if (ok) {
                        pd.binaryArchives = @[ ar ];
                        pso = [device newComputePipelineStateWithDescriptor:pd
                                                                    options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo |
                                                                            MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                 reflection:&refl
                                                                      error:&err];
                    }
                } else {
                    const MTLPipelineOption o = v == "none" ? MTLPipelineOptionNone
                                                            : MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo;
                    pso = [device newComputePipelineStateWithDescriptor:pd options:o reflection:(v == "none" ? nil : &refl) error:&err];
                }
                const double dt = now_s() - t0;
                std::printf("  %-8s %8.2f s%s%s  threadExecutionWidth %lu, maxThreads %lu, static threadgroup memory %lu B\n",
                            v.c_str(), dt, v == "archive" ? (" (add " + std::to_string(add_s) + " s)").c_str() : v == "file" ? (" (open " + std::to_string(add_s) + " s)").c_str() : "",
                            pso ? "" : (std::string(" FAILED: ") + err.localizedDescription.UTF8String).c_str(),
                            pso ? (unsigned long)pso.threadExecutionWidth : 0ul,
                            pso ? (unsigned long)pso.maxTotalThreadsPerThreadgroup : 0ul,
                            pso ? (unsigned long)pso.staticThreadgroupMemoryLength : 0ul);
                std::fflush(stdout);
            }
        }
    }
    return 0;
}
