// isa/fexcore tests: guest x86-64 code (fex_guest.S, copied into non-executable memory) runs through the fexcore
// backend's public interface in this arm64 process. --bench adds the throughput table.

#include <gta6rt/isa.h>
#include <gta6rt/nt_layout.h>
#include <gta6rt/os.h>

#include "../../tests/test_common.h"
#include "fex_guest_blob.h"
#include "isa_internal.h"
#include "sse4a.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <climits>
#include <cfenv>
#include <cmath>
#include <ctime>
#include <pthread.h>
#include <sys/sysctl.h>

using namespace gta6rt::isa;

namespace {

uint8_t* g_blob = nullptr;
constexpr size_t kBlobRegion = 1u << 20;
constexpr size_t kScratch = 0x80000;

gta6rt_gva fn(size_t off) {
    return reinterpret_cast<gta6rt_gva>(g_blob + off);
}

uint64_t bits(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

double dbl(uint64_t u) {
    double d;
    memcpy(&d, &u, 8);
    return d;
}

uint64_t now_ns() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

gta6rt_guest_ret call(gta6rt_gva f, std::initializer_list<uint64_t> a, uint32_t float_mask = 0) {
    gta6rt_guest_args g{};
    for (uint64_t v : a) g.args[g.count++] = v;
    g.float_mask = float_mask;
    gta6rt_guest_ret r{};
    const gta6rt_status st = gta6rt_isa_call_guest(f, &g, &r);
    CHECK(st == GTA6RT_OK, "call_guest status %d", st);
    return r;
}

// ---- thunk handlers ------------------------------------------------------------------------------------------

void h_six(gta6rt_guest_frame* f, void* user) {
    const auto* st = reinterpret_cast<const uint64_t*>(f->rsp + 0x28);
    f->rax = f->rcx + 10 * f->rdx + 100 * f->r8 + 1000 * f->r9 + 10000 * st[0] + 100000 * st[1] +
             reinterpret_cast<uintptr_t>(user);
}

void h_float(gta6rt_guest_frame* f, void*) {
    const double r = (dbl(f->xmm0) + double(f->rdx) + dbl(f->xmm2) + double(f->r9)) * 2.0;
    f->xmm0_ret = bits(r);
    f->ret_is_float = 1;
}

gta6rt_gva g_reenter_thunk = 0;
int g_reenter_depth = 0;

void h_reenter(gta6rt_guest_frame* f, void*) {
    if (f->rcx > 0 && g_reenter_depth < 12) {
        ++g_reenter_depth;
        const gta6rt_guest_ret r = call(fn(kOff_call1), {g_reenter_thunk, f->rcx - 1});
        --g_reenter_depth;
        f->rax = r.rax + 1;
        return;
    }
    gta6rt_guest_args a{};
    a.args[0] = 1;
    a.args[1] = 2;
    a.args[2] = 3;
    a.count = 3;
    gta6rt_guest_ret r{};
    gta6rt_isa_call_guest(fn(kOff_add3), &a, &r);
    f->rax = r.rax;
}

void h_count(gta6rt_guest_frame* f, void*) {
    f->rax = f->rcx & 1;
}

std::atomic<int> g_trap_calls{0};
void h_trap(gta6rt_guest_frame* f, void*) {
    g_trap_calls.fetch_add(1);
    f->rax = 0x7a7;
}

// ---- fault handler -------------------------------------------------------------------------------------------

struct FaultLog {
    uint32_t code = 0;
    uint64_t address = 0, p0 = 0, p1 = 0, rip = 0;
    uint32_t n = 0;
    uint64_t resume_at = 0;
    gta6rt_status capture = GTA6RT_OK;
};
thread_local FaultLog t_fault;

gta6rt_fault_action on_fault(gta6rt_x64_exception_record* rec, gta6rt_x64_context* ctx, void*) {
    FaultLog& l = t_fault;
    l.code = rec->code;
    l.address = rec->address;
    l.p0 = rec->params[0];
    l.p1 = rec->params[1];
    l.rip = ctx->rip;
    ++l.n;
    gta6rt_x64_context tmp;
    l.capture = gta6rt_isa_capture_guest_context(&tmp);
    if (!l.resume_at) return GTA6RT_FAULT_CONTINUE_SEARCH;
    ctx->rip = l.resume_at;
    ctx->rax = 0x600D;
    l.resume_at = 0;
    return GTA6RT_FAULT_RESUME;
}

void h_fault_inside(gta6rt_guest_frame* f, void*) {
    t_fault = {};
    t_fault.resume_at = fn(kOff_fault_read_site) + 3;
    const gta6rt_guest_ret r = call(fn(kOff_fault_read), {8});
    f->rax = r.rax == 0x600D && t_fault.code == 0xC0000005 && t_fault.capture == GTA6RT_E_NOT_FOUND ? 0x1234 : 0xBAD;
}


// ---- NATIVE thunks with a signature (ms_abi -> AAPCS64) --------------------------------------------------------

double nat_mixed(int32_t a, float b, double c, int64_t d, float e, uint8_t f, double g) {
    return a + b * 2 + c * 3 + double(d) * 4 + e * 5 + f * 6 + g * 7;
}
int64_t nat_ten(int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                int64_t a9, int64_t a10) {
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 + 10 * a10;
}
int64_t nat_small(int8_t a, uint16_t b, int16_t c, bool d, int32_t e, int8_t f, uint8_t g, int16_t h, int8_t i, uint8_t j) {
    return a + 3 * b + 5 * c + 7 * d + 11 * int64_t(e) + 13 * f + 17 * g + 19 * h + 23 * i + 29 * j;
}
struct S24 {
    int64_t a, b, c;
};
S24 nat_sret(int64_t x, int64_t y) {
    return {x + y, x - y, x * y};
}
struct S16 {
    int64_t a, b;
};
S16 nat_s16(int64_t x) {
    return {x * 3, ~x};
}
struct V2 {
    float x, y;
};
V2 nat_v2(float a, float b) {
    return {a + b, a * b};
}
struct V4 {
    float x, y, z, w;
};
V4 nat_v4(V4 v, float k) {
    return {v.x * k, v.y * k, v.z * k, v.w + k};
}
typedef float f32x4 __attribute__((vector_size(16)));
f32x4 nat_vec(f32x4 a, f32x4 b) {
    return a * b + a;
}
char g_var_buf[128];
int nat_var(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(g_var_buf, sizeof g_var_buf, fmt, ap);
    va_end(ap);
    return n;
}

gta6rt_gva native(const void* f, gta6rt_abi_arg ret, std::initializer_list<gta6rt_abi_arg> args, uint8_t fixed = GTA6RT_ABI_NOT_VARIADIC) {
    gta6rt_native_sig s{};
    s.ret = ret;
    s.fixed = fixed;
    for (const auto& a : args) s.args[s.count++] = a;
    gta6rt_thunk_desc d{};
    d.kind = GTA6RT_THUNK_NATIVE;
    d.fn = f;
    d.debug_name = "test!native";
    gta6rt_gva out = 0;
    CHECK(gta6rt_isa_make_native_thunk(&d, &s, &out) == GTA6RT_OK, "make_native_thunk");
    return out;
}

struct Win64Call {
    uint64_t in[32] = {};
    uint64_t out[3] = {};
    void reg(unsigned i, uint64_t v) { in[1 + i] = v; }
    void xmm(unsigned i, uint64_t v) { in[5 + i] = v; }
    void stack(std::initializer_list<uint64_t> v) {
        for (uint64_t x : v) in[10 + in[9]++] = x;
    }
    void run(gta6rt_gva target) {
        in[0] = target;
        call(fn(kOff_call_win64), {reinterpret_cast<uint64_t>(in), reinterpret_cast<uint64_t>(out)});
    }
};

uint64_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return 0xDEAD000000000000ull | u;
}

void test_native() {
    constexpr gta6rt_abi_arg I64{GTA6RT_ABI_I64, 0, 0}, I32{GTA6RT_ABI_I32, 0, 0}, F32{GTA6RT_ABI_F32, 0, 0},
        F64{GTA6RT_ABI_F64, 0, 0}, U8{GTA6RT_ABI_U8, 0, 0}, I8{GTA6RT_ABI_I8, 0, 0}, U16{GTA6RT_ABI_U16, 0, 0},
        I16{GTA6RT_ABI_I16, 0, 0}, M128{GTA6RT_ABI_M128, 0, 0};
    {
        Win64Call c;
        c.reg(0, 0xFFFFFFFF00000007ull);
        c.xmm(1, fbits(1.5f));
        c.xmm(2, bits(2.25));
        c.reg(3, static_cast<uint64_t>(-4));
        c.stack({fbits(0.5f), 0xABCDEF0000000009ull, bits(10.0)});
        c.run(native(reinterpret_cast<const void*>(&nat_mixed), F64, {I32, F32, F64, I64, F32, U8, F64}));
        const double want = nat_mixed(7, 1.5f, 2.25, -4, 0.5f, 9, 10.0);
        CHECK(dbl(c.out[1]) == want, "native mixed int/float, 7 args: %f want %f", dbl(c.out[1]), want);
    }
    {
        Win64Call c;
        for (unsigned i = 0; i < 4; ++i) c.reg(i, i + 1);
        c.stack({5, 6, 7, 8, 9, 10});
        c.run(native(reinterpret_cast<const void*>(&nat_ten), I64, {I64, I64, I64, I64, I64, I64, I64, I64, I64, I64}));
        CHECK(c.out[0] == static_cast<uint64_t>(nat_ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)), "native 10 integer args");
    }
    {
        Win64Call c;
        c.reg(0, 0xFFFFFF80);
        c.reg(1, 0xFFFF1234);
        c.reg(2, 0xFFFFFFFE);
        c.reg(3, 0x0101);
        c.stack({0xAAAAAAAA80000000ull, 0x1111117Full, 0x22222281ull, 0x3333FFF0ull, 0x444444FFull, 0x555555C8ull});
        c.run(native(reinterpret_cast<const void*>(&nat_small), I64, {I8, U16, I16, U8, I32, I8, U8, I16, I8, U8}));
        const int64_t want = nat_small(-128, 0x1234, -2, true, INT32_MIN, 0x7F, 0x81, -16, -1, 0xC8);
        CHECK(c.out[0] == static_cast<uint64_t>(want), "native sub-32-bit args with garbage upper bits, 2 packed on the stack: %lld want %lld",
              (long long)c.out[0], (long long)want);
    }
    {
        S24 r{};
        Win64Call c;
        c.reg(0, reinterpret_cast<uint64_t>(&r));
        c.reg(1, 6);
        c.reg(2, 4);
        c.run(native(reinterpret_cast<const void*>(&nat_sret), {GTA6RT_ABI_AGG, 0, 24}, {I64, I64}));
        CHECK(r.a == 10 && r.b == 2 && r.c == 24 && c.out[0] == reinterpret_cast<uint64_t>(&r),
              "native 24-byte struct return (Win64 hidden pointer -> AAPCS x8)");
    }
    {
        S16 r{};
        Win64Call c;
        c.reg(0, reinterpret_cast<uint64_t>(&r));
        c.reg(1, 5);
        c.run(native(reinterpret_cast<const void*>(&nat_s16), {GTA6RT_ABI_AGG, 0, 16}, {I64}));
        CHECK(r.a == 15 && r.b == ~5ll && c.out[0] == reinterpret_cast<uint64_t>(&r),
              "native 16-byte struct return (Win64 hidden pointer <- AAPCS x0/x1)");
    }
    {
        Win64Call c;
        c.xmm(0, fbits(3.0f));
        c.xmm(1, fbits(4.0f));
        c.run(native(reinterpret_cast<const void*>(&nat_v2), {GTA6RT_ABI_AGG, 2, 8}, {F32, F32}));
        V2 r;
        memcpy(&r, &c.out[0], 8);
        CHECK(r.x == 7.0f && r.y == 12.0f, "native 2-float struct return (Win64 rax <- AAPCS s0/s1)");
    }
    {
        V4 in{1, 2, 3, 4}, r{};
        Win64Call c;
        c.reg(0, reinterpret_cast<uint64_t>(&r));
        c.reg(1, reinterpret_cast<uint64_t>(&in));
        c.xmm(2, fbits(2.0f));
        c.run(native(reinterpret_cast<const void*>(&nat_v4), {GTA6RT_ABI_AGG, 4, 16}, {{GTA6RT_ABI_AGG, 4, 16}, F32}));
        CHECK(r.x == 2 && r.y == 4 && r.z == 6 && r.w == 6, "native HFA by value in and out (Win64 by reference)");
    }
    {
        alignas(16) f32x4 a = {1, 2, 3, 4}, b = {2, 2, 2, 2};
        Win64Call c;
        c.reg(0, reinterpret_cast<uint64_t>(&a));
        c.reg(1, reinterpret_cast<uint64_t>(&b));
        c.run(native(reinterpret_cast<const void*>(&nat_vec), M128, {M128, M128}));
        float r[4];
        memcpy(r, &c.out[1], 16);
        CHECK(r[0] == 3 && r[1] == 6 && r[2] == 9 && r[3] == 12, "native __m128 args (Win64 by pointer) and xmm0 return");
    }
    {
        static const char fmt[] = "%d|%.2f|%s|%lld";
        static const char word[] = "fex";
        Win64Call c;
        c.reg(0, reinterpret_cast<uint64_t>(fmt));
        c.reg(1, 0xFFFFFFFF0000002Aull);
        c.reg(2, bits(3.25));
        c.xmm(2, bits(3.25));
        c.reg(3, reinterpret_cast<uint64_t>(word));
        c.stack({static_cast<uint64_t>(-77)});
        c.run(native(reinterpret_cast<const void*>(&nat_var), I32, {I64, I32, F64, I64, I64}, 1));
        CHECK(!strcmp(g_var_buf, "42|3.25|fex|-77") && static_cast<int32_t>(c.out[0]) == 15,
              "native variadic call (Darwin: variadic args on the stack): \"%s\"", g_var_buf);
    }
}

// ---- tests ---------------------------------------------------------------------------------------------------

void test_calls() {
    gta6rt_guest_ret r = call(fn(kOff_sum16), {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    uint64_t want = 0;
    for (uint64_t i = 1; i <= 16; ++i) want += i * i;
    CHECK(r.rax == want, "sum16 got 0x%llx want 0x%llx (bit 63 = misaligned rsp)", (unsigned long long)r.rax,
          (unsigned long long)want);
    r = call(fn(kOff_fmix), {bits(1.25), 7, bits(2.5), static_cast<uint64_t>(-3), bits(10.0)}, 0x15);
    CHECK(dbl(r.xmm0_lo) == 1.25 * 2 + 7 + 2.5 * 3 - 3 + 10, "fmix got %f", dbl(r.xmm0_lo));
    CHECK(gta6rt_isa_call_guest4(fn(kOff_add3), 5, 6, 7, 0) == 5 + 18 + 7, "call_guest4");
    uint64_t rax = 0;
    CHECK(gta6rt_isa_run_entry(fn(kOff_add3), 1, 1, 1, &rax) == GTA6RT_OK && rax == 5, "run_entry");
}

void test_thunks() {
    gta6rt_thunk_desc d{};
    d.kind = GTA6RT_THUNK_FRAME;
    d.fn = reinterpret_cast<const void*>(&h_six);
    d.user = reinterpret_cast<void*>(7);
    d.debug_name = "test!six";
    gta6rt_gva six = 0;
    CHECK(gta6rt_isa_make_thunk(&d, &six) == GTA6RT_OK && six, "make FRAME thunk");
    gta6rt_guest_ret r = call(fn(kOff_call6), {six, 1, 2, 3, 4, 5});
    const uint64_t want = 1 + 20 + 300 + 4000 + 50000 + 100000 * 0x66 + 7;
    CHECK(r.rax == want, "6-arg thunk got 0x%llx want 0x%llx", (unsigned long long)r.rax, (unsigned long long)want);

    d.fn = reinterpret_cast<const void*>(&h_float);
    d.user = nullptr;
    gta6rt_gva fl = 0;
    CHECK(gta6rt_isa_make_thunk(&d, &fl) == GTA6RT_OK, "make float thunk");
    r = call(fn(kOff_call_float), {fl});
    CHECK(dbl(r.xmm0_lo) == (1.5 + 2 + 3.25 + 4) * 2 + 0.5, "float thunk got %f", dbl(r.xmm0_lo));

    d.fn = reinterpret_cast<const void*>(&h_reenter);
    CHECK(gta6rt_isa_make_thunk(&d, &g_reenter_thunk) == GTA6RT_OK, "make re-entry thunk");
    r = call(fn(kOff_call1), {g_reenter_thunk, 0});
    CHECK(r.rax == 1 + 6 + 3 + 1000, "guest->host->guest got %llu", (unsigned long long)r.rax);
    r = call(fn(kOff_call1), {g_reenter_thunk, 10});
    CHECK(r.rax == 10 + 1000 * 11 + 10, "10 nested re-entries got %llu", (unsigned long long)r.rax);

    d.kind = GTA6RT_THUNK_NATIVE;
    d.fn = reinterpret_cast<const void*>(&h_count);
    gta6rt_gva nat = 0;
    CHECK(gta6rt_isa_make_thunk(&d, &nat) == GTA6RT_E_UNSUPPORTED, "NATIVE thunk refused on arm64");

    d.kind = GTA6RT_THUNK_TRAP;
    d.fn = reinterpret_cast<const void*>(&h_trap);
    d.debug_name = "test.dll!Unresolved";
    gta6rt_gva trap = 0;
    CHECK(gta6rt_isa_make_thunk(&d, &trap) == GTA6RT_OK, "make TRAP thunk");
    r = call(fn(kOff_call1), {trap, 0});
    CHECK(r.rax == 0x7a7 + 1000 && g_trap_calls.load() == 1, "TRAP thunk ran its handler");
}

uint8_t* g_tls_block = nullptr;
void* tls_ctor(uint32_t, uint32_t tid, void*) {
    auto* b = static_cast<uint64_t*>(calloc(4, 8));
    b[0] = 0xC0FFEE00ull + tid;
    return b;
}
void tls_dtor(void* b, void*) {
    free(b);
}

bool probe_teb(uint32_t tid_expect) {
    uint64_t out[6] = {};
    const gta6rt_guest_ret r = call(fn(kOff_gs_probe), {reinterpret_cast<uint64_t>(out)});
    auto* teb = static_cast<uint8_t*>(gta6rt_isa_current_teb());
    const uint64_t base = teb_get(teb, GTA6RT_TEB_STACK_BASE), limit = teb_get(teb, GTA6RT_TEB_STACK_LIMIT);
    const bool ok = out[0] == reinterpret_cast<uint64_t>(teb) && out[1] == base && out[2] == limit &&
                    out[3] == teb_get(teb, GTA6RT_TEB_PEB) &&
                    out[4] == reinterpret_cast<uint64_t*>(teb_get(teb, GTA6RT_TEB_TLS_POINTER))[0] &&
                    *reinterpret_cast<uint64_t*>(out[4]) == 0xC0FFEE00ull + tid_expect && out[5] == 0 &&
                    r.rax > limit && r.rax < base;
    static std::atomic<int> reported{0};
    if (!ok && reported.fetch_add(1) == 0)
        std::printf("probe: teb %p gs30 %llx base %llx/%llx limit %llx/%llx peb %llx/%llx tls0 %llx want %llx gs0 %llx rsp %llx\n",
                    teb, (unsigned long long)out[0], (unsigned long long)out[1], (unsigned long long)base,
                    (unsigned long long)out[2], (unsigned long long)limit, (unsigned long long)out[3],
                    (unsigned long long)teb_get(teb, GTA6RT_TEB_PEB), (unsigned long long)*reinterpret_cast<uint64_t*>(out[4]),
                    (unsigned long long)(0xC0FFEE00ull + tid_expect), (unsigned long long)out[5], (unsigned long long)r.rax);
    return ok;
}

uint64_t host_add(uint64_t a, uint64_t b, uint64_t c) {
    return a * 100 + b * 10 + c;
}

void test_os_invoker() {
    const uint64_t args[3] = {1, 2, 3};
    CHECK(gta6rt_os_invoke(fn(kOff_add3), args, 3) == 1 + 6 + 3, "os invoker routes a guest address through call_guest");
    CHECK(gta6rt_os_invoke(reinterpret_cast<gta6rt_gva>(&host_add), args, 3) == 123, "os invoker calls a host function directly");
}

void test_teb() {
    auto* teb = static_cast<uint8_t*>(gta6rt_isa_current_teb());
    const uint32_t tid = static_cast<uint32_t>(teb_get(teb, GTA6RT_TEB_CLIENT_ID_THREAD));
    CHECK(probe_teb(tid), "gs:[0x30]/[0x08]/[0x10]/[0x60]/[0x58]->[0]/[0x00] on the main thread");
    const uint64_t base = teb_get(teb, GTA6RT_TEB_STACK_BASE);
    CHECK(base == reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(pthread_self())),
          "main thread (8 MiB stack): TEB keeps the pthread bounds");
    CHECK(gta6rt_isa_thread_teb_mode() == GTA6RT_TEB_SWAP, "teb mode reports the GS-is-TEB model");
}

struct ThreadArg {
    size_t stack;
    bool ok = false;
    bool pthread_bounds = false;
    uint64_t bench = 0;
};

void* thread_main(void* p) {
    auto* a = static_cast<ThreadArg*>(p);
    gta6rt_teb_desc d{};
    void* teb = nullptr;
    if (gta6rt_isa_thread_attach(&d, &teb) != GTA6RT_OK) return nullptr;
    const uint32_t tid = static_cast<uint32_t>(teb_get(static_cast<uint8_t*>(teb), GTA6RT_TEB_CLIENT_ID_THREAD));
    bool ok = true;
    for (int i = 0; i < 2000 && ok; ++i) ok = probe_teb(tid);
    a->pthread_bounds = teb_get(static_cast<uint8_t*>(teb), GTA6RT_TEB_STACK_BASE) ==
                        reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(pthread_self()));
    a->bench = call(fn(kOff_bench_int), {2000000, tid}).rax;
    ok = ok && call(fn(kOff_call1), {g_reenter_thunk, 3}).rax == 3 + 1000 * 4 + 10;
    a->ok = ok;
    gta6rt_isa_thread_detach();
    return nullptr;
}

__attribute__((noinline)) uint64_t ref_bench_int(uint64_t n, uint64_t seed) {
    uint64_t x = seed;
    for (uint64_t i = 0; i < n; ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        x ^= x >> 29;
        __asm__ volatile("" : "+r"(x));
    }
    return x;
}

void test_threads() {
    constexpr int kN = 7;
    pthread_t th[kN];
    ThreadArg args[kN];
    for (int i = 0; i < kN; ++i) {
        args[i].stack = i < 4 ? (8u << 20) : i < 6 ? 0 : (256u << 10);
        pthread_attr_t at;
        pthread_attr_init(&at);
        if (args[i].stack) pthread_attr_setstacksize(&at, args[i].stack);
        pthread_create(&th[i], &at, thread_main, &args[i]);
        pthread_attr_destroy(&at);
    }
    for (int i = 0; i < kN; ++i) pthread_join(th[i], nullptr);
    for (int i = 0; i < kN; ++i) {
        CHECK(args[i].ok, "thread %d: 2000 TEB probes + nested thunk calls", i);
        CHECK(args[i].pthread_bounds == (i < 6), "thread %d: TEB bounds %s", i,
              i < 4   ? "are the 8 MiB pthread stack (split)"
              : i < 6 ? "are the default 512 KiB pthread stack (split)"
                      : "are a separate guest stack (256 KiB pthread stack)");
    }
    uint64_t ref = 0;
    bool same = true;
    for (int i = 0; i < kN; ++i) (void)ref, same &= args[i].bench != 0;
    CHECK(same, "threads ran bench_int");
}

gta6rt_gva frame_thunk(void (*h)(gta6rt_guest_frame*, void*), const char* name);

// Every round maps fresh guest functions, so all threads hit FEX's block compile (LookupCache's WritePriorityMutex)
// at the same time. A lost futex wake there leaves every thread asleep; the watchdog turns that into a FAIL exit.
constexpr uint32_t kRaceFns = 256;
constexpr size_t kRaceStride = 64;

void h_race(gta6rt_guest_frame* f, void*) {
    f->rax = f->rcx * 0x10001ull + f->rdx;
}

uint64_t race_expect(uint32_t i, uint32_t round) {
    return uint64_t(i) * 0x10001ull + round + (i * 7 + round);
}

void emit_race_fn(uint8_t* p, uint32_t i, uint32_t round, gta6rt_gva thunk) {
    const uint32_t k = i * 7 + round;
    const uint8_t head[] = {0x48, 0x83, 0xEC, 0x28, 0xB9};
    memcpy(p, head, sizeof head);
    p += sizeof head;
    memcpy(p, &i, 4);
    p += 4;
    *p++ = 0xBA;
    memcpy(p, &round, 4);
    p += 4;
    *p++ = 0x48;
    *p++ = 0xB8;
    memcpy(p, &thunk, 8);
    p += 8;
    const uint8_t tail[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0x48, 0x05};
    memcpy(p, tail, sizeof tail);
    p += sizeof tail;
    memcpy(p, &k, 4);
    p += 4;
    *p = 0xC3;
}

struct RaceRound {
    uint8_t* code;
    uint32_t round;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> bad{0};
};

struct RaceThread {
    RaceRound* r;
    uint32_t index;
};

std::atomic<uint64_t> g_race_calls{0};
std::atomic<bool> g_race_active{false};
std::atomic<uint32_t> g_race_round{0};

void* race_main(void* p) {
    auto* t = static_cast<RaceThread*>(p);
    RaceRound* r = t->r;
    gta6rt_teb_desc d{};
    void* teb = nullptr;
    if (gta6rt_isa_thread_attach(&d, &teb) != GTA6RT_OK) {
        r->bad.fetch_add(1);
        r->ready.fetch_add(1);
        return nullptr;
    }
    r->ready.fetch_add(1);
    while (!r->go.load(std::memory_order_acquire)) sched_yield();
    const uint32_t stride = (2 * t->index + 1) % kRaceFns;
    uint32_t i = (t->index * 37) % kRaceFns;
    for (uint32_t n = 0; n < kRaceFns; ++n, i = (i + stride) % kRaceFns) {
        const uint64_t got = gta6rt_isa_call_guest4(reinterpret_cast<gta6rt_gva>(r->code + i * kRaceStride), 0, 0, 0, 0);
        if (got != race_expect(i, r->round)) r->bad.fetch_add(1);
        g_race_calls.fetch_add(1, std::memory_order_relaxed);
    }
    gta6rt_isa_thread_detach();
    return nullptr;
}

void* race_watchdog(void*) {
    constexpr int kStallTicks = 100;
    uint64_t last = 0;
    int still = 0;
    for (;;) {
        usleep(100000);
        if (!g_race_active.load()) {
            still = 0;
            continue;
        }
        const uint64_t now = g_race_calls.load();
        still = now == last ? still + 1 : 0;
        last = now;
        if (still >= kStallTicks) {
            std::printf("FAIL isa_fex_test: concurrent first compile: no guest call finished for 10 s in round %u "
                        "(%llu calls done): deadlock\n",
                        g_race_round.load(), (unsigned long long)now);
            _exit(3);
        }
    }
    return nullptr;
}

void test_concurrent_compile(int threads, uint32_t rounds) {
    static bool watchdog = false;
    if (!watchdog) {
        pthread_t w;
        pthread_create(&w, nullptr, race_watchdog, nullptr);
        pthread_detach(w);
        watchdog = true;
    }
    const gta6rt_gva thunk = frame_thunk(&h_race, "test!race");
    const size_t region = (size_t(rounds) * kRaceFns * kRaceStride + 16383) & ~size_t(16383);
    auto* code = static_cast<uint8_t*>(mmap(nullptr, region, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    CHECK(code != MAP_FAILED, "race code region");
    if (code == MAP_FAILED) return;
    gta6rt_isa_register_guest_code(reinterpret_cast<gta6rt_gva>(code), region);
    std::vector<pthread_t> th(threads);
    std::vector<RaceThread> ta(threads);
    std::vector<double> ms;
    int bad = 0;
    const uint64_t t_all = now_ns();
    for (uint32_t round = 0; round < rounds; ++round) {
        RaceRound r;
        r.code = code + size_t(round) * kRaceFns * kRaceStride;
        r.round = round;
        for (uint32_t i = 0; i < kRaceFns; ++i) emit_race_fn(r.code + i * kRaceStride, i, round, thunk);
        g_race_round.store(round);
        for (int i = 0; i < threads; ++i) {
            ta[i] = {&r, uint32_t(i)};
            pthread_attr_t at;
            pthread_attr_init(&at);
            const int kind = i % 3;
            if (kind == 0) pthread_attr_setstacksize(&at, 8u << 20);
            else if (kind == 2) pthread_attr_setstacksize(&at, 256u << 10);
            pthread_create(&th[i], &at, race_main, &ta[i]);
            pthread_attr_destroy(&at);
        }
        while (r.ready.load() < threads) sched_yield();
        g_race_active.store(true);
        const uint64_t t0 = now_ns();
        r.go.store(true, std::memory_order_release);
        for (int i = 0; i < threads; ++i) pthread_join(th[i], nullptr);
        ms.push_back(double(now_ns() - t0) / 1e6);
        g_race_active.store(false);
        bad += r.bad.load();
    }
    const double total = double(now_ns() - t_all) / 1e6;
    std::sort(ms.begin(), ms.end());
    std::printf("concurrent first compile: %d threads x %u rounds x %u fresh functions, %.1f ms total, "
                "per round min %.2f / median %.2f / max %.2f ms, %d wrong results\n",
                threads, rounds, kRaceFns, total, ms.front(), ms[ms.size() / 2], ms.back(), bad);
    CHECK(bad == 0, "concurrent first compile: %d wrong results or failed attaches", bad);
}

void test_faults() {
    CHECK(gta6rt_isa_set_guest_fault_handler(&on_fault, nullptr) == GTA6RT_OK, "set fault handler");
    struct Case {
        const char* name;
        size_t entry, site;
        unsigned len;
        uint64_t arg0, arg1;
        uint32_t code;
    };
    static uint64_t ro_word = 1;
    void* ro = mmap(nullptr, 16384, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    const Case cases[] = {
        {"ud2", kOff_fault_ud2, kOff_fault_ud2_site, 2, 0, 0, 0xC000001D},
        {"int3", kOff_fault_int3, kOff_fault_int3_site, 1, 0, 0, 0x80000003},
        {"read NULL+8", kOff_fault_read, kOff_fault_read_site, 3, 8, 0, 0xC0000005},
        {"write read-only page", kOff_fault_write, kOff_fault_write_site, 3, reinterpret_cast<uint64_t>(ro), 5,
         0xC0000005},
        {"int 0x29 (__fastfail)", kOff_fastfail, kOff_fastfail_site, 2, 0, 0, 0xC0000409},
    };
    (void)ro_word;
    for (const Case& c : cases) {
        t_fault = {};
        t_fault.resume_at = fn(c.site) + c.len;
        const gta6rt_guest_ret r = call(fn(c.entry), {c.arg0, c.arg1});
        std::printf("fault %-22s code 0x%08x address exe-site%+lld rip-site%+lld params 0x%llx 0x%llx\n", c.name,
                    t_fault.code, (long long)(t_fault.address - fn(c.site)), (long long)(t_fault.rip - fn(c.site)),
                    (unsigned long long)t_fault.p0, (unsigned long long)t_fault.p1);
        CHECK(t_fault.n == 1 && t_fault.code == c.code && r.rax == 0x600D && t_fault.rip == fn(c.site), "%s", c.name);
    }
    t_fault = {};
    t_fault.resume_at = fn(kOff_fault_write_site) + 3;
    call(fn(kOff_fault_write), {reinterpret_cast<uint64_t>(ro), 5});
    CHECK(t_fault.p0 == 1 && t_fault.p1 == reinterpret_cast<uint64_t>(ro), "AV write: params {1, address}");
    t_fault = {};
    t_fault.resume_at = fn(kOff_fault_read_site) + 3;
    call(fn(kOff_fault_read), {16});
    CHECK(t_fault.p0 == 0 && t_fault.p1 == 16, "AV read: params {0, address}");
    munmap(ro, 16384);

    gta6rt_thunk_desc d{};
    d.kind = GTA6RT_THUNK_FRAME;
    d.fn = reinterpret_cast<const void*>(&h_fault_inside);
    d.debug_name = "test!fault_inside";
    gta6rt_gva th = 0;
    gta6rt_isa_make_thunk(&d, &th);
    CHECK(call(fn(kOff_call1), {th, 0}).rax == 0x1234 + 1000,
          "fault in a nested host->guest call, resumed there; capture in its fault handler is refused (outer thunk only)");

    t_fault = {};
    const gta6rt_guest_ret dz = call(fn(kOff_div), {100, 0, 0});
    std::printf("div 100/0: rax=0x%llx, %u guest exception(s) code 0x%08x\n", (unsigned long long)dz.rax, t_fault.n,
                t_fault.code);
    CHECK(call(fn(kOff_div), {100, 0, 7}).rax == 14, "div 100/7");
}

void test_zen_and_sse4a() {
    const uint64_t traps0 = gta6rt_isa_trap_count();
    Sse4aInsn in;
    const uint8_t* ins = g_blob + kOff_insertq + 10;
    CHECK(sse4a_decode(ins, 8, &in) && in.op == Sse4aOp::kInsertqImm, "decode the blob's insertq");
    const uint64_t a = 0x1122334455667788ull, b = 0x99aabbccddeeff00ull;
    uint64_t x[16][2] = {};
    x[0][0] = a;
    x[1][0] = b;
    sse4a_execute(in, x);
    CHECK(call(fn(kOff_insertq), {a, b}).rax == x[0][0], "insertq imm under FEX matches the reference");
    const uint8_t* ex = g_blob + kOff_extrq + 5;
    CHECK(sse4a_decode(ex, 8, &in) && in.op == Sse4aOp::kExtrqImm, "decode the blob's extrq");
    memset(x, 0, sizeof x);
    x[0][0] = a;
    sse4a_execute(in, x);
    CHECK(call(fn(kOff_extrq), {a}).rax == x[0][0], "extrq imm under FEX matches the reference");
    const uint64_t traps_sse4a = gta6rt_isa_trap_count() - traps0;

    alignas(64) static uint64_t line[16];
    for (auto& v : line) v = 0xFFFF;
    const uint64_t t1 = gta6rt_isa_trap_count();
    CHECK(call(fn(kOff_clzero), {reinterpret_cast<uint64_t>(line)}).rax == 0 && line[0] == 0 && line[8] == 0xFFFF,
          "clzero zeroes exactly the 64-byte line");
    const uint64_t traps_clzero = gta6rt_isa_trap_count() - t1;
    const uint64_t t2 = gta6rt_isa_trap_count();
    line[0] = 0x55;
    CHECK(call(fn(kOff_clwb), {reinterpret_cast<uint64_t>(line)}).rax == 0x55, "clwb + clflushopt");
    const uint64_t traps_clwb = gta6rt_isa_trap_count() - t2;
    const uint64_t t3 = gta6rt_isa_trap_count();
    CHECK(call(fn(kOff_monitorx), {reinterpret_cast<uint64_t>(line)}).rax == 0x77, "monitorx + mwaitx continue");
    const uint64_t traps_mon = gta6rt_isa_trap_count() - t3;
    std::printf("isa emulation traps: insertq+extrq %llu, clzero %llu, clwb+clflushopt %llu, monitorx+mwaitx %llu\n",
                (unsigned long long)traps_sse4a, (unsigned long long)traps_clzero, (unsigned long long)traps_clwb,
                (unsigned long long)traps_mon);

    alignas(32) uint64_t data[4] = {0x8000000000000001ull, 0xF0F0F0F0F0F0F0F0ull, 0x123456789ull, ~0ull};
    alignas(32) uint64_t cnt[4] = {1, 63, 64, 0x100000001ull};
    alignas(32) uint64_t out[8] = {};
    call(fn(kOff_vshift), {reinterpret_cast<uint64_t>(data), reinterpret_cast<uint64_t>(cnt), reinterpret_cast<uint64_t>(out)});
    bool vok = true;
    for (int i = 0; i < 4; ++i) {
        const uint64_t l = cnt[i] >= 64 ? 0 : data[i] << cnt[i];
        const uint64_t r = cnt[i] >= 64 ? 0 : data[i] >> cnt[i];
        vok &= out[i] == l && out[4 + i] == r;
    }
    CHECK(vok, "vpsllvq/vpsrlvq counts {1,63,64,2^32+1} follow x86 (the Rosetta bug is absent)");
}

void test_cpuid() {
    bool ok = true;
    for (uint32_t leaf : {0u, 1u, 7u, 0xDu, 0x80000000u, 0x80000001u, 0x80000008u}) {
        uint32_t g[4] = {};
        call(fn(kOff_cpuid), {leaf, 0, reinterpret_cast<uint64_t>(g)});
        gta6rt_cpuid_regs p{};
        gta6rt_isa_cpuid(leaf, 0, &p);
        if (g[0] != p.eax || g[1] != p.ebx || g[2] != p.ecx || g[3] != p.edx) {
            ok = false;
            std::printf("cpuid 0x%x guest %08x %08x %08x %08x policy %08x %08x %08x %08x\n", leaf, g[0], g[1], g[2], g[3],
                        p.eax, p.ebx, p.ecx, p.edx);
        }
    }
    CHECK(ok, "guest cpuid == policy for leaves 0, 1, 7, 0xD, 0x8000000[0,1,8]");
    CHECK(call(fn(kOff_xgetbv), {}).rax == 7, "xgetbv(0) = 7");
    CHECK(gta6rt_isa_advertised_features() == GTA6RT_CPUID_DEFAULT, "advertised 0x%llx",
          (unsigned long long)gta6rt_isa_advertised_features());
    CHECK(gta6rt_isa_cpuid_enforced() == 1, "enforced");
    std::printf("cpuid: FEX raw features 0x%llx, guest sees 0x%llx (policy 0x%llx)\n",
                (unsigned long long)gta6rt_isa_host_features(), (unsigned long long)gta6rt_isa_advertised_features(),
                (unsigned long long)GTA6RT_CPUID_DEFAULT);
}

uint64_t host_div_up(double a, double b) {
    const int old = fegetround();
    fesetround(FE_UPWARD);
    volatile double x = a, y = b;
    const double q = x / y;
    fesetround(old);
    return bits(q);
}

void h_nested_div(gta6rt_guest_frame* f, void*) {
    f->rax = call(fn(kOff_divsd), {f->rcx, f->rdx}).rax;
}

void test_mxcsr() {
    constexpr uint32_t kUp = 0x1F80 | (2u << 13);
    const uint64_t a = bits(1.0), b = bits(3.0);
    const uint64_t nearest = bits(1.0 / 3.0), up = host_div_up(1.0, 3.0);
    CHECK(nearest != up, "1/3 rounds differently to nearest and upward");
    CHECK(call(fn(kOff_divsd), {a, b}).rax == nearest, "divsd under the default MXCSR rounds to nearest");
    const uint32_t prev = static_cast<uint32_t>(call(fn(kOff_set_mxcsr), {kUp}).rax);
    CHECK(prev == 0x1F80, "default guest MXCSR 0x%x", prev);
    CHECK(call(fn(kOff_divsd), {a, b}).rax == up, "MXCSR RC=up set by ldmxcsr holds for the next host->guest call");
    CHECK(fegetround() == FE_TONEAREST, "host rounding stays to nearest while the guest has RC=up");
    gta6rt_thunk_desc d{};
    d.kind = GTA6RT_THUNK_FRAME;
    d.fn = reinterpret_cast<const void*>(&h_nested_div);
    d.debug_name = "test!nested_div";
    gta6rt_gva th = 0;
    CHECK(gta6rt_isa_make_thunk(&d, &th) == GTA6RT_OK, "make nested-div thunk");
    call(fn(kOff_set_mxcsr), {0x1F80});
    uint64_t after = 0;
    const uint64_t inner = call(fn(kOff_mx_nested), {th, kUp, a, b, reinterpret_cast<uint64_t>(&after)}).rax;
    CHECK(inner == up, "a callback nested in a thunk call sees the guest's RC=up");
    CHECK(after == up, "RC=up still holds in the guest after the thunk returns");
    CHECK(call(fn(kOff_divsd), {a, b}).rax == nearest, "MXCSR restored by the guest: nearest again");
}

gta6rt_x64_context g_captured;
gta6rt_status g_capture_status = GTA6RT_E_NOT_IMPLEMENTED;
void h_capture(gta6rt_guest_frame* f, void*) {
    g_capture_status = gta6rt_isa_capture_guest_context(&g_captured);
    f->rax = 0x77;
}

void h_resume(gta6rt_guest_frame*, void*) {
    alignas(16) gta6rt_x64_context c;
    if (gta6rt_isa_capture_guest_context(&c) != GTA6RT_OK) return;
    c.rip = fn(kOff_resume_landing);
    c.rax = 0xC0DE;
    gta6rt_isa_resume_guest_context(&c);
}

gta6rt_gva g_resume_thunk = 0;
void h_nested_resume(gta6rt_guest_frame* f, void*) {
    f->rax = call(fn(kOff_resume_probe), {g_resume_thunk}).rax;
}

gta6rt_gva frame_thunk(void (*h)(gta6rt_guest_frame*, void*), const char* name) {
    gta6rt_thunk_desc d{};
    d.kind = GTA6RT_THUNK_FRAME;
    d.fn = reinterpret_cast<const void*>(h);
    d.debug_name = name;
    gta6rt_gva t = 0;
    CHECK(gta6rt_isa_make_thunk(&d, &t) == GTA6RT_OK, "make %s", name);
    return t;
}

void test_context_capture_resume() {
    gta6rt_x64_context outside;
    CHECK(gta6rt_isa_capture_guest_context(&outside) == GTA6RT_E_NOT_FOUND, "capture outside a thunk handler");
    uint64_t at[2] = {};
    const uint64_t r = call(fn(kOff_ctx_probe), {frame_thunk(&h_capture, "test!capture"), reinterpret_cast<uint64_t>(at)}).rax;
    const gta6rt_x64_context& c = g_captured;
    CHECK(r == 0x77 && g_capture_status == GTA6RT_OK, "capture inside a FRAME thunk");
    CHECK(c.rip == at[1] && c.rsp == at[0], "captured rip = return address, rsp = rsp after the return (rip %llx/%llx rsp %llx/%llx)",
          (unsigned long long)c.rip, (unsigned long long)at[1], (unsigned long long)c.rsp, (unsigned long long)at[0]);
    CHECK(c.rbx == 0x1111111111111111ull && c.rbp == 0x2222222222222222ull && c.rsi == 0x3333333333333333ull &&
              c.rdi == 0x4444444444444444ull && c.r12 == 0x5555555555555555ull && c.r13 == 0x6666666666666666ull &&
              c.r14 == 0x7777777777777777ull && c.r15 == 0x8888888888888888ull,
          "captured callee-saved registers");
    CHECK(c.seg_cs == 0x33 && c.mxcsr == 0x1F80, "captured cs/mxcsr");

    g_resume_thunk = frame_thunk(&h_resume, "test!resume");
    CHECK(call(fn(kOff_resume_probe), {g_resume_thunk}).rax == 0xC0DE + 0x5151, "resume from a thunk at another guest rip");
    const gta6rt_gva nested = frame_thunk(&h_nested_resume, "test!nested_resume");
    CHECK(call(fn(kOff_call1), {nested, 0}).rax == 0xC0DE + 0x5151 + 1000,
          "resume inside a nested host->guest call; the outer thunk call returns normally");
    bool all = true;
    for (int i = 0; i < 100000 && all; ++i) all = call(fn(kOff_resume_probe), {g_resume_thunk}).rax == 0xC0DE + 0x5151;
    CHECK(all, "100000 resumes from a thunk (host stack is not leaked)");
}

void test_atomics() {
    alignas(64) static uint8_t buf[128];
    memset(buf, 0, sizeof buf);
    auto* p = reinterpret_cast<uint64_t*>(buf + 12);
    CHECK(call(fn(kOff_lock_add), {reinterpret_cast<uint64_t>(p), 1000, 3}).rax == 3000,
          "lock xadd on a qword crossing a 16-byte boundary");
    auto* q = reinterpret_cast<uint64_t*>(buf + 64);
    CHECK(call(fn(kOff_lock_add), {reinterpret_cast<uint64_t>(q), 1000, 5}).rax == 5000, "aligned lock xadd");
}

void test_chkstk(const char* exe) {
    MappedImage img;
    if (!map_pe(exe, &img)) {
        CHECK(false, "cannot map %s", exe);
        return;
    }
    const gta6rt_gva f = gta6rt_isa_find_chkstk(reinterpret_cast<uint64_t>(img.base), img.size);
    CHECK(f == reinterpret_cast<uint64_t>(img.base) + 0x5647140, "find the title's __chkstk");
    if (f) {
        uint8_t* dst = g_blob + kScratch;
        memcpy(dst, reinterpret_cast<void*>(f), 96);
        for (uint64_t size : {0x100ull, 0x3000ull, 0x20000ull})
            CHECK(call(fn(kOff_call_chkstk), {reinterpret_cast<uint64_t>(dst), size}).rax == 0,
                  "the title's __chkstk bytes, unpatched, size 0x%llx: rsp/r10/r11 preserved", (unsigned long long)size);
    }
    munmap(img.base, img.size);
}

template <typename F>
double best_ns(int reps, F&& f) {
    double best = 1e300;
    for (int i = 0; i < reps; ++i) {
        const uint64_t t0 = now_ns();
        f();
        const double d = double(now_ns() - t0);
        if (d < best) best = d;
    }
    return best;
}

__attribute__((noinline)) uint64_t native_mix(uint64_t n) {
    uint64_t acc = 0;
    for (uint64_t i = n; i; --i) {
        uint64_t d = i & 7;
        if (d >= 3) {
            d *= i;
            d = (d >> 7) | (d << 57);
        } else {
            d = (i * 5) ^ 0x5a5a;
        }
        acc += d;
        __asm__ volatile("" : "+r"(acc));
    }
    return acc;
}

void bench() {
    std::printf("\n## Throughput through gta6rt_isa_call_guest (best of 5)\n\n");
    std::printf("| kernel | FEX ns/iter | native arm64 ns/iter | ratio | checksum |\n|---|---:|---:|---:|---|\n");
    constexpr uint64_t kInt = 100000000;
    call(fn(kOff_bench_int), {1000, 1});
    uint64_t gr = 0, nr = 0;
    double g = best_ns(5, [&] { gr = call(fn(kOff_bench_int), {kInt, 1}).rax; });
    double n = best_ns(5, [&] { nr = ref_bench_int(kInt, 1); });
    std::printf("| int LCG chain | %.3f | %.3f | %.2fx | %s |\n", g / kInt, n / kInt, g / n, gr == nr ? "match" : "MISMATCH");
    CHECK(gr == nr, "bench_int checksum");

    constexpr uint64_t kMix = 50000000;
    call(fn(kOff_bench_mix), {1000});
    g = best_ns(5, [&] { gr = call(fn(kOff_bench_mix), {kMix}).rax; });
    n = best_ns(5, [&] { nr = native_mix(kMix); });
    std::printf("| branchy mix + call | %.3f | %.3f | %.2fx | %s |\n", g / kMix, n / kMix, g / n, gr == nr ? "match" : "MISMATCH");
    CHECK(gr == nr, "bench_mix checksum");

    constexpr uint64_t kLen = 4096, kReps = 5000;
    auto* mem = static_cast<uint64_t*>(calloc(kLen, 8));
    auto* fa = static_cast<float*>(calloc(kLen, 4));
    auto* fb = static_cast<float*>(calloc(kLen, 4));
    auto* ia = static_cast<int32_t*>(calloc(kLen, 4));
    auto* ib = static_cast<int32_t*>(calloc(kLen, 4));
    for (uint64_t i = 0; i < kLen; ++i) {
        mem[i] = i;
        fa[i] = 1.0f + float(i % 7) * 1e-3f;
        fb[i] = 1.0f - float(i % 5) * 1e-3f;
        ia[i] = int32_t(i * 2654435761u);
        ib[i] = int32_t(i | 1);
    }
    g = best_ns(5, [&] { gr = call(fn(kOff_bench_mem), {reinterpret_cast<uint64_t>(mem), kReps, kLen}).rax; });
    std::printf("| load/store chain | %.3f | - | - | - |\n", g / (kLen * kReps));
    g = best_ns(5, [&] { gr = call(fn(kOff_bench_sse), {reinterpret_cast<uint64_t>(fa), reinterpret_cast<uint64_t>(fb), kReps, kLen}).rax; });
    std::printf("| SSE mulps/addps (per 8 floats) | %.3f | - | - | - |\n", g / (kLen / 8 * kReps));
    g = best_ns(5, [&] { gr = call(fn(kOff_bench_avx2), {reinterpret_cast<uint64_t>(ia), reinterpret_cast<uint64_t>(ib), kReps, kLen}).rax; });
    std::printf("| AVX2 vpmulld/vpaddd (per 8 ints) | %.3f | - | - | - |\n", g / (kLen / 8 * kReps));

    gta6rt_thunk_desc d{};
    d.kind = GTA6RT_THUNK_FRAME;
    d.fn = reinterpret_cast<const void*>(&h_count);
    d.debug_name = "bench!count";
    gta6rt_gva t = 0;
    gta6rt_isa_make_thunk(&d, &t);
    constexpr uint64_t kCalls = 5000000;
    call(fn(kOff_bench_calls), {t, 100});
    g = best_ns(5, [&] { gr = call(fn(kOff_bench_calls), {t, kCalls}).rax; });
    std::printf("| guest->host FRAME thunk call+return | %.1f | - | - | sum %llu |\n", g / kCalls, (unsigned long long)gr);
    CHECK(gr == kCalls / 2, "bench_calls sum");
    constexpr int kEntries = 200000;
    g = best_ns(5, [&] {
        for (int i = 0; i < kEntries; ++i) gta6rt_isa_call_guest4(fn(kOff_add3), 1, 2, 3, 0);
    });
    std::printf("| host->guest call_guest+return | %.1f | - | - | - |\n", g / kEntries);
    free(mem);
    free(fa);
    free(fb);
    free(ia);
    free(ib);
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool do_bench = false;
    const char* exe = nullptr;
    int race_threads = 0;
    unsigned race_rounds = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--bench")) do_bench = true;
        else if (sscanf(argv[i], "--race=%d,%u", &race_threads, &race_rounds) == 2) continue;
        else exe = argv[i];
    }
    int translated = 0;
    size_t ts = sizeof translated;
    sysctlbyname("sysctl.proc_translated", &translated, &ts, nullptr, 0);
    std::printf("isa_fex_test: proc_translated=%d page=%ld\n", translated, sysconf(_SC_PAGESIZE));
    const uint64_t t0 = now_ns();
    gta6rt_isa_config cfg{GTA6RT_TEB_SHARE, 0};
    CHECK(gta6rt_isa_init(&cfg) == GTA6RT_OK, "init");
    std::printf("backend=%s, init %.2f ms\n", gta6rt_isa_backend_name(), double(now_ns() - t0) / 1e6);
    void* m = mmap(nullptr, kBlobRegion, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    g_blob = static_cast<uint8_t*>(m);
    memcpy(g_blob, kGuestBlob, sizeof kGuestBlob);
    gta6rt_isa_register_guest_code(reinterpret_cast<gta6rt_gva>(g_blob), kBlobRegion);
    static uint64_t peb[256];
    gta6rt_isa_set_peb(peb);
    CHECK(gta6rt_isa_tls_register_image(0, tls_ctor, tls_dtor, nullptr) == GTA6RT_OK, "tls image 0");
    gta6rt_teb_desc d{};
    d.thread_id = 0x104;
    void* teb = nullptr;
    CHECK(gta6rt_isa_thread_attach(&d, &teb) == GTA6RT_OK, "main attach");
    if (race_threads > 0) {
        test_concurrent_compile(race_threads, race_rounds);
        return finish("isa_fex_test --race");
    }
    test_calls();
    test_thunks();
    test_native();
    test_os_invoker();
    test_teb();
    test_threads();
    test_concurrent_compile(16, 32);
    test_cpuid();
    test_zen_and_sse4a();
    test_mxcsr();
    test_context_capture_resume();
    if (!getenv("SKIP_ATOMICS")) test_atomics();
    test_faults();
    if (const char* e = exe ? exe : GTA6RT_ISA_TEST_EXE) test_chkstk(e);
    if (do_bench) bench();
    return finish("isa_fex_test");
}
