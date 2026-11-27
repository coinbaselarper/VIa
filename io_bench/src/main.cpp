#include <dirent.h>

#include <cmath>
#include <functional>

#include "common.h"
#include "reader.h"
#include "shapes.h"

namespace iob {
int cmd_decode(const Args& a);
int cmd_model(const Args& a);
int cmd_pressure(const Args& a);
}  // namespace iob

using namespace iob;

namespace {

std::string game_dir(const Args& a) {
    std::string g = a.str("game");
    if (g.empty()) {
        const char* e = std::getenv("IOB_GAME");
        g = e ? e : "/Users/a/Desktop/Projects/GTA-VI/xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK";
    }
    return g;
}

std::vector<std::string> abs_paths(const Shape& s, const std::string& game) {
    std::vector<std::string> v;
    for (const auto& f : s.files) v.push_back(f.empty() || f[0] == '/' ? f : game + "/" + f);
    return v;
}

void walk_ttpf(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string p = dir + "/" + n;
        if (e->d_type == DT_DIR)
            walk_ttpf(p, out);
        else if (n.size() > 5 && n.substr(n.size() - 5) == ".ttpf")
            out.push_back(p);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
}

int cmd_shapes(const Args& a) {
    if (a.pos.empty()) return 2;
    const std::string game = game_dir(a), out = a.str("out");
    Shape s;
    if (a.pos[0] == "rpf") {
        const std::string rel = a.str("file", "levels.rpf");
        const int windows = static_cast<int>(a.num("windows", 6));
        const uint64_t wlen = static_cast<uint64_t>(a.num("window-mib", 128)) << 20;
        int fd = open((game + "/" + rel).c_str(), O_RDONLY);
        if (fd < 0) {
            std::perror(rel.c_str());
            return 1;
        }
        const uint64_t fsz = file_size(fd);
        close(fd);
        s.files.push_back(rel);
        for (int w = 0; w < windows; ++w) {
            uint64_t start = static_cast<uint64_t>((w + 0.5) / windows * static_cast<double>(fsz - wlen));
            start = start / 16384 * 16384;
            const size_t before = s.reqs.size();
            scan_rpf_window(game + "/" + rel, 0, start, wlen, static_cast<uint32_t>(w), s);
            std::fprintf(stderr, "window %d start=%llu MiB streams=%zu\n", w, static_cast<unsigned long long>(start >> 20), s.reqs.size() - before);
        }
    } else if (a.pos[0] == "ttpf") {
        std::vector<std::string> files;
        walk_ttpf(game + "/data/ttdb", files);
        if (a.num("packs", 0) != 0) {
            std::vector<std::string> pack_dirs;
            if (DIR* d = opendir((game + "/pack").c_str())) {
                while (dirent* e = readdir(d))
                    if (e->d_name[0] != '.') pack_dirs.push_back(game + "/pack/" + e->d_name + "/data/ttdb");
                closedir(d);
            }
            std::sort(pack_dirs.begin(), pack_dirs.end());
            for (const auto& dir : pack_dirs) walk_ttpf(dir, files);
        }
        for (const auto& p : files) {
            const uint32_t idx = static_cast<uint32_t>(s.files.size());
            const size_t before = s.reqs.size();
            if (scan_ttpf(p, idx, s)) {
                s.files.push_back(p.substr(game.size() + 1));
            } else {
                s.reqs.resize(before);
                std::fprintf(stderr, "skip %s\n", p.c_str());
            }
        }
        const long long sample = a.num("sample", 0);
        if (sample > 0 && static_cast<size_t>(sample) < s.reqs.size()) {
            std::mt19937_64 rng(7);
            std::shuffle(s.reqs.begin(), s.reqs.end(), rng);
            s.reqs.resize(sample);
            std::sort(s.reqs.begin(), s.reqs.end(), [](const Req& x, const Req& y) { return x.file != y.file ? x.file < y.file : x.off < y.off; });
        }
    } else {
        return 2;
    }
    if (out.empty() || !shape_save(out, s)) {
        std::fprintf(stderr, "cannot write shape file\n");
        return 1;
    }
    std::printf("wrote %s files=%zu requests=%zu\n", out.c_str(), s.files.size(), s.reqs.size());
    return 0;
}

const char* codec_name(uint8_t c) {
    static char buf[16];
    if (c == kRaw) return "raw";
    if (c == kZlib) return "zlib";
    std::snprintf(buf, sizeof buf, "bcpack%u", c - kBcpackBase);
    return buf;
}

int cmd_info(const Args& a) {
    Shape s;
    if (a.pos.empty() || !shape_load(a.pos[0], s)) return 1;
    std::map<uint8_t, std::vector<double>> src, dst;
    std::map<uint8_t, double> srcsum, dstsum;
    for (const auto& r : s.reqs) {
        src[r.codec].push_back(r.src);
        dst[r.codec].push_back(r.dst);
        srcsum[r.codec] += r.src;
        dstsum[r.codec] += r.dst;
    }
    std::printf("shape %s files=%zu requests=%zu\n", a.pos[0].c_str(), s.files.size(), s.reqs.size());
    for (auto& [c, v] : src) {
        const Pct ps = percentiles(v), pd = percentiles(dst[c]);
        std::printf(
            "INFO codec=%s n=%zu src_mib=%.1f dst_mib=%.1f ratio=%.2f src_mean=%.0f src_p50=%.0f src_p90=%.0f src_p99=%.0f src_max=%.0f dst_p50=%.0f dst_p99=%.0f dst_max=%.0f\n",
            codec_name(c), v.size(), srcsum[c] / (1 << 20), dstsum[c] / (1 << 20), dstsum[c] / std::max(1.0, srcsum[c]), ps.mean, ps.p50, ps.p90, ps.p99, ps.max, pd.p50, pd.p99, pd.max);
    }
    static const uint32_t edges[] = {4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576, 4194304, 0xffffffffu};
    std::vector<size_t> cnt(11, 0);
    std::vector<double> bytes(11, 0);
    for (const auto& r : s.reqs) {
        size_t b = 0;
        while (r.src > edges[b]) ++b;
        cnt[b]++;
        bytes[b] += r.src;
    }
    double tot = 0;
    for (double b : bytes) tot += b;
    for (size_t b = 0; b < 11; ++b)
        std::printf("INFO src_le=%u n=%zu pct_n=%.1f pct_bytes=%.1f\n", edges[b], cnt[b], 100.0 * cnt[b] / std::max<size_t>(1, s.reqs.size()), 100.0 * bytes[b] / std::max(1.0, tot));
    return 0;
}

std::vector<long long> list_arg(const Args& a, const char* k, const char* d) { return split_nums(a.str(k, d)); }

std::vector<std::string> str_list(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

void sweep(const Args& a, const std::vector<std::string>& paths, const std::vector<Req>& reqs, const char* tag, const std::string& extra) {
    const auto qds = list_arg(a, "qd", "4");
    const auto rdas = list_arg(a, "rdahead", "-1");
    const auto advs = list_arg(a, "advise", "0");
    const auto splits = list_arg(a, "split", "0");
    const auto hws = list_arg(a, "hw", "0");
    const auto backs = str_list(a.str("backend", "pread"));
    const auto caches = str_list(a.str("cache", "cache"));
    const auto madvs = str_list(a.str("madv", "none"));
    const int trials = static_cast<int>(a.num("trials", 1));
    const bool eb = a.num("evict-behind", 0) != 0;
    const bool keep = a.num("no-evict", 0) != 0;
    const bool useful = a.num("coalesce", -1) >= 0;
    for (const auto& b : backs)
        for (const auto& c : caches)
            for (long long qd : qds)
                for (long long ra : rdas)
                    for (long long ad : advs)
                        for (long long sp : splits)
                            for (long long hw : hws)
                                for (const auto& m : madvs) {
                                    ReadCfg cfg;
                                    cfg.backend = b;
                                    cfg.nocache = c == "nocache";
                                    cfg.threads = static_cast<int>(qd);
                                    cfg.rdahead = static_cast<int>(ra);
                                    cfg.advise = static_cast<int>(ad);
                                    cfg.split = static_cast<size_t>(sp);
                                    cfg.dio_hw = static_cast<size_t>(hw);
                                    cfg.madv = m;
                                    cfg.evict_behind = eb;
                                    cfg.evict_before = !keep;
                                    cfg.evict_after = !keep;
                                    cfg.useful_in_dst = useful;
                                    for (int t = 0; t < trials; ++t) {
                                        ReadResult r = run_reads(paths, reqs, cfg);
                                        print_read_result(tag, cfg, r, extra.c_str());
                                    }
                                }
}

int cmd_read(const Args& a) {
    Shape s;
    if (a.pos.empty() || !shape_load(a.pos[0], s)) {
        std::fprintf(stderr, "cannot load shape\n");
        return 1;
    }
    const std::string game = game_dir(a);
    const std::string want = a.str("codec", "any");
    std::vector<Req> sel;
    for (const auto& r : s.reqs) {
        if (want == "any" || (want == "zlib" && r.codec == kZlib) || (want == "bcpack" && r.codec >= kBcpackBase) || (want == "raw" && r.codec == kRaw)) sel.push_back(r);
    }
    const std::string ord = a.str("order", "seq");
    const Order o = ord == "rand" ? Order::Rand : ord == "runs" ? Order::Runs : Order::Seq;
    sel = make_order(sel, o, static_cast<uint64_t>(a.num("seed", 1)), a.real("run-mean", 16));
    const uint64_t limit = static_cast<uint64_t>(a.num("limit-mib", 1024)) << 20;
    std::vector<Req> use;
    uint64_t sum = 0;
    for (const auto& r : sel) {
        if (sum >= limit) break;
        use.push_back(r);
        sum += r.src;
    }
    if (a.has("min-src")) {
        std::vector<Req> f;
        for (const auto& r : use)
            if (r.src >= static_cast<uint64_t>(a.num("min-src", 0))) f.push_back(r);
        use = f;
    }
    const long long gap = a.num("coalesce", -1);
    if (gap >= 0) {
        const size_t batch = static_cast<size_t>(a.num("batch", 256));
        const uint64_t max_merge = static_cast<uint64_t>(a.num("max-merge", 4 << 20));
        std::vector<Req> merged;
        for (size_t i = 0; i < use.size(); i += batch) {
            const size_t batch_start = merged.size();
            std::vector<Req> b(use.begin() + i, use.begin() + std::min(use.size(), i + batch));
            std::sort(b.begin(), b.end(), [](const Req& x, const Req& y) { return x.file != y.file ? x.file < y.file : x.off < y.off; });
            for (const auto& r : b) {
                if (!merged.empty() && merged.size() > batch_start && merged.back().file == r.file && r.off >= merged.back().off &&
                    r.off <= merged.back().off + merged.back().src + static_cast<uint64_t>(gap) &&
                    r.off + r.src - merged.back().off <= max_merge) {
                    Req& m = merged.back();
                    m.dst += r.src;
                    m.src = static_cast<uint32_t>(std::max<uint64_t>(m.off + m.src, r.off + r.src) - m.off);
                } else {
                    Req m = r;
                    m.dst = r.src;
                    merged.push_back(m);
                }
            }
        }
        use = merged;
    }
    char extra[256];
    std::snprintf(extra, sizeof extra, "shape=%s codec=%s order=%s run_mean=%.0f coalesce=%lld", a.pos[0].c_str(), want.c_str(), ord.c_str(), a.real("run-mean", 16), gap);
    sweep(a, abs_paths(s, game), use, "replay", extra);
    return 0;
}

int cmd_bulk(const Args& a) {
    const std::string game = game_dir(a);
    const std::string rel = a.str("file", "levels.rpf");
    int fd = open((game + "/" + rel).c_str(), O_RDONLY);
    if (fd < 0) return 1;
    const uint64_t fsz = file_size(fd);
    close(fd);
    const uint64_t base = static_cast<uint64_t>(a.real("base-gib", 30) * (1ull << 30)) / 16384 * 16384 + static_cast<uint64_t>(a.num("misalign", 0));
    const uint64_t span = std::min<uint64_t>(static_cast<uint64_t>(a.num("span-mib", 4096)) << 20, fsz - base);
    const uint64_t total = static_cast<uint64_t>(a.num("bytes-mib", 1024)) << 20;
    const bool rnd = a.str("order", "seq") == "rand";
    for (long long io : list_arg(a, "io", "1048576")) {
        auto reqs = uniform_requests(0, base, span, static_cast<uint32_t>(io), rnd, static_cast<uint64_t>(a.num("seed", 1)), total);
        char extra[128];
        std::snprintf(extra, sizeof extra, "io=%lld order=%s file=%s misalign=%lld", io, rnd ? "rand" : "seq", rel.c_str(), a.num("misalign", 0));
        sweep(a, {game + "/" + rel}, reqs, "bulk", extra);
    }
    return 0;
}

int cmd_export(const Args& a) {
    Shape s;
    if (a.pos.empty() || !shape_load(a.pos[0], s)) return 1;
    const std::string game = game_dir(a);
    std::vector<Req> sel;
    for (const auto& r : s.reqs)
        if (r.codec == kZlib) sel.push_back(r);
    const std::string ord = a.str("order", "runs");
    sel = make_order(sel, ord == "rand" ? Order::Rand : ord == "seq" ? Order::Seq : Order::Runs, static_cast<uint64_t>(a.num("seed", 1)), a.real("run-mean", 16));
    const uint64_t limit = static_cast<uint64_t>(a.num("limit-mib", 256)) << 20;
    const auto paths = abs_paths(s, game);
    uint64_t sum = 0;
    for (const auto& r : sel) {
        if (sum >= limit) break;
        std::printf("%s\t%llu\t%u\t%u\n", paths[r.file].c_str(), static_cast<unsigned long long>(r.off), r.src, r.dst);
        sum += r.src;
    }
    return 0;
}

int cmd_membw(const Args& a) {
    const size_t n = static_cast<size_t>(a.num("mib", 16)) << 20;
    const int reps = static_cast<int>(a.num("reps", 20));
    uint8_t* src = alloc_prefaulted(n);
    uint8_t* dst = alloc_prefaulted(n);
    std::vector<uint8_t> v;
    double best_cp = 0, best_set = 0, best_vec = 0;
    for (int r = 0; r < reps; ++r) {
        double t = now_s();
        std::memcpy(dst, src, n);
        best_cp = std::max(best_cp, n / (now_s() - t) / (1 << 20));
        t = now_s();
        std::memset(dst, r, n);
        best_set = std::max(best_set, n / (now_s() - t) / (1 << 20));
        t = now_s();
        std::vector<uint8_t> w(n);
        v.swap(w);
        best_vec = std::max(best_vec, n / (now_s() - t) / (1 << 20));
    }
    std::printf("RESULT bench=membw mib=%zu memcpy_mibs=%.0f memset_mibs=%.0f vector_resize_mibs=%.0f\n", n >> 20, best_cp, best_set, best_vec);
    return 0;
}

int cmd_sysinfo(const Args&) {
    char brand[128] = "";
    size_t n = sizeof brand;
    sysctlbyname("machdep.cpu.brand_string", brand, &n, nullptr, 0);
    int ncpu = 0, pc = 0, ec = 0;
    n = sizeof ncpu;
    sysctlbyname("hw.ncpu", &ncpu, &n, nullptr, 0);
    sysctlbyname("hw.perflevel0.logicalcpu", &pc, &n, nullptr, 0);
    sysctlbyname("hw.perflevel1.logicalcpu", &ec, &n, nullptr, 0);
    uint64_t mem = 0;
    n = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &n, nullptr, 0);
    int translated = 0;
    n = sizeof translated;
    sysctlbyname("sysctl.proc_translated", &translated, &n, nullptr, 0);
    const VmSnap v = vm_snap();
    std::printf(
        "SYSINFO arch=%s translated=%d cpu=\"%s\" ncpu=%d perf=%d eff=%d mem_gib=%.1f kernel_page=%llu getpagesize=%zu free_mib=%llu file_mib=%llu anon_mib=%llu compressor_mib=%llu pressure_level=%d\n",
#if defined(__arm64__)
        "arm64",
#else
        "x86_64",
#endif
        translated, brand, ncpu, pc, ec, mem / double(1ull << 30), static_cast<unsigned long long>(v.page), host_page(), static_cast<unsigned long long>(v.mib(v.free)),
        static_cast<unsigned long long>(v.mib(v.file)), static_cast<unsigned long long>(v.mib(v.anon)), static_cast<unsigned long long>(v.mib(v.compressor)), pressure_level());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: iobench shapes|info|read|bulk|decode|model|pressure|sysinfo ...\n");
        return 2;
    }
    const std::string cmd = argv[1];
    Args a(argc, argv, 2);
    if (cmd == "shapes") return cmd_shapes(a);
    if (cmd == "info") return cmd_info(a);
    if (cmd == "read") return cmd_read(a);
    if (cmd == "bulk") return cmd_bulk(a);
    if (cmd == "sysinfo") return cmd_sysinfo(a);
    if (cmd == "export") return cmd_export(a);
    if (cmd == "membw") return cmd_membw(a);
    if (cmd == "decode") return cmd_decode(a);
    if (cmd == "model") return cmd_model(a);
    if (cmd == "pressure") return cmd_pressure(a);
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
