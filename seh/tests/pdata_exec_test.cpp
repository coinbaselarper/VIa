// Runs real prolog/epilog code of a mapped PE (the exe or a packaged DLL) under Rosetta, traps with int3 at
// instruction boundaries, and checks that seh's RtlLookupFunctionEntry + RtlVirtualUnwind over the image's own
// .pdata recover the caller state (rip, rsp, the eight nonvolatile GPRs, xmm6-15) and the establisher frame.
// Also: every entry is found at its first/last byte, and every handler-bearing entry reports its handler and
// HandlerData. Vectors come from tools/gen_vectors.py. x86_64 only.
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <time.h>

#include <vector>

#include <windows.h>

#include <gta6rt/seh.h>

#include "../src/seh_internal.h"
#include "pe_image.h"

extern "C" void seh_test_enter_guest(uint64_t func, uint64_t rsp, const uint64_t* regs, const uint8_t* xmm);

namespace {

constexpr uint64_t kSentinel = 0x00007ffe12345678ull;
constexpr int kMaxSeg = 8;

struct vec {
    char kind;
    uint8_t n;
    uint32_t s[kMaxSeg], t[kMaxSeg];
};

sigjmp_buf g_jb;
volatile int g_stage, g_nseg, g_result;
const vec* g_cur;
uint64_t g_base;
gta6rt_x64_context g_trapped;
uint64_t g_seg_rsp[kMaxSeg];
volatile uint64_t g_bad_pc;
uint8_t g_saved[kMaxSeg];
uint8_t* g_mem;
bool g_reload;
uint32_t g_push_mask;
int g_freg;
const uint64_t* g_regs;
const uint8_t* g_xmm;
constexpr int kNonvol[] = {3, 5, 6, 7, 12, 13, 14, 15};
constexpr uint64_t kJunk = 0x0bad0bad0bad0badull;

void reload_nonvolatile(_STRUCT_X86_THREAD_STATE64& ss, _STRUCT_X86_FLOAT_STATE64& fs) {
    uint64_t* g[16] = {&ss.__rax, &ss.__rcx, &ss.__rdx, &ss.__rbx, &ss.__rsp, &ss.__rbp, &ss.__rsi, &ss.__rdi,
                       &ss.__r8,  &ss.__r9,  &ss.__r10, &ss.__r11, &ss.__r12, &ss.__r13, &ss.__r14, &ss.__r15};
    for (int i = 0; i < 8; ++i) {
        const int r = kNonvol[i];
        if (r == g_freg) continue;
        *g[r] = (g_push_mask >> r) & 1 ? kJunk : g_regs[i];
    }
    memcpy(&fs.__fpu_xmm6, g_xmm, 10 * 16);
}

void on_signal(int sig, siginfo_t*, void* ucv) {
    auto* uc = static_cast<ucontext_t*>(ucv);
    auto& ss = uc->uc_mcontext->__ss;
    if (sig == SIGTRAP) {
        const uint64_t pc = ss.__rip - 1;
        const int st = g_stage;
        if (st < g_nseg - 1 && pc == g_base + g_cur->t[st]) {
            g_seg_rsp[st] = ss.__rsp;
            g_stage = st + 1;
            bool later = false;
            for (int i = st + 1; i < g_nseg; ++i) later |= g_cur->t[i] == g_cur->t[st];
            if (!later) g_mem[g_cur->t[st]] = g_saved[st];
            if (g_reload && st + 1 == g_nseg - 1) reload_nonvolatile(ss, uc->uc_mcontext->__fs);
            ss.__rip = g_base + g_cur->s[st + 1];
            return;
        }
        if (st == g_nseg - 1 && pc == g_base + g_cur->t[st]) {
            gta6rt_seh_context_from_ucontext(uc, &g_trapped);
            g_trapped.rip = pc;
            g_seg_rsp[st] = ss.__rsp;
            g_result = 1;
        } else {
            g_result = 2;
            g_bad_pc = pc;
        }
    } else {
        g_result = -sig;
        g_bad_pc = ss.__rip;
    }
    siglongjmp(g_jb, 1);
}

struct uinfo_view {
    uint8_t flags, prolog, count, freg, foff;
    const uint16_t* codes;
};

uinfo_view view(const uint8_t* p) {
    return {static_cast<uint8_t>(p[0] >> 3), p[1], p[2], static_cast<uint8_t>(p[3] & 15), static_cast<uint8_t>(p[3] >> 4),
            reinterpret_cast<const uint16_t*>(p + 4)};
}

int slots(uint16_t c, int version) {
    const int op = (c >> 8) & 15, info = c >> 12;
    if (op == 1) return info ? 3 : 2;
    if (op == 4 || op == 8) return 2;
    if (op == 5 || op == 9 || op == 7) return 3;
    if (op == 6) return version >= 2 ? 1 : 2;
    return 1;
}

// Independent statement of the Windows establisher-frame rule, from the trapped registers.
uint64_t expected_establisher(const pe::image& im, const RUNTIME_FUNCTION* fe, const gta6rt_x64_context& c) {
    const uint8_t* ui = im.mem + fe->UnwindData;
    const uinfo_view u = view(ui);
    if (!u.freg) return c.rsp;
    const uint64_t off = c.rip - (im.base + fe->BeginAddress);
    bool use_fp = (u.flags & 4) || off >= u.prolog;
    if (!use_fp)
        for (int i = 0; i < u.count; i += slots(u.codes[i], ui[0] & 7))
            if (((u.codes[i] >> 8) & 15) == 3) {
                use_fp = off >= (u.codes[i] & 0xffu);
                break;
            }
    return use_fp ? (&c.rax)[u.freg] - u.foff * 16ull : c.rsp;
}

bool load_vectors(const char* path, std::vector<vec>* out, uint32_t* chkstk) {
    FILE* f = fopen(path, "r");
    if (!f) {
        perror(path);
        return false;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'K') {
            sscanf(line + 2, "%x", chkstk);
            continue;
        }
        if (line[0] != 'V') continue;
        vec v{};
        int n = 0, used = 0;
        char kind;
        if (sscanf(line + 2, "%c %d%n", &kind, &n, &used) != 2 || n < 1 || n > kMaxSeg) continue;
        v.kind = kind;
        v.n = static_cast<uint8_t>(n);
        const char* p = line + 2 + used;
        bool ok = true;
        for (int i = 0; i < n && ok; ++i) {
            int u = 0;
            ok = sscanf(p, "%x %x%n", &v.s[i], &v.t[i], &u) == 2;
            p += u;
        }
        if (ok) out->push_back(v);
    }
    fclose(f);
    return true;
}

// PUSH_NONVOL registers and frame register over the whole chain of the entry covering rva.
void chain_regs(const pe::image& im, uint32_t rva, uint32_t* push_mask, int* freg) {
    *push_mask = 0;
    *freg = 0;
    DWORD64 b = 0;
    const RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(im.base + rva, &b, nullptr);
    for (int d = 0; f && d < 32; ++d) {
        const uint8_t* ui = im.mem + f->UnwindData;
        const uinfo_view u = view(ui);
        if (!*freg) *freg = u.freg;
        for (int i = 0; i < u.count; i += slots(u.codes[i], ui[0] & 7))
            if (((u.codes[i] >> 8) & 15) == 0) *push_mask |= 1u << (u.codes[i] >> 12);
        if (!(u.flags & 4)) break;
        f = reinterpret_cast<const RUNTIME_FUNCTION*>(ui + 4 + 2 * ((u.count + 1) & ~1));
    }
}

int kind_index(char k) {
    switch (k) {
    case 'P': return 0;
    case 'E': return 1;
    case 'C': return 2;
    case 'F': return 3;
    default: return 4;
    }
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s image.(exe|dll) [vectors.txt]\n", argv[0]);
        return 2;
    }
    pe::image im;
    if (!pe::map(argv[1], 0, &im)) return 1;
    g_base = im.base;
    g_mem = im.mem;
    const auto* table = reinterpret_cast<const RUNTIME_FUNCTION*>(im.mem + im.dir_rva(3));
    const uint32_t count = im.dir_size(3) / 12;
    if (gta6rt_seh_add_function_table(im.base, reinterpret_cast<const gta6rt_runtime_function*>(table), count, im.size) !=
        GTA6RT_OK) {
        printf("add_function_table failed\n");
        return 1;
    }
    printf("image %s at %#llx, %u RUNTIME_FUNCTION entries\n", argv[1], static_cast<unsigned long long>(im.base), count);

    uint32_t lookup_bad = 0;
    for (uint32_t i = 0; i < count; ++i) {
        DWORD64 b = 0;
        const RUNTIME_FUNCTION* f = &table[i];
        if (RtlLookupFunctionEntry(im.base + f->BeginAddress, &b, nullptr) != f || b != im.base ||
            RtlLookupFunctionEntry(im.base + f->EndAddress - 1, &b, nullptr) != f)
            ++lookup_bad;
    }
    printf("lookup: %u entries, %u not found at their first/last byte\n", count, lookup_bad);

    const size_t scratch_size = 64u << 20;
    auto* scratch = static_cast<uint8_t*>(mmap(nullptr, scratch_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    const uint64_t mid = reinterpret_cast<uint64_t>(scratch) + scratch_size / 2;
    uint32_t h_entries = 0, h_ok = 0, h_bad = 0, h_skipped_epilog = 0, h_none_ok = 0, h_none_bad = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const RUNTIME_FUNCTION* f = &table[i];
        const uint8_t* ui = im.mem + f->UnwindData;
        const uinfo_view u = view(ui);
        const bool chained = u.flags & 4;
        const uint8_t* root = ui;
        for (int d = 0; d < 32 && (view(root).flags & 4); ++d) {
            const uinfo_view r = view(root);
            const auto* parent = reinterpret_cast<const RUNTIME_FUNCTION*>(root + 4 + 2 * ((r.count + 1) & ~1));
            root = im.mem + parent->UnwindData;
        }
        const uinfo_view r = view(root);
        const uint64_t pc = im.base + f->BeginAddress + (chained ? 0 : u.prolog);
        if (pc >= im.base + f->EndAddress) continue;
        alignas(16) gta6rt_x64_context c;
        memset(&c, 0, sizeof c);
        for (int g = 0; g < 16; ++g) (&c.rax)[g] = mid;
        c.rip = pc;
        seh::unwind_out o;
        seh::virtual_unwind(UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER, im.base, pc, reinterpret_cast<const seh::rf_t*>(f), &c,
                            nullptr, false, &o);
        if (o.in_epilog) {
            ++h_skipped_epilog;
            continue;
        }
        alignas(16) CONTEXT wc;
        memset(&wc, 0, sizeof wc);
        for (int g = 0; g < 16; ++g) (&wc.Rax)[g] = mid;
        wc.Rip = pc;
        PVOID hd = nullptr;
        DWORD64 est = 0;
        PEXCEPTION_ROUTINE h = RtlVirtualUnwind(UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER, im.base, pc,
                                                const_cast<PRUNTIME_FUNCTION>(f), &wc, &hd, &est, nullptr);
        const uint64_t want_est = u.freg ? mid - u.foff * 16ull : mid;
        if (r.flags & 3) {
            ++h_entries;
            const uint8_t* tail = root + 4 + 2 * ((r.count + 1) & ~1);
            const uint64_t want_h = im.base + pe::u32(tail);
            const void* want_d = tail + 4;
            if (reinterpret_cast<uint64_t>(h) == want_h && hd == want_d && est == want_est && o.handler == want_h &&
                o.handler_data == want_d)
                ++h_ok;
            else if (h_bad++ < 10)
                printf("  handler mismatch at %#x: got %p data %p est %+lld, want %#llx data %p\n", f->BeginAddress,
                       reinterpret_cast<void*>(h), hd, static_cast<long long>(est - want_est),
                       static_cast<unsigned long long>(want_h), want_d);
        } else {
            if (!h && est == want_est) ++h_none_ok;
            else ++h_none_bad;
        }
    }
    printf("handlers: %u entries with a handler (after chains): %u ok, %u wrong; %u without: %u ok, %u wrong; "
           "%u skipped (body start is an epilog)\n",
           h_entries, h_ok, h_bad, h_none_ok + h_none_bad, h_none_ok, h_none_bad, h_skipped_epilog);
    munmap(scratch, scratch_size);

    int rc = (lookup_bad || h_bad || h_none_bad) ? 1 : 0;
    if (argc < 3) {
        printf("seh_pdata_exec_test: %s (static checks only)\n", rc ? "FAIL" : "PASS");
        return rc;
    }

    std::vector<vec> vecs;
    uint32_t chkstk = 0;
    if (!load_vectors(argv[2], &vecs, &chkstk)) return 1;
    const uint8_t chkstk_orig = chkstk ? im.mem[chkstk] : 0;
    if (chkstk) im.mem[chkstk] = 0xc3;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGTRAP, SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(s, &sa, nullptr);
    stack_t ss{};
    ss.ss_sp = malloc(1 << 16);
    ss.ss_size = 1 << 16;
    sigaltstack(&ss, nullptr);

    const size_t stack_size = 4u << 20;
    auto* stk = static_cast<uint8_t*>(mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    const uint64_t entry_rsp = ((reinterpret_cast<uint64_t>(stk) + stack_size - 8192) & ~15ull) - 8;

    static const int nonvol[] = {3, 5, 6, 7, 12, 13, 14, 15};
    uint64_t regs[8];
    alignas(16) uint8_t xmm[10][16];
    for (int i = 0; i < 8; ++i)
        regs[i] = 0xA5A5000000000000ull | (static_cast<uint64_t>(nonvol[i]) << 32) | (0x1111u * (i + 1));
    for (int i = 0; i < 10; ++i)
        for (int j = 0; j < 16; ++j) xmm[i][j] = static_cast<uint8_t>(0x60 + i * 16 + j);
    g_regs = regs;
    g_xmm = xmm[0];

    const char* kinds = "PECF?";
    uint64_t pass[5] = {}, fail[5] = {}, sig[5] = {}, epi[5] = {}, naive[5] = {}, total[5] = {};
    uint64_t est_ok = 0, est_bad = 0, body_ok = 0, body_bad = 0, v2_total = 0, v2_pass = 0;
    int shown = 0;
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (const vec& v : vecs) {
        const int k = kind_index(v.kind);
        ++total[k];
        const bool own_chkstk = chkstk && v.s[0] == chkstk;
        if (own_chkstk) im.mem[chkstk] = chkstk_orig;
        for (int i = 0; i < v.n; ++i) g_saved[i] = im.mem[v.t[i]];
        for (int i = 0; i < v.n; ++i) im.mem[v.t[i]] = 0xcc;
        memset(reinterpret_cast<void*>(entry_rsp - (1u << 20)), 0x5c, (1u << 20) + 4096);
        memcpy(reinterpret_cast<void*>(entry_rsp), &kSentinel, 8);
        g_reload = v.kind == 'E' || v.kind == 'F';
        if (g_reload) chain_regs(im, v.s[v.n - 1], &g_push_mask, &g_freg);
        g_cur = &v;
        g_stage = 0;
        g_nseg = v.n;
        g_result = 0;
        if (!sigsetjmp(g_jb, 1)) seh_test_enter_guest(im.base + v.s[0], entry_rsp, regs, xmm[0]);
        for (int i = v.n - 1; i >= 0; --i) im.mem[v.t[i]] = g_saved[i];
        if (own_chkstk) im.mem[chkstk] = 0xc3;
        if (g_result != 1) {
            ++sig[k];
            if (shown++ < 20)
                printf("  %s %d in %c vector %x..%x (stage %d) at %llx\n", g_result == 2 ? "unexpected trap" : "signal",
                       g_result, v.kind, v.s[0], v.t[v.n - 1], g_stage,
                       static_cast<unsigned long long>(g_bad_pc - im.base));
            continue;
        }
        {
            uint64_t ra;
            memcpy(&ra, reinterpret_cast<const void*>(g_trapped.rsp), 8);
            bool ok = ra == kSentinel && g_trapped.rsp == entry_rsp;
            for (int r = 0; r < 8; ++r) ok &= (&g_trapped.rax)[nonvol[r]] == regs[r];
            naive[k] += ok;
        }
        alignas(16) CONTEXT c;
        memcpy(&c, &g_trapped, sizeof c);
        DWORD64 base = 0, est = 0;
        PVOID hd = nullptr;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!fe) {
            ++fail[k];
            if (shown++ < 20) printf("  no function entry for trap pc %llx\n", static_cast<unsigned long long>(c.Rip - im.base));
            continue;
        }
        alignas(16) gta6rt_x64_context c2 = g_trapped;
        seh::unwind_out o;
        seh::virtual_unwind(0, base, c2.rip, reinterpret_cast<const seh::rf_t*>(fe), &c2, nullptr, false, &o);
        epi[k] += o.in_epilog;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &est, nullptr);
        bool ok = c.Rip == kSentinel && c.Rsp == entry_rsp + 8 && c2.rip == c.Rip && c2.rsp == c.Rsp;
        const uint64_t* got = &c.Rax;
        for (int r = 0; r < 8; ++r) ok &= got[nonvol[r]] == regs[r];
        for (int r = 0; r < 10; ++r) ok &= memcmp(&c.FltSave.XmmRegisters[6 + r], xmm[r], 16) == 0;
        const uint64_t want_est = expected_establisher(im, fe, g_trapped);
        if (est == want_est && o.establisher == want_est) ++est_ok;
        else ++est_bad;
        const uint8_t* fui = im.mem + fe->UnwindData;
        if (!(fui[3] & 15) && g_trapped.rsp == g_seg_rsp[0] && (v.kind != 'P' || v.t[0] - v.s[0] == fui[1])) {
            if (est == g_seg_rsp[0]) ++body_ok;
            else ++body_bad;
        }
        if ((fui[0] & 7) == 2) {
            ++v2_total;
            v2_pass += ok;
        }
        if (ok) {
            ++pass[k];
        } else {
            ++fail[k];
            if (shown++ < 20)
                printf("  FAIL %c %x..%x pc=%llx rip=%llx rsp=%+lld rbx=%llx rbp=%llx in_epilog=%d\n", v.kind, v.s[0],
                       v.t[v.n - 1], static_cast<unsigned long long>(g_trapped.rip - im.base),
                       static_cast<unsigned long long>(c.Rip), static_cast<long long>(c.Rsp - entry_rsp - 8),
                       static_cast<unsigned long long>(c.Rbx), static_cast<unsigned long long>(c.Rbp), o.in_epilog);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t all_fail = 0, all_sig = 0;
    for (int k = 0; k < 5; ++k) {
        if (!total[k]) continue;
        printf("%c vectors: %llu: %llu pass, %llu fail, %llu signalled; %llu detected as epilog; naive leaf unwind would "
               "pass %llu\n",
               kinds[k], (unsigned long long)total[k], (unsigned long long)pass[k], (unsigned long long)fail[k],
               (unsigned long long)sig[k], (unsigned long long)epi[k], (unsigned long long)naive[k]);
        all_fail += fail[k];
        all_sig += sig[k];
    }
    printf("establisher frame: %llu match the WRK rule, %llu do not; at body points %llu equal the post-prolog rsp, "
           "%llu do not\n",
           (unsigned long long)est_ok, (unsigned long long)est_bad, (unsigned long long)body_ok,
           (unsigned long long)body_bad);
    printf("version-2 UNWIND_INFO trap points: %llu, %llu pass\n", (unsigned long long)v2_total,
           (unsigned long long)v2_pass);
    printf("%zu vectors in %.1f s\n", vecs.size(),
           static_cast<double>(t1.tv_sec - t0.tv_sec) + static_cast<double>(t1.tv_nsec - t0.tv_nsec) / 1e9);
    if (all_fail || all_sig || est_bad || body_bad) rc = 1;
    printf("seh_pdata_exec_test: %s\n", rc ? "FAIL" : "PASS");
    return rc;
}
