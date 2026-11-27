// Test of src/submission_marks.h: an object a queue picked while replaying (a bindless table generation, a title
// texture) is not retired before that queue's submission completes, however long the replay and encode take.
// Build and run: d3d12_metal/tools/submission_marks_test.sh
#include "submission_marks.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

struct FakeFence {
    std::atomic<std::uint64_t> done{0};
    std::uint64_t completed_value() const { return done.load(); }
};

using Marks = leonida::SubmissionMarks<FakeFence*>;
using Clock = std::chrono::steady_clock;

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

struct Retired {
    Marks::Marks marks, settle;
    Clock::time_point at;
    bool settled = false;
};

void sequence() {
    Marks m;
    FakeFence a, b;
    const auto t0 = Clock::now();
    const auto later = t0 + std::chrono::milliseconds(150);
    const auto much_later = t0 + std::chrono::milliseconds(400);

    m.begin_in_flight(&a, 1);
    m.note_submitted(&b, 1);
    Retired g{m.submitted(), {}, t0};
    Retired old{m.submitted_only(), {}, t0};
    b.done = 1;
    check(!m.passed(g.marks, g.at, later, g.settled, g.settle), "a generation retired while a replay is in flight stays");
    const bool old_free = m.reached(old.marks) && m.reached(m.submitted_only());
    check(old_free, "without the in-flight mark the same generation would have been freed (the vfx-t10 crash)");
    m.note_submitted(&a, 1);
    m.end_in_flight(&a);
    check(!m.passed(g.marks, g.at, much_later, g.settled, g.settle), "still kept after the submission, before the GPU");
    a.done = 1;
    check(m.passed(g.marks, g.at, much_later, g.settled, g.settle), "freed once the in-flight submission completed");

    m.begin_in_flight(&a, 2);
    Retired h{m.submitted(), {}, t0};
    m.note_submitted(&a, 2);
    m.end_in_flight(&a);
    check(!m.passed(h.marks, h.at, much_later, h.settled, h.settle), "a failed submission's signalled value holds it");
    a.done = 2;
    check(m.passed(h.marks, h.at, much_later, h.settled, h.settle), "and releases it when reached");

    FakeFence c;
    m.begin_in_flight(&c, 1);
    Retired k{m.submitted(), {}, t0};
    m.forget(&c);
    check(m.passed(k.marks, k.at, much_later, k.settled, k.settle), "a destroyed queue counts as done");

    for (const auto& [f, v] : m.submitted_only())
        check(f != &c, "submitted_only lists no forgotten queue");
}

// Threads like the layer's: queues replay with the current generation, sometimes stalling (a pipeline compiling at
// first use), then touch it at close (residency_for_submit); a flusher retires and frees generations by the marks.
struct Generation {
    std::mutex mutex;
    std::atomic<bool> freed{false};
    Retired retired;
};

std::uint64_t stress(bool in_flight_marks, int seconds_ms) {
    Marks m;
    constexpr int kQueues = 3;
    FakeFence fences[kQueues];
    std::mutex gen_mutex;
    std::deque<std::shared_ptr<Generation>> live;
    std::vector<std::shared_ptr<Generation>> graveyard;
    std::shared_ptr<Generation> current = std::make_shared<Generation>();
    std::atomic<Generation*> published{current.get()};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> used_after_free{0}, submissions{0};

    std::vector<std::thread> threads;
    for (int q = 0; q < kQueues; ++q) {
        threads.emplace_back([&, q] {
            std::mt19937 rng(1234 + q);
            std::uint64_t submitted = 0;
            while (!stop) {
                if (in_flight_marks) m.begin_in_flight(&fences[q], submitted + 1);
                Generation* g = published.load();
                const int stall = rng() % 8 == 0 ? 120 + int(rng() % 200) : int(rng() % 3);
                std::this_thread::sleep_for(std::chrono::milliseconds(stall));
                if (g->freed) ++used_after_free;
                { std::lock_guard lock(g->mutex); }
                ++submitted;
                m.note_submitted(&fences[q], submitted);
                if (in_flight_marks) m.end_in_flight(&fences[q]);
                ++submissions;
                std::thread([&f = fences[q], v = submitted] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    std::uint64_t d = f.done.load();
                    while (d < v && !f.done.compare_exchange_weak(d, v)) {}
                }).detach();
            }
        });
    }
    threads.emplace_back([&] {
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::lock_guard lock(gen_mutex);
            const auto now = Clock::now();
            current->retired = {m.submitted(), {}, now};
            live.push_back(current);
            current = std::make_shared<Generation>();
            published = current.get();
            for (auto it = live.begin(); it != live.end();) {
                Retired& r = (*it)->retired;
                if (m.passed(r.marks, r.at, now, r.settled, r.settle)) {
                    (*it)->freed = true;
                    graveyard.push_back(*it);
                    it = live.erase(it);
                } else {
                    ++it;
                }
            }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(seconds_ms));
    stop = true;
    for (auto& t : threads) t.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::printf("  %s: %llu submissions, %llu uses of a freed generation\n",
                in_flight_marks ? "with in-flight marks" : "without", (unsigned long long)submissions.load(),
                (unsigned long long)used_after_free.load());
    return used_after_free.load();
}

}  // namespace

int main() {
    sequence();
    const std::uint64_t without = stress(false, 3000);
    check(without > 0, "the stress reproduces uses of freed generations without in-flight marks");
    const std::uint64_t with = stress(true, 3000);
    check(with == 0, "no use of a freed generation with in-flight marks");
    std::printf("%s (%d failed)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
