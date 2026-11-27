#include <windows.h>
#include <d3d12.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dstorage_xs_abi.h"
#include "file_system.h"
#include "shims.h"
#include <gta6rt/shims.h>

void gdk_test_register_xmem_fake();
void gdk_test_register_console_files();

using namespace dsx;
namespace fs = std::filesystem;

namespace {

constexpr GUID kFactoryId = {0x0d5e0001, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x01}};
constexpr GUID kQueueId = {0x0d5e0002, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x02}};
constexpr GUID kFileId = {0x0d5e0003, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x03}};
constexpr GUID kStatusId = {0x0d5e0004, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x04}};

struct Item {
    std::string path;
    uint64_t off;
    uint32_t src, dst;
    int queue = -1;
};

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void evict(const std::string& path, const std::vector<Item>& items) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    struct stat st{};
    fstat(fd, &st);
    for (const auto& it : items) {
        if (it.path != path) continue;
        const uint64_t a = it.off / 16384 * 16384, b = std::min<uint64_t>((it.off + it.src + 16383) / 16384 * 16384, (st.st_size + 16383) / 16384 * 16384);
        void* m = mmap(nullptr, b - a, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(a));
        if (m != MAP_FAILED) {
            msync(m, b - a, MS_INVALIDATE);
            munmap(m, b - a);
        }
    }
    close(fd);
}

struct Burners {
    std::vector<std::thread> threads;
    std::vector<std::atomic<uint64_t>> iters;
    std::atomic<bool> stop{false};
    explicit Burners(int n, int duty_pct) : iters(n) {
        for (int i = 0; i < n; ++i)
            threads.emplace_back([this, i, duty_pct] {
                uint64_t x = 0x9e3779b97f4a7c15ull + i;
                while (!stop.load(std::memory_order_relaxed)) {
                    const double a = now_s();
                    for (int k = 0; k < 20000; ++k) x = x * 6364136223846793005ull + 1442695040888963407ull ^ (x >> 29);
                    iters[i].fetch_add(1, std::memory_order_relaxed);
                    if (duty_pct < 100) std::this_thread::sleep_for(std::chrono::duration<double>((now_s() - a) * (100 - duty_pct) / std::max(1, duty_pct)));
                }
                if (x == 42) std::fputc(0, stderr);
            });
    }
    ~Burners() {
        stop = true;
        for (auto& t : threads) t.join();
    }
    uint64_t total() const {
        uint64_t n = 0;
        for (const auto& v : iters) n += v.load(std::memory_order_relaxed);
        return n;
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: dstorage_replay_bench requests.tsv [queues=3] [reps=3] [label]\n");
        return 2;
    }
    int queues = argc > 2 ? std::atoi(argv[2]) : 3;
    const int reps = argc > 3 ? std::atoi(argv[3]) : 3;
    const std::string label = argc > 4 ? argv[4] : "";
    const bool raw = std::getenv("DSREPLAY_RAW") != nullptr;
    std::vector<Item> items;
    {
        std::ifstream f(argv[1]);
        std::string line;
        uint64_t skipped = 0;
        while (std::getline(f, line)) {
            std::istringstream is(line);
            Item it;
            std::string off, src, dst, qn;
            std::getline(is, it.path, '\t');
            std::getline(is, off, '\t');
            std::getline(is, src, '\t');
            std::getline(is, dst, '\t');
            if (std::getline(is, qn, '\t') && !qn.empty()) it.queue = std::stoi(qn);
            it.off = std::stoull(off);
            it.src = static_cast<uint32_t>(std::stoul(src));
            it.dst = static_cast<uint32_t>(std::stoul(dst));
            if (raw) it.dst = it.src;
            if ((!raw && it.dst < it.src) || it.off % 16 || it.dst > (32u << 20)) {
                ++skipped;
                continue;
            }
            items.push_back(it);
        }
        if (skipped) std::fprintf(stderr, "skipped %llu requests the zlib request rules refuse\n", static_cast<unsigned long long>(skipped));
    }
    const int burn = std::getenv("DSREPLAY_BURN") ? std::atoi(std::getenv("DSREPLAY_BURN")) : 0;
    const int duty = std::getenv("DSREPLAY_BURN_DUTY") ? std::atoi(std::getenv("DSREPLAY_BURN_DUTY")) : 100;
    std::unique_ptr<Burners> burners;
    double solo_rate = 0;
    if (burn > 0) {
        burners = std::make_unique<Burners>(burn, duty);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const uint64_t n0 = burners->total();
        const double b0 = now_s();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        solo_rate = (burners->total() - n0) / (now_s() - b0);
    }
    GDK_TEST_REGISTER();
    gdk_test_register_xmem_fake();
    gdk_test_register_console_files();

    using GetFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto get = reinterpret_cast<GetFactory>(rt::shims::find("dstorage_xs.dll", "DStorageGetFactory"));
    void* fv = nullptr;
    if (!get || FAILED(get(kFactoryId, &fv))) return 1;
    auto* factory = static_cast<IDStorageFactoryX2*>(fv);

    const bool by_column = !items.empty() && items.front().queue >= 0;
    if (by_column) {
        queues = 0;
        for (const auto& it : items) queues = std::max(queues, it.queue + 1);
    }
    uint64_t src_total = 0, dst_total = 0, max_dst = 0;
    for (const auto& it : items) {
        src_total += it.src;
        dst_total += it.dst;
        max_dst = std::max<uint64_t>(max_dst, it.dst);
    }
    std::vector<std::string> paths;
    for (const auto& it : items)
        if (std::find(paths.begin(), paths.end(), it.path) == paths.end()) paths.push_back(it.path);
    std::vector<std::vector<uint8_t>> dest(queues, std::vector<uint8_t>(max_dst + 64, 0));

    for (int rep = 0; rep < reps; ++rep) {
        for (const auto& p : paths) evict(p, items);
        std::map<std::string, IDStorageFileX*> files;
        for (const auto& p : paths) {
            std::u16string w(p.begin(), p.end());
            void* fo = nullptr;
            if (FAILED(factory->OpenFile(w.c_str(), kFileId, &fo))) {
                std::fprintf(stderr, "open failed %s\n", p.c_str());
                return 1;
            }
            files[p] = static_cast<IDStorageFileX*>(fo);
        }
        std::vector<IDStorageQueueX2*> qs;
        std::vector<IDStorageStatusArrayX*> ss;
        for (int q = 0; q < queues; ++q) {
            DSTORAGE_QUEUE_DESC1 d{};
            d.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            d.Capacity = 0x1000;
            d.Priority = DSTORAGE_PRIORITY_NORMAL;
            d.Name = "replay";
            void* qv = nullptr;
            factory->CreateQueue1(&d, kQueueId, &qv);
            qs.push_back(static_cast<IDStorageQueueX2*>(qv));
            void* sv = nullptr;
            factory->CreateStatusArray(1, "s", kStatusId, &sv);
            ss.push_back(static_cast<IDStorageStatusArrayX*>(sv));
        }
        rusage r0{}, r1{};
        getrusage(RUSAGE_SELF, &r0);
        const double t0 = now_s();
        const uint64_t burn0 = burners ? burners->total() : 0;
        std::vector<std::thread> feeders;
        for (int q = 0; q < queues; ++q)
            feeders.emplace_back([&, q] {
                for (size_t i = by_column ? 0 : q; i < items.size(); i += by_column ? 1 : queues) {
                    if (by_column && items[i].queue != q) continue;
                    DSTORAGE_REQUEST r{};
                    r.Options.ZlibDecompress = !raw && items[i].dst > items[i].src ? 1 : 0;
                    r.File = files[items[i].path];
                    r.FileOffset = items[i].off;
                    r.SourceSize = items[i].src;
                    r.Destination = dest[q].data();
                    r.DestinationSize = items[i].dst;
                    qs[q]->EnqueueRequest(&r);
                }
                qs[q]->EnqueueStatus(ss[q], 0);
                qs[q]->Submit();
            });
        for (auto& t : feeders) t.join();
        bool ok = true;
        for (int q = 0; q < queues; ++q) {
            const double deadline = now_s() + 300;
            while (!ss[q]->IsComplete(0) && now_s() < deadline) std::this_thread::sleep_for(std::chrono::microseconds(300));
            ok &= ss[q]->IsComplete(0) && ss[q]->GetHResult(0) == S_OK;
        }
        const double wall = now_s() - t0;
        const double burn_ratio = burners && solo_rate > 0 ? (burners->total() - burn0) / wall / solo_rate : 0;
        getrusage(RUSAGE_SELF, &r1);
        const double cpu = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) * 1e-6 + (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) +
                           (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) * 1e-6;
        std::printf("RESULT bench=dsreplay raw=%d label=%s queues=%d n=%zu src_mib=%.1f dst_mib=%.1f wall_s=%.3f src_mibs=%.0f out_mibs=%.0f cpu_s=%.2f cpu_s_per_gib_out=%.2f ok=%d burn_threads=%d burn_vs_solo=%.3f\n", int(raw), label.c_str(), queues,
                    items.size(), src_total / 1048576.0, dst_total / 1048576.0, wall, src_total / 1048576.0 / wall, dst_total / 1048576.0 / wall, cpu, cpu / (dst_total / 1073741824.0), ok, burn, burn_ratio);
        std::fflush(stdout);
        for (auto* q : qs) {
            q->Close();
            q->Release();
        }
        for (auto* s : ss) s->Release();
        for (auto& [p, f] : files) f->Release();
    }
    return 0;
}
