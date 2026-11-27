// gta6rt_xma engine checks, driving the contexts the way the title does (title_driver.h) on synthetic XMA2 streams
// whose frames spill across packets. The reference is the same decoder fed the real packets in order.
#include <gta6rt/xma.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "synth_xma.h"
#include "title_driver.h"

namespace {

int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_fail;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

struct EngineHal {
    void command(uint32_t cmd, const void* cfg) { gta6rt_xma_command(cmd, cfg); }
};

std::vector<int16_t> reference(const std::vector<std::vector<uint8_t>>& blocks, uint32_t rate, uint32_t ch) {
    Gta6rtXmaDecoder* d = nullptr;
    gta6rt_xma_decoder_create(rate, ch, &d);
    std::vector<int16_t> out;
    std::vector<int16_t> tmp(64 * 512 * 2);
    for (const auto& b : blocks) {
        for (std::size_t p = 0; p + 2048 <= b.size(); p += 2048) {
            uint32_t frames = 0;
            gta6rt_xma_decoder_packet(d, b.data() + p, tmp.data(), tmp.size(), &frames);
            out.insert(out.end(), tmp.begin(), tmp.begin() + frames * 512 * ch);
        }
    }
    gta6rt_xma_decoder_destroy(d);
    return out;
}

struct Rig {
    title::Apu apu{64u << 20};
    std::vector<uint32_t> contexts = std::vector<uint32_t>(title::kContexts * 16);
    EngineHal hal;
    title::Title<EngineHal> t{hal, apu, contexts.data()};
    Rig() { gta6rt_xma_bind(contexts.data(), title::kContexts, &title::Apu::span, &apu); }
    ~Rig() { gta6rt_xma_unbind(); }
    title::Wave load(const std::vector<uint8_t>& bytes, uint32_t rate, uint32_t ch) {
        title::Wave w;
        w.apu = apu.alloc(static_cast<uint32_t>(bytes.size()));
        std::memcpy(apu.ptr(w.apu), bytes.data(), bytes.size());
        w.data = apu.ptr(w.apu);
        w.bytes = static_cast<uint32_t>(bytes.size());
        w.rate = rate;
        w.channels = ch;
        return w;
    }
    // Consumes until `want` samples arrived or nothing new came for 300 ms.
    std::vector<int16_t> run(title::Voice& v, std::size_t want) {
        std::vector<int16_t> out;
        auto last = std::chrono::steady_clock::now();
        while (out.size() < want) {
            if (t.update(v, out)) {
                last = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - last > std::chrono::milliseconds(300)) {
                break;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
        return out;
    }
};

// Samples of `got` (hardware output from `position`) equal `ref` from `position`, except the first `loose`
// samples (the rest of the frame decoded without its predecessor's state: all 512 samples of it differ).
std::size_t mismatches(const std::vector<int16_t>& got, const std::vector<int16_t>& ref, std::size_t position,
                       std::size_t loose, std::size_t count) {
    std::size_t bad = 0;
    for (std::size_t k = loose; k < count; ++k) {
        if (position + k >= ref.size() || k >= got.size() || got[k] != ref[position + k]) ++bad;
    }
    return bad;
}

void full_stream(bool stereo, uint32_t rate_index, uint32_t frames) {
    const uint32_t rates[] = {24000, 32000, 44100, 48000};
    const uint32_t ch = stereo ? 2 : 1;
    const auto bytes = synth::stream(frames, stereo, rate_index);
    const auto ref = reference({bytes}, rates[rate_index], ch);
    Rig rig;
    auto wave = rig.load(bytes, rates[rate_index], ch);
    auto v = rig.t.submit(wave, {});
    rig.t.enable(v);
    const auto out = rig.run(v, std::size_t(frames) * 512 * ch);
    rig.t.release(v);
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s %u Hz, %u frames over %zu packets: %zu of %zu samples, all equal to the "
                  "in-order packet decode", stereo ? "stereo" : "mono", rates[rate_index], frames, bytes.size() / 2048,
                  out.size(), ref.size());
    check(out.size() == ref.size() && out.size() == std::size_t(frames) * 512 * ch &&
              std::equal(out.begin(), out.end(), ref.begin()), msg);
}

void frame_accurate_starts() {
    const uint32_t frames = 400;
    const auto bytes = synth::stream(frames);
    const auto ref = reference({bytes}, 32000, 1);
    const uint32_t starts[] = {0, 100, 128, 255, 300, 511, 512, 700, 1500, 5000, 13037, 40000, 77777, 150001};
    int ok = 0, n = 0;
    for (uint32_t s : starts) {
        Rig rig;
        auto wave = rig.load(bytes, 32000, 1);
        auto v = rig.t.submit(wave, {s, 0, 0, 0});
        rig.t.enable(v);
        const std::size_t want = std::size_t(frames) * 512 - v.base - v.position;
        const auto out = rig.run(v, want);
        rig.t.release(v);
        const uint32_t frame_head = (v.position & ~511u) + 512;
        const std::size_t loose = frame_head > v.position ? frame_head - v.position : 0;
        const std::size_t bad = mismatches(out, ref, v.base + v.position, loose, want);
        ++n;
        if (out.size() == want && bad == 0 && v.position <= s - v.base && s - v.base - v.position < 256) ++ok;
        else std::printf("  start %u: base %u position %u got %zu want %zu mismatches %zu\n", s, v.base, v.position,
                         out.size(), want, bad);
    }
    char msg[160];
    std::snprintf(msg, sizeof msg, "frame-accurate starts: %d of %d start samples begin at the title's position and "
                  "match the full decode after the first frame", ok, n);
    check(ok == n, msg);
}

void loops() {
    const uint32_t frames = 300;
    const auto bytes = synth::stream(frames);
    const auto ref = reference({bytes}, 32000, 1);
    struct L { uint32_t ls, le; } cases[] = {{0, 20000}, {4096, 30000}, {5000 + 128, 60000 + 300}, {65536, 65536 + 640},
                                             {1000, 140000}};
    int ok = 0, n = 0;
    for (auto c : cases) {
        Rig rig;
        auto wave = rig.load(bytes, 32000, 1);
        auto v = rig.t.submit(wave, {0, 0, c.ls, c.le});
        rig.t.enable(v);
        const uint32_t region_start = (c.ls >> 9) * 512 + ((c.ls >> 7) & 3) * 128;
        const uint32_t region_end = (c.le >> 9) * 512 + (((c.le >> 7) & 3) + 1) * 128;
        const uint32_t first = region_end;
        const uint32_t body = region_end - region_start;
        const std::size_t want = first + 3 * body;
        const auto out = rig.run(v, want);
        rig.t.release(v);
        std::size_t bad = mismatches(out, ref, 0, 0, first);
        for (int it = 0; it < 3; ++it) {
            std::vector<int16_t> seg(out.begin() + std::min(out.size(), std::size_t(first + it * body)),
                                     out.begin() + std::min(out.size(), std::size_t(first + (it + 1) * body)));
            const uint32_t head = (region_start & ~511u) + 512;
            bad += mismatches(seg, ref, region_start, head - region_start, body);
        }
        ++n;
        if (out.size() >= want && bad == 0) ++ok;
        else std::printf("  loop [%u, %u]: got %zu want %zu mismatches %zu\n", c.ls, c.le, out.size(), want, bad);
    }
    gta6rt_xma_stats st{};
    gta6rt_xma_get_stats(&st);
    char msg[200];
    std::snprintf(msg, sizeof msg, "loops: %d of %d loop regions play [start frame+skip, end frame+sub-end] three "
                  "times; samples equal the full decode outside the first frame after each jump", ok, n);
    check(ok == n, msg);
}

void two_inputs_one_context() {
    const uint32_t frames = 200;
    const auto bytes = synth::stream(frames);
    const auto ref = reference({bytes}, 32000, 1);
    const std::size_t split = (bytes.size() / 2048 / 2) * 2048;
    Rig rig;
    auto wave = rig.load(bytes, 32000, 1);
    auto v = rig.t.submit(wave, {});
    uint32_t* c = rig.t.w(v.index);
    c[0] = (c[0] & 0xfffff000u) | uint32_t(split / 2048);
    c[1] = (c[1] & 0xfffff000u) | uint32_t((bytes.size() - split) / 2048);
    c[6] = wave.apu + uint32_t(split);
    c[0] |= 3u << 20;
    rig.t.enable(v);
    const auto out = rig.run(v, ref.size());
    const uint32_t valid = (c[0] >> 20) & 3;
    rig.t.release(v);
    check(out.size() == ref.size() && std::equal(out.begin(), out.end(), ref.begin()) && valid == 0,
          "two input buffers in one context: the frame spilling from input 0 into input 1 decodes, both inputs "
          "retire, output equals the single-buffer decode");
}

void work_buffer_handoff(bool copy) {
    const auto a = synth::stream(90, false, 1, 0);
    const auto b = synth::stream(110, false, 1, 90);
    const auto ref = reference({a, b}, 32000, 1);
    Rig rig;
    auto wa = rig.load(a, 32000, 1), wb = rig.load(b, 32000, 1);
    auto va = rig.t.submit(wa, {});
    auto vb = rig.t.submit(wb, {});
    rig.t.enable(va);
    auto out = rig.run(va, 90 * 512);
    if (copy) rig.t.copy_work(va, vb);
    rig.t.release(va);
    rig.t.enable(vb);
    const auto outb = rig.run(vb, 110 * 512);
    rig.t.release(vb);
    const std::size_t head_bad = mismatches(outb, ref, 90 * 512, 0, 512);
    const std::size_t tail_bad = mismatches(outb, ref, 90 * 512, 512, 110 * 512);
    gta6rt_xma_stats st{};
    gta6rt_xma_get_stats(&st);
    if (copy) {
        check(out.size() == 90 * 512 && outb.size() == 110 * 512 && head_bad == 0 && tail_bad == 0 &&
                  st.state_imports >= 1,
              "work buffer copied from context A to B (exe+0x781c6): B continues A's decoder state, the 110 frames "
              "equal the continuous decode sample for sample");
    } else {
        check(outb.size() == 110 * 512 && tail_bad == 0 && head_bad > 0,
              "without the work buffer copy B starts fresh: its first frame (512 samples) differs from the "
              "continuous decode, the rest is equal");
    }
}

void ran_dry_then_refill() {
    const auto a = synth::stream(60, false, 1, 0);
    const auto b = synth::stream(60, false, 1, 60);
    const auto ref = reference({a, b}, 32000, 1);
    Rig rig;
    auto wa = rig.load(a, 32000, 1), wb = rig.load(b, 32000, 1);
    auto v = rig.t.submit(wa, {});
    rig.t.enable(v);
    auto out = rig.run(v, 60 * 512);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint32_t* c = rig.t.w(v.index);
    const bool dry = ((c[0] >> 20) & 3) == 0 && ((c[2] >> 26) & 0x1f) == 4 && ((c[2] >> 31) & 1);
    c[5] = wb.apu;
    c[0] = (c[0] & 0xfffff000u) | uint32_t(b.size() / 2048);
    c[2] = (c[2] & 0xfc000000u) | 32u;
    c[0] |= 1u << 20;
    const uint32_t cfg[2] = {v.index, 0};
    gta6rt_xma_command(18, cfg);
    const auto more = rig.run(v, 60 * 512);
    out.insert(out.end(), more.begin(), more.end());
    rig.t.release(v);
    check(dry && out.size() == 120 * 512 && std::equal(out.begin(), out.end(), ref.begin()),
          "a voice that ran dry (valid 0, error 4) resumes on a refilled input without a new ENABLE and continues "
          "the stream");
}

void ring_and_blocks() {
    const auto bytes = synth::stream(64);
    Rig rig;
    auto wave = rig.load(bytes, 32000, 1);
    auto v = rig.t.submit(wave, {0, 0, 0, 0});
    rig.t.enable(v);
    uint32_t* c = rig.t.w(v.index);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint32_t write = (c[0] >> 27) & 31, read = c[9] & 31, valid = c[1] >> 31;
    check(write == read && valid == 0, "the engine fills all 23 ring blocks, then clears output-valid (full ring)");
    std::vector<int16_t> out;
    const uint32_t got = rig.t.update(v, out);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint32_t write2 = (c[0] >> 27) & 31;
    check(got == 23 && out.size() == 23 * 128 && write2 == read && (c[1] >> 31) == 0,
          "the title takes 23 blocks and advances by 23: the engine refills the whole ring again");
    rig.t.release(v);
}

void many_voices() {
    const uint32_t frames = 120;
    const auto bytes = synth::stream(frames);
    const auto ref = reference({bytes}, 32000, 1);
    Rig rig;
    auto wave = rig.load(bytes, 32000, 1);
    std::vector<title::Voice> vs;
    for (uint32_t i = 0; i < title::kContexts; ++i) vs.push_back(rig.t.submit(wave, {}));
    const auto t0 = std::chrono::steady_clock::now();
    for (auto& v : vs) rig.t.enable(v);
    std::vector<std::vector<int16_t>> outs(vs.size());
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        bool any = false, done = true;
        for (std::size_t i = 0; i < vs.size(); ++i) {
            if (outs[i].size() < ref.size()) {
                done = false;
                any |= rig.t.update(vs[i], outs[i]) != 0;
            }
        }
        if (done) break;
        if (any) last = std::chrono::steady_clock::now();
        else if (std::chrono::steady_clock::now() - last > std::chrono::milliseconds(500)) break;
        else std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int equal = 0;
    for (auto& o : outs) equal += (o.size() >= ref.size() && std::equal(ref.begin(), ref.end(), o.begin()));
    for (auto& v : vs) rig.t.release(v);
    gta6rt_xma_stats st{};
    gta6rt_xma_get_stats(&st);
    char msg[200];
    std::snprintf(msg, sizeof msg, "350 contexts at once (%u workers): %d of 350 outputs equal the reference; "
                  "%.2f s of audio each, done in %.3f s wall", st.workers, equal, frames * 512 / 32000.0, secs);
    check(equal == 350, msg);
}

void wanted() {
    unsetenv("GTA6RT_XMA");
    unsetenv("GTA6RT_AUDIO");
    const int unset = gta6rt_xma_wanted();
    setenv("GTA6RT_AUDIO", "off", 1);
    const int off = gta6rt_xma_wanted();
    setenv("GTA6RT_AUDIO", "null", 1);
    const int null = gta6rt_xma_wanted();
    setenv("GTA6RT_AUDIO", "on", 1);
    const int on = gta6rt_xma_wanted();
    setenv("GTA6RT_XMA", "0", 1);
    const int forced_off = gta6rt_xma_wanted();
    setenv("GTA6RT_AUDIO", "off", 1);
    setenv("GTA6RT_XMA", "1", 1);
    const int forced_on = gta6rt_xma_wanted();
    unsetenv("GTA6RT_XMA");
    unsetenv("GTA6RT_AUDIO");
    check(!unset && !off && null && on && !forced_off && forced_on,
          "gta6rt_xma_wanted: off when GTA6RT_AUDIO is unset/off, on for on/null, GTA6RT_XMA=0/1 overrides");
}

}  // namespace

int main() {
    wanted();
    full_stream(false, 1, 300);
    full_stream(false, 0, 200);
    full_stream(false, 2, 200);
    full_stream(true, 3, 250);
    full_stream(true, 1, 150);
    frame_accurate_starts();
    loops();
    two_inputs_one_context();
    work_buffer_handoff(true);
    work_buffer_handoff(false);
    ran_dry_then_refill();
    ring_and_blocks();
    many_voices();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
