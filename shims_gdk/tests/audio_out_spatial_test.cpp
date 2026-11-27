// The spatial stream on the host output (GTA6RT_AUDIO=null by default, --device for CoreAudio with the master muted):
// a stand-in title renders FL and FR objects from its own thread on the stream's event, with jitter; the tap WAV must
// hold both tones at their levels, in their channels, with no underrun.
#include <windows.h>

#include <objbase.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <spatialaudioclient.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gta6rt/audio_out.h>

#include "silent_audio.h"

namespace {

namespace sa = rt::silent_audio;
int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

double tone(const std::vector<float>& x, size_t ch, size_t c, size_t a, size_t n, double f) {
    double re = 0, im = 0;
    for (size_t i = 0; i < n; ++i) {
        re += x[(a + i) * ch + c] * std::cos(2 * M_PI * f * i / 48000);
        im += x[(a + i) * ch + c] * std::sin(2 * M_PI * f * i / 48000);
    }
    return 2 * std::sqrt(re * re + im * im) / n;
}

bool read_float_wav(const std::string& path, std::vector<float>* out, uint32_t* channels) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t h[44];
    if (std::fread(h, 1, 44, f) != 44 || std::memcmp(h, "RIFF", 4) || h[20] != 3) {
        std::fclose(f);
        return false;
    }
    *channels = h[22];
    float buf[4096];
    size_t n;
    while ((n = std::fread(buf, 4, 4096, f)) > 0) out->insert(out->end(), buf, buf + n);
    std::fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const bool device = argc > 1 && !std::strcmp(argv[1], "--device");
    setenv("GTA6RT_AUDIO", device ? "on" : "null", 1);
    setenv("GTA6RT_AUDIO_MUTE", "1", 1);
    setenv("GTA6RT_AUDIO_STATS", "0", 1);
    const std::string wav = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/audio_out_spatial_test.wav";
    setenv("GTA6RT_AUDIO_WAV", wav.c_str(), 1);

    IMMDeviceEnumerator* e = nullptr;
    sa::serve_enumerator(nullptr, REGDB_E_CLASSNOTREG, sa::Mode::Auto, nullptr, __uuidof(IMMDeviceEnumerator),
                         reinterpret_cast<void**>(&e));
    IMMDevice* d = nullptr;
    check(e && e->GetDefaultAudioEndpoint(eRender, eConsole, &d) == S_OK && d, "endpoint");
    ISpatialAudioClient* c = nullptr;
    check(d && d->Activate(__uuidof(ISpatialAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&c)) == S_OK,
          "Activate(ISpatialAudioClient)");
    if (!c) return 1;
    WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT, 1, 48000, 48000 * 4, 4, 32, 0};
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SpatialAudioObjectRenderStreamActivationParams params{};
    params.ObjectFormat = &format;
    params.StaticObjectTypeMask = static_cast<AudioObjectType>(AudioObjectType_FrontLeft | AudioObjectType_FrontRight);
    params.Category = AudioCategory_GameEffects;
    params.EventHandle = event;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof params;
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    ISpatialAudioObjectRenderStream* stream = nullptr;
    check(c->ActivateSpatialAudioStream(&pv, __uuidof(ISpatialAudioObjectRenderStream), reinterpret_cast<void**>(&stream)) ==
              S_OK,
          "ActivateSpatialAudioStream(FL | FR) like the title (mask 0x6)");
    if (!stream) return 1;
    ISpatialAudioObject* fl = nullptr;
    ISpatialAudioObject* fr = nullptr;
    check(stream->ActivateSpatialAudioObject(AudioObjectType_FrontLeft, &fl) == S_OK &&
              stream->ActivateSpatialAudioObject(AudioObjectType_FrontRight, &fr) == S_OK,
          "FL and FR objects");
    check(stream->Start() == S_OK, "Start");

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> passes{0}, late{0};
    const DWORD t_begin = GetTickCount();
    std::thread title([&] {
        std::mt19937 rng(3);
        std::uniform_real_distribution<double> u(0, 1);
        uint64_t n = 0;
        while (!stop.load()) {
            if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;
            const double r = u(rng);
            const double ms = r < 0.05 ? 5 + 20 * u(rng) : 2 * u(rng);
            if (ms > 5) late.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(ms * 1000)));
            UINT32 available = 0, frames = 0;
            if (stream->BeginUpdatingAudioObjects(&available, &frames) != S_OK) continue;
            BYTE* b = nullptr;
            UINT32 len = 0;
            if (fl->GetBuffer(&b, &len) == S_OK)
                for (UINT32 i = 0; i < frames; ++i)
                    reinterpret_cast<float*>(b)[i] = 0.3f * static_cast<float>(std::sin(2 * M_PI * 440.0 * (n + i) / 48000));
            if (fr->GetBuffer(&b, &len) == S_OK)
                for (UINT32 i = 0; i < frames; ++i)
                    reinterpret_cast<float*>(b)[i] = 0.2f * static_cast<float>(std::sin(2 * M_PI * 660.0 * (n + i) / 48000));
            n += frames;
            stream->EndUpdatingAudioObjects();
            passes.fetch_add(1);
        }
    });
    Sleep(1000);
    const uint64_t passes_1s = passes.load();
    const DWORD t_1s = GetTickCount();
    Sleep(5000);
    const double steady = (passes.load() - passes_1s) / ((GetTickCount() - t_1s) / 1000.0);
    gta6rt_audio_stats st;
    gta6rt_audio_out_streams_stats(&st);
    stop.store(true);
    title.join();
    const double seconds = (GetTickCount() - t_begin) / 1000.0;
    stream->Stop();
    std::printf("%s: %llu passes in %.2f s (%.1f/s after the first second), %llu late 5..25 ms; written %llu consumed %llu underruns %llu "
                "(%llu frames) dropped %llu\n",
                device ? "CoreAudio (muted)" : "null clock", (unsigned long long)passes.load(), seconds,
                steady, (unsigned long long)late.load(), (unsigned long long)st.frames_written,
                (unsigned long long)st.frames_consumed, (unsigned long long)st.underruns,
                (unsigned long long)st.underrun_frames, (unsigned long long)st.frames_dropped);
    check(std::fabs(steady - 100.0) < 2.0, "100 passes a second once the output runs");
    check(st.underruns == 0, "no underruns with a jittery title thread");
    check(st.frames_dropped == 0, "no drops");
    fl->Release();
    fr->Release();
    stream->Release();
    c->Release();
    d->Release();
    e->Release();
    gta6rt_audio_out_set_tap(nullptr);
    gta6rt_audio_out_shutdown();

    std::vector<float> x;
    uint32_t ch = 0;
    check(read_float_wav(wav, &x, &ch) && ch >= 2, "tap WAV");
    const size_t frames = ch ? x.size() / ch : 0;
    if (frames > 96000) {
        const size_t a = frames / 2;
        const double l440 = tone(x, ch, 0, a, 48000, 440), r660 = tone(x, ch, 1, a, 48000, 660);
        const double l660 = tone(x, ch, 0, a, 48000, 660), r440 = tone(x, ch, 1, a, 48000, 440);
        std::printf("tap: L 440 Hz %.4f (0.3 written to FL), R 660 Hz %.4f (0.2 to FR), crosstalk L660 %.5f R440 %.5f\n",
                    l440, r660, l660, r440);
        check(std::fabs(l440 - 0.3) < 0.003 && std::fabs(r660 - 0.2) < 0.003, "object levels reach the output");
        check(l660 < 1e-3 && r440 < 1e-3, "FL and FR stay in their channels");
    }
    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
