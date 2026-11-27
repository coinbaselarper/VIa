// MONITORX/MWAITX emulation for the title's patched wait loops (x86_patch.cpp, kZenWaitSites). The guest calls
// these through NATIVE thunks from per-site stubs that keep every register and the flags.
//
// MONITORX arms a per-thread monitor on the 64-byte line at rAX and snapshots it. MWAITX returns when that line
// differs from the snapshot or the wait budget runs out: a short spin, then waits of `step` on the qword at rAX with
// os_sync_wait_on_address (a host writer that calls gta6rt_isa_monitor_wake wakes it at once; plain stores are seen
// at the next step). The budget is the title's timer (EBX TSC ticks when ECX bit 1 is set) raised to `min` and
// capped at `max`; the title loops back to MONITORX after each return, so a longer budget only lowers the loop rate.

#include "isa_internal.h"
#include "x86_patch.h"

#include <os/os_sync_wait_on_address.h>
#include <mach/mach_time.h>
#include <time.h>
#include <x86intrin.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace gta6rt::isa {
namespace {

struct Monitor {
    uintptr_t line = 0;
    uintptr_t word = 0;
    uint64_t snap[8];
    bool armed = false;
};
thread_local Monitor t_mon;

struct Params {
    uint64_t spin_ns = 2000, step_ns = 20000, min_ns = 200000, max_ns = 1000000;
    double ns_per_tick = 1.0;
};
Params g_p;
std::atomic<bool> g_ready{false};

std::atomic<uint64_t> g_monitors{0}, g_waits{0}, g_woken{0}, g_timeouts{0}, g_unarmed{0}, g_sync_errors{0};
std::atomic<uint64_t> g_last_report{0}, g_last_waits{0};
std::atomic<uint32_t> g_last_timer{0};

uint64_t now_ns() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * tb.numer / tb.denom;
}

uint64_t env_us(const char* name, uint64_t def_ns) {
    if (const char* v = getenv(name)) {
        char* end = nullptr;
        const unsigned long long us = strtoull(v, &end, 10);
        if (end != v && *end == 0 && us <= 100000) return us * 1000;
        log_line("%s=%s ignored (expected microseconds, 0..100000)", name, v);
    }
    return def_ns;
}

void init_params() {
    if (g_ready.load(std::memory_order_acquire)) return;
    Params p;
    p.spin_ns = env_us("GTA6RT_ISA_MWAITX_SPIN_US", p.spin_ns);
    p.step_ns = env_us("GTA6RT_ISA_MWAITX_STEP_US", p.step_ns);
    p.min_ns = env_us("GTA6RT_ISA_MWAITX_MIN_US", p.min_ns);
    p.max_ns = env_us("GTA6RT_ISA_MWAITX_MAX_US", p.max_ns);
    if (p.step_ns == 0) p.step_ns = 1000;
    if (p.max_ns < p.min_ns) p.max_ns = p.min_ns;
    const uint64_t c0 = __rdtsc(), t0 = now_ns();
    timespec ts{0, 1000000};
    nanosleep(&ts, nullptr);
    const uint64_t c1 = __rdtsc(), t1 = now_ns();
    if (c1 > c0 && t1 > t0) p.ns_per_tick = double(t1 - t0) / double(c1 - c0);
    g_p = p;
    g_ready.store(true, std::memory_order_release);
    log_line("mwaitx emulation: spin %llu us, step %llu us, budget %llu..%llu us, rdtsc %.4f ns/tick",
             (unsigned long long)(p.spin_ns / 1000), (unsigned long long)(p.step_ns / 1000),
             (unsigned long long)(p.min_ns / 1000), (unsigned long long)(p.max_ns / 1000), p.ns_per_tick);
}

bool line_changed(const Monitor& m) {
    const auto* l = reinterpret_cast<const volatile uint64_t*>(m.line);
    for (int i = 0; i < 8; ++i)
        if (l[i] != m.snap[i]) return true;
    return false;
}

void report(uint64_t t) {
    uint64_t last = g_last_report.load(std::memory_order_relaxed);
    if (last == 0) {
        g_last_report.compare_exchange_strong(last, t);
        return;
    }
    if (t - last < 10000000000ull || !g_last_report.compare_exchange_strong(last, t)) return;
    const uint64_t w = g_waits.load(), dw = w - g_last_waits.exchange(w);
    log_line("mwaitx: %llu waits in the last %.1f s (%llu in all: %llu woken by a write, %llu at the budget, %llu "
             "without monitorx, %llu wait errors); title timer %u ticks",
             (unsigned long long)dw, double(t - last) / 1e9, (unsigned long long)w,
             (unsigned long long)g_woken.load(), (unsigned long long)g_timeouts.load(),
             (unsigned long long)g_unarmed.load(), (unsigned long long)g_sync_errors.load(), g_last_timer.load());
}

}  // namespace

ZenWaitStats zen_wait_stats() {
    return ZenWaitStats{g_monitors.load(), g_waits.load(), g_woken.load(), g_timeouts.load(), g_unarmed.load(),
                        g_sync_errors.load()};
}

}  // namespace gta6rt::isa

using namespace gta6rt::isa;

extern "C" GTA6RT_GUEST_ABI void gta6rt_rosetta_monitorx(uint64_t addr) {
    Monitor& m = t_mon;
    m.line = addr & ~uint64_t{63};
    m.word = addr & ~uint64_t{7};
    std::memcpy(m.snap, reinterpret_cast<const void*>(m.line), sizeof m.snap);
    m.armed = true;
    g_monitors.fetch_add(1, std::memory_order_relaxed);
}

extern "C" GTA6RT_GUEST_ABI void gta6rt_rosetta_mwaitx(uint64_t ecx, uint64_t eax, uint64_t ebx) {
    (void)eax;
    Monitor& m = t_mon;
    if (!m.armed) {
        g_unarmed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m.armed = false;
    init_params();
    g_waits.fetch_add(1, std::memory_order_relaxed);
    const uint64_t start = now_ns();
    uint64_t budget = g_p.max_ns;
    if (ecx & 2) {
        g_last_timer.store(static_cast<uint32_t>(ebx), std::memory_order_relaxed);
        budget = static_cast<uint64_t>(double(static_cast<uint32_t>(ebx)) * g_p.ns_per_tick);
    }
    if (budget < g_p.min_ns) budget = g_p.min_ns;
    if (budget > g_p.max_ns) budget = g_p.max_ns;
    const uint64_t deadline = start + budget;
    report(start);
    for (uint64_t t = start; t - start < g_p.spin_ns; t = now_ns()) {
        if (line_changed(m)) {
            g_woken.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        _mm_pause();
    }
    uint64_t t = now_ns();
    bool use_sync = true;
    while (t < deadline) {
        const uint64_t step = deadline - t < g_p.step_ns ? deadline - t : g_p.step_ns;
        bool slept = false;
        if (use_sync) {
            if (__builtin_available(macOS 14.4, *)) {
                const uint64_t expect = m.snap[(m.word - m.line) / 8];
                const int r = os_sync_wait_on_address_with_timeout(reinterpret_cast<void*>(m.word), expect, 8,
                                                                   OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                                   OS_CLOCK_MACH_ABSOLUTE_TIME, step);
                if (r >= 0 || errno == ETIMEDOUT || errno == EINTR) {
                    slept = true;
                } else {
                    g_sync_errors.fetch_add(1, std::memory_order_relaxed);
                    use_sync = false;
                }
            } else {
                use_sync = false;
            }
        }
        if (!slept) {
            timespec ts{0, static_cast<long>(step)};
            nanosleep(&ts, nullptr);
        }
        if (line_changed(m)) {
            g_woken.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        t = now_ns();
    }
    g_timeouts.fetch_add(1, std::memory_order_relaxed);
}

namespace gta6rt::isa {

uint64_t rosetta_zen_wait_entry(ZenWaitKind kind) {
    static uint64_t entries[3];
    static std::atomic<bool> made{false};
    static std::atomic<int> lock{0};
    if (!made.load(std::memory_order_acquire)) {
        while (lock.exchange(1, std::memory_order_acquire)) {
        }
        const char* mode = getenv("GTA6RT_ISA_MWAITX");
        if (!made.load(std::memory_order_relaxed) && mode && std::strcmp(mode, "nop") == 0) {
            log_line("mwaitx emulation off (GTA6RT_ISA_MWAITX=nop): the title's MONITORX/MWAITX become NOPs and its wait loop spins");
            made.store(true, std::memory_order_release);
        }
        if (!made.load(std::memory_order_relaxed)) {
            gta6rt_thunk_desc d{};
            d.kind = GTA6RT_THUNK_NATIVE;
            d.fn = reinterpret_cast<const void*>(&gta6rt_rosetta_monitorx);
            d.debug_name = "isa!monitorx";
            gta6rt_gva a = 0;
            if (gta6rt_isa_make_thunk(&d, &a) == GTA6RT_OK) entries[1] = a;
            d.fn = reinterpret_cast<const void*>(&gta6rt_rosetta_mwaitx);
            d.debug_name = "isa!mwaitx";
            a = 0;
            if (gta6rt_isa_make_thunk(&d, &a) == GTA6RT_OK) entries[2] = a;
            made.store(true, std::memory_order_release);
        }
        lock.store(0, std::memory_order_release);
    }
    return entries[static_cast<int>(kind)];
}

}  // namespace gta6rt::isa
