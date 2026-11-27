#include "common.h"

namespace iob {

namespace {

struct Sample {
    double t;
    double streamed_gib;
    VmSnap v;
    double sweep_ms;
    int level;
};

double sweep(const uint8_t* p, size_t n, uint64_t& sink) {
    const double t0 = now_s();
    uint64_t s = 0;
    for (size_t i = 0; i < n; i += 16384) s += p[i];
    sink += s;
    return (now_s() - t0) * 1e3;
}

}  // namespace

int cmd_pressure(const Args& a) {
    const char* ge = std::getenv("IOB_GAME");
    const std::string game = a.str("game", ge ? ge : "/Users/a/Desktop/Projects/GTA-VI/xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK");
    const std::string rel = a.str("file", "levels.rpf");
    const std::string mode = a.str("mode", "cache");
    const uint64_t base = static_cast<uint64_t>(a.real("base-gib", 4) * (1ull << 30)) / 16384 * 16384;
    const uint64_t total = static_cast<uint64_t>(a.real("gib", 16) * (1ull << 30));
    const size_t victim_bytes = static_cast<size_t>(a.real("victim-gib", 2) * (1ull << 30));
    const int threads = static_cast<int>(a.num("threads", 4));
    const size_t io = static_cast<size_t>(a.num("io", 1 << 20));
    const double interval = a.real("interval-ms", 250) * 1e-3;
    const uint64_t lag = static_cast<uint64_t>(a.num("evict-lag-mib", 256)) << 20;
    const int abort_level = static_cast<int>(a.num("abort-level", 4));
    int fd = open((game + "/" + rel).c_str(), O_RDONLY);
    if (fd < 0) {
        std::perror("open");
        return 1;
    }
    const uint64_t fsz = file_size(fd);
    if (base + total > fsz) {
        std::fprintf(stderr, "range exceeds file\n");
        return 1;
    }
    if (mode == "nocache") fcntl(fd, F_NOCACHE, 1);
    if (mode == "nocache-rdahead0") {
        fcntl(fd, F_NOCACHE, 1);
        fcntl(fd, F_RDAHEAD, 0);
    }
    evict_range(fd, base, total);

    uint8_t* victim = alloc_prefaulted(victim_bytes);
    {
        uint64_t x = 0x9e3779b97f4a7c15ull;
        for (size_t i = 0; i < victim_bytes; i += 8) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            std::memcpy(victim + i, &x, 8);
        }
    }
    uint64_t sink = 0;
    sweep(victim, victim_bytes, sink);
    sweep(victim, victim_bytes, sink);
    const VmSnap v0 = vm_snap();
    const double base_sweep = sweep(victim, victim_bytes, sink);

    std::atomic<uint64_t> cursor{0};
    std::atomic<uint64_t> done_bytes{0};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> evicted_to{0};
    std::mutex evict_m;
    auto reader = [&] {
        std::vector<uint8_t> buf(io);
        while (!stop) {
            const uint64_t off = cursor.fetch_add(io);
            if (off >= total) break;
            size_t d = 0;
            while (d < io && off + d < total) {
                ssize_t n = pread(fd, buf.data() + d, std::min<size_t>(io - d, total - off - d), static_cast<off_t>(base + off + d));
                if (n <= 0) break;
                d += static_cast<size_t>(n);
            }
            const uint64_t dn = done_bytes.fetch_add(d) + d;
            if (mode == "evict-behind" && dn > lag) {
                std::unique_lock<std::mutex> lk(evict_m, std::try_to_lock);
                if (lk.owns_lock()) {
                    const uint64_t upto = (dn - lag) / (64ull << 20) * (64ull << 20);
                    const uint64_t from = evicted_to.load();
                    if (upto > from + (64ull << 20)) {
                        evict_range(fd, base + from, upto - from);
                        evicted_to = upto;
                    }
                }
            }
        }
    };
    std::vector<std::thread> th;
    std::vector<Sample> samples;
    const double t0 = now_s();
    const Cpu c0 = cpu_now();
    for (int i = 0; i < threads; ++i) th.emplace_back(reader);
    bool aborted = false;
    double finish = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::duration<double>(interval));
        Sample s;
        s.t = now_s() - t0;
        s.streamed_gib = done_bytes.load() / double(1ull << 30);
        s.v = vm_snap();
        s.level = pressure_level();
        s.sweep_ms = sweep(victim, victim_bytes, sink);
        samples.push_back(s);
        std::printf("PSAMPLE mode=%s t=%.2f streamed_gib=%.2f free_mib=%llu file_mib=%llu anon_mib=%llu compressor_mib=%llu inactive_mib=%llu swapouts=%llu pageouts=%llu decompressions=%llu sweep_ms=%.1f level=%d\n",
                    mode.c_str(), s.t, s.streamed_gib, (unsigned long long)s.v.mib(s.v.free), (unsigned long long)s.v.mib(s.v.file), (unsigned long long)s.v.mib(s.v.anon),
                    (unsigned long long)s.v.mib(s.v.compressor), (unsigned long long)s.v.mib(s.v.inactive), (unsigned long long)(s.v.swapouts - v0.swapouts),
                    (unsigned long long)(s.v.pageouts - v0.pageouts), (unsigned long long)(s.v.decompressions - v0.decompressions), s.sweep_ms, s.level);
        std::fflush(stdout);
        if (s.level >= abort_level) {
            aborted = true;
            stop = true;
        }
        if (done_bytes.load() >= total || aborted) {
            finish = now_s() - t0;
            break;
        }
    }
    stop = true;
    for (auto& t : th) t.join();
    const Cpu c1 = cpu_now();
    const double wall = finish > 0 ? finish : now_s() - t0;
    const VmSnap v1 = vm_snap();
    double sweep_after = sweep(victim, victim_bytes, sink);
    const VmSnap v2 = vm_snap();

    uint64_t peak_file = v0.file, min_free = v0.free, peak_comp = v0.compressor, peak_swapouts = 0;
    double max_sweep = 0;
    int max_level = 0;
    std::vector<double> sw;
    for (const auto& s : samples) {
        peak_file = std::max<uint64_t>(peak_file, s.v.file);
        min_free = std::min<uint64_t>(min_free, s.v.free);
        peak_comp = std::max<uint64_t>(peak_comp, s.v.compressor);
        max_sweep = std::max(max_sweep, s.sweep_ms);
        max_level = std::max(max_level, s.level);
        sw.push_back(s.sweep_ms);
    }
    (void)peak_swapouts;
    const Pct psw = percentiles(sw);
    std::printf(
        "RESULT bench=pressure mode=%s file=%s streamed_gib=%.2f io=%zu threads=%d victim_gib=%.1f wall_s=%.2f mibs=%.0f cpu_s=%.2f aborted=%d base_file_mib=%llu peak_file_mib=%llu "
        "file_growth_mib=%lld base_free_mib=%llu min_free_mib=%llu compressor_base_mib=%llu compressor_peak_mib=%llu swapouts_delta=%llu pageouts_delta=%llu decompressions_delta=%llu "
        "swapins_delta=%llu sweep_base_ms=%.1f sweep_p50_ms=%.1f sweep_max_ms=%.1f sweep_after_ms=%.1f max_pressure_level=%d file_mib_after=%llu\n",
        mode.c_str(), rel.c_str(), done_bytes.load() / double(1ull << 30), io, threads, victim_bytes / double(1ull << 30), wall, done_bytes.load() / double(1 << 20) / wall,
        c1.total() - c0.total(), aborted, (unsigned long long)v0.mib(v0.file), (unsigned long long)v0.mib(peak_file),
        (long long)v0.mib(peak_file) - (long long)v0.mib(v0.file), (unsigned long long)v0.mib(v0.free), (unsigned long long)v0.mib(min_free),
        (unsigned long long)v0.mib(v0.compressor), (unsigned long long)v0.mib(peak_comp), (unsigned long long)(v1.swapouts - v0.swapouts), (unsigned long long)(v1.pageouts - v0.pageouts),
        (unsigned long long)(v1.decompressions - v0.decompressions), (unsigned long long)(v1.swapins - v0.swapins), base_sweep, psw.p50, max_sweep, sweep_after, max_level,
        (unsigned long long)v0.mib(v2.file));
    if (sink == 42) std::printf("%llu\n", (unsigned long long)sink);
    evict_range(fd, base, total);
    free_region(victim, victim_bytes);
    close(fd);
    return 0;
}

}  // namespace iob
