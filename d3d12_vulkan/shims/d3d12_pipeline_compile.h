// Timing and scheduling for Vulkan pipeline creation on the D3D12 device.
#pragma once

#include "../src/vulkan_replay.h"
#include "../src/vk_run_counters.h"
#include <gta6rt_hud/stats.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <string_view>
#include <vector>

namespace rt::d3d12 {
inline bool trace_pipeline_compile() {
    static const bool on = [] {
        wchar_t v[2]{};
        return GetEnvironmentVariableW(L"LEONIDA_TRACE_PIPELINE_COMPILE", v, 2) == 1 && v[0] == L'1';
    }();
    return on;
}

// port/: on unless LEONIDA_ASYNC_PIPELINES=0. A draw or dispatch whose pipeline is not built yet hands the build to the
// compiler thread and waits for it within the submission's budget (LEONIDA_PIPELINE_WAIT_MS, vk_run_counters.h); past
// the budget it is skipped until the pipeline exists, so a title thread is never held by the driver's compiler for
// longer than that (the title's hang watchdog fires after 30 s without a frame).
inline bool async_replay_compile() {
    static const bool on = [] {
        wchar_t v[2]{};
        return !(GetEnvironmentVariableW(L"LEONIDA_ASYNC_PIPELINES", v, 2) == 1 && v[0] == L'0');
    }();
    return on && !leonida::strict_replay();
}

// Absolute monotonic timestamps let trace readers distinguish time in the ICD from time queued for the worker.
using CompileClock = std::chrono::steady_clock;
inline long long compile_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(CompileClock::now().time_since_epoch()).count();
}

inline long long compile_wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

template<class Build> VkPipeline replay_pipeline(const char* trigger, Build&& get) {
    if (!trace_pipeline_compile()) return get();
    const auto start = compile_us();
    auto handle = get();
    auto elapsed = compile_us() - start;
    if (elapsed >= 1000)
        std::fprintf(stderr, "[rt] pipeline-use trigger=%s tid=%lu us=%lld ready=%d end_ms=%lld\n",
                     trigger, GetCurrentThreadId(), elapsed, int(handle != VK_NULL_HANDLE), compile_wall_ms());
    return handle;
}

// port/: per-stage feedback too. MoltenVK sets the application-cache hit bit only on the stages, never on the
// pipeline (spike/r4-pso-census, mvk_feedback/fb.c), so a hit is the pipeline's bit or every stage's.
struct CompileTrace {
    VkPipelineCreationFeedback feedback{};
    VkPipelineCreationFeedback stage_feedback[4]{};
    VkPipelineCreationFeedbackCreateInfo chain{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                               nullptr, &feedback, 0, stage_feedback};
    long long started = 0;
    const char* key = "-";

    template<class Info> void begin(Info& info, bool supported, const char* trigger, const char* shaders,
                                    std::uint32_t variant, std::size_t bytes, const std::string& description_key) {
        // port/: the creation feedback is always asked for, so the run counters see pipeline-cache hits untraced.
        if (supported) {
            std::uint32_t stages = 1;
            if constexpr (requires { info.stageCount; }) stages = info.stageCount;
            chain.pipelineStageCreationFeedbackCount = stages <= 4 ? stages : 0;
            chain.pNext = info.pNext;
            info.pNext = &chain;
        }
        started = compile_us();
        if (!trace_pipeline_compile()) return;
        if (!description_key.empty()) key = description_key.c_str();
        std::fprintf(stderr, "[rt] pipeline-compile-begin trigger=%s tid=%lu bytes=%zu variant=%u begin_ms=%lld key=%s shaders=%s\n",
                     trigger, GetCurrentThreadId(), bytes, variant, compile_wall_ms(), key, shaders);
        started = compile_us();
    }

    void end(const char* trigger, const char* shaders, std::uint32_t variant, std::size_t bytes, VkResult result) const {
        if (!started) return;
        const auto ended = compile_us();
        constexpr auto kHit = VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
        bool stages_hit = chain.pipelineStageCreationFeedbackCount > 0;
        for (std::uint32_t i = 0; i < chain.pipelineStageCreationFeedbackCount; ++i)
            stages_hit = stages_hit && (stage_feedback[i].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
                         (stage_feedback[i].flags & kHit);
        const int hit = !(feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? -1 :
                        int((feedback.flags & kHit) != 0 || stages_hit);
        if (hit == 1) leonida::run_counters::add(leonida::run_counters::counters().vk_cache_hits);
        leonida::run_counters::build_feedback(hit, std::uint64_t(ended - started));
        if (!trace_pipeline_compile()) return;
        std::fprintf(stderr, "[rt] pipeline-compile trigger=%s tid=%lu start_us=%lld us=%lld hit=%d bytes=%zu "
                             "variant=%u result=%d end_ms=%lld key=%s shaders=%s\n", trigger, GetCurrentThreadId(), started,
                             ended - started, hit, bytes, variant, int(result), compile_wall_ms(), key, shaders);
    }
};


// Demanded work has priority zero, speculative work is ordered by module size, and
// an idle checkpoint runs last. No Vulkan call runs with the queue lock held.
//
// port/: with two or more workers, one is kept for demanded work: a speculative job (priority above zero) is taken
// only while fewer than thread_count() - 1 speculative jobs run, so a build the title waits for never queues behind
// warm-up (spike/r4-pso-census: 1.2-2.1 s first builds of the UI's pipelines behind three warm-ups, 0.07 s without).
// Jobs at kWarmupPriority and above (the compute warm-up) also wait until open_warmup(): the first present after the
// title's first emitted draw, unless LEONIDA_WARMUP_START=now. Boot-manifest builds (1 .. kWarmupPriority - 1) do not.
//
// Lifetime. A job names its owner (the PSO it builds, or the device for a cache checkpoint) and the device whose
// Vulkan objects it uses, and holds no reference to either: a reference kept the device alive past the title's last
// Release until the worker reached the job, so a released device's builds ran on (a test that then destroyed its
// VkDevice crashed).
//   - A PSO's last Release does not wait for its running build, which can be a dispatch-worker build of a module the
//     driver takes minutes over: after_running() drops its queued jobs and hands the PSO's deletion to the worker,
//     to run when that build returns (PipelineState::last_release). The PSO lets its device reference go at once.
//   - A device's teardown waits: cancel() drops its queued jobs (by owner or device) and waits for its running one
//     and what runs after it, so an in-flight vkCreate*Pipelines finishes before the device's objects go.
//   - The worker thread runs while a device is alive (retain/release): the last device's release stops and joins it.
//   - stop_for_exit() is for a process that ends with its device alive (main.cpp: the title's exit, a console Ctrl+C
//     or close). After it nothing more is queued: submit() refuses, and the callers build in place or not at all.
class PipelineCompiler {
public:
    static constexpr std::size_t kWarmupPriority = std::size_t(1) << 32;

    static PipelineCompiler& instance() {
        static auto* pool = new PipelineCompiler;  // never destroyed: static destruction must not race the worker
        return *pool;
    }

    // Speculative warm-up may start (see above). Idempotent; logs once with `why`.
    void open_warmup(const char* why) {
        {
            std::lock_guard lock(mutex_);
            if (warmup_open_) return;
            warmup_open_ = true;
            std::fprintf(stderr, "[rt] pipeline compiler: speculative warm-up starts (%s; %zu jobs queued)\n", why,
                         jobs_.size());
        }
        wake_.notify_all();
    }
    bool warmup_open() {
        std::lock_guard lock(mutex_);
        return warmup_open_;
    }
    // port/: speculative warm-up held while a movie plays (the title decodes at most one movie frame a tick, so a
    // tick lost to the compiler is a dropped movie frame). Demanded builds are not affected. LEONIDA_WARMUP_MOVIE_PAUSE=0
    // keeps warming up through movies.
    void pause_warmup(bool paused, const char* why) {
        static const bool enabled = [] {
            const char* v = std::getenv("LEONIDA_WARMUP_MOVIE_PAUSE");
            return !(v && v[0] == '0');
        }();
        if (!enabled) return;
        {
            std::lock_guard lock(mutex_);
            if (warmup_paused_ == paused) return;
            warmup_paused_ = paused;
            ++stats_.warmup_pauses;
            if (stats_.warmup_pauses <= 16)
                std::fprintf(stderr, "[rt] pipeline compiler: speculative warm-up %s (%s; %zu jobs queued, %zu running)\n",
                             paused ? "paused" : "resumed", why, jobs_.size(), warmup_running_);
        }
        if (!paused) wake_.notify_all();
    }

    struct Stats {
        std::uint64_t warmup_pauses = 0;
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;  // jobs the worker ran
        std::uint64_t dropped = 0;    // queued jobs thrown away by cancel, after_running, release or stop_for_exit
        std::uint64_t refused = 0;    // submissions after stop_for_exit
        std::uint64_t waited = 0;     // cancel calls that waited for a running job
        std::uint64_t deferred = 0;   // after_running calls that left something to run after a running job
    };

    // False, with nothing queued, after stop_for_exit.
    bool submit(const void* owner, const void* device, std::size_t priority, std::function<void()> work) {
        std::lock_guard lock(mutex_);
        if (closed_) {
            ++stats_.refused;
            return false;
        }
        if (workers_.size() < thread_count() && running_.size() + jobs_.size() >= workers_.size()) {
            if (workers_.empty()) ++generation_;
            const std::uint64_t generation = generation_;
            workers_.emplace_back([this, generation] { run(generation); });
        }
        jobs_.emplace(priority, Job{owner, device, std::move(work)});
        ++stats_.submitted;
        gta6rt_hud_set_pipelines_pending(jobs_.size() + running_.size());
        wake_.notify_one();
        return true;
    }

    bool closed() {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    bool has_compile_waiter() {
        std::lock_guard lock(mutex_);
        return !jobs_.empty() && jobs_.begin()->first != SIZE_MAX;
    }
    // Returns when no job is queued or running. A job owns its pipeline state, and through it the device, until it
    // finishes; a caller that counts references (rt::d3d12_xs::device_proxy_ref_count) waits here first.
    void wait_idle() {
        std::unique_lock wait(mutex_);
        done_.wait(wait, [this] { return jobs_.empty() && running_.empty(); });
    }

    // Drops the queued jobs whose owner or device is `key` and waits for the running ones, with what runs after them
    // (after_running), unless one is on this thread. Jobs a running one queues for `key` meanwhile are dropped too.
    // Returns how many were dropped.
    std::size_t cancel(const void* key) {
        std::vector<Job> dropped;
        {
            std::unique_lock lock(mutex_);
            const auto me = std::this_thread::get_id();
            bool counted = false;
            for (;;) {
                drop_locked(dropped, [key](const Job& job) { return job.owner == key || job.device == key; });
                const bool busy = std::any_of(running_.begin(), running_.end(), [&](const Running& r) {
                    return (r.owner == key || r.device == key) && r.thread != me;
                });
                if (!busy) break;
                if (!counted) ++stats_.waited;
                counted = true;
                done_.wait(lock);
            }
        }
        return dropped.size();  // the dropped jobs' captures are destroyed here, outside the lock
    }

    // Drops the queued jobs filed under `owner`. When one of its jobs is running, `last` is run on the worker after it
    // returns, before the job counts as done (a device's cancel waits for both), and the answer is true; otherwise
    // false, and `last` is not kept. On the worker inside that job itself, `last` runs after the job returns too.
    bool after_running(const void* owner, std::function<void()> last) {
        std::vector<Job> dropped;
        std::lock_guard lock(mutex_);
        drop_locked(dropped, [owner](const Job& job) { return job.owner == owner; });
        for (Running& r : running_) {
            if (r.owner != owner || r.finished) continue;
            r.after = r.after ? [first = std::move(r.after), then = std::move(last)] { first(); then(); }
                              : std::move(last);
            ++stats_.deferred;
            return true;
        }
        return false;
    }

    // True on the worker while it runs a job whose owner or device is `key` (not what runs after it).
    bool running_here(const void* key) {
        std::lock_guard lock(mutex_);
        const auto me = std::this_thread::get_id();
        return std::any_of(running_.begin(), running_.end(), [&](const Running& r) {
            return r.thread == me && !r.finished && (r.owner == key || r.device == key);
        });
    }

    // A device holds the worker while it lives. The last release drops whatever is still queued, stops the worker
    // and joins it; a release on the worker itself lets it return after the job it is in.
    void retain() {
        std::lock_guard lock(mutex_);
        ++users_;
    }
    void release() {
        std::vector<Job> dropped;
        std::vector<std::thread> workers;
        {
            std::lock_guard lock(mutex_);
            if (users_ && --users_) return;
            workers = stop_locked(dropped);
        }
        finish(std::move(workers), true);
    }

    // Process exit with a device still alive: drops the queue, refuses new jobs and waits up to `wait` for a running
    // one, so ExitProcess does not end the worker inside the driver. False when a job was still running.
    bool stop_for_exit(std::chrono::milliseconds wait) {
        std::vector<Job> dropped;
        std::vector<std::thread> workers;
        bool idle = true;
        {
            std::unique_lock lock(mutex_);
            closed_ = true;
            workers = stop_locked(dropped);
            const auto me = std::this_thread::get_id();
            idle = done_.wait_for(lock, wait, [&] {
                return std::none_of(running_.begin(), running_.end(), [&](const Running& r) { return r.thread != me; });
            });
        }
        finish(std::move(workers), idle);
        return idle;
    }

    Stats stats() {
        std::lock_guard lock(mutex_);
        return stats_;
    }
    bool worker_running() {
        std::lock_guard lock(mutex_);
        return !workers_.empty();
    }

    // port/: LEONIDA_PIPELINE_COMPILE_THREADS workers (default 4; 1 is leonida's single thread). The driver's compiler
    // runs builds of different pipelines side by side (MoltenVK: one MTLCompilerService request each).
    static std::size_t thread_count() {
        static const std::size_t n = [] {
            char text[16]{};
            const DWORD got = GetEnvironmentVariableA("LEONIDA_PIPELINE_COMPILE_THREADS", text, sizeof(text));
            const unsigned long v = got > 0 && got < sizeof(text) ? std::strtoul(text, nullptr, 10) : 0;
            return std::size_t(v >= 1 && v <= 16 ? v : 4);
        }();
        return n;
    }
    // Tests: called on the worker with each job's owner, once the job counts as running and before its work.
    void set_job_probe(std::function<void(const void* owner)> probe) {
        std::lock_guard lock(mutex_);
        probe_ = std::move(probe);
    }

private:
    struct Job {
        const void* owner = nullptr;
        const void* device = nullptr;
        std::function<void()> work;
    };
    struct Running {
        const void* owner;
        const void* device;
        std::thread::id thread;
        std::function<void()> after;  // after_running
        bool finished = false;        // the job returned; `after` may be running
        bool speculative = false;     // taken at a priority above zero
        bool warmup = false;          // a compute warm-up (kWarmupPriority and above)
    };

    template<class Match> void drop_locked(std::vector<Job>& dropped, const Match& match) {
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (match(it->second)) {
                dropped.push_back(std::move(it->second));
                it = jobs_.erase(it);
                ++stats_.dropped;
            } else {
                ++it;
            }
        }
    }

    std::vector<std::thread> stop_locked(std::vector<Job>& dropped) {
        drop_locked(dropped, [](const Job&) { return true; });
        ++generation_;  // the workers return when they next look: after the job each is running, if any
        wake_.notify_all();
        return std::move(workers_);
    }

    static void finish(std::vector<std::thread> workers, bool join) {
        for (std::thread& worker : workers) {
            if (!worker.joinable()) continue;
            if (join && worker.get_id() != std::this_thread::get_id()) worker.join();
            else worker.detach();
        }
    }

    Running& mine(std::thread::id me) {
        return *std::find_if(running_.begin(), running_.end(), [me](const Running& r) { return r.thread == me; });
    }

    // The first job a worker may take now, or jobs_.end().
    std::multimap<std::size_t, Job>::iterator takeable_locked() {
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
            if (it->first == 0) return it;
            const std::size_t n = thread_count();
            if (n >= 2 && speculative_running_ >= n - 1) return jobs_.end();
            const bool warmup = it->first >= kWarmupPriority && it->first != SIZE_MAX;
            if (warmup && (!warmup_open_ || warmup_paused_ || warmup_running_ >= warmup_threads())) continue;
            return it;
        }
        return jobs_.end();
    }
    // LEONIDA_WARMUP_THREADS (1 by default): compute warm-up builds at once. With three, the storm that starts with the
    // landing prompt held its replay at about 10 ms a submission (33 frames/s, r4vk-step4-b) against about 5.7 ms.
    static std::size_t warmup_threads() {
        static const std::size_t n = [] {
            const char* v = std::getenv("LEONIDA_WARMUP_THREADS");
            return v && *v ? std::size_t(std::strtoul(v, nullptr, 10)) : std::size_t(1);
        }();
        return n;
    }

    void run(std::uint64_t generation) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const auto me = std::this_thread::get_id();
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return generation_ != generation || takeable_locked() != jobs_.end(); });
            if (generation_ != generation) return;
            auto first = takeable_locked();
            const bool speculative = first->first != 0;
            const bool warmup = first->first >= kWarmupPriority && first->first != SIZE_MAX;
            Job job = std::move(first->second);
            jobs_.erase(first);
            running_.push_back({job.owner, job.device, me});
            running_.back().speculative = speculative;
            running_.back().warmup = warmup;
            speculative_running_ += speculative;
            warmup_running_ += warmup;
            const auto probe = probe_;
            lock.unlock();
            try {
                if (probe) probe(job.owner);
                job.work();
            } catch (...) {
                std::fprintf(stderr, "[rt] pipeline compile worker: a job threw\n");
            }
            job.work = nullptr;  // its captures go before the job counts as done
            lock.lock();
            ++stats_.completed;
            Running& self = mine(me);
            self.finished = true;
            if (std::function<void()> after = std::move(self.after)) {
                lock.unlock();
                try {
                    after();
                } catch (...) {
                    std::fprintf(stderr, "[rt] pipeline compile worker: work after a job threw\n");
                }
                after = nullptr;
                lock.lock();
            }
            const auto self_it = std::find_if(running_.begin(), running_.end(), [me](const Running& r) {
                return r.thread == me;
            });
            if (self_it->speculative) {
                --speculative_running_;
                warmup_running_ -= self_it->warmup;
                wake_.notify_all();
            }
            running_.erase(self_it);
            gta6rt_hud_set_pipelines_pending(jobs_.size() + running_.size());
            done_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::multimap<std::size_t, Job> jobs_;
    std::vector<Running> running_;  // one per worker; a stopped worker may still be finishing one
    std::vector<std::thread> workers_;
    std::uint64_t generation_ = 0;  // a worker runs while this is the value it started with
    unsigned users_ = 0;            // live devices (retain/release)
    bool closed_ = false;           // stop_for_exit: nothing more is queued
    std::size_t speculative_running_ = 0;
    std::size_t warmup_running_ = 0;
    bool warmup_paused_ = false;
    bool warmup_open_ = [] {
        const char* v = std::getenv("LEONIDA_WARMUP_START");
        return v && std::string_view(v) == "now";
    }();
    Stats stats_;
    std::function<void(const void*)> probe_;
};
// A frame was presented: the first lit one opens the speculative warm-up. "Lit" is a frame with at least
// LEONIDA_WARMUP_DRAWS emitted draws (4 by default): the black boot frames have one (the title's blit), the landing
// prompt's first frame about 19 (blit and Scaleform batches, spike/r4-pso-census). After 90 s it opens anyway.
inline void note_present_for_warmup() {
    static std::atomic<bool> opened{false};
    static std::atomic<std::uint64_t> last_draws{0};
    static const auto started = std::chrono::steady_clock::now();
    static const std::uint64_t needed = [] {
        const char* v = std::getenv("LEONIDA_WARMUP_DRAWS");
        return v && *v ? std::strtoull(v, nullptr, 10) : 4ull;
    }();
    if (opened.load(std::memory_order_relaxed)) return;
    const std::uint64_t draws = leonida::run_counters::counters().draws_emitted.load(std::memory_order_relaxed);
    const std::uint64_t frame = draws - last_draws.exchange(draws, std::memory_order_relaxed);
    const bool late = std::chrono::steady_clock::now() - started > std::chrono::seconds(90);
    if (frame < needed && !late) return;
    if (opened.exchange(true)) return;
    PipelineCompiler::instance().open_warmup(late ? "90 s after the first present" : "the first frame with the title's UI draws");
}
}  // namespace rt::d3d12
