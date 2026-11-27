#include <pthread/qos.h>
#include <memory>
#include <zlib.h>

#include "common.h"
#include "inflate.h"
#include "shapes.h"

#ifdef IOB_HAVE_LEONIDA
#include "bcpack.h"
#endif

namespace iob {

namespace {

struct Item {
    const uint8_t* src;
    uint32_t src_size;
    uint32_t dst_size;
};

struct Work {
    std::vector<uint8_t> blob;
    std::vector<Item> items;
    std::vector<Req> reqs;
    std::vector<std::string> files;
    uint64_t src_bytes = 0, dst_bytes = 0;
};

bool load_work(const std::string& shape_path, const std::string& game, const std::string& codec, uint64_t limit, int skip_mod, Work& w) {
    Shape s;
    if (!shape_load(shape_path, s)) return false;
    const uint8_t want = codec == "zlib" ? kZlib : static_cast<uint8_t>(kBcpackBase + std::stoi(codec.substr(6)));
    std::vector<Req> sel;
    uint64_t sum = 0;
    for (const auto& r : s.reqs) {
        if (r.codec != want) continue;
        if (skip_mod > 1 && (r.group % skip_mod) != 0) continue;
        if (sum >= limit) break;
        sel.push_back(r);
        sum += r.src;
    }
    w.blob.resize(sum + 16 * sel.size() + 64);
    std::vector<int> fds(s.files.size(), -1);
    size_t pos = 0;
    std::vector<size_t> offs;
    for (const auto& r : sel) {
        if (fds[r.file] < 0) {
            fds[r.file] = open((s.files[r.file][0] == '/' ? s.files[r.file] : game + "/" + s.files[r.file]).c_str(), O_RDONLY);
            if (fds[r.file] < 0) return false;
            fcntl(fds[r.file], F_NOCACHE, 1);
        }
        size_t d = 0;
        while (d < r.src) {
            ssize_t n = pread(fds[r.file], w.blob.data() + pos + d, r.src - d, static_cast<off_t>(r.off + d));
            if (n <= 0) return false;
            d += static_cast<size_t>(n);
        }
        offs.push_back(pos);
        pos += (r.src + 15) & ~size_t{15};
    }
    w.reqs = sel;
    w.files = s.files;
    for (size_t i = 0; i < sel.size(); ++i) {
        w.items.push_back({w.blob.data() + offs[i], sel[i].src, sel[i].dst});
        w.src_bytes += sel[i].src;
        w.dst_bytes += sel[i].dst;
    }
    for (int fd : fds)
        if (fd >= 0) close(fd);
    return true;
}

uint32_t qos_class(const std::string& q) {
    if (q == "interactive") return QOS_CLASS_USER_INTERACTIVE;
    if (q == "initiated") return QOS_CLASS_USER_INITIATED;
    if (q == "utility") return QOS_CLASS_UTILITY;
    if (q == "background") return QOS_CLASS_BACKGROUND;
    return QOS_CLASS_UNSPECIFIED;
}

struct Policy {
    const void* history = nullptr;
    bool fitted = true;
};

struct Outcome {
    double wall = 0;
    std::vector<const char*> why;
    uint64_t confirmed = 0, fitted = 0;
    Cpu cpu;
    std::vector<double> lat_ms;
    uint64_t failed = 0;
    uint32_t digest = 0;
};

Outcome run_parallel(const Work& w, int threads, const std::string& qos, bool fresh, const std::string& impl, const std::string& codec, const Policy& pol) {
    Outcome o;
    o.lat_ms.assign(w.items.size(), 0);
    o.why.assign(w.items.size(), nullptr);
    std::atomic<uint64_t> n_confirmed{0}, n_fitted{0};
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint32_t> digest{0};
    size_t maxdst = 0;
    for (const auto& it : w.items) maxdst = std::max<size_t>(maxdst, it.dst_size);
    const Cpu c0 = cpu_now();
    const double t0 = now_s();
    auto worker = [&] {
        if (qos_class(qos) != QOS_CLASS_UNSPECIFIED) pthread_set_qos_class_self_np(static_cast<qos_class_t>(qos_class(qos)), 0);
        std::vector<uint8_t> out;
        if (!fresh) out.resize(maxdst + 64);
        Inflater inf(impl);
        uint32_t local_digest = 0;
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= w.items.size()) break;
            const Item& it = w.items[i];
            const double a = now_s();
            bool ok = false;
            const uint8_t* result = nullptr;
            if (codec == "zlib") {
                ok = inf.run(it.src, it.src_size, it.dst_size, fresh, result);
            }
#ifdef IOB_HAVE_LEONIDA
            else {
                if (fresh) out.assign(it.dst_size, 0);
                const char* failure = nullptr;
                const auto q = leonida::bcpack::decode(static_cast<unsigned>(std::stoi(codec.substr(6))), std::span<const uint8_t>(it.src, it.src_size),
                                                       std::span<uint8_t>(out.data(), it.dst_size), pol.fitted,
                                                       static_cast<const leonida::bcpack::Calibration*>(pol.history), &failure, true);
                ok = q != leonida::bcpack::Quality::failed;
                if (!ok) o.why[i] = failure;
                (q == leonida::bcpack::Quality::fitted ? n_fitted : n_confirmed) += ok;
                result = out.data();
            }
#endif
            if (result && ok) local_digest ^= static_cast<uint32_t>(crc32(0, result, static_cast<uInt>(std::min<size_t>(it.dst_size, 4096))));
            if (!ok) failed++;
            o.lat_ms[i] = (now_s() - a) * 1e3;
        }
        digest ^= local_digest;
    };
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t) th.emplace_back(worker);
    for (auto& t : th) t.join();
    o.wall = now_s() - t0;
    const Cpu c1 = cpu_now();
    o.cpu = {c1.user - c0.user, c1.sys - c0.sys};
    o.failed = failed;
    o.confirmed = n_confirmed;
    o.fitted = n_fitted;
    o.digest = digest;
    return o;
}

}  // namespace

int cmd_decode(const Args& a) {
    if (a.pos.empty()) return 2;
    const char* ge = std::getenv("IOB_GAME");
    const std::string game = a.str("game", ge ? ge : "/Users/a/Desktop/Projects/GTA-VI/xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK");
    const std::string codec = a.str("codec", "zlib");
    Work w;
    if (!load_work(a.pos[0], game, codec, static_cast<uint64_t>(a.num("limit-mib", 128)) << 20, static_cast<int>(a.num("group-mod", 1)), w)) {
        std::fprintf(stderr, "cannot load work\n");
        return 1;
    }
    std::vector<std::string> impls;
    {
        std::string s = a.str("impl", codec == "zlib" ? "libz,leonida" : "leonida");
        size_t i = 0;
        while (i <= s.size()) {
            size_t j = s.find(',', i);
            if (j == std::string::npos) j = s.size();
            impls.push_back(s.substr(i, j - i));
            i = j + 1;
        }
    }
    const auto threads = split_nums(a.str("threads", "1"));
    const int reps = static_cast<int>(a.num("reps", 3));
    const bool fresh = a.num("fresh", 0) != 0;
    const std::string qos = a.str("qos", "");
    Policy pol;
    pol.fitted = a.num("fitted", 1) != 0;
    const std::string history_path = a.str("history", "");
#ifdef IOB_HAVE_LEONIDA
    std::unique_ptr<leonida::bcpack::Calibration> history;
    if (!history_path.empty()) {
        history = std::make_unique<leonida::bcpack::Calibration>();
        FILE* f = std::fopen(history_path.c_str(), "rb");
        char magic[8];
        if (!f || std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "BCPHIST1", 8) || std::fread(history.get(), 1, sizeof *history, f) != sizeof *history || std::fgetc(f) != EOF) {
            std::fprintf(stderr, "cannot read BCPack profile %s\n", history_path.c_str());
            return 1;
        }
        std::fclose(f);
        pol.history = history.get();
    }
#else
    if (!history_path.empty()) {
        std::fprintf(stderr, "--history needs the leonida codecs (IOB_HAVE_LEONIDA)\n");
        return 1;
    }
#endif
    const std::string failures_out = a.str("failures-out", "");
    for (const auto& impl : impls)
        for (long long t : threads) {
            std::vector<double> mibs_out;
            Outcome best;
            std::vector<double> walls;
            for (int r = 0; r < reps; ++r) {
                Outcome o = run_parallel(w, static_cast<int>(t), qos, fresh, impl, codec, pol);
                walls.push_back(o.wall);
                if (best.wall == 0 || o.wall < best.wall) best = o;
            }
            std::sort(walls.begin(), walls.end());
            const double med = walls[walls.size() / 2];
            const Pct p = percentiles(best.lat_ms);
            std::printf(
                "RESULT bench=decode codec=%s impl=%s threads=%lld fresh=%d qos=%s n=%zu src_mib=%.1f dst_mib=%.1f med_wall_s=%.3f best_wall_s=%.3f out_mibs_med=%.0f out_mibs_best=%.0f "
                "in_mibs_med=%.0f p50_ms=%.3f p99_ms=%.3f cpu_s_per_gib_out=%.3f failed=%llu digest=%08x shape=%s\n",
                codec.c_str(), impl.c_str(), t, fresh, qos.empty() ? "default" : qos.c_str(), w.items.size(), w.src_bytes / double(1 << 20), w.dst_bytes / double(1 << 20), med,
                best.wall, w.dst_bytes / double(1 << 20) / med, w.dst_bytes / double(1 << 20) / best.wall, w.src_bytes / double(1 << 20) / med, p.p50, p.p99,
                best.cpu.total() / (w.dst_bytes / double(1ull << 30)), static_cast<unsigned long long>(best.failed), best.digest, a.pos[0].c_str());
            if (codec != "zlib") {
                std::map<std::string, std::pair<uint64_t, uint64_t>> by;
                for (size_t i = 0; i < best.why.size(); ++i)
                    if (best.why[i]) {
                        auto& c = by[best.why[i]];
                        ++c.first;
                        c.second += w.items[i].src_size;
                    }
                std::printf("RESULT bench=decode_quality codec=%s impl=%s profile=%s fitted_allowed=%d n=%zu confirmed=%llu fitted=%llu failed=%llu", codec.c_str(), impl.c_str(),
                            history_path.empty() ? "none" : "loaded", int(pol.fitted), w.items.size(), static_cast<unsigned long long>(best.confirmed),
                            static_cast<unsigned long long>(best.fitted), static_cast<unsigned long long>(best.failed));
                for (const auto& [why, c] : by) std::printf(" fail_%s=%llu fail_%s_src_kib=%llu", why.c_str(), static_cast<unsigned long long>(c.first), why.c_str(), static_cast<unsigned long long>(c.second >> 10));
                std::printf(" shape=%s\n", a.pos[0].c_str());
                if (!failures_out.empty() && t == threads.front() && impl == impls.front()) {
                    if (FILE* fo = std::fopen(failures_out.c_str(), "w")) {
                        for (size_t i = 0; i < best.why.size(); ++i)
                            if (best.why[i])
                                std::fprintf(fo, "%s\t%llu\t%u\t%u\t%s\t%u\n", w.files[w.reqs[i].file].c_str(), static_cast<unsigned long long>(w.reqs[i].off), w.items[i].src_size,
                                             w.items[i].dst_size, best.why[i], w.reqs[i].group);
                        std::fclose(fo);
                    }
                }
            }
            std::fflush(stdout);
        }
    return 0;
}

}  // namespace iob
