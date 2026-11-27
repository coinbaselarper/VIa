// Checks patches/acphal-xma-fixes.patch against shims_gdk/src/acphal.cpp, built twice in this tree (patched and
// unpatched) with the stand-in decoder in this directory. The harness (context layout, AcpHal command calls, ApuAlloc)
// follows shims_gdk/tests/acphal_test.cpp (GPL-3.0-or-later, from leonida).
//   blocks:  a stream whose packets do not fill a 128-sample block, then advance without samples (zero-frame packets,
//            as skipped frames and input boundaries do with the real decoder). Unpatched publishes a zero-padded block
//            mid-stream (the click); patched waits for a whole block.
//   revive:  the voice runs dry, then the title refills input 0 while the context stays enabled. Unpatched stays
//            ended until the next ENABLE_XMA; patched decodes the refill.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <gta6rt/shims.h>

#include "shims.h"

namespace {

using AcpHalCreateFn = HRESULT(__stdcall*)(void**);
using AcpHalShapeCtxFn = HRESULT(__stdcall*)(void*);
using ApuAllocFn = HRESULT(__stdcall*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t);
using ApuFreeFn = void(__stdcall*)(void*);
using ConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
using SubmitFn = HRESULT(__stdcall*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
using ReleaseFn = HRESULT(__stdcall*)(void*);

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct APU_ADDRESS {
    uint64_t value;
};
struct ShapeContexts {
    uint32_t numSrc, numEqComp, numFiltVol, numDma, numXma, numPcm;
    void *src, *eqComp, *filtVol, *dma, *xma, *pcm;
    APU_ADDRESS apu[6];
};
static_assert(sizeof(ShapeContexts) == 120);

struct Words {
    uint32_t* data;
    std::atomic_ref<uint32_t> operator[](size_t i) const { return std::atomic_ref(data[i]); }
};

void packet(uint8_t* p, uint32_t frames) {
    std::memset(p, 0xFF, 2048);
    const uint32_t header = (frames << 26) | (1u << 8);
    p[0] = static_cast<uint8_t>(header >> 24);
    p[1] = static_cast<uint8_t>(header >> 16);
    p[2] = static_cast<uint8_t>(header >> 8);
    p[3] = 0;  // packet_skip 0: the next packet continues the stream
}

struct Rig {
    AcpHalCreateFn create;
    AcpHalShapeCtxFn alloc_ctx, release_ctx;
    ApuAllocFn alloc;
    ApuFreeFn free_mem;
    ShapeContexts ctx{};
    void* acp = nullptr;
    ConnectFn connect;
    SubmitFn submit;
    ReleaseFn release;
    uint64_t token = 0x100;

    bool init() {
        create = reinterpret_cast<AcpHalCreateFn>(rt::shims::find("acphal.dll", "AcpHalCreate"));
        alloc_ctx = reinterpret_cast<AcpHalShapeCtxFn>(rt::shims::find("acphal.dll", "AcpHalAllocateShapeContexts"));
        release_ctx = reinterpret_cast<AcpHalShapeCtxFn>(rt::shims::find("acphal.dll", "AcpHalReleaseShapeContexts"));
        alloc = reinterpret_cast<ApuAllocFn>(rt::shims::find("acphal.dll", "ApuAlloc"));
        free_mem = reinterpret_cast<ApuFreeFn>(rt::shims::find("acphal.dll", "ApuFree"));
        if (!create || !alloc_ctx || !release_ctx || !alloc || !free_mem) return false;
        ctx.numXma = 4;
        if (alloc_ctx(&ctx) != S_OK || !ctx.xma) return false;
        if (create(&acp) != S_OK || !acp) return false;
        auto vt = *static_cast<void* const* const*>(acp);
        connect = reinterpret_cast<ConnectFn>(vt[0]);
        submit = reinterpret_cast<SubmitFn>(vt[2]);
        release = reinterpret_cast<ReleaseFn>(vt[5]);
        return connect(acp, 0x1000, 0x1000) == S_OK;
    }
    HRESULT cmd(uint32_t c, const uint32_t* args) { return submit(acp, c, ++token, 0xffffffffu, args, 0); }
    Words voice(uint32_t i) const { return Words{static_cast<uint32_t*>(ctx.xma) + 16 * i}; }
    void done() {
        release(acp);
        release_ctx(&ctx);
    }
};

template <class F>
bool wait_for(F&& f, int ms) {
    for (int i = 0; i < ms; ++i) {
        if (f()) return true;
        Sleep(1);
    }
    return f();
}

void test_whole_blocks(Rig& r) {
    void* in_va = nullptr;
    uint32_t in_apu = 0;
    check(r.alloc(&in_va, &in_apu, 20 * 2048, 0x800, 1) == S_OK, "blocks: ApuAlloc 20-packet input");
    auto* p = static_cast<uint8_t*>(in_va);
    packet(p, 3);
    for (int i = 1; i < 18; ++i) packet(p + i * 2048, 0);
    packet(p + 18 * 2048, 3);
    packet(p + 19 * 2048, 3);
    void* out_va = nullptr;
    uint32_t out_apu = 0;
    check(r.alloc(&out_va, &out_apu, 0x400, 0x100, 2) == S_OK, "blocks: ApuAlloc 4-block ring");
    std::memset(out_va, 0x55, 0x400);
    const Words w = r.voice(1);
    w[0] = 20u | (1u << 20) | (4u << 22);
    w[1] = (1u << 31) | (3u << 27);
    w[2] = 32u;
    w[5] = in_apu;
    w[7] = out_apu;
    const uint32_t voice = 1;
    check(r.cmd(5, &voice) == S_OK, "blocks: ENABLE_XMA");
    const bool full = wait_for([&] { return ((w[1] >> 31) & 1u) == 0u; }, 500);
    check(full, "blocks: the ring fills (900 decoded samples, 512 published)");
    const auto* s = static_cast<const int16_t*>(out_va);
    const int zeros = static_cast<int>(std::count(s, s + 512, int16_t{0}));
    std::printf("blocks: %d zero samples in the 4 published blocks (block 2: %d)\n", zeros,
                static_cast<int>(std::count(s + 256, s + 384, int16_t{0})));
#if defined(XMA_FIX_EXPECT_PATCHED)
    check(zeros == 0, "blocks (patched): no zero-padded block mid-stream");
#else
    check(zeros == 84, "blocks (unpatched): block 2 is 44 samples + 84 zeros (the click the patch removes)");
#endif
    check(r.cmd(7, &voice) == S_OK, "blocks: DISABLE_XMA");
    r.free_mem(in_va);
    r.free_mem(out_va);
}

void test_revive(Rig& r) {
    void* in_va = nullptr;
    uint32_t in_apu = 0;
    check(r.alloc(&in_va, &in_apu, 0x800, 0x800, 1) == S_OK, "revive: ApuAlloc 1-packet input");
    auto* p = static_cast<uint8_t*>(in_va);
    packet(p, 1);
    void* out_va = nullptr;
    uint32_t out_apu = 0;
    check(r.alloc(&out_va, &out_apu, 0x400, 0x100, 2) == S_OK, "revive: ApuAlloc 4-block ring");
    std::memset(out_va, 0x55, 0x400);
    const Words w = r.voice(2);
    w[0] = 1u | (1u << 20) | (4u << 22);
    w[1] = (1u << 31) | (3u << 27);
    w[2] = 32u;
    w[5] = in_apu;
    w[7] = out_apu;
    const uint32_t voice = 2;
    check(r.cmd(5, &voice) == S_OK, "revive: ENABLE_XMA");
    const bool dry = wait_for([&] { return ((w[2] >> 31) & 1u) == 1u && ((w[0] >> 27) & 0x1Fu) == 1u; }, 500);
    check(dry, "revive: 100 samples published as the final padded block, INPUT_INVALID raised");
    check(((w[2] >> 26) & 0x1Fu) == 4u, "revive: error status INPUT_INVALID (4)");

    packet(p, 3);
    w[2] = (w[2] & ~0x3FFFFFFu) | 32u;
    w[0] = (w[0] & ~0xFFFu) | 1u | (1u << 20);
    const bool moved = wait_for([&] { return ((w[0] >> 27) & 0x1Fu) != 1u; }, 200);
    std::printf("revive: write block %u after the refill\n", (w[0] >> 27) & 0x1Fu);
#if defined(XMA_FIX_EXPECT_PATCHED)
    check(moved, "revive (patched): the refilled input is decoded while the context stays enabled");
    const auto* s = static_cast<const int16_t*>(out_va);
    check(s[128] == 1000 && s[255] == 1000, "revive (patched): block 1 holds the refill's PCM");
#else
    check(!moved, "revive (unpatched): the voice stays ended until the next ENABLE_XMA (the bug)");
#endif
    check(r.cmd(7, &voice) == S_OK, "revive: DISABLE_XMA");
    r.free_mem(in_va);
    r.free_mem(out_va);
}

}  // namespace

int main() {
    GDK_TEST_REGISTER();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Rig r;
    const bool ok = r.init();
    check(ok, "acphal registered, contexts allocated, AcpHal connected");
    if (ok) {
        test_whole_blocks(r);
        test_revive(r);
        r.done();
    }
#if defined(XMA_FIX_EXPECT_PATCHED)
    const char* name = "media_xma_fix_test_patched";
#else
    const char* name = "media_xma_fix_test_unpatched";
#endif
    std::printf("%s: %d checks, %d failed: %s\n", name, g_checks, g_failures, g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
