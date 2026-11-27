// Metal's own shader cache on KosmicKrisp's path (review round 3, B3): KosmicKrisp 26.3.1's pipeline cache keeps MSL
// source, and a cache hit still compiles it (kk_deserialize_shader -> kk_compile_compute_pipeline: MTL4Compiler
// newLibraryWithDescriptor + newComputePipelineStateWithDescriptor, fast math, MSL 4.0). This times that compile for
// MSL taken from a session file, as-is, with a unique comment appended (no entry in Metal's cache can match), and that
// unique source again.
//   metal_cache_probe DIR [N]      DIR holds NNNN.metal + NNNN.entry pairs; N spread over the size order (default 12)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

static double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static double compile(id<MTL4Compiler> compiler, NSString* source, NSString* entry, bool* ok) {
    const double t0 = now_ms();
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion4_0;
    opts.mathMode = MTLMathModeFast;
    opts.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsFast;
    MTL4LibraryDescriptor* ld = [MTL4LibraryDescriptor new];
    ld.source = source;
    ld.options = opts;
    NSError* error = nil;
    id<MTLLibrary> lib = [compiler newLibraryWithDescriptor:ld error:&error];
    *ok = lib != nil;
    if (lib) {
        MTL4LibraryFunctionDescriptor* fd = [MTL4LibraryFunctionDescriptor new];
        fd.name = entry;
        fd.library = lib;
        MTL4ComputePipelineDescriptor* pd = [MTL4ComputePipelineDescriptor new];
        pd.computeFunctionDescriptor = fd;
        id<MTLComputePipelineState> pso = [compiler newComputePipelineStateWithDescriptor:pd compilerTaskOptions:nil
                                                                                     error:&error];
        *ok = pso != nil;
    }
    if (!*ok) std::fprintf(stderr, "compile failed: %s\n", error ? error.localizedDescription.UTF8String : "?");
    return now_ms() - t0;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) {
            std::fprintf(stderr, "usage: metal_cache_probe DIR [N]\n");
            return 2;
        }
        const int want = argc > 2 ? std::atoi(argv[2]) : 12;
        NSString* dir = [NSString stringWithUTF8String:argv[1]];
        NSArray<NSString*>* files = [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]
            filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.metal'"]];
        struct Item {
            NSString* source;
            NSString* entry;
            NSString* name;
        };
        std::vector<Item> items;
        for (NSString* f in files) {
            NSString* path = [dir stringByAppendingPathComponent:f];
            NSString* src = [NSString stringWithContentsOfFile:path encoding:NSASCIIStringEncoding error:nil];
            NSString* entry = [NSString stringWithContentsOfFile:[[path stringByDeletingPathExtension]
                                                                     stringByAppendingPathExtension:@"entry"]
                                                         encoding:NSASCIIStringEncoding error:nil];
            if (src && entry) items.push_back({src, entry, f});
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.source.length < b.source.length; });
        std::vector<Item> sample;
        for (int i = 0; i < want && !items.empty(); ++i)
            sample.push_back(items[std::size_t((double(i) + 0.5) / want * items.size())]);

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSError* error = nil;
        id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
        if (!compiler) {
            std::fprintf(stderr, "no MTL4Compiler: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        std::printf("%-12s %9s %10s %10s %12s\n", "msl", "bytes", "as-is ms", "unique ms", "unique again");
        double sum[3] = {};
        int failed = 0;
        const std::string tag = std::to_string(long(now_ms()));
        for (const Item& it : sample) {
            NSString* unique = [it.source stringByAppendingFormat:@"\n// metal_cache_probe %s %@\n", tag.c_str(), it.name];
            bool ok[3];
            const double t[3] = {compile(compiler, it.source, it.entry, &ok[0]), compile(compiler, unique, it.entry, &ok[1]),
                                 compile(compiler, unique, it.entry, &ok[2])};
            failed += !ok[0] + !ok[1] + !ok[2];
            for (int k = 0; k < 3; ++k) sum[k] += t[k];
            std::printf("%-12s %9lu %10.1f %10.1f %12.1f\n", it.name.UTF8String, (unsigned long)it.source.length, t[0], t[1],
                        t[2]);
        }
        const double n = double(sample.size());
        std::printf("mean over %zu: as-is %.1f ms, unique %.1f ms, unique again %.1f ms; %d compiles failed\n",
                    sample.size(), sum[0] / n, sum[1] / n, sum[2] / n, failed);
        return failed ? 1 : 0;
    }
}
