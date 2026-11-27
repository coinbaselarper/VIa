#include "governor.h"

#include <gta6rt/governor.h>
#include <gta6rt/gpu_memory.h>
#include <gta6rt/os.h>
#include <gta6rt_hud/stats.h>

#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gpu_metal.h"
#include "title_hooks.h"
#include "xmem.h"

namespace host::governor {

namespace {

constexpr double kMiB = 1024.0 * 1024.0;
constexpr uint64_t kGiB = 1ull << 30;
constexpr uint64_t kTitleS = 7776ull << 20, kTitleX = 13824ull << 20;
// S3 (port/STATUS.md round 4): 13.7 GiB footprint at the world phase with the 7.6 GiB Series S title memory.
constexpr uint64_t kHostAdd = 6ull * kGiB + (kGiB / 2);
constexpr uint32_t kGameStateRva = 0x7446510, kSessionRva = 0x98f828c, kLoadingRva = 0x7453ed0;
constexpr double kCooldownS = 15.0;

Options g_opt;
Budget g_start_budget;
std::atomic<uint32_t> g_level{0};
std::atomic<uint32_t> g_os_event{0};
std::atomic<bool> g_started{false};
uint64_t g_exe_base = 0;
std::chrono::steady_clock::time_point g_t0;

struct Reclaimer {
    std::string name;
    gta6rt_governor_reclaim_fn fn;
    void* ctx;
};
std::mutex g_reg_mu;
std::vector<Reclaimer> g_reclaimers[GTA6RT_GOV_ACTION_COUNT];

std::mutex g_report_mu, g_poll_mu;
std::condition_variable g_report_cv, g_poll_cv;
std::vector<std::string> g_table_requests;

std::atomic<uint64_t> g_poll_ns_total{0}, g_polls{0}, g_poll_ns_max{0};
std::atomic<uint64_t> g_walk_ns_total{0}, g_walks{0}, g_walk_ns_max{0};
std::atomic<uint64_t> g_poll_thread_cpu_ns{0}, g_report_thread_cpu_ns{0};

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[gov] %s\n", buf);
}

double since_start() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count(); }

uint64_t now_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

uint64_t thread_cpu_ns() {
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    const mach_port_t self = mach_thread_self();
    const kern_return_t kr = thread_info(self, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n);
    mach_port_deallocate(mach_task_self(), self);
    if (kr != KERN_SUCCESS) return 0;
    return (uint64_t(info.user_time.seconds) + info.system_time.seconds) * 1000000000ull +
           (uint64_t(info.user_time.microseconds) + info.system_time.microseconds) * 1000ull;
}

void raise_max(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t seen = a.load(std::memory_order_relaxed);
    while (v > seen && !a.compare_exchange_weak(seen, v, std::memory_order_relaxed)) {
    }
}

struct Process {
    uint64_t footprint = 0, resident = 0, compressed = 0, internal = 0, external = 0;
    uint64_t graphics = 0, graphics_compressed = 0, graphics_nofootprint = 0;
    int64_t swapins = 0;
};

Process read_process() {
    Process p;
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vi), &n) != KERN_SUCCESS) return p;
    p.footprint = vi.phys_footprint;
    p.resident = vi.resident_size;
    p.compressed = vi.compressed;
    p.internal = vi.internal;
    p.external = vi.external;
    if (n >= TASK_VM_INFO_REV3_COUNT) {
        p.graphics = uint64_t(std::max<int64_t>(0, vi.ledger_tag_graphics_footprint));
        p.graphics_compressed = uint64_t(std::max<int64_t>(0, vi.ledger_tag_graphics_footprint_compressed));
        p.graphics_nofootprint = uint64_t(std::max<int64_t>(0, vi.ledger_tag_graphics_nofootprint));
    }
    if (n >= TASK_VM_INFO_REV6_COUNT) p.swapins = vi.ledger_swapins;
    return p;
}

uint32_t memorystatus_level() {
    int v = 0;
    size_t n = sizeof v;
    if (sysctlbyname("kern.memorystatus_level", &v, &n, nullptr, 0) != 0) return 0;
    return uint32_t(std::clamp(v, 0, 100));
}

const char* pressure_name(uint32_t level) { return level >= 4 ? "critical" : level >= 2 ? "warn" : "normal"; }

std::string mib(uint64_t b) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", double(b) / kMiB);
    return buf;
}

// ---- stage

bool read_u64(uint64_t at, uint64_t* out) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), at, sizeof *out, reinterpret_cast<mach_vm_address_t>(out), &got) ==
               KERN_SUCCESS &&
           got == sizeof *out;
}

std::string stage_of(uint64_t base, bool* loading_seen) {
    uint64_t game = 0, session = 0, load = 0;
    if (!base || !read_u64(base + kGameStateRva, &game)) return "no title state";
    const bool s = read_u64(base + kSessionRva, &session), l = read_u64(base + kLoadingRva, &load);
    if ((game & 0xffffffff) != 3) return "game state " + std::to_string(game & 0xffffffff);
    if (!s) return "running";
    const uint32_t sess = uint32_t(session);
    if (l && (load & 0xff) == 2) *loading_seen = true;
    if (sess == 0 && l && *loading_seen && (load & 0xff) == 0) return "world (loading screen ended)";
    switch (sess) {
    case 1: return "boot screens";
    case 3: return "landing";
    case 0xd: return "first-run pages";
    case 0: return l && (load & 0xff) ? "story load" : "session 0";
    default: return "session " + std::to_string(sess);
    }
}

// ---- accounting

struct Categories {
    uint64_t pool_reserved = 0, pool_committed = 0, pool_resident = 0, pool_compressed = 0;
    uint64_t pool_live = 0, pool_free_slots = 0, pool_slots_used = 0;
    uint64_t win_reserved = 0, win_committed = 0, win_resident = 0, win_compressed = 0;
    uint64_t img_committed = 0, img_resident = 0, img_compressed = 0;
    uint64_t view_committed = 0, view_resident = 0, view_compressed = 0;
    uint64_t priv_reserved = 0, priv_committed = 0, priv_resident = 0, priv_compressed = 0;
    uint64_t released_touched = 0;
    uint64_t malloc_in_use = 0, malloc_allocated = 0;
    size_t regions = 0;
    double walk_ms = 0;
};

bool overlaps(uint64_t base, uint64_t size, const std::vector<std::pair<uintptr_t, uint64_t>>& ranges) {
    for (const auto& [b, s] : ranges)
        if (base < b + s && b < base + size) return true;
    return false;
}

Categories walk(const char* why) {
    Categories c;
    std::vector<gta6rt_memory_region_usage> regions(4096);
    size_t count = 0;
    const uint64_t t0 = now_ns();
    gta6rt_os_memory_report_regions(why, 64ull << 20, 12, regions.data(), regions.size(), &count);
    const uint64_t dt = now_ns() - t0;
    g_walk_ns_total.fetch_add(dt, std::memory_order_relaxed);
    g_walks.fetch_add(1, std::memory_order_relaxed);
    raise_max(g_walk_ns_max, dt);
    c.walk_ms = double(dt) / 1e6;
    c.regions = count;
    const rt::xmem::PoolInfo pool = rt::xmem::pool_info();
    c.pool_live = pool.live_pages;
    c.pool_free_slots = pool.free_slots;
    c.pool_slots_used = pool.slots_used;
    const auto windows = rt::xmem::window_ranges();
    for (size_t i = 0; i < std::min(count, regions.size()); ++i) {
        const gta6rt_memory_region_usage& u = regions[i];
        c.released_touched += u.released_touched;
        if (pool.base && u.base == reinterpret_cast<uintptr_t>(pool.base)) {
            c.pool_reserved += u.size;
            c.pool_committed += u.committed;
            c.pool_resident += u.resident;
            c.pool_compressed += u.compressed;
        } else if (u.kind == 3) {
            c.img_committed += u.committed;
            c.img_resident += u.resident;
            c.img_compressed += u.compressed;
        } else if (u.kind == 2) {
            c.view_committed += u.committed;
            c.view_resident += u.resident;
            c.view_compressed += u.compressed;
        } else if (overlaps(u.base, u.size, windows)) {
            c.win_reserved += u.size;
            c.win_committed += u.committed;
            c.win_resident += u.resident;
            c.win_compressed += u.compressed;
        } else {
            c.priv_reserved += u.size;
            c.priv_committed += u.committed;
            c.priv_resident += u.resident;
            c.priv_compressed += u.compressed;
        }
    }
    malloc_statistics_t ms{};
    malloc_zone_statistics(nullptr, &ms);
    c.malloc_in_use = ms.size_in_use;
    c.malloc_allocated = ms.size_allocated;
    return c;
}

// ---- reclaimers

uint64_t run_registered(gta6rt_governor_action action, uint32_t level, uint64_t goal, std::string* names) {
    std::vector<Reclaimer> list;
    {
        std::lock_guard lock(g_reg_mu);
        list = g_reclaimers[action];
    }
    uint64_t total = 0;
    for (const Reclaimer& r : list) {
        const uint64_t got = r.fn(r.ctx, level, goal);
        total += got;
        if (names) *names += (names->empty() ? "" : ", ") + r.name + " " + mib(got) + " MiB";
    }
    return total;
}

size_t registered(gta6rt_governor_action action) {
    std::lock_guard lock(g_reg_mu);
    return g_reclaimers[action].size();
}

struct ActionState {
    double last_s = -1e9;
    uint64_t fired = 0;
    uint64_t claimed = 0;
    int64_t measured = 0;
    bool told_empty = false;
};
ActionState g_actions[5];
const char* const kActionName[5] = {"a layer-evict", "b compile-shrink", "c host-caches", "d pool-pages", "e warn"};

struct Sample {
    Process p;
    gta6rt_system_memory sys{};
    gpu::Info gpu;
    gta6rt_gpu_bytes layer{};
    uint64_t budget = 0, available = 0;
    uint32_t available_pct = 100;
    double fp_frac = 0, gpu_frac = 0;
    uint32_t os = 1;
};

std::atomic<uint64_t> g_failed_seen{0};

uint32_t level_of(const Sample& s) {
    const uint64_t swap_free = s.sys.swap_total > s.sys.swap_used ? s.sys.swap_total - s.sys.swap_used : 0;
    const bool os_warn = s.os >= 2, os_crit = s.os >= 4;
    const bool gpu_refused = s.layer.valid && s.layer.failed_allocations > g_failed_seen.load(std::memory_order_relaxed);
    const bool exhausted = os_warn && s.available_pct <= 20 && swap_free < kGiB + kGiB / 2;
    if (gpu_refused || exhausted || os_crit || s.gpu_frac >= 0.95 || (s.fp_frac >= 1.0 && (os_warn || swap_free < kGiB))) return 3;
    if (s.fp_frac >= 0.9 || s.gpu_frac >= 0.9 || (os_warn && s.fp_frac >= 0.8)) return 2;
    if (s.fp_frac >= 0.8 || s.gpu_frac >= 0.8 || os_warn) return 1;
    return 0;
}

std::string trigger_text(const Sample& s) {
    char buf[300];
    std::snprintf(buf, sizeof buf,
                  "footprint %s MiB = %.0f%% of budget %s, gpu %s MiB = %.0f%% of working set, OS %s, available %u%%, swap free %s MiB",
                  mib(s.p.footprint).c_str(), 100.0 * s.fp_frac, mib(s.budget).c_str(), mib(s.gpu.allocated).c_str(),
                  100.0 * s.gpu_frac, pressure_name(s.os), s.available_pct,
                  mib(s.sys.swap_total > s.sys.swap_used ? s.sys.swap_total - s.sys.swap_used : 0).c_str());
    return buf;
}

void fire(int idx, uint32_t level, const Sample& s, bool force = false) {
    ActionState& a = g_actions[idx];
    const double t = since_start();
    if (!force && t - a.last_s < kCooldownS) return;
    a.last_s = t;
    const uint32_t bit = 1u << idx;
    if (g_opt.mode == Mode::on && (g_opt.actions & bit) && idx <= 1 &&
        !registered(idx == 0 ? GTA6RT_GOV_LAYER_EVICT : GTA6RT_GOV_COMPILE_SHRINK) && !(idx == 0 && gta6rt_gpu_memory_release)) {
        if (!a.told_empty)
            logf("action %s (level %u): no reclaimer registered (%s has not added one); nothing done [%s]", kActionName[idx], level,
                 idx == 0 ? "the D3D12 layer" : "the pipeline compiler", trigger_text(s).c_str());
        a.told_empty = true;
        return;
    }
    if (g_opt.mode != Mode::on || !(g_opt.actions & bit)) {
        if (!a.told_empty) {
            logf("action %s (level %u): %s; not run [%s]", kActionName[idx], level,
                 g_opt.mode != Mode::on ? "observe mode" : "switched off (--governor-actions)", trigger_text(s).c_str());
            a.told_empty = true;
        }
        return;
    }
    const uint64_t goal = s.p.footprint > s.budget * 3 / 4 ? s.p.footprint - s.budget * 3 / 4 : 0;
    const uint64_t before = read_process().footprint;
    const uint64_t t0 = now_ns();
    uint64_t claimed = 0;
    std::string what;
    switch (idx) {
    case 0: {
        if (gta6rt_gpu_memory_release) {
            const uint64_t want = goal ? goal : s.layer.evictable;
            const uint64_t got = gta6rt_gpu_memory_release(want);
            claimed = got;
            what = "layer released " + mib(got) + " MiB (asked " + mib(want) + ", evictable " + mib(s.layer.evictable) + ")";
        }
        std::string more;
        claimed += run_registered(GTA6RT_GOV_LAYER_EVICT, level, goal, &more);
        if (!more.empty()) what += (what.empty() ? "" : ", ") + more;
        break;
    }
    case 1:
        claimed = run_registered(GTA6RT_GOV_COMPILE_SHRINK, level, goal, &what);
        break;
    case 2: {
        int translated = 0;
        size_t tn = sizeof translated;
        if (sysctlbyname("sysctl.proc_translated", &translated, &tn, nullptr, 0) != 0) translated = 0;
        if (translated) {
            what = "malloc pressure relief skipped under Rosetta (it returned 0 in 11 measured calls and holds the zone locks for "
                   "57-254 ms)";
        } else {
            const size_t relief = malloc_zone_pressure_relief(nullptr, 0);
            claimed = relief;
            what = "malloc pressure relief " + mib(relief) + " MiB";
        }
        std::string more;
        claimed += run_registered(GTA6RT_GOV_HOST_CACHES, level, goal, &more);
        if (!more.empty()) what += ", " + more;
        break;
    }
    case 3: {
        const bool was = hooks::reset_advice();
        hooks::set_reset_advice(hooks::installed());
        const auto& c = hooks::counters();
        what = !hooks::installed() ? std::string("title hooks not installed: MEM_RESET not advised")
               : was ? "MEM_RESET advice already on (" + mib(c.advised_bytes.load()) + " MiB advised so far)"
                     : "MEM_RESET pages from now on are advised MADV_FREE (" + mib(c.reset_bytes.load()) +
                           " MiB reset before, not touched)";
        std::string more;
        claimed = run_registered(GTA6RT_GOV_POOL_PAGES, level, goal, &more);
        if (!more.empty()) what += ", " + more;
        break;
    }
    case 4:
        gta6rt_hud_set_mem_note("MEMORY LOW");
        what = "HUD warning raised";
        break;
    }
    const double ms = double(now_ns() - t0) / 1e6;
    const uint64_t after = read_process().footprint;
    a.fired++;
    a.claimed += claimed;
    a.measured += int64_t(before) - int64_t(after);
    if (idx == 4) {
        logf("WARNING memory about to run out (level %u): %s; the title may lose its GPU device or be stopped by the OS", level,
             trigger_text(s).c_str());
        return;
    }
    logf("action %s (level %u): %s; footprint %s -> %s MiB (%+.0f) in %.1f ms [%s]", kActionName[idx], level, what.c_str(),
         mib(before).c_str(), mib(after).c_str(), -(double(int64_t(before) - int64_t(after)) / kMiB), ms, trigger_text(s).c_str());
}

void restore_all(const Sample& s) {
    std::string what;
    run_registered(GTA6RT_GOV_COMPILE_SHRINK, 0, 0, &what);
    gta6rt_hud_set_mem_note("");
    logf("level 0: all clear (%s)%s%s", trigger_text(s).c_str(), what.empty() ? "" : "; restored: ", what.c_str());
}

Sample take_sample(bool with_system_gpu) {
    Sample s;
    s.p = read_process();
    gta6rt_os_system_memory(&s.sys);
    s.gpu = gpu::sample(with_system_gpu);
    if (gta6rt_gpu_memory_report) gta6rt_gpu_memory_report(&s.layer);
    const Budget b = compute_budget(s.p.footprint);
    s.budget = b.budget;
    s.available = b.available_now;
    s.available_pct = b.physical ? uint32_t(100.0 * double(b.available_now) / double(b.physical) + 0.5) : 100;
    s.fp_frac = s.budget ? double(s.p.footprint) / double(s.budget) : 0;
    s.gpu_frac = s.gpu.working_set_limit ? double(s.gpu.allocated) / double(s.gpu.working_set_limit) : 0;
    s.os = std::max(s.sys.pressure_level, g_os_event.load(std::memory_order_relaxed));
    return s;
}

std::atomic<int64_t> g_last_swapins{0};
std::atomic<uint64_t> g_last_pageins{0};
std::atomic<double> g_last_line_t{0};

void line(const Sample& s, const Categories& c, const char* why) {
    const auto& h = hooks::counters();
    const double t = since_start();
    const double dt = std::max(1e-3, t - g_last_line_t.exchange(t));
    const int64_t swapins = s.p.swapins - g_last_swapins.exchange(s.p.swapins);
    const uint64_t pageins = s.sys.pageins - g_last_pageins.exchange(s.sys.pageins);
    logf("memory +%.0fs (%s): footprint %s MiB (budget %s, %.0f%%), resident %s, compressed %s, swapped in %.0f MiB since "
         "last; pool %s committed, %s touched (%s resident + %s compressed), %s MiB live pages, %s reserved; gpu metal %s of "
         "%s working set, graphics ledger %s (+%s compressed, %s not charged), all processes %s allocated %s in use; "
         "layer vk %s in %" PRIu64 " (title copies %s, render targets %s, staging %s, shadows %s, descriptors %s, buffers %s, "
         "textures %s, present %s, other %s; imported %s, evictable %s, failed allocations %" PRIu64 "); "
         "malloc %s in use of %s; released but held %s; title gave back: MEM_RESET %s MiB in %" PRIu64 " (advised %s), "
         "decommit %s MiB in %" PRIu64 ", %" PRIu64 " pool pages freed (%" PRIu64 " alloc, %" PRIu64 " free calls seen); machine: pressure %s, available %u%%, swap %s of %s, "
         "free+speculative %s, wired %s, file cache %s (%.0f pageins/s), compressor %s holding %s, load %.1f; level %u; "
         "walk %.0f ms; governor cpu %.0f ms (poll mean %.0f us, max %.1f ms)",
         t, why, mib(s.p.footprint).c_str(), mib(s.budget).c_str(), 100.0 * s.fp_frac, mib(s.p.resident).c_str(),
         mib(s.p.compressed).c_str(), double(swapins) / kMiB, mib(c.pool_committed).c_str(),
         mib(c.pool_resident + c.pool_compressed).c_str(), mib(c.pool_resident).c_str(), mib(c.pool_compressed).c_str(),
         mib(c.pool_live * rt::xmem::kPhysicalPageSize).c_str(), mib(c.pool_reserved).c_str(), mib(s.gpu.allocated).c_str(),
         mib(s.gpu.working_set_limit).c_str(), mib(s.p.graphics).c_str(), mib(s.p.graphics_compressed).c_str(),
         mib(s.p.graphics_nofootprint).c_str(), mib(s.gpu.system_alloc).c_str(), mib(s.gpu.system_in_use).c_str(),
         mib(s.layer.driver_total).c_str(), s.layer.driver_allocations, mib(s.layer.title_copies).c_str(),
         mib(s.layer.render_targets).c_str(), mib(s.layer.staging).c_str(), mib(s.layer.title_shadows).c_str(),
         mib(s.layer.descriptors).c_str(), mib(s.layer.buffers).c_str(), mib(s.layer.textures).c_str(),
         mib(s.layer.present).c_str(), mib(s.layer.other).c_str(), mib(s.layer.imported).c_str(),
         mib(s.layer.evictable).c_str(), s.layer.failed_allocations, mib(c.malloc_in_use).c_str(), mib(c.malloc_allocated).c_str(), mib(c.released_touched).c_str(),
         mib(h.reset_bytes.load()).c_str(), h.reset_calls.load(), mib(h.advised_bytes.load()).c_str(),
         mib(h.decommit_bytes.load()).c_str(), h.decommit_calls.load(), h.freed_pages.load(), h.alloc_calls.load(),
         h.free_calls.load(), pressure_name(s.os),
         memorystatus_level(), mib(s.sys.swap_used).c_str(), mib(s.sys.swap_total).c_str(),
         mib(s.sys.free_bytes + s.sys.speculative_bytes).c_str(), mib(s.sys.wired_bytes).c_str(),
         mib(s.sys.file_backed_bytes).c_str(), double(pageins) / dt, mib(s.sys.compressor_bytes).c_str(),
         mib(s.sys.compressed_bytes).c_str(), s.sys.load1, g_level.load(), c.walk_ms,
         double(g_poll_thread_cpu_ns.load() + g_report_thread_cpu_ns.load()) / 1e6,
         double(g_poll_ns_total.load()) / double(std::max<uint64_t>(1, g_polls.load())) / 1e3, double(g_poll_ns_max.load()) / 1e6);
}

void table(const Sample& s, const Categories& c, const std::string& stage) {
    const double t = since_start();
    const uint64_t attributed = c.pool_resident + c.pool_compressed + c.priv_resident + c.priv_compressed + c.img_resident +
                                c.img_compressed + c.view_resident + c.view_compressed + s.p.graphics + s.p.graphics_compressed;
    logf("table +%.0fs at %s: footprint %s MiB, resident %s, compressed %s; budget %s; machine pressure %s, swap %s of %s MiB, "
         "free+speculative %s, wired %s, load %.1f",
         t, stage.c_str(), mib(s.p.footprint).c_str(), mib(s.p.resident).c_str(), mib(s.p.compressed).c_str(),
         mib(s.budget).c_str(), pressure_name(s.os), mib(s.sys.swap_used).c_str(), mib(s.sys.swap_total).c_str(),
         mib(s.sys.free_bytes + s.sys.speculative_bytes).c_str(), mib(s.sys.wired_bytes).c_str(), s.sys.load1);
    logf("table   %-34s %10s %10s %10s %10s", "category (MiB)", "reserved", "committed", "resident", "compressed");
    auto row = [&](const char* name, uint64_t reserved, uint64_t committed, uint64_t resident, uint64_t compressed) {
        logf("table   %-34s %10s %10s %10s %10s", name, reserved ? mib(reserved).c_str() : "-", committed ? mib(committed).c_str() : "-",
             mib(resident).c_str(), mib(compressed).c_str());
    };
    row("title pool (xmem physical pages)", c.pool_reserved, c.pool_committed, c.pool_resident, c.pool_compressed);
    row("title windows (aliases of the pool)", c.win_reserved, c.win_committed, c.win_resident, c.win_compressed);
    row("title + host vm table, other", c.priv_reserved, c.priv_committed, c.priv_resident, c.priv_compressed);
    row("images (exe, package DLLs)", 0, c.img_committed, c.img_resident, c.img_compressed);
    row("file views", 0, c.view_committed, c.view_resident, c.view_compressed);
    row("GPU charged (graphics ledger)", 0, 0, s.p.graphics, s.p.graphics_compressed);
    logf("table   %-34s %10s %10s %10s %10s", "GPU Metal allocated (all kinds)", "-", mib(s.gpu.allocated).c_str(), "-", "-");
    if (s.layer.valid) {
        const struct {
            const char* name;
            uint64_t bytes;
        } layer_rows[] = {{"  layer: title copies", s.layer.title_copies}, {"  layer: render targets", s.layer.render_targets},
                          {"  layer: staging", s.layer.staging},           {"  layer: title shadows", s.layer.title_shadows},
                          {"  layer: descriptors/arena", s.layer.descriptors}, {"  layer: buffers", s.layer.buffers},
                          {"  layer: textures", s.layer.textures},         {"  layer: present", s.layer.present},
                          {"  layer: other", s.layer.other},               {"  layer: imported title pages", s.layer.imported}};
        for (const auto& r : layer_rows)
            logf("table   %-34s %10s %10s %10s %10s", r.name, "-", mib(r.bytes).c_str(), "-", "-");
        logf("table   layer vk memory %s MiB in %" PRIu64 " allocations, evictable %s MiB, %" PRIu64 " failed allocations, %" PRIu64
             " pipelines, heap budget %s of %s MiB",
             mib(s.layer.driver_total).c_str(), s.layer.driver_allocations, mib(s.layer.evictable).c_str(),
             s.layer.failed_allocations, s.layer.pipelines, mib(s.layer.heap_usage).c_str(), mib(s.layer.heap_budget).c_str());
    } else {
        logf("table   layer: no GPU memory report yet (%s)", gta6rt_gpu_memory_report ? "no device" : "hook not in this build");
    }
    logf("table   %-34s %10s %10s %10s %10s", "malloc (in use / allocated)", "-", mib(c.malloc_allocated).c_str(),
         mib(c.malloc_in_use).c_str(), "-");
    logf("table   %-34s %10s %10s %10s %10s", "footprint not attributed above", "-", "-",
         mib(s.p.footprint > attributed ? s.p.footprint - attributed : 0).c_str(), "-");
    logf("table   pool: %s live pages (%s MiB), %s slots used, %s on the free list (decommitted); released but held %s MiB",
         std::to_string(c.pool_live).c_str(), mib(c.pool_live * rt::xmem::kPhysicalPageSize).c_str(),
         std::to_string(c.pool_slots_used).c_str(), std::to_string(c.pool_free_slots).c_str(), mib(c.released_touched).c_str());
}

void request_table(const std::string& why) {
    {
        std::lock_guard lock(g_report_mu);
        g_table_requests.push_back(why);
    }
    g_report_cv.notify_one();
}

void report_thread() {
    pthread_setname_np("gta6rt-governor-report");
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    const unsigned every = g_opt.report_s ? g_opt.report_s : 30;
    auto next = std::chrono::steady_clock::now() + std::chrono::seconds(every);
    for (;;) {
        std::vector<std::string> tables;
        bool periodic = false;
        {
            std::unique_lock lock(g_report_mu);
            g_report_cv.wait_until(lock, next, [] { return !g_table_requests.empty(); });
            tables.swap(g_table_requests);
            if (std::chrono::steady_clock::now() >= next) {
                periodic = true;
                next += std::chrono::seconds(every);
            }
        }
        const bool log_os = g_opt.report_s != 0;
        if (periodic) {
            const Sample s = take_sample(true);
            const Categories c = walk(log_os ? "periodic" : "governor");
            line(s, c, "periodic");
        }
        for (const std::string& why : tables) {
            const Sample s = take_sample(true);
            const Categories c = walk(why.c_str());
            line(s, c, why.c_str());
            table(s, c, why);
        }
        g_report_thread_cpu_ns.store(thread_cpu_ns(), std::memory_order_relaxed);
    }
}

void poll_thread() {
    pthread_setname_np("gta6rt-governor");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    gpu::init();
    bool loading_seen = false;
    std::string stage;
    uint32_t prev = 0;
    bool critical_logged = false;
    double last_warn_s = -1e9;
    uint64_t n = 0;
    for (;;) {
        const uint64_t t0 = now_ns();
        const Sample s = take_sample(++n % 4 == 0);
        const uint32_t level = level_of(s);
        g_level.store(level, std::memory_order_relaxed);
        gta6rt_hud_publish_memory(s.budget, s.gpu.allocated, s.gpu.working_set_limit, level, level >= 3);
        if (level != prev)
            logf("level %u -> %u: %s", prev, level, trigger_text(s).c_str());
        if (s.layer.valid && s.layer.failed_allocations > g_failed_seen.load(std::memory_order_relaxed)) {
            logf("WARNING the GPU refused %" PRIu64 " allocation(s) (layer count, %" PRIu64 " so far): %s; layer vk %s MiB, Metal "
                 "allocated %s MiB, all processes' GPU memory %s MiB allocated",
                 s.layer.failed_allocations - g_failed_seen.load(), s.layer.failed_allocations, trigger_text(s).c_str(),
                 mib(s.layer.driver_total).c_str(), mib(s.gpu.allocated).c_str(), mib(s.gpu.system_alloc).c_str());
            g_failed_seen.store(s.layer.failed_allocations, std::memory_order_relaxed);
            request_table("GPU allocation refused");
        }
        if (level >= 1) {
            fire(0, level, s);
            fire(2, level, s);
        }
        if (level >= 2) {
            fire(1, level, s);
            fire(3, level, s);
            if (!critical_logged) {
                request_table("level " + std::to_string(level));
                critical_logged = true;
            }
        }
        if (level >= 3 && since_start() - last_warn_s >= 30.0) {
            fire(4, level, s, true);
            last_warn_s = since_start();
        }
        if (level == 0 && prev >= 1) restore_all(s);
        prev = level;
        if (g_exe_base) {
            const std::string now = stage_of(g_exe_base, &loading_seen);
            if (now != stage) {
                stage = now;
                request_table("stage " + now);
            }
        }
        const uint64_t dt = now_ns() - t0;
        g_poll_ns_total.fetch_add(dt, std::memory_order_relaxed);
        g_polls.fetch_add(1, std::memory_order_relaxed);
        raise_max(g_poll_ns_max, dt);
        g_poll_thread_cpu_ns.store(thread_cpu_ns(), std::memory_order_relaxed);
        std::unique_lock lock(g_poll_mu);
        g_poll_cv.wait_for(lock, std::chrono::milliseconds(g_opt.poll_ms),
                             [] { return g_os_event.load(std::memory_order_relaxed) & 0x100; });
        g_os_event.fetch_and(~0x100u, std::memory_order_relaxed);
    }
}

dispatch_source_t g_pressure_source = nullptr;

void start_pressure_source() {
    dispatch_queue_t q = dispatch_queue_create("gta6rt.governor.pressure", DISPATCH_QUEUE_SERIAL);
    g_pressure_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                                               DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
                                                   DISPATCH_MEMORYPRESSURE_CRITICAL,
                                               q);
    if (!g_pressure_source) {
        logf("memory-pressure dispatch source: not created; polling only");
        return;
    }
    dispatch_source_set_event_handler(g_pressure_source, ^{
        const unsigned long d = dispatch_source_get_data(g_pressure_source);
        const uint32_t level = (d & DISPATCH_MEMORYPRESSURE_CRITICAL) ? 4 : (d & DISPATCH_MEMORYPRESSURE_WARN) ? 2 : 1;
        g_os_event.store(level | 0x100, std::memory_order_relaxed);
        std::fprintf(stderr, "[gov] OS memory pressure event: %s at +%.1fs\n", pressure_name(level), since_start());
        g_poll_cv.notify_all();
    });
    dispatch_resume(g_pressure_source);
}

}  // namespace

bool parse_mode(const char* text, Mode* out) {
    if (!text) return false;
    if (!std::strcmp(text, "on")) *out = Mode::on;
    else if (!std::strcmp(text, "observe")) *out = Mode::observe;
    else if (!std::strcmp(text, "off")) *out = Mode::off;
    else return false;
    return true;
}

bool parse_actions(const char* text, uint32_t* out) {
    if (!text) return false;
    if (!std::strcmp(text, "all")) {
        *out = kActAll;
        return true;
    }
    if (!std::strcmp(text, "none")) {
        *out = 0;
        return true;
    }
    uint32_t m = 0;
    for (const char* p = text; *p; ++p) {
        if (*p == ',') continue;
        if (*p < 'a' || *p > 'e') return false;
        m |= 1u << (*p - 'a');
    }
    *out = m;
    return true;
}

bool parse_size(const char* text, uint64_t* out) {
    if (!text || !*text) return false;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || v <= 0) return false;
    double mul = 1;
    if (*end == 'G' || *end == 'g') mul = double(kGiB), ++end;
    else if (*end == 'M' || *end == 'm') mul = kMiB, ++end;
    if (*end == 'i' || *end == 'B' || *end == 'b') ++end;
    if (*end == 'B') ++end;
    if (*end) return false;
    *out = uint64_t(v * mul);
    return *out >= 512ull << 20;
}

void prepare(const Options& opt) {
    g_opt = opt;
    g_t0 = std::chrono::steady_clock::now();
    const char* medium = getenv("GTA6RT_MALLOC_MEDIUM");
    if (!(medium && !std::strcmp(medium, "on"))) setenv("MallocMediumZone", "0", 0);
    if (opt.mode == Mode::off) return;
    if (opt.report_s) setenv("GTA6RT_MEMORY_REPORT_OWNER", "host", 1);
}

Budget compute_budget(uint64_t own_footprint) {
    Budget b;
    size_t n = sizeof b.physical;
    sysctlbyname("hw.memsize", &b.physical, &n, nullptr, 0);
    b.os_reserve = std::clamp<uint64_t>(b.physical / 4, 4 * kGiB, 8 * kGiB);
    b.static_ceiling = b.physical > b.os_reserve ? b.physical - b.os_reserve : b.physical / 2;
    b.available_now = uint64_t(double(memorystatus_level()) / 100.0 * double(b.physical));
    const uint64_t keep = b.physical * 15 / 100;
    const uint64_t dyn = own_footprint + b.available_now;
    b.dynamic_ceiling = dyn > keep ? dyn - keep : 0;
    b.title_s = kTitleS;
    b.title_x = kTitleX;
    b.host_add = kHostAdd;
    uint64_t fixed = 0;
    if (g_opt.budget != "auto" && parse_size(g_opt.budget.c_str(), &fixed)) {
        b.fixed = true;
        b.budget = fixed;
        return b;
    }
    b.budget = std::max<uint64_t>(4 * kGiB, std::min(b.static_ceiling, b.dynamic_ceiling));
    return b;
}

const char* profile_for_budget(const Budget& b, std::string* why) {
    const bool x = b.budget >= b.title_x + b.host_add;
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "budget %.1f GiB (%s; physical %.0f GiB less an OS reserve of %.1f GiB, and at most own footprint + available now %.1f GiB "
                  "less 15%% of physical); Series X needs %.1f GiB "
                  "(13.5 title + %.1f host), Series S %.1f GiB",
                  double(b.budget) / kGiB, b.fixed ? "--memory-budget" : "auto", double(b.physical) / kGiB,
                  double(b.os_reserve) / kGiB, double(b.available_now) / kGiB, double(b.title_x + b.host_add) / kGiB,
                  double(b.host_add) / kGiB, double(b.title_s + b.host_add) / kGiB);
    if (why) *why = buf;
    return x ? "series-x" : "series-s";
}

void install_title_hooks() {
    if (g_opt.mode == Mode::off) return;
    const unsigned n = hooks::install();
    logf("title hooks: %u of 4 wrapped (VirtualAlloc, XMemVirtualAlloc, VirtualFree, XMemFreePhysicalPages): MEM_RESET, "
         "MEM_DECOMMIT and freed pool pages are counted%s",
         n, n ? "" : " (none: nothing is counted)");
}

void start(uint64_t exe_base) {
    if (g_opt.mode == Mode::off) {
        logf("off: no accounting thread, no actions (os/ keeps its own memory report)");
        return;
    }
    if (g_started.exchange(true)) return;
    g_exe_base = exe_base;
    g_start_budget = compute_budget(read_process().footprint);
    gta6rt_system_memory sys{};
    gta6rt_os_system_memory(&sys);
    g_last_pageins.store(sys.pageins);
    g_last_swapins.store(read_process().swapins);
    g_last_line_t.store(since_start());
    std::string why;
    profile_for_budget(g_start_budget, &why);
    char acts[8] = {};
    int k = 0;
    for (int i = 0; i < 5; ++i)
        if (g_opt.actions & (1u << i)) acts[k++] = char('a' + i);
    const char* medium = getenv("MallocMediumZone");
    logf("malloc medium zone %s", medium && !std::strcmp(medium, "0")
                                      ? "off (MallocMediumZone=0 from process start: freed 32 KiB-8 MiB blocks go back to the OS; "
                                        "GTA6RT_MALLOC_MEDIUM=on keeps it)"
                                      : "on (GTA6RT_MALLOC_MEDIUM=on, or the process was not re-executed after the setting)");
    logf("%s: poll %u ms, report every %u s%s, actions %s; %s", g_opt.mode == Mode::on ? "on" : "observe (no actions)",
         g_opt.poll_ms, g_opt.report_s ? g_opt.report_s : 30, g_opt.report_s ? " (owns --memory-report)" : "",
         k ? acts : "none", why.c_str());
    start_pressure_source();
    std::thread(poll_thread).detach();
    std::thread(report_thread).detach();
}

void finish(const char* why) {
    if (!g_started.load()) return;
    const Sample s = take_sample(true);
    const Categories c = walk(why);
    line(s, c, why);
    table(s, c, why);
    summary();
}

void summary() {
    if (!g_started.load()) return;
    for (int i = 0; i < 5; ++i)
        if (g_actions[i].fired)
            logf("action %s: fired %" PRIu64 " times, reclaimers claimed %s MiB, footprint change right after %+.0f MiB", kActionName[i],
                 g_actions[i].fired, mib(g_actions[i].claimed).c_str(), -double(g_actions[i].measured) / kMiB);
    const double t = since_start();
    const double polls = double(std::max<uint64_t>(1, g_polls.load()));
    const double walks = double(std::max<uint64_t>(1, g_walks.load()));
    const double cpu_ms = double(g_poll_thread_cpu_ns.load() + g_report_thread_cpu_ns.load()) / 1e6;
    logf("overhead over %.0f s: poll thread %.0f ms cpu (%" PRIu64 " polls, mean %.0f us, max %.1f ms), report thread %.0f ms cpu "
         "(%" PRIu64 " walks, mean %.0f ms, max %.0f ms); together %.3f%% of one core",
         t, double(g_poll_thread_cpu_ns.load()) / 1e6, g_polls.load(), double(g_poll_ns_total.load()) / polls / 1e3,
         double(g_poll_ns_max.load()) / 1e6, double(g_report_thread_cpu_ns.load()) / 1e6, g_walks.load(),
         double(g_walk_ns_total.load()) / walks / 1e6, double(g_walk_ns_max.load()) / 1e6, 100.0 * cpu_ms / 1e3 / std::max(1.0, t));
}

}  // namespace host::governor

extern "C" int gta6rt_governor_register(gta6rt_governor_action action, const char* name, gta6rt_governor_reclaim_fn fn,
                                        void* ctx) {
    if (action < 0 || action >= GTA6RT_GOV_ACTION_COUNT || !fn) return -1;
    std::lock_guard lock(host::governor::g_reg_mu);
    auto& list = host::governor::g_reclaimers[action];
    if (list.size() >= 8) return -1;
    list.push_back({name ? name : "?", fn, ctx});
    std::fprintf(stderr, "[gov] reclaimer registered for action %d: %s\n", int(action), name ? name : "?");
    return 0;
}

extern "C" uint32_t gta6rt_governor_level(void) { return host::governor::g_level.load(std::memory_order_relaxed); }
