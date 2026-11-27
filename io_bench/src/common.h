#pragma once
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace iob {

inline double now_s() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return static_cast<double>(mach_absolute_time()) * tb.numer / tb.denom * 1e-9;
}

struct Cpu {
    double user = 0, sys = 0;
    double total() const { return user + sys; }
};

inline Cpu cpu_now() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return {ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6};
}

struct VmSnap {
    uint64_t page = 16384;
    uint64_t free = 0, active = 0, inactive = 0, speculative = 0, wired = 0, file = 0, anon = 0, compressor = 0;
    uint64_t pageouts = 0, swapouts = 0, swapins = 0, compressions = 0, decompressions = 0, pageins = 0;
    uint64_t mib(uint64_t pages) const { return pages * page >> 20; }
};

inline VmSnap vm_snap() {
    VmSnap s;
    vm_size_t ps = 16384;
    host_page_size(mach_host_self(), &ps);
    s.page = ps;
    vm_statistics64_data_t v{};
    mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&v), &c);
    s.free = v.free_count;
    s.active = v.active_count;
    s.inactive = v.inactive_count;
    s.speculative = v.speculative_count;
    s.wired = v.wire_count;
    s.file = v.external_page_count;
    s.anon = v.internal_page_count;
    s.compressor = v.compressor_page_count;
    s.pageouts = v.pageouts;
    s.swapouts = v.swapouts;
    s.swapins = v.swapins;
    s.compressions = v.compressions;
    s.decompressions = v.decompressions;
    s.pageins = v.pageins;
    return s;
}

inline int pressure_level() {
    int v = -1;
    size_t n = sizeof v;
    sysctlbyname("kern.memorystatus_vm_pressure_level", &v, &n, nullptr, 0);
    return v;
}

struct Pct {
    double mean = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

inline Pct percentiles(std::vector<double> v) {
    Pct p;
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<size_t>(q * v.size()))]; };
    double sum = 0;
    for (double x : v) sum += x;
    p.mean = sum / v.size();
    p.p50 = at(0.50);
    p.p90 = at(0.90);
    p.p99 = at(0.99);
    p.p999 = at(0.999);
    p.max = v.back();
    return p;
}

inline uint64_t file_size(int fd) {
    struct stat st{};
    fstat(fd, &st);
    return static_cast<uint64_t>(st.st_size);
}

inline size_t host_page() {
    static size_t p = static_cast<size_t>(getpagesize());
    return p;
}

inline void evict_range(int fd, uint64_t off, uint64_t len) {
    const size_t ps = std::max<size_t>(host_page(), 16384);
    uint64_t a = off / ps * ps, b = (off + len + ps - 1) / ps * ps;
    b = std::min<uint64_t>(b, (file_size(fd) + ps - 1) / ps * ps);
    if (b <= a) return;
    void* m = mmap(nullptr, b - a, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(a));
    if (m == MAP_FAILED) return;
    msync(m, b - a, MS_INVALIDATE);
    munmap(m, b - a);
}

inline double resident_fraction(int fd, uint64_t off, uint64_t len) {
    const size_t ps = host_page();
    uint64_t a = off / ps * ps, b = (off + len + ps - 1) / ps * ps;
    b = std::min<uint64_t>(b, (file_size(fd) + ps - 1) / ps * ps);
    if (b <= a) return 0;
    void* m = mmap(nullptr, b - a, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(a));
    if (m == MAP_FAILED) return -1;
    const size_t pages = (b - a) / ps;
    std::vector<char> vec(pages);
    double r = -1;
    if (mincore(m, b - a, vec.data()) == 0) {
        size_t c = 0;
        for (char x : vec) c += x & 1;
        r = static_cast<double>(c) / pages;
    }
    munmap(m, b - a);
    return r;
}

inline uint8_t* alloc_prefaulted(size_t n) {
    n = (n + 16383) & ~size_t{16383};
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    memset(p, 0x5a, n);
    return static_cast<uint8_t*>(p);
}

inline void free_region(void* p, size_t n) {
    n = (n + 16383) & ~size_t{16383};
    munmap(p, n);
}

struct Args {
    std::map<std::string, std::string> kv;
    std::vector<std::string> pos;
    Args(int argc, char** argv, int from) {
        for (int i = from; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) == 0) {
                std::string k = a.substr(2);
                if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                    kv[k] = argv[++i];
                else
                    kv[k] = "1";
            } else {
                pos.push_back(a);
            }
        }
    }
    std::string str(const char* k, const std::string& d = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? d : it->second;
    }
    long long num(const char* k, long long d) const {
        auto it = kv.find(k);
        return it == kv.end() ? d : std::stoll(it->second);
    }
    double real(const char* k, double d) const {
        auto it = kv.find(k);
        return it == kv.end() ? d : std::stod(it->second);
    }
    bool has(const char* k) const { return kv.count(k) != 0; }
};

inline std::vector<long long> split_nums(const std::string& s) {
    std::vector<long long> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(std::stoll(s.substr(i, j - i)));
        i = j + 1;
    }
    return out;
}

}  // namespace iob
