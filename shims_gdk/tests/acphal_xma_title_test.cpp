// acphal.dll XMA contexts driven the way game_scarlett_master.exe drives them (port/xma/tests/title_driver.h):
// ApuCreateHeap and AllocateShapeContexts with the title's sizes, one non-cached block of 350 x (23-block ring +
// 0x200 work buffer), context setup per buffer, ENABLE (id 0xf6f6f6f6), ADVANCE (id 0xf1f1f1f1), DISABLE (id
// index + 1). With GTA6RT_AUDIO=null the port/xma engine must decode; with it unset or off the shim must keep its
// old silent output.
#include <windows.h>

#include <gta6rt/shims.h>
#include <gta6rt/xma.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "shims.h"
#include "synth_xma.h"
#include "title_driver.h"

namespace {

int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fail;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

using CreateFn = HRESULT(__stdcall*)(void**);
using AllocCtxFn = HRESULT(__stdcall*)(void*);
using ReleaseCtxFn = HRESULT(__stdcall*)(void*);
using CreateHeapFn = HRESULT(__stdcall*)(uint32_t, uint32_t);
using AllocFn = HRESULT(__stdcall*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t);
using MapApuFn = void*(__stdcall*)(uint32_t);
using ConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
using SubmitFn = HRESULT(__stdcall*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
using HalReleaseFn = HRESULT(__stdcall*)(void*);

struct ShapeContexts {
    uint32_t counts[6];
    void* arrays[6];
    uint64_t apu[6];
};

AllocFn g_alloc;
MapApuFn g_map;

struct ShimApu {
    uint32_t alloc(uint32_t bytes, uint32_t align = 0x800, uint32_t flags = 2) {
        void* va = nullptr;
        uint32_t apu = 0;
        return g_alloc(&va, &apu, bytes, align, flags) == S_OK ? apu : 0;
    }
    uint8_t* ptr(uint32_t apu) { return static_cast<uint8_t*>(g_map(apu)); }
};

struct ShimHal {
    void* hal;
    SubmitFn submit;
    void command(uint32_t cmd, const void* cfg) {
        uint32_t index = 0;
        std::memcpy(&index, cfg, 4);
        const uint64_t id = cmd == 5 ? 0xf6f6f6f6u : cmd == 18 ? 0xf1f1f1f1u : index + 1u;
        submit(hal, cmd, id, 0xffffffffu, cfg, 0);
    }
};

std::vector<int16_t> reference(const std::vector<uint8_t>& bytes, uint32_t rate, uint32_t ch) {
    Gta6rtXmaDecoder* d = nullptr;
    gta6rt_xma_decoder_create(rate, ch, &d);
    std::vector<int16_t> out, tmp(64 * 512 * 2);
    for (std::size_t p = 0; p + 2048 <= bytes.size(); p += 2048) {
        uint32_t frames = 0;
        gta6rt_xma_decoder_packet(d, bytes.data() + p, tmp.data(), tmp.size(), &frames);
        out.insert(out.end(), tmp.begin(), tmp.begin() + frames * 512 * ch);
    }
    gta6rt_xma_decoder_destroy(d);
    return out;
}

template <class T>
std::vector<int16_t> play(T& t, title::Voice& v, std::size_t want) {
    std::vector<int16_t> out;
    t.enable(v);
    auto last = std::chrono::steady_clock::now();
    while (out.size() < want) {
        if (t.update(v, out)) last = std::chrono::steady_clock::now();
        else if (std::chrono::steady_clock::now() - last > std::chrono::milliseconds(400)) break;
        else std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    t.release(v);
    return out;
}

}  // namespace

int main() {
    GDK_TEST_REGISTER();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool host = gta6rt_xma_wanted() != 0;
    std::printf("acphal_xma_title_test: %s path\n", host ? "port/xma host decode" : "silent (old)");
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("acphal.dll", "AcpHalCreate"));
    auto alloc_ctx = reinterpret_cast<AllocCtxFn>(rt::shims::find("acphal.dll", "AcpHalAllocateShapeContexts"));
    auto release_ctx = reinterpret_cast<ReleaseCtxFn>(rt::shims::find("acphal.dll", "AcpHalReleaseShapeContexts"));
    auto create_heap = reinterpret_cast<CreateHeapFn>(rt::shims::find("acphal.dll", "ApuCreateHeap"));
    g_alloc = reinterpret_cast<AllocFn>(rt::shims::find("acphal.dll", "ApuAlloc"));
    g_map = reinterpret_cast<MapApuFn>(rt::shims::find("acphal.dll", "ApuMapApuAddress"));
    if (!create || !alloc_ctx || !release_ctx || !create_heap || !g_alloc || !g_map) {
        std::printf("FAIL acphal.dll exports missing\n");
        return 1;
    }
    check(create_heap(267386880u, 2256384u) == S_OK, "ApuCreateHeap(267386880, 2256384) as the title");
    ShapeContexts sc{};
    sc.counts[4] = 350;
    check(alloc_ctx(&sc) == S_OK && sc.arrays[4], "AllocateShapeContexts: 350 XMA contexts");
    void* hal = nullptr;
    check(create(&hal) == S_OK && hal, "AcpHalCreate");
    auto vt = *static_cast<void* const* const*>(hal);
    check(reinterpret_cast<ConnectFn>(vt[0])(hal, 0x1000, 0x1000) == S_OK, "Connect(0x1000, 0x1000)");
    ShimHal sh{hal, reinterpret_cast<SubmitFn>(vt[2])};
    ShimApu apu;
    title::Title<ShimHal, ShimApu> t(sh, apu, static_cast<uint32_t*>(sc.arrays[4]));
    check(t.ring[349] != 0 && t.work[349] == t.ring[349] + title::kRingBytes,
          "350 x (0x1700 ring + 0x200 work) fit the title's non-cached heap");

    struct Case { uint32_t frames; bool stereo; uint32_t rate_index; uint32_t rate; } cases[] = {
        {300, false, 1, 32000}, {160, true, 3, 48000}};
    for (const auto& c : cases) {
        const uint32_t ch = c.stereo ? 2 : 1;
        const auto bytes = synth::stream(c.frames, c.stereo, c.rate_index);
        const uint32_t wave_apu = apu.alloc(static_cast<uint32_t>(bytes.size()), 0x800, 0);
        std::memcpy(apu.ptr(wave_apu), bytes.data(), bytes.size());
        title::Wave w{apu.ptr(wave_apu), wave_apu, static_cast<uint32_t>(bytes.size()), c.rate, ch};
        auto v = t.submit(w, {});
        const auto out = play(t, v, std::size_t(c.frames) * 512 * ch);
        char msg[200];
        if (host) {
            const auto ref = reference(bytes, c.rate, ch);
            std::snprintf(msg, sizeof msg, "%s %u Hz through SubmitCommand: %zu samples equal the reference decode",
                          c.stereo ? "stereo" : "mono", c.rate, out.size());
            check(out.size() == ref.size() && std::equal(out.begin(), out.end(), ref.begin()), msg);
            const uint32_t start = 12345;
            auto vs = t.submit(w, {start, 0, 0, 0});
            const auto seek = play(t, vs, std::size_t(c.frames) * 512 * ch);
            const std::size_t from = vs.base + vs.position;
            const std::size_t loose = (512 - (from & 511)) * ch;
            bool same = seek.size() == ref.size() - from * ch;
            for (std::size_t k = loose; same && k < seek.size(); ++k) same = seek[k] == ref[from * ch + k];
            std::snprintf(msg, sizeof msg, "%s start sample %u: output begins at the title's position %zu and "
                          "matches the reference after the first frame", c.stereo ? "stereo" : "mono", start, from);
            check(same, msg);
        } else {
            std::snprintf(msg, sizeof msg, "%s %u Hz on the old path: %zu samples of silence (%u frames x 512)",
                          c.stereo ? "stereo" : "mono", c.rate, out.size(), c.frames);
            check(out.size() == std::size_t(c.frames) * 512 * ch &&
                      std::all_of(out.begin(), out.end(), [](int16_t s) { return s == 0; }),
                  msg);
        }
    }
    if (host) {
        gta6rt_xma_stats st{};
        gta6rt_xma_get_stats(&st);
        std::printf("engine: enables %llu disables %llu advances %llu frames %llu workers %u\n",
                    (unsigned long long)st.enables, (unsigned long long)st.disables,
                    (unsigned long long)st.advances, (unsigned long long)st.frames, st.workers);
        check(st.enables == 4 && st.disables == 4 && st.active == 0, "4 ENABLE and 4 DISABLE reached port/xma");
    }
    check(reinterpret_cast<HalReleaseFn>(vt[5])(hal) == S_OK, "AcpHal Release");
    check(release_ctx(nullptr) == S_OK, "AcpHalReleaseShapeContexts stops the decoder");
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
