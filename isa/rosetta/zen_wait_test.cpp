// isa_zen_wait_test (x86_64/Rosetta): the MONITORX/MWAITX wait windows of the title, patched into generated code.
//   1. register and flag preservation across both window kinds;
//   2. a wait loop of the title's shape woken by a writer thread: wake latency and the waiter's CPU time, with the
//      emulation, with gta6rt_isa_monitor_wake after each store, and with the windows as NOPs (the S1 state).
// Tuning: GTA6RT_ISA_MWAITX_{SPIN,STEP,MIN,MAX}_US.

#include "x86_patch.h"

#include <gta6rt/isa.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace gta6rt::isa;

namespace gta6rt::isa {
uint64_t rosetta_zen_wait_entry(ZenWaitKind kind);
}

namespace {

int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_fail;                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                     \
    } while (0)

const uint8_t kWinMonitor[10] = {0x48, 0x89, 0xf8, 0x31, 0xc9, 0x31, 0xd2, 0x0f, 0x01, 0xfa};
const uint8_t kWinMwait[10] = {0xb9, 0x02, 0x00, 0x00, 0x00, 0x31, 0xc0, 0x0f, 0x01, 0xfb};

uint8_t* g_code = nullptr;
size_t g_used = 0;

uint8_t* emit(const std::vector<uint8_t>& b) {
    uint8_t* at = g_code + g_used;
    std::memcpy(at, b.data(), b.size());
    g_used = (g_used + b.size() + 63) & ~size_t{63};
    return at;
}

void add(std::vector<uint8_t>& v, std::initializer_list<uint8_t> b) { v.insert(v.end(), b); }
void add32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

struct alignas(16) Regs {
    uint64_t unused, addr, rbx, r8, r9, r10, r11, pad0;
    uint64_t xin[12];
    uint64_t flags, rax, rcx, rdx, obx, o8, o9, o10, o11, rdi;
    uint64_t xout[12];
};
static_assert(offsetof(Regs, addr) == 0x08 && offsetof(Regs, xin) == 0x40 && offsetof(Regs, flags) == 0xa0 &&
              offsetof(Regs, rdi) == 0xe8 && offsetof(Regs, xout) == 0xf0);

// ms_abi fn(Regs* rcx): loads rdi, rbx, r8-r11, xmm0-5, runs the window, stores flags and registers.
uint8_t* gen_preserve(const uint8_t* window, uint8_t** window_at) {
    std::vector<uint8_t> v;
    add(v, {0x53, 0x55, 0x57, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x49, 0x89, 0xCF});
    add(v, {0x49, 0x8B, 0x7F, 0x08, 0x49, 0x8B, 0x5F, 0x10});
    add(v, {0x4D, 0x8B, 0x47, 0x18, 0x4D, 0x8B, 0x4F, 0x20, 0x4D, 0x8B, 0x57, 0x28, 0x4D, 0x8B, 0x5F, 0x30});
    add(v, {0xF3, 0x41, 0x0F, 0x6F, 0x47, 0x40, 0xF3, 0x41, 0x0F, 0x6F, 0x4F, 0x50});
    add(v, {0xF3, 0x41, 0x0F, 0x6F, 0x57, 0x60, 0xF3, 0x41, 0x0F, 0x6F, 0x5F, 0x70});
    add(v, {0xF3, 0x41, 0x0F, 0x6F, 0xA7, 0x80, 0, 0, 0, 0xF3, 0x41, 0x0F, 0x6F, 0xAF, 0x90, 0, 0, 0});
    const size_t woff = v.size();
    v.insert(v.end(), window, window + 10);
    add(v, {0x9C, 0x41, 0x8F, 0x87, 0xA0, 0, 0, 0});
    const uint8_t gpr[][2] = {{0x49, 0x87}, {0x49, 0x8F}, {0x49, 0x97}, {0x49, 0x9F}, {0x4D, 0x87},
                              {0x4D, 0x8F}, {0x4D, 0x97}, {0x4D, 0x9F}, {0x49, 0xBF}};
    for (int i = 0; i < 9; ++i) {
        add(v, {gpr[i][0], 0x89, gpr[i][1]});
        add32(v, 0xA8 + 8 * i);
    }
    for (int x = 0; x < 6; ++x) {
        add(v, {0xF3, 0x41, 0x0F, 0x7F, static_cast<uint8_t>(0x87 | (x << 3))});
        add32(v, 0xF0 + 16 * x);
    }
    add(v, {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5E, 0x5F, 0x5D, 0x5B, 0xC3});
    uint8_t* fn = emit(v);
    *window_at = fn + woff;
    return fn;
}

// ms_abi uint64_t fn(uint64_t* addr, uint64_t target): the title's loop shape; returns the iterations it waited.
uint8_t* gen_loop(uint8_t** a_at, uint8_t** b_at, uint32_t timer) {
    std::vector<uint8_t> v;
    add(v, {0x53, 0x57, 0x56, 0x48, 0x89, 0xCF, 0x48, 0x89, 0xD6, 0x45, 0x31, 0xC9, 0xBB});
    add32(v, timer);
    const size_t l = v.size();
    v.insert(v.end(), kWinMonitor, kWinMonitor + 10);
    add(v, {0x48, 0x8B, 0x17, 0x48, 0x39, 0xF2, 0x73, 0});
    const size_t j1 = v.size() - 1;
    const size_t b = v.size();
    v.insert(v.end(), kWinMwait, kWinMwait + 10);
    add(v, {0x48, 0x8B, 0x17, 0x48, 0x39, 0xF2, 0x73, 0});
    const size_t j2 = v.size() - 1;
    const int back = static_cast<int>(l) - static_cast<int>(v.size() + 5);
    add(v, {0x49, 0xFF, 0xC1, 0xEB, static_cast<uint8_t>(static_cast<int8_t>(back))});
    const size_t done = v.size();
    v[j1] = static_cast<uint8_t>(done - (j1 + 1));
    v[j2] = static_cast<uint8_t>(done - (j2 + 1));
    add(v, {0x4C, 0x89, 0xC8, 0x5E, 0x5F, 0x5B, 0xC3});
    uint8_t* fn = emit(v);
    *a_at = fn + l;
    *b_at = fn + b;
    return fn;
}

using PreserveFn = void(__attribute__((ms_abi)) *)(Regs*);
using LoopFn = uint64_t(__attribute__((ms_abi)) *)(volatile uint64_t*, uint64_t);

uint64_t now_ns() {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

uint64_t thread_cpu_ns(thread_t t) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(t, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) != KERN_SUCCESS) return 0;
    return (info.user_time.seconds + info.system_time.seconds) * 1000000000ull +
           (info.user_time.microseconds + info.system_time.microseconds) * 1000ull;
}

void test_preserve(ZenWaitKind kind) {
    uint8_t* w = nullptr;
    auto fn = reinterpret_cast<PreserveFn>(gen_preserve(kind == ZenWaitKind::MonitorX ? kWinMonitor : kWinMwait, &w));
    CHECK(patch_zen_wait_window(reinterpret_cast<uint64_t>(w), kind == ZenWaitKind::MonitorX ? kWinMonitor : kWinMwait,
                                10, 7, kind) == 1);
    CHECK(w[0] == 0xE8);
    static uint64_t word = 0x1234;
    Regs r{};
    r.addr = reinterpret_cast<uint64_t>(&word);
    r.rbx = 1000;
    r.r8 = 0x8888888888888888ull;
    r.r9 = 0x9999999999999999ull;
    r.r10 = 0xaaaaaaaaaaaaaaaaull;
    r.r11 = 0xbbbbbbbbbbbbbbbbull;
    for (int i = 0; i < 12; ++i) r.xin[i] = 0x0101010101010101ull * (i + 1);
    fn(&r);
    if (kind == ZenWaitKind::MonitorX) {
        CHECK(r.rax == r.addr);
        CHECK(r.rcx == 0 && r.rdx == 0);
    } else {
        CHECK(r.rax == 0 && r.rcx == 2);
    }
    CHECK((r.flags & 0x8d5) == 0x44);
    CHECK(r.obx == 1000 && r.o8 == r.r8 && r.o9 == r.r9 && r.o10 == r.r10 && r.o11 == r.r11 && r.rdi == r.addr);
    CHECK(std::memcmp(r.xin, r.xout, sizeof r.xin) == 0);
}

struct Shared {
    alignas(64) volatile uint64_t value = 0;
    std::atomic<int> phase{0};
    LoopFn loop = nullptr;
    int rounds = 0;
    bool wake = false;
    std::vector<uint64_t> stored, returned;
    uint64_t waiter_cpu = 0, waiter_wall = 0;
};

void* waiter(void* p) {
    auto* s = static_cast<Shared*>(p);
    const thread_t self = mach_thread_self();
    const uint64_t c0 = thread_cpu_ns(self), t0 = now_ns();
    for (int i = 1; i <= s->rounds; ++i) {
        s->loop(&s->value, static_cast<uint64_t>(i));
        s->returned[i - 1] = now_ns();
    }
    s->waiter_cpu = thread_cpu_ns(self) - c0;
    s->waiter_wall = now_ns() - t0;
    mach_port_deallocate(mach_task_self(), self);
    return nullptr;
}

void run_wait(const char* label, LoopFn loop, bool wake, int rounds) {
    Shared s;
    s.loop = loop;
    s.rounds = rounds;
    s.wake = wake;
    s.stored.assign(rounds, 0);
    s.returned.assign(rounds, 0);
    pthread_t t;
    pthread_create(&t, nullptr, waiter, &s);
    srandom(7);
    for (int i = 1; i <= rounds; ++i) {
        const useconds_t gap = 1000 + static_cast<useconds_t>(random() % 2000);
        usleep(gap);
        s.stored[i - 1] = now_ns();
        s.value = static_cast<uint64_t>(i);
        if (wake) gta6rt_isa_monitor_wake(const_cast<const uint64_t*>(&s.value));
    }
    pthread_join(t, nullptr);
    std::vector<uint64_t> lat;
    for (int i = 0; i < rounds; ++i)
        if (s.returned[i] >= s.stored[i]) lat.push_back(s.returned[i] - s.stored[i]);
    std::sort(lat.begin(), lat.end());
    CHECK(lat.size() == static_cast<size_t>(rounds));
    if (lat.empty()) return;
    std::printf("%-34s rounds %d: wake latency p50 %6.1f us p90 %6.1f us max %7.1f us; waiter CPU %5.1f%% of %.2f s\n",
                label, rounds, lat[lat.size() / 2] / 1e3, lat[lat.size() * 9 / 10] / 1e3, lat.back() / 1e3,
                100.0 * double(s.waiter_cpu) / double(s.waiter_wall), s.waiter_wall / 1e9);
}

}  // namespace

int main(int argc, char** argv) {
    gta6rt_isa_config cfg{};
    CHECK(gta6rt_isa_init(&cfg) == GTA6RT_OK);
    void* p = mmap(nullptr, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(p != MAP_FAILED);
    g_code = static_cast<uint8_t*>(p);
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 300;

    CHECK(rosetta_zen_wait_entry(ZenWaitKind::MonitorX) != 0 && rosetta_zen_wait_entry(ZenWaitKind::MWaitX) != 0);
    test_preserve(ZenWaitKind::MonitorX);
    test_preserve(ZenWaitKind::MWaitX);
    const ZenWaitStats st0 = zen_wait_stats();
    CHECK(st0.monitors >= 1 && st0.waits >= 1);

    uint8_t *a = nullptr, *b = nullptr;
    auto emulated = reinterpret_cast<LoopFn>(gen_loop(&a, &b, 1000));
    CHECK(patch_zen_wait_window(reinterpret_cast<uint64_t>(a), kWinMonitor, 10, 7, ZenWaitKind::MonitorX) == 1);
    CHECK(patch_zen_wait_window(reinterpret_cast<uint64_t>(b), kWinMwait, 10, 7, ZenWaitKind::MWaitX) == 1);
    CHECK(patch_zen_wait_window(reinterpret_cast<uint64_t>(b), kWinMwait, 10, 7, ZenWaitKind::MWaitX) == -1);

    set_zen_wait_provider(nullptr);
    auto nops = reinterpret_cast<LoopFn>(gen_loop(&a, &b, 1000));
    CHECK(patch_zen_wait_window(reinterpret_cast<uint64_t>(a), kWinMonitor, 10, 7, ZenWaitKind::MonitorX) == 0);
    CHECK(patch_zen_wait_window(reinterpret_cast<uint64_t>(b), kWinMwait, 10, 7, ZenWaitKind::MWaitX) == 0);
    CHECK(a[7] == 0x0f && a[8] == 0x1f && a[9] == 0x00 && a[0] == 0x48);

    run_wait("emulated (poll)", emulated, false, rounds);
    run_wait("emulated + gta6rt_isa_monitor_wake", emulated, true, rounds);
    run_wait("NOP windows (S1 state)", nops, false, rounds / 3);
    const ZenWaitStats st = zen_wait_stats();
    std::printf("stats: monitors %llu waits %llu woken %llu at budget %llu unarmed %llu wait errors %llu\n",
                (unsigned long long)st.monitors, (unsigned long long)st.waits, (unsigned long long)st.woken,
                (unsigned long long)st.timeouts, (unsigned long long)st.unarmed, (unsigned long long)st.sync_errors);
    CHECK(st.sync_errors == 0);
    std::printf("isa_zen_wait_test: %s, %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
