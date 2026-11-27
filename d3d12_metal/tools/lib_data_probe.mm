// lib_data_probe: what a Metal library made from metallib bytes keeps in the process, by how the bytes are given.
//   lib_data_probe MODE LIST [N]
// MODE: copy (dispatch_data DEFAULT destructor, port/metal's create_library_metallib before round 15), mmap (the file
// mapped read-only, dispatch_data with the MUNMAP destructor), url (newLibraryWithURL), cachefile (bytes read, written
// to a content-addressed file, that file mapped). LIST: one metallib path a line. Prints footprint and Metal allocated
// before/after loading, after touching every function (newFunctionWithName), and after a second footprint read 2 s
// later.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static double footprint_mib() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
    return info.phys_footprint / 1048576.0;
}

static dispatch_data_t map_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return nil;
    struct stat sb {};
    ::fstat(fd, &sb);
    void* p = ::mmap(nullptr, size_t(sb.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) return nil;
    return dispatch_data_create(p, size_t(sb.st_size), nullptr, DISPATCH_DATA_DESTRUCTOR_MUNMAP);
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) {
            std::fprintf(stderr, "usage: lib_data_probe copy|mmap|url|cachefile LIST [N]\n");
            return 2;
        }
        const std::string mode = argv[1];
        const long limit = argc > 3 ? std::atol(argv[3]) : -1;
        std::vector<std::string> paths;
        std::ifstream in(argv[2]);
        for (std::string l; std::getline(in, l) && (limit < 0 || long(paths.size()) < limit);)
            if (!l.empty()) paths.push_back(l);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        const double f0 = footprint_mib(), a0 = dev.currentAllocatedSize / 1048576.0;
        NSMutableArray* libs = [NSMutableArray array];
        double bytes = 0;
        const std::string cache_dir = "/tmp/lib_data_probe_cache";
        ::mkdir(cache_dir.c_str(), 0755);
        for (const auto& p : paths) {
            NSError* err = nil;
            id<MTLLibrary> lib = nil;
            if (mode == "url") {
                lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(p.c_str())] error:&err];
            } else if (mode == "mmap") {
                dispatch_data_t d = map_file(p);
                if (d) lib = [dev newLibraryWithData:d error:&err];
            } else {
                std::ifstream f(p, std::ios::binary);
                std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                if (mode == "copy") {
                    dispatch_data_t d = dispatch_data_create(b.data(), b.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                    lib = [dev newLibraryWithData:d error:&err];
                } else {
                    std::uint64_t h = 1469598103934665603ull;
                    for (std::uint8_t c : b) h = (h ^ c) * 1099511628211ull;
                    char name[64];
                    std::snprintf(name, sizeof name, "/%016llx.metallib", (unsigned long long)h);
                    const std::string cp = cache_dir + name;
                    if (::access(cp.c_str(), R_OK) != 0) {
                        std::ofstream o(cp + ".tmp", std::ios::binary);
                        o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
                        o.close();
                        std::rename((cp + ".tmp").c_str(), cp.c_str());
                    }
                    dispatch_data_t d = map_file(cp);
                    if (d) lib = [dev newLibraryWithData:d error:&err];
                }
            }
            struct stat sb {};
            ::stat(p.c_str(), &sb);
            bytes += double(sb.st_size);
            if (lib) [libs addObject:lib];
        }
        const double f1 = footprint_mib(), a1 = dev.currentAllocatedSize / 1048576.0;
        NSMutableArray* fns = [NSMutableArray array];
        for (id<MTLLibrary> lib in libs)
            for (NSString* n in lib.functionNames) {
                id<MTLFunction> fn = [lib newFunctionWithName:n];
                if (fn) [fns addObject:fn];
            }
        const double f2 = footprint_mib(), a2 = dev.currentAllocatedSize / 1048576.0;
        sleep(2);
        const double f3 = footprint_mib();
        std::printf("mode %s: %zu libraries (%lu loaded, %.1f MiB on disk), %lu functions; footprint %.1f -> %.1f (+%.1f) "
                    "-> functions %.1f (+%.1f) -> 2 s later %.1f; Metal allocated %.1f -> %.1f -> %.1f MiB\n",
                    mode.c_str(), paths.size(), (unsigned long)libs.count, bytes / 1048576.0, (unsigned long)fns.count, f0,
                    f1, f1 - f0, f2, f2 - f1, f3, a0, a1, a2);
    }
    return 0;
}
