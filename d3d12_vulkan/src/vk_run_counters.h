#pragma once
// port/d3d12_vulkan: per-run counts of PSOs, Vulkan pipeline builds, draws and dispatches, printed with each
// "submit phases" line as "[rt] vk-counters ...".

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace leonida::run_counters {

struct Counters {
    std::atomic<std::uint64_t> pso_graphics_ok{0}, pso_graphics_failed{0}, pso_compute_ok{0}, pso_compute_failed{0};
    std::atomic<std::uint64_t> vk_graphics_built{0}, vk_graphics_failed{0}, vk_compute_built{0}, vk_compute_failed{0};
    std::atomic<std::uint64_t> vk_build_us{0}, vk_build_max_us{0}, vk_cache_hits{0};
    // Builds by pipeline-creation feedback: a hit in the application pipeline cache, a miss, or no valid feedback.
    std::atomic<std::uint64_t> vk_hit_builds{0}, vk_hit_us{0}, vk_miss_builds{0}, vk_miss_us{0}, vk_nofb_builds{0},
        vk_nofb_us{0};
    std::atomic<std::uint64_t> worker_builds{0}, worker_build_us{0};
    std::atomic<std::uint64_t> submit_wall_us{0}, submit_max_us{0}, submits_over_1s{0};
    std::atomic<std::uint64_t> deferred_uses{0}, deferred_waits{0}, deferred_wait_us{0};
    std::atomic<std::uint64_t> draws_pipeline_pending{0}, worker_builds_null{0};
    std::atomic<std::uint64_t> cache_loaded_bytes{0}, cache_written_bytes{0}, cache_writes{0}, cache_write_us{0};
    std::atomic<std::uint64_t> draws_emitted{0}, draws_skipped{0}, draws_in_failed_submissions{0}, dispatches_emitted{0};
    std::atomic<std::uint64_t> dispatches_in_failed_submissions{0}, submissions_ok{0}, submissions_failed{0};
    std::atomic<std::uint64_t> float_control_refusals{0};
    std::atomic<std::uint64_t> pso_graphics_ok_us{0}, pso_graphics_failed_us{0}, pso_compute_us{0}, pso_max_us{0};
    std::atomic<std::uint64_t> pso_first_ms{0}, pso_last_ms{0}, pso_threads{0}, pso_inflight{0}, pso_inflight_max{0};
    std::atomic<std::uint64_t> pso_phase_us[8]{};
    std::atomic<std::uint64_t> spirv_loads{0}, spirv_load_bytes{0}, spirv_cache_hits{0}, graphics_check_hits{0};
};

enum class PsoPhase : unsigned { desc, resolve, load, check, link, patch, rest, count };
inline constexpr const char* pso_phase_names[] = {"desc", "resolve", "load", "check", "link", "patch", "rest"};

// Seconds since the process started (sysctl KERN_PROC start time; the host's re-exec keeps it). 0 elsewhere.
inline double process_age_s() {
#if defined(__APPLE__)
    static const double start = [] {
        kinfo_proc info{};
        size_t size = sizeof info;
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
        if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) return 0.0;
        return double(info.kp_proc.p_starttime.tv_sec) + double(info.kp_proc.p_starttime.tv_usec) * 1e-6;
    }();
    timeval now{};
    gettimeofday(&now, nullptr);
    return start ? double(now.tv_sec) + double(now.tv_usec) * 1e-6 - start : 0.0;
#else
    return 0.0;
#endif
}

inline Counters& counters() {
    static Counters c;
    return c;
}

inline void add(std::atomic<std::uint64_t>& counter, std::uint64_t n = 1) {
    counter.fetch_add(n, std::memory_order_relaxed);
}

inline void raise_max(std::atomic<std::uint64_t>& counter, std::uint64_t value) {
    std::uint64_t seen = counter.load(std::memory_order_relaxed);
    while (value > seen && !counter.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
}

// Triggers that run on the pipeline compiler's own thread (d3d12_pipeline_compile.h); every other build runs while a
// title thread waits for it (a draw, a dispatch, a PSO created eagerly).
inline bool worker_trigger(const char* trigger) {
    if (!trigger) return false;
    const std::string t(trigger);
    return t == "warmup" || t.ends_with("-worker");
}

// hit: 1 cache hit, 0 miss, -1 no valid creation feedback.
inline void build_feedback(int hit, std::uint64_t us) {
    Counters& c = counters();
    if (hit == 1) {
        add(c.vk_hit_builds);
        add(c.vk_hit_us, us);
    } else if (hit == 0) {
        add(c.vk_miss_builds);
        add(c.vk_miss_us, us);
    } else {
        add(c.vk_nofb_builds);
        add(c.vk_nofb_us, us);
    }
}

inline void pipeline_built(bool compute, bool ok, std::uint64_t us, const char* trigger) {
    Counters& c = counters();
    add(compute ? (ok ? c.vk_compute_built : c.vk_compute_failed) : (ok ? c.vk_graphics_built : c.vk_graphics_failed));
    add(c.vk_build_us, us);
    if (worker_trigger(trigger)) {
        add(c.worker_builds);
        add(c.worker_build_us, us);
    }
    raise_max(c.vk_build_max_us, us);
}

// One ExecuteCommandLists call: the title's submitting thread was held this long (replay, and the pipeline builds or
// waits it needed).
inline void submission_wall(std::uint64_t us) {
    Counters& c = counters();
    add(c.submit_wall_us, us);
    raise_max(c.submit_max_us, us);
    if (us >= 1000000) add(c.submits_over_1s);
}

// How long a draw or dispatch whose pipeline the compiler thread builds may wait for it (LEONIDA_PIPELINE_WAIT_MS,
// default 2000) before it is skipped, per ExecuteCommandLists call: the budget left on this thread.
inline std::uint64_t& wait_budget_us() {
    thread_local std::uint64_t budget = 0;
    return budget;
}
inline std::uint64_t pipeline_wait_ms() {
    static const std::uint64_t ms = [] {
        const char* v = std::getenv("LEONIDA_PIPELINE_WAIT_MS");
        if (!v || !*v) return std::uint64_t{2000};
        char* end = nullptr;
        const unsigned long long n = std::strtoull(v, &end, 10);
        if (end == v || *end) {
            std::fprintf(stderr, "[rt] d3d12: LEONIDA_PIPELINE_WAIT_MS=%s is not a number of milliseconds; using 2000\n", v);
            return std::uint64_t{2000};
        }
        return std::uint64_t(n);
    }();
    return ms;
}
inline void begin_submission() { wait_budget_us() = pipeline_wait_ms() * 1000; }

// Failed PSO creation calls by reason, and every creation call by the entry point the title called. A call is counted
// once, by the outermost PsoCall on its thread: slot 57 builds through the device's own CreateGraphicsPipelineState or
// CreatePipelineState, and a refused description is retried through a second path; neither inner call is counted.
struct Refusals {
    std::mutex mutex;
    std::map<std::string, std::uint64_t> by_reason;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> by_entry;  // ok, failed
    std::map<std::string, std::uint64_t> by_outcome;  // outcome(): what one stage of an entry did, for a census
};
inline Refusals& refusals() {
    static Refusals r;
    return r;
}

enum class PsoKind { unknown, graphics, compute };

// Splits one graphics PSO creation's wall time into phases: to() charges the time since the last switch to the phase
// in force, and the destructor charges the remainder.
class PsoClock {
public:
    PsoClock() : last_(std::chrono::steady_clock::now()) {}
    PsoClock(const PsoClock&) = delete;
    PsoClock& operator=(const PsoClock&) = delete;
    ~PsoClock() { to(PsoPhase::rest); }
    void to(PsoPhase next) {
        const auto now = std::chrono::steady_clock::now();
        add(counters().pso_phase_us[unsigned(phase_)],
            std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - last_).count()));
        last_ = now;
        phase_ = next;
    }

private:
    std::chrono::steady_clock::time_point last_;
    PsoPhase phase_ = PsoPhase::desc;
};

struct PsoCallState {
    int depth = 0;
    PsoKind kind = PsoKind::unknown;
    std::string reason;
};
inline PsoCallState& pso_call_state() {
    thread_local PsoCallState state;
    return state;
}

// Why the PSO call in flight on this thread fails; the first reason given is kept (the innermost check that refused).
// Outside a PsoCall it counts at once as one failed graphics PSO's reason.
inline void refused(const std::string& reason) {
    PsoCallState& s = pso_call_state();
    if (s.depth > 0) {
        if (s.reason.empty()) s.reason = reason.empty() ? "?" : reason;
        return;
    }
    Refusals& r = refusals();
    std::lock_guard lock(r.mutex);
    ++r.by_reason[reason.empty() ? "?" : reason];
}
inline void refused(const char* reason) { refused(std::string(reason ? reason : "?")); }

// The reason recorded so far for the call in flight, left in place.
inline std::string current_reason() { return pso_call_state().reason; }

// The reason recorded so far for the call in flight, cleared (a retry through another path starts without it).
inline std::string take_reason() {
    PsoCallState& s = pso_call_state();
    std::string r = std::move(s.reason);
    s.reason.clear();
    return r;
}

// A failure the code below the entry point gave no reason for: the HRESULT names it.
inline std::string unexplained(const char* entry, long hr) {
    char text[160];
    std::snprintf(text, sizeof text, "%s returned 0x%08lx with no reason recorded", entry,
                  static_cast<unsigned long>(hr) & 0xffffffffUL);
    return text;
}

inline void set_kind(PsoKind kind) {
    PsoCallState& s = pso_call_state();
    if (s.depth > 0 && s.kind == PsoKind::unknown) s.kind = kind;
}

class PsoCall {
public:
    PsoCall(const char* entry, PsoKind kind) : entry_(entry), outer_(pso_call_state().depth++ == 0) {
        if (outer_) {
            pso_call_state().kind = kind;
            pso_call_state().reason.clear();
            Counters& c = counters();
            thread_local bool seen = false;
            if (!seen) {
                seen = true;
                add(c.pso_threads);
            }
            std::uint64_t none = 0;
            c.pso_first_ms.compare_exchange_strong(none, std::uint64_t(process_age_s() * 1000.0));
            raise_max(c.pso_inflight_max, c.pso_inflight.fetch_add(1, std::memory_order_relaxed) + 1);
            started_ = std::chrono::steady_clock::now();
        }
    }
    PsoCall(const PsoCall&) = delete;
    PsoCall& operator=(const PsoCall&) = delete;
    ~PsoCall() {
        PsoCallState& s = pso_call_state();
        --s.depth;
        if (!outer_) return;
        Counters& c = counters();
        const bool compute = s.kind == PsoKind::compute;
        const auto us = std::uint64_t(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started_).count());
        add(compute ? c.pso_compute_us : ok_ ? c.pso_graphics_ok_us : c.pso_graphics_failed_us, us);
        raise_max(c.pso_max_us, us);
        raise_max(c.pso_last_ms, std::uint64_t(process_age_s() * 1000.0));
        c.pso_inflight.fetch_sub(1, std::memory_order_relaxed);
        add(compute ? (ok_ ? c.pso_compute_ok : c.pso_compute_failed) : (ok_ ? c.pso_graphics_ok : c.pso_graphics_failed));
        Refusals& r = refusals();
        std::lock_guard lock(r.mutex);
        auto& entry = r.by_entry[entry_];
        ++(ok_ ? entry.first : entry.second);
        if (!ok_) ++r.by_reason[s.reason.empty() ? unexplained(entry_, hr_) : s.reason];
        s.reason.clear();
        s.kind = PsoKind::unknown;
    }
    // hr < 0 is a failure (HRESULT); returns hr.
    long finish(long hr) {
        hr_ = hr;
        ok_ = hr >= 0;
        return hr;
    }

private:
    const char* entry_;
    bool outer_;
    bool ok_ = false;
    long hr_ = static_cast<long>(0x80004005L);
    std::chrono::steady_clock::time_point started_{};
};

// A path inside an entry point that built the PSO (listed with the entries, ok only).
inline void count_path(const char* path) {
    Refusals& r = refusals();
    std::lock_guard lock(r.mutex);
    ++r.by_entry[path].first;
}

// A census line, not a refusal: e.g. what slot 57's factory planned and what the device answered.
inline void outcome(const std::string& what) {
    Refusals& r = refusals();
    std::lock_guard lock(r.mutex);
    ++r.by_outcome[what];
}

inline std::uint64_t failed_with_reason() {
    Refusals& r = refusals();
    std::lock_guard lock(r.mutex);
    std::uint64_t n = 0;
    for (const auto& [reason, count] : r.by_reason) n += count;
    return n;
}

inline void report(double age_s) {
    Counters& c = counters();
    const auto v = [](const std::atomic<std::uint64_t>& a) {
        return static_cast<unsigned long long>(a.load(std::memory_order_relaxed));
    };
    std::fprintf(stderr,
                 "[rt] vk-counters t=%.0fs: psos graphics %llu ok %llu failed, compute %llu ok %llu failed; "
                 "vk pipelines graphics %llu built %llu failed, compute %llu built %llu failed, %.1f s building "
                 "(longest %.2f s, %llu cache hits; on the compiler thread %llu builds, %.1f s); float-controls refusals %llu; "
                 "submissions %llu ok %llu failed, %.1f s in ExecuteCommandLists (longest %.2f s, %llu over 1 s); "
                 "pipelines not ready at use %llu (%llu waits, %.1f s waited, %llu compiler-thread builds without a pipeline); "
                 "draws %llu emitted %llu skipped in replay (%llu of them for a pipeline not built yet or never buildable) %llu in failed "
                 "submissions; dispatches %llu in failed "
                 "submissions; session cache %llu bytes loaded, %llu writes (%.2f s, last %llu bytes)\n",
                 age_s, v(c.pso_graphics_ok), v(c.pso_graphics_failed), v(c.pso_compute_ok), v(c.pso_compute_failed),
                 v(c.vk_graphics_built), v(c.vk_graphics_failed), v(c.vk_compute_built), v(c.vk_compute_failed),
                 double(v(c.vk_build_us)) * 1e-6, double(v(c.vk_build_max_us)) * 1e-6, v(c.vk_cache_hits),
                 v(c.worker_builds), double(v(c.worker_build_us)) * 1e-6, v(c.float_control_refusals),
                 v(c.submissions_ok), v(c.submissions_failed), double(v(c.submit_wall_us)) * 1e-6,
                 double(v(c.submit_max_us)) * 1e-6, v(c.submits_over_1s), v(c.deferred_uses), v(c.deferred_waits),
                 double(v(c.deferred_wait_us)) * 1e-6, v(c.worker_builds_null), v(c.draws_emitted), v(c.draws_skipped),
                 v(c.draws_pipeline_pending), v(c.draws_in_failed_submissions), v(c.dispatches_in_failed_submissions), v(c.cache_loaded_bytes),
                 v(c.cache_writes), double(v(c.cache_write_us)) * 1e-6, v(c.cache_written_bytes));
    const auto mean_ms = [&](const std::atomic<std::uint64_t>& us, const std::atomic<std::uint64_t>& n) {
        return v(n) ? double(v(us)) / 1000.0 / double(v(n)) : 0.0;
    };
    std::fprintf(stderr,
                 "[rt] vk-counters: pipeline builds by cache feedback: %llu hits (mean %.0f ms), %llu misses (mean "
                 "%.0f ms), %llu without feedback (mean %.0f ms)\n",
                 v(c.vk_hit_builds), mean_ms(c.vk_hit_us, c.vk_hit_builds), v(c.vk_miss_builds),
                 mean_ms(c.vk_miss_us, c.vk_miss_builds), v(c.vk_nofb_builds), mean_ms(c.vk_nofb_us, c.vk_nofb_builds));
    std::fprintf(stderr,
                 "[rt] vk-counters: PSO call time: graphics ok %.2f s, graphics failed %.2f s, compute %.2f s (longest "
                 "%.1f ms); calls from %.1f s to %.1f s on %llu threads, at most %llu at once\n",
                 double(v(c.pso_graphics_ok_us)) * 1e-6, double(v(c.pso_graphics_failed_us)) * 1e-6,
                 double(v(c.pso_compute_us)) * 1e-6, double(v(c.pso_max_us)) * 1e-3, double(v(c.pso_first_ms)) * 1e-3,
                 double(v(c.pso_last_ms)) * 1e-3, v(c.pso_threads), v(c.pso_inflight_max));
    std::string phases;
    for (unsigned i = 0; i < unsigned(PsoPhase::count); ++i) {
        char part[64];
        std::snprintf(part, sizeof part, "%s%s %.2f s", i ? ", " : "", pso_phase_names[i],
                      double(v(c.pso_phase_us[i])) * 1e-6);
        phases += part;
    }
    std::fprintf(stderr, "[rt] vk-counters: graphics PSO phases: %s; SPIR-V %llu loads (%.0f MiB), %llu from the pack; "
                 "%llu PSOs passed the SPIR-V checks from the memo\n",
                 phases.c_str(), v(c.spirv_loads), double(v(c.spirv_load_bytes)) / 1048576.0, v(c.spirv_cache_hits),
                 v(c.graphics_check_hits));
    Refusals& r = refusals();
    std::lock_guard lock(r.mutex);
    for (const auto& [entry, n] : r.by_entry)
        std::fprintf(stderr, "[rt] vk-counters: PSO calls %s: %llu ok %llu failed\n", entry.c_str(),
                     static_cast<unsigned long long>(n.first), static_cast<unsigned long long>(n.second));
    for (const auto& [what, n] : r.by_outcome)
        std::fprintf(stderr, "[rt] vk-counters: census %s: %llu\n", what.c_str(), static_cast<unsigned long long>(n));
    std::uint64_t with_reason = 0;
    for (const auto& [reason, n] : r.by_reason) {
        with_reason += n;
        std::fprintf(stderr, "[rt] vk-counters: %llu PSOs refused: %s\n", static_cast<unsigned long long>(n),
                     reason.c_str());
    }
    const std::uint64_t failed = v(c.pso_graphics_failed) + v(c.pso_compute_failed);
    std::fprintf(stderr, "[rt] vk-counters: %llu failed PSO calls, %llu refusal reasons%s\n",
                 static_cast<unsigned long long>(failed), static_cast<unsigned long long>(with_reason),
                 failed == with_reason ? "" : " (MISMATCH: a failure without a reason or a reason without a call)");
}

}  // namespace leonida::run_counters
