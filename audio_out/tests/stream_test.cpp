#include "check.h"
#include "harness.h"
#include "wav.h"

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    setenv("GTA6RT_AUDIO", "null", 1);
    setenv("GTA6RT_AUDIO_STATS", "0", 1);
    setenv("GTA6RT_AUDIO_NULL_CHANNELS", "2", 1);
    const std::string wav = argc > 1 ? argv[1] : "audio_out_stream_test.wav";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 8;

    CHECK(gta6rt_audio_out_mode() == GTA6RT_AUDIO_NULL, "mode from GTA6RT_AUDIO=null");
    gta6rt_audio_stream_desc d{48000, 2, 0x3, 4800, "stereo"};
    gta6rt_audio_stream* s = nullptr;
    CHECK(gta6rt_audio_stream_open(&d, &s) == 0 && s, "open");
    gta6rt_audio_stream_desc bad{44100, 2, 0x3, 4800, "bad rate"};
    gta6rt_audio_stream* b = nullptr;
    CHECK(gta6rt_audio_stream_open(&bad, &b) != 0 && !b, "44.1 kHz stream refused with a log line");
    CHECK(gta6rt_audio_out_set_tap(wav.c_str()) == 0, "tap");

    Harness h;
    h.s = s;
    h.channels = 2;
    gta6rt_audio_stream_start(s);
    const uint64_t t_start = now_ns();
    h.run_threads();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    h.join();
    gta6rt_audio_stream_stop(s);
    gta6rt_audio_stats st;
    gta6rt_audio_stream_stats(s, &st);
    const Fit fit = fit_positions(h.positions, t_start + 500'000'000ull);
    std::printf("null clock, jittery producer (10%% of passes late 5..20 ms), %d s: passes %llu, written %llu, consumed %llu, "
                "underruns %llu (%llu frames), dropped %llu, position samples %zu, regressions %llu, fitted rate %.2f Hz, "
                "max deviation %.1f frames, max write call %.1f us\n",
                seconds, (unsigned long long)h.passes.load(), (unsigned long long)st.frames_written,
                (unsigned long long)st.frames_consumed, (unsigned long long)st.underruns,
                (unsigned long long)st.underrun_frames, (unsigned long long)st.frames_dropped, h.positions.size(),
                (unsigned long long)h.position_regressions.load(), fit.rate, fit.max_dev, h.max_write_ns.load() / 1000.0);
    CHECK(st.underruns == 0, "no underruns (%llu)", (unsigned long long)st.underruns);
    CHECK(st.frames_dropped == 0, "no drops");
    CHECK(h.position_regressions.load() == 0, "position monotonic");
    CHECK(std::fabs(fit.rate - 48000) < 48, "position advances at 48000 Hz +-0.1%% (%.2f)", fit.rate);
    CHECK(fit.max_dev < 480, "position within one pass of the fitted line (%.1f frames)", fit.max_dev);
    CHECK(h.max_write_ns.load() < 1'000'000, "write never waits (max %.1f us)", h.max_write_ns.load() / 1000.0);
    CHECK(gta6rt_audio_stream_padding(s) == st.frames_written - st.frames_consumed, "padding = written - consumed");

    const uint64_t p0 = gta6rt_audio_stream_position(s, nullptr);
    const uint32_t pad0 = gta6rt_audio_stream_padding(s);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(gta6rt_audio_stream_position(s, nullptr) == p0 && gta6rt_audio_stream_padding(s) == pad0,
          "stopped: position and padding frozen");
    std::vector<float> big(10000 * 2, 0.0f);
    const uint64_t t0 = now_ns();
    const uint32_t took = gta6rt_audio_stream_write(s, big.data(), 10000);
    const uint64_t dt = now_ns() - t0;
    CHECK(took == 4800 - pad0, "write into a nearly full ring takes only the room (%u of 10000)", took);
    CHECK(dt < 1'000'000, "full ring write returns at once (%.1f us)", dt / 1000.0);
    gta6rt_audio_stream_stats(s, &st);
    CHECK(st.frames_dropped == 10000 - took, "dropped frames counted");
    CHECK(gta6rt_audio_stream_reset(s) == 0 && gta6rt_audio_stream_padding(s) == 0, "reset empties the ring");

    gta6rt_audio_stream_desc d71{48000, 8, 0x63F, 4800, "7.1"};
    gta6rt_audio_stream* s8 = nullptr;
    CHECK(gta6rt_audio_stream_open(&d71, &s8) == 0, "open 7.1");
    gta6rt_audio_stream_start(s8);
    std::vector<float> tone(4800 * 8, 0.0f);
    for (int i = 0; i < 4800; ++i) {
        tone[size_t(i) * 8 + 0] = 0.5f * std::sin(2 * M_PI * 1000 * i / 48000.0);
        tone[size_t(i) * 8 + 6] = 0.5f * std::sin(2 * M_PI * 3000 * i / 48000.0);
    }
    gta6rt_audio_stream_write(s8, tone.data(), 4800);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    gta6rt_audio_stream_stop(s8);
    gta6rt_audio_stream_close(s8);
    gta6rt_audio_stream_close(s);
    gta6rt_audio_out_set_tap(nullptr);
    gta6rt_audio_out_shutdown();

    gta6rt::audio_out::WavData w;
    CHECK(gta6rt::audio_out::read_wav(wav, &w) && w.channels == 2 && w.rate == 48000, "tap WAV readable");
    const size_t frames = w.samples.size() / 2;
    size_t first = 0;
    while (first < frames && w.samples[first * 2] == 0.0f && w.samples[first * 2 + 1] == 0.0f) ++first;
    size_t mismatches = 0, n = 0;
    for (; n < st.frames_consumed && first + n < frames; ++n) {
        for (uint32_t c = 0; c < 2; ++c)
            if (w.samples[(first + n) * 2 + c] != Harness::sample(n, c)) ++mismatches;
    }
    std::printf("tap: %zu frames, stream content from frame %zu: %zu frames compared, %zu sample mismatches\n", frames,
                first, n, mismatches);
    CHECK(n == st.frames_consumed && mismatches == 0, "tap holds the written frames in order, sample-exact");
    double el = 0, er = 0;
    size_t tail = first + n;
    while (tail < frames && w.samples[tail * 2] == 0.0f) ++tail;
    tail += 1200;
    for (size_t i = tail; i < tail + 2400 && i < frames; ++i) {
        el += w.samples[i * 2] * w.samples[i * 2];
        er += w.samples[i * 2 + 1] * w.samples[i * 2 + 1];
    }
    const double rl = std::sqrt(el / 2400), rr = std::sqrt(er / 2400);
    const double expect_l = std::sqrt(0.125 + 0.5 * 0.125);
    std::printf("7.1 stream through the stereo null output: L rms %.4f (expect %.4f: FL 1.0 + SL 0.707), R rms %.4f\n", rl,
                expect_l, rr);
    CHECK(std::fabs(rl - expect_l) < 0.005 && rr < 1e-6, "7.1 FL+SL downmixed into L only");
    return finish("audio_out_stream_test");
}
