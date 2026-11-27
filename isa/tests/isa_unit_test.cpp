// Backend-independent tests: SSE4a semantics vs a bit-loop reference, decoders, CPUID policy, the VEX
// codec against the title's own encodings, TEB/TLS bookkeeping, and the image fixups on the real exe.
// Runs on both arches.

#include <gta6rt/isa.h>

#include "isa_internal.h"
#include "sse4a.h"
#include "x86_patch.h"
#include "test_common.h"

#include <pthread.h>

#include <atomic>

using namespace gta6rt::isa;

namespace gta6rt::isa {
struct TitleSite {
    uint32_t rva;
    uint8_t kind;
    uint8_t len;
    uint8_t bytes[15];
};
#include "title_sites.inc"
}  // namespace gta6rt::isa

static uint64_t ref_extrq(uint64_t v, unsigned len, unsigned idx) {
    const unsigned fl = (len & 63) ? (len & 63) : 64, sh = idx & 63;
    uint64_t r = 0;
    for (unsigned b = 0; b < fl && sh + b < 64; ++b)
        if ((v >> (sh + b)) & 1) r |= 1ull << b;
    return r;
}

static uint64_t ref_insertq(uint64_t d, uint64_t s, unsigned len, unsigned idx) {
    const unsigned fl = (len & 63) ? (len & 63) : 64, sh = idx & 63;
    uint64_t r = d;
    for (unsigned b = 0; b < fl && sh + b < 64; ++b) {
        r &= ~(1ull << (sh + b));
        if ((s >> b) & 1) r |= 1ull << (sh + b);
    }
    return r;
}

static void test_sse4a_semantics() {
    Rng g(1);
    int bad = 0;
    for (int i = 0; i < 300000; ++i) {
        const uint64_t a = g.next(), b = g.next();
        const unsigned len = g.next() & 63, idx = g.next() & 63;
        if (sse4a_extrq(a, len, idx) != ref_extrq(a, len, idx)) ++bad;
        if (sse4a_insertq(a, b, len, idx) != ref_insertq(a, b, len, idx)) ++bad;
    }
    for (unsigned len = 0; len < 64; ++len)
        for (unsigned idx = 0; idx < 64; ++idx) {
            const uint64_t a = 0xfedcba9876543210ull, b = 0x0123456789abcdefull;
            if (sse4a_extrq(a, len, idx) != ref_extrq(a, len, idx)) ++bad;
            if (sse4a_insertq(a, b, len, idx) != ref_insertq(a, b, len, idx)) ++bad;
        }
    CHECK(bad == 0, "%d mismatches vs bit-loop reference", bad);
}

static void test_sse4a_decode() {
    struct D {
        std::vector<uint8_t> b;
        bool ok;
        Sse4aOp op;
        int dst, src, len, idx, n;
    } cases[] = {
        {{0xf2, 0x44, 0x0f, 0x78, 0xcf, 0x08, 0x00}, true, Sse4aOp::kInsertqImm, 9, 7, 8, 0, 7},
        {{0xf2, 0x45, 0x0f, 0x78, 0xdc, 0x10, 0x10}, true, Sse4aOp::kInsertqImm, 11, 12, 16, 16, 7},
        {{0xf2, 0x0f, 0x78, 0xe5, 0x10, 0x30}, true, Sse4aOp::kInsertqImm, 4, 5, 16, 48, 6},
        {{0x66, 0x0f, 0x78, 0xc0, 0x10, 0x08}, true, Sse4aOp::kExtrqImm, 0, 0, 16, 8, 6},
        {{0x66, 0x41, 0x0f, 0x78, 0xc6, 0x05, 0x3b}, true, Sse4aOp::kExtrqImm, 14, 0, 5, 59, 7},
        {{0x66, 0x0f, 0x79, 0xc1}, true, Sse4aOp::kExtrqReg, 0, 1, 0, 0, 4},
        {{0xf2, 0x0f, 0x79, 0xc1}, true, Sse4aOp::kInsertqReg, 0, 1, 0, 0, 4},
        {{0xf2, 0x45, 0x0f, 0x79, 0xfe}, true, Sse4aOp::kInsertqReg, 15, 14, 0, 0, 5},
        {{0x66, 0x0f, 0x78, 0xc8, 0x10, 0x08}, false, Sse4aOp::kNone, 0, 0, 0, 0, 0},  // /1 is not extrq
        {{0x66, 0x0f, 0x79, 0x01}, false, Sse4aOp::kNone, 0, 0, 0, 0, 0},              // memory form
        {{0x0f, 0x79, 0xc1}, false, Sse4aOp::kNone, 0, 0, 0, 0, 0},                    // vmread
        {{0xf3, 0x0f, 0x78, 0xc1, 1, 2}, false, Sse4aOp::kNone, 0, 0, 0, 0, 0},
    };
    for (auto& c : cases) {
        Sse4aInsn in;
        const bool ok = sse4a_decode(c.b.data(), c.b.size(), &in);
        CHECK(ok == c.ok, "decode ok=%d for case starting %02x %02x", ok, c.b[0], c.b[1]);
        if (ok && c.ok) {
            CHECK(in.op == c.op && in.dst == c.dst && in.src == c.src && in.len == c.len && in.idx == c.idx &&
                      in.length == c.n,
                  "decode fields op=%d dst=%d src=%d len=%d idx=%d n=%d", (int)in.op, in.dst, in.src, in.len, in.idx,
                  in.length);
        }
    }
    int n = 0;
    for (const TitleSite& s : kTitleSites) {
        if (s.kind != 2) continue;
        Sse4aInsn in;
        const bool ok = sse4a_decode(s.bytes, s.len, &in);
        CHECK(ok && in.op == Sse4aOp::kInsertqImm && in.length == s.len, "title site 0x%x decodes", s.rva);
        ++n;
    }
    CHECK(n == 19, "title has %d sse4a sites, expected 19", n);
}

static void test_cpuid_policy() {
    gta6rt_cpuid_regs r{~0u, ~0u, ~0u, ~0u};
    cpuid_apply_policy(1, 0, GTA6RT_CPUID_DEFAULT, &r);
    CHECK((r.ecx >> 28) & 1 && (r.ecx >> 27) & 1 && (r.ecx >> 12) & 1 && (r.ecx >> 29) & 1 && (r.ecx >> 22) & 1,
          "leaf1 ecx=%08x", r.ecx);
    r = {0, 0, 0, 0};
    cpuid_apply_policy(1, 0, GTA6RT_CPUID_DEFAULT, &r);
    CHECK(r.ecx == ((1u << 12) | (1u << 22) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29)), "leaf1 from zero ecx=%08x",
          r.ecx);
    r = {0, ~0u, ~0u, ~0u};
    cpuid_apply_policy(7, 0, GTA6RT_CPUID_DEFAULT, &r);
    CHECK(((r.ebx >> 3) & 1) && ((r.ebx >> 5) & 1) && ((r.ebx >> 8) & 1), "leaf7 bmi/avx2 ebx=%08x", r.ebx);
    CHECK(!((r.ebx >> 19) & 1) && !((r.ebx >> 29) & 1) && !((r.ebx >> 16) & 1) && !((r.ebx >> 0) & 1),
          "leaf7 adx/sha/avx512f/fsgsbase hidden ebx=%08x", r.ebx);
    CHECK(!((r.ecx >> 9) & 1) && !((r.ecx >> 10) & 1), "leaf7 vaes/vpclmulqdq hidden ecx=%08x", r.ecx);
    r = {0, 0, ~0u, ~0u};
    cpuid_apply_policy(0x80000001, 0, GTA6RT_CPUID_DEFAULT, &r);
    CHECK(((r.ecx >> 5) & 1) && !((r.ecx >> 6) & 1) && !((r.ecx >> 11) & 1) && !((r.ecx >> 29) & 1),
          "ext1 lzcnt on, sse4a/xop/monitorx off ecx=%08x", r.ecx);
    r = {0, 0, 0, 0};
    cpuid_apply_policy(0xD, 0, GTA6RT_CPUID_DEFAULT, &r);
    CHECK(r.eax == 7 && r.ebx == 0x340 && r.ecx == 0x340, "leaf 0xD.0 eax=%x ebx=%x ecx=%x", r.eax, r.ebx, r.ecx);
    r = {~0u, ~0u, ~0u, ~0u};
    cpuid_apply_policy(1, 0, GTA6RT_CPUID_BMI1, &r);
    CHECK(!((r.ecx >> 28) & 1) && !((r.ecx >> 27) & 1) && !((r.ecx >> 12) & 1), "policy without AVX hides it");
    CHECK(policy_xcr0(GTA6RT_CPUID_DEFAULT, 0) == 7 && policy_xcr0(0, 0) == 3, "xcr0 policy");
    gta6rt_cpuid_regs v;
    CHECK(gta6rt_isa_cpuid(0x80000001, 0, &v) == GTA6RT_OK && !((v.ecx >> 6) & 1), "sse4a hidden from guest");
    CHECK(gta6rt_isa_cpuid(1, 0, &v) == GTA6RT_OK && ((v.ecx >> 28) & 1), "avx advertised to guest");
}

static void test_vex_codec() {
    int n = 0;
    for (const TitleSite& s : kTitleSites) {
        if (s.kind != 3 && s.kind != 4) continue;
        VexInsn in;
        const uint64_t ip = 0x140000000ull + s.rva;
        const bool ok = vex_decode(s.bytes, s.len, ip, &in);
        CHECK(ok && in.length == s.len && in.map == 2 && in.w == 1 && in.pp == 1 &&
                  (in.opcode == 0x45 || in.opcode == 0x47),
              "vex decode site 0x%x", s.rva);
        if (!ok) continue;
        X86Emitter e(ip);
        if (in.rm_is_reg)
            e.vex_rr(in.map, in.pp, in.w, in.l, in.opcode, in.reg, in.vvvv, in.rm);
        else
            e.vex_rm(in.map, in.pp, in.w, in.l, in.opcode, in.reg, in.vvvv, in.mem);
        // Memory forms are re-encoded with disp32, so only register forms must round-trip byte for byte.
        if (in.rm_is_reg)
            CHECK(e.bytes().size() == s.len && !memcmp(e.bytes().data(), s.bytes, s.len), "re-encode site 0x%x", s.rva);
        else
            CHECK(in.mem.rip && in.mem.rip_target > ip, "rip operand at 0x%x", s.rva);
        if (s.kind == 3) {
            CHECK(in.rm_is_reg, "patch site 0x%x has a register count", s.rva);
            auto stub = make_vshift_stub(0x100000000ull, in, 0x100000000ull - 64);
            CHECK(stub.size() > 40 && stub.back() == 0xC3, "vshift stub for 0x%x", s.rva);
        }
        ++n;
    }
    CHECK(n == 112, "vshift sites %d, expected 112", n);
    // [rsp+0x10] and [r12+rax*4+0x100] memory operands round trip through decode -> encode -> decode.
    const uint8_t m1[] = {0xc4, 0xe2, 0xed, 0x45, 0x4c, 0x24, 0x10};  // vpsrlvq ymm1, ymm2, [rsp+0x10]
    VexInsn a;
    CHECK(vex_decode(m1, sizeof m1, 0, &a) && !a.rm_is_reg && a.mem.base == 4 && a.mem.index < 0 && a.mem.disp == 0x10 &&
              a.reg == 1 && a.vvvv == 2 && a.l == 1,
          "decode [rsp+0x10]");
    X86Mem m;
    m.base = 12;
    m.index = 0;
    m.scale = 2;
    m.disp = 0x100;
    X86Emitter e(0);
    e.vex_rm(2, 1, 1, 0, 0x47, 9, 3, m);
    VexInsn b;
    CHECK(vex_decode(e.bytes().data(), e.bytes().size(), 0, &b) && b.mem.base == 12 && b.mem.index == 0 &&
              b.mem.scale == 2 && b.mem.disp == 0x100 && b.reg == 9 && b.vvvv == 3 && b.opcode == 0x47,
          "encode/decode [r12+rax*4+0x100]");
    auto st = make_sse4a_stub(0, (uint8_t)Sse4aOp::kInsertqImm, 9, 7, 8, 0);
    CHECK(st.size() > 30 && st.back() == 0xC3, "sse4a stub");
}

struct TlsBlock {
    uint64_t tag;
};

static void* tls_ctor(uint32_t idx, uint32_t tid, void* user) {
    auto* b = new TlsBlock{(static_cast<uint64_t>(idx) << 32) | tid};
    static_cast<std::atomic<int>*>(user)->fetch_add(1);
    return b;
}

static void tls_dtor(void* b, void* user) {
    delete static_cast<TlsBlock*>(b);
    static_cast<std::atomic<int>*>(user)->fetch_sub(1);
}

static std::atomic<int> g_live_blocks{0};
static Barrier g_bar1, g_bar2, g_bar3;
static uint32_t g_slot_a, g_slot_b;

static void* worker(void*) {
    gta6rt_teb_desc d{};
    void* teb = nullptr;
    CHECK(gta6rt_isa_thread_attach(&d, &teb) == GTA6RT_OK && teb, "worker attach");
    auto* t = static_cast<uint8_t*>(teb);
    const uint32_t tid = static_cast<uint32_t>(teb_get(t, GTA6RT_TEB_CLIENT_ID_THREAD));
    CHECK(teb_get(t, GTA6RT_TEB_STACK_BASE) == reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(pthread_self())),
          "worker StackBase is its pthread stack top");
    gta6rt_isa_tls_slot_set(g_slot_a, reinterpret_cast<void*>(0x1000 + tid));
    gta6rt_isa_tls_slot_set(g_slot_b, reinterpret_cast<void*>(0x2000 + tid));
    g_bar1.wait();  // main registers late TLS images now
    g_bar2.wait();
    auto** arr = reinterpret_cast<TlsBlock**>(teb_get(t, GTA6RT_TEB_TLS_POINTER));
    CHECK(arr[0] && arr[0]->tag == tid, "worker tls[0]");
    CHECK(arr[1] && arr[1]->tag == ((1ull << 32) | tid), "worker late tls[1]");
    CHECK(arr[9] && arr[9]->tag == ((9ull << 32) | tid), "worker grown tls[9]");
    CHECK(gta6rt_isa_tls_slot_get(g_slot_a) == nullptr, "slot freed by main is cleared here");
    CHECK(gta6rt_isa_tls_slot_get(g_slot_b) == reinterpret_cast<void*>(0x2000 + tid), "slot b kept");
    g_bar3.wait();
    gta6rt_isa_thread_detach();
    CHECK(gta6rt_isa_current_teb() == nullptr, "detached");
    return nullptr;
}

static void test_teb_tls() {
    CHECK(gta6rt_isa_tls_register_image(0, tls_ctor, tls_dtor, &g_live_blocks) == GTA6RT_OK, "register tls 0");
    gta6rt_teb_desc d{};
    d.thread_id = 0x104;
    void* teb = nullptr;
    CHECK(gta6rt_isa_thread_attach(&d, &teb) == GTA6RT_OK && teb, "main attach");
    void* teb2 = nullptr;
    CHECK(gta6rt_isa_thread_attach(&d, &teb2) == GTA6RT_OK && teb2 == teb, "attach is idempotent");
    auto* t = static_cast<uint8_t*>(teb);
    CHECK(gta6rt_isa_current_teb() == teb, "current_teb");
    CHECK(teb_get(t, GTA6RT_TEB_SELF) == reinterpret_cast<uint64_t>(teb), "TEB.Self");
    CHECK(teb_get(t, GTA6RT_TEB_CLIENT_ID_THREAD) == 0x104, "ClientId.UniqueThread");
    auto** arr = reinterpret_cast<TlsBlock**>(teb_get(t, GTA6RT_TEB_TLS_POINTER));
    CHECK(arr && arr[0] && arr[0]->tag == 0x104, "main tls[0]");
    static int peb[512];
    CHECK(gta6rt_isa_set_peb(peb) == GTA6RT_OK && teb_get(t, GTA6RT_TEB_PEB) == reinterpret_cast<uint64_t>(peb), "PEB");

    CHECK(gta6rt_isa_tls_slot_alloc(&g_slot_a) == GTA6RT_OK, "slot alloc");
    uint32_t idx = 0;
    for (int i = 0; i < 70; ++i) gta6rt_isa_tls_slot_alloc(&idx);
    CHECK(idx >= 64, "allocated into expansion slots (%u)", idx);
    g_slot_b = idx;
    CHECK(gta6rt_isa_tls_slot_set(g_slot_b, (void*)0x77) == GTA6RT_OK && gta6rt_isa_tls_slot_get(g_slot_b) == (void*)0x77,
          "expansion slot set/get");

    g_bar1.init(4);
    g_bar2.init(4);
    g_bar3.init(4);
    pthread_t th[3];
    for (auto& x : th) pthread_create(&x, nullptr, worker, nullptr);
    g_bar1.wait();
    CHECK(gta6rt_isa_tls_register_image(1, tls_ctor, tls_dtor, &g_live_blocks) == GTA6RT_OK, "late tls 1");
    CHECK(gta6rt_isa_tls_register_image(9, tls_ctor, tls_dtor, &g_live_blocks) == GTA6RT_OK, "late tls 9 (grows)");
    CHECK(gta6rt_isa_tls_slot_free(g_slot_a) == GTA6RT_OK, "slot free");
    g_bar2.wait();
    g_bar3.wait();
    for (auto& x : th) pthread_join(x, nullptr);
    arr = reinterpret_cast<TlsBlock**>(teb_get(t, GTA6RT_TEB_TLS_POINTER));
    CHECK(arr[9] && arr[9]->tag == ((9ull << 32) | 0x104), "main grown tls[9]");
    CHECK(g_live_blocks.load() == 3, "blocks of exited threads freed (live=%d)", g_live_blocks.load());
    CHECK(gta6rt_isa_tls_unregister_image(9) == GTA6RT_OK && g_live_blocks.load() == 2, "unregister frees");

    gta6rt_teb_desc dup{};
    dup.stack_base = teb_get(t, GTA6RT_TEB_STACK_BASE);
    pthread_t x;
    static gta6rt_status dup_st;
    pthread_create(
        &x, nullptr,
        [](void* p) -> void* {
            dup_st = gta6rt_isa_thread_attach(static_cast<gta6rt_teb_desc*>(p), nullptr);
            return nullptr;
        },
        &dup);
    pthread_join(x, nullptr);
    CHECK(dup_st == GTA6RT_E_ALREADY_EXISTS, "duplicate StackBase refused (%d)", dup_st);
}

static void test_fixups(const char* exe) {
    MappedImage img;
    if (!map_pe(exe, &img)) {
        CHECK(false, "cannot map %s", exe);
        return;
    }
    const uint64_t base = reinterpret_cast<uint64_t>(img.base);
    CHECK(gta6rt_isa_find_chkstk(base, img.size) == base + 0x5647140, "find __chkstk");
    gta6rt_isa_fixup_report r{};
    CHECK(gta6rt_isa_apply_image_fixups(base, img.size, GTA6RT_ISA_FIXUP_ALL, &r) == GTA6RT_OK, "apply fixups");
    std::printf("fixups: known=%u chkstk=%u sse4a=%u vshift=%u vshift_safe=%u cpuid=%u xgetbv=%u mismatched=%u\n",
                r.known_image, r.chkstk, r.sse4a, r.vshift, r.vshift_safe, r.cpuid, r.xgetbv, r.mismatched);
    CHECK(r.known_image == 1 && r.chkstk == 1 && r.sse4a == 19 && r.vshift == 7 && r.vshift_safe == 105 && r.cpuid == 9 &&
              r.xgetbv == 2 && r.mismatched == 0,
          "fixup counts");
    CHECK(img.base[0x5647140] == 0xC3, "__chkstk patched to ret");
    int calls = 0;
    for (const TitleSite& s : kTitleSites)
        if ((s.kind == 2 || s.kind == 3) && img.base[s.rva] == 0xE8) ++calls;
    CHECK(calls == 26, "call rel32 written at %d of 26 sites", calls);
    // A second application finds nothing it recognises and must not touch anything.
    gta6rt_isa_fixup_report r2{};
    gta6rt_isa_apply_image_fixups(base, img.size, GTA6RT_ISA_FIXUP_ALL, &r2);
    CHECK(r2.known_image == 0 && r2.sse4a == 0 && r2.vshift == 0 && r2.cpuid == 0, "second pass is a no-op");
    munmap(img.base, img.size);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    gta6rt_isa_config cfg{GTA6RT_TEB_SHARE, 0};
    CHECK(gta6rt_isa_init(&cfg) == GTA6RT_OK, "init");
    std::printf("backend=%s\n", gta6rt_isa_backend_name());
    test_sse4a_semantics();
    test_sse4a_decode();
    test_cpuid_policy();
    test_vex_codec();
    test_teb_tls();
    test_fixups(exe_path(argc, argv));
    return finish("isa_unit_test");
}
