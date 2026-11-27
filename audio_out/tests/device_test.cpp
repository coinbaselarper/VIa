#include "check.h"
#include "harness.h"
#include "wav.h"

#include <sys/resource.h>

#include <cstdlib>
#include <string>

namespace {

double cpu_s() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec * 1e-6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec * 1e-6;
}

double goertzel_amp(const float* x, size_t n, size_t stride, double f, double rate) {
    const double w = 2 * M_PI * f / rate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        const double s0 = x[i * stride] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    return 2 * std::sqrt(re * re + im * im) / n;
}

struct Tone71 : Harness {};

}  // namespace

int main(int argc, char** argv) {
    setenv("GTA6RT_AUDIO", "on", 1);
    setenv("GTA6RT_AUDIO_MUTE", "1", 1);
    setenv("GTA6RT_AUDIO_STATS", "0", 1);
    const std::string wav = argc > 1 ? argv[1] : "audio_out_device_test.wav";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 10;

    gta6rt_audio_stream_desc d{48000, 2, 0x3, 4800, "device-test"};
    gta6rt_audio_stream* s = nullptr;
    CHECK(gta6rt_audio_stream_open(&d, &s) == 0, "open");
    gta6rt_audio_device_stats ds{};
    for (int i = 0; i < 200 && ds.device_rate == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        gta6rt_audio_device_stats_get(&ds);
    }
    CHECK(ds.device_rate != 0, "default output device opened");
    std::printf("device: %u ch, %u Hz, io %u frames, latency %u frames; stream latency %u frames at 48 kHz\n",
                ds.device_channels, ds.device_rate, ds.io_frames, ds.latency_frames, gta6rt_audio_stream_latency(s));
    CHECK(gta6rt_audio_out_set_tap(wav.c_str()) == 0, "tap");

    Harness h;
    h.s = s;
    h.channels = 2;
    gta6rt_audio_stream_start(s);
    const double c0 = cpu_s();
    const uint64_t t0 = now_ns();
    h.run_threads();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    gta6rt_audio_stats st;
    gta6rt_audio_stream_stats(s, &st);
    gta6rt_audio_device_stats_get(&ds);
    const double c1 = cpu_s();
    const uint64_t t1 = now_ns();
    const double wall = (t1 - t0) * 1e-9;
    std::printf("phase A (%d s, jittery producer, master muted): passes %llu, consumed %llu, underruns %llu (%llu frames), "
                "dropped %llu, callbacks %llu, callback us p50 %.0f p90 %.0f p99 %.0f p99.9 %.0f max %.0f, interval us p50 "
                "%.0f p99 %.0f max %.0f\n",
                seconds, (unsigned long long)h.passes.load(), (unsigned long long)st.frames_consumed,
                (unsigned long long)st.underruns, (unsigned long long)st.underrun_frames,
                (unsigned long long)st.frames_dropped, (unsigned long long)ds.callbacks, ds.callback_p50_us,
                ds.callback_p90_us, ds.callback_p99_us, ds.callback_p999_us, ds.callback_max_us,
                ds.callback_interval_p50_us, ds.callback_interval_p99_us, ds.callback_interval_max_us);
    std::printf("cost: process CPU %.1f ms per s of audio (producer + pacer + reader threads + callback), callback %.2f ms "
                "per s of audio\n",
                (c1 - c0) * 1000 / wall, ds.callback_cpu_us_total / 1000 / wall);
    const Fit fit = fit_positions(h.positions, t0 + 500'000'000ull);
    std::printf("position: %zu samples, regressions %llu, fitted rate %.2f Hz, max deviation %.1f frames, max write %.1f us\n",
                h.positions.size(), (unsigned long long)h.position_regressions.load(), fit.rate, fit.max_dev,
                h.max_write_ns.load() / 1000.0);
    CHECK(st.underruns == 0, "no underruns with the device (%llu)", (unsigned long long)st.underruns);
    CHECK(h.position_regressions.load() == 0, "position monotonic");
    CHECK(std::fabs(fit.rate - 48000) < 48000 * 0.002, "device consumes at 48 kHz +-0.2%% (%.2f)", fit.rate);
    CHECK(fit.max_dev < 1024, "position within 1024 frames of the fit (%.1f)", fit.max_dev);
    CHECK(ds.callback_p99_us < 2000, "callback p99 under 2 ms (%.0f us)", ds.callback_p99_us);
    CHECK(h.max_write_ns.load() < 1'000'000, "write never waits");

    const uint64_t changes0 = ds.device_changes;
    const size_t mark = h.positions.size();
    gta6rt_audio_out_force_reconfigure();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    h.join();
    gta6rt_audio_device_stats_get(&ds);
    gta6rt_audio_stats st2;
    gta6rt_audio_stream_stats(s, &st2);
    uint64_t max_gap_ns = 0, last_t = 0, last_p = 0;
    for (size_t i = mark; i < h.positions.size(); ++i) {
        auto [t, p] = h.positions[i];
        if (p != last_p) {
            if (last_t) max_gap_ns = std::max(max_gap_ns, t - last_t);
            last_t = t;
            last_p = p;
        }
    }
    std::printf("phase B (output unit rebuilt as on a device change): device changes %llu -> %llu, longest time without "
                "position progress %.1f ms, host-clock frames %llu, underruns during the switch %llu (%llu frames), max write %.1f us\n",
                (unsigned long long)changes0, (unsigned long long)ds.device_changes, max_gap_ns / 1e6,
                (unsigned long long)st2.clocked_frames, (unsigned long long)(st2.underruns - st.underruns),
                (unsigned long long)(st2.underrun_frames - st.underrun_frames), h.max_write_ns.load() / 1000.0);
    CHECK(ds.device_changes == changes0 + 1, "the unit was rebuilt");
    CHECK(max_gap_ns < 100'000'000ull, "position keeps moving through the rebuild (%.1f ms)", max_gap_ns / 1e6);
    CHECK(h.max_write_ns.load() < 1'000'000, "producer never blocked through the rebuild");
    CHECK(h.position_regressions.load() == 0, "position monotonic through the rebuild");
    gta6rt_audio_stream_stop(s);
    gta6rt_audio_stream_close(s);
    gta6rt_audio_out_set_tap(nullptr);
    gta6rt_audio_out_shutdown();

    gta6rt::audio_out::WavData w;
    CHECK(gta6rt::audio_out::read_wav(wav, &w) && w.rate == 48000, "tap WAV");
    const size_t ch = w.channels, frames = w.samples.size() / ch;
    const size_t at = frames / 3, n = 48000;
    if (at + n < frames && ch >= 2) {
        const double a440 = goertzel_amp(w.samples.data() + at * ch + 1, n, ch, 880.0, 48000);
        std::printf("tap (%zu ch, %zu frames): FR carries the 880 Hz test sine at amplitude %.4f (written 0.25)\n", ch,
                    frames, a440);
        CHECK(std::fabs(a440 - 0.25) < 0.005, "tone amplitude through the device path (%.4f)", a440);
    }
    return finish("audio_out_device_test");
}
