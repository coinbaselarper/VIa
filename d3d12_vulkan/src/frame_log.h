#pragma once
// port/: one line per presented frame (LEONIDA_FRAME_LOG=PATH), for frame-pacing work: when the frame was shown
// (process age), the interval since the previous one, what the submitting thread spent since then by submit phase,
// how long the present call itself took (fence wait, acquire, the rest), the pipeline waits, the title's frame-event
// waits and whether a new movie frame (a Bink set switch) went out with it. Off by default; the cost when on is a
// formatted line per frame on a buffered stream.

#include "submit_phases.h"
#include "vk_run_counters.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace leonida::frame_log {

struct State {
    std::FILE* file = nullptr;
    bool tried = false;
    std::mutex mutex;
    double last_t = 0;
    std::uint64_t last_submissions = 0, last_wait_us = 0, last_build_us = 0, last_switches = 0, last_tokens = 0,
                  last_token_wait_us = 0, frames = 0;
};
inline State& state() {
    static State s;
    return s;
}

// Movie frames handed to the GPU (Bink set switches the title-texture refresh saw), and the title's frame-event waits
// (WaitFrameEventX: tokens handed out, microseconds spent blocked).
inline std::atomic<std::uint64_t>& movie_switches() {
    static std::atomic<std::uint64_t> n{0};
    return n;
}
inline std::atomic<std::uint64_t>& frame_tokens() {
    static std::atomic<std::uint64_t> n{0};
    return n;
}
inline std::atomic<std::uint64_t>& frame_token_wait_us() {
    static std::atomic<std::uint64_t> n{0};
    return n;
}
inline std::atomic<bool>& movie_playing() {
    static std::atomic<bool> on{false};
    return on;
}

inline bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("LEONIDA_FRAME_LOG");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return on;
}

// At a present the engine took. present_us: the whole present call; fence_us: waiting for the frame slot's previous
// present; acquire_us: vkAcquireNextImageKHR.
inline void frame(std::uint64_t present_us, std::uint64_t fence_us, std::uint64_t acquire_us) {
    submit_phases::Totals& t = submit_phases::totals();
    std::uint64_t ns[submit_phases::kCount];
    for (unsigned p = 0; p < submit_phases::kCount; ++p) ns[p] = t.frame_ns[p].exchange(0, std::memory_order_relaxed);
    if (!enabled()) return;
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.tried) {
        s.tried = true;
        s.file = std::fopen(std::getenv("LEONIDA_FRAME_LOG"), "w");
        if (s.file) {
            std::setvbuf(s.file, nullptr, _IOFBF, 1 << 16);
            std::fprintf(s.file, "# t_s dt_ms subs wait heaps shadows geometry textures replay present fence acquire "
                                 "pipe_wait_ms build_ms movie_new movie_on tokens token_wait_ms\n");
        }
    }
    if (!s.file) return;
    const double now = submit_phases::process_age();
    run_counters::Counters& c = run_counters::counters();
    const std::uint64_t subs = t.all_submissions.load(std::memory_order_relaxed);
    const std::uint64_t wait_us = c.deferred_wait_us.load(std::memory_order_relaxed);
    const std::uint64_t build_us = c.vk_build_us.load(std::memory_order_relaxed);
    const std::uint64_t switches = movie_switches().load(std::memory_order_relaxed);
    const std::uint64_t tokens = frame_tokens().load(std::memory_order_relaxed);
    const std::uint64_t token_wait = frame_token_wait_us().load(std::memory_order_relaxed);
    std::fprintf(s.file, "%.4f %.2f %llu %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.1f %.1f %llu %d %llu %.2f\n", now,
                 s.frames ? (now - s.last_t) * 1e3 : 0.0, static_cast<unsigned long long>(subs - s.last_submissions),
                 ns[0] * 1e-6, ns[1] * 1e-6, ns[2] * 1e-6, ns[3] * 1e-6, ns[4] * 1e-6, ns[5] * 1e-6,
                 present_us * 1e-3, fence_us * 1e-3, acquire_us * 1e-3, (wait_us - s.last_wait_us) * 1e-3,
                 (build_us - s.last_build_us) * 1e-3, static_cast<unsigned long long>(switches - s.last_switches),
                 movie_playing().load(std::memory_order_relaxed) ? 1 : 0,
                 static_cast<unsigned long long>(tokens - s.last_tokens), (token_wait - s.last_token_wait_us) * 1e-3);
    s.last_t = now;
    s.last_submissions = subs;
    s.last_wait_us = wait_us;
    s.last_build_us = build_us;
    s.last_switches = switches;
    s.last_tokens = tokens;
    s.last_token_wait_us = token_wait;
    if (++s.frames % 30 == 0) std::fflush(s.file);
}

}  // namespace leonida::frame_log
