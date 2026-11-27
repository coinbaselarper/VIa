#pragma once

#include "audio_output.h"
#include "guest_abi.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <gta6rt/os.h>

namespace rt::xa2 {

using media::HRESULT;

enum class Kind { Source, Submix, Master };

struct Voice;
class Engine;

// The guest's IXAudio2*Voice pointer points here.
struct GuestVoice {
    const void* const* vtbl;
    Voice* voice;
};

struct SampleFormat {
    uint16_t tag = 0;       // kWaveFormatPcm or kWaveFormatIeeeFloat after unwrapping EXTENSIBLE
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint16_t container_bits = 0;
    uint16_t block_align = 0;
    uint32_t channel_mask = 0;
};

struct QueuedBuffer {
    media::XAudio2Buffer desc{};
    const uint8_t* data = nullptr;
    uint32_t play_end = 0;
    uint32_t loop_begin = 0, loop_end = 0;
    uint32_t loops_left = 0;
    uint32_t pos = 0;
    bool started = false;
};

struct Send {
    Voice* out = nullptr;
    uint32_t flags = 0;
    std::vector<float> matrix;  // out->channels rows x source channels
};

struct Voice {
    GuestVoice guest{};
    Engine* engine = nullptr;
    Kind kind = Kind::Source;
    uint32_t creation_flags = 0;
    uint32_t channels = 0;
    uint32_t rate = 0;
    uint32_t stage = 0;
    float volume = 1.0f;
    std::vector<float> channel_volumes;
    std::vector<Send> sends;
    std::vector<float> mix;

    SampleFormat fmt;
    float max_ratio = 1.0f;
    float ratio = 1.0f;
    uint64_t callback = 0;
    std::deque<QueuedBuffer> queue;
    bool running = false;
    uint64_t samples_played = 0;
    double frac = 0.0;
    std::vector<float> cur, nxt;
    bool cur_real = false, nxt_real = false, primed = false;
    std::vector<float> scratch;
    struct Pending {
        int type;
        uint64_t context;
    };
    // Events of the buffers whose last frame sits in cur/nxt: released when that frame is output.
    std::vector<Pending> cur_ev, nxt_ev;
    // Buffers fully fetched but whose last frame has not been output yet (still counted in BuffersQueued).
    std::deque<uint64_t> finished_ctx;
    std::vector<Pending> deferred_events;
};

// Event delivered to an IXAudio2VoiceCallback after a voice was mixed.
struct VoiceEvent {
    enum Type { BufferStart = 3, BufferEnd = 4, LoopEnd = 5, StreamEnd = 2, VoiceError = 6 } type;
    uint64_t callback;
    uint64_t context;
    HRESULT error;
};

struct EngineStats {
    uint64_t passes = 0;
    uint64_t pass_ns_total = 0;
    uint64_t pass_ns_max = 0;
    uint64_t pass_ns_min = ~0ull;
    uint64_t callbacks = 0;
};

class Engine {
public:
    enum class OutputMode { CoreAudio, Silent, Manual };

    Engine(uint32_t flags, uint32_t processor, OutputMode mode);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    HRESULT create_mastering(uint32_t channels, uint32_t rate, uint32_t flags, uint64_t device_id, uint64_t effects,
                             uint32_t category, Voice** out);
    HRESULT create_source(const media::WaveFormatEx* fmt, uint32_t flags, float max_ratio, uint64_t callback,
                          const media::XAudio2VoiceSends* sends, uint64_t effects, Voice** out);
    HRESULT create_submix(uint32_t channels, uint32_t rate, uint32_t flags, uint32_t stage,
                          const media::XAudio2VoiceSends* sends, uint64_t effects, Voice** out);
    void destroy_voice(Voice* v);
    Voice* lookup(uint64_t guest_voice);

    HRESULT register_callback(uint64_t cb);
    void unregister_callback(uint64_t cb);
    HRESULT start_engine();
    void stop_engine();
    HRESULT commit(uint32_t operation_set);
    void performance(media::XAudio2PerformanceData* out);

    void details(Voice* v, media::XAudio2VoiceDetails* out);
    HRESULT set_output_voices(Voice* v, const media::XAudio2VoiceSends* sends);
    HRESULT set_volume(Voice* v, float volume, uint32_t op);
    void get_volume(Voice* v, float* out);
    HRESULT set_channel_volumes(Voice* v, uint32_t count, const float* volumes, uint32_t op);
    HRESULT get_channel_volumes(Voice* v, uint32_t count, float* out);
    HRESULT set_output_matrix(Voice* v, uint64_t dest, uint32_t src_ch, uint32_t dst_ch, const float* m, uint32_t op);
    HRESULT get_output_matrix(Voice* v, uint64_t dest, uint32_t src_ch, uint32_t dst_ch, float* m);

    HRESULT start(Voice* v, uint32_t flags, uint32_t op);
    HRESULT stop(Voice* v, uint32_t flags, uint32_t op);
    HRESULT submit(Voice* v, const media::XAudio2Buffer* buf, const media::XAudio2BufferWma* wma);
    HRESULT flush(Voice* v);
    HRESULT discontinuity(Voice* v);
    HRESULT exit_loop(Voice* v, uint32_t op);
    void get_state(Voice* v, media::XAudio2VoiceState* out, uint32_t flags);
    HRESULT set_frequency_ratio(Voice* v, float ratio, uint32_t op);
    void get_frequency_ratio(Voice* v, float* out);
    HRESULT set_source_sample_rate(Voice* v, uint32_t rate);
    HRESULT get_channel_mask(Voice* v, uint32_t* mask);

    // One processing pass into `out` (master channels x quantum frames). Public for Manual mode tests.
    void run_pass(float* out);
    uint32_t quantum() const { return quantum_; }
    uint32_t engine_rate() const { return rate_; }
    uint32_t master_channels() const { return master_ ? master_->channels : 0; }
    EngineStats stats();
    const AudioOutput* output() const { return output_.get(); }

private:
    void start_thread_locked();
    void stop_thread();
    void thread_main();
    bool on_engine_thread() const;
    void defer_or_run(uint32_t op, std::function<void()> fn);
    HRESULT build_sends(Voice* v, const media::XAudio2VoiceSends* sends, std::vector<Send>* out);
    void default_matrix(uint32_t src, uint32_t dst, uint32_t dst_mask, std::vector<float>* m) const;
    bool fetch_frame(Voice* v, float* frame, std::vector<Voice::Pending>* attach, std::vector<VoiceEvent>* events);
    void render_source(Voice* v, std::vector<VoiceEvent>* events);
    void route(Voice* v, const float* frames);
    void finish_buffer(Voice* v, std::vector<Voice::Pending>* attach);
    void release(Voice* v, std::vector<Voice::Pending>* evs, std::vector<VoiceEvent>* out);
    uint32_t bytes_required(Voice* v) const;

    std::recursive_mutex mu_;
    std::condition_variable_any pass_cv_;
    bool in_pass_ = false;
    uint32_t flags_ = 0;
    OutputMode mode_;
    uint32_t rate_ = 48000;
    uint32_t quantum_ = 480;
    bool engine_running_ = true;
    Voice* master_ = nullptr;
    std::vector<Voice*> voices_;
    std::set<Voice*> live_;
    std::vector<uint64_t> engine_callbacks_;
    struct Deferred {
        uint32_t op;
        std::function<void()> fn;
    };
    std::vector<Deferred> deferred_;
    std::unique_ptr<AudioOutput> output_;
    gta6rt_thread* os_thread_ = nullptr;
    std::thread std_thread_;
    std::atomic<bool> thread_stop_{false};
    std::atomic<uint64_t> engine_thread_marker_{0};
    std::vector<float> out_buffer_;
    EngineStats stats_;
    uint64_t stats_query_passes_ = 0, stats_query_ns_ = 0, stats_query_wall_ = 0;
    uint64_t created_sources_ = 0;
};

// The Engine behind a guest IXAudio2 pointer made by XAudio2Create (tests).
Engine* engine_of(uint64_t guest_ixaudio2);

// The process-wide output choice: GTA6RT_XAUDIO2_OUTPUT = coreaudio (default) | silent.
Engine::OutputMode default_output_mode();
// Test seam: force a mode for engines created afterwards (Manual makes engines that never start a thread).
void set_output_mode_override(int mode);  // -1 clears

}  // namespace rt::xa2
