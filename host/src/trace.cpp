#include "trace.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gta6rt/isa.h>
#include <gta6rt/loader.h>
#include <gta6rt/os.h>

namespace host::trace {
namespace {

struct Rec {
    gta6rt_gva target;
    const char* name;
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> calls_exe{0};
    uint64_t first_seq = 0;
    uint64_t stub = 0;
    bool unimplemented = false;
};
static_assert(offsetof(Rec, target) == 0, "the trampoline jumps through [r10]");

struct Hit {
    uint64_t seq;
    Rec* rec;
    uint64_t ret;
    uint64_t a[4];
    uint32_t tid;
};

constexpr size_t kRing = 4096;
constexpr uint32_t kMaxOrder = 16384;
constexpr size_t kStubSize = 24;
constexpr size_t kStubArena = 2u << 20;

Hit g_ring[kRing];
std::atomic<uint64_t> g_seq{0};
std::atomic<uint64_t> g_exe_calls{0};
Rec* g_order[kMaxOrder];
std::atomic<uint32_t> g_order_n{0};
std::atomic<uint64_t> g_match_logged{0};

Options g_opt;
std::vector<std::string> g_match;
gta6rt_gva g_exe_base = 0;
uint64_t g_exe_size = 0;

std::mutex g_mu;
std::map<std::pair<uint64_t, std::string>, Rec*> g_by_key;
std::map<uint64_t, Rec*> g_by_stub;
std::vector<Rec*> g_all;
std::vector<std::string> g_unimpl_bindings;
std::vector<std::string> g_missing_procs;
uint8_t* g_arena = nullptr;
size_t g_arena_used = 0;
#if !defined(__x86_64__)
uint64_t g_tramp = 0;
#endif

uint32_t current_tid() {
    static thread_local uint32_t tid = 0;
    if (!tid) {
        if (gta6rt_thread* t = gta6rt_thread_current()) tid = gta6rt_thread_id(t);
        if (!tid) {
            uint64_t id = 0;
            pthread_threadid_np(nullptr, &id);
            tid = static_cast<uint32_t>(id) | 0x80000000u;
        }
    }
    return tid;
}

void where(uint64_t addr, char* buf, size_t n) {
    if (g_exe_size && addr - g_exe_base < g_exe_size) {
        snprintf(buf, n, "exe+0x%" PRIx64, addr - g_exe_base);
        return;
    }
    gta6rt_loader_describe_address(addr, buf, n);
}

bool ends_with(const char* s, const char* tail) {
    const size_t n = strlen(s), m = strlen(tail);
    return n >= m && !strcmp(s + n - m, tail);
}

// The path argument of the calls that take one first, for --trace-match CreateFile and friends.
void path_arg(const Rec* r, uint64_t rcx, char* out, size_t n) {
    out[0] = 0;
    if (rcx < 0x10000) return;
    static const char* const wide[] = {"CreateFileW", "CreateFile2", "GetFileAttributesW", "GetFileAttributesExW",
                                       "FindFirstFileW", "FindFirstFileExW", "CreateDirectoryW", "DeleteFileW",
                                       "LoadLibraryExW", "LoadLibraryW", "GetModuleHandleW", "_wfopen", "_wfsopen"};
    static const char* const narrow[] = {"CreateFileA", "GetFileAttributesA", "LoadLibraryA", "LoadLibraryExA", "fopen",
                                         "_fsopen", "GetModuleHandleA"};
    bool is_wide = false, is_narrow = false;
    for (const char* t : wide) is_wide |= ends_with(r->name, t);
    for (const char* t : narrow) is_narrow |= ends_with(r->name, t);
    size_t k = 0;
    if (is_wide) {
        const auto* w = reinterpret_cast<const uint16_t*>(rcx);
        for (; k + 1 < n && k < 260 && w[k]; ++k) out[k] = w[k] < 0x80 ? static_cast<char>(w[k]) : '?';
    } else if (is_narrow) {
        const auto* a = reinterpret_cast<const char*>(rcx);
        for (; k + 1 < n && k < 260 && a[k]; ++k) out[k] = a[k];
    }
    out[k] = 0;
}

void log_hit(uint64_t seq, const Rec* r, const uint64_t* regs, uint64_t ret) {
    char w[160];
    char path[272];
    where(ret, w, sizeof w);
    path_arg(r, regs[0], path, sizeof path);
    fprintf(stderr, "[trace] #%" PRIu64 " t%x %s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ") <- %s%s%s%s\n", seq,
            current_tid(), r->name, regs[0], regs[1], regs[2], regs[3], w, path[0] ? " \"" : "", path, path[0] ? "\"" : "");
}

}  // namespace
}  // namespace host::trace

extern "C" GTA6RT_GUEST_ABI void gta6rt_host_trace_hit(host::trace::Rec* r, const uint64_t* regs, uint64_t ret) {
    using namespace host::trace;
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    if (g_exe_size && ret - g_exe_base < g_exe_size) {
        r->calls_exe.fetch_add(1, std::memory_order_relaxed);
        g_exe_calls.fetch_add(1, std::memory_order_relaxed);
    }
    if (r->calls.fetch_add(1, std::memory_order_relaxed) == 0) {
        r->first_seq = seq;
        const uint32_t i = g_order_n.fetch_add(1);
        if (i < kMaxOrder) g_order[i] = r;
    }
    Hit& h = g_ring[seq % kRing];
    h.seq = seq;
    h.rec = r;
    h.ret = ret;
    std::memcpy(h.a, regs, sizeof h.a);
    h.tid = current_tid();
    if (seq < g_opt.log_first) {
        log_hit(seq, r, regs, ret);
    } else if (!g_match.empty() && g_match_logged.load(std::memory_order_relaxed) < g_opt.log_match_max) {
        for (const std::string& m : g_match) {
            if (!std::strstr(r->name, m.c_str())) continue;
            if (g_match_logged.fetch_add(1, std::memory_order_relaxed) < g_opt.log_match_max) log_hit(seq, r, regs, ret);
            break;
        }
    }
}

#if defined(__x86_64__)
extern "C" void gta6rt_host_trace_tramp(void);
#else
namespace {
void trace_hit_frame(gta6rt_guest_frame* f, void*) {
    gta6rt_host_trace_hit(reinterpret_cast<host::trace::Rec*>(f->rcx), reinterpret_cast<const uint64_t*>(f->rdx), f->r8);
}

// The bytes of trace_x86_64.S, with its call to gta6rt_host_trace_hit replaced by a call through a FRAME thunk: on
// arm64 the trampoline is guest code that FEX runs from plain readable memory.
uint64_t make_guest_tramp(uint8_t* at) {
    static const uint8_t pre[] = {0x48, 0x81, 0xec, 0x98, 0x00, 0x00, 0x00, 0x48, 0x89, 0x4c, 0x24, 0x20, 0x48, 0x89, 0x54,
                                  0x24, 0x28, 0x4c, 0x89, 0x44, 0x24, 0x30, 0x4c, 0x89, 0x4c, 0x24, 0x38, 0xf3, 0x0f, 0x7f,
                                  0x44, 0x24, 0x40, 0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x50, 0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x60,
                                  0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x70, 0x4c, 0x89, 0x94, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48,
                                  0x89, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xd1, 0x48, 0x8d, 0x54, 0x24, 0x20,
                                  0x4c, 0x8b, 0x84, 0x24, 0x98, 0x00, 0x00, 0x00};
    static const uint8_t post[] = {0x4c, 0x8b, 0x94, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x84, 0x24, 0x88, 0x00, 0x00,
                                   0x00, 0x48, 0x8b, 0x4c, 0x24, 0x20, 0x48, 0x8b, 0x54, 0x24, 0x28, 0x4c, 0x8b, 0x44, 0x24,
                                   0x30, 0x4c, 0x8b, 0x4c, 0x24, 0x38, 0xf3, 0x0f, 0x6f, 0x44, 0x24, 0x40, 0xf3, 0x0f, 0x6f,
                                   0x4c, 0x24, 0x50, 0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x60, 0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x70,
                                   0x48, 0x81, 0xc4, 0x98, 0x00, 0x00, 0x00, 0x41, 0xff, 0x22};
    gta6rt_thunk_desc d{};
    d.kind = GTA6RT_THUNK_FRAME;
    d.fn = reinterpret_cast<const void*>(&trace_hit_frame);
    d.debug_name = "host!trace_hit";
    gta6rt_gva hit = 0;
    if (gta6rt_isa_make_thunk(&d, &hit) != GTA6RT_OK) return 0;
    uint8_t* p = at;
    std::memcpy(p, pre, sizeof pre);
    p += sizeof pre;
    *p++ = 0x48;  // mov rax, hit
    *p++ = 0xB8;
    std::memcpy(p, &hit, 8);
    p += 8;
    *p++ = 0xFF;  // call rax
    *p++ = 0xD0;
    std::memcpy(p, post, sizeof post);
    return reinterpret_cast<uint64_t>(at);
}
}  // namespace
#endif

namespace host::trace {

void configure(const Options& opt, gta6rt_gva exe_base, uint64_t exe_size) {
    g_opt = opt;
    g_match.clear();
    for (const char* m = opt.match; m && *m;) {
        const char* e = std::strchr(m, ',');
        const std::string one = e ? std::string(m, e) : std::string(m);
        if (!one.empty()) g_match.push_back(one);
        m = e ? e + 1 : nullptr;
    }
    g_exe_base = exe_base;
    g_exe_size = exe_size;
}

bool enabled() { return g_opt.enabled; }

void set_exe_range(gta6rt_gva base, uint64_t size) {
    g_exe_base = base;
    g_exe_size = size;
}

gta6rt_gva wrap(gta6rt_gva target, const char* name, bool unimplemented) {
    if (!g_opt.enabled || !target) return target;
    std::lock_guard lock(g_mu);
    if (g_by_stub.count(target)) return target;
    const auto key = std::make_pair(static_cast<uint64_t>(target), std::string(name ? name : "?"));
    if (auto it = g_by_key.find(key); it != g_by_key.end()) return it->second->stub;
    if (!g_arena || g_arena_used + kStubSize > kStubArena) {
#if defined(__x86_64__)
        void* p = mmap(nullptr, kStubArena, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
#else
        void* p = mmap(nullptr, kStubArena, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
        if (p == MAP_FAILED) return target;
        g_arena = static_cast<uint8_t*>(p);
        g_arena_used = 0;
#if !defined(__x86_64__)
        if (!g_tramp) {
            g_tramp = make_guest_tramp(g_arena);
            if (!g_tramp) return target;
            g_arena_used = 256;
        }
#endif
    }
    auto* r = new Rec;
    r->target = target;
    r->name = strdup(key.second.c_str());
    r->unimplemented = unimplemented;
    uint8_t code[kStubSize] = {0x49, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0, 0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0, 0x41, 0xFF, 0xE3, 0xCC};
    const uint64_t rp = reinterpret_cast<uint64_t>(r);
#if defined(__x86_64__)
    const uint64_t tp = reinterpret_cast<uint64_t>(&gta6rt_host_trace_tramp);
#else
    const uint64_t tp = g_tramp;
#endif
    std::memcpy(code + 2, &rp, 8);
    std::memcpy(code + 12, &tp, 8);
    uint8_t* at = g_arena + g_arena_used;
    std::memcpy(at, code, sizeof code);
    g_arena_used += kStubSize;
    const uint64_t stub = reinterpret_cast<uint64_t>(at);
    r->stub = stub;
    g_by_key.emplace(key, r);
    g_by_stub.emplace(stub, r);
    g_all.push_back(r);
    return stub;
}

void note_unimplemented_binding(const char* name) {
    std::lock_guard lock(g_mu);
    g_unimpl_bindings.emplace_back(name);
}

void note_missing_proc(const char* module, const char* name) {
    std::lock_guard lock(g_mu);
    std::string s = std::string(module) + "!" + name;
    if (std::find(g_missing_procs.begin(), g_missing_procs.end(), s) == g_missing_procs.end())
        g_missing_procs.push_back(std::move(s));
}

uint64_t total_calls() { return g_seq.load(); }
uint64_t exe_calls() { return g_exe_calls.load(); }

void dump(FILE* out, const char* why, size_t last_n) {
    const uint64_t total = g_seq.load();
    const uint32_t order_n = std::min<uint32_t>(g_order_n.load(), kMaxOrder);
    fprintf(out, "=== import trace (%s) ===\n", why);
    if (!g_opt.enabled) {
        fprintf(out, "trace disabled (--trace); unimplemented calls are listed by the shim report\n");
        return;
    }
    size_t distinct = 0, distinct_unimpl = 0;
    {
        std::lock_guard lock(g_mu);
        for (const Rec* r : g_all)
            if (r->calls.load()) {
                ++distinct;
                distinct_unimpl += r->unimplemented;
            }
        fprintf(out, "calls: %" PRIu64 " total, %" PRIu64 " from the exe, %zu distinct imports called (%zu unimplemented), %zu wrapped\n",
                total, g_exe_calls.load(), distinct, distinct_unimpl, g_all.size());
    }
    fprintf(out, "unimplemented imports called, in order of first call:\n");
    size_t k = 0;
    for (uint32_t i = 0; i < order_n; ++i) {
        const Rec* r = g_order[i];
        if (!r || !r->unimplemented) continue;
        fprintf(out, "  %zu. %s  first=#%" PRIu64 " calls=%" PRIu64 "\n", ++k, r->name, r->first_seq, r->calls.load());
    }
    if (!k) fprintf(out, "  (none)\n");
    {
        std::lock_guard lock(g_mu);
        if (!g_missing_procs.empty()) {
            fprintf(out, "GetProcAddress names not served, in order:\n");
            for (size_t i = 0; i < g_missing_procs.size(); ++i) fprintf(out, "  %zu. %s\n", i + 1, g_missing_procs[i].c_str());
        }
        std::vector<const Rec*> top(g_all.begin(), g_all.end());
        std::sort(top.begin(), top.end(), [](const Rec* a, const Rec* b) { return a->calls.load() > b->calls.load(); });
        fprintf(out, "top imports by calls:\n");
        for (size_t i = 0; i < top.size() && i < 40 && top[i]->calls.load(); ++i)
            fprintf(out, "  %10" PRIu64 " (exe %" PRIu64 ")  %s\n", top[i]->calls.load(), top[i]->calls_exe.load(), top[i]->name);
    }
    fprintf(out, "distinct imports in order of first call:\n");
    for (uint32_t i = 0; i < order_n; ++i)
        if (g_order[i]) fprintf(out, "  %u. %s%s\n", i + 1, g_order[i]->name, g_order[i]->unimplemented ? "  [unimplemented]" : "");
    const uint64_t n = std::min<uint64_t>({total, last_n, kRing});
    fprintf(out, "last %" PRIu64 " import calls:\n", n);
    for (uint64_t s = total - n; s < total; ++s) {
        const Hit& h = g_ring[s % kRing];
        if (h.seq != s || !h.rec) {
            fprintf(out, "  #%" PRIu64 " (overwritten)\n", s);
            continue;
        }
        char w[160];
        where(h.ret, w, sizeof w);
        fprintf(out, "  #%" PRIu64 " t%x %s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ") <- %s\n", h.seq, h.tid,
                h.rec->name, h.a[0], h.a[1], h.a[2], h.a[3], w);
    }
    fprintf(out, "=== end of import trace ===\n");
    fflush(out);
}

void dump_fatal(const char* why) {
    char buf[512];
    const uint64_t total = g_seq.load();
    int len = snprintf(buf, sizeof buf, "=== import trace (%s): %" PRIu64 " calls, %" PRIu64 " from the exe ===\n", why, total,
                       g_exe_calls.load());
    write(2, buf, static_cast<size_t>(len));
    if (!g_opt.enabled) return;
    const uint32_t order_n = std::min<uint32_t>(g_order_n.load(), kMaxOrder);
    for (uint32_t i = 0; i < order_n; ++i) {
        const Rec* r = g_order[i];
        if (!r || !r->unimplemented) continue;
        len = snprintf(buf, sizeof buf, "  unimplemented: %s first=#%" PRIu64 " calls=%" PRIu64 "\n", r->name, r->first_seq,
                       r->calls.load());
        write(2, buf, static_cast<size_t>(len));
    }
    const uint64_t n = std::min<uint64_t>({total, 30, kRing});
    for (uint64_t s = total - n; s < total; ++s) {
        const Hit& h = g_ring[s % kRing];
        if (h.seq != s || !h.rec) {
            len = snprintf(buf, sizeof buf, "  #%" PRIu64 " (counted, record not written yet)\n", s);
            write(2, buf, static_cast<size_t>(len));
            continue;
        }
        if (g_exe_size && h.ret - g_exe_base < g_exe_size)
            len = snprintf(buf, sizeof buf, "  #%" PRIu64 " t%x %s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ") <- exe+0x%" PRIx64 "\n",
                           h.seq, h.tid, h.rec->name, h.a[0], h.a[1], h.a[2], h.a[3], h.ret - g_exe_base);
        else
            len = snprintf(buf, sizeof buf, "  #%" PRIu64 " t%x %s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ") <- %#" PRIx64 "\n",
                           h.seq, h.tid, h.rec->name, h.a[0], h.a[1], h.a[2], h.a[3], h.ret);
        write(2, buf, static_cast<size_t>(len));
    }
}

}  // namespace host::trace
