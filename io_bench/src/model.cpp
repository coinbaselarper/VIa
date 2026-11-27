#include <condition_variable>
#include <deque>

#include "common.h"
#include "inflate.h"
#include "reader.h"
#include "shapes.h"

namespace iob {

namespace {

struct MReq {
    Req r;
    uint32_t q = 0;
    uint32_t seq = 0;
    double t0 = 0, t1 = 0;
};

struct ModelCfg {
    std::string design = "current";
    std::string impl = "leonida";
    int queues = 3;
    int helpers_cap = 2;
    uint64_t window = 64ull << 20;
    uint64_t prep_limit = 16ull << 20;
    int pool_threads = 8;
    bool nocache = false;
    int rdahead = -1;
    bool fresh = true;
    bool zero_fill = true;
    int advise = 0;
    bool evict_behind = false;
};

uint64_t staging_bytes(const Req& r) { return static_cast<uint64_t>(r.src) + r.dst; }

class Window {
public:
    explicit Window(uint64_t limit) : limit_(limit) {}
    bool try_pair(uint64_t bytes, int cap) {
        std::lock_guard<std::mutex> lk(m_);
        if (helpers_ >= cap || next_ != serving_ || bytes > limit_ || used_ > limit_ - bytes) return false;
        ++helpers_;
        take(bytes);
        return true;
    }
    void acquire(uint64_t bytes) {
        std::unique_lock<std::mutex> lk(m_);
        const uint64_t t = next_++;
        c_.wait(lk, [&] { return t == serving_ && (!used_ || (bytes <= limit_ && used_ <= limit_ - bytes)); });
        take(bytes);
        ++serving_;
        if (next_ != serving_) c_.notify_all();
    }
    void acquire_ordered(uint64_t bytes) {
        std::unique_lock<std::mutex> lk(m_);
        c_.wait(lk, [&] { return !used_ || (bytes <= limit_ && used_ <= limit_ - bytes); });
        take(bytes);
    }
    void release(uint64_t bytes, bool pair) {
        std::lock_guard<std::mutex> lk(m_);
        used_ -= bytes;
        if (pair) --helpers_;
        c_.notify_all();
    }
    uint64_t peak() const { return peak_; }

private:
    void take(uint64_t b) {
        used_ += b;
        peak_ = std::max(peak_, used_);
    }
    std::mutex m_;
    std::condition_variable c_;
    uint64_t limit_, used_ = 0, peak_ = 0, next_ = 0, serving_ = 0;
    int helpers_ = 0;
};

struct Env {
    std::vector<int> fds;
    ModelCfg cfg;
    std::vector<std::vector<MReq*>> queues;
    std::vector<uint8_t*> dest;
    size_t dest_size = 0;
    std::atomic<uint64_t> failed{0};
};

bool read_exact(int fd, uint8_t* buf, uint64_t off, size_t n) {
    size_t d = 0;
    while (d < n) {
        ssize_t k = pread(fd, buf + d, n - d, static_cast<off_t>(off + d));
        if (k <= 0) return false;
        d += static_cast<size_t>(k);
    }
    return true;
}

void advise_req(const Env& env, const MReq& m) {
    radvisory ra;
    ra.ra_offset = static_cast<off_t>(m.r.off);
    ra.ra_count = static_cast<int>(m.r.src);
    fcntl(env.fds[m.r.file], F_RDADVISE, &ra);
}

struct Prepared {
    std::vector<uint8_t> data;
    std::vector<uint8_t> inflated;
    const uint8_t* result = nullptr;
    bool ok = false;
};

void prepare_current(Env& env, Inflater& inf, MReq& m, Prepared& p) {
    const Req& r = m.r;
    if (env.cfg.zero_fill) p.data.assign(r.src, 0);
    else p.data.resize(r.src);
    if (!read_exact(env.fds[r.file], p.data.data(), r.off, r.src)) {
        p.ok = false;
        return;
    }
    if (env.cfg.evict_behind) evict_range(env.fds[r.file], r.off, r.src);
    p.ok = inf.run(p.data.data(), r.src, r.dst, env.cfg.fresh, p.result);
}

void publish(Env& env, const Prepared& p, MReq& m) {
    if (p.ok) std::memcpy(env.dest[m.q], p.result, std::min<size_t>(m.r.dst, env.dest_size));
    else env.failed++;
}

struct Helper {
    std::mutex m;
    std::condition_variable c;
    MReq* job = nullptr;
    Prepared* out = nullptr;
    bool busy = false, stop = false;
    std::thread th;
    void start(Env* env) {
        th = std::thread([this, env] {
            Inflater inf(env->cfg.impl);
            std::unique_lock<std::mutex> lk(m);
            for (;;) {
                c.wait(lk, [&] { return job || stop; });
                if (stop && !job) return;
                MReq* j = job;
                lk.unlock();
                prepare_current(*env, inf, *j, *out);
                lk.lock();
                job = nullptr;
                busy = false;
                c.notify_all();
            }
        });
    }
    void submit(MReq* j, Prepared* o) {
        std::lock_guard<std::mutex> lk(m);
        job = j;
        out = o;
        busy = true;
        c.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lk(m);
        c.wait(lk, [&] { return !busy; });
    }
    void join() {
        {
            std::lock_guard<std::mutex> lk(m);
            stop = true;
            c.notify_all();
        }
        th.join();
    }
};

void run_current(Env& env, Window& win) {
    std::vector<std::thread> th;
    for (size_t q = 0; q < env.queues.size(); ++q)
        th.emplace_back([&, q] {
            Inflater inf(env.cfg.impl);
            Helper helper;
            helper.start(&env);
            auto& L = env.queues[q];
            auto eligible = [&](const MReq& m) { return staging_bytes(m.r) <= env.cfg.prep_limit; };
            for (size_t i = 0; i < L.size();) {
                MReq& head = *L[i];
                bool paired = false;
                const bool can = i + 1 < L.size() && eligible(head) && eligible(*L[i + 1]);
                const uint64_t pair_bytes = can ? staging_bytes(head.r) + staging_bytes(L[i + 1]->r) : 0;
                if (can) paired = win.try_pair(pair_bytes, env.cfg.helpers_cap);
                Prepared hp, np;
                if (env.cfg.advise > 0) {
                    const size_t j = i + (paired ? 2 : 1) - 1 + env.cfg.advise;
                    if (j < L.size()) advise_req(env, *L[j]);
                    if (paired && j + 1 < L.size()) advise_req(env, *L[j + 1]);
                }
                head.t0 = now_s();
                if (paired) {
                    L[i + 1]->t0 = head.t0;
                    helper.submit(L[i + 1], &np);
                } else {
                    win.acquire(staging_bytes(head.r));
                }
                prepare_current(env, inf, head, hp);
                publish(env, hp, head);
                head.t1 = now_s();
                if (paired) {
                    helper.wait();
                    publish(env, np, *L[i + 1]);
                    L[i + 1]->t1 = now_s();
                    win.release(pair_bytes, true);
                    i += 2;
                } else {
                    win.release(staging_bytes(head.r), false);
                    i += 1;
                }
            }
            helper.join();
        });
    for (auto& t : th) t.join();
}

void run_pool(Env& env, Window& win) {
    std::vector<MReq*> order;
    {
        size_t maxlen = 0;
        for (auto& q : env.queues) maxlen = std::max(maxlen, q.size());
        for (size_t i = 0; i < maxlen; ++i)
            for (auto& q : env.queues)
                if (i < q.size()) order.push_back(q[i]);
    }
    std::mutex pm;
    size_t next = 0;
    struct Done {
        std::vector<uint8_t> buf;
        bool ok = false;
        MReq* m = nullptr;
    };
    struct Turn {
        std::mutex m;
        std::map<uint32_t, Done> parked;
        uint32_t next_pub = 0;
    };
    std::vector<Turn> turns(env.queues.size());
    std::mutex pool_m;
    std::vector<std::vector<uint8_t>> spare;
    std::vector<std::thread> th;
    for (int t = 0; t < env.cfg.pool_threads; ++t)
        th.emplace_back([&] {
            Inflater inf(env.cfg.impl);
            std::vector<uint8_t> in;
            for (;;) {
                MReq* m;
                {
                    std::unique_lock<std::mutex> lk(pm);
                    if (next >= order.size()) return;
                    m = order[next++];
                    if (env.cfg.advise > 0 && next - 1 + env.cfg.advise * env.queues.size() < order.size()) advise_req(env, *order[next - 1 + env.cfg.advise * env.queues.size()]);
                    win.acquire_ordered(staging_bytes(m->r));
                }
                {
                    std::lock_guard<std::mutex> lk(pool_m);
                    if (!spare.empty()) {
                        inf.give(std::move(spare.back()));
                        spare.pop_back();
                    }
                }
                m->t0 = now_s();
                if (in.size() < m->r.src) in.resize(m->r.src);
                const uint8_t* res = nullptr;
                const bool rd = read_exact(env.fds[m->r.file], in.data(), m->r.off, m->r.src);
                if (env.cfg.evict_behind) evict_range(env.fds[m->r.file], m->r.off, m->r.src);
                Done d;
                d.m = m;
                d.ok = rd && inf.run(in.data(), m->r.src, m->r.dst, env.cfg.fresh, res);
                d.buf = inf.take();
                Turn& tn = turns[m->q];
                std::vector<std::vector<uint8_t>> recycled;
                {
                    std::lock_guard<std::mutex> lk(tn.m);
                    tn.parked.emplace(m->seq, std::move(d));
                    for (auto it = tn.parked.find(tn.next_pub); it != tn.parked.end(); it = tn.parked.find(tn.next_pub)) {
                        Done& x = it->second;
                        if (x.ok) std::memcpy(env.dest[x.m->q], x.buf.data(), std::min<size_t>(x.m->r.dst, env.dest_size));
                        else env.failed++;
                        x.m->t1 = now_s();
                        win.release(staging_bytes(x.m->r), false);
                        recycled.push_back(std::move(x.buf));
                        tn.parked.erase(it);
                        ++tn.next_pub;
                    }
                }
                std::lock_guard<std::mutex> lk(pool_m);
                for (auto& v : recycled) spare.push_back(std::move(v));
            }
        });
    for (auto& t : th) t.join();
}

}  // namespace

int cmd_model(const Args& a) {
    if (a.pos.empty()) return 2;
    Shape s;
    if (!shape_load(a.pos[0], s)) return 1;
    const char* ge = std::getenv("IOB_GAME");
    const std::string game = a.str("game", ge ? ge : "/Users/a/Desktop/Projects/GTA-VI/xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK");
    std::vector<std::string> paths;
    for (const auto& f : s.files) paths.push_back(f[0] == '/' ? f : game + "/" + f);
    const std::string ord = a.str("order", "runs");
    std::vector<Req> sel;
    for (const auto& r : s.reqs)
        if (r.codec == kZlib) sel.push_back(r);
    sel = make_order(sel, ord == "rand" ? Order::Rand : ord == "seq" ? Order::Seq : Order::Runs, static_cast<uint64_t>(a.num("seed", 1)), a.real("run-mean", 16));
    const uint64_t limit = static_cast<uint64_t>(a.num("limit-mib", 256)) << 20;
    std::vector<Req> use;
    uint64_t sum = 0;
    for (const auto& r : sel) {
        if (sum >= limit) break;
        use.push_back(r);
        sum += r.src;
    }
    std::vector<std::string> designs, impls;
    auto split_s = [](const std::string& x) {
        std::vector<std::string> v;
        size_t i = 0;
        while (i <= x.size()) {
            size_t j = x.find(',', i);
            if (j == std::string::npos) j = x.size();
            v.push_back(x.substr(i, j - i));
            i = j + 1;
        }
        return v;
    };
    designs = split_s(a.str("design", "current"));
    impls = split_s(a.str("impl", "leonida"));
    const auto queues = split_nums(a.str("queues", "3"));
    const auto windows = split_nums(a.str("window-mib", "64"));
    const auto pools = split_nums(a.str("pool", "8"));
    const auto caches = split_s(a.str("cache", "cache"));
    const int trials = static_cast<int>(a.num("trials", 1));
    size_t maxdst = 0;
    for (const auto& r : use) maxdst = std::max<size_t>(maxdst, r.dst);
    for (const auto& design : designs)
        for (const auto& impl : impls)
            for (long long nq : queues)
                for (long long wm : windows)
                    for (long long pt : pools)
                        for (const auto& cm : caches)
                            for (int trial = 0; trial < trials; ++trial) {
                                ModelCfg cfg;
                                cfg.design = design;
                                cfg.impl = impl;
                                cfg.queues = static_cast<int>(nq);
                                cfg.window = static_cast<uint64_t>(wm) << 20;
                                cfg.pool_threads = static_cast<int>(pt);
                                cfg.nocache = cm == "nocache";
                                cfg.rdahead = static_cast<int>(a.num("rdahead", -1));
                                cfg.helpers_cap = static_cast<int>(a.num("helpers", 2));
                                cfg.advise = static_cast<int>(a.num("advise", 0));
                                cfg.evict_behind = a.num("evict-behind", 0) != 0;
                                cfg.fresh = design == "current";
                                cfg.zero_fill = design == "current";
                                if (a.has("fresh")) cfg.fresh = a.num("fresh", 1) != 0;
                                if (a.has("zero-fill")) cfg.zero_fill = a.num("zero-fill", 1) != 0;
                                evict_requests(paths, use);
                                Env env;
                                env.cfg = cfg;
                                for (const auto& p : paths) {
                                    int fd = open(p.c_str(), O_RDONLY);
                                    if (cfg.nocache) fcntl(fd, F_NOCACHE, 1);
                                    if (cfg.rdahead >= 0) fcntl(fd, F_RDAHEAD, cfg.rdahead);
                                    env.fds.push_back(fd);
                                }
                                std::vector<MReq> reqs(use.size());
                                env.queues.assign(cfg.queues, {});
                                std::vector<uint32_t> seqs(cfg.queues, 0);
                                for (size_t i = 0; i < use.size(); ++i) {
                                    reqs[i].r = use[i];
                                    reqs[i].q = static_cast<uint32_t>(i % cfg.queues);
                                    reqs[i].seq = seqs[reqs[i].q]++;
                                    env.queues[reqs[i].q].push_back(&reqs[i]);
                                }
                                env.dest_size = maxdst;
                                for (int q = 0; q < cfg.queues; ++q) env.dest.push_back(alloc_prefaulted(maxdst));
                                Window win(cfg.window);
                                const VmSnap v0 = vm_snap();
                                const Cpu c0 = cpu_now();
                                const double t0 = now_s();
                                if (cfg.design == "current") run_current(env, win);
                                else run_pool(env, win);
                                const double wall = now_s() - t0;
                                const Cpu c1 = cpu_now();
                                const VmSnap v1 = vm_snap();
                                std::vector<double> lat, done;
                                double dst_bytes = 0;
                                for (const auto& m : reqs) {
                                    lat.push_back((m.t1 - m.t0) * 1e3);
                                    done.push_back((m.t1 - t0) * 1e3);
                                    dst_bytes += m.r.dst;
                                }
                                const Pct pl = percentiles(lat);
                                const double sm = static_cast<double>(sum) / (1 << 20), dm = dst_bytes / (1 << 20);
                                std::printf(
                                    "RESULT bench=model design=%s impl=%s queues=%d window_mib=%llu pool=%d cache=%s advise=%d eb=%d zero_fill=%d fresh=%d order=%s n=%zu src_mib=%.1f dst_mib=%.1f wall_s=%.3f src_mibs=%.0f out_mibs=%.0f "
                                    "svc_p50_ms=%.3f svc_p99_ms=%.3f cpu_s=%.3f cpu_s_per_gib_out=%.3f peak_staging_mib=%.1f file_pages_delta_mib=%lld failed=%llu shape=%s\n",
                                    design.c_str(), impl.c_str(), cfg.queues, static_cast<unsigned long long>(cfg.window >> 20), design == "pool" ? cfg.pool_threads : 0,
                                    cfg.nocache ? "nocache" : "cache", cfg.advise, cfg.evict_behind ? 1 : 0, cfg.zero_fill ? 1 : 0, cfg.fresh ? 1 : 0, ord.c_str(), reqs.size(), sm, dm, wall, sm / wall, dm / wall, pl.p50, pl.p99, c1.total() - c0.total(),
                                    (c1.total() - c0.total()) / (dm / 1024), win.peak() / double(1 << 20),
                                    (static_cast<long long>(v1.file) - static_cast<long long>(v0.file)) * static_cast<long long>(v0.page) / (1 << 20),
                                    static_cast<unsigned long long>(env.failed.load()), a.pos[0].c_str());
                                std::fflush(stdout);
                                for (auto d : env.dest) free_region(d, maxdst);
                                for (int fd : env.fds) close(fd);
                                evict_requests(paths, use);
                            }
    return 0;
}

}  // namespace iob
