// Decode cost of the XMA path: the plain frame decoder per 512-sample frame (one thread), and the context engine
// with N voices playing the same stream through the title's protocol (wall time, CPU of the decode calls).
//
//   xma_bench IN.xma RATE CHANNELS [--voices N] [--seconds S] [--realtime S]
// --realtime S then plays the N voices at real time for S seconds (a consumer tick every 5 ms takes 5 ms of audio
// per voice) and counts the ticks where a voice had less than that buffered (underruns).
#include <gta6rt/xma.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include "title_driver.h"

namespace {

struct EngineHal {
    void command(uint32_t cmd, const void* cfg) { gta6rt_xma_command(cmd, cfg); }
};

double cpu_seconds() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: xma_bench IN.xma RATE CHANNELS [--voices N] [--seconds S]\n");
        return 2;
    }
    std::vector<uint8_t> in;
    if (FILE* f = std::fopen(argv[1], "rb")) {
        std::fseek(f, 0, SEEK_END);
        in.resize(static_cast<std::size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(in.data(), 1, in.size(), f) != in.size()) in.clear();
        std::fclose(f);
    }
    const uint32_t rate = std::atoi(argv[2]), ch = std::atoi(argv[3]);
    uint32_t voices = 64;
    double seconds = 2.0, realtime = 0.0;
    for (int i = 4; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--voices") voices = std::atoi(argv[++i]);
        else if (a == "--seconds") seconds = std::atof(argv[++i]);
        else if (a == "--realtime") realtime = std::atof(argv[++i]);
    }
    if (in.empty() || in.size() % 2048) return 2;

    Gta6rtXmaDecoder* d = nullptr;
    gta6rt_xma_decoder_create(rate, ch, &d);
    std::vector<int16_t> tmp(64 * 512 * 2);
    uint64_t frames = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 5; ++rep) {
        gta6rt_xma_decoder_reset(d);
        for (std::size_t p = 0; p + 2048 <= in.size(); p += 2048) {
            uint32_t n = 0;
            gta6rt_xma_decoder_packet(d, in.data() + p, tmp.data(), tmp.size(), &n);
            frames += n;
        }
    }
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    gta6rt_xma_decoder_destroy(d);
    const double us_per_frame = dt / double(frames) * 1e6;
    const double ms_per_audio_s = us_per_frame * (rate / 512.0) / 1000.0;
    std::printf("{\"packet_decoder_us_per_frame\":%.2f,\"packet_decoder_ms_per_audio_second\":%.3f", us_per_frame,
                ms_per_audio_s);

    title::Apu apu(in.size() + title::kContexts * (title::kRingBytes + title::kWorkBytes) + (1u << 20));
    std::vector<uint32_t> ctx(title::kContexts * 16);
    EngineHal hal;
    title::Title<EngineHal> t(hal, apu, ctx.data());
    gta6rt_xma_bind(ctx.data(), title::kContexts, &title::Apu::span, &apu);
    title::Wave w;
    w.apu = apu.alloc(static_cast<uint32_t>(in.size()));
    std::memcpy(apu.ptr(w.apu), in.data(), in.size());
    w.data = apu.ptr(w.apu);
    w.bytes = static_cast<uint32_t>(in.size());
    w.rate = rate;
    w.channels = ch;
    uint32_t total_frames = 0;
    for (std::size_t p = 0; p < in.size(); p += 2048) total_frames += in[p] >> 2;
    const uint32_t stream_samples = total_frames * 512;
    std::vector<title::Voice> vs;
    for (uint32_t i = 0; i < voices; ++i) vs.push_back(t.submit(w, {0, 0, 0, stream_samples > 1024 ? stream_samples - 600 : 0}));
    gta6rt_xma_stats s0{};
    gta6rt_xma_get_stats(&s0);
    const double c0 = cpu_seconds();
    const auto w0 = std::chrono::steady_clock::now();
    for (auto& v : vs) t.enable(v);
    const std::size_t want = std::size_t(seconds * rate) * ch;
    std::vector<std::size_t> got(vs.size(), 0);
    std::vector<int16_t> sink;
    for (;;) {
        bool done = true;
        for (std::size_t i = 0; i < vs.size(); ++i) {
            if (got[i] >= want) continue;
            done = false;
            sink.clear();
            t.update(vs[i], sink);
            got[i] += sink.size();
        }
        if (done) break;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    const double cpu = cpu_seconds() - c0;
    gta6rt_xma_stats s1{};
    gta6rt_xma_get_stats(&s1);
    uint64_t underruns = 0, ticks = 0;
    if (realtime > 0) {
        for (auto& v : vs) t.release(v);
        vs.clear();
        for (uint32_t i = 0; i < voices; ++i) vs.push_back(t.submit(w, {0, 0, 0, stream_samples > 1024 ? stream_samples - 600 : 0}));
        for (auto& v : vs) t.enable(v);
        std::vector<std::size_t> local(vs.size(), 0);
        const auto tick = std::chrono::milliseconds(5);
        const std::size_t need = std::size_t(rate) * ch / 200;
        auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        std::this_thread::sleep_until(next);
        const auto end = next + std::chrono::milliseconds(int(realtime * 1000));
        while (std::chrono::steady_clock::now() < end) {
            for (std::size_t i = 0; i < vs.size(); ++i) {
                sink.clear();
                t.update(vs[i], sink);
                local[i] += sink.size();
                if (local[i] < need) ++underruns;
                local[i] -= std::min(local[i], need);
            }
            ++ticks;
            next += tick;
            std::this_thread::sleep_until(next);
        }
    }
    for (auto& v : vs) t.release(v);
    gta6rt_xma_unbind();
    const double audio = seconds * voices;
    std::printf(",\"voices\":%u,\"workers\":%u,\"audio_seconds_per_voice\":%.2f,\"wall_s\":%.3f,\"process_cpu_s\":%.3f,"
                "\"decode_cpu_s\":%.3f,\"engine_frames\":%llu,\"realtime_factor\":%.1f,"
                "\"decode_ms_per_voice_audio_second\":%.3f,\"process_cpu_ms_per_voice_audio_second\":%.3f,"
                "\"realtime_s\":%.1f,\"realtime_ticks\":%llu,\"realtime_underruns\":%llu}\n",
                voices, s1.workers, seconds, wall, cpu, (s1.decode_ns - s0.decode_ns) / 1e9,
                (unsigned long long)(s1.frames - s0.frames), audio / wall, (s1.decode_ns - s0.decode_ns) / 1e6 / audio,
                cpu * 1000 / audio, realtime, (unsigned long long)ticks, (unsigned long long)underruns);
    return 0;
}
