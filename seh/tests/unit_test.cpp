// Data-level tests of the .pdata unwinder, table registry and conversions. Runs natively on arm64 and on x86_64;
// the x86_64 part also exercises RtlCaptureContext/RtlRestoreContext, RaiseException with a vectored handler and
// the signal -> EXCEPTION_RECORD conversion.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <algorithm>
#include <vector>

#include <windows.h>

#include <gta6rt/seh.h>

#if defined(__x86_64__)
#include <sys/ucontext.h>
#endif

namespace {

int g_fail = 0, g_checks = 0;

#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_fail;                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
        }                                                                     \
    } while (0)

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { PUSH = 0, ALLOC_LARGE, ALLOC_SMALL, SET_FPREG, SAVE_NONVOL, SAVE_NONVOL_FAR, EPILOG, SPARE, SAVE_XMM, SAVE_XMM_FAR,
       MACHFRAME };

uint16_t uc(unsigned off, unsigned op, unsigned info) { return static_cast<uint16_t>(off | op << 8 | info << 12); }

constexpr uint64_t kRet = 0x00007ff0000c0de0ull;
uint64_t orig(int r) { return 0x0a110000000000ull | (static_cast<uint64_t>(r) << 8) | 0x11; }
uint64_t junk(int r) { return 0xbad0000000000000ull | static_cast<uint64_t>(r); }

struct image {
    uint8_t* mem = nullptr;
    uint64_t base = 0;
    uint32_t size = 0x10000, code = 0x1000, xdata = 0x8000;
    std::vector<gta6rt_runtime_function> fns;
    const gta6rt_runtime_function* table = nullptr;

    image() {
        mem = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        base = reinterpret_cast<uint64_t>(mem);
        memset(mem, 0xcc, size);
    }
    uint32_t add_code(std::initializer_list<uint8_t> b) {
        code = (code + 15) & ~15u;
        const uint32_t r = code;
        for (uint8_t x : b) mem[code++] = x;
        return r;
    }
    void patch_rel32(uint32_t at, uint32_t target) {
        const int32_t d = static_cast<int32_t>(target - (at + 5));
        memcpy(mem + at + 1, &d, 4);
    }
    uint32_t add_unwind(int version, int flags, int prolog, int freg, int foff, std::vector<uint16_t> codes,
                        std::vector<uint32_t> tail = {}) {
        xdata = (xdata + 3) & ~3u;
        const uint32_t r = xdata;
        mem[xdata++] = static_cast<uint8_t>(version | flags << 3);
        mem[xdata++] = static_cast<uint8_t>(prolog);
        mem[xdata++] = static_cast<uint8_t>(codes.size());
        mem[xdata++] = static_cast<uint8_t>(freg | foff << 4);
        for (uint16_t c : codes) {
            memcpy(mem + xdata, &c, 2);
            xdata += 2;
        }
        if (codes.size() & 1) xdata += 2;
        for (uint32_t t : tail) {
            memcpy(mem + xdata, &t, 4);
            xdata += 4;
        }
        return r;
    }
    void add_fn(uint32_t begin, uint32_t end, uint32_t unwind) { fns.push_back({begin, end, unwind}); }
    void finish() {
        std::sort(fns.begin(), fns.end(), [](auto& a, auto& b) { return a.begin_rva < b.begin_rva; });
        auto* t = reinterpret_cast<gta6rt_runtime_function*>(mem + 0xe000);
        memcpy(t, fns.data(), fns.size() * sizeof fns[0]);
        table = t;
        CHECK(gta6rt_seh_add_function_table(base, t, static_cast<uint32_t>(fns.size()), size) == GTA6RT_OK);
    }
    uint64_t at(uint32_t rva) const { return base + rva; }
};

struct stack {
    alignas(16) uint64_t s[4096];
    uint64_t* top = &s[3000];
    uint64_t E() const { return reinterpret_cast<uint64_t>(top); }
    void put(uint64_t addr, uint64_t v) { memcpy(reinterpret_cast<void*>(addr), &v, 8); }
    void put128(uint64_t addr, uint64_t lo, uint64_t hi) {
        put(addr, lo);
        put(addr + 8, hi);
    }
};

void base_ctx(gta6rt_x64_context* c, uint64_t rip, uint64_t rsp) {
    memset(c, 0, sizeof *c);
    c->context_flags = CONTEXT_FULL;
    uint64_t* g = &c->rax;
    for (int r = 0; r < 16; ++r) g[r] = orig(r);
    for (int x = 0; x < 16; ++x) {
        c->fp.v.xmm[x].lo = orig(x) ^ 0x5500;
        c->fp.v.xmm[x].hi = static_cast<int64_t>(orig(x) ^ 0x6600);
    }
    c->rip = rip;
    c->rsp = rsp;
}

uint64_t& reg(gta6rt_x64_context& c, int r) { return (&c.rax)[r]; }

struct result {
    gta6rt_status st;
    uint64_t handler, establisher;
    void* data;
    gta6rt_x64_context c;
};

result unwind(const image& im, gta6rt_x64_context c, uint32_t type = 0) {
    result r{};
    uint64_t base = 0;
    const gta6rt_runtime_function* f = gta6rt_seh_lookup(c.rip, &base);
    CHECK(f != nullptr);
    CHECK(base == im.base);
    if (!f) {
        r.st = GTA6RT_E_NOT_FOUND;
        return r;
    }
    r.st = gta6rt_seh_virtual_unwind(type, base, c.rip, f, &c, &r.data, &r.establisher, &r.handler);
    r.c = c;
    return r;
}

void test_canonical(image& im) {
    printf("canonical push/alloc prolog and epilog\n");
    const uint32_t a = im.add_code({0x53, 0x56, 0x48, 0x83, 0xec, 0x28, 0x90, 0x90, 0x90, 0x90, 0x48, 0x83, 0xc4, 0x28,
                                    0x5e, 0x5b, 0xc3});
    im.add_fn(a, a + 17, im.add_unwind(1, 0, 6, 0, 0, {uc(6, ALLOC_SMALL, 4), uc(2, PUSH, RSI), uc(1, PUSH, RBX)}));
    im.finish();
    stack st;
    const uint64_t E = st.E();
    st.put(E, kRet);
    st.put(E - 8, orig(RBX));
    st.put(E - 16, orig(RSI));
    struct pt {
        uint32_t off;
        uint64_t rsp;
        bool rbx_saved, rsi_saved, epi;
    } pts[] = {{0, E, false, false, false},        {1, E - 8, true, false, false},       {2, E - 16, true, true, false},
               {6, E - 16 - 0x28, true, true, false}, {10, E - 16 - 0x28, true, true, true}, {14, E - 16, true, true, true},
               {15, E - 8, true, false, true},    {16, E, false, false, true}};
    for (const pt& p : pts) {
        gta6rt_x64_context c;
        base_ctx(&c, im.at(a + p.off), p.rsp);
        if (p.rbx_saved) c.rbx = junk(RBX);
        if (p.rsi_saved) c.rsi = junk(RSI);
        result r = unwind(im, c);
        CHECK(r.st == GTA6RT_OK);
        CHECK(r.c.rip == kRet);
        CHECK(r.c.rsp == E + 8);
        CHECK(r.c.rbx == orig(RBX));
        CHECK(r.c.rsi == orig(RSI));
        CHECK(r.establisher == p.rsp);
        if (r.c.rip != kRet || r.c.rsp != E + 8) printf("    at +%u: rip %llx rsp %+lld\n", p.off,
                                                         (unsigned long long)r.c.rip, (long long)(r.c.rsp - E - 8));
    }
}

void test_frame_and_far(image& im) {
    printf("frame register, SAVE_NONVOL(_FAR), SAVE_XMM128(_FAR), ALLOC_LARGE, lea-rsp epilog\n");
    const uint32_t f = im.add_code({0x55,                                            // push rbp
                                    0x48, 0x81, 0xec, 0x00, 0x02, 0x00, 0x00,        // sub rsp,0x200
                                    0x48, 0x89, 0x9c, 0x24, 0x10, 0x01, 0x00, 0x00,  // mov [rsp+0x110],rbx
                                    0x0f, 0x29, 0xb4, 0x24, 0xe0, 0x00, 0x00, 0x00,  // movaps [rsp+0xe0],xmm6
                                    0x4c, 0x89, 0xa4, 0x24, 0x08, 0x01, 0x00, 0x00,  // mov [rsp+0x108],r12
                                    0x44, 0x0f, 0x29, 0xbc, 0x24, 0xc0, 0x00, 0x00, 0x00,  // movaps [rsp+0xc0],xmm15
                                    0x48, 0x8d, 0xac, 0x24, 0x80, 0x00, 0x00, 0x00,  // lea rbp,[rsp+0x80]
                                    0x48, 0x83, 0xec, 0x40, 0x90, 0x90,              // sub rsp,0x40 (body)
                                    0x48, 0x8d, 0xa5, 0x80, 0x01, 0x00, 0x00,        // lea rsp,[rbp+0x180]
                                    0x5d, 0xc3});
    const uint32_t len = 1 + 7 + 8 + 8 + 8 + 9 + 8 + 6 + 7 + 2;
    im.add_fn(f, f + len,
              im.add_unwind(1, 0, 49, RBP, 8,
                            {uc(49, SET_FPREG, 0), uc(41, SAVE_XMM_FAR, 15), 0xc0, 0x0000, uc(32, SAVE_NONVOL_FAR, R12),
                             0x108, 0x0000, uc(24, SAVE_XMM, 6), 0xe0 / 16, uc(16, SAVE_NONVOL, RBX), 0x110 / 8,
                             uc(8, ALLOC_LARGE, 0), 0x200 / 8, uc(1, PUSH, RBP)}));
    im.finish();
    stack st;
    const uint64_t E = st.E(), F = E - 8 - 0x200;
    st.put(E, kRet);
    st.put(E - 8, orig(RBP));
    st.put(F + 0x110, orig(RBX));
    st.put(F + 0x108, orig(R12));
    st.put128(F + 0xe0, 0x6666000000000006ull, 0x6666000000000106ll);
    st.put128(F + 0xc0, 0xfff000000000000full, 0xfff000000000010fll);
    auto xmm_ok = [](const gta6rt_x64_context& c, int x, uint64_t lo, uint64_t hi) {
        return c.fp.v.xmm[x].lo == lo && static_cast<uint64_t>(c.fp.v.xmm[x].hi) == hi;
    };
    {
        gta6rt_x64_context c;
        base_ctx(&c, im.at(f + 49 + 4), F - 0x40);
        c.rbp = F + 0x80;
        c.rbx = junk(RBX);
        c.r12 = junk(R12);
        c.fp.v.xmm[6].lo = c.fp.v.xmm[15].lo = 0;
        result r = unwind(im, c);
        CHECK(r.st == GTA6RT_OK && r.c.rip == kRet && r.c.rsp == E + 8);
        CHECK(r.c.rbp == orig(RBP) && r.c.rbx == orig(RBX) && r.c.r12 == orig(R12));
        CHECK(xmm_ok(r.c, 6, 0x6666000000000006ull, 0x6666000000000106ull));
        CHECK(xmm_ok(r.c, 15, 0xfff000000000000full, 0xfff000000000010full));
        CHECK(r.establisher == F);

        KNONVOLATILE_CONTEXT_POINTERS ptrs;
        memset(&ptrs, 0, sizeof ptrs);
        CONTEXT wc;
        memcpy(&wc, &c, sizeof wc);
        DWORD64 base = 0, est = 0;
        PVOID hd = nullptr;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.rip, &base, nullptr);
        CHECK(fe != nullptr && base == im.base);
        PEXCEPTION_ROUTINE h = RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.rip, fe, &wc, &hd, &est, &ptrs);
        CHECK(h == nullptr && est == F && wc.Rip == kRet && wc.Rsp == E + 8 && wc.Rbx == orig(RBX));
        CHECK(ptrs.IntegerContext[RBX] == reinterpret_cast<PDWORD64>(F + 0x110));
        CHECK(ptrs.IntegerContext[R12] == reinterpret_cast<PDWORD64>(F + 0x108));
        CHECK(ptrs.IntegerContext[RBP] == reinterpret_cast<PDWORD64>(E - 8));
        CHECK(ptrs.FloatingContext[6] == reinterpret_cast<PM128A>(F + 0xe0));
        CHECK(ptrs.FloatingContext[15] == reinterpret_cast<PM128A>(F + 0xc0));
    }
    {
        // Inside the prolog before SET_FPREG, rbp still holds the caller's value; the frame base must be rsp.
        gta6rt_x64_context c;
        base_ctx(&c, im.at(f + 41), F);
        c.rbx = junk(RBX);
        c.r12 = junk(R12);
        result r = unwind(im, c);
        CHECK(r.st == GTA6RT_OK && r.c.rip == kRet && r.c.rsp == E + 8 && r.establisher == F);
        CHECK(r.c.rbp == orig(RBP) && r.c.rbx == orig(RBX) && r.c.r12 == orig(R12));
        CHECK(xmm_ok(r.c, 15, 0xfff000000000000full, 0xfff000000000010full));
    }
    {
        gta6rt_x64_context c;
        base_ctx(&c, im.at(f + 16), F);
        c.rbx = junk(RBX);
        result r = unwind(im, c);
        CHECK(r.st == GTA6RT_OK && r.c.rip == kRet && r.c.rsp == E + 8 && r.c.rbx == orig(RBX) && r.c.r12 == orig(R12));
    }
    {
        gta6rt_x64_context c;
        base_ctx(&c, im.at(f + 49 + 6), F - 0x40);
        c.rbp = F + 0x80;
        result r = unwind(im, c);
        CHECK(r.st == GTA6RT_OK && r.c.rip == kRet && r.c.rsp == E + 8 && r.c.rbp == orig(RBP));
        CHECK(r.establisher == F);
        base_ctx(&c, im.at(f + 49 + 6 + 7), E - 8);
        r = unwind(im, c);
        CHECK(r.st == GTA6RT_OK && r.c.rip == kRet && r.c.rsp == E + 8 && r.c.rbp == orig(RBP));
    }
}

void test_machframe(image& im) {
    printf("PUSH_MACHFRAME with and without error code\n");
    const uint32_t m0 = im.add_code({0x90, 0x90, 0x90, 0x90});
    const uint32_t m1 = im.add_code({0x90, 0x90, 0x90, 0x90});
    im.add_fn(m0, m0 + 4, im.add_unwind(1, 0, 0, 0, 0, {uc(0, MACHFRAME, 0)}));
    im.add_fn(m1, m1 + 4, im.add_unwind(1, 0, 0, 0, 0, {uc(0, MACHFRAME, 1)}));
    im.finish();
    stack st;
    const uint64_t S = st.E() - 0x100;
    for (int k = 0; k < 2; ++k) {
        const uint64_t mf = S + (k ? 8 : 0);
        st.put(S, 0xe77);
        st.put(mf, kRet);
        st.put(mf + 8, 0x33);
        st.put(mf + 16, 0x202);
        st.put(mf + 24, st.E() + 0x40);
        st.put(mf + 32, 0x2b);
        gta6rt_x64_context c;
        base_ctx(&c, im.at((k ? m1 : m0) + 1), S);
        result r = unwind(im, c);
        CHECK(r.st == GTA6RT_OK && r.c.rip == kRet && r.c.rsp == st.E() + 0x40);
    }
}

void test_chained(image& im) {
    printf("chained unwind info, jumps between fragments vs tail calls\n");
    const uint32_t p = im.add_code({0x53, 0x55, 0x48, 0x83, 0xec, 0x20, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                                    0x48, 0x83, 0xc4, 0x20, 0x5d, 0x5b, 0xc3});
    const uint32_t r = im.add_code({0xc3});
    const uint32_t q = im.add_code({0x90, 0x90,                          // body
                                    0xe9, 0, 0, 0, 0,                    // jmp p+8 (same function)
                                    0x48, 0x83, 0xc4, 0x20, 0x5d, 0x5b,  // epilog ...
                                    0xe9, 0, 0, 0, 0});                  // ... tail call to r
    im.patch_rel32(q + 2, p + 8);
    im.patch_rel32(q + 13, r);
    const uint32_t pu = im.add_unwind(1, 0, 6, 0, 0, {uc(6, ALLOC_SMALL, 3), uc(2, PUSH, RBP), uc(1, PUSH, RBX)});
    im.add_fn(p, p + 21, pu);
    im.add_fn(r, r + 1, im.add_unwind(1, 0, 0, 0, 0, {}));
    im.add_fn(q, q + 18, im.add_unwind(1, 4, 0, 0, 0, {}, {p, p + 21, pu}));
    im.finish();
    stack st;
    const uint64_t E = st.E();
    st.put(E, kRet);
    st.put(E - 8, orig(RBX));
    st.put(E - 16, orig(RBP));
    for (uint32_t off : {0u, 2u, 7u}) {
        gta6rt_x64_context c;
        base_ctx(&c, im.at(q + off), E - 16 - 0x20);
        c.rbx = junk(RBX);
        c.rbp = junk(RBP);
        uint64_t base = 0;
        const gta6rt_runtime_function* fe = gta6rt_seh_lookup(c.rip, &base);
        CHECK(fe && fe->begin_rva == q);
        result res = unwind(im, c);
        CHECK(res.st == GTA6RT_OK && res.c.rip == kRet && res.c.rsp == E + 8);
        CHECK(res.c.rbx == orig(RBX) && res.c.rbp == orig(RBP));
    }
    {
        // At the tail-call jmp itself: an epilog, so only the return address is popped.
        gta6rt_x64_context c;
        base_ctx(&c, im.at(q + 13), E);
        result res = unwind(im, c);
        CHECK(res.st == GTA6RT_OK && res.c.rip == kRet && res.c.rsp == E + 8 && res.c.rbx == orig(RBX));
        // At the jmp into the parent fragment: not an epilog, the parent's codes apply.
        base_ctx(&c, im.at(q + 2), E - 16 - 0x20);
        c.rbx = junk(RBX);
        res = unwind(im, c);
        CHECK(res.c.rbx == orig(RBX) && res.c.rsp == E + 8);
    }
}

void test_v2(image& im) {
    printf("version 2 UNWIND_INFO (UWOP_EPILOG), packaged vcruntime140 memcpy shape and an odd EPILOG count\n");
    const uint32_t a = im.add_code({0x57, 0x56, 0x48, 0x8b, 0xf9, 0x48, 0x8b, 0xf2, 0x49, 0x8b, 0xc8, 0xf3, 0xa4,
                                    0x5e, 0x5f, 0xc3});
    im.add_fn(a, a + 16, im.add_unwind(2, 0, 2, 0, 0, {0x1603, 0x0600, uc(2, PUSH, RSI), uc(1, PUSH, RDI)}));
    const uint32_t b = im.add_code({0x48, 0x83, 0xec, 0x28, 0x90, 0x90, 0x48, 0x83, 0xc4, 0x28, 0xc3});
    im.add_fn(b, b + 11, im.add_unwind(2, 0, 4, 0, 0, {0x1601, uc(4, ALLOC_SMALL, 4)}));
    const uint32_t h = im.add_code({0x48, 0x83, 0xec, 0x28, 0x90, 0x90, 0x48, 0x83, 0xc4, 0x28, 0xc3, 0xcc});
    im.add_fn(h, h + 12, im.add_unwind(2, 0, 4, 0, 0, {0x0601, 0x0602, uc(4, ALLOC_SMALL, 4)}));
    im.finish();
    stack st;
    const uint64_t E = st.E();
    st.put(E, kRet);
    st.put(E - 8, orig(RDI));
    st.put(E - 16, orig(RSI));
    struct pt {
        uint32_t off;
        uint64_t rsp;
        bool rdi_saved, rsi_saved, epi;
    } pts[] = {{0, E, false, false, false},  {1, E - 8, true, false, false}, {5, E - 16, true, true, false},
               {13, E - 16, true, true, true}, {14, E - 8, true, false, true}, {15, E, false, false, true}};
    for (const pt& p : pts) {
        gta6rt_x64_context c;
        base_ctx(&c, im.at(a + p.off), p.rsp);
        if (p.rdi_saved) c.rdi = junk(RDI);
        if (p.rsi_saved) c.rsi = junk(RSI);
        result r = unwind(im, c);
        CHECK(r.st == GTA6RT_OK && r.c.rip == kRet && r.c.rsp == E + 8);
        CHECK(r.c.rdi == orig(RDI) && r.c.rsi == orig(RSI));
    }
    for (uint32_t fn : {b, h}) {
        for (uint32_t off : {4u, 6u, 10u}) {
            gta6rt_x64_context c;
            base_ctx(&c, im.at(fn + off), off == 10 ? E : E - 0x28);
            result r = unwind(im, c);
            CHECK(r.st == GTA6RT_OK && r.c.rip == kRet && r.c.rsp == E + 8);
            if (r.c.rsp != E + 8) printf("    v2 fn +%x off %u: rsp %+lld\n", fn, off, (long long)(r.c.rsp - E - 8));
        }
    }
}

void test_handlers(image& im) {
    printf("language handler, handler data, handler type filter\n");
    const uint32_t f = im.add_code({0x53, 0x48, 0x83, 0xec, 0x20, 0x90, 0x90, 0x90, 0x90, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
    const uint32_t g = im.add_code({0x53, 0x48, 0x83, 0xec, 0x20, 0x90, 0x90, 0x90, 0x90, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
    const uint32_t fu = im.add_unwind(1, 3, 5, 0, 0, {uc(5, ALLOC_SMALL, 3), uc(1, PUSH, RBX)}, {0x5000, 0xdeadbeef});
    const uint32_t gu = im.add_unwind(1, 2, 5, 0, 0, {uc(5, ALLOC_SMALL, 3), uc(1, PUSH, RBX)}, {0x5100, 0xfeedf00d});
    im.add_fn(f, f + 15, fu);
    im.add_fn(g, g + 15, gu);
    im.finish();
    stack st;
    const uint64_t E = st.E();
    st.put(E, kRet);
    st.put(E - 8, orig(RBX));
    gta6rt_x64_context c;
    base_ctx(&c, im.at(f + 6), E - 8 - 0x20);
    result r = unwind(im, c, UNW_FLAG_EHANDLER);
    CHECK(r.handler == im.at(0x5000));
    CHECK(r.data == reinterpret_cast<void*>(im.at(fu + 4 + 2 * 2 + 4)));
    CHECK(*static_cast<uint32_t*>(r.data) == 0xdeadbeef);
    CHECK(r.establisher == E - 8 - 0x20);
    r = unwind(im, c, UNW_FLAG_NHANDLER);
    CHECK(r.handler == 0);
    base_ctx(&c, im.at(f + 1), E - 8);
    r = unwind(im, c, UNW_FLAG_EHANDLER);
    CHECK(r.handler == 0 && r.c.rip == kRet);
    base_ctx(&c, im.at(f + 9), E - 8 - 0x20);
    r = unwind(im, c, UNW_FLAG_EHANDLER);
    CHECK(r.handler == 0 && r.c.rip == kRet);
    base_ctx(&c, im.at(g + 6), E - 8 - 0x20);
    r = unwind(im, c, UNW_FLAG_EHANDLER);
    CHECK(r.handler == 0);
    r = unwind(im, c, UNW_FLAG_UHANDLER);
    CHECK(r.handler == im.at(0x5100) && *static_cast<uint32_t*>(r.data) == 0xfeedf00d);
}

void test_terminators(image& im) {
    printf("epilog terminators (WRK rule)\n");
    struct tcase {
        const char* name;
        std::vector<uint8_t> tail;
        bool epilog;
        uint32_t rva = 0, len = 0;
    } cases[] = {
        {"ret", {0xc3}, true},
        {"rep ret", {0xf3, 0xc3}, true},
        {"ret imm16", {0xc2, 0x00, 0x00}, true},
        {"rex.w jmp rax", {0x48, 0xff, 0xe0}, true},
        {"rex.w jmp [rax]", {0x48, 0xff, 0x20}, true},
        {"jmp [rip+0]", {0xff, 0x25, 0, 0, 0, 0}, true},
        {"jmp rax (no rex.w)", {0xff, 0xe0}, false},
        {"jmp rel8 inside", {0xeb, 0xfb}, false},
        {"jmp rel32 to own start", {0xe9, 0, 0, 0, 0}, true},
        {"call", {0xe8, 0, 0, 0, 0}, false},
    };
    for (tcase& t : cases) {
        std::vector<uint8_t> b = {0x53, 0x90, 0x90, 0x5b};
        b.insert(b.end(), t.tail.begin(), t.tail.end());
        im.code = (im.code + 15) & ~15u;
        t.rva = im.code;
        for (uint8_t x : b) im.mem[im.code++] = x;
        t.len = static_cast<uint32_t>(b.size());
        if (!strcmp(t.name, "jmp rel32 to own start")) im.patch_rel32(t.rva + 4, t.rva);
        im.add_fn(t.rva, t.rva + t.len, im.add_unwind(1, 0, 1, 0, 0, {uc(1, PUSH, RBX)}));
    }
    const uint32_t r12f = im.add_code({0x41, 0x54, 0x90, 0x90, 0x41, 0x5c, 0xc3});
    im.add_fn(r12f, r12f + 7, im.add_unwind(1, 0, 2, 0, 0, {uc(2, PUSH, R12)}));
    im.finish();
    stack st;
    const uint64_t E = st.E();
    st.put(E, kRet);
    st.put(E - 8, orig(RBX));
    for (const tcase& t : cases) {
        gta6rt_x64_context c;
        base_ctx(&c, im.at(t.rva + 4), E);
        uint64_t base = 0;
        const gta6rt_runtime_function* fe = gta6rt_seh_lookup(c.rip, &base);
        gta6rt_x64_context w = c;
        uint64_t est = 0, h = 0;
        void* hd = nullptr;
        gta6rt_seh_virtual_unwind(0, base, c.rip, fe, &w, &hd, &est, &h);
        const bool looks_epilog = w.rip == kRet && w.rsp == E + 8 && w.rbx == orig(RBX);
        CHECK(looks_epilog == t.epilog);
        if (looks_epilog != t.epilog) printf("    case '%s': epilog=%d expected %d\n", t.name, looks_epilog, t.epilog);
        base_ctx(&c, im.at(t.rva + 3), E - 8);
        c.rbx = junk(RBX);
        result r = unwind(im, c);
        CHECK(r.c.rip == kRet && r.c.rsp == E + 8 && r.c.rbx == orig(RBX));
    }
    st.put(E - 8, orig(R12));
    gta6rt_x64_context c;
    base_ctx(&c, im.at(r12f + 4), E - 8);
    c.r12 = junk(R12);
    result r = unwind(im, c);
    CHECK(r.c.rip == kRet && r.c.rsp == E + 8 && r.c.r12 == orig(R12));
}

void test_tables_backtrace(image& im) {
    printf("lookup, indirect entries, dynamic tables, leaf rule, backtrace\n");
    const uint32_t a = im.add_code({0x53, 0x56, 0x48, 0x83, 0xec, 0x28, 0x90, 0x90, 0x48, 0x83, 0xc4, 0x28, 0x5e, 0x5b, 0xc3});
    const uint32_t b = im.add_code({0x53, 0x56, 0x48, 0x83, 0xec, 0x28, 0x90, 0x90, 0x48, 0x83, 0xc4, 0x28, 0x5e, 0x5b, 0xc3});
    const uint32_t ua = im.add_unwind(1, 0, 6, 0, 0, {uc(6, ALLOC_SMALL, 4), uc(2, PUSH, RSI), uc(1, PUSH, RBX)});
    im.add_fn(a, a + 15, ua);
    im.xdata = (im.xdata + 3) & ~3u;
    const uint32_t indirect = im.xdata;
    const gta6rt_runtime_function real{b, b + 15, ua};
    memcpy(im.mem + indirect, &real, sizeof real);
    im.xdata += 12;
    im.add_fn(b, b + 15, indirect | 1);
    im.finish();
    const uint32_t leaf = 0x3800;
    uint64_t base = 0;
    const gta6rt_runtime_function* fe = gta6rt_seh_lookup(im.at(b + 7), &base);
    CHECK(fe && fe->unwind_info_rva == ua && fe->begin_rva == b);
    CHECK(gta6rt_seh_lookup(im.at(leaf), &base) == nullptr && base == 0);
    CHECK(gta6rt_seh_image_base_of(im.at(leaf)) == im.base);
    CHECK(gta6rt_seh_lookup(im.at(a) - 0x100000, nullptr) == nullptr);
    PVOID hdr = nullptr;
    CHECK(RtlPcToFileHeader(reinterpret_cast<PVOID>(im.at(a + 3)), &hdr) == reinterpret_cast<PVOID>(im.base));

    stack st;
    const uint64_t S0 = st.E() - 0x200, S1 = S0 + 0x40, S2 = S1 + 0x40;
    st.put(S0 + 0x38, im.at(b + 7));
    st.put(S1 + 0x38, im.at(leaf));
    st.put(S2, 0x1234);
    gta6rt_x64_context c;
    base_ctx(&c, im.at(a + 7), S0);
    uint64_t pcs[16];
    const uint32_t n = gta6rt_seh_backtrace(&c, pcs, 16);
    CHECK(n == 4);
    if (n == 4) CHECK(pcs[0] == im.at(a + 7) && pcs[1] == im.at(b + 7) && pcs[2] == im.at(leaf) && pcs[3] == 0x1234);
    CHECK(c.rip == im.at(a + 7));

    auto* dyn = static_cast<uint8_t*>(mmap(nullptr, 0x2000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    const uint64_t dbase = reinterpret_cast<uint64_t>(dyn);
    memcpy(dyn + 0x100, im.mem + ua, 16);
    static RUNTIME_FUNCTION dt[2];
    dt[0] = {0x800, 0x900, 0x100};
    dt[1] = {0x400, 0x500, 0x100};
    CHECK(RtlAddFunctionTable(dt, 2, dbase) == TRUE);
    CHECK(RtlLookupFunctionEntry(dbase + 0x450, &base, nullptr) == &dt[1] && base == dbase);
    CHECK(RtlLookupFunctionEntry(dbase + 0x8ff, &base, nullptr) == &dt[0]);
    CHECK(RtlLookupFunctionEntry(dbase + 0x600, &base, nullptr) == nullptr);
    CHECK(RtlDeleteFunctionTable(dt) == TRUE);
    CHECK(RtlLookupFunctionEntry(dbase + 0x450, &base, nullptr) == nullptr);
    CHECK(RtlDeleteFunctionTable(dt) == FALSE);
}

void test_api_misc() {
    printf("policy and export tables\n");
    CHECK(gta6rt_seh_init(GTA6RT_SEH_LOG_AND_ABORT) == GTA6RT_OK);
    CHECK(gta6rt_seh_get_policy() == GTA6RT_SEH_LOG_AND_ABORT);
    CHECK(gta6rt_seh_set_policy(GTA6RT_SEH_DISPATCH) == GTA6RT_OK && gta6rt_seh_get_policy() == GTA6RT_SEH_DISPATCH);
    CHECK(gta6rt_seh_set_policy(static_cast<gta6rt_seh_policy>(7)) == GTA6RT_E_INVALID_ARG);
    CHECK(gta6rt_seh_set_policy(GTA6RT_SEH_LOG_AND_ABORT) == GTA6RT_OK);
    const char* names[] = {"RtlCaptureContext", "RtlLookupFunctionEntry", "RtlVirtualUnwind", "RtlUnwindEx", "RtlUnwind",
                           "RtlPcToFileHeader", "RtlAddFunctionTable", "RtlDeleteFunctionTable",
                           "RtlInstallFunctionTableCallback", "RtlRestoreContext", "RtlRaiseException",
                           "RtlCaptureStackBackTrace", "RaiseException", "AddVectoredExceptionHandler",
                           "RemoveVectoredExceptionHandler", "AddVectoredContinueHandler", "RemoveVectoredContinueHandler",
                           "SetUnhandledExceptionFilter", "UnhandledExceptionFilter"};
    for (const char* n : names) {
#if defined(__x86_64__)
        CHECK(gta6rt_seh_native_export(n) != 0);
#else
        CHECK(gta6rt_seh_native_export(n) == 0);
#endif
    }
    CHECK(gta6rt_seh_native_export("CreateFileW") == 0);
    for (const char* n : {"_CxxThrowException", "__CxxFrameHandler3", "__CxxFrameHandler4", "__C_specific_handler"}) {
#if defined(__x86_64__)
        CHECK(gta6rt_seh_vcruntime_fallback(n) != 0);
#else
        CHECK(gta6rt_seh_vcruntime_fallback(n) == 0);
#endif
    }
    void* cookie = nullptr;
    CHECK(gta6rt_seh_add_vectored_handler(1, 0x1000, &cookie) == GTA6RT_OK && cookie);
    CHECK(gta6rt_seh_remove_vectored_handler(cookie) == GTA6RT_OK);
    CHECK(gta6rt_seh_remove_vectored_handler(cookie) == GTA6RT_E_NOT_FOUND);
    CHECK(gta6rt_seh_set_unhandled_filter(0x2000) == 0);
    CHECK(gta6rt_seh_set_unhandled_filter(0) == 0x2000);
}

#if defined(__x86_64__)

int g_veh_calls = 0;
uint64_t g_veh_param = 0;

LONG NTAPI test_veh(EXCEPTION_POINTERS* p) {
    if (p->ExceptionRecord->ExceptionCode != 0xE0001234u) return EXCEPTION_CONTINUE_SEARCH;
    ++g_veh_calls;
    if (p->ExceptionRecord->NumberParameters == 2) g_veh_param = p->ExceptionRecord->ExceptionInformation[1];
    return EXCEPTION_CONTINUE_EXECUTION;
}

__attribute__((noinline)) int capture_restore_roundtrip() {
    volatile int n = 0;
    alignas(16) CONTEXT c;
    RtlCaptureContext(&c);
    const int seen = n;
    n = seen + 1;
    if (seen == 0) RtlRestoreContext(&c, nullptr);
    return n;
}

void test_x86_runtime() {
    printf("x86_64: RtlCaptureContext/RtlRestoreContext, RaiseException + vectored handler, signal conversion\n");
    CHECK(capture_restore_roundtrip() == 2);

    alignas(16) CONTEXT c;
    RtlCaptureContext(&c);
    const uint64_t sp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
    CHECK((c.ContextFlags & CONTEXT_FULL) == CONTEXT_FULL && c.Rsp < sp && sp - c.Rsp < 0x10000);

    PVOID cookie = AddVectoredExceptionHandler(1, test_veh);
    CHECK(cookie != nullptr);
    const ULONG_PTR args[2] = {7, 0x99887766};
    volatile int after = 0;
    RaiseException(0xE0001234u, 0, 2, args);
    after = 1;
    CHECK(after == 1 && g_veh_calls == 1 && g_veh_param == 0x99887766);
    CHECK(RemoveVectoredExceptionHandler(cookie) == 1);
    RaiseException(0x406D1388u, 0, 0, nullptr);
    CHECK(true);

    _STRUCT_MCONTEXT_AVX64 mc;
    memset(&mc, 0, sizeof mc);
    ucontext_t u;
    memset(&u, 0, sizeof u);
    u.uc_mcontext = reinterpret_cast<mcontext_t>(&mc);
    u.uc_mcsize = sizeof mc;
    alignas(16) static uint8_t code[16] = {0xcc, 0x90, 0xcd, 0x29, 0x90};
    mc.__ss.__rax = 1;
    mc.__ss.__rbx = 2;
    mc.__ss.__rcx = 7;
    mc.__ss.__r15 = 15;
    mc.__ss.__rsp = 0x7000;
    mc.__ss.__rflags = 0x246;
    mc.__fs.__fpu_mxcsr = 0x1f80;
    memset(&mc.__fs.__fpu_xmm6, 0x66, 16);
    gta6rt_x64_context x;
    gta6rt_x64_exception_record rec;
    siginfo_t si;
    memset(&si, 0, sizeof si);

    mc.__ss.__rip = reinterpret_cast<uint64_t>(code) + 1;
    CHECK(gta6rt_seh_context_from_ucontext(&u, &x) == GTA6RT_OK);
    CHECK(x.rax == 1 && x.rbx == 2 && x.r15 == 15 && x.rsp == 0x7000 && x.eflags == 0x246 && x.mxcsr == 0x1f80);
    CHECK(x.fp.v.xmm[6].lo == 0x6666666666666666ull);
    CHECK(gta6rt_seh_record_from_signal(SIGTRAP, &si, &u, &x, &rec) == GTA6RT_OK);
    CHECK(rec.code == STATUS_BREAKPOINT && x.rip == reinterpret_cast<uint64_t>(code) && rec.address == x.rip);

    mc.__ss.__rip = reinterpret_cast<uint64_t>(code) + 2;
    gta6rt_seh_context_from_ucontext(&u, &x);
    gta6rt_seh_record_from_signal(SIGSEGV, &si, &u, &x, &rec);
    CHECK(rec.code == STATUS_STACK_BUFFER_OVERRUN && rec.n_params == 1 && rec.params[0] == 7);

    mc.__ss.__rip = reinterpret_cast<uint64_t>(code) + 4;
    si.si_addr = reinterpret_cast<void*>(0x10);
    mc.__es.__err = 4;
    gta6rt_seh_context_from_ucontext(&u, &x);
    gta6rt_seh_record_from_signal(SIGSEGV, &si, &u, &x, &rec);
    CHECK(rec.code == STATUS_ACCESS_VIOLATION && rec.n_params == 2 && rec.params[0] == 0 && rec.params[1] == 0x10);
    mc.__es.__err = 6;
    gta6rt_seh_record_from_signal(SIGBUS, &si, &u, &x, &rec);
    CHECK(rec.code == STATUS_ACCESS_VIOLATION && rec.params[0] == 1);
    si.si_code = FPE_INTDIV;
    gta6rt_seh_record_from_signal(SIGFPE, &si, &u, &x, &rec);
    CHECK(rec.code == STATUS_INTEGER_DIVIDE_BY_ZERO);
    si.si_code = ILL_ILLOPC;
    gta6rt_seh_record_from_signal(SIGILL, &si, &u, &x, &rec);
    CHECK(rec.code == STATUS_ILLEGAL_INSTRUCTION);

    x.rbx = 0x4242;
    x.rip = 0x5000;
    CHECK(gta6rt_seh_context_to_ucontext(&x, &u) == GTA6RT_OK);
    CHECK(mc.__ss.__rbx == 0x4242 && mc.__ss.__rip == 0x5000);
}

#endif

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    test_api_misc();
    {
        image im;
        test_canonical(im);
    }
    {
        image im;
        test_frame_and_far(im);
    }
    {
        image im;
        test_machframe(im);
    }
    {
        image im;
        test_chained(im);
    }
    {
        image im;
        test_v2(im);
    }
    {
        image im;
        test_handlers(im);
    }
    {
        image im;
        test_terminators(im);
    }
    {
        image im;
        test_tables_backtrace(im);
    }
#if defined(__x86_64__)
    test_x86_runtime();
#else
    alignas(16) CONTEXT c;
    RtlCaptureContext(&c);
    CHECK(c.ContextFlags == CONTEXT_FULL && c.Rip == 0);
#endif
    printf("seh_unit_test: %d checks, %d failed: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
