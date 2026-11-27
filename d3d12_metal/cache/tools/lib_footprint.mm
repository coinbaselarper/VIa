// lib_footprint: footprint of only loading the Metal libraries a pipeline spec list names (no pipelines), to split
// prepare_shaders' footprint into libraries and pipelines.
//   lib_footprint CATALOG.tsv SPECS [N]   (N: stop after the first N spec lines)
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static double footprint_mib() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
    return info.phys_footprint / 1048576.0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: lib_footprint CATALOG.tsv SPECS [N]\n");
        return 2;
    }
    const std::string cat = argv[1];
    const long limit = argc > 3 ? std::atol(argv[3]) : -1;
    std::ifstream in(cat);
    std::string line, root;
    std::map<std::string, std::vector<std::string>> libs;
    while (std::getline(in, line)) {
        if (line.rfind("#", 0) == 0) {
            auto p = line.find("root=");
            if (p != std::string::npos && root.empty()) {
                std::string r = line.substr(p + 5);
                r = r.substr(0, r.find_first_of(" \t"));
                root = cat.substr(0, cat.find_last_of('/') + 1) + r + "/";
            }
            continue;
        }
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, '\t')) f.push_back(c);
        if (f.size() < 5) continue;
        std::vector<std::string> paths;
        for (const auto& x : f)
            if (x.size() > 9 && x.compare(x.size() - 9, 9, ".metallib") == 0) paths.push_back(x);
        libs[f[0]] = paths;
    }
    std::ifstream sp(argv[2]);
    const std::regex name(R"(\b(?:vs|ps|cs|ms|as)=([^@ ]+)@)");
    std::set<std::string> want_files;
    long n = 0;
    std::size_t unknown = 0;
    while (std::getline(sp, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (limit >= 0 && n >= limit) break;
        ++n;
        for (std::sregex_iterator it(line.begin(), line.end(), name), e; it != e; ++it) {
            auto l = libs.find((*it)[1].str());
            if (l == libs.end()) {
                ++unknown;
                continue;
            }
            for (const auto& p : l->second) want_files.insert(p);
        }
    }
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        const double f0 = footprint_mib();
        NSMutableArray* kept = [NSMutableArray array];
        std::uint64_t bytes = 0;
        std::size_t failed = 0;
        for (const auto& p : want_files) {
            NSData* d = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:(root + p).c_str()]];
            if (!d) {
                ++failed;
                continue;
            }
            bytes += d.length;
            dispatch_data_t dd = dispatch_data_create(d.bytes, d.length, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            NSError* err = nil;
            id<MTLLibrary> lib = [dev newLibraryWithData:dd error:&err];
            if (lib) [kept addObject:lib]; else ++failed;
        }
        std::printf("%ld spec lines, %zu metallib files (%.1f MiB on disk), %zu not loaded, %zu names not in the catalog; "
                    "footprint %.0f -> %.0f MiB (+%.0f) with all libraries held; device allocated %.0f MiB\n",
                    n, want_files.size(), bytes / 1048576.0, failed, unknown, f0, footprint_mib(), footprint_mib() - f0,
                    dev.currentAllocatedSize / 1048576.0);
    }
    return 0;
}
