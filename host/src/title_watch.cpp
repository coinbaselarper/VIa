#include "title_watch.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

namespace host::title {
namespace {

// Retail 1.0.2.1 facts from leonida (runtime/src/main.cpp, tools/stage_timeline.py), checked against our exe's bytes:
// the hang watchdog thread (RVA 0x2215a60) ends the process with `movl $0, 0` at 0x2215c35 when the main loop has
// not set its kick byte (0x71b6331) for 30 one-second ticks.
constexpr uint32_t kWatchdogTrapRva = 0x2215c35;
constexpr uint32_t kWatchdogKickRva = 0x71b6331;
constexpr uint8_t kWatchdogTrap[] = {0xC7, 0x04, 0x25, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr uint32_t kGameStateRva = 0x7446510;
constexpr uint32_t kSessionRva = 0x98f828c;
constexpr uint32_t kLoadingRva = 0x7453ed0;
constexpr unsigned stage_period_ms = 500;

bool read_u64(uint64_t at, uint64_t* out) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), at, sizeof *out, reinterpret_cast<mach_vm_address_t>(out), &got) ==
               KERN_SUCCESS &&
           got == sizeof *out;
}

bool page_writable(uint64_t at) {
    mach_vm_address_t addr = at;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info),
                       &count, &obj) != KERN_SUCCESS)
        return false;
    return addr <= at && (info.protection & VM_PROT_WRITE);
}

unsigned thread_count() {
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return 0;
    for (mach_msg_type_number_t i = 0; i < n; ++i) mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), n * sizeof(thread_act_t));
    return n;
}

struct Item {
    uint64_t rva = 0;
    bool deref = false;
    uint64_t offset = 0;
    unsigned count = 1;
};

std::vector<Item> parse_spec(const std::string& spec) {
    std::vector<Item> out;
    const char* at = spec.c_str();
    while (*at) {
        char* end = nullptr;
        Item it;
        it.rva = std::strtoull(at, &end, 16);
        if (end == at) break;
        if (*end == '>') {
            it.deref = true;
            it.offset = std::strtoull(end + 1, &end, 16);
        }
        if (*end == ':') it.count = static_cast<unsigned>(std::strtoul(end + 1, &end, 10));
        if (it.count > 32) it.count = 32;
        out.push_back(it);
        if (*end != ',') break;
        at = end + 1;
    }
    return out;
}

const char* session_name(uint32_t s) {
    switch (s) {
    case 1: return "boot screens";
    case 3: return "landing";
    case 0xd: return "first-run pages";
    case 0: return "story load";
    default: return nullptr;
    }
}

std::string stage_of(uint64_t base, bool* loading_seen) {
    uint64_t game = 0, session = 0, load = 0;
    const bool g = read_u64(base + kGameStateRva, &game), s = read_u64(base + kSessionRva, &session),
               l = read_u64(base + kLoadingRva, &load);
    if (!g) return "no title state";
    char buf[96];
    if ((game & 0xffffffff) != 3) {
        std::snprintf(buf, sizeof buf, "game state %#" PRIx64, game & 0xffffffff);
        return buf;
    }
    if (!s) return "running";
    const uint32_t sess = static_cast<uint32_t>(session);
    if (l && (load & 0xff) == 2) *loading_seen = true;
    if (sess == 0 && l && *loading_seen && (load & 0xff) == 0) return "world (loading screen ended)";
    if (const char* n = session_name(sess)) {
        if (sess == 0 && l) {
            std::snprintf(buf, sizeof buf, "story load (loading byte %#x)", static_cast<unsigned>(load & 0xff));
            return buf;
        }
        return n;
    }
    std::snprintf(buf, sizeof buf, "session %#x", sess);
    return buf;
}

std::atomic<uint64_t> g_null_pso_skips{0};
std::atomic<uint64_t> g_fsr_skips{0};

void* exec_page(const std::vector<uint8_t>& code) {
    void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) return nullptr;
    std::memcpy(page, code.data(), code.size());
    if (mprotect(page, 4096, PROT_READ | PROT_EXEC) != 0) return nullptr;
    return page;
}

void emit64(std::vector<uint8_t>& c, uint64_t v) {
    for (int i = 0; i < 8; ++i) c.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

}  // namespace

uint64_t null_pso_skips() { return g_null_pso_skips.load(std::memory_order_relaxed); }
uint64_t fsr_skips() { return g_fsr_skips.load(std::memory_order_relaxed); }

// amd_fidelityfx_upscaler_xbox.dll+0x65ff: the FSR4 dispatch reads the pipeline it just tried to create. The package's
// FSPX program 03 has no translation (neither the port's translator nor leonida's: "fall-through into undecodable
// words"), so slots 67 and 62 refuse it, the DLL keeps a null pipeline and reads address 0x10 (s3r4-e). The patch
// returns from that dispatch as the DLL's own end at +0x6831 does (S_OK, nothing recorded), after restoring the four
// registers the function reloads from its frame on the way there; counted as fsr_skips.
bool patch_fsr_null_pipeline(uint64_t dll_base, uint8_t out_patch[13], uint32_t* out_rva) {
    static const uint8_t kSite[] = {0xc5, 0xfa, 0x6f, 0x47, 0x10, 0x4c, 0x8b, 0xa4, 0x24, 0x50, 0x01, 0x00, 0x00};
    static const uint8_t kRsi[] = {0x48, 0x8b, 0xb4, 0x24, 0x60, 0x01, 0x00, 0x00};
    static const uint8_t kRdi[] = {0x48, 0x8b, 0xbc, 0x24, 0x58, 0x01, 0x00, 0x00};
    static const uint8_t kRbx[] = {0x48, 0x8b, 0x9c, 0x24, 0x68, 0x01, 0x00, 0x00};
    static const uint8_t kEnd[] = {0x33, 0xc0};
    auto at = [&](uint32_t rva, const uint8_t* b, size_t n) { return !std::memcmp(reinterpret_cast<const void*>(dll_base + rva), b, n); };
    if (!at(0x65ff, kSite, sizeof kSite) || !at(0x660c, kRsi, sizeof kRsi) || !at(0x6754, kRdi, sizeof kRdi) ||
        !at(0x675c, kRbx, sizeof kRbx) || !at(0x6831, kEnd, sizeof kEnd)) {
        std::fprintf(stderr, "[host] FSR null-pipeline patch: bytes differ from package 1.0.2.1; not patched\n");
        return false;
    }
    std::vector<uint8_t> c = {0x48, 0x85, 0xff, 0x75, 0x00};
    const size_t jnz = c.size() - 1;
    c.insert(c.end(), {0x4c, 0x8b, 0xa4, 0x24, 0x50, 0x01, 0x00, 0x00});
    c.insert(c.end(), kRsi, kRsi + sizeof kRsi);
    c.insert(c.end(), kRdi, kRdi + sizeof kRdi);
    c.insert(c.end(), kRbx, kRbx + sizeof kRbx);
    c.insert(c.end(), {0x48, 0xb8});
    emit64(c, reinterpret_cast<uint64_t>(&g_fsr_skips));
    c.insert(c.end(), {0xf0, 0x48, 0xff, 0x00, 0x48, 0xb8});
    emit64(c, dll_base + 0x6831);
    c.insert(c.end(), {0xff, 0xe0});
    c[jnz] = static_cast<uint8_t>(c.size() - (jnz + 1));
    c.insert(c.end(), kSite, kSite + sizeof kSite);
    c.insert(c.end(), {0x48, 0xb8});
    emit64(c, dll_base + 0x660c);
    c.insert(c.end(), {0xff, 0xe0});
    void* page = exec_page(c);
    if (!page) return false;
    out_patch[0] = 0x48;
    out_patch[1] = 0xb8;
    const uint64_t tramp = reinterpret_cast<uint64_t>(page);
    std::memcpy(out_patch + 2, &tramp, 8);
    out_patch[10] = 0xff;
    out_patch[11] = 0xe0;
    out_patch[12] = 0x90;
    *out_rva = 0x65ff;
    return true;
}

// exe+0x248c2a0 (a draw-state flush on the job threads) binds the PSO pointer at [state+0x120]: it copies 32 bytes from
// the PSO into the console list's pipeline record and calls list slot 25 (SetPipelineState). A PSO whose creation the
// D3D12 layer refused is a null pointer there and the title reads address 0x10 (s3r4-d, 10 s into the world phase).
// The patch jumps from exe+0x248c306 to a trampoline that leaves the previous pipeline bound when the pointer is null
// (counted), as the title's own "same PSO" path at exe+0x248c35d does.
bool patch_null_pso_bind(uint64_t base, uint8_t out_patch[12], uint32_t* out_rva) {
    static const uint8_t kLoad[] = {0x49, 0x8b, 0x95, 0x20, 0x01, 0x00, 0x00};
    static const uint8_t kSite[] = {0x48, 0x89, 0x96, 0x48, 0x21, 0x03, 0x00, 0xc5, 0xf8, 0x28, 0x42, 0x10};
    constexpr uint32_t kLoadRva = 0x248c2d9, kSiteRva = 0x248c306, kResumeRva = 0x248c312, kSkipRva = 0x248c35d;
    if (std::memcmp(reinterpret_cast<const void*>(base + kLoadRva), kLoad, sizeof kLoad) ||
        std::memcmp(reinterpret_cast<const void*>(base + kSiteRva), kSite, sizeof kSite)) {
        std::fprintf(stderr, "[host] null-PSO bind patch: bytes at RVA %#x differ from 1.0.2.1; not patched\n", kSiteRva);
        return false;
    }
    void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) return false;
    uint8_t* t = static_cast<uint8_t*>(page);
    size_t n = 0;
    auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) t[n++] = x; };
    auto put64 = [&](uint64_t v) { std::memcpy(t + n, &v, 8); n += 8; };
    put({0x48, 0x85, 0xd2, 0x74, 0x18});
    put({0x48, 0x89, 0x96, 0x48, 0x21, 0x03, 0x00, 0xc5, 0xf8, 0x28, 0x42, 0x10});
    put({0x48, 0xb8}); put64(base + kResumeRva); put({0xff, 0xe0});
    put({0x48, 0xb8}); put64(reinterpret_cast<uint64_t>(&g_null_pso_skips));
    put({0xf0, 0x48, 0xff, 0x00});
    put({0x48, 0xb8}); put64(base + kSkipRva); put({0xff, 0xe0});
    if (mprotect(page, 4096, PROT_READ | PROT_EXEC) != 0) return false;
    out_patch[0] = 0x48;
    out_patch[1] = 0xb8;
    const uint64_t tramp = reinterpret_cast<uint64_t>(page);
    std::memcpy(out_patch + 2, &tramp, 8);
    out_patch[10] = 0xff;
    out_patch[11] = 0xe0;
    *out_rva = kSiteRva;
    return true;
}

void start_peek(uint64_t base, const std::string& spec, unsigned period_s, bool stages) {
    if (!period_s) period_s = 10;
    const std::vector<Item> items = parse_spec(spec);
    std::thread([base, items, period_s, stages] {
        const auto t0 = std::chrono::steady_clock::now();
        std::string last_stage;
        bool loading_seen = false;
        unsigned max_threads = 0;
        for (uint64_t tick = 0;; ++tick) {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const std::string stage = stage_of(base, &loading_seen);
            const unsigned threads = thread_count();
            if (threads > max_threads) max_threads = threads;
            if (stages && stage != last_stage) {
                std::fprintf(stderr, "[host] stage t=%.1fs: %s (threads %u)\n", t, stage.c_str(), threads);
                last_stage = stage;
            }
            const uint64_t every = (period_s * 1000ull) / stage_period_ms;
            if (!items.empty() && every && tick % every == 0) {
                std::string line = "[rt] peek t=" + std::to_string(static_cast<unsigned>(t)) + "s:";
                char text[64];
                for (const Item& it : items) {
                    if (it.deref)
                        std::snprintf(text, sizeof text, " 0x%" PRIx64 ">%" PRIx64 "=", it.rva, it.offset);
                    else
                        std::snprintf(text, sizeof text, " 0x%" PRIx64 "=", it.rva);
                    line += text;
                    uint64_t from = base + it.rva;
                    unsigned count = it.count;
                    if (it.deref) {
                        uint64_t p = 0;
                        if (!read_u64(from, &p) || !p) {
                            line += "?";
                            count = 0;
                        }
                        from = p + it.offset;
                    }
                    for (unsigned i = 0; i < count; ++i) {
                        uint64_t v = 0;
                        const bool ok = read_u64(from + 8ull * i, &v);
                        std::snprintf(text, sizeof text, ok ? "%s%" PRIx64 : "%s?", i ? "," : "", v);
                        line += text;
                    }
                }
                std::snprintf(text, sizeof text, " threads=%u max=%u null_pso_skips=%llu fsr_skips=%llu", threads, max_threads,
                              static_cast<unsigned long long>(g_null_pso_skips.load(std::memory_order_relaxed)),
                              static_cast<unsigned long long>(g_fsr_skips.load(std::memory_order_relaxed)));
                line += text;
                std::fprintf(stderr, "%s\n", line.c_str());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(stage_period_ms));
        }
    }).detach();
}

bool start_watchdog_kick(uint64_t base) {
    uint8_t trap[sizeof kWatchdogTrap];
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), base + kWatchdogTrapRva, sizeof trap, reinterpret_cast<mach_vm_address_t>(trap),
                               &got) != KERN_SUCCESS ||
        got != sizeof trap || std::memcmp(trap, kWatchdogTrap, sizeof trap) != 0) {
        std::fprintf(stderr, "[host] title watchdog: trap bytes at RVA %#x differ from 1.0.2.1; left armed\n", kWatchdogTrapRva);
        return false;
    }
    if (!page_writable(base + kWatchdogKickRva)) {
        std::fprintf(stderr, "[host] title watchdog: kick byte RVA %#x is not writable; left armed\n", kWatchdogKickRva);
        return false;
    }
    auto* kick = reinterpret_cast<volatile uint8_t*>(base + kWatchdogKickRva);
    std::thread([kick] {
        for (;;) {
            *kick = 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
    std::fprintf(stderr, "[host] title watchdog: kick byte RVA %#x set every 500 ms (--title-watchdog off)\n", kWatchdogKickRva);
    return true;
}

}  // namespace host::title
