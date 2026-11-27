#include "pcache.h"
#include "phase.h"

#include <gta6rt/governor.h>
#include <gta6rt_metal/metal_backend.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <pthread.h>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <unordered_map>

namespace gta6rt::pcache {

namespace {

enum class HeldState { queued, waiting_lib, building, ready, failed, dropped, adopted };

struct Held {
    Spec spec;
    std::string text, key, label;
    Phase phase = Phase::boot;
    std::uint64_t seq = 0;
    HeldState state = HeldState::queued;
    bool hold = true;
    bool bootstrap = false;
    double t_start = 0;
    std::unique_ptr<gpu::RenderPipeline> render;
    std::unique_ptr<gpu::MeshPipeline> mesh;
    std::unique_ptr<gpu::ComputePipeline> compute;
    gpu::Pipeline* pipeline() const {
        return render ? static_cast<gpu::Pipeline*>(render.get())
               : mesh ? static_cast<gpu::Pipeline*>(mesh.get())
                      : static_cast<gpu::Pipeline*>(compute.get());
    }
    void release() {
        render.reset();
        mesh.reset();
        compute.reset();
    }
};

struct Watch {
    const void* owner;
    gpu::Pipeline* pipeline;
    std::string key;
    double t;
    bool adopted;
};

struct Stats {
    std::uint64_t first_uses = 0, adopted_ready = 0, adopted_compiling = 0, created = 0, demanded_queued = 0;
    std::uint64_t demand_ready_from_archive = 0, demand_compiled = 0, demand_failed = 0;
    std::uint64_t prebuilt = 0, prebuilt_from_archive = 0, prebuild_failed = 0, dropped = 0, bootstrap_matched = 0;
    std::uint64_t waits = 0;
    double wait_sum = 0, wait_longest = 0, prebuild_seconds = 0;
    std::string wait_longest_label;
    std::map<std::string, std::uint64_t> failures;
};

struct State {
    std::mutex m;
    std::condition_variable cv;
    StartConfig cfg;
    bool started = false, stopping = false, paused = false;
    std::atomic<bool> ready{false};
    std::string manifest_path, run_path;
    Manifest prior, run;
    bool dirty = false;
    std::unordered_map<std::string, Phase> boot_keys, boot_labels;
    std::unordered_map<std::string, unsigned> label_uses;
    std::unordered_map<std::string, std::unique_ptr<Held>> table;
    // (speculative, phase, first-use order, key): every recorded pipeline before any bootstrap guess.
    std::set<std::tuple<int, int, std::uint64_t, std::string>> queue;
    std::uint64_t seq = 0;
    std::vector<Held*> in_flight;
    std::vector<Watch> watch;
    Phase phase = Phase::boot;
    bool movie = false;
    double quiet_until = 0, quiet_s = 10;
    double world_t0 = 0, world_next = 0;
    std::uint64_t world_skips0 = 0, world_first_uses0 = 0, world_ready0 = 0, world_waits0 = 0;
    double world_wait_longest = 0;
    std::string world_wait_label;
    std::uint64_t flushed_pending = 0;
    std::size_t held_count = 0, max_hold = 4096;
    Stats stats;
    std::thread worker, monitor;
    std::string run_lines;
};

State& st() {
    static State* s = new State;
    return *s;
}

const char* stage_of(Kind k, int i) {
    if (i == 1) return "ps";
    return k == Kind::render ? "vs" : k == Kind::mesh ? "ms" : "cs";
}

std::string label_of(const Spec& s) {
    return s.names[1].empty() ? s.names[0] : s.names[0] + "," + s.names[1];
}

bool is_bink(const Spec& s) {
    for (const auto& n : s.names) {
        std::string l = n;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (l.find("bink") != std::string::npos) return true;
    }
    return false;
}

void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[rt] pcache: %s\n", buf);
}

// Called with the lock held.
std::tuple<int, int, std::uint64_t, std::string> queue_key(const Held& h) {
    return {h.bootstrap ? 1 : 0, int(h.phase), h.seq, h.key};
}

void enqueue_locked(State& s, Held& h) {
    h.state = HeldState::queued;
    s.queue.emplace(queue_key(h));
    s.cv.notify_all();
}

// The first seconds of the world (its opening cutscene) and of a movie: only pipelines recorded for the phase so
// far are prebuilt, no guesses and nothing of a later phase.
bool startable_locked(const State& s, const Held& h) {
    if (process_age_s() >= s.quiet_until) return true;
    return !h.bootstrap && int(h.phase) <= int(s.phase);
}

Held& add_locked(State& s, const std::string& key, const std::string& text, const Spec& spec, Phase phase, bool bootstrap) {
    auto [it, fresh] = s.table.try_emplace(key);
    if (fresh) {
        it->second = std::make_unique<Held>();
        Held& h = *it->second;
        h.key = key;
        h.text = text;
        h.spec = spec;
        h.label = label_of(spec) + " (pcache)";
        h.phase = phase;
        h.seq = ++s.seq;
        h.bootstrap = bootstrap;
        enqueue_locked(s, h);
    }
    return *it->second;
}

bool should_hold_locked(const State& s, const Held& h) {
    return s.held_count < s.max_hold;
}

void note_use_locked(State& s, const std::string& key, const std::string& text, const Spec& spec, const char* how) {
    if (s.run.find(key)) return;
    Entry e;
    e.key = key;
    if (s.phase == Phase::landing && !s.movie && is_bink(spec)) s.movie = true;
    e.phase = s.phase == Phase::landing && s.movie ? Phase::movie : s.phase;
    e.t = process_age_s();
    e.text = text;
    s.run.note(e);
    s.dirty = true;
    ++s.stats.first_uses;
    char t[32];
    std::snprintf(t, sizeof t, "%.3f", e.t);
    s.run_lines += std::string(t) + "\t" + phase_name(e.phase) + "\t" + key + "\t" + how + "\t" + label_of(spec) + "\n";
}

void write_locked(State& s, const char* why) {
    if (s.manifest_path.empty()) return;
    if (s.dirty) {
        Manifest merged = s.prior;
        merged.merge(s.run, true);
        std::string error;
        if (!merged.save(s.manifest_path, &error))
            log_line("manifest not written (%s): %s", why, error.c_str());
        else
            s.dirty = false;
    }
    if (!s.run_lines.empty() && !s.run_path.empty()) {
        if (FILE* f = std::fopen(s.run_path.c_str(), "a")) {
            std::fwrite(s.run_lines.data(), 1, s.run_lines.size(), f);
            std::fclose(f);
            s.run_lines.clear();
        }
    }
}

std::string report_locked(State& s) {
    const auto& t = s.stats;
    std::size_t queued = s.queue.size(), held = 0, waiting = 0;
    for (const auto& [k, h] : s.table) {
        if (h->state == HeldState::ready || (h->state == HeldState::building && h->pipeline())) ++held;
        if (h->state == HeldState::waiting_lib) ++waiting;
    }
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "t=%.0fs phase=%s: first uses %llu (adopted ready %llu, adopted compiling %llu, created at use %llu: "
                  "%llu from the archive, %llu compiled, %llu failed); prebuild: %zu known, %llu built (%llu archive "
                  "hits, %.1f s), %llu failed, %zu queued, %zu waiting for a shader, %zu holding, %llu dropped after "
                  "build, %llu bootstrap matches; first-use waits %llu (sum %.0f ms, longest %.0f ms: %s); manifest "
                  "%zu earlier + %zu this run",
                  process_age_s(), phase_name(s.phase), (unsigned long long)t.first_uses,
                  (unsigned long long)t.adopted_ready, (unsigned long long)t.adopted_compiling,
                  (unsigned long long)t.created, (unsigned long long)t.demand_ready_from_archive,
                  (unsigned long long)t.demand_compiled, (unsigned long long)t.demand_failed, s.table.size(),
                  (unsigned long long)t.prebuilt, (unsigned long long)t.prebuilt_from_archive, t.prebuild_seconds,
                  (unsigned long long)t.prebuild_failed, queued, waiting, held, (unsigned long long)t.dropped,
                  (unsigned long long)t.bootstrap_matched, (unsigned long long)t.waits, t.wait_sum * 1e3,
                  t.wait_longest * 1e3, t.wait_longest_label.empty() ? "-" : t.wait_longest_label.c_str(),
                  s.prior.size(), s.run.size());
    std::string out = buf;
    if (s.cfg.device) {
        const auto pc = s.cfg.device->pipeline_cache_stats();
        const auto ds = s.cfg.device->stats();
        const auto q = s.cfg.device->compile_queue_stats();
        std::snprintf(buf, sizeof buf,
                      "; archive %s: %u shards, %llu hits, %llu misses, %llu added (%llu not written yet), %llu bytes on "
                      "disk, longest shard write %.2f s, %llu write failures; build time: lookups %.1f s, compiles %.1f "
                      "s, archive adds %.1f s; compile queue: %llu running (%llu background, limits %u/%u), queued "
                      "%llu/%llu/%llu; draws skipped not ready %llu",
                      pc.open ? (pc.loaded ? "loaded" : "new") : "off", pc.shards, (unsigned long long)pc.hits,
                      (unsigned long long)pc.misses, (unsigned long long)pc.added, (unsigned long long)pc.pending,
                      (unsigned long long)pc.file_bytes, pc.serialize_longest, (unsigned long long)pc.flush_failures,
                      pc.lookup_seconds, pc.compile_seconds, pc.add_seconds, (unsigned long long)q.running,
                      (unsigned long long)q.running_background, q.active_limit, q.background_limit,
                      (unsigned long long)q.queued[0], (unsigned long long)q.queued[1], (unsigned long long)q.queued[2],
                      (unsigned long long)ds.draws_skipped_not_ready);
        out += buf;
    }
    return out;
}

struct Made {
    std::unique_ptr<gpu::RenderPipeline> render;
    std::unique_ptr<gpu::MeshPipeline> mesh;
    std::unique_ptr<gpu::ComputePipeline> compute;
    bool any() const { return render || mesh || compute; }
};

// Creates the backend pipeline for a held entry (libraries resolved, no lock held; `spec`, `key`, `label` are not
// changed by anyone once the entry exists).
bool create_held(State& s, const Held& h, Made& made, std::string& why) {
    gpu::ShaderRef refs[2];
    for (int i = 0; i < 2; ++i) {
        if (h.spec.names[i].empty()) continue;
        const std::string key = h.key;
        Library lib = s.cfg.libraries(h.spec.names[i], stage_of(h.spec.kind, i), [key] {
            State& s = st();
            std::lock_guard lock(s.m);
            auto it = s.table.find(key);
            if (it != s.table.end() && it->second->state == HeldState::waiting_lib) enqueue_locked(s, *it->second);
        });
        if (lib.pending) return false;
        if (!lib.library) {
            why = "no library for " + h.spec.names[i] + ": " + lib.error;
            return false;
        }
        refs[i] = {lib.library, lib.entry};
    }
    switch (h.spec.kind) {
    case Kind::render: {
        gpu::RenderPipelineDesc d = h.spec.render;
        d.vs = refs[0];
        d.ps = refs[1];
        d.root = s.cfg.root;
        d.mode = gpu::CompileMode::async;
        d.not_ready = gpu::NotReady::skip;
        d.priority = gpu::Priority::background;
        d.label = h.label.c_str();
        made.render = s.cfg.device->create_render_pipeline(d);
        break;
    }
    case Kind::mesh: {
        gpu::MeshPipelineDesc d = h.spec.mesh;
        d.mesh = refs[0];
        d.ps = refs[1];
        d.root = s.cfg.root;
        d.mode = gpu::CompileMode::async;
        d.not_ready = gpu::NotReady::skip;
        d.priority = gpu::Priority::background;
        d.label = h.label.c_str();
        made.mesh = s.cfg.device->create_mesh_pipeline(d);
        break;
    }
    case Kind::compute: {
        gpu::ComputePipelineDesc d = h.spec.compute;
        d.cs = refs[0];
        d.root = s.cfg.root;
        d.mode = gpu::CompileMode::async;
        d.not_ready = gpu::NotReady::skip;
        d.priority = gpu::Priority::background;
        d.label = h.label.c_str();
        made.compute = s.cfg.device->create_compute_pipeline(d);
        break;
    }
    }
    if (!made.any()) {
        why = "the backend refused the description";
        return false;
    }
    return true;
}

void worker_main() {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    pthread_setname_np("pcache prebuild");
    State& s = st();
    std::unique_lock lock(s.m);
    while (!s.stopping) {
        s.cv.wait_for(lock, std::chrono::milliseconds(250), [&] {
            return s.stopping || (!s.paused && !s.queue.empty() && s.in_flight.size() < s.cfg.max_in_flight);
        });
        if (s.stopping) break;
        if (s.paused || s.queue.empty() || s.in_flight.size() >= s.cfg.max_in_flight) continue;
        auto qi = s.queue.begin();
        while (qi != s.queue.end()) {
            auto t = s.table.find(std::get<3>(*qi));
            if (t == s.table.end() || startable_locked(s, *t->second)) break;
            ++qi;
        }
        if (qi == s.queue.end()) continue;
        const auto key = std::get<3>(*qi);
        s.queue.erase(qi);
        auto it = s.table.find(key);
        if (it == s.table.end() || it->second->state != HeldState::queued) continue;
        Held& h = *it->second;
        h.state = HeldState::building;
        h.hold = should_hold_locked(s, h);
        if (h.hold) ++s.held_count;
        h.t_start = process_age_s();
        lock.unlock();
        std::string why;
        Made made;
        const bool ok = create_held(s, h, made, why);
        lock.lock();
        if (h.state == HeldState::adopted) {
            if (h.hold) --s.held_count;
            lock.unlock();
            made = Made{};  // a first use created its own meanwhile
            lock.lock();
            continue;
        }
        if (!ok) {
            if (h.hold) --s.held_count;
            if (why.empty()) {
                h.state = HeldState::waiting_lib;
            } else {
                h.state = HeldState::failed;
                ++s.stats.prebuild_failed;
                ++s.stats.failures[why.substr(0, 80)];
            }
            continue;
        }
        h.render = std::move(made.render);
        h.mesh = std::move(made.mesh);
        h.compute = std::move(made.compute);
        s.in_flight.push_back(&h);
    }
}

void poll_locked(State& s) {
    const double now = process_age_s();
    for (std::size_t i = 0; i < s.in_flight.size();) {
        Held& h = *s.in_flight[i];
        gpu::Pipeline* p = h.state == HeldState::adopted ? nullptr : h.pipeline();
        const gpu::PipelineState ps = p ? p->state() : gpu::PipelineState::ready;
        if (p && ps != gpu::PipelineState::ready && ps != gpu::PipelineState::failed) {
            ++i;
            continue;
        }
        s.in_flight.erase(s.in_flight.begin() + std::ptrdiff_t(i));
        s.cv.notify_all();
        if (!p) continue;
        if (ps == gpu::PipelineState::failed) {
            if (s.stats.prebuild_failed < 5) log_line("prebuild failed (%s): %s", h.label.c_str(), p->error().substr(0, 300).c_str());
            ++s.stats.prebuild_failed;
            ++s.stats.failures[p->error().substr(0, 80)];
            h.state = HeldState::failed;
            if (h.hold) --s.held_count;
            h.release();
            continue;
        }
        ++s.stats.prebuilt;
        if (p->from_archive()) ++s.stats.prebuilt_from_archive;
        s.stats.prebuild_seconds += now - h.t_start;
        h.state = HeldState::ready;
        if (!h.hold) {
            h.release();
            h.state = HeldState::dropped;
            ++s.stats.dropped;
        }
    }
    for (std::size_t i = 0; i < s.watch.size();) {
        Watch& w = s.watch[i];
        const gpu::PipelineState ps = w.pipeline->state();
        if (ps != gpu::PipelineState::ready && ps != gpu::PipelineState::failed) {
            ++i;
            continue;
        }
        const double waited = now - w.t;
        if (ps == gpu::PipelineState::ready) {
            ++s.stats.waits;
            s.stats.wait_sum += waited;
            if (!w.adopted) ++(w.pipeline->from_archive() ? s.stats.demand_ready_from_archive : s.stats.demand_compiled);
            if (s.world_t0 > 0 && waited > s.world_wait_longest) {
                s.world_wait_longest = waited;
                s.world_wait_label = w.key;
                if (const Entry* e = s.run.find(w.key)) {
                    Spec spec;
                    if (decode(e->text, spec)) s.world_wait_label = label_of(spec);
                }
            }
            if (waited > s.stats.wait_longest) {
                s.stats.wait_longest = waited;
                auto it = s.table.find(w.key);
                s.stats.wait_longest_label = it != s.table.end() ? it->second->label : w.key;
                const Entry* e = s.run.find(w.key);
                if (e) {
                    Spec spec;
                    if (decode(e->text, spec)) s.stats.wait_longest_label = label_of(spec);
                }
            }
            char line[160];
            std::snprintf(line, sizeof line, "%.3f\twait\t%s\t%.1f ms\t%s\n", now, w.key.c_str(), waited * 1e3,
                          w.pipeline->from_archive() ? "archive" : "compiled");
            s.run_lines += line;
        } else {
            if (++s.stats.demand_failed <= 5)
                log_line("pipeline failed at first use: %s", w.pipeline->error().substr(0, 300).c_str());
        }
        s.watch.erase(s.watch.begin() + std::ptrdiff_t(i));
    }
}

void set_phase_locked(State& s, Phase p) {
    if (p == s.phase) return;
    const Phase before = s.phase;
    s.phase = p;
    if (p != Phase::landing) s.movie = false;
    if (p == Phase::world) {
        s.quiet_until = process_age_s() + s.quiet_s;
        s.world_t0 = s.world_next = process_age_s();
        s.world_skips0 = s.cfg.device ? s.cfg.device->stats().draws_skipped_not_ready : 0;
        s.world_first_uses0 = s.stats.first_uses;
        s.world_ready0 = s.stats.adopted_ready;
        s.world_waits0 = s.stats.waits;
    }
    log_line("phase %s -> %s at t=%.1fs", phase_name(before), phase_name(p), process_age_s());
    if (s.cfg.device) {
        const bool late = p == Phase::story_load || p == Phase::world;
        const std::uint32_t threads = std::max<std::uint32_t>(1, s.cfg.device->compile_queue_stats().active_limit);
        const char* v = std::getenv("GTA6RT_METAL_PCACHE_LATE_BACKGROUND");
        const std::uint32_t bg = late ? (v ? std::uint32_t(std::atoi(v)) : 1) : std::max<std::uint32_t>(1, threads - 1);
        if (bg) gta6rt::metal::set_compile_limits(s.cfg.device, 0, bg);
        gta6rt::metal::set_pipeline_cache_tag(s.cfg.device, phase_name(p));
    }
    for (auto& [k, h] : s.table)
        if (h->state == HeldState::dropped && int(h->phase) <= int(p) + 2) enqueue_locked(s, *h);
    write_locked(s, "phase change");
}

void monitor_main() {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    pthread_setname_np("pcache monitor");
    State& s = st();
    unsigned tick = 0;
    std::unique_lock lock(s.m);
    while (!s.stopping) {
        s.cv.wait_for(lock, std::chrono::milliseconds(10));
        if (s.stopping) break;
        poll_locked(s);
        ++tick;
        if (tick % 25 == 0) {
            Phase p;
            lock.unlock();
            const bool known = read_title_phase(p);
            lock.lock();
            if (known) set_phase_locked(s, p);
            const std::uint32_t level = gta6rt_governor_level ? gta6rt_governor_level() : 0;
            const bool pause = level >= 2;
            if (pause != s.paused) {
                s.paused = pause;
                log_line("prebuild %s (governor level %u)", pause ? "paused" : "resumed", level);
                s.cv.notify_all();
            }
        }
        // Every 2 s while there is something new: the world's first seconds are where runs end in a crash.
        if (tick % 200 == 0 && (s.dirty || !s.run_lines.empty())) write_locked(s, "periodic");
        // The world's first seconds: what was handed over ready, what is still compiling, draws dropped meanwhile.
        if (s.world_t0 > 0 && s.cfg.device && process_age_s() >= s.world_next) {
            const double rel = process_age_s() - s.world_t0;
            s.world_next = s.world_t0 + (rel < 14.5 ? std::floor(rel) + 1 : rel < 118 ? std::floor(rel / 5) * 5 + 5 : 1e30);
            const auto q = s.cfg.device->compile_queue_stats();
            const std::uint64_t skips = s.cfg.device->stats().draws_skipped_not_ready;
            log_line("world+%.0fs: draws skipped not ready %llu since the world began; first uses %llu (handed over ready "
                     "%llu, compiled or waited for %llu); first-use waits %llu, longest %.0f ms (%s); compiles running "
                     "%llu, queued %llu",
                     rel, (unsigned long long)(skips - s.world_skips0),
                     (unsigned long long)(s.stats.first_uses - s.world_first_uses0),
                     (unsigned long long)(s.stats.adopted_ready - s.world_ready0),
                     (unsigned long long)(s.stats.first_uses - s.world_first_uses0 - (s.stats.adopted_ready - s.world_ready0)),
                     (unsigned long long)(s.stats.waits - s.world_waits0), s.world_wait_longest * 1e3,
                     s.world_wait_label.empty() ? "-" : s.world_wait_label.c_str(), (unsigned long long)q.running,
                     (unsigned long long)(q.queued[0] + q.queued[1] + q.queued[2]));
        }
        if (tick % 3000 == 0) log_line("%s", report_locked(s).c_str());
        // A burst of new pipelines (the world's first seconds) is written at once, not at the next 20 s flush: runs
        // have ended there in a crash. The write holds no lock a compile needs.
        if (tick % 100 == 50 && s.cfg.device) {
            const auto pc = s.cfg.device->pipeline_cache_stats();
            if (pc.pending >= 64) {
                lock.unlock();
                std::string why;
                flush_archive(*s.cfg.device, &why, false);
                lock.lock();
            }
        }
    }
}

std::uint64_t governor_reclaim(void*, std::uint32_t level, std::uint64_t) {
    State& s = st();
    std::lock_guard lock(s.m);
    s.paused = level >= 1;
    std::uint64_t dropped = 0;
    if (level >= 2) {
        for (auto& [k, h] : s.table) {
            if (h->state != HeldState::ready) continue;
            h->release();
            h->state = HeldState::dropped;
            if (h->hold) --s.held_count;
            ++dropped;
        }
    }
    if (dropped) log_line("governor level %u: %llu held pipelines dropped (the archive keeps them)", level,
                          (unsigned long long)dropped);
    s.cv.notify_all();
    return 0;
}

void load_bootstrap(State& s, const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    std::size_t keys = 0, labels = 0;
    while (std::getline(in, line)) {
        if (line.size() < 4 || line[0] == '#') continue;
        const auto t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) continue;
        const auto phase = phase_from_name(line.substr(t1 + 1, t2 - t1 - 1));
        if (!phase) continue;
        const std::string what = line.substr(t2 + 1);
        if (line[0] == 'V') {
            s.boot_keys.emplace(what, *phase);
            ++keys;
        } else if (line[0] == 'L') {
            s.boot_labels.emplace(what, *phase);
            ++labels;
        }
    }
    log_line("bootstrap %s: %zu PSO keys (Vulkan boot manifests), %zu pipeline names (census logs)", path.c_str(), keys,
             labels);
}

template <class D>
void created_any(const std::string& pso_key, const std::string& label, const D& d, const std::string& n0,
                 const std::string& n1, bool compute) {
    State& s = st();
    if (!s.ready.load(std::memory_order_acquire)) return;
    const Phase* phase = nullptr;
    if (!pso_key.empty())
        if (auto it = s.boot_keys.find(pso_key); it != s.boot_keys.end()) phase = &it->second;
    if (!phase)
        if (auto it = s.boot_labels.find(label); it != s.boot_labels.end()) phase = &it->second;
    if (!phase) return;
    Spec spec;
    if constexpr (std::is_same_v<D, gpu::ComputePipelineDesc>)
        spec = spec_of(d, n0, s.cfg.content_key);
    else
        spec = spec_of(d, n0, n1, s.cfg.content_key);
    (void)compute;
    const std::string text = encode(spec), key = key_of(text);
    std::lock_guard lock(s.m);
    if (s.stopping || s.table.count(key)) return;
    if (++s.label_uses[label] > 4) return;
    ++s.stats.bootstrap_matched;
    add_locked(s, key, text, spec, *phase, true);
}

template <class P, class D>
std::unique_ptr<P> make_any(gpu::Device& device, const D& desc, const void* owner, const Spec& spec,
                            std::unique_ptr<P> Held::*member, std::unique_ptr<P> (gpu::Device::*create)(const D&)) {
    State& s = st();
    const std::string text = encode(spec), key = key_of(text);
    std::unique_ptr<P> out;
    bool bump = false;
    {
        std::lock_guard lock(s.m);
        auto it = s.table.find(key);
        if (it != s.table.end()) {
            Held& h = *it->second;
            if ((h.state == HeldState::ready || h.state == HeldState::building) && h.*member) {
                out = std::move(h.*member);
                const bool ready = out->state() == gpu::PipelineState::ready;
                ++(ready ? s.stats.adopted_ready : s.stats.adopted_compiling);
                bump = !ready;
                if (h.hold) --s.held_count;
                h.state = HeldState::adopted;
                note_use_locked(s, key, text, spec, ready ? "adopted-ready" : "adopted-compiling");
                if (bump) s.watch.push_back({owner, out.get(), key, process_age_s(), true});
            } else if (h.state == HeldState::queued || h.state == HeldState::waiting_lib ||
                       h.state == HeldState::building) {
                s.queue.erase(queue_key(h));
                h.state = HeldState::adopted;
                ++s.stats.demanded_queued;
            }
        }
    }
    if (out) {
        if (bump) {
            gpu::Pipeline* p = out.get();
            device.precompile(std::span<gpu::Pipeline* const>(&p, 1), gpu::Priority::demand);
        }
        return out;
    }
    out = (device.*create)(desc);
    std::lock_guard lock(s.m);
    ++s.stats.created;
    note_use_locked(s, key, text, spec, "created");
    if (out) s.watch.push_back({owner, out.get(), key, process_age_s(), false});
    return out;
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("GTA6RT_METAL_PCACHE");
        return !v || v[0] != '0';
    }();
    return on;
}

void start(const StartConfig& config) {
    if (!enabled() || !config.device || config.directory.empty()) return;
    State& s = st();
    std::lock_guard lock(s.m);
    if (s.started) return;
    s.started = true;
    s.cfg = config;
    if (const char* v = std::getenv("GTA6RT_METAL_PCACHE_INFLIGHT")) s.cfg.max_in_flight = unsigned(std::atoi(v));
    if (const char* v = std::getenv("GTA6RT_METAL_PCACHE_HOLD")) s.max_hold = std::size_t(std::atol(v));
    if (const char* v = std::getenv("GTA6RT_METAL_PCACHE_QUIET_S")) s.quiet_s = std::atof(v);
    ::mkdir(config.directory.c_str(), 0755);
    s.manifest_path = config.directory + "/pcache-manifest.txt";
    {
        const std::string runs = config.directory + "/pcache-runs";
        ::mkdir(runs.c_str(), 0755);
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
        s.run_path = runs + "/" + stamp + "-" + std::to_string(::getpid()) + ".tsv";
    }
    std::string error;
    struct stat sb {};
    if (::stat(s.manifest_path.c_str(), &sb) == 0) {
        if (!s.prior.load(s.manifest_path, &error)) {
            const std::string aside = s.manifest_path + ".unreadable";
            std::rename(s.manifest_path.c_str(), aside.c_str());
            log_line("%s; moved to %s, starting a new manifest", error.c_str(), aside.c_str());
            s.prior = Manifest{};
        }
    }
    std::size_t bad = 0;
    for (const Entry& e : s.prior.ordered()) {
        Spec spec;
        if (!decode(e.text, spec, &error)) {
            ++bad;
            continue;
        }
        Held& h = add_locked(s, e.key, e.text, spec, e.phase, false);
        h.seq = std::uint64_t(e.t * 1000);
    }
    s.queue.clear();
    for (auto& [k, h] : s.table) s.queue.emplace(queue_key(*h));
    load_bootstrap(s, config.directory + "/pcache-bootstrap.txt");
    log_line("manifest %s: %zu pipelines%s; prebuilding in phase order, %u at a time, holding up to %zu", s.manifest_path.c_str(),
             s.prior.size(), bad ? (" (" + std::to_string(bad) + " unreadable)").c_str() : "", s.cfg.max_in_flight,
             s.max_hold);
    if (gta6rt_governor_register) gta6rt_governor_register(GTA6RT_GOV_COMPILE_SHRINK, "pcache prebuild", governor_reclaim, nullptr);
    s.worker = std::thread(worker_main);
    s.monitor = std::thread(monitor_main);
    s.ready.store(true, std::memory_order_release);
}

void flush(const char* why) {
    State& s = st();
    std::lock_guard lock(s.m);
    if (!s.started) return;
    poll_locked(s);
    write_locked(s, why);
    log_line("%s (%s)", report_locked(s).c_str(), why);
}

void stop() {
    State& s = st();
    {
        std::lock_guard lock(s.m);
        if (!s.started || s.stopping) return;
        s.stopping = true;
        s.ready.store(false);
        write_locked(s, "stop");
    }
    s.cv.notify_all();
    if (s.worker.joinable()) s.worker.join();
    if (s.monitor.joinable()) s.monitor.join();
    std::lock_guard lock(s.m);
    s.watch.clear();
    s.in_flight.clear();
    for (auto& [k, h] : s.table) h->release();
}

void created(const void*, const std::string& pso_key, const std::string& label, const gpu::RenderPipelineDesc& d,
             const std::string& vs, const std::string& ps) {
    created_any(pso_key, label, d, vs, ps, false);
}
void created(const void*, const std::string& pso_key, const std::string& label, const gpu::MeshPipelineDesc& d,
             const std::string& ms, const std::string& ps) {
    created_any(pso_key, label, d, ms, ps, false);
}
void created(const void*, const std::string& pso_key, const std::string& label, const gpu::ComputePipelineDesc& d,
             const std::string& cs) {
    created_any(pso_key, label, d, cs, std::string(), true);
}

std::unique_ptr<gpu::RenderPipeline> make_render(gpu::Device& device, const gpu::RenderPipelineDesc& desc,
                                                 const void* owner, const std::string& vs, const std::string& ps) {
    State& s = st();
    if (!s.ready.load(std::memory_order_acquire)) return device.create_render_pipeline(desc);
    return make_any(device, desc, owner, spec_of(desc, vs, ps, s.cfg.content_key), &Held::render,
                    &gpu::Device::create_render_pipeline);
}

std::unique_ptr<gpu::MeshPipeline> make_mesh(gpu::Device& device, const gpu::MeshPipelineDesc& desc, const void* owner,
                                             const std::string& ms, const std::string& ps) {
    State& s = st();
    if (!s.ready.load(std::memory_order_acquire)) return device.create_mesh_pipeline(desc);
    return make_any(device, desc, owner, spec_of(desc, ms, ps, s.cfg.content_key), &Held::mesh,
                    &gpu::Device::create_mesh_pipeline);
}

std::unique_ptr<gpu::ComputePipeline> make_compute(gpu::Device& device, const gpu::ComputePipelineDesc& desc,
                                                   const void* owner, const std::string& cs) {
    State& s = st();
    if (!s.ready.load(std::memory_order_acquire)) return device.create_compute_pipeline(desc);
    return make_any(device, desc, owner, spec_of(desc, cs, s.cfg.content_key), &Held::compute,
                    &gpu::Device::create_compute_pipeline);
}

void forget(const void* owner) {
    State& s = st();
    if (!s.started) return;
    std::lock_guard lock(s.m);
    std::erase_if(s.watch, [owner](const Watch& w) { return w.owner == owner; });
}

Counters counters() {
    State& s = st();
    std::lock_guard lock(s.m);
    poll_locked(s);
    Counters c;
    const auto& t = s.stats;
    c.first_uses = t.first_uses;
    c.adopted_ready = t.adopted_ready;
    c.adopted_compiling = t.adopted_compiling;
    c.created = t.created;
    c.prebuilt = t.prebuilt;
    c.prebuilt_from_archive = t.prebuilt_from_archive;
    c.prebuild_failed = t.prebuild_failed;
    c.bootstrap_matched = t.bootstrap_matched;
    c.waits = t.waits;
    c.queued = s.queue.size() + s.in_flight.size();
    c.wait_longest = t.wait_longest;
    c.wait_sum = t.wait_sum;
    return c;
}

gpu::Status open_archive(gpu::Device& device, const std::string& directory, const std::string& version, bool load,
                         std::string* log) {
    gpu::PipelineCacheDesc pc;
    pc.directory = directory.c_str();
    pc.translator_version = version;
    pc.load = load;
    if (const char* v = std::getenv("GTA6RT_METAL_PCACHE_SHARD_MB")) pc.shard_bytes = std::uint64_t(std::atof(v) * 1e6);
    std::string error;
    const gpu::Status status = device.open_pipeline_cache(pc, &error);
    const auto stats = device.pipeline_cache_stats();
    if (log) {
        char buf[200];
        std::snprintf(buf, sizeof buf, " (%u shards, %llu bytes, %.3f s%s)", stats.shards,
                      static_cast<unsigned long long>(stats.file_bytes), stats.load_seconds,
                      stats.shards_unreadable ? (", " + std::to_string(stats.shards_unreadable) + " unreadable moved aside").c_str() : "");
        *log = stats.path + "-*: " + gpu::status_name(status) + (stats.loaded ? ", loaded" : ", new") + buf;
        if (status != gpu::Status::ok) *log += ": " + error;
    }
    return status;
}

// Each flush writes what was added since the last one as a new shard file; a failed write loses that shard only
// (port/metal never overwrites a written shard and drops a shard that failed to serialise from its lookups).
// when_idle is kept for callers; GTA6RT_METAL_PCACHE_FLUSH_WHEN_IDLE=1 restores the old deferral.
gpu::Status flush_archive(gpu::Device& device, std::string* error, bool when_idle) {
    static std::mutex m;
    static double last = 0;
    static std::uint64_t deferred = 0, failures = 0;
    std::lock_guard lock(m);
    const double now = process_age_s();
    static const bool defer = [] {
        const char* v = std::getenv("GTA6RT_METAL_PCACHE_FLUSH_WHEN_IDLE");
        return v && v[0] == '1';
    }();
    if (when_idle && defer && now - last < 120) {
        const auto q = device.compile_queue_stats();
        if (q.running || q.queued[0] || q.queued[1] || q.queued[2]) {
            if (++deferred <= 3 || deferred % 20 == 0)
                log_line("archive flush deferred: %llu compiles running, %llu queued (%llu deferrals so far)",
                         (unsigned long long)q.running, (unsigned long long)(q.queued[0] + q.queued[1] + q.queued[2]),
                         (unsigned long long)deferred);
            return gpu::Status::not_ready;
        }
    }
    last = now;
    const auto before = device.pipeline_cache_stats();
    std::string why;
    const gpu::Status st = device.flush_pipeline_cache(&why);
    const auto after = device.pipeline_cache_stats();
    if (st != gpu::Status::ok && st != gpu::Status::not_ready) {
        if (++failures <= 5)
            log_line("archive shard not written (%s); %llu pipelines lost from this run's archive, the files on disk "
                     "are unchanged", why.substr(0, 300).c_str(), (unsigned long long)before.pending);
    } else if (after.shards > before.shards) {
        log_line("archive shard written at t=%.0fs: %llu pipelines, %llu bytes in %.2f s with no compile lock held; %u "
                 "shards, %llu bytes on disk",
                 now, (unsigned long long)(after.written - before.written),
                 (unsigned long long)(after.file_bytes - before.file_bytes), after.serialize_seconds, after.shards,
                 (unsigned long long)after.file_bytes);
    }
    if (error) *error = why;
    return st;
}

std::string report() {
    State& s = st();
    std::lock_guard lock(s.m);
    return report_locked(s);
}

}  // namespace gta6rt::pcache
