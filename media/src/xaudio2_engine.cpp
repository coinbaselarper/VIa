#include "xaudio2_engine.h"

#include "media_common.h"

#include <pthread.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rt::xa2 {

using namespace rt::media;

namespace {

std::atomic<int> g_mode_override{-1};
std::atomic<bool> g_logged_mode{false}, g_logged_device{false}, g_logged_effects{false}, g_logged_filter{false},
    g_logged_destroy_cb{false}, g_logged_wma{false};

uint64_t self_marker() { return reinterpret_cast<uint64_t>(pthread_self()); }

const char* format_name(uint16_t tag) {
    switch (tag) {
    case kWaveFormatAdpcm: return "ADPCM";
    case kWaveFormatWmaudio2: return "xWMA (WMAUDIO2)";
    case kWaveFormatWmaudio3: return "xWMA (WMAUDIO3)";
    case kWaveFormatXma2: return "XMA2";
    default: return "unknown";
    }
}

uint32_t default_mask(uint32_t channels) {
    switch (channels) {
    case 1: return 0x4;
    case 2: return 0x3;
    case 4: return 0x33;
    case 6: return 0x3F;
    case 8: return 0x63F;
    default: return 0;
    }
}

}  // namespace

Engine::OutputMode default_output_mode() {
    const int o = g_mode_override.load();
    if (o >= 0) return static_cast<Engine::OutputMode>(o);
    const char* v = std::getenv("GTA6RT_XAUDIO2_OUTPUT");
    if (v && std::strcmp(v, "silent") == 0) return Engine::OutputMode::Silent;
    if (v && *v && std::strcmp(v, "coreaudio") != 0)
        MLOG_ONCE(g_logged_mode, WARN, "xaudio2", "GTA6RT_XAUDIO2_OUTPUT=%s not understood; using coreaudio", v);
    return Engine::OutputMode::CoreAudio;
}

void set_output_mode_override(int mode) { g_mode_override.store(mode); }

Engine::Engine(uint32_t flags, uint32_t processor, OutputMode mode) : flags_(flags), mode_(mode) {
    (void)processor;
    if (flags & kXa21024Quantum) quantum_ = 1024;
}

Engine::~Engine() {
    stop_thread();
    std::lock_guard lock(mu_);
    for (Voice* v : voices_) delete v;
    voices_.clear();
    live_.clear();
    master_ = nullptr;
}

bool Engine::on_engine_thread() const { return engine_thread_marker_.load() == self_marker(); }

Voice* Engine::lookup(uint64_t guest_voice) {
    if (!guest_voice) return nullptr;
    Voice* v = reinterpret_cast<GuestVoice*>(guest_voice)->voice;
    std::lock_guard lock(mu_);
    return live_.count(v) ? v : nullptr;
}

void Engine::defer_or_run(uint32_t op, std::function<void()> fn) {
    if (op == kXa2CommitNow) {
        fn();
        return;
    }
    deferred_.push_back({op, std::move(fn)});
}

HRESULT Engine::commit(uint32_t op) {
    std::lock_guard lock(mu_);
    std::vector<Deferred> keep;
    for (auto& d : deferred_) {
        if (op == kXa2CommitAll || d.op == op) d.fn();
        else keep.push_back(std::move(d));
    }
    deferred_.swap(keep);
    return kSOk;
}

void Engine::default_matrix(uint32_t src, uint32_t dst, uint32_t dst_mask, std::vector<float>* m) const {
    m->assign(static_cast<size_t>(src) * dst, 0.0f);
    auto at = [&](uint32_t d, uint32_t s) -> float& { return (*m)[static_cast<size_t>(d) * src + s]; };
    if (src == dst) {
        for (uint32_t i = 0; i < src; ++i) at(i, i) = 1.0f;
        return;
    }
    if (src == 1) {
        if (dst >= 2) {
            at(0, 0) = 1.0f;
            at(1, 0) = 1.0f;
        } else {
            at(0, 0) = 1.0f;
        }
        return;
    }
    if (dst == 1) {
        for (uint32_t s = 0; s < src; ++s) at(0, s) = 1.0f / static_cast<float>(src);
        return;
    }
    if (dst == 2 && src > 2) {
        // Speaker order FL FR FC LFE BL BR (SL SR): fold centre and surrounds into the front pair.
        constexpr float k = 0.70710678f;
        at(0, 0) = 1.0f;
        at(1, 1) = 1.0f;
        if (src > 2) at(0, 2) = at(1, 2) = k;
        for (uint32_t s = 4; s < src; ++s) at((s - 4) % 2, s) = k;
        return;
    }
    (void)dst_mask;
    for (uint32_t i = 0; i < std::min(src, dst); ++i) at(i, i) = 1.0f;
}

HRESULT Engine::build_sends(Voice* v, const XAudio2VoiceSends* sends, std::vector<Send>* out) {
    out->clear();
    const uint32_t src_ch = v->channels;
    if (!sends) {
        if (!master_) return kXAudio2EInvalidCall;
        Send s;
        s.out = master_;
        default_matrix(src_ch, master_->channels, default_mask(master_->channels), &s.matrix);
        out->push_back(std::move(s));
        return kSOk;
    }
    if (sends->SendCount && !sends->pSends) return kEInvalidArg;
    const auto* desc = reinterpret_cast<const XAudio2SendDescriptor*>(sends->pSends);
    uint32_t rate = 0;
    for (uint32_t i = 0; i < sends->SendCount; ++i) {
        XAudio2SendDescriptor d;
        std::memcpy(&d, desc + i, sizeof d);
        Voice* dst = d.pOutputVoice ? reinterpret_cast<GuestVoice*>(d.pOutputVoice)->voice : nullptr;
        if (!dst || !live_.count(dst) || dst == v || dst->kind == Kind::Source) return kEInvalidArg;
        if (v->kind == Kind::Submix && dst->kind == Kind::Submix && dst->stage <= v->stage) return kXAudio2EInvalidCall;
        if (rate && dst->rate != rate) return kXAudio2EInvalidCall;
        rate = dst->rate;
        if (d.Flags & kXa2SendUseFilter) {
            MLOG_ONCE(g_logged_filter, ERROR, "xaudio2",
                      "send filters (XAUDIO2_SEND_USEFILTER) are not implemented: E_NOTIMPL");
            return kENotImpl;
        }
        for (const Send& s : *out)
            if (s.out == dst) return kEInvalidArg;
        Send s;
        s.out = dst;
        s.flags = d.Flags;
        default_matrix(src_ch, dst->channels, default_mask(dst->channels), &s.matrix);
        out->push_back(std::move(s));
    }
    return kSOk;
}

HRESULT Engine::create_mastering(uint32_t channels, uint32_t rate, uint32_t flags, uint64_t device_id,
                                 uint64_t effects, uint32_t category, Voice** out) {
    if (!out) return kEInvalidArg;
    *out = nullptr;
    if (effects && reinterpret_cast<const XAudio2EffectChain*>(effects)->EffectCount) {
        MLOG_ONCE(g_logged_effects, ERROR, "xaudio2", "effect chains (XAPO) are not implemented: E_NOTIMPL");
        return kENotImpl;
    }
    std::lock_guard lock(mu_);
    if (master_) return kXAudio2EInvalidCall;
    if (channels == 0) channels = 2;
    if (rate == 0) rate = 48000;
    if (channels > kXa2MaxAudioChannels || rate < kXa2MinSampleRate || rate > kXa2MaxSampleRate) return kEInvalidArg;
    if (device_id) MLOG_ONCE(g_logged_device, INFO, "xaudio2", "CreateMasteringVoice: device id ignored, default output");
    auto* v = new Voice;
    v->guest.voice = v;
    v->engine = this;
    v->kind = Kind::Master;
    v->creation_flags = flags;
    v->channels = channels;
    v->rate = rate;
    v->channel_volumes.assign(channels, 1.0f);
    (void)category;
    rate_ = rate;
    if (flags_ & kXa21024Quantum) quantum_ = 1024;
    else quantum_ = rate / 100;
    v->mix.assign(static_cast<size_t>(channels) * quantum_, 0.0f);
    master_ = v;
    voices_.push_back(v);
    live_.insert(v);
    out_buffer_.assign(static_cast<size_t>(channels) * quantum_, 0.0f);
    if (mode_ != OutputMode::Manual) start_thread_locked();
    *out = v;
    MLOG(INFO, "xaudio2", "mastering voice: %u ch, %u Hz, quantum %u frames, output %s", channels, rate, quantum_,
         output_ ? output_->name() : "manual");
    return kSOk;
}

HRESULT Engine::create_source(const WaveFormatEx* wfx, uint32_t flags, float max_ratio, uint64_t callback,
                              const XAudio2VoiceSends* sends, uint64_t effects, Voice** out) {
    if (!out || !wfx) return kEInvalidArg;
    *out = nullptr;
    WaveFormatEx f;
    std::memcpy(&f, wfx, sizeof f);
    SampleFormat sf;
    sf.channels = f.nChannels;
    sf.rate = f.nSamplesPerSec;
    sf.container_bits = f.wBitsPerSample;
    sf.block_align = f.nBlockAlign;
    uint16_t tag = f.wFormatTag;
    if (tag == kWaveFormatExtensible) {
        if (f.cbSize < 22) return kEInvalidArg;
        WaveFormatExtensible x;
        std::memcpy(&x, wfx, sizeof x);
        sf.channel_mask = x.dwChannelMask;
        if (x.SubFormat == kSubtypePcm) tag = kWaveFormatPcm;
        else if (x.SubFormat == kSubtypeFloat) tag = kWaveFormatIeeeFloat;
        else {
            MLOG(ERROR, "xaudio2", "CreateSourceVoice: WAVE_FORMAT_EXTENSIBLE subformat {%08X-...} not implemented: E_NOTIMPL",
                 x.SubFormat.data1);
            return kENotImpl;
        }
    }
    if (tag != kWaveFormatPcm && tag != kWaveFormatIeeeFloat) {
        MLOG(ERROR, "xaudio2", "CreateSourceVoice: format 0x%04x (%s) not implemented: E_NOTIMPL", tag, format_name(tag));
        return kENotImpl;
    }
    sf.tag = tag;
    const bool bits_ok = tag == kWaveFormatIeeeFloat ? sf.container_bits == 32
                                                      : (sf.container_bits == 8 || sf.container_bits == 16 ||
                                                         sf.container_bits == 24 || sf.container_bits == 32);
    if (!bits_ok || sf.channels == 0 || sf.channels > kXa2MaxAudioChannels || sf.rate < kXa2MinSampleRate ||
        sf.rate > kXa2MaxSampleRate || sf.block_align != sf.channels * (sf.container_bits / 8))
        return kEInvalidArg;
    if (flags & kXa2VoiceUseFilter) {
        MLOG_ONCE(g_logged_filter, ERROR, "xaudio2", "voice filters (XAUDIO2_VOICE_USEFILTER) are not implemented: E_NOTIMPL");
        return kENotImpl;
    }
    if (effects && reinterpret_cast<const XAudio2EffectChain*>(effects)->EffectCount) {
        MLOG_ONCE(g_logged_effects, ERROR, "xaudio2", "effect chains (XAPO) are not implemented: E_NOTIMPL");
        return kENotImpl;
    }
    if (!(flags & kXa2VoiceNoPitch)) {
        if (max_ratio == 0.0f) max_ratio = kXa2DefaultFreqRatio;
        if (!(max_ratio >= kXa2MinFreqRatio && max_ratio <= kXa2MaxFreqRatio)) return kEInvalidArg;
    } else {
        max_ratio = 1.0f;
    }
    std::lock_guard lock(mu_);
    if (!master_) return kXAudio2EInvalidCall;
    auto* v = new Voice;
    v->guest.voice = v;
    v->engine = this;
    v->kind = Kind::Source;
    v->creation_flags = flags;
    v->channels = sf.channels;
    v->rate = sf.rate;
    v->fmt = sf;
    v->max_ratio = max_ratio;
    v->callback = callback;
    v->channel_volumes.assign(v->channels, 1.0f);
    v->cur.assign(v->channels, 0.0f);
    v->nxt.assign(v->channels, 0.0f);
    v->scratch.assign(static_cast<size_t>(v->channels) * quantum_, 0.0f);
    const HRESULT h = build_sends(v, sends, &v->sends);
    if (h < 0) {
        delete v;
        return h;
    }
    voices_.push_back(v);
    live_.insert(v);
    ++created_sources_;
    *out = v;
    return kSOk;
}

HRESULT Engine::create_submix(uint32_t channels, uint32_t rate, uint32_t flags, uint32_t stage,
                              const XAudio2VoiceSends* sends, uint64_t effects, Voice** out) {
    if (!out) return kEInvalidArg;
    *out = nullptr;
    if (effects && reinterpret_cast<const XAudio2EffectChain*>(effects)->EffectCount) {
        MLOG_ONCE(g_logged_effects, ERROR, "xaudio2", "effect chains (XAPO) are not implemented: E_NOTIMPL");
        return kENotImpl;
    }
    if (flags & kXa2VoiceUseFilter) {
        MLOG_ONCE(g_logged_filter, ERROR, "xaudio2", "voice filters (XAUDIO2_VOICE_USEFILTER) are not implemented: E_NOTIMPL");
        return kENotImpl;
    }
    if (channels == 0 || channels > kXa2MaxAudioChannels || rate < kXa2MinSampleRate || rate > kXa2MaxSampleRate)
        return kEInvalidArg;
    std::lock_guard lock(mu_);
    if (!master_) return kXAudio2EInvalidCall;
    if (rate != master_->rate) {
        MLOG(ERROR, "xaudio2",
             "CreateSubmixVoice: input rate %u differs from the mastering rate %u; submix sample-rate conversion is not "
             "implemented: E_NOTIMPL",
             rate, master_->rate);
        return kENotImpl;
    }
    auto* v = new Voice;
    v->guest.voice = v;
    v->engine = this;
    v->kind = Kind::Submix;
    v->creation_flags = flags;
    v->channels = channels;
    v->rate = rate;
    v->stage = stage;
    v->channel_volumes.assign(channels, 1.0f);
    v->mix.assign(static_cast<size_t>(channels) * quantum_, 0.0f);
    const HRESULT h = build_sends(v, sends, &v->sends);
    if (h < 0) {
        delete v;
        return h;
    }
    voices_.push_back(v);
    live_.insert(v);
    *out = v;
    return kSOk;
}

void Engine::destroy_voice(Voice* v) {
    if (on_engine_thread()) {
        MLOG_ONCE(g_logged_destroy_cb, ERROR, "xaudio2",
                  "DestroyVoice called from an XAudio2 callback (not allowed); the voice is kept");
        return;
    }
    std::unique_lock lock(mu_);
    if (!live_.count(v)) return;
    for (Voice* o : voices_)
        for (const Send& s : o->sends)
            if (s.out == v && o != v) {
                MLOG(ERROR, "xaudio2", "DestroyVoice: another voice still sends to this voice; not destroyed");
                return;
            }
    pass_cv_.wait(lock, [&] { return !in_pass_; });
    live_.erase(v);
    voices_.erase(std::remove(voices_.begin(), voices_.end(), v), voices_.end());
    const bool was_master = v == master_;
    if (was_master) master_ = nullptr;
    lock.unlock();
    if (was_master) stop_thread();
    delete v;
}

HRESULT Engine::register_callback(uint64_t cb) {
    if (!cb) return kEInvalidArg;
    std::lock_guard lock(mu_);
    if (std::find(engine_callbacks_.begin(), engine_callbacks_.end(), cb) == engine_callbacks_.end())
        engine_callbacks_.push_back(cb);
    return kSOk;
}

void Engine::unregister_callback(uint64_t cb) {
    std::lock_guard lock(mu_);
    engine_callbacks_.erase(std::remove(engine_callbacks_.begin(), engine_callbacks_.end(), cb), engine_callbacks_.end());
}

HRESULT Engine::start_engine() {
    std::lock_guard lock(mu_);
    engine_running_ = true;
    return kSOk;
}

void Engine::stop_engine() {
    std::lock_guard lock(mu_);
    engine_running_ = false;
}

void Engine::performance(XAudio2PerformanceData* out) {
    std::lock_guard lock(mu_);
    std::memset(out, 0, sizeof *out);
    const uint64_t passes = stats_.passes - stats_query_passes_;
    const uint64_t busy = stats_.pass_ns_total - stats_query_ns_;
    const uint64_t now = now_ns();
    out->AudioCyclesSinceLastQuery = busy;
    out->TotalCyclesSinceLastQuery = stats_query_wall_ ? now - stats_query_wall_ : busy;
    out->MinimumCyclesPerQuantum = passes ? static_cast<uint32_t>(std::min<uint64_t>(stats_.pass_ns_min, UINT32_MAX)) : 0;
    out->MaximumCyclesPerQuantum = static_cast<uint32_t>(std::min<uint64_t>(stats_.pass_ns_max, UINT32_MAX));
    stats_query_passes_ = stats_.passes;
    stats_query_ns_ = stats_.pass_ns_total;
    stats_query_wall_ = now;
    out->CurrentLatencyInSamples = output_ ? output_->latency_frames() + quantum_ : quantum_;
    out->GlitchesSinceEngineStarted = output_ ? output_->glitches() : 0;
    for (Voice* v : voices_) {
        if (v->kind == Kind::Source) {
            ++out->TotalSourceVoiceCount;
            if (v->running) ++out->ActiveSourceVoiceCount;
            if (v->running && (v->rate != rate_ || v->ratio != 1.0f)) ++out->ActiveResamplerCount;
        } else if (v->kind == Kind::Submix) {
            ++out->ActiveSubmixVoiceCount;
        }
        out->ActiveMatrixMixCount += static_cast<uint32_t>(v->sends.size());
        out->MemoryUsageInBytes += static_cast<uint32_t>(sizeof(Voice) + v->mix.size() * 4 + v->scratch.size() * 4);
    }
}

void Engine::details(Voice* v, XAudio2VoiceDetails* out) {
    std::lock_guard lock(mu_);
    out->CreationFlags = v->creation_flags;
    out->ActiveFlags = v->creation_flags;
    out->InputChannels = v->channels;
    out->InputSampleRate = v->rate;
}

HRESULT Engine::set_output_voices(Voice* v, const XAudio2VoiceSends* sends) {
    if (v->kind == Kind::Master) return kXAudio2EInvalidCall;
    std::unique_lock lock(mu_);
    std::vector<Send> s;
    const HRESULT h = build_sends(v, sends, &s);
    if (h < 0) return h;
    v->sends.swap(s);
    return kSOk;
}

HRESULT Engine::set_volume(Voice* v, float volume, uint32_t op) {
    if (!(std::fabs(volume) <= kXa2MaxVolumeLevel)) return kEInvalidArg;
    std::lock_guard lock(mu_);
    defer_or_run(op, [v, volume] { v->volume = volume; });
    return kSOk;
}

void Engine::get_volume(Voice* v, float* out) {
    std::lock_guard lock(mu_);
    *out = v->volume;
}

HRESULT Engine::set_channel_volumes(Voice* v, uint32_t count, const float* volumes, uint32_t op) {
    if (!volumes) return kEInvalidArg;
    std::lock_guard lock(mu_);
    if (count != v->channels) return kEInvalidArg;
    std::vector<float> vol(volumes, volumes + count);
    for (float x : vol)
        if (!(std::fabs(x) <= kXa2MaxVolumeLevel)) return kEInvalidArg;
    defer_or_run(op, [v, vol = std::move(vol)] { v->channel_volumes = vol; });
    return kSOk;
}

HRESULT Engine::get_channel_volumes(Voice* v, uint32_t count, float* out) {
    std::lock_guard lock(mu_);
    if (!out || count != v->channels) return kEInvalidArg;
    std::copy(v->channel_volumes.begin(), v->channel_volumes.end(), out);
    return kSOk;
}

HRESULT Engine::set_output_matrix(Voice* v, uint64_t dest, uint32_t src_ch, uint32_t dst_ch, const float* m,
                                  uint32_t op) {
    if (!m) return kEInvalidArg;
    std::lock_guard lock(mu_);
    Send* s = nullptr;
    if (!dest) {
        if (v->sends.size() != 1) return kXAudio2EInvalidCall;
        s = &v->sends[0];
    } else {
        Voice* d = reinterpret_cast<GuestVoice*>(dest)->voice;
        for (Send& x : v->sends)
            if (x.out == d) s = &x;
        if (!s) return kXAudio2EInvalidCall;
    }
    if (src_ch != v->channels || dst_ch != s->out->channels) {
        MLOG(WARN, "xaudio2", "SetOutputMatrix: %ux%u does not match the voices (%ux%u): XAUDIO2_E_INVALID_CALL", src_ch,
             dst_ch, v->channels, s->out->channels);
        return kXAudio2EInvalidCall;
    }
    std::vector<float> mat(m, m + static_cast<size_t>(src_ch) * dst_ch);
    Voice* target = s->out;
    defer_or_run(op, [v, target, mat = std::move(mat)] {
        for (Send& x : v->sends)
            if (x.out == target) x.matrix = mat;
    });
    return kSOk;
}

HRESULT Engine::get_output_matrix(Voice* v, uint64_t dest, uint32_t src_ch, uint32_t dst_ch, float* m) {
    std::lock_guard lock(mu_);
    if (!m) return kEInvalidArg;
    const Send* s = nullptr;
    if (!dest) {
        if (v->sends.size() == 1) s = &v->sends[0];
    } else {
        Voice* d = reinterpret_cast<GuestVoice*>(dest)->voice;
        for (const Send& x : v->sends)
            if (x.out == d) s = &x;
    }
    if (!s || src_ch != v->channels || dst_ch != s->out->channels) return kXAudio2EInvalidCall;
    std::copy(s->matrix.begin(), s->matrix.end(), m);
    return kSOk;
}

HRESULT Engine::start(Voice* v, uint32_t flags, uint32_t op) {
    if (v->kind != Kind::Source) return kXAudio2EInvalidCall;
    if (flags != 0) return kXAudio2EInvalidCall;
    std::lock_guard lock(mu_);
    defer_or_run(op, [v] { v->running = true; });
    return kSOk;
}

HRESULT Engine::stop(Voice* v, uint32_t flags, uint32_t op) {
    if (v->kind != Kind::Source) return kXAudio2EInvalidCall;
    if (flags & ~kXa2PlayTails) return kXAudio2EInvalidCall;
    std::lock_guard lock(mu_);
    defer_or_run(op, [v] { v->running = false; });
    return kSOk;
}

HRESULT Engine::submit(Voice* v, const XAudio2Buffer* buf, const XAudio2BufferWma* wma) {
    if (v->kind != Kind::Source) return kXAudio2EInvalidCall;
    if (!buf) return kEInvalidArg;
    if (wma) {
        MLOG_ONCE(g_logged_wma, ERROR, "xaudio2", "SubmitSourceBuffer with XAUDIO2_BUFFER_WMA is not implemented: E_NOTIMPL");
        return kENotImpl;
    }
    QueuedBuffer q;
    std::memcpy(&q.desc, buf, sizeof q.desc);
    const XAudio2Buffer& b = q.desc;
    const uint32_t align = v->fmt.block_align;
    if (b.Flags & ~kXa2EndOfStream) return kXAudio2EInvalidCall;
    if (b.AudioBytes == 0 || b.AudioBytes > kXa2MaxBufferBytes || b.AudioBytes % align || !b.pAudioData)
        return kXAudio2EInvalidCall;
    const uint32_t frames = b.AudioBytes / align;
    const uint32_t play_begin = b.PlayBegin;
    const uint32_t play_end = b.PlayLength ? b.PlayBegin + b.PlayLength : frames;
    if (play_begin >= frames || play_end > frames || play_end <= play_begin) return kXAudio2EInvalidCall;
    if (b.LoopCount == 0) {
        if (b.LoopBegin || b.LoopLength) return kXAudio2EInvalidCall;
    } else {
        if (b.LoopCount > kXa2MaxLoopCount && b.LoopCount != kXa2LoopInfinite) return kXAudio2EInvalidCall;
        q.loop_begin = b.LoopBegin;
        q.loop_end = b.LoopLength ? b.LoopBegin + b.LoopLength : play_end;
        if (q.loop_begin < play_begin || q.loop_end > play_end || q.loop_end <= q.loop_begin) return kXAudio2EInvalidCall;
        q.loops_left = b.LoopCount;
    }
    q.data = reinterpret_cast<const uint8_t*>(b.pAudioData);
    q.play_end = play_end;
    q.pos = play_begin;
    std::lock_guard lock(mu_);
    if (v->queue.size() >= kXa2MaxQueuedBuffers) return kXAudio2EInvalidCall;
    v->queue.push_back(q);
    return kSOk;
}

HRESULT Engine::flush(Voice* v) {
    if (v->kind != Kind::Source) return kXAudio2EInvalidCall;
    std::lock_guard lock(mu_);
    const size_t keep = (v->running && !v->queue.empty() && v->queue.front().started) ? 1 : 0;
    if (keep == 0) {
        v->deferred_events.insert(v->deferred_events.end(), v->cur_ev.begin(), v->cur_ev.end());
        v->deferred_events.insert(v->deferred_events.end(), v->nxt_ev.begin(), v->nxt_ev.end());
        v->cur_ev.clear();
        v->nxt_ev.clear();
        v->primed = false;
        v->cur_real = v->nxt_real = false;
    }
    for (size_t i = keep; i < v->queue.size(); ++i) {
        v->finished_ctx.push_back(v->queue[i].desc.pContext);
        v->deferred_events.push_back({VoiceEvent::BufferEnd, v->queue[i].desc.pContext});
    }
    v->queue.erase(v->queue.begin() + static_cast<long>(keep), v->queue.end());
    return kSOk;
}

HRESULT Engine::discontinuity(Voice* v) {
    if (v->kind != Kind::Source) return kXAudio2EInvalidCall;
    std::lock_guard lock(mu_);
    if (!v->queue.empty()) v->queue.back().desc.Flags |= kXa2EndOfStream;
    return kSOk;
}

HRESULT Engine::exit_loop(Voice* v, uint32_t op) {
    if (v->kind != Kind::Source) return kXAudio2EInvalidCall;
    std::lock_guard lock(mu_);
    defer_or_run(op, [v] {
        if (!v->queue.empty()) v->queue.front().loops_left = 0;
    });
    return kSOk;
}

void Engine::get_state(Voice* v, XAudio2VoiceState* out, uint32_t flags) {
    std::lock_guard lock(mu_);
    XAudio2VoiceState s{};
    s.BuffersQueued = static_cast<uint32_t>(v->queue.size() + v->finished_ctx.size());
    s.pCurrentBufferContext = !v->finished_ctx.empty() ? v->finished_ctx.front()
                              : v->queue.empty()       ? 0
                                                       : v->queue.front().desc.pContext;
    s.SamplesPlayed = (flags & kXa2VoiceNoSamplesPlayed) ? 0 : v->samples_played;
    std::memcpy(out, &s, sizeof s);
}

HRESULT Engine::set_frequency_ratio(Voice* v, float ratio, uint32_t op) {
    if (v->kind != Kind::Source || (v->creation_flags & kXa2VoiceNoPitch)) return kXAudio2EInvalidCall;
    if (!(ratio >= 0.0f)) return kEInvalidArg;
    const float r = std::clamp(ratio, kXa2MinFreqRatio, v->max_ratio);
    std::lock_guard lock(mu_);
    defer_or_run(op, [v, r] { v->ratio = r; });
    return kSOk;
}

void Engine::get_frequency_ratio(Voice* v, float* out) {
    std::lock_guard lock(mu_);
    *out = v->ratio;
}

HRESULT Engine::set_source_sample_rate(Voice* v, uint32_t rate) {
    if (v->kind != Kind::Source) return kXAudio2EInvalidCall;
    if (rate < kXa2MinSampleRate || rate > kXa2MaxSampleRate) return kEInvalidArg;
    std::lock_guard lock(mu_);
    if (!v->queue.empty() || !v->finished_ctx.empty()) return kXAudio2EInvalidCall;
    v->rate = rate;
    v->fmt.rate = rate;
    return kSOk;
}

HRESULT Engine::get_channel_mask(Voice* v, uint32_t* mask) {
    if (!mask) return kEInvalidArg;
    std::lock_guard lock(mu_);
    *mask = default_mask(v->channels);
    return kSOk;
}

EngineStats Engine::stats() {
    std::lock_guard lock(mu_);
    return stats_;
}

// ---- processing ----

void Engine::finish_buffer(Voice* v, std::vector<Voice::Pending>* attach) {
    const QueuedBuffer b = v->queue.front();
    v->queue.pop_front();
    v->finished_ctx.push_back(b.desc.pContext);
    attach->push_back({VoiceEvent::BufferEnd, b.desc.pContext});
    if (b.desc.Flags & kXa2EndOfStream) attach->push_back({VoiceEvent::StreamEnd, 0});
}

void Engine::release(Voice* v, std::vector<Voice::Pending>* evs, std::vector<VoiceEvent>* out) {
    for (const Voice::Pending& e : *evs) {
        if (e.type == VoiceEvent::BufferEnd && !v->finished_ctx.empty()) v->finished_ctx.pop_front();
        if (e.type == VoiceEvent::StreamEnd) v->samples_played = 0;
        if (v->callback) out->push_back({static_cast<VoiceEvent::Type>(e.type), v->callback, e.context, 0});
    }
    evs->clear();
}

bool Engine::fetch_frame(Voice* v, float* frame, std::vector<Voice::Pending>* attach, std::vector<VoiceEvent>* events) {
    while (!v->queue.empty()) {
        QueuedBuffer& b = v->queue.front();
        if (!b.started) {
            b.started = true;
            if (v->callback) events->push_back({VoiceEvent::BufferStart, v->callback, b.desc.pContext, 0});
        }
        const SampleFormat& f = v->fmt;
        const uint8_t* p = b.data + static_cast<size_t>(b.pos) * f.block_align;
        for (uint32_t c = 0; c < f.channels; ++c) {
            float s;
            if (f.tag == kWaveFormatIeeeFloat) {
                std::memcpy(&s, p + c * 4, 4);
            } else {
                switch (f.container_bits) {
                case 8: s = (static_cast<float>(p[c]) - 128.0f) * (1.0f / 128.0f); break;
                case 16: {
                    int16_t x;
                    std::memcpy(&x, p + c * 2, 2);
                    s = static_cast<float>(x) * (1.0f / 32768.0f);
                    break;
                }
                case 24: {
                    const uint8_t* q = p + c * 3;
                    const int32_t x = static_cast<int32_t>((static_cast<uint32_t>(q[0]) << 8) |
                                                           (static_cast<uint32_t>(q[1]) << 16) |
                                                           (static_cast<uint32_t>(q[2]) << 24)) >>
                                      8;
                    s = static_cast<float>(x) * (1.0f / 8388608.0f);
                    break;
                }
                default: {
                    int32_t x;
                    std::memcpy(&x, p + c * 4, 4);
                    s = static_cast<float>(x) * (1.0f / 2147483648.0f);
                    break;
                }
                }
            }
            frame[c] = s;
        }
        ++b.pos;
        if (b.loops_left > 0 && b.pos == b.loop_end) {
            if (b.loops_left != kXa2LoopInfinite) --b.loops_left;
            b.pos = b.loop_begin;
            attach->push_back({VoiceEvent::LoopEnd, b.desc.pContext});
        } else if (b.pos >= b.play_end) {
            finish_buffer(v, attach);
        }
        return true;
    }
    return false;
}

uint32_t Engine::bytes_required(Voice* v) const {
    const double step = static_cast<double>(v->rate) * v->ratio / rate_;
    const uint64_t need = static_cast<uint64_t>(std::ceil(quantum_ * step + v->frac)) + (v->primed ? 0 : 2);
    uint64_t have = v->primed ? (v->cur_real ? 1 : 0) + (v->nxt_real ? 1 : 0) : 0;
    for (const QueuedBuffer& b : v->queue) {
        if (b.loops_left == kXa2LoopInfinite) return 0;
        have += (b.play_end - b.pos) + static_cast<uint64_t>(b.loops_left) * (b.loop_end - b.loop_begin);
        if (have >= need) return 0;
    }
    return static_cast<uint32_t>((need - have) * v->fmt.block_align);
}

void Engine::render_source(Voice* v, std::vector<VoiceEvent>* events) {
    const uint32_t ch = v->channels;
    float* out = v->scratch.data();
    const double step = static_cast<double>(v->rate) * v->ratio / rate_;
    auto pull_next = [&] {
        v->nxt_real = fetch_frame(v, v->nxt.data(), &v->nxt_ev, events);
        if (!v->nxt_real) std::fill(v->nxt.begin(), v->nxt.end(), 0.0f);
    };
    if (!v->primed) {
        v->cur_real = fetch_frame(v, v->cur.data(), &v->cur_ev, events);
        if (!v->cur_real) std::fill(v->cur.begin(), v->cur.end(), 0.0f);
        pull_next();
        v->primed = v->cur_real;
        v->frac = 0.0;
    }
    for (uint32_t i = 0; i < quantum_; ++i) {
        const float t = static_cast<float>(v->frac);
        for (uint32_t c = 0; c < ch; ++c) out[i * ch + c] = v->cur[c] + (v->nxt[c] - v->cur[c]) * t;
        v->frac += step;
        while (v->frac >= 1.0) {
            v->frac -= 1.0;
            if (v->cur_real) ++v->samples_played;
            release(v, &v->cur_ev, events);
            v->cur.swap(v->nxt);
            v->cur_ev.swap(v->nxt_ev);
            v->cur_real = v->nxt_real;
            pull_next();
        }
    }
    if (!v->cur_real && !v->nxt_real) v->primed = false;
}

void Engine::route(Voice* v, const float* frames) {
    const uint32_t ch = v->channels;
    for (Send& s : v->sends) {
        const uint32_t dch = s.out->channels;
        float* dst = s.out->mix.data();
        for (uint32_t i = 0; i < quantum_; ++i) {
            const float* in = frames + static_cast<size_t>(i) * ch;
            float* o = dst + static_cast<size_t>(i) * dch;
            for (uint32_t d = 0; d < dch; ++d) {
                const float* row = &s.matrix[static_cast<size_t>(d) * ch];
                float acc = 0.0f;
                for (uint32_t c = 0; c < ch; ++c) acc += row[c] * in[c];
                o[d] += acc;
            }
        }
    }
}

void Engine::run_pass(float* out) {
    const uint64_t t0 = now_ns();
    std::vector<uint64_t> engine_cbs;
    struct Active {
        Voice* v;
        uint64_t cb;
        uint32_t bytes;
    };
    std::vector<Active> active;
    std::vector<std::pair<uint64_t, Voice::Pending>> early;
    {
        std::unique_lock lock(mu_);
        pass_cv_.wait(lock, [&] { return !in_pass_; });
        in_pass_ = true;
        engine_cbs = engine_callbacks_;
        if (engine_running_)
            for (Voice* v : voices_) {
                if (v->kind != Kind::Source) continue;
                for (const auto& e : v->deferred_events) {
                    if (e.type == VoiceEvent::BufferEnd && !v->finished_ctx.empty()) v->finished_ctx.pop_front();
                    if (e.type == VoiceEvent::StreamEnd) v->samples_played = 0;
                    if (v->callback) early.push_back({v->callback, e});
                }
                v->deferred_events.clear();
                if (v->running) active.push_back({v, v->callback, v->callback ? bytes_required(v) : 0});
            }
    }
    uint64_t ncb = 0;
    for (uint64_t cb : engine_cbs) {
        guest_method(cb, 0);
        ++ncb;
    }
    for (const auto& [cb, e] : early) {
        const uint64_t a[1] = {e.context};
        guest_method(cb, static_cast<uint32_t>(e.type), a, e.type == VoiceEvent::StreamEnd ? 0 : 1);
        ++ncb;
    }
    for (const Active& a : active)
        if (a.cb) {
            const uint64_t arg[1] = {a.bytes};
            guest_method(a.cb, 0, arg, 1);
            ++ncb;
        }
    std::vector<VoiceEvent> events;
    {
        std::lock_guard lock(mu_);
        const uint32_t mch = master_ ? master_->channels : 0;
        if (master_ && engine_running_) {
            for (Voice* v : voices_)
                if (v->kind != Kind::Source) std::fill(v->mix.begin(), v->mix.end(), 0.0f);
            for (const Active& a : active) {
                Voice* v = a.v;
                if (!live_.count(v) || !v->running) continue;
                render_source(v, &events);
                const uint32_t ch = v->channels;
                for (uint32_t i = 0; i < quantum_; ++i)
                    for (uint32_t c = 0; c < ch; ++c) v->scratch[i * ch + c] *= v->volume * v->channel_volumes[c];
                route(v, v->scratch.data());
            }
            std::vector<Voice*> subs;
            for (Voice* v : voices_)
                if (v->kind == Kind::Submix) subs.push_back(v);
            std::stable_sort(subs.begin(), subs.end(), [](Voice* a, Voice* b) { return a->stage < b->stage; });
            for (Voice* v : subs) {
                const uint32_t ch = v->channels;
                for (uint32_t i = 0; i < quantum_; ++i)
                    for (uint32_t c = 0; c < ch; ++c) v->mix[i * ch + c] *= v->volume * v->channel_volumes[c];
                route(v, v->mix.data());
            }
            for (uint32_t i = 0; i < quantum_; ++i)
                for (uint32_t c = 0; c < mch; ++c)
                    out[i * mch + c] = master_->mix[i * mch + c] * master_->volume * master_->channel_volumes[c];
        } else if (mch) {
            std::fill(out, out + static_cast<size_t>(quantum_) * mch, 0.0f);
        }
    }
    for (const VoiceEvent& e : events) {
        if (e.type == VoiceEvent::StreamEnd) {
            guest_method(e.callback, e.type);
        } else if (e.type == VoiceEvent::VoiceError) {
            const uint64_t a[2] = {e.context, static_cast<uint32_t>(e.error)};
            guest_method(e.callback, e.type, a, 2);
        } else {
            const uint64_t a[1] = {e.context};
            guest_method(e.callback, e.type, a, 1);
        }
        ++ncb;
    }
    for (const Active& a : active)
        if (a.cb) {
            guest_method(a.cb, 1);
            ++ncb;
        }
    for (uint64_t cb : engine_cbs) {
        guest_method(cb, 1);
        ++ncb;
    }
    const uint64_t dt = now_ns() - t0;
    std::lock_guard lock(mu_);
    in_pass_ = false;
    ++stats_.passes;
    stats_.pass_ns_total += dt;
    stats_.pass_ns_max = std::max(stats_.pass_ns_max, dt);
    stats_.pass_ns_min = std::min(stats_.pass_ns_min, dt);
    stats_.callbacks += ncb;
    pass_cv_.notify_all();
}

// ---- engine thread ----

void Engine::start_thread_locked() {
    if (os_thread_ || std_thread_.joinable()) return;
    if (mode_ == OutputMode::CoreAudio) {
        output_ = make_coreaudio_output();
        if (!output_->start(rate_, master_->channels, quantum_)) {
            MLOG(ERROR, "xaudio2", "falling back to silent output");
            output_ = make_silent_output();
            output_->start(rate_, master_->channels, quantum_);
        }
    } else {
        output_ = make_silent_output();
        output_->start(rate_, master_->channels, quantum_);
    }
    thread_stop_.store(false);
    gta6rt_thread_desc d{};
    d.fn = [](void* self) { static_cast<Engine*>(self)->thread_main(); };
    d.arg = this;
    d.name = "xaudio2";
    d.guest = 1;
    if (gta6rt_thread_create(&d, &os_thread_) != GTA6RT_OK) {
        os_thread_ = nullptr;
        std_thread_ = std::thread([this] { thread_main(); });
    }
}

void Engine::stop_thread() {
    thread_stop_.store(true);
    if (output_) output_->stop();
    if (os_thread_) {
        gta6rt_thread_join(os_thread_, GTA6RT_WAIT_INFINITE);
        gta6rt_thread_release(os_thread_);
        os_thread_ = nullptr;
    }
    if (std_thread_.joinable()) std_thread_.join();
    std::lock_guard lock(mu_);
    output_.reset();
}

void Engine::thread_main() {
    engine_thread_marker_.store(self_marker());
    pthread_setname_np("xaudio2");
    std::vector<float> buf;
    while (!thread_stop_.load()) {
        if (!output_->wait_ready()) break;
        uint32_t ch;
        {
            std::lock_guard lock(mu_);
            if (!master_) break;
            ch = master_->channels;
        }
        buf.resize(static_cast<size_t>(ch) * quantum_);
        run_pass(buf.data());
        output_->write(buf.data(), quantum_);
    }
    engine_thread_marker_.store(0);
}

}  // namespace rt::xa2
