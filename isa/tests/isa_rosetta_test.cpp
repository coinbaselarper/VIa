// x86_64 (Rosetta) execution tests for isa/rosetta: TEB SHARE/SWAP through real gs: reads, TLS growth seen
// by guest code on other threads, foreign threads, Win64 guest calls and thunks (shadow space, alignment,
// stack args, floats, callee-saved registers, re-entry), faults, __chkstk, SSE4a emulation (SIGILL and patch),
// the vpsrlvq/vpsllvq fix, cpuid/xgetbv traps, and the fixups executed in place on a copy of the real exe.

#include <gta6rt/isa.h>

#include "isa_internal.h"
#include "sse4a.h"
#include "x86_patch.h"
#include "test_common.h"

#include <pthread.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>

#include <atomic>
#include <cmath>

using namespace gta6rt::isa;

extern "C" {
void isa_test_run_ymm(const uint64_t in[16][4], uint64_t out[16][4], uint64_t target);
void isa_test_run_gpr(uint64_t r[4], uint64_t target);
uint64_t isa_test_chkstk(uint64_t fn, uint64_t size);
void guest_sum16();
void guest_fmix();
void guest_tls_entry();
void guest_stack_base();
void guest_gs60();
void guest_gs10();
void guest_gs58_dbl();
void guest_call6();
void guest_fault_null();
void guest_int3();
void guest_ud2();
}

namespace gta6rt::isa {
struct TitleSite {
    uint32_t rva;
    uint8_t kind;
    uint8_t len;
    uint8_t bytes[15];
};
#include "title_sites.inc"
}  // namespace gta6rt::isa

static uint64_t G(void (*f)()) {
    return reinterpret_cast<uint64_t>(f);
}

static uint64_t now_ns() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static uint64_t call(uint64_t fn, std::initializer_list<uint64_t> a, uint32_t fmask = 0, gta6rt_guest_ret* ret = nullptr) {
    gta6rt_guest_args g{};
    uint32_t i = 0;
    for (uint64_t v : a) g.args[i++] = v;
    g.count = i;
    g.float_mask = fmask;
    gta6rt_guest_ret r{};
    const gta6rt_status st = gta6rt_isa_call_guest(fn, &g, &r);
    CHECK(st == GTA6RT_OK, "call_guest status %d", st);
    if (ret) *ret = r;
    return r.rax;
}

static uint64_t code_near = 0;

static uint64_t put_code(const std::vector<uint8_t>& c) {
    const uint64_t a = arena_put(code_near, c);
    CHECK(a != 0, "arena_put");
    return a;
}

// ---- TLS / TEB -----------------------------------------------------------------------------------------

struct Blk {
    uint64_t tag;
};
static std::atomic<int> g_blocks{0};
static void* ctor(uint32_t idx, uint32_t tid, void*) {
    g_blocks++;
    return new Blk{(static_cast<uint64_t>(idx) << 32) | tid};
}
static void dtor(void* b, void*) {
    g_blocks--;
    delete static_cast<Blk*>(b);
}

static Barrier b1, b2, b3;
static int g_peb_dummy[512];

static void check_thread_view(const char* who) {
    auto* teb = static_cast<uint8_t*>(gta6rt_isa_current_teb());
    const uint32_t tid = static_cast<uint32_t>(teb_get(teb, GTA6RT_TEB_CLIENT_ID_THREAD));
    auto* blk = reinterpret_cast<Blk*>(call(G(guest_tls_entry), {0}));
    CHECK(blk && blk->tag == tid, "%s: gs:[0x58][0]", who);
    CHECK(call(G(guest_stack_base), {}) == teb_get(teb, GTA6RT_TEB_STACK_BASE), "%s: gs:[0x30]->StackBase", who);
    CHECK(call(G(guest_stack_base), {}) == reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(pthread_self())),
          "%s: StackBase is this thread's stack top", who);
    CHECK(call(G(guest_gs60), {}) == reinterpret_cast<uint64_t>(g_peb_dummy), "%s: gs:[0x60] = PEB", who);
    CHECK(call(G(guest_gs58_dbl), {}) == teb_get(teb, GTA6RT_TEB_TLS_POINTER), "%s: 65 65 prefixed gs:[0x58]", who);
}

static void* share_worker(void*) {
    gta6rt_teb_desc d{};
    CHECK(gta6rt_isa_thread_attach(&d, nullptr) == GTA6RT_OK, "worker attach");
    CHECK(gta6rt_isa_thread_teb_mode() == GTA6RT_TEB_SHARE, "worker SHARE");
    check_thread_view("worker");
    b1.wait();
    b2.wait();
    auto* teb = static_cast<uint8_t*>(gta6rt_isa_current_teb());
    const uint32_t tid = static_cast<uint32_t>(teb_get(teb, GTA6RT_TEB_CLIENT_ID_THREAD));
    auto* blk = reinterpret_cast<Blk*>(call(G(guest_tls_entry), {12}));
    CHECK(blk && blk->tag == ((12ull << 32) | tid), "worker: guest sees TLS index 12 registered later (array grown)");
    CHECK(call(G(guest_gs58_dbl), {}) == teb_get(teb, GTA6RT_TEB_TLS_POINTER), "worker: gs:[0x58] follows growth");
    b3.wait();
    return nullptr;
}

static void* foreign_worker(void*) {
    CHECK(gta6rt_isa_current_teb() == nullptr, "foreign thread starts without TEB");
    auto* blk = reinterpret_cast<Blk*>(call(G(guest_tls_entry), {0}));
    CHECK(blk && (blk->tag >> 32) == 0, "foreign thread got TLS on first guest call");
    CHECK(gta6rt_isa_current_teb() != nullptr, "foreign thread auto-attached");
    return nullptr;
}

static void test_share() {
    CHECK(gta6rt_isa_tls_register_image(0, ctor, dtor, nullptr) == GTA6RT_OK, "tls 0");
    gta6rt_isa_set_peb(g_peb_dummy);
    gta6rt_teb_desc d{};
    d.thread_id = 0x104;
    CHECK(gta6rt_isa_thread_attach(&d, nullptr) == GTA6RT_OK, "main attach");
    CHECK(gta6rt_isa_thread_teb_mode() == GTA6RT_TEB_SHARE, "main SHARE");
    check_thread_view("main");

    b1.init(4);
    b2.init(4);
    b3.init(4);
    pthread_t th[3];
    for (auto& t : th) pthread_create(&t, nullptr, share_worker, nullptr);
    b1.wait();
    CHECK(gta6rt_isa_tls_register_image(12, ctor, dtor, nullptr) == GTA6RT_OK, "late tls 12");
    b2.wait();
    b3.wait();
    for (auto& t : th) pthread_join(t, nullptr);
    const int before = g_blocks.load();
    pthread_t f;
    pthread_create(&f, nullptr, foreign_worker, nullptr);
    pthread_join(f, nullptr);
    CHECK(g_blocks.load() == before, "foreign thread's TLS freed at exit (%d vs %d)", g_blocks.load(), before);
}

// ---- calls and thunks ------------------------------------------------------------------------------------

static void test_calls() {
    uint64_t sum = 0;
    gta6rt_guest_args a{};
    for (int i = 0; i < 16; ++i) {
        a.args[i] = 0x100000001ull * (i + 1);
        sum += a.args[i];
    }
    a.count = 16;
    gta6rt_guest_ret r{};
    CHECK(gta6rt_isa_call_guest(G(guest_sum16), &a, &r) == GTA6RT_OK && r.rax == sum, "16-arg call: %llx vs %llx",
          (unsigned long long)r.rax, (unsigned long long)sum);
    double b = 1.5, dd = -2.25, e = 1000.125;
    uint64_t bb, db, eb;
    memcpy(&bb, &b, 8);
    memcpy(&db, &dd, 8);
    memcpy(&eb, &e, 8);
    call(G(guest_fmix), {7, bb, 9, db, eb}, 0b11010, &r);
    double got;
    memcpy(&got, &r.xmm0_lo, 8);
    CHECK(got == b * dd + e + 7 + 9, "float args/return: %f", got);
    CHECK(gta6rt_isa_call_guest4(G(guest_tls_entry), 0, 0, 0, 0) != 0, "call_guest4");
    uint64_t rax = 0;
    CHECK(gta6rt_isa_run_entry(G(guest_stack_base), 0, 0, 0, &rax) == GTA6RT_OK && rax != 0, "run_entry");
}

static std::atomic<int> g_frame_hits{0};
static bool g_frame_ok = false;

static void clobber_sysv_volatiles() {
    __asm__ volatile(
        "xor %%esi, %%esi\n xor %%edi, %%edi\n pxor %%xmm6, %%xmm6\n pxor %%xmm7, %%xmm7\n pxor %%xmm8, %%xmm8\n"
        "pxor %%xmm9, %%xmm9\n pxor %%xmm10, %%xmm10\n pxor %%xmm11, %%xmm11\n pxor %%xmm12, %%xmm12\n"
        "pxor %%xmm13, %%xmm13\n pxor %%xmm14, %%xmm14\n pxor %%xmm15, %%xmm15\n vzeroall\n"
        :
        :
        : "rsi", "rdi", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
          "xmm12", "xmm13", "xmm14", "xmm15");
}

static void frame6(gta6rt_guest_frame* f, void* user) {
    g_frame_hits++;
    const auto* st = reinterpret_cast<const uint64_t*>(f->rsp);
    g_frame_ok = f->rcx == 11 && f->rdx == 22 && f->r8 == 33 && f->r9 == 44 && st[5] == 55 && st[6] == 66 &&
                 f->xmm0 == 11 && f->xmm1 == 22 && f->xmm2 == 33 && f->xmm3 == 44 && (f->rsp & 15) == 8 &&
                 user == &g_frame_hits;
    char buf[64];
    snprintf(buf, sizeof buf, "%.3f %p", 3.25, static_cast<void*>(pthread_self()));
    clobber_sysv_volatiles();
    f->rax = 0x600D + (strlen(buf) > 5 ? 0 : 1);
}

static void frame_float(gta6rt_guest_frame* f, void*) {
    double x;
    memcpy(&x, &f->xmm1, 8);
    x = std::sqrt(x);
    memcpy(&f->xmm0_ret, &x, 8);
    f->ret_is_float = 1;
}

static void frame_reenter(gta6rt_guest_frame* f, void*) {
    gta6rt_guest_args a{};
    for (int i = 0; i < 16; ++i) a.args[i] = f->rcx + i;
    a.count = 16;
    gta6rt_guest_ret r{};
    gta6rt_isa_call_guest(G(guest_sum16), &a, &r);
    f->rax = r.rax;
}

__attribute__((ms_abi)) static uint64_t native6(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t g) {
    __asm__ volatile("xor %%esi, %%esi\n xor %%edi, %%edi\n pxor %%xmm6, %%xmm6\n pxor %%xmm15, %%xmm15" ::: "rsi", "rdi",
                     "xmm6", "xmm15");
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * g;
}

static void trap_handler(gta6rt_guest_frame* f, void*) {
    f->rax = 0x7A9;
}

static gta6rt_gva g_thunk_frame6, g_thunk_native6, g_thunk_float, g_thunk_reenter, g_thunk_trap;

static void make_thunks() {
    gta6rt_thunk_desc d{GTA6RT_THUNK_FRAME, reinterpret_cast<const void*>(&frame6), &g_frame_hits, "test!frame6"};
    CHECK(gta6rt_isa_make_thunk(&d, &g_thunk_frame6) == GTA6RT_OK, "frame thunk");
    d = {GTA6RT_THUNK_NATIVE, reinterpret_cast<const void*>(&native6), nullptr, "test!native6"};
    CHECK(gta6rt_isa_make_thunk(&d, &g_thunk_native6) == GTA6RT_OK, "native thunk");
    d = {GTA6RT_THUNK_FRAME, reinterpret_cast<const void*>(&frame_float), nullptr, "test!sqrt"};
    CHECK(gta6rt_isa_make_thunk(&d, &g_thunk_float) == GTA6RT_OK, "float thunk");
    d = {GTA6RT_THUNK_FRAME, reinterpret_cast<const void*>(&frame_reenter), nullptr, "test!reenter"};
    CHECK(gta6rt_isa_make_thunk(&d, &g_thunk_reenter) == GTA6RT_OK, "reenter thunk");
    d = {GTA6RT_THUNK_TRAP, reinterpret_cast<const void*>(&trap_handler), nullptr, "missing.dll!Unbound"};
    CHECK(gta6rt_isa_make_thunk(&d, &g_thunk_trap) == GTA6RT_OK, "trap thunk");
}

static void test_thunks(const char* mode) {
    const int hits = g_frame_hits.load();
    g_frame_ok = false;
    uint64_t r = call(G(guest_call6), {g_thunk_frame6, 11, 22, 33, 44, 55, 66});
    CHECK(r == 0x600D && g_frame_ok && g_frame_hits.load() == hits + 1,
          "%s: FRAME thunk: rax=%llx args_ok=%d (0xBADx = callee-saved register lost)", mode, (unsigned long long)r,
          g_frame_ok);
    r = call(G(guest_call6), {g_thunk_native6, 1, 2, 3, 4, 5, 6});
    CHECK(r == 1 + 4 + 9 + 16 + 25 + 36, "%s: NATIVE thunk: rax=%llx", mode, (unsigned long long)r);
    double x = 2.25;
    uint64_t xb;
    memcpy(&xb, &x, 8);
    gta6rt_guest_ret ret{};
    call(g_thunk_float, {0, xb}, 2, &ret);
    double y;
    memcpy(&y, &ret.xmm0_lo, 8);
    CHECK(y == 1.5, "%s: float return through FRAME thunk: %f", mode, y);
    uint64_t expect = 0;
    for (int i = 0; i < 16; ++i) expect += 100 + i;
    r = call(G(guest_call6), {g_thunk_reenter, 100, 0, 0, 0, 0, 0});
    CHECK(r == expect, "%s: guest->host->guest re-entry: %llx", mode, (unsigned long long)r);
    r = call(G(guest_call6), {g_thunk_trap, 0, 0, 0, 0, 0, 0});
    CHECK(r == 0x7A9, "%s: TRAP thunk with handler", mode);
}

static void bench_thunks(const char* mode) {
    const int n = 20000;
    const uint64_t t0 = now_ns();
    for (int i = 0; i < n; ++i) call(G(guest_call6), {g_thunk_native6, 1, 2, 3, 4, 5, 6});
    const uint64_t t1 = now_ns();
    for (int i = 0; i < n; ++i) call(G(guest_sum16), {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    const uint64_t t2 = now_ns();
    std::printf("timing %s: host->guest->NATIVE thunk round trip %.0f ns, host->guest call alone %.0f ns\n", mode,
                double(t1 - t0) / n, double(t2 - t1) / n);
}

// ---- SWAP ---------------------------------------------------------------------------------------------------

static void* swap_worker(void*) {
    gta6rt_isa_config cfg{GTA6RT_TEB_SWAP, 0};
    gta6rt_isa_init(&cfg);
    gta6rt_teb_desc d{};
    CHECK(gta6rt_isa_thread_attach(&d, nullptr) == GTA6RT_OK, "swap attach");
    cfg.teb_mode = GTA6RT_TEB_SHARE;
    gta6rt_isa_init(&cfg);
    CHECK(gta6rt_isa_thread_teb_mode() == GTA6RT_TEB_SWAP, "thread is in SWAP mode");
    check_thread_view("swap");
    auto* teb = static_cast<uint8_t*>(gta6rt_isa_current_teb());
    CHECK(call(G(guest_gs10), {}) == teb_get(teb, GTA6RT_TEB_STACK_LIMIT), "swap: gs:[0x10] = StackLimit");
    test_thunks("SWAP");
    const uint64_t ins = put_code({0xF2, 0x0F, 0x78, 0xC1, 0x10, 0x10, 0x66, 0x48, 0x0F, 0x7E, 0xC0, 0xC3});
    const uint64_t a0 = 0x1111222233334444ull, a1 = 0xaaaabbbbccccddddull;
    CHECK(call(ins, {a0, a1}, 3) == sse4a_insertq(a0, a1, 16, 16), "swap: SIGILL (insertq) emulated inside guest code");
    CHECK(call(G(guest_fault_null), {}) == 42, "swap: access violation -> handler -> resume");
    CHECK(call(G(guest_int3), {}) == 43, "swap: int3 -> handler -> resume");
    char buf[32];
    snprintf(buf, sizeof buf, "%.2f", 1.25);
    CHECK(!strcmp(buf, "1.25"), "swap: libc float printf works after guest calls and signals");
    bench_thunks("SWAP");
    return nullptr;
}

// ---- foreign thread hook ----------------------------------------------------------------------------------

static std::atomic<int> g_hook_calls{0};

static gta6rt_status foreign_hook(void* user) {
    g_hook_calls++;
    gta6rt_teb_desc d{};
    d.thread_id = *static_cast<uint32_t*>(user);
    return gta6rt_isa_thread_attach(&d, nullptr);
}

static void test_foreign_hook() {
    static uint32_t tid = 0x4444;
    gta6rt_isa_set_foreign_thread_hook(foreign_hook, &tid);
    pthread_t f;
    pthread_create(
        &f, nullptr,
        [](void*) -> void* {
            auto* blk = reinterpret_cast<Blk*>(call(G(guest_tls_entry), {0}));
            CHECK(blk && blk->tag == 0x4444, "hooked foreign thread: TLS built for the hook's thread id");
            return nullptr;
        },
        nullptr);
    pthread_join(f, nullptr);
    CHECK(g_hook_calls.load() == 1, "foreign hook called once");
    gta6rt_isa_set_foreign_thread_hook(nullptr, nullptr);
}

// ---- faults -------------------------------------------------------------------------------------------------

static std::atomic<int> g_ill_hook_calls{0};

static gta6rt_fault_action fault_handler(gta6rt_x64_exception_record* rec, gta6rt_x64_context* ctx, void*) {
    if (rec->code == 0xC0000005 && rec->n_params == 2 && rec->params[1] == 0x10 && rec->params[0] == 0) {
        ctx->rip += 8;
        ctx->rax = 42;
        return GTA6RT_FAULT_RESUME;
    }
    if (rec->code == 0x80000003 && *reinterpret_cast<const uint8_t*>(ctx->rip) == 0xCC) {
        ctx->rip += 1;
        ctx->rax = 43;
        return GTA6RT_FAULT_RESUME;
    }
    if (rec->code == 0xC000001D && rec->address == ctx->rip) {
        ctx->rip += 2;
        ctx->rax = 44 + (ctx->mxcsr ? 0 : 100);
        return GTA6RT_FAULT_RESUME;
    }
    return GTA6RT_FAULT_CONTINUE_SEARCH;
}

static gta6rt_fault_action ill_hook(gta6rt_x64_context*, const uint8_t* insn, void*) {
    g_ill_hook_calls++;
    return insn[0] == 0x0F && insn[1] == 0x0B ? GTA6RT_FAULT_CONTINUE_SEARCH : GTA6RT_FAULT_CONTINUE_SEARCH;
}

static void test_faults() {
    CHECK(gta6rt_isa_set_guest_fault_handler(fault_handler, nullptr) == GTA6RT_OK, "set fault handler");
    CHECK(gta6rt_isa_set_illegal_instruction_hook(ill_hook, nullptr) == GTA6RT_OK, "set ill hook");
    CHECK(call(G(guest_fault_null), {}) == 42, "access violation -> handler -> resume");
    CHECK(call(G(guest_int3), {}) == 43, "int3 -> STATUS_BREAKPOINT with rip at the int3");
    CHECK(call(G(guest_ud2), {}) == 44, "ud2 -> illegal hook -> STATUS_ILLEGAL_INSTRUCTION");
    CHECK(g_ill_hook_calls.load() == 1, "illegal hook called once");
}

// ---- __chkstk ------------------------------------------------------------------------------------------------

static void test_chkstk(const char* exe) {
    FILE* f = fopen(exe, "rb");
    if (!f) {
        CHECK(false, "open exe");
        return;
    }
    std::vector<uint8_t> c(78);
    fseek(f, 0x5646540, SEEK_SET);  // file offset of RVA 0x5647140
    CHECK(fread(c.data(), 1, c.size(), f) == c.size(), "read __chkstk");
    fclose(f);
    const uint64_t at = put_code(c);
    CHECK(gta6rt_isa_find_chkstk(at - 64, 64 + 78 + 64) == at, "find __chkstk in code");
    for (uint64_t sz : {0x20ull, 0x1000ull, 0x5000ull, 0x100000ull})
        CHECK(isa_test_chkstk(at, sz) == 0, "unpatched __chkstk(0x%llx) under SHARE (silent no-op by accident)",
              (unsigned long long)sz);
    CHECK(gta6rt_isa_patch_chkstk(at) == GTA6RT_OK && *reinterpret_cast<uint8_t*>(at) == 0xC3, "patch __chkstk");
    for (uint64_t sz : {0x20ull, 0x5000ull, 0x4000000ull})
        CHECK(isa_test_chkstk(at, sz) == 0, "patched __chkstk(0x%llx)", (unsigned long long)sz);
    std::vector<uint8_t> other(c);
    other[30] ^= 1;
    const uint64_t at2 = put_code(other);
    CHECK(gta6rt_isa_patch_chkstk(at2) == GTA6RT_E_BAD_FORMAT, "patch refuses non-matching bytes");
}

// ---- SSE4a -------------------------------------------------------------------------------------------------

static std::vector<uint8_t> enc_sse4a(Sse4aOp op, int d, int s, int len, int idx) {
    std::vector<uint8_t> b;
    const bool ins = op == Sse4aOp::kInsertqImm || op == Sse4aOp::kInsertqReg;
    const bool imm = op == Sse4aOp::kInsertqImm || op == Sse4aOp::kExtrqImm;
    b.push_back(ins ? 0xF2 : 0x66);
    uint8_t rex = 0x40, modrm;
    if (op == Sse4aOp::kExtrqImm) {
        if (d >= 8) rex |= 1;
        modrm = static_cast<uint8_t>(0xC0 | (d & 7));
    } else {
        if (d >= 8) rex |= 4;
        if (s >= 8) rex |= 1;
        modrm = static_cast<uint8_t>(0xC0 | ((d & 7) << 3) | (s & 7));
    }
    if (rex != 0x40) b.push_back(rex);
    b.insert(b.end(), {0x0F, static_cast<uint8_t>(imm ? 0x78 : 0x79), modrm});
    if (imm) {
        b.push_back(static_cast<uint8_t>(len));
        b.push_back(static_cast<uint8_t>(idx));
    }
    return b;
}

static void sse4a_expect(const std::vector<uint8_t>& insn, const uint64_t in[16][4], uint64_t exp[16][4]) {
    Sse4aInsn d;
    sse4a_decode(insn.data(), insn.size(), &d);
    memcpy(exp, in, 16 * 32);
    uint64_t x[16][2];
    for (int r = 0; r < 16; ++r) {
        x[r][0] = in[r][0];
        x[r][1] = in[r][1];
    }
    // Independent of sse4a_execute: field semantics written out again.
    const uint64_t dv = x[d.dst][0], sv = x[d.src][0], sh = x[d.src][1];
    unsigned len, idx;
    uint64_t r;
    switch (d.op) {
    case Sse4aOp::kExtrqImm:
        len = d.len, idx = d.idx;
        goto extr;
    case Sse4aOp::kExtrqReg:
        len = sv & 63, idx = (sv >> 8) & 63;
    extr : {
        const unsigned fl = len ? len : 64;
        r = (dv >> idx) & (fl == 64 ? ~0ull : ((1ull << fl) - 1));
        break;
    }
    case Sse4aOp::kInsertqImm:
        len = d.len, idx = d.idx;
        goto ins;
    case Sse4aOp::kInsertqReg:
        len = sh & 63, idx = (sh >> 8) & 63;
    ins : {
        const unsigned fl = len ? len : 64;
        const uint64_t m = fl == 64 ? ~0ull : ((1ull << fl) - 1);
        r = (dv & ~(m << idx)) | ((sv & m) << idx);
        break;
    }
    default:
        r = dv;
    }
    exp[d.dst][0] = r;
}

static void fill_random(Rng& g, uint64_t in[16][4]) {
    for (int r = 0; r < 16; ++r)
        for (int k = 0; k < 4; ++k) in[r][k] = g.next();
}

static int run_and_compare(uint64_t code, const uint64_t in[16][4], const uint64_t exp[16][4], uint64_t out[16][4]) {
    memset(out, 0, 16 * 32);
    isa_test_run_ymm(in, out, code);
    int bad = 0;
    for (int r = 0; r < 16; ++r)
        for (int k = 0; k < 4; ++k)
            if (out[r][k] != exp[r][k]) ++bad;
    return bad;
}

static double time_loop(uint64_t code, int n) {
    uint64_t in[16][4] = {}, out[16][4];
    const uint64_t t0 = now_ns();
    isa_test_run_ymm(in, out, code);
    return double(now_ns() - t0) / n;
}

static std::vector<uint8_t> loop_code(const std::vector<uint8_t>& insn, int n, size_t* insn_off) {
    std::vector<uint8_t> c = {0xB9};
    for (int k = 0; k < 4; ++k) c.push_back(static_cast<uint8_t>(n >> (8 * k)));
    *insn_off = c.size();
    c.insert(c.end(), insn.begin(), insn.end());
    c.insert(c.end(), {0xFF, 0xC9});                       // dec ecx
    const int back = -static_cast<int>(insn.size() + 2 + 2);  // to the insn
    c.insert(c.end(), {0x75, static_cast<uint8_t>(back)});    // jnz
    c.push_back(0xC3);
    return c;
}

static void test_sse4a() {
    Rng g(42);
    uint64_t in[16][4], exp[16][4], out[16][4];
    int cases = 0, bad_trap = 0, bad_patch = 0, patched = 0;
    const uint64_t traps0 = gta6rt_isa_trap_count();
    const Sse4aOp ops[] = {Sse4aOp::kInsertqImm, Sse4aOp::kExtrqImm, Sse4aOp::kInsertqReg, Sse4aOp::kExtrqReg};
    for (int i = 0; i < 1200; ++i) {
        const Sse4aOp op = ops[i % 4];
        const int d = g.next() & 15, s = g.next() & 15;
        const int len = (i % 16 == 0) ? 0 : static_cast<int>(g.next() & 63);
        int idx = static_cast<int>(g.next() & 63);
        if (i % 3 == 0 && len) idx = static_cast<int>(g.next() % (65 - len));  // most cases inside the defined range
        auto insn = enc_sse4a(op, d, s, len, idx);
        fill_random(g, in);
        sse4a_expect(insn, in, exp);
        auto code = insn;
        code.push_back(0xC3);
        const uint64_t a = put_code(code);
        if (run_and_compare(a, in, exp, out)) ++bad_trap;
        if (op == Sse4aOp::kInsertqImm || op == Sse4aOp::kExtrqImm) {
            std::vector<uint8_t> pad = insn;
            pad.push_back(0xC3);
            const uint64_t b = put_code(pad);
            if (gta6rt_isa_patch_sse4a_site(b) == GTA6RT_OK) {
                ++patched;
                if (run_and_compare(b, in, exp, out)) ++bad_patch;
            }
        } else {
            CHECK(gta6rt_isa_patch_sse4a_site(a) == GTA6RT_E_UNSUPPORTED, "register form left to the SIGILL path");
        }
        ++cases;
    }
    const uint64_t traps = gta6rt_isa_trap_count() - traps0;
    std::printf("sse4a random: %d cases (all 4 forms) via SIGILL, %d imm cases also patched; mismatches trap=%d patch=%d; "
                "traps counted=%llu; arena write faults retried after SIGILL on the page: %llu\n",
                cases, patched, bad_trap, bad_patch, (unsigned long long)traps,
                (unsigned long long)arena_fault_retries());
    CHECK(bad_trap == 0 && bad_patch == 0 && patched == 600 && traps == static_cast<uint64_t>(cases),
          "sse4a vs reference");

    int site_cases = 0, site_bad = 0;
    for (const TitleSite& s : kTitleSites) {
        if (s.kind != 2) continue;
        std::vector<uint8_t> insn(s.bytes, s.bytes + s.len);
        for (int k = 0; k < 50; ++k) {
            fill_random(g, in);
            sse4a_expect(insn, in, exp);
            auto code = insn;
            code.push_back(0xC3);
            const uint64_t a = put_code(code), b = put_code(code);
            site_bad += run_and_compare(a, in, exp, out) != 0;
            CHECK(gta6rt_isa_patch_sse4a_site(b) == GTA6RT_OK, "patch title encoding 0x%x", s.rva);
            site_bad += run_and_compare(b, in, exp, out) != 0;
            site_cases += 2;
        }
    }
    std::printf("sse4a title encodings: 19 sites x 50 inputs x {SIGILL, patched}: %d runs, %d mismatches\n", site_cases,
                site_bad);
    CHECK(site_bad == 0 && site_cases == 19 * 100, "title sse4a encodings");

    const std::vector<uint8_t> ins = {0xF2, 0x0F, 0x78, 0xE3, 0x10, 0x10};  // insertq xmm4, xmm3, 16, 16 (title site)
    const std::vector<uint8_t> nop6 = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
    size_t off;
    const uint64_t lnop = put_code(loop_code(nop6, 1000000, &off));
    const uint64_t ltrap = put_code(loop_code(ins, 20000, &off));
    const uint64_t lpatch = put_code(loop_code(ins, 1000000, &off));
    CHECK(gta6rt_isa_patch_sse4a_site(lpatch + off) == GTA6RT_OK, "patch timing loop");
    const double tn = time_loop(lnop, 1000000), tt = time_loop(ltrap, 20000), tp = time_loop(lpatch, 1000000);
    std::printf("timing sse4a: loop with 6-byte nop %.2f ns/iter, SIGILL emulation %.0f ns/iter, patched stub %.2f ns/iter\n",
                tn, tt, tp);
}

// ---- vpsrlvq / vpsllvq ----------------------------------------------------------------------------------------

static const uint64_t kCounts[] = {0,           1,           31,          32,          63,
                                   64,          65,          127,         0x100000000, 0x100000001,
                                   0x80000000000, 0x800000000003, 0x8000000000000000ull, ~0ull,
                                   ~0ull - 254, 0x10000003f, 0xffffffff, 0x100000040, 7};

static uint64_t ref_shift(bool left, uint64_t v, uint64_t c) {
    if (c > 63) return 0;
    return left ? v << c : v >> c;
}

static uint64_t rosetta_model(bool left, uint64_t v, uint64_t c) {
    return ref_shift(left, v, c & 0xffffffffull);
}

struct VsCase {
    std::vector<uint8_t> code;
    size_t site_off;
    int dst, src, cnt_reg;  // cnt_reg -1: count from mem_count
    bool left, ymm;
    const uint64_t* mem_count;
};

static void vs_expect(const VsCase& c, const uint64_t in[16][4], const uint64_t cnt[4], bool model, uint64_t exp[16][4]) {
    memcpy(exp, in, 16 * 32);
    const int lanes = c.ymm ? 4 : 2;
    uint64_t r[4] = {0, 0, 0, 0};
    for (int k = 0; k < lanes; ++k)
        r[k] = model ? rosetta_model(c.left, in[c.src][k], cnt[k]) : ref_shift(c.left, in[c.src][k], cnt[k]);
    memcpy(exp[c.dst], r, 32);
}

static void test_vshift() {
    Rng g(7);
    uint64_t in[16][4], exp[16][4], out[16][4];
    alignas(32) static uint64_t memcnt[4];
    std::vector<VsCase> cases;
    for (const TitleSite& s : kTitleSites) {
        if (s.kind != 3) continue;
        VexInsn v;
        vex_decode(s.bytes, s.len, 0, &v);
        VsCase c;
        c.code.assign(s.bytes, s.bytes + s.len);
        c.code.push_back(0xC3);
        c.site_off = 0;
        c.dst = v.reg;
        c.src = v.vvvv;
        c.cnt_reg = v.rm;
        c.left = v.opcode == 0x47;
        c.ymm = v.l;
        c.mem_count = nullptr;
        cases.push_back(c);
    }
    for (int i = 0; i < 24; ++i) {
        X86Emitter e(0);
        VsCase c;
        c.dst = g.next() & 15;
        c.src = g.next() & 15;
        c.cnt_reg = g.next() & 15;
        c.left = i & 1;
        c.ymm = (i >> 1) & 1;
        e.vex_rr(2, 1, 1, c.ymm, c.left ? 0x47 : 0x45, c.dst, c.src, c.cnt_reg);
        c.code = e.bytes();
        c.code.push_back(0xC3);
        c.site_off = 0;
        c.mem_count = nullptr;
        cases.push_back(c);
    }
    {
        // vpsrlvq ymm1, ymm2, [rsp+0x10] after spilling ymm15 there: exercises the rsp displacement fix-up.
        VsCase c;
        c.code = {0x48, 0x8D, 0x64, 0x24, 0xC0};  // lea rsp, [rsp-0x40]
        X86Emitter e(0);
        X86Mem m;
        m.base = 4;
        m.disp = 0x10;
        e.vex_rm(1, 2, 0, 1, 0x7F, 15, 0, m);  // vmovdqu [rsp+0x10], ymm15
        c.code.insert(c.code.end(), e.bytes().begin(), e.bytes().end());
        c.site_off = c.code.size();
        X86Emitter e2(0);
        e2.vex_rm(2, 1, 1, 1, 0x45, 1, 2, m);
        c.code.insert(c.code.end(), e2.bytes().begin(), e2.bytes().end());
        c.code.insert(c.code.end(), {0x48, 0x8D, 0x64, 0x24, 0x40, 0xC3});
        c.dst = 1;
        c.src = 2;
        c.cnt_reg = 15;
        c.left = false;
        c.ymm = true;
        c.mem_count = nullptr;
        cases.push_back(c);
    }
    {
        // vpsllvq xmm3, xmm4, [rdi+0x200]: count from memory through a register base.
        VsCase c;
        X86Emitter e(0);
        X86Mem m;
        m.base = 7;
        m.disp = 0x200;
        e.vex_rm(2, 1, 1, 0, 0x47, 3, 4, m);
        c.code = e.bytes();
        c.code.push_back(0xC3);
        c.site_off = 0;
        c.dst = 3;
        c.src = 4;
        c.cnt_reg = -2;
        c.left = true;
        c.ymm = false;
        c.mem_count = nullptr;
        cases.push_back(c);
    }
    {
        // vpsrlvq ymm5, ymm6, [rip+...] with the count vector stored after the code.
        VsCase c;
        c.dst = 5;
        c.src = 6;
        c.cnt_reg = -3;
        c.left = false;
        c.ymm = true;
        c.mem_count = memcnt;
        c.site_off = 0;
        cases.push_back(c);
    }

    int raw_bad_ref = 0, raw_bad_model = 0, patched_bad = 0, runs = 0, triggering = 0;
    alignas(32) uint64_t inx[16 * 4 + 4];
    for (VsCase& c : cases) {
        for (int k = 0; k < 40; ++k) {
            fill_random(g, in);
            uint64_t cnt[4];
            for (int l = 0; l < 4; ++l) cnt[l] = (g.next() & 1) ? kCounts[g.next() % (sizeof kCounts / 8)] : (g.next() & 127);
            uint64_t code_raw, code_fix;
            if (c.cnt_reg >= 0) {
                memcpy(in[c.cnt_reg], cnt, 32);
                if (c.cnt_reg == c.src) memcpy(cnt, in[c.src], 32);
            }
            memcpy(&inx[0], in, sizeof in);
            memcpy(&inx[64], cnt, 32);
            if (c.cnt_reg == -3) {
                // rip-relative: emit insn + ret, then the data, with disp computed per placement.
                std::vector<uint8_t> probe(64, 0);
                const uint64_t base_raw = arena_reserve(code_near, 128, nullptr);
                const uint64_t base_fix = arena_reserve(code_near, 128, nullptr);
                for (uint64_t base : {base_raw, base_fix}) {
                    X86Emitter e(base);
                    X86Mem m;
                    m.rip = true;
                    m.rip_target = base + 64;
                    e.vex_rm(2, 1, 1, 1, 0x45, c.dst, c.src, m);
                    std::vector<uint8_t> code = e.bytes();
                    code.push_back(0xC3);
                    code.resize(64, 0xCC);
                    for (int l = 0; l < 4; ++l)
                        for (int b = 0; b < 8; ++b) code.push_back(static_cast<uint8_t>(cnt[l] >> (8 * b)));
                    arena_write(base, code);
                }
                code_raw = base_raw;
                code_fix = base_fix;
            } else {
                code_raw = put_code(c.code);
                code_fix = put_code(c.code);
            }
            CHECK(gta6rt_isa_patch_vshift_site(code_fix + c.site_off) == GTA6RT_OK, "patch vshift case");
            uint64_t exp_model[16][4];
            vs_expect(c, in, cnt, true, exp_model);
            vs_expect(c, in, cnt, false, exp);
            const bool trig = memcmp(exp, exp_model, sizeof exp) != 0;
            triggering += trig;
            uint64_t outx[16][4];
            auto run = [&](uint64_t code) {
                isa_test_run_ymm(reinterpret_cast<const uint64_t(*)[4]>(inx), outx, code);
                int bad = 0;
                for (int r = 0; r < 16; ++r)
                    for (int l = 0; l < 4; ++l) bad += outx[r][l] != exp[r][l];
                return bad;
            };
            patched_bad += run(code_fix) != 0;
            const bool raw_wrong = run(code_raw) != 0;
            raw_bad_ref += raw_wrong;
            vs_expect(c, in, cnt, true, exp);
            raw_bad_model += run(code_raw) != 0;
            CHECK(raw_wrong == trig, "raw result wrong exactly when the low-32-bit model differs from x86 (run %d)", runs);
            ++runs;
        }
    }
    (void)out;
    std::printf("vshift: %d runs (%zu encodings: 7 title patch sites, 24 generic, [rsp], [rdi], [rip]); %d had a "
                "triggering lane; raw Rosetta wrong in %d (vs low-32-bit model: %d off); patched wrong in %d\n",
                runs, cases.size(), triggering, raw_bad_ref, raw_bad_model, patched_bad);
    CHECK(patched_bad == 0, "patched vshift matches x86 semantics");
    CHECK(raw_bad_model == 0, "raw Rosetta matches the low-32-bit-count model");

    X86Emitter e(0);
    e.vex_rr(2, 1, 1, 1, 0x45, 9, 9, 11);  // vpsrlvq ymm9, ymm9, ymm11 (title's most common form)
    const std::vector<uint8_t> ins = e.bytes();
    const std::vector<uint8_t> nop5 = {0x0F, 0x1F, 0x44, 0x00, 0x00};
    size_t off;
    const uint64_t lnop = put_code(loop_code(nop5, 1000000, &off));
    const uint64_t lraw = put_code(loop_code(ins, 1000000, &off));
    const uint64_t lfix = put_code(loop_code(ins, 1000000, &off));
    CHECK(gta6rt_isa_patch_vshift_site(lfix + off) == GTA6RT_OK, "patch timing loop");
    std::printf("timing vshift: loop with 5-byte nop %.2f ns/iter, raw vpsrlvq %.2f ns/iter, patched %.2f ns/iter\n",
                time_loop(lnop, 1000000), time_loop(lraw, 1000000), time_loop(lfix, 1000000));
}

// ---- cpuid / xgetbv traps -------------------------------------------------------------------------------------

static void test_cpuid() {
    const uint64_t cpuid_code = put_code({0x0F, 0xA2, 0xC3});
    const uint64_t raw_code = put_code({0x0F, 0xA2, 0xC3});
    const uint64_t xgetbv_code = put_code({0x0F, 0x01, 0xD0, 0xC3});
    CHECK(gta6rt_isa_patch_cpuid_site(cpuid_code) == GTA6RT_OK, "trap cpuid");
    CHECK(gta6rt_isa_patch_cpuid_site(xgetbv_code) == GTA6RT_OK, "trap xgetbv");
    for (uint32_t leaf : {0u, 1u, 7u, 0xDu, 0x80000001u}) {
        uint64_t r[4] = {leaf, 0x5555, 0, 0x6666};
        isa_test_run_gpr(r, cpuid_code);
        gta6rt_cpuid_regs p;
        gta6rt_isa_cpuid(leaf, 0, &p);
        CHECK(r[0] == p.eax && r[1] == p.ebx && r[2] == p.ecx && r[3] == p.edx, "trapped cpuid leaf %x = policy", leaf);
        uint64_t q[4] = {leaf, 0, 0, 0};
        isa_test_run_gpr(q, raw_code);
        std::printf("cpuid %08x: raw Rosetta eax=%08llx ebx=%08llx ecx=%08llx edx=%08llx | policy eax=%08x ebx=%08x "
                    "ecx=%08x edx=%08x\n",
                    leaf, (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                    (unsigned long long)q[3], p.eax, p.ebx, p.ecx, p.edx);
    }
    uint64_t r[4] = {0, 0, 0, 0};
    isa_test_run_gpr(r, xgetbv_code);
    CHECK(r[0] == 7 && r[3] == 0, "trapped xgetbv(0) = 7");
    const char* env = getenv("ROSETTA_ADVERTISE_AVX");
    const bool adv = env && !strcmp(env, "1");
    const uint64_t a = gta6rt_isa_advertised_features();
    std::printf("ROSETTA_ADVERTISE_AVX=%s; host features 0x%llx; advertised by Rosetta 0x%llx (policy 0x%llx)\n",
                env ? env : "(unset)", (unsigned long long)gta6rt_isa_host_features(), (unsigned long long)a,
                (unsigned long long)GTA6RT_CPUID_DEFAULT);
    if (adv)
        CHECK(a == GTA6RT_CPUID_DEFAULT && gta6rt_isa_cpuid_enforced() == 1, "advertised == policy with the variable set");
    else
        CHECK(!(a & GTA6RT_CPUID_AVX) && gta6rt_isa_cpuid_enforced() == 0, "no AVX advertised without the variable");
}

// ---- fixups executed in place on a copy of the exe --------------------------------------------------------------

// ---- code writes while another thread executes on the same page ------------------------------------------

static std::atomic<bool> g_cw_stop{false};
static std::atomic<uint64_t> g_cw_calls{0}, g_cw_bad{0};
static const uint8_t kCwFn[6] = {0xB8, 0x67, 0x45, 0x23, 0x01, 0xC3};  // mov eax, 0x1234567; ret

static void* cw_runner(void* p) {
    auto fn = reinterpret_cast<uint64_t (*)()>(p);
    while (!g_cw_stop.load(std::memory_order_relaxed)) {
        if (fn() != 0x1234567) g_cw_bad.fetch_add(1, std::memory_order_relaxed);
        g_cw_calls.fetch_add(1, std::memory_order_relaxed);
    }
    return nullptr;
}

struct CwResult {
    uint64_t calls, writes, ns;
};

template <class W>
static CwResult cw_phase(uint64_t fn, uint64_t ms, W&& write_one) {
    g_cw_stop = false;
    g_cw_calls = 0;
    pthread_t t;
    pthread_create(&t, nullptr, cw_runner, reinterpret_cast<void*>(fn));
    while (g_cw_calls.load() < 1000) {
    }
    uint64_t writes = 0;
    const uint64_t t0 = now_ns(), c0 = g_cw_calls.load();
    while (now_ns() - t0 < ms * 1000000) write_one(writes++);
    const uint64_t dt = now_ns() - t0, calls = g_cw_calls.load() - c0;
    g_cw_stop = true;
    pthread_join(t, nullptr);
    return CwResult{calls, writes, dt};
}

// Runs one thread executing kCwFn on a fresh RX page while this thread rewrites other bytes of that page, in a
// forked child for 3 s. Returns the child's wait status.
static int cw_fork_trial(bool old_path) {
    const pid_t pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        const uint64_t pg = static_cast<uint64_t>(getpagesize());
        void* q = mmap(nullptr, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        memcpy(q, kCwFn, sizeof kCwFn);
        mprotect(q, pg, PROT_READ | PROT_EXEC);
        const uint64_t qa = reinterpret_cast<uint64_t>(q);
        uint8_t fill[64];
        memset(fill, 0xCC, sizeof fill);
        cw_phase(qa, 3000, [&](uint64_t i) {
            if (old_path) {
                mprotect(q, pg, PROT_READ | PROT_WRITE);
                memcpy(static_cast<uint8_t*>(q) + 256 + (i % 48) * 64, fill, sizeof fill);
                mprotect(q, pg, PROT_READ | PROT_EXEC);
            } else {
                gta6rt_isa_patch_code(qa + 256 + (i % 48) * 64, fill, sizeof fill);
            }
        });
        _exit(0);
    }
    int ws = 0;
    waitpid(pid, &ws, 0);
    return ws;
}

static void test_concurrent_code_writes() {
    const uint64_t pg = static_cast<uint64_t>(getpagesize());
    const uint64_t probe = arena_reserve(code_near, 16, nullptr);
    CHECK(probe != 0, "arena");
    if (const uint64_t off = (probe + 16) & (pg - 1)) arena_reserve(code_near, pg - off, nullptr);
    const uint64_t page = arena_reserve(code_near, 16, nullptr);
    const uint64_t scratch = arena_reserve(code_near, 1024, nullptr);
    CHECK((page & (pg - 1)) == 0 && scratch == page + 16, "hot page layout %llx %llx", (unsigned long long)page,
          (unsigned long long)scratch);
    CHECK(arena_write(page, kCwFn, sizeof kCwFn), "hot fn in arena");
    uint64_t nthunks = 0, same_page = 0;
    std::vector<gta6rt_gva> thunks;
    uint8_t fill[64];
    memset(fill, 0xCC, sizeof fill);
    uint64_t write_fail = 0, thunk_fail = 0;
    const CwResult base = cw_phase(page, 300, [](uint64_t) {});
    g_cw_bad = 0;
    const CwResult a = cw_phase(page, 1500, [&](uint64_t i) {
        write_fail += !arena_write(scratch + (i % 15) * 64, fill, sizeof fill);
        if (i % 64 == 0 && nthunks < 2000) {
            gta6rt_thunk_desc d{GTA6RT_THUNK_NATIVE, reinterpret_cast<const void*>(&native6), nullptr, "test!cw"};
            gta6rt_gva th = 0;
            if (gta6rt_isa_make_thunk(&d, &th) == GTA6RT_OK) {
                thunks.push_back(th);
                same_page += (th & ~(pg - 1)) == page;
            }
            else
                ++thunk_fail;
            ++nthunks;
        }
    });
    std::printf("concurrent arena writes (%s): %llu writes + %llu thunks in %.1f s while another thread made %llu calls "
                "on the page (%.1f M calls/s, %.1f M/s with no writer), %llu wrong results; %llu thunks on the hot page\n",
                arena_is_rwx() ? "RWX arena" : "RW alias", (unsigned long long)a.writes, (unsigned long long)nthunks,
                a.ns / 1e9, (unsigned long long)a.calls, a.calls * 1e3 / a.ns, base.calls * 1e3 / base.ns,
                (unsigned long long)g_cw_bad.load(), (unsigned long long)same_page);
    CHECK(same_page > 0, "some thunks were written into the page another thread was executing");
    CHECK(g_cw_bad.load() == 0 && a.calls > 0 && a.writes > 1000 && write_fail == 0 && thunk_fail == 0,
          "arena writes and make_thunk vs live code (%llu/%llu failed)", (unsigned long long)write_fail,
          (unsigned long long)thunk_fail);
    int thunk_bad = 0;
    for (gta6rt_gva th : thunks) thunk_bad += call(G(guest_call6), {th, 1, 2, 3, 4, 5, 6}) != 91;
    CHECK(thunk_bad == 0 && thunks.size() == nthunks, "%d of %zu concurrently made thunks wrong", thunk_bad, thunks.size());

    const int ws_patch = cw_fork_trial(false), ws_old = cw_fork_trial(true);
    auto desc = [](int ws, char* buf, size_t n) {
        if (WIFSIGNALED(ws))
            snprintf(buf, n, "killed by signal %d", WTERMSIG(ws));
        else
            snprintf(buf, n, "survived 3 s (exit %d)", WEXITSTATUS(ws));
        return buf;
    };
    char b1[64], b2[64];
    std::printf("live non-arena page, another thread executing on it: gta6rt_isa_patch_code (RX->RWX->RX) child %s; "
                "old code_write (RX->RW->RX) child %s\n",
                desc(ws_patch, b1, sizeof b1), desc(ws_old, b2, sizeof b2));
    CHECK(WIFSIGNALED(ws_old) && (WTERMSIG(ws_old) == SIGSEGV || WTERMSIG(ws_old) == SIGBUS),
          "control: the old write path crashes a thread running code on the page");
}

static void test_image(const char* exe) {
    MappedImage img;
    if (!map_pe(exe, &img)) {
        CHECK(false, "map exe");
        return;
    }
    const uint64_t base = reinterpret_cast<uint64_t>(img.base);
    gta6rt_isa_fixup_report rep{};
    CHECK(gta6rt_isa_apply_image_fixups(base, img.size, GTA6RT_ISA_FIXUP_ALL, &rep) == GTA6RT_OK && rep.known_image &&
              rep.sse4a == 19 && rep.vshift == 7 && rep.cpuid == 9 && rep.xgetbv == 2 && rep.chkstk == 1,
          "fixups on image copy");
    CHECK(mprotect(img.base + img.text_rva, (img.text_size + 0xfff) & ~0xfffu, PROT_READ | PROT_EXEC) == 0, "text RX");
    CHECK(gta6rt_isa_cpuid_enforced() == 1, "cpuid enforced after trapping the title's sites");
    Rng g(99);
    uint64_t in[16][4], exp[16][4], out[16][4];
    int sse_bad = 0, vs_bad = 0, cpuid_bad = 0, n_sse = 0, n_vs = 0, n_cpuid = 0;
    for (const TitleSite& s : kTitleSites) {
        const uint64_t site = base + s.rva;
        const uint8_t ret = 0xC3;
        if (s.kind == 2 || s.kind == 3) {
            CHECK(gta6rt_isa_patch_code(site + s.len, &ret, 1) == GTA6RT_OK, "ret after site");
            std::vector<uint8_t> orig(s.bytes, s.bytes + s.len);
            for (int k = 0; k < 20; ++k) {
                fill_random(g, in);
                if (s.kind == 2) {
                    sse4a_expect(orig, in, exp);
                    sse_bad += run_and_compare(site, in, exp, out) != 0;
                    ++n_sse;
                } else {
                    VexInsn v;
                    vex_decode(s.bytes, s.len, 0, &v);
                    for (int l = 0; l < 4; ++l)
                        in[v.rm][l] = (g.next() & 1) ? kCounts[g.next() % (sizeof kCounts / 8)] : (g.next() & 127);
                    VsCase c;
                    c.dst = v.reg;
                    c.src = v.vvvv;
                    c.cnt_reg = v.rm;
                    c.left = v.opcode == 0x47;
                    c.ymm = v.l;
                    vs_expect(c, in, in[v.rm], false, exp);
                    vs_bad += run_and_compare(site, in, exp, out) != 0;
                    ++n_vs;
                }
            }
        } else if (s.kind == 5) {
            CHECK(gta6rt_isa_patch_code(site + s.len, &ret, 1) == GTA6RT_OK, "ret after cpuid");
            for (uint32_t leaf : {0u, 1u, 7u}) {
                uint64_t r[4] = {leaf, 0, 0, 0};
                isa_test_run_gpr(r, site);
                gta6rt_cpuid_regs p;
                gta6rt_isa_cpuid(leaf, 0, &p);
                cpuid_bad += !(r[0] == p.eax && r[1] == p.ebx && r[2] == p.ecx && r[3] == p.edx);
                ++n_cpuid;
            }
        }
    }
    std::printf("image copy at %p: executed 19 patched insertq sites x20 (%d bad), 7 patched vshift sites x20 (%d bad), "
                "9 trapped cpuid sites x3 (%d bad)\n",
                img.base, sse_bad, vs_bad, cpuid_bad);
    CHECK(sse_bad == 0 && n_sse == 19 * 20, "title insertq sites in place");
    CHECK(vs_bad == 0 && n_vs == 7 * 20, "title vshift sites in place");
    CHECK(cpuid_bad == 0 && n_cpuid == 27, "title cpuid sites in place");
    munmap(img.base, img.size);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    code_near = reinterpret_cast<uint64_t>(&test_sse4a);
    int tr = 0;
    size_t n = sizeof tr;
    sysctlbyname("sysctl.proc_translated", &tr, &n, nullptr, 0);
    std::printf("isa_rosetta_test: proc_translated=%d backend=%s\n", tr, gta6rt_isa_backend_name());
    gta6rt_isa_config cfg{GTA6RT_TEB_SHARE, 0};
    CHECK(gta6rt_isa_init(&cfg) == GTA6RT_OK, "init");
    const char* exe = exe_path(argc, argv);
    test_share();
    test_calls();
    make_thunks();
    test_thunks("SHARE");
    bench_thunks("SHARE");
    test_faults();
    pthread_t sw;
    pthread_create(&sw, nullptr, swap_worker, nullptr);
    pthread_join(sw, nullptr);
    test_foreign_hook();
    test_chkstk(exe);
    test_sse4a();
    test_vshift();
    test_cpuid();
    test_concurrent_code_writes();
    test_image(exe);
    return finish("isa_rosetta_test");
}
