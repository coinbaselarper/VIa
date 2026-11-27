// Host-driven fake client for the guest-facing XAudio2 objects, engine in Manual mode (the test runs each processing
// pass). Covers Bink2Scarlett's exact call sequence (disassembled from Bink2Scarlett.dll, see STATUS.md) plus the rest
// of the 2.9 surface that is implemented.
#include "media_common.h"
#include "media_exports.h"
#include "test_util.h"
#include "xaudio2_engine.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace rt::media;
using mt::call;

namespace {

enum Slot {
    QI = 0, AddRef = 1, Release = 2, RegisterCb = 3, UnregisterCb = 4, CreateSource = 5, CreateSubmix = 6,
    CreateMaster = 7, StartEngine = 8, StopEngine = 9, Commit = 10, PerfData = 11,
    Details = 0, SetOutputVoices = 1, SetEffectChain = 2, SetFilter = 8, SetVolume = 12, GetVolume = 13,
    SetChannelVolumes = 14, GetChannelVolumes = 15, SetOutputMatrix = 16, GetOutputMatrix = 17, Destroy = 18,
    Start = 19, Stop = 20, Submit = 21, Flush = 22, Discontinuity = 23, ExitLoop = 24, GetState = 25,
    SetFreqRatio = 26, GetFreqRatio = 27, SetSourceRate = 28, GetChannelMask = 19,
};

struct Event {
    std::string what;
    uint64_t arg;
    uint64_t pass;
};

uint64_t g_pass = 0;

struct FakeCallback {
    const void* const* vtbl;
    std::vector<Event> events;
};

#define CB_FN(name, ...) GTA6RT_GUEST_ABI void name(FakeCallback* self __VA_OPT__(, ) __VA_ARGS__)
CB_FN(cb_pass_start, uint32_t bytes) { self->events.push_back({"PassStart", bytes, g_pass}); }
CB_FN(cb_pass_end) { self->events.push_back({"PassEnd", 0, g_pass}); }
CB_FN(cb_stream_end) { self->events.push_back({"StreamEnd", 0, g_pass}); }
CB_FN(cb_buffer_start, uint64_t ctx) { self->events.push_back({"BufferStart", ctx, g_pass}); }
CB_FN(cb_buffer_end, uint64_t ctx) { self->events.push_back({"BufferEnd", ctx, g_pass}); }
CB_FN(cb_loop_end, uint64_t ctx) { self->events.push_back({"LoopEnd", ctx, g_pass}); }
CB_FN(cb_voice_error, uint64_t ctx, int32_t hr) { self->events.push_back({"VoiceError", ctx, g_pass}); }
const void* const kCbVtbl[] = {
    reinterpret_cast<const void*>(&cb_pass_start),   reinterpret_cast<const void*>(&cb_pass_end),
    reinterpret_cast<const void*>(&cb_stream_end),   reinterpret_cast<const void*>(&cb_buffer_start),
    reinterpret_cast<const void*>(&cb_buffer_end),   reinterpret_cast<const void*>(&cb_loop_end),
    reinterpret_cast<const void*>(&cb_voice_error),
};

struct FakeEngineCallback {
    const void* const* vtbl;
    std::vector<Event>* sink;
};
GTA6RT_GUEST_ABI void ecb_start(FakeEngineCallback* s) { s->sink->push_back({"EnginePassStart", 0, g_pass}); }
GTA6RT_GUEST_ABI void ecb_end(FakeEngineCallback* s) { s->sink->push_back({"EnginePassEnd", 0, g_pass}); }
GTA6RT_GUEST_ABI void ecb_error(FakeEngineCallback* s, int32_t) { s->sink->push_back({"CriticalError", 0, g_pass}); }
const void* const kEcbVtbl[] = {reinterpret_cast<const void*>(&ecb_start), reinterpret_cast<const void*>(&ecb_end),
                                reinterpret_cast<const void*>(&ecb_error)};

WaveFormatEx pcm(uint16_t ch, uint32_t rate, uint16_t bits, uint16_t tag = kWaveFormatPcm) {
    WaveFormatEx f{};
    f.wFormatTag = tag;
    f.nChannels = ch;
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = bits;
    f.nBlockAlign = static_cast<uint16_t>(ch * bits / 8);
    f.nAvgBytesPerSec = rate * f.nBlockAlign;
    return f;
}

XAudio2Buffer buffer(const void* data, uint32_t bytes, uint64_t ctx = 0, uint32_t flags = 0) {
    XAudio2Buffer b{};
    b.Flags = flags;
    b.AudioBytes = bytes;
    b.pAudioData = reinterpret_cast<uint64_t>(data);
    b.pContext = ctx;
    return b;
}

XAudio2VoiceState state(uint64_t v) {
    XAudio2VoiceState s{};
    call<void>(v, GetState, &s, 0u);
    return s;
}

struct Rig {
    uint64_t x = 0, master = 0;
    rt::xa2::Engine* e = nullptr;
    std::vector<float> out;
    Rig(uint32_t flags = 0x400, uint32_t ch = 0, uint32_t rate = 0) {
        CHECK_HR(XAudio2Create(&x, flags, 1), kSOk);
        e = rt::xa2::engine_of(x);
        CHECK_HR((call<int32_t>(x, CreateMaster, &master, ch, rate, 0u, static_cast<const char16_t*>(nullptr),
                                static_cast<const void*>(nullptr), 6u)),
                 kSOk);
        out.assign(static_cast<size_t>(e->master_channels()) * e->quantum(), 0.0f);
    }
    void pass(int n = 1) {
        for (int i = 0; i < n; ++i) {
            ++g_pass;
            e->run_pass(out.data());
        }
    }
    ~Rig() {
        call<void>(master, Destroy);
        CHECK(call<uint32_t>(x, Release) == 0);
    }
};

uint64_t source(Rig& r, const WaveFormatEx& f, uint32_t flags = 0, float ratio = 2.0f, FakeCallback* cb = nullptr,
                const XAudio2VoiceSends* sends = nullptr) {
    uint64_t v = 0;
    CHECK_HR((call<int32_t>(r.x, CreateSource, &v, &f, flags, ratio, cb, sends, static_cast<const void*>(nullptr))),
             kSOk);
    return v;
}

// Bink2Scarlett's XAudio2 sound driver (BinkOpenXAudio2, Bink2Scarlett.dll+0xb810 and +0xb190..+0xb430): 48 kHz
// stereo 16-bit, 16 buffers of bytes/s/16 rounded up to 32, MaxFrequencyRatio 1.0, flags NOPITCH, no callback, polls
// GetState and counts BuffersQueued.
void test_bink_sequence() {
    Rig r;
    CHECK(r.e->master_channels() == 2 && r.e->engine_rate() == 48000 && r.e->quantum() == 480);
    uint32_t mask = 0;
    CHECK_HR(call<int32_t>(r.master, GetChannelMask, &mask), kSOk);
    CHECK(mask == 3);
    XAudio2VoiceDetails md{};
    call<void>(r.master, Details, &md);
    CHECK(md.InputChannels == 2 && md.InputSampleRate == 48000);

    const WaveFormatEx f = pcm(2, 48000, 16);
    const uint64_t v = source(r, f, kXa2VoiceNoPitch, 1.0f);
    const uint32_t bytes = ((48000 * 2 * 2) / 16 + 31) & ~31u;
    CHECK(bytes == 12000);
    std::vector<int16_t> ring(16 * bytes / 2);
    for (size_t i = 0; i < ring.size(); ++i) ring[i] = static_cast<int16_t>((i * 37) % 20000 - 10000);
    for (int i = 0; i < 8; ++i) {
        const XAudio2Buffer b = buffer(&ring[i * bytes / 2], bytes);
        CHECK_HR(call<int32_t>(v, Submit, &b, static_cast<const void*>(nullptr)), kSOk);
    }
    CHECK(state(v).BuffersQueued == 8);
    r.pass();
    CHECK(state(v).SamplesPlayed == 0);
    CHECK_HR(call<int32_t>(v, Start, 0u, 0u), kSOk);
    r.pass();
    bool exact = true;
    for (uint32_t i = 0; i < 480 * 2; ++i) exact &= r.out[i] == static_cast<float>(ring[i]) / 32768.0f;
    CHECK(exact);
    r.pass(5);
    CHECK(state(v).BuffersQueued == 8);
    CHECK(state(v).SamplesPlayed == 2880);
    r.pass();
    CHECK(state(v).BuffersQueued == 7);
    CHECK(state(v).SamplesPlayed == 3360);

    const float half[2] = {0.5f, 0.5f};
    CHECK_HR(call<int32_t>(v, SetChannelVolumes, 2u, half, 0u), kSOk);
    const uint64_t played = state(v).SamplesPlayed;
    r.pass();
    exact = true;
    for (uint32_t i = 0; i < 480 * 2; ++i)
        exact &= r.out[i] == 0.5f * static_cast<float>(ring[played * 2 + i]) / 32768.0f;
    CHECK(exact);
    const float m8[16] = {1, 0, 0, 1};
    CHECK_HR(call<int32_t>(v, SetOutputMatrix, uint64_t{0}, 2u, 8u, m8, 0u), kXAudio2EInvalidCall);
    CHECK_HR(call<int32_t>(v, SetOutputMatrix, uint64_t{0}, 2u, 2u, m8, 0u), kSOk);
    CHECK_HR(call<int32_t>(v, SetFreqRatio, 0.5f, 0u), kXAudio2EInvalidCall);

    // Bink's stop path: Stop(0, 0) keeps the queue; Start(0, 0) resumes.
    CHECK_HR(call<int32_t>(v, Stop, 0u, 0u), kSOk);
    const XAudio2VoiceState s0 = state(v);
    r.pass(3);
    const XAudio2VoiceState s1 = state(v);
    CHECK(s0.BuffersQueued == s1.BuffersQueued && s0.SamplesPlayed == s1.SamplesPlayed);
    CHECK_HR(call<int32_t>(v, Start, 0u, 0u), kSOk);
    // Underflow: Bink sees BuffersQueued == 0 once all 8 buffers (24000 frames) are played.
    r.pass(60);
    CHECK(state(v).BuffersQueued == 0);
    CHECK(state(v).SamplesPlayed == 24000);
    call<void>(v, Destroy);
}

void test_callbacks() {
    Rig r;
    std::vector<Event> engine_events;
    FakeEngineCallback ecb{kEcbVtbl, &engine_events};
    CHECK_HR(call<int32_t>(r.x, RegisterCb, &ecb), kSOk);
    FakeCallback cb{kCbVtbl, {}};
    const WaveFormatEx f = pcm(1, 48000, 16);
    const uint64_t v = source(r, f, 0, 2.0f, &cb);
    std::vector<int16_t> data(3000, 1000);
    for (int i = 0; i < 3; ++i) {
        const XAudio2Buffer b = buffer(&data[i * 1000], 2000, 100 + i, i == 2 ? kXa2EndOfStream : 0);
        CHECK_HR(call<int32_t>(v, Submit, &b, static_cast<const void*>(nullptr)), kSOk);
    }
    CHECK_HR(call<int32_t>(v, Start, 0u, 0u), kSOk);
    const uint64_t p0 = g_pass;
    r.pass(8);
    auto find = [&](const char* what, uint64_t arg) -> int64_t {
        for (const Event& e : cb.events)
            if (e.what == what && e.arg == arg) return static_cast<int64_t>(e.pass - p0);
        return -1;
    };
    // Frame 999 is output in pass 3 (frames 960..1439), 1999 in pass 5, 2999 in pass 7.
    CHECK(find("BufferStart", 100) == 1);
    CHECK(find("BufferEnd", 100) == 3);
    CHECK(find("BufferStart", 101) == 3);
    CHECK(find("BufferEnd", 101) == 5);
    CHECK(find("BufferEnd", 102) == 7);
    CHECK(find("StreamEnd", 0) == 7);
    CHECK(state(v).SamplesPlayed == 0);
    int order_ok = 1;
    for (size_t i = 1; i < cb.events.size(); ++i)
        if (cb.events[i].what == "StreamEnd") order_ok &= cb.events[i - 1].what == "BufferEnd";
    CHECK(order_ok);
    uint64_t first_bytes = ~0ull, pass7_bytes = 0;
    for (const Event& e : cb.events) {
        if (e.what != "PassStart") continue;
        if (e.pass - p0 == 1) first_bytes = e.arg;
        if (e.pass - p0 == 7) pass7_bytes = e.arg;
    }
    CHECK(first_bytes == 0);
    CHECK(pass7_bytes == (480 - 120) * 2);
    // Per pass: engine start, voice pass start, ..., voice pass end, engine end.
    bool framing = !engine_events.empty() && engine_events.front().what == "EnginePassStart" &&
                   engine_events.back().what == "EnginePassEnd" && engine_events.size() == 16;
    CHECK(framing);
    int pass_starts = 0, pass_ends = 0;
    for (const Event& e : cb.events) {
        pass_starts += e.what == "PassStart";
        pass_ends += e.what == "PassEnd";
    }
    CHECK(pass_starts == 8 && pass_ends == 8);
    call<void>(r.x, UnregisterCb, &ecb);
    r.pass();
    CHECK(engine_events.size() == 16);
    call<void>(v, Destroy);
}

void test_loops_and_exit() {
    Rig r;
    FakeCallback cb{kCbVtbl, {}};
    const WaveFormatEx f = pcm(1, 48000, 16);
    const uint64_t v = source(r, f, 0, 2.0f, &cb);
    std::vector<int16_t> ramp(100);
    for (int i = 0; i < 100; ++i) ramp[i] = static_cast<int16_t>(i * 100);
    XAudio2Buffer b = buffer(ramp.data(), 200, 7);
    b.LoopBegin = 20;
    b.LoopLength = 30;
    b.LoopCount = 2;
    CHECK_HR(call<int32_t>(v, Submit, &b, static_cast<const void*>(nullptr)), kSOk);
    CHECK_HR(call<int32_t>(v, Start, 0u, 0u), kSOk);
    r.pass();
    std::vector<int> want;
    for (int i = 0; i < 50; ++i) want.push_back(i);
    for (int k = 0; k < 2; ++k)
        for (int i = 20; i < 50; ++i) want.push_back(i);
    for (int i = 50; i < 100; ++i) want.push_back(i);
    CHECK(want.size() == 160);
    bool ok = true;
    for (size_t i = 0; i < 480; ++i) {
        const float expect = i < want.size() ? static_cast<float>(want[i] * 100) / 32768.0f : 0.0f;
        ok &= r.out[i * 2] == expect && r.out[i * 2 + 1] == expect;
    }
    CHECK(ok);
    int loops = 0;
    for (const Event& e : cb.events) loops += e.what == "LoopEnd" && e.arg == 7;
    CHECK(loops == 2);
    CHECK(state(v).BuffersQueued == 0);

    XAudio2Buffer inf = buffer(ramp.data(), 200, 8);
    inf.LoopCount = kXa2LoopInfinite;
    CHECK_HR(call<int32_t>(v, Submit, &inf, static_cast<const void*>(nullptr)), kSOk);
    r.pass(3);
    CHECK(state(v).BuffersQueued == 1);
    CHECK_HR(call<int32_t>(v, ExitLoop, 0u), kSOk);
    r.pass();
    CHECK(state(v).BuffersQueued == 0);

    XAudio2Buffer bad = buffer(ramp.data(), 200);
    bad.LoopBegin = 10;
    CHECK_HR(call<int32_t>(v, Submit, &bad, static_cast<const void*>(nullptr)), kXAudio2EInvalidCall);
    bad.LoopCount = 1;
    bad.LoopLength = 200;
    CHECK_HR(call<int32_t>(v, Submit, &bad, static_cast<const void*>(nullptr)), kXAudio2EInvalidCall);
    XAudio2Buffer region = buffer(ramp.data(), 200);
    region.PlayBegin = 90;
    region.PlayLength = 20;
    CHECK_HR(call<int32_t>(v, Submit, &region, static_cast<const void*>(nullptr)), kXAudio2EInvalidCall);
    region.PlayLength = 10;
    CHECK_HR(call<int32_t>(v, Submit, &region, static_cast<const void*>(nullptr)), kSOk);
    r.pass();
    CHECK(r.out[0] == 9000.0f / 32768.0f && r.out[18] == 9900.0f / 32768.0f && r.out[20] == 0.0f);
    call<void>(v, Destroy);
}

void test_resample_and_ratio() {
    Rig r;
    const WaveFormatEx f = pcm(1, 24000, 16);
    const uint64_t v = source(r, f);
    std::vector<int16_t> ramp(2400);
    for (int i = 0; i < 2400; ++i) ramp[i] = static_cast<int16_t>(i * 10);
    const XAudio2Buffer b = buffer(ramp.data(), 4800);
    CHECK_HR(call<int32_t>(v, Submit, &b, static_cast<const void*>(nullptr)), kSOk);
    CHECK_HR(call<int32_t>(v, Start, 0u, 0u), kSOk);
    r.pass();
    bool ok = true;
    for (int i = 0; i < 480; ++i) {
        const float expect = static_cast<float>(i * 5) / 32768.0f;
        ok &= std::fabs(r.out[i * 2] - expect) < 1e-6f;
    }
    CHECK(ok);
    CHECK(state(v).SamplesPlayed == 240);
    CHECK_HR(call<int32_t>(v, SetFreqRatio, 2.0f, 0u), kSOk);
    r.pass();
    CHECK(state(v).SamplesPlayed == 720);
    float ratio = 0;
    call<void>(v, GetFreqRatio, &ratio);
    CHECK(ratio == 2.0f);
    CHECK_HR(call<int32_t>(v, SetFreqRatio, 8.0f, 0u), kSOk);
    call<void>(v, GetFreqRatio, &ratio);
    CHECK(ratio == 2.0f);
    CHECK_HR(call<int32_t>(v, SetSourceRate, 48000u), kXAudio2EInvalidCall);
    CHECK_HR(call<int32_t>(v, Flush), kSOk);
    CHECK_HR(call<int32_t>(v, Stop, 0u, 0u), kSOk);
    CHECK_HR(call<int32_t>(v, Flush), kSOk);
    r.pass();
    CHECK(state(v).BuffersQueued == 0);
    CHECK_HR(call<int32_t>(v, SetSourceRate, 48000u), kSOk);
    XAudio2VoiceDetails d{};
    call<void>(v, Details, &d);
    CHECK(d.InputSampleRate == 48000);
    call<void>(v, Destroy);
}

void test_flush_and_discontinuity() {
    Rig r;
    FakeCallback cb{kCbVtbl, {}};
    const uint64_t v = source(r, pcm(1, 48000, 16), 0, 2.0f, &cb);
    std::vector<int16_t> d(3000, 1);
    for (int i = 0; i < 3; ++i) {
        const XAudio2Buffer b = buffer(&d[i * 1000], 2000, 1 + i);
        CHECK_HR(call<int32_t>(v, Submit, &b, static_cast<const void*>(nullptr)), kSOk);
    }
    CHECK_HR(call<int32_t>(v, Start, 0u, 0u), kSOk);
    r.pass();
    CHECK_HR(call<int32_t>(v, Flush), kSOk);
    CHECK(state(v).BuffersQueued == 3);
    r.pass();
    CHECK(state(v).BuffersQueued == 1);
    int ends2 = 0, ends3 = 0;
    for (const Event& e : cb.events) {
        ends2 += e.what == "BufferEnd" && e.arg == 2;
        ends3 += e.what == "BufferEnd" && e.arg == 3;
    }
    CHECK(ends2 == 1 && ends3 == 1);
    const XAudio2Buffer b4 = buffer(d.data(), 2000, 4);
    CHECK_HR(call<int32_t>(v, Submit, &b4, static_cast<const void*>(nullptr)), kSOk);
    CHECK_HR(call<int32_t>(v, Discontinuity), kSOk);
    r.pass(5);
    int streams = 0;
    for (const Event& e : cb.events) streams += e.what == "StreamEnd";
    CHECK(streams == 1);
    CHECK(state(v).SamplesPlayed == 0);
    call<void>(v, Destroy);
}

void test_validation() {
    uint64_t x = 0;
    CHECK_HR(XAudio2Create(nullptr, 0, 1), kEInvalidArg);
    CHECK_HR(XAudio2Create(&x, 0, 1), kSOk);
    const WaveFormatEx f = pcm(2, 48000, 16);
    uint64_t v = 1;
    CHECK_HR((call<int32_t>(x, CreateSource, &v, &f, 0u, 2.0f, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr))),
             kXAudio2EInvalidCall);
    CHECK(v == 0);
    uint64_t m = 0, m2 = 0;
    CHECK_HR((call<int32_t>(x, CreateMaster, &m, 2u, 48000u, 0u, static_cast<const char16_t*>(nullptr),
                            static_cast<const void*>(nullptr), 6u)),
             kSOk);
    CHECK_HR((call<int32_t>(x, CreateMaster, &m2, 2u, 48000u, 0u, static_cast<const char16_t*>(nullptr),
                            static_cast<const void*>(nullptr), 6u)),
             kXAudio2EInvalidCall);
    WaveFormatEx adpcm = pcm(2, 48000, 4, kWaveFormatAdpcm);
    CHECK_HR((call<int32_t>(x, CreateSource, &v, &adpcm, 0u, 2.0f, static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr), static_cast<void*>(nullptr))),
             kENotImpl);
    WaveFormatEx xma = pcm(2, 48000, 16, kWaveFormatXma2);
    CHECK_HR((call<int32_t>(x, CreateSource, &v, &xma, 0u, 2.0f, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr))),
             kENotImpl);
    WaveFormatEx badalign = f;
    badalign.nBlockAlign = 3;
    CHECK_HR((call<int32_t>(x, CreateSource, &v, &badalign, 0u, 2.0f, static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr), static_cast<void*>(nullptr))),
             kEInvalidArg);
    CHECK_HR((call<int32_t>(x, CreateSource, &v, &f, kXa2VoiceUseFilter, 2.0f, static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr), static_cast<void*>(nullptr))),
             kENotImpl);
    XAudio2EffectDescriptor fx{0x1234, 1, 2};
    XAudio2EffectChain chain{1, reinterpret_cast<uint64_t>(&fx)};
    CHECK_HR((call<int32_t>(x, CreateSource, &v, &f, 0u, 2.0f, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                            &chain)),
             kENotImpl);
    CHECK_HR((call<int32_t>(x, CreateSource, &v, &f, 0u, 2.0f, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr))),
             kSOk);
    std::vector<int16_t> d(4000);
    XAudio2Buffer odd = buffer(d.data(), 6);
    CHECK_HR(call<int32_t>(v, Submit, &odd, static_cast<const void*>(nullptr)), kXAudio2EInvalidCall);
    XAudio2Buffer zero = buffer(d.data(), 0);
    CHECK_HR(call<int32_t>(v, Submit, &zero, static_cast<const void*>(nullptr)), kXAudio2EInvalidCall);
    XAudio2BufferWma wma{0, 0};
    XAudio2Buffer ok = buffer(d.data(), 8);
    CHECK_HR(call<int32_t>(v, Submit, &ok, &wma), kENotImpl);
    for (uint32_t i = 0; i < kXa2MaxQueuedBuffers; ++i)
        CHECK_HR(call<int32_t>(v, Submit, &ok, static_cast<const void*>(nullptr)), kSOk);
    CHECK_HR(call<int32_t>(v, Submit, &ok, static_cast<const void*>(nullptr)), kXAudio2EInvalidCall);
    CHECK_HR(call<int32_t>(v, Start, 1u, 0u), kXAudio2EInvalidCall);
    CHECK_HR(call<int32_t>(v, SetEffectChain, &chain), kENotImpl);
    CHECK_HR(call<int32_t>(v, SetEffectChain, static_cast<const void*>(nullptr)), kSOk);
    XAudio2FilterParameters fp{0, 0.5f, 1.0f};
    CHECK_HR(call<int32_t>(v, SetFilter, &fp, 0u), kXAudio2EInvalidCall);
    // Deferred operation sets.
    CHECK_HR(call<int32_t>(v, SetVolume, 0.25f, 5u), kSOk);
    float vol = 0;
    call<void>(v, GetVolume, &vol);
    CHECK(vol == 1.0f);
    CHECK_HR(call<int32_t>(x, Commit, 4u), kSOk);
    call<void>(v, GetVolume, &vol);
    CHECK(vol == 1.0f);
    CHECK_HR(call<int32_t>(x, Commit, 5u), kSOk);
    call<void>(v, GetVolume, &vol);
    CHECK(vol == 0.25f);
    // QueryInterface.
    uint64_t ext = 0, unk = 0;
    CHECK_HR(call<int32_t>(x, QI, &kIidIXAudio2Extension, &ext), kSOk);
    uint32_t num = 0, den = 0;
    call<void>(ext, 3, &num, &den);
    CHECK(num == 480 && den == 48000);
    CHECK(call<uint32_t>(ext, 2) == 1);
    const Guid other{1, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}};
    CHECK_HR(call<int32_t>(x, QI, &other, &unk), kENoInterface);
    CHECK_HR(call<int32_t>(x, QI, &kIidIXAudio2, &unk), kSOk);
    CHECK(unk == x && call<uint32_t>(x, Release) == 1);
    XAudio2PerformanceData pd{};
    call<void>(x, PerfData, &pd);
    CHECK(pd.TotalSourceVoiceCount == 1 && pd.ActiveSourceVoiceCount == 0);
    call<void>(v, Destroy);
    call<void>(m, Destroy);
    CHECK(call<uint32_t>(x, Release) == 0);
}

void test_submix_and_formats() {
    Rig r;
    uint64_t sub = 0;
    CHECK_HR((call<int32_t>(r.x, CreateSubmix, &sub, 2u, 48000u, 0u, 0u, static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr))),
             kSOk);
    uint64_t bad = 0;
    CHECK_HR((call<int32_t>(r.x, CreateSubmix, &bad, 2u, 44100u, 0u, 0u, static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr))),
             kENotImpl);
    XAudio2SendDescriptor sd{0, sub};
    XAudio2VoiceSends sends{1, reinterpret_cast<uint64_t>(&sd)};
    const uint64_t mono = source(r, pcm(1, 48000, 16), 0, 2.0f, nullptr, &sends);
    std::vector<int16_t> d(480, 16384);
    const XAudio2Buffer b = buffer(d.data(), 960);
    CHECK_HR(call<int32_t>(mono, Submit, &b, static_cast<const void*>(nullptr)), kSOk);
    CHECK_HR(call<int32_t>(mono, Start, 0u, 0u), kSOk);
    CHECK_HR(call<int32_t>(sub, SetVolume, 0.5f, 0u), kSOk);
    r.pass();
    CHECK(r.out[0] == 0.25f && r.out[1] == 0.25f && r.out[958] == 0.25f && r.out[959] == 0.25f);
    float mtx[2] = {};
    call<void>(mono, GetOutputMatrix, sub, 1u, 2u, mtx);
    CHECK(mtx[0] == 1.0f && mtx[1] == 1.0f);
    call<void>(sub, Destroy);
    CHECK(r.e->lookup(sub) != nullptr);
    call<void>(mono, Destroy);
    call<void>(sub, Destroy);
    CHECK(r.e->lookup(sub) == nullptr);

    struct Case {
        WaveFormatEx f;
        std::vector<uint8_t> bytes;
        float expect0, expect1;
    };
    std::vector<Case> cases;
    cases.push_back({pcm(1, 48000, 8), {0xC0, 0x40}, 0.5f, -0.5f});
    cases.push_back({pcm(1, 48000, 24), {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0}, 0.5f, -0.5f});
    cases.push_back({pcm(1, 48000, 32), {0, 0, 0, 0x40, 0, 0, 0, 0xC0}, 0.5f, -0.5f});
    {
        std::vector<uint8_t> fb(8);
        const float a = 0.75f, c = -0.25f;
        std::memcpy(fb.data(), &a, 4);
        std::memcpy(fb.data() + 4, &c, 4);
        cases.push_back({pcm(1, 48000, 32, kWaveFormatIeeeFloat), fb, 0.75f, -0.25f});
    }
    for (Case& c : cases) {
        const uint64_t v = source(r, c.f);
        const XAudio2Buffer cb = buffer(c.bytes.data(), static_cast<uint32_t>(c.bytes.size()));
        CHECK_HR(call<int32_t>(v, Submit, &cb, static_cast<const void*>(nullptr)), kSOk);
        CHECK_HR(call<int32_t>(v, Start, 0u, 0u), kSOk);
        r.pass();
        CHECK(r.out[0] == c.expect0 && r.out[2] == c.expect1 && r.out[4] == 0.0f);
        call<void>(v, Destroy);
    }
    WaveFormatExtensible x{};
    x.Format = pcm(2, 48000, 16, kWaveFormatExtensible);
    x.Format.cbSize = 22;
    x.wValidBitsPerSample = 16;
    x.dwChannelMask = 3;
    x.SubFormat = kSubtypePcm;
    uint64_t xv = 0;
    CHECK_HR((call<int32_t>(r.x, CreateSource, &xv, &x, 0u, 2.0f, static_cast<void*>(nullptr),
                            static_cast<void*>(nullptr), static_cast<void*>(nullptr))),
             kSOk);
    call<void>(xv, Destroy);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    set_guest_invoker(&direct_guest_invoker);
    rt::xa2::set_output_mode_override(static_cast<int>(rt::xa2::Engine::OutputMode::Manual));
    test_bink_sequence();
    test_callbacks();
    test_loops_and_exit();
    test_resample_and_ratio();
    test_flush_and_discontinuity();
    test_validation();
    test_submix_and_formats();
    return mt::finish("media_xaudio2_test");
}
