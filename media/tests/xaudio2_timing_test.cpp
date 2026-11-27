// Threaded XAudio2 engine against a real clock: `silent` (host monotonic clock) or `coreaudio` (default output device,
// writes silence). Two fake clients run at once:
//   A: Bink2Scarlett's pattern: 16 x 62.5 ms buffers, no callback, a feeder polling GetState every 2 ms;
//   B: a callback client: 40 ms buffers refilled from OnBufferEnd on the engine thread.
// Prints measured callback timing, consumption rate and per-pass cost.
#include "media_common.h"
#include "media_exports.h"
#include "test_util.h"
#include "xaudio2_engine.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rt::media;
using mt::call;

namespace {

enum { CreateSource = 5, CreateMaster = 7, Destroy = 18, Start = 19, Submit = 21, GetState = 25, Release = 2 };

constexpr uint32_t kBFrames = 1920;  // 40 ms at 48 kHz

struct Client {
    const void* const* vtbl;
    uint64_t voice = 0;
    std::vector<int16_t> data;
    std::mutex mu;
    std::vector<uint64_t> end_ns;
    uint64_t first_start_ns = 0;
    uint64_t submitted = 0;
};

void submit_b(Client* c, uint64_t ctx) {
    XAudio2Buffer b{};
    b.AudioBytes = kBFrames * 4;
    b.pAudioData = reinterpret_cast<uint64_t>(c->data.data() + (ctx % 4) * kBFrames * 2);
    b.pContext = ctx;
    call<int32_t>(c->voice, Submit, &b, static_cast<const void*>(nullptr));
    ++c->submitted;
}

GTA6RT_GUEST_ABI void b_noop(Client*) {}
GTA6RT_GUEST_ABI void b_pass_start(Client*, uint32_t) {}
GTA6RT_GUEST_ABI void b_buffer_start(Client* c, uint64_t) {
    if (!c->first_start_ns) c->first_start_ns = now_ns();
}
GTA6RT_GUEST_ABI void b_buffer_end(Client* c, uint64_t ctx) {
    {
        std::lock_guard lock(c->mu);
        c->end_ns.push_back(now_ns());
    }
    submit_b(c, c->submitted);
}
GTA6RT_GUEST_ABI void b_ctx(Client*, uint64_t) {}
GTA6RT_GUEST_ABI void b_err(Client*, uint64_t, int32_t) {}
const void* const kBVtbl[] = {
    reinterpret_cast<const void*>(&b_pass_start), reinterpret_cast<const void*>(&b_noop),
    reinterpret_cast<const void*>(&b_noop),       reinterpret_cast<const void*>(&b_buffer_start),
    reinterpret_cast<const void*>(&b_buffer_end), reinterpret_cast<const void*>(&b_ctx),
    reinterpret_cast<const void*>(&b_err),
};

WaveFormatEx stereo16() {
    WaveFormatEx f{};
    f.wFormatTag = kWaveFormatPcm;
    f.nChannels = 2;
    f.nSamplesPerSec = 48000;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = 192000;
    return f;
}

struct Stats {
    double mean, sd, min, max;
};
Stats stats(const std::vector<double>& v) {
    Stats s{0, 0, 1e300, -1e300};
    for (double x : v) {
        s.mean += x;
        s.min = std::min(s.min, x);
        s.max = std::max(s.max, x);
    }
    s.mean /= static_cast<double>(v.size());
    for (double x : v) s.sd += (x - s.mean) * (x - s.mean);
    s.sd = std::sqrt(s.sd / static_cast<double>(v.size()));
    return s;
}

int run(rt::xa2::Engine::OutputMode mode, double seconds) {
    const char* name = mode == rt::xa2::Engine::OutputMode::CoreAudio ? "coreaudio" : "silent";
    rt::xa2::set_output_mode_override(static_cast<int>(mode));
    uint64_t x = 0, master = 0;
    CHECK_HR(XAudio2Create(&x, 0x400, 1), kSOk);
    CHECK_HR((call<int32_t>(x, CreateMaster, &master, 0u, 0u, 0u, static_cast<const char16_t*>(nullptr),
                            static_cast<const void*>(nullptr), 6u)),
             kSOk);
    rt::xa2::Engine* e = rt::xa2::engine_of(x);
    const WaveFormatEx f = stereo16();

    // Client A (Bink).
    uint64_t a = 0;
    CHECK_HR((call<int32_t>(x, CreateSource, &a, &f, kXa2VoiceNoPitch, 1.0f, static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr), static_cast<void*>(nullptr))),
             kSOk);
    constexpr uint32_t kABytes = 12000;
    std::vector<int16_t> ring(16 * kABytes / 2, 0);
    uint64_t a_submits = 0;
    for (int i = 0; i < 8; ++i) {
        XAudio2Buffer b{};
        b.AudioBytes = kABytes;
        b.pAudioData = reinterpret_cast<uint64_t>(&ring[(a_submits % 16) * kABytes / 2]);
        CHECK_HR(call<int32_t>(a, Submit, &b, static_cast<const void*>(nullptr)), kSOk);
        ++a_submits;
    }

    // Client B (callbacks).
    Client c{kBVtbl};
    c.data.assign(4 * kBFrames * 2, 0);
    CHECK_HR((call<int32_t>(x, CreateSource, &c.voice, &f, 0u, 2.0f, &c, static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr))),
             kSOk);
    for (int i = 0; i < 3; ++i) submit_b(&c, c.submitted);

    const uint64_t t_start = now_ns();
    CHECK_HR(call<int32_t>(a, Start, 0u, 0u), kSOk);
    CHECK_HR(call<int32_t>(c.voice, Start, 0u, 0u), kSOk);

    std::vector<std::pair<uint64_t, uint64_t>> a_samples;
    uint64_t underflows = 0, polls = 0;
    const uint64_t t_end = t_start + static_cast<uint64_t>(seconds * 1e9);
    while (now_ns() < t_end) {
        XAudio2VoiceState s{};
        call<void>(a, GetState, &s, 0u);
        ++polls;
        if (s.BuffersQueued == 0) ++underflows;
        while (s.BuffersQueued < 16) {
            XAudio2Buffer b{};
            b.AudioBytes = kABytes;
            b.pAudioData = reinterpret_cast<uint64_t>(&ring[(a_submits % 16) * kABytes / 2]);
            call<int32_t>(a, Submit, &b, static_cast<const void*>(nullptr));
            ++a_submits;
            ++s.BuffersQueued;
        }
        if (a_samples.empty() || a_samples.back().second != s.SamplesPlayed) a_samples.push_back({now_ns(), s.SamplesPlayed});
        usleep(2000);
    }

    // GetState cost (Bink polls it).
    const uint64_t g0 = now_ns();
    XAudio2VoiceState tmp{};
    for (int i = 0; i < 100000; ++i) call<void>(a, GetState, &tmp, 0u);
    const double getstate_ns = static_cast<double>(now_ns() - g0) / 100000.0;

    std::vector<uint64_t> ends;
    {
        std::lock_guard lock(c.mu);
        ends = c.end_ns;
    }
    const rt::xa2::EngineStats es = e->stats();
    rt::xa2::CoreAudioInfo ci{};
    const bool have_ci = rt::xa2::coreaudio_info(e->output(), &ci);
    XAudio2PerformanceData pd{};
    call<void>(x, 11, &pd);

    // Tear down before printing (stops the engine thread).
    call<void>(a, Destroy);
    call<void>(c.voice, Destroy);
    call<void>(master, Destroy);
    CHECK(call<uint32_t>(x, Release) == 0);

    std::printf("[%s] %.1f s run, output %s\n", name, seconds, have_ci ? "coreaudio" : "silent");
    CHECK(ends.size() > 10);
    if (ends.size() > 10) {
        std::vector<double> iv;
        for (size_t i = 1; i < ends.size(); ++i) iv.push_back(static_cast<double>(ends[i] - ends[i - 1]) / 1e6);
        const Stats s = stats(iv);
        // Least-squares fit end_k = t0 + k * period; residuals are the callback lateness jitter.
        const double n = static_cast<double>(ends.size());
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t k = 0; k < ends.size(); ++k) {
            const double y = static_cast<double>(ends[k] - ends[0]) / 1e6;
            sx += static_cast<double>(k);
            sy += y;
            sxx += static_cast<double>(k * k);
            sxy += static_cast<double>(k) * y;
        }
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        const double icpt = (sy - slope * sx) / n;
        double maxres = 0;
        for (size_t k = 0; k < ends.size(); ++k) {
            const double y = static_cast<double>(ends[k] - ends[0]) / 1e6;
            maxres = std::max(maxres, std::fabs(y - (icpt + slope * static_cast<double>(k))));
        }
        const double ppm = (slope / 40.0 - 1.0) * 1e6;
        std::printf("[%s] OnBufferEnd (40 ms buffers): %zu callbacks, interval mean %.3f ms sd %.3f min %.3f max %.3f; "
                    "fitted period %.4f ms (%+.0f ppm vs 40 ms), max residual %.3f ms\n",
                    name, ends.size(), s.mean, s.sd, s.min, s.max, slope, ppm, maxres);
        std::printf("[%s] first OnBufferStart %.3f ms after Start\n", name,
                    static_cast<double>(c.first_start_ns - t_start) / 1e6);
        CHECK(std::fabs(s.mean - 40.0) < 0.5);
        CHECK(maxres < 15.0);
    }
    if (a_samples.size() > 100) {
        // First observation of each new SamplesPlayed value (one per processing pass), from 10% in to the end.
        const auto& p0 = a_samples[a_samples.size() / 10];
        const auto& p1 = a_samples.back();
        const double rate = static_cast<double>(p1.second - p0.second) / (static_cast<double>(p1.first - p0.first) / 1e9);
        std::printf("[%s] Bink-pattern voice: %llu buffers submitted, %llu GetState polls, %llu polls saw "
                    "BuffersQueued==0, consumption %.1f frames/s (%+.0f ppm vs 48000)\n",
                    name, static_cast<unsigned long long>(a_submits), static_cast<unsigned long long>(polls),
                    static_cast<unsigned long long>(underflows), rate, (rate / 48000.0 - 1.0) * 1e6);
        CHECK(underflows == 0);
        CHECK(std::fabs(rate / 48000.0 - 1.0) < 0.005);
    }
    std::printf("[%s] GetState: %.1f ns per call (100k calls)\n", name, getstate_ns);
    std::printf("[%s] engine: %llu passes, pass time mean %.1f us min %.1f max %.1f, %llu callbacks\n", name,
                static_cast<unsigned long long>(es.passes),
                es.passes ? static_cast<double>(es.pass_ns_total) / static_cast<double>(es.passes) / 1e3 : 0.0,
                static_cast<double>(es.pass_ns_min) / 1e3, static_cast<double>(es.pass_ns_max) / 1e3,
                static_cast<unsigned long long>(es.callbacks));
    std::printf("[%s] performance data: latency %u samples, glitches %u\n", name, pd.CurrentLatencyInSamples,
                pd.GlitchesSinceEngineStarted);
    if (have_ci) {
        const double span = static_cast<double>(ci.last_callback_ns - ci.first_callback_ns) / 1e9;
        std::printf("[coreaudio] device %.0f Hz, io buffer %u frames, device latency %u, safety offset %u, ring target "
                    "%u frames; %llu render callbacks, %llu frames rendered, %.1f frames/s measured\n",
                    ci.device_rate, ci.io_buffer_frames, ci.device_latency_frames, ci.safety_offset_frames,
                    ci.ring_target_frames, static_cast<unsigned long long>(ci.callbacks),
                    static_cast<unsigned long long>(ci.frames_rendered),
                    span > 0 ? static_cast<double>(ci.frames_rendered) / span : 0.0);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    set_guest_invoker(&direct_guest_invoker);
    const std::string mode = argc > 1 ? argv[1] : "silent";
    const double secs = argc > 2 ? std::atof(argv[2]) : 3.0;
    if (mode == "silent" || mode == "both") run(rt::xa2::Engine::OutputMode::Silent, secs);
    if (mode == "coreaudio" || mode == "both") run(rt::xa2::Engine::OutputMode::CoreAudio, secs);
    return mt::finish("media_xaudio2_timing_test");
}
