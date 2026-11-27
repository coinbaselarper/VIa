#pragma once
// Helper threads for the copies the submitting thread makes of the title's memory before a submission (slot-133
// shadows and title windows): the runs are cut into pieces of kPiece bytes and copied by the caller and
// LEONIDA_COPY_THREADS helpers (3 by default, 0: the caller alone). One copy of 56 MiB took the submitting thread
// 2.4-2.8 ms a submission on the first-run pages (round 3); memory bandwidth is not the limit on an M4 Pro.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace leonida {

class CopyPool {
public:
    struct Task {
        std::uint8_t* to;
        const std::uint8_t* from;
        std::size_t bytes;
    };
    static constexpr std::size_t kPiece = 1u << 20;
    using CopyFn = void (*)(std::uint8_t* to, const std::uint8_t* from, std::size_t bytes);

    static CopyPool& get() {
        static CopyPool* pool = new CopyPool(helpers_from_env());
        return *pool;
    }
    unsigned helpers() const { return unsigned(threads_.size()); }

    // Copies every task with `copy` (safe to call from any thread) and returns when all are done. One caller at a time.
    void run(const std::vector<Task>& tasks, CopyFn copy) {
        std::lock_guard caller(caller_mutex_);
        pieces_.clear();
        std::size_t total = 0;
        for (const Task& t : tasks) {
            total += t.bytes;
            for (std::size_t at = 0; at < t.bytes; at += kPiece)
                pieces_.push_back({t.to + at, t.from + at, t.bytes - at < kPiece ? t.bytes - at : kPiece});
        }
        if (threads_.empty() || total < 2 * kPiece) {
            for (const Task& p : pieces_) copy(p.to, p.from, p.bytes);
            return;
        }
        copy_ = copy;
        run_locked(pieces_.size(), [this](std::size_t i) { copy_(pieces_[i].to, pieces_[i].from, pieces_[i].bytes); });
    }

    // Runs job(0) .. job(n - 1) on the caller and the helpers and returns when all are done. Callers take turns; `job`
    // must be safe to run on several threads at once.
    void run_jobs(std::size_t n, std::function<void(std::size_t)> job) {
        std::lock_guard caller(caller_mutex_);
        run_locked(n, std::move(job));
    }

private:
    void run_locked(std::size_t n, std::function<void(std::size_t)> job) {
        if (threads_.empty() || n < 2) {
            for (std::size_t i = 0; i < n; ++i) job(i);
            return;
        }
        {
            std::unique_lock lock(mutex_);
            idle_.wait(lock, [&] { return active_ == 0; });
            job_ = std::move(job);
            jobs_ = n;
            next_.store(0, std::memory_order_relaxed);
            pending_.store(n, std::memory_order_release);
            ++generation_;
        }
        wake_.notify_all();
        work();
        while (pending_.load(std::memory_order_acquire)) std::this_thread::yield();
    }

    static unsigned helpers_from_env() {
        const char* v = std::getenv("LEONIDA_COPY_THREADS");
        if (!v || !*v) return 3;
        const long n = std::strtol(v, nullptr, 10);
        return n < 0 ? 0u : n > 8 ? 8u : unsigned(n);
    }
    explicit CopyPool(unsigned helpers) {
        for (unsigned i = 0; i < helpers; ++i) threads_.emplace_back([this] { helper(); });
        for (auto& t : threads_) t.detach();
    }
    void work() {
        for (;;) {
            const std::size_t i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= jobs_) return;
            job_(i);
            pending_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    void helper() {
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return generation_ != seen; });
                seen = generation_;
                ++active_;
            }
            work();
            {
                std::lock_guard lock(mutex_);
                --active_;
            }
            idle_.notify_all();
        }
    }

    std::mutex caller_mutex_;  // one run at a time (the queues' submitting threads)
    std::vector<std::thread> threads_;
    std::vector<Task> pieces_;  // run()'s, read by its jobs
    CopyFn copy_ = nullptr;
    std::function<void(std::size_t)> job_;  // replaced only under mutex_ while no helper is in work()
    std::size_t jobs_ = 0;
    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::uint64_t generation_ = 0;
    unsigned active_ = 0;  // helpers in work(), under mutex_
    std::atomic<std::size_t> next_{0};
    std::atomic<std::size_t> pending_{0};
};

}  // namespace leonida
