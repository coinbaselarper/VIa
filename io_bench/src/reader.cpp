#include "reader.h"

#include <condition_variable>

namespace iob {

namespace {

struct Range {
    int file;
    uint64_t a, b;
};

std::vector<Range> merged_ranges(const std::vector<Req>& reqs, uint64_t gap) {
    std::vector<Range> v;
    for (const auto& r : reqs) v.push_back({static_cast<int>(r.file), r.off, r.off + r.src});
    std::sort(v.begin(), v.end(), [](const Range& x, const Range& y) { return x.file != y.file ? x.file < y.file : x.a < y.a; });
    std::vector<Range> out;
    for (const auto& r : v) {
        if (!out.empty() && out.back().file == r.file && r.a <= out.back().b + gap)
            out.back().b = std::max(out.back().b, r.b);
        else
            out.push_back(r);
    }
    return out;
}

struct Files {
    std::vector<int> fd;
    std::vector<uint8_t*> map;
    std::vector<uint64_t> size;
    ~Files() {
        for (size_t i = 0; i < fd.size(); ++i) {
            if (map[i]) munmap(map[i], size[i]);
            if (fd[i] >= 0) close(fd[i]);
        }
    }
};

bool open_files(const std::vector<std::string>& paths, const ReadCfg& cfg, Files& f) {
    for (const auto& p : paths) {
        int fd = open(p.c_str(), O_RDONLY);
        if (fd < 0) {
            std::perror(p.c_str());
            return false;
        }
        if (cfg.nocache) fcntl(fd, F_NOCACHE, 1);
        if (cfg.rdahead >= 0) fcntl(fd, F_RDAHEAD, cfg.rdahead);
        f.fd.push_back(fd);
        f.size.push_back(file_size(fd));
        f.map.push_back(nullptr);
    }
    return true;
}

void advise_read(int fd, const Req& r) {
    radvisory ra;
    ra.ra_offset = static_cast<off_t>(r.off);
    ra.ra_count = static_cast<int>(std::min<uint32_t>(r.src, 0x7fffffff));
    fcntl(fd, F_RDADVISE, &ra);
}

size_t max_src(const std::vector<Req>& reqs) {
    size_t m = 4096;
    for (const auto& r : reqs) m = std::max<size_t>(m, r.src);
    return m;
}

void run_pread(const Files& f, const std::vector<Req>& reqs, const ReadCfg& cfg, ReadResult& res) {
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> errors{0};
    const size_t maxb = max_src(reqs);
    auto worker = [&] {
        uint8_t* dest = alloc_prefaulted(maxb);
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= reqs.size()) break;
            const Req& r = reqs[i];
            if (cfg.advise > 0 && i + cfg.advise < reqs.size()) advise_read(f.fd[reqs[i + cfg.advise].file], reqs[i + cfg.advise]);
            const double t0 = now_s();
            size_t done = 0;
            const size_t step = cfg.split ? cfg.split : r.src;
            while (done < r.src) {
                const size_t want = std::min<size_t>(step, r.src - done);
                ssize_t n = pread(f.fd[r.file], dest + done, want, static_cast<off_t>(r.off + done));
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) continue;
                    errors++;
                    break;
                }
                done += static_cast<size_t>(n);
            }
            res.lat_ms[i] = (now_s() - t0) * 1e3;
            if (cfg.evict_behind) evict_range(f.fd[r.file], r.off, r.src);
        }
        free_region(dest, maxb);
    };
    std::vector<std::thread> th;
    for (int t = 0; t < cfg.threads; ++t) th.emplace_back(worker);
    for (auto& t : th) t.join();
    res.errors += errors;
}

void run_mmap(Files& f, const std::vector<Req>& reqs, const ReadCfg& cfg, ReadResult& res) {
    for (size_t i = 0; i < f.fd.size(); ++i) {
        void* m = mmap(nullptr, f.size[i], PROT_READ, MAP_SHARED, f.fd[i], 0);
        if (m == MAP_FAILED) {
            std::perror("mmap");
            res.errors++;
            return;
        }
        f.map[i] = static_cast<uint8_t*>(m);
        if (cfg.madv == "random") madvise(m, f.size[i], MADV_RANDOM);
        if (cfg.madv == "seq") madvise(m, f.size[i], MADV_SEQUENTIAL);
    }
    const size_t ps = 16384;
    auto willneed = [&](const Req& r) {
        uint64_t a = r.off / ps * ps, b = (r.off + r.src + ps - 1) / ps * ps;
        madvise(f.map[r.file] + a, b - a, MADV_WILLNEED);
    };
    std::atomic<size_t> next{0};
    const size_t maxb = max_src(reqs);
    auto worker = [&] {
        uint8_t* dest = alloc_prefaulted(maxb);
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= reqs.size()) break;
            const Req& r = reqs[i];
            if (cfg.madv == "willneed") {
                if (cfg.advise > 0) {
                    if (i + cfg.advise < reqs.size()) willneed(reqs[i + cfg.advise]);
                } else {
                    willneed(r);
                }
            }
            const double t0 = now_s();
            std::memcpy(dest, f.map[r.file] + r.off, r.src);
            res.lat_ms[i] = (now_s() - t0) * 1e3;
        }
        free_region(dest, maxb);
    };
    std::vector<std::thread> th;
    for (int t = 0; t < cfg.threads; ++t) th.emplace_back(worker);
    for (auto& t : th) t.join();
}

void run_dio(const Files& f, const std::vector<Req>& reqs, const ReadCfg& cfg, ReadResult& res) {
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    std::vector<dispatch_io_t> ch;
    for (size_t i = 0; i < f.fd.size(); ++i) {
        dispatch_io_t c = dispatch_io_create(DISPATCH_IO_RANDOM, f.fd[i], q, ^(int) {});
        if (cfg.dio_hw) dispatch_io_set_high_water(c, cfg.dio_hw);
        ch.push_back(c);
    }
    const size_t maxb = max_src(reqs);
    const int depth = cfg.threads;
    std::vector<uint8_t*> slot(depth);
    struct State {
        std::vector<int> freelist;
        std::mutex mu;
        std::atomic<uint64_t> errors{0};
    } state;
    State* st = &state;
    for (int i = 0; i < depth; ++i) {
        slot[i] = alloc_prefaulted(maxb);
        st->freelist.push_back(i);
    }
    dispatch_semaphore_t sem = dispatch_semaphore_create(depth);
    dispatch_group_t grp = dispatch_group_create();
    double* lat = res.lat_ms.data();
    for (size_t i = 0; i < reqs.size(); ++i) {
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        int s;
        {
            std::lock_guard<std::mutex> lk(st->mu);
            s = st->freelist.back();
            st->freelist.pop_back();
        }
        if (cfg.advise > 0 && i + cfg.advise < reqs.size()) advise_read(f.fd[reqs[i + cfg.advise].file], reqs[i + cfg.advise]);
        const Req r = reqs[i];
        uint8_t* dest = slot[s];
        const double t0 = now_s();
        auto got = std::make_shared<std::atomic<size_t>>(0);
        dispatch_group_enter(grp);
        dispatch_io_read(ch[r.file], static_cast<off_t>(r.off), r.src, q, ^(bool done, dispatch_data_t data, int err) {
            if (data) {
                const size_t base = got->load();
                dispatch_data_apply(data, ^bool(dispatch_data_t, size_t offset, const void* buf, size_t size) {
                    std::memcpy(dest + base + offset, buf, size);
                    return true;
                });
                got->fetch_add(dispatch_data_get_size(data));
            }
            if (done) {
                if (err || got->load() != r.src) st->errors++;
                lat[i] = (now_s() - t0) * 1e3;
                {
                    std::lock_guard<std::mutex> lk(st->mu);
                    st->freelist.push_back(s);
                }
                dispatch_semaphore_signal(sem);
                dispatch_group_leave(grp);
            }
        });
    }
    dispatch_group_wait(grp, DISPATCH_TIME_FOREVER);
    for (auto c : ch) {
        dispatch_io_close(c, DISPATCH_IO_STOP);
        dispatch_release(c);
    }
    for (auto p : slot) free_region(p, maxb);
    dispatch_release(sem);
    dispatch_release(grp);
    res.errors += st->errors.load();
}

}  // namespace

ReadResult run_reads(const std::vector<std::string>& paths, const std::vector<Req>& reqs, const ReadCfg& cfg) {
    ReadResult res;
    Files f;
    if (!open_files(paths, cfg, f)) {
        res.errors = 1;
        return res;
    }
    const auto ranges = merged_ranges(reqs, 1 << 20);
    double t_e = now_s();
    if (cfg.evict_before)
        for (const auto& r : ranges) evict_range(f.fd[r.file], r.a, r.b - r.a);
    res.evict_s = now_s() - t_e;
    {
        double sum = 0, tot = 0;
        for (const auto& r : ranges) {
            const double fr = resident_fraction(f.fd[r.file], r.a, r.b - r.a);
            if (fr >= 0) {
                sum += fr * (r.b - r.a);
                tot += r.b - r.a;
            }
        }
        res.resident_before = tot > 0 ? sum / tot : -1;
    }
    for (const auto& r : reqs) {
        res.read_bytes += r.src;
        res.bytes += cfg.useful_in_dst ? r.dst : r.src;
    }
    res.n = reqs.size();
    res.lat_ms.assign(reqs.size(), 0.0);
    res.vm0 = vm_snap();
    const Cpu c0 = cpu_now();
    const double t0 = now_s();
    if (cfg.backend == "pread")
        run_pread(f, reqs, cfg, res);
    else if (cfg.backend == "mmap")
        run_mmap(f, reqs, cfg, res);
    else if (cfg.backend == "dio")
        run_dio(f, reqs, cfg, res);
    res.wall = now_s() - t0;
    const Cpu c1 = cpu_now();
    res.cpu = {c1.user - c0.user, c1.sys - c0.sys};
    res.vm1 = vm_snap();
    if (cfg.evict_after)
        for (const auto& r : ranges) evict_range(f.fd[r.file], r.a, r.b - r.a);
    return res;
}

void evict_requests(const std::vector<std::string>& paths, const std::vector<Req>& reqs) {
    std::vector<int> fds;
    for (const auto& p : paths) fds.push_back(open(p.c_str(), O_RDONLY));
    for (const auto& r : merged_ranges(reqs, 1 << 20))
        if (fds[r.file] >= 0) evict_range(fds[r.file], r.a, r.b - r.a);
    for (int fd : fds)
        if (fd >= 0) close(fd);
}

void print_read_result(const char* tag, const ReadCfg& cfg, const ReadResult& r, const char* extra) {
    const Pct p = percentiles(r.lat_ms);
    const double gib = static_cast<double>(r.bytes) / (1ull << 30);
    const double mibs = static_cast<double>(r.bytes) / (1 << 20) / std::max(r.wall, 1e-9);
    const long long file_delta = static_cast<long long>(r.vm1.file) - static_cast<long long>(r.vm0.file);
    std::printf(
        "RESULT bench=%s backend=%s qd=%d cache=%s rdahead=%d advise=%d split=%zu madv=%s hw=%zu eb=%d n=%llu bytes_mib=%.1f read_mib=%.1f wall_s=%.3f mibs=%.0f iops=%.0f "
        "lat_mean_ms=%.3f p50_ms=%.3f p90_ms=%.3f p99_ms=%.3f p999_ms=%.3f max_ms=%.3f cpu_user_s=%.3f cpu_sys_s=%.3f cpu_s_per_gib=%.3f "
        "file_pages_delta_mib=%lld free_delta_mib=%lld resident_before_pct=%.1f errors=%llu %s\n",
        tag, cfg.backend.c_str(), cfg.threads, cfg.nocache ? "nocache" : "cache", cfg.rdahead, cfg.advise, cfg.split, cfg.madv.c_str(), cfg.dio_hw,
        cfg.evict_behind ? 1 : 0, static_cast<unsigned long long>(r.n), static_cast<double>(r.bytes) / (1 << 20), static_cast<double>(r.read_bytes) / (1 << 20), r.wall, mibs, r.n / std::max(r.wall, 1e-9), p.mean, p.p50, p.p90,
        p.p99, p.p999, p.max, r.cpu.user, r.cpu.sys, r.cpu.total() / std::max(gib, 1e-9), file_delta * static_cast<long long>(r.vm0.page) / (1 << 20),
        (static_cast<long long>(r.vm1.free) - static_cast<long long>(r.vm0.free)) * static_cast<long long>(r.vm0.page) / (1 << 20),
        r.resident_before * 100, static_cast<unsigned long long>(r.errors), extra);
    std::fflush(stdout);
}

}  // namespace iob
