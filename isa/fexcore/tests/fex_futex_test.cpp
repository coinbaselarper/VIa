// third_party/fex/compat futex emulation: the Linux futex semantics FEX relies on, and FEX's own WritePriorityMutex and
// InterruptableConditionVariable running on it with many host threads.
// usage: fex_futex_test [bitset|mutex|timeout|cv|unknown]...   (default: all)

#include <FEXCore/Utils/InterruptableConditionVariable.h>
#include <FEXCore/Utils/WritePriorityMutex.h>

#include "../../tests/test_common.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <ctime>
#include <pthread.h>
#include <sched.h>

namespace {

constexpr uint32_t kReaders = 1u << 0;
constexpr uint32_t kWriters = 1u << 1;

long futex(uint32_t* a, int op, uint32_t val, const timespec* ts, uint32_t bitset) {
    return syscall(SYS_futex, a, op, val, ts, nullptr, bitset);
}

uint64_t now_ns() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

std::atomic<uint64_t> g_progress{0};
std::atomic<const char*> g_phase{nullptr};
uint32_t* g_watch_word = nullptr;

void* watchdog(void*) {
    uint64_t last = 0;
    int still = 0;
    for (;;) {
        usleep(100000);
        const char* phase = g_phase.load();
        if (!phase) {
            still = 0;
            continue;
        }
        const uint64_t now = g_progress.load();
        still = now == last ? still + 1 : 0;
        last = now;
        if (still >= 50) {
            std::printf("FAIL fex_futex_test: %s: no progress for 5 s (%llu steps, futex word 0x%08x): deadlock\n", phase,
                        (unsigned long long)now, g_watch_word ? __atomic_load_n(g_watch_word, __ATOMIC_RELAXED) : 0u);
            _exit(3);
        }
    }
    return nullptr;
}

struct Waiter {
    uint32_t* word;
    uint32_t expect;
    uint32_t bitset;
    std::atomic<bool> started{false};
    std::atomic<bool> returned{false};
};

void* wait_main(void* p) {
    auto* w = static_cast<Waiter*>(p);
    w->started.store(true);
    futex(w->word, FUTEX_PRIVATE_FLAG | FUTEX_WAIT_BITSET, w->expect, nullptr, w->bitset);
    w->returned.store(true);
    return nullptr;
}

// The deadlock shape from review round 3: readers and one writer sleep on the same word; a wake of one writer must
// reach the writer.
void test_bitset_wake() {
    alignas(4) static uint32_t word = 5;
    constexpr int kSide = 8;
    Waiter ws[2 * kSide + 1];
    pthread_t th[2 * kSide + 1];
    for (int i = 0; i < 2 * kSide + 1; ++i) {
        ws[i].word = &word;
        ws[i].expect = 5;
        ws[i].bitset = i == kSide ? kWriters : kReaders;
        pthread_create(&th[i], nullptr, wait_main, &ws[i]);
        while (!ws[i].started.load()) sched_yield();
        usleep(20000);
    }
    usleep(100000);
    int early = 0;
    for (auto& w : ws) early += w.returned.load();
    CHECK(early == 0, "bitset: %d waiter(s) returned before any wake", early);
    futex(&word, FUTEX_PRIVATE_FLAG | FUTEX_WAKE_BITSET, 1, nullptr, kWriters);
    const uint64_t t0 = now_ns();
    while (!ws[kSide].returned.load() && now_ns() - t0 < 2000000000ull) usleep(1000);
    const bool writer = ws[kSide].returned.load();
    int readers = 0;
    for (int i = 0; i < 2 * kSide + 1; ++i) readers += i != kSide && ws[i].returned.load();
    std::printf("bitset: WAKE_BITSET(1, writers) with %d readers and 1 writer asleep: writer %s, %d reader(s) woken too\n",
                2 * kSide, writer ? "woken" : "NOT woken after 2 s", readers);
    CHECK(writer, "bitset: FUTEX_WAKE_BITSET(1, WRITERS) must wake the writer, not only a reader");
    __atomic_store_n(&word, 6, __ATOMIC_RELEASE);
    futex(&word, FUTEX_PRIVATE_FLAG | FUTEX_WAKE_BITSET, INT_MAX, nullptr, FUTEX_BITSET_MATCH_ANY);
    for (auto& t : th) pthread_join(t, nullptr);
}

FEXCore::Utils::WritePriorityMutex::Mutex g_mutex;
uint64_t g_guarded = 0;
uint64_t g_guarded_copy = 0;

void spin_ns(uint64_t ns) {
    const uint64_t t0 = now_ns();
    while (now_ns() - t0 < ns) {
    }
}

struct MutexArg {
    uint32_t index;
    uint32_t iterations;
    uint64_t writes = 0;
    uint64_t torn = 0;
};

void* mutex_main(void* p) {
    auto* a = static_cast<MutexArg*>(p);
    Rng rng(0x9E3779B97F4A7C15ull * (a->index + 1));
    for (uint32_t n = 0; n < a->iterations; ++n) {
        const uint64_t r = rng.next();
        if ((r & 7) == 0) {
            g_mutex.lock();
            const uint64_t v = g_guarded;
            spin_ns(15000 + (r >> 8) % 20000);
            g_guarded = v + 1;
            g_guarded_copy = v + 1;
            g_mutex.unlock();
            ++a->writes;
        } else {
            g_mutex.lock_shared();
            const uint64_t v = g_guarded;
            spin_ns(2000 + (r >> 8) % 15000);
            if (g_guarded != v || g_guarded_copy != v) ++a->torn;
            g_mutex.unlock_shared();
        }
        g_progress.fetch_add(1, std::memory_order_relaxed);
    }
    return nullptr;
}

// FEX's LookupCache lock pattern: many readers, writers holding it longer than the 10 us WFE spin, so both sides
// reach the futex sleep.
void test_mutex(int threads, uint32_t iterations) {
    g_watch_word = reinterpret_cast<uint32_t*>(&g_mutex);
    g_guarded = g_guarded_copy = 0;
    g_phase.store("WritePriorityMutex");
    std::vector<pthread_t> th(threads);
    std::vector<MutexArg> args(threads);
    const uint64_t t0 = now_ns();
    for (int i = 0; i < threads; ++i) {
        args[i].index = uint32_t(i);
        args[i].iterations = iterations;
        pthread_create(&th[i], nullptr, mutex_main, &args[i]);
    }
    for (auto& t : th) pthread_join(t, nullptr);
    g_phase.store(nullptr);
    uint64_t writes = 0, torn = 0;
    for (auto& a : args) writes += a.writes, torn += a.torn;
    std::printf("mutex: %d threads x %u lock/lock_shared, %llu writes, %.1f ms, guarded %llu, torn reads %llu\n", threads,
                iterations, (unsigned long long)writes, double(now_ns() - t0) / 1e6, (unsigned long long)g_guarded,
                (unsigned long long)torn);
    CHECK(g_guarded == writes && torn == 0, "mutex: exclusion holds (guarded %llu, writes %llu, torn %llu)",
          (unsigned long long)g_guarded, (unsigned long long)writes, (unsigned long long)torn);
}

void test_timeout() {
    alignas(4) static uint32_t word = 1;
    g_phase.store("timeout");
    timespec rel{0, 30000000};
    uint64_t t0 = now_ns();
    long r = futex(&word, FUTEX_PRIVATE_FLAG | FUTEX_WAIT, 1, &rel, 0);
    int e = errno;
    double ms = double(now_ns() - t0) / 1e6;
    std::printf("timeout: FUTEX_WAIT relative 30 ms -> %ld errno %d after %.1f ms\n", r, e, ms);
    CHECK(r == -1 && e == ETIMEDOUT && ms >= 29.0, "FUTEX_WAIT relative timeout");

    timespec abs{};
    clock_gettime(CLOCK_MONOTONIC, &abs);
    abs.tv_nsec += 30000000;
    if (abs.tv_nsec >= 1000000000) abs.tv_sec += 1, abs.tv_nsec -= 1000000000;
    t0 = now_ns();
    r = futex(&word, FUTEX_PRIVATE_FLAG | FUTEX_WAIT_BITSET, 1, &abs, FUTEX_BITSET_MATCH_ANY);
    e = errno;
    ms = double(now_ns() - t0) / 1e6;
    std::printf("timeout: FUTEX_WAIT_BITSET absolute CLOCK_MONOTONIC +30 ms -> %ld errno %d after %.1f ms\n", r, e, ms);
    CHECK(r == -1 && e == ETIMEDOUT && ms >= 29.0 && ms < 2000.0, "FUTEX_WAIT_BITSET absolute timeout");

    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec -= 1;
    t0 = now_ns();
    r = futex(&word, FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME | FUTEX_WAIT_BITSET, 1, &abs, FUTEX_BITSET_MATCH_ANY);
    e = errno;
    ms = double(now_ns() - t0) / 1e6;
    CHECK(r == -1 && e == ETIMEDOUT && ms < 1000.0, "FUTEX_WAIT_BITSET deadline in the past (CLOCK_REALTIME)");

    t0 = now_ns();
    r = futex(&word, FUTEX_PRIVATE_FLAG | FUTEX_WAIT, 2, nullptr, 0);
    ms = double(now_ns() - t0) / 1e6;
    CHECK(r == 0 && ms < 1000.0, "FUTEX_WAIT with a different value returns at once");
    g_phase.store(nullptr);
}

FEXCore::InterruptableConditionVariable g_cv;

void* notify_main(void*) {
    usleep(20000);
    g_cv.NotifyOne();
    return nullptr;
}

void test_cv() {
    g_phase.store("InterruptableConditionVariable");
    uint64_t t0 = now_ns();
    const bool timed = g_cv.WaitFor(std::chrono::milliseconds(25));
    const double ms = double(now_ns() - t0) / 1e6;
    CHECK(!timed && ms >= 24.0, "cv: WaitFor 25 ms without a notify times out (%.1f ms)", ms);
    pthread_t t;
    pthread_create(&t, nullptr, notify_main, nullptr);
    t0 = now_ns();
    const bool woke = g_cv.WaitFor(std::chrono::seconds(3));
    const double wms = double(now_ns() - t0) / 1e6;
    pthread_join(t, nullptr);
    CHECK(woke && wms < 2000.0, "cv: NotifyOne wakes the waiter (%.1f ms)", wms);
    g_phase.store(nullptr);
}

void test_unknown() {
    alignas(4) static uint32_t word = 0;
    errno = 0;
    const long r = syscall(SYS_futex, &word, FUTEX_PRIVATE_FLAG | 3, 1, nullptr, nullptr, 0);
    CHECK(r == -1 && errno == ENOSYS, "unsupported futex op fails with ENOSYS");
    errno = 0;
    const long s = syscall(SYS_statx, -1, "", 0, 0, nullptr);
    CHECK(s == -1 && errno == ENOSYS, "unimplemented syscall fails with ENOSYS");
}

bool want(int argc, char** argv, const char* name) {
    if (argc < 2) return true;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], name)) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    pthread_t w;
    pthread_create(&w, nullptr, watchdog, nullptr);
    pthread_detach(w);
    if (want(argc, argv, "bitset")) test_bitset_wake();
    if (want(argc, argv, "mutex")) {
        test_mutex(4, 4000);
        test_mutex(16, 4000);
    }
    if (want(argc, argv, "timeout")) test_timeout();
    if (want(argc, argv, "cv")) test_cv();
    if (want(argc, argv, "unknown")) test_unknown();
    return finish("fex_futex_test");
}
