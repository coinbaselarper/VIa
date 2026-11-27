#include <gta6rt/audio_out.h>

#include "device.h"
#include "downmix.h"
#include "wav.h"

#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/task.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <strings.h>
#include <thread>
#include <time.h>
#include <vector>

using namespace gta6rt::audio_out;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kMaxStreams = 16;
constexpr uint32_t kScratchFrames = 8192;
constexpr uint32_t kClockChunk = 480;
constexpr uint64_t kDeviceStallNs = 20'000'000;
constexpr uint32_t kHistBins = 65536;

uint32_t fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float bitsf(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

const char* env(const char* n) {
    const char* v = std::getenv(n);
    return v && *v ? v : nullptr;
}

}  // namespace

struct gta6rt_audio_stream {
    std::string name;
    uint32_t channels = 0;
    uint32_t capacity = 0;
    Layout layout;
    std::unique_ptr<float[]> ring;
    Matrix mx;

    alignas(64) std::atomic<uint64_t> write_pos{0};
    alignas(64) std::atomic<uint64_t> read_pos{0};
    std::atomic<uint64_t> base{0};
    std::atomic<bool> running{false};
    std::atomic<bool> primed{false};
    std::atomic<uint32_t> volume{fbits(1.0f)};
    std::atomic<uint32_t> mute{0};
    std::atomic<uint32_t> ch_volume[kMaxChannels];

    std::atomic<uint32_t> pos_seq{0};
    std::atomic<uint64_t> pull_start{0};
    std::atomic<uint64_t> pull_frames{0};
    std::atomic<uint64_t> pull_ns{0};

    std::atomic<uint64_t> frames_dropped{0};
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> underrun_frames{0};
    std::atomic<uint64_t> clocked{0};

    semaphore_t sem = MACH_PORT_NULL;
    std::atomic<int> waiters{0};
    uint64_t retire_epoch = 0;

    gta6rt_audio_stream() {
        for (auto& v : ch_volume) v.store(fbits(1.0f));
    }
};

namespace {

using Stream = gta6rt_audio_stream;

double thread_cpu_us(uint32_t port) {
    if (!port) return 0;
    thread_basic_info_data_t info{};
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) != KERN_SUCCESS) return 0;
    return info.user_time.seconds * 1e6 + info.user_time.microseconds + info.system_time.seconds * 1e6 +
           info.system_time.microseconds;
}

struct Hist {
    std::unique_ptr<std::atomic<uint32_t>[]> bins{new std::atomic<uint32_t>[kHistBins]()};
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> max_us{0};
    void add(uint64_t us) {
        bins[us < kHistBins ? us : kHistBins - 1].fetch_add(1, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        if (us > max_us.load(std::memory_order_relaxed)) max_us.store(us, std::memory_order_relaxed);
    }
    double pct(double p) const {
        const uint64_t n = count.load();
        if (!n) return 0;
        const uint64_t want = static_cast<uint64_t>(std::ceil(p * n));
        uint64_t acc = 0;
        for (uint32_t i = 0; i < kHistBins; ++i) {
            acc += bins[i].load(std::memory_order_relaxed);
            if (acc >= want) return i;
        }
        return kHistBins - 1;
    }
};

class Output {
public:
    static Output& get() {
        static Output* o = new Output();
        return *o;
    }

    int open_stream(const gta6rt_audio_stream_desc* d, Stream** out);
    void close_stream(Stream* s);
    void device_render(float* out, uint32_t frames, uint32_t channels);
    void stats(gta6rt_audio_device_stats* out);
    void log_summary(const char* why);
    void streams_stats(gta6rt_audio_stats* o) {
        std::memset(o, 0, sizeof *o);
        for (auto& slot : slots_) {
            Stream* s = slot.load();
            if (!s) continue;
            gta6rt_audio_stats one;
            gta6rt_audio_stream_stats(s, &one);
            o->frames_written += one.frames_written;
            o->frames_consumed += one.frames_consumed;
            o->frames_dropped += one.frames_dropped;
            o->underruns += one.underruns;
            o->underrun_frames += one.underrun_frames;
            o->clocked_frames += one.clocked_frames;
        }
    }
    void set_master(float g, int mute) {
        master_.store(fbits(std::clamp(g, 0.0f, 1.0f)));
        master_mute_.store(mute ? 1 : 0);
    }
    int set_tap(const char* path);
    void request_reconfigure() {
        reconfigure_.store(true);
        if (device_sem_ != MACH_PORT_NULL) semaphore_signal(device_sem_);
    }
    void force_reconfigure() {
        force_reconfigure_.store(true);
        if (device_sem_ != MACH_PORT_NULL) semaphore_signal(device_sem_);
    }
    void shutdown();
    uint32_t latency_frames() const { return latency_.load(); }
    bool has_device() const { return device_ok_.load(); }

private:
    Output();
    void ensure_service();
    void service_loop();
    void device_loop();
    void reconfigure();
    void clock_tick(uint64_t now);
    void mix(float* out, uint32_t frames, uint32_t channels, uint64_t now, bool clocked);
    void apply_layout_locked(const Layout& l);
    void drain_tap();
    void free_retired(bool all);
    void wake() {
        if (wake_sem_ != MACH_PORT_NULL) semaphore_signal(wake_sem_);
    }
    static void render_thunk(void* ctx, float* out, uint32_t frames, uint32_t channels) {
        static_cast<Output*>(ctx)->device_render(out, frames, channels);
    }
    static void change_thunk(void* ctx) { static_cast<Output*>(ctx)->request_reconfigure(); }

    gta6rt_audio_mode mode_ = GTA6RT_AUDIO_OFF;
    std::mutex config_mutex_;
    Layout out_layout_;
    std::atomic<uint32_t> out_channels_{2};
    std::atomic<Stream*> slots_[kMaxStreams] = {};
    std::vector<Stream*> retired_;
    std::mutex retired_mutex_;
    std::atomic<uint64_t> epoch_{0};
    std::atomic<int> consumer_{0};

    std::unique_ptr<Device> device_;
    DeviceInfo info_;
    std::atomic<bool> device_ok_{false};
    std::atomic<bool> device_running_{false};
    std::atomic<uint64_t> last_device_ns_{0};
    std::atomic<bool> reconfigure_{false};
    std::atomic<bool> force_reconfigure_{false};
    bool device_changed();
    std::atomic<uint32_t> latency_{0};
    uint64_t next_retry_ns_ = 0;

    bool clock_active_ = false;
    uint64_t clock_anchor_ns_ = 0;
    uint64_t clock_done_ = 0;

    std::unique_ptr<float[]> pull_{new float[kScratchFrames * kMaxChannels]};
    std::unique_ptr<float[]> clock_buf_{new float[kClockChunk * kMaxChannels]};
    Limiter limiter_;
    std::atomic<uint32_t> master_{fbits(1.0f)};
    std::atomic<uint32_t> master_mute_{0};

    std::unique_ptr<float[]> tap_ring_;
    uint32_t tap_capacity_ = 0;
    uint32_t tap_channels_ = 0;
    std::atomic<uint64_t> tap_w_{0}, tap_r_{0};
    std::atomic<bool> tap_on_{false};
    std::atomic<uint64_t> tap_dropped_{0};
    std::atomic<uint64_t> tap_first_ns_{0};
    bool tap_first_logged_ = false;
    WavWriter wav_;
    std::mutex tap_mutex_;

    std::atomic<uint64_t> callbacks_{0}, cb_frames_{0}, device_changes_{0}, limiter_frames_{0};
    std::atomic<uint64_t> cb_ns_total_{0};
    uint64_t last_cb_start_ns_ = 0;
    Hist cb_hist_, interval_hist_;

    std::thread service_;
    std::thread device_thread_;
    semaphore_t device_sem_ = MACH_PORT_NULL;
    std::mutex info_mutex_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> service_port_{0};
    semaphore_t wake_sem_ = MACH_PORT_NULL;
    uint32_t stats_every_s_ = 0;
    uint64_t next_stats_ns_ = 0;
    std::mutex service_mutex_;
};

Output::Output() {
    semaphore_create(mach_task_self(), &wake_sem_, SYNC_POLICY_FIFO, 0);
    semaphore_create(mach_task_self(), &device_sem_, SYNC_POLICY_FIFO, 0);
    mlock(pull_.get(), sizeof(float) * kScratchFrames * kMaxChannels);
    mlock(clock_buf_.get(), sizeof(float) * kClockChunk * kMaxChannels);
    limiter_.configure(0.98f, kRate);
    out_layout_ = layout_from_mask(0x3, 2);
    float g = 1.0f;
    if (const char* v = env("GTA6RT_AUDIO_GAIN")) g = static_cast<float>(std::atof(v));
    set_master(g, env("GTA6RT_AUDIO_MUTE") && std::atoi(env("GTA6RT_AUDIO_MUTE")) != 0);
}

void Output::ensure_service() {
    std::lock_guard<std::mutex> lock(service_mutex_);
    if (service_.joinable()) return;
    mode_ = gta6rt_audio_out_mode();
    if (mode_ == GTA6RT_AUDIO_OFF) mode_ = GTA6RT_AUDIO_NULL;
    uint32_t null_channels = 2;
    if (const char* v = env("GTA6RT_AUDIO_NULL_CHANNELS")) null_channels = std::clamp(std::atoi(v), 1, 8);
    {
        std::lock_guard<std::mutex> c(config_mutex_);
        apply_layout_locked(layout_from_mask(0, null_channels));
    }
    stats_every_s_ = 10;
    if (const char* v = env("GTA6RT_AUDIO_STATS")) stats_every_s_ = static_cast<uint32_t>(std::atoi(v));
    stop_.store(false);
    if (mode_ == GTA6RT_AUDIO_ON) {
        device_ = std::make_unique<Device>();
        force_reconfigure_.store(true);
    }
    std::fprintf(stderr, "[audio_out] output %s: 48 kHz float, master gain %.3f%s\n", gta6rt_audio_mode_name(mode_),
                 bitsf(master_.load()), master_mute_.load() ? " (muted)" : "");
    if (const char* wav = env("GTA6RT_AUDIO_WAV")) set_tap(wav);
    service_ = std::thread([this] { service_loop(); });
    if (device_) device_thread_ = std::thread([this] { device_loop(); });
}

void Output::apply_layout_locked(const Layout& l) {
    out_layout_ = l;
    out_channels_.store(l.channels);
    for (auto& slot : slots_) {
        if (Stream* s = slot.load()) s->mx = build_matrix(s->layout, l);
    }
}

int Output::open_stream(const gta6rt_audio_stream_desc* d, Stream** out) {
    if (!d || !out) return -1;
    *out = nullptr;
    if (d->sample_rate != kRate) {
        std::fprintf(stderr, "[audio_out] stream %s: %u Hz not implemented (48000 only)\n", d->name ? d->name : "?",
                     d->sample_rate);
        return -2;
    }
    if (d->channels == 0 || d->channels > kMaxChannels) {
        std::fprintf(stderr, "[audio_out] stream %s: %u channels not implemented (1..8)\n", d->name ? d->name : "?",
                     d->channels);
        return -2;
    }
    ensure_service();
    auto* s = new (std::nothrow) Stream();
    if (!s) return -3;
    s->name = d->name ? d->name : "stream";
    s->channels = d->channels;
    s->capacity = std::max<uint32_t>(d->buffer_frames, 480);
    s->layout = layout_from_mask(d->channel_mask, d->channels);
    s->ring.reset(new (std::nothrow) float[static_cast<size_t>(s->capacity) * s->channels]());
    if (s->ring) mlock(s->ring.get(), sizeof(float) * s->capacity * s->channels);
    if (!s->ring || semaphore_create(mach_task_self(), &s->sem, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
        delete s;
        return -3;
    }
    std::lock_guard<std::mutex> lock(config_mutex_);
    s->mx = build_matrix(s->layout, out_layout_);
    for (auto& slot : slots_) {
        Stream* expected = nullptr;
        if (slot.compare_exchange_strong(expected, s)) {
            *out = s;
            std::fprintf(stderr, "[audio_out] stream %s opened: %u ch mask 0x%x, %u-frame ring, %s to %u device channels\n",
                         s->name.c_str(), s->channels, d->channel_mask, s->capacity,
                         s->mx.identity ? "pass-through" : "downmix", out_layout_.channels);
            return 0;
        }
    }
    semaphore_destroy(mach_task_self(), s->sem);
    delete s;
    std::fprintf(stderr, "[audio_out] stream %s: more than %u streams not implemented\n", d->name ? d->name : "?",
                 kMaxStreams);
    return -4;
}

void Output::close_stream(Stream* s) {
    if (!s) return;
    s->running.store(false);
    for (auto& slot : slots_) {
        Stream* expected = s;
        slot.compare_exchange_strong(expected, nullptr);
    }
    s->retire_epoch = epoch_.load();
    {
        std::lock_guard<std::mutex> lock(retired_mutex_);
        retired_.push_back(s);
    }
    wake();
}

void Output::free_retired(bool all) {
    std::vector<Stream*> done;
    {
        std::lock_guard<std::mutex> lock(retired_mutex_);
        const uint64_t e = epoch_.load();
        auto it = std::remove_if(retired_.begin(), retired_.end(), [&](Stream* s) {
            const bool safe = all || (s->retire_epoch & 1) == 0 || e != s->retire_epoch;
            if (safe) done.push_back(s);
            return safe;
        });
        retired_.erase(it, retired_.end());
    }
    for (Stream* s : done) {
        semaphore_destroy(mach_task_self(), s->sem);
        delete s;
    }
}

void Output::mix(float* out, uint32_t frames, uint32_t channels, uint64_t now, bool clocked) {
    std::memset(out, 0, sizeof(float) * frames * channels);
    for (auto& slot : slots_) {
        Stream* s = slot.load(std::memory_order_acquire);
        if (!s || !s->running.load(std::memory_order_acquire) || s->mx.out != channels) continue;
        const uint64_t r = s->read_pos.load(std::memory_order_relaxed);
        const uint64_t w = s->write_pos.load(std::memory_order_acquire);
        const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(w - r, frames));
        const uint32_t ch = s->channels;
        const uint32_t at = static_cast<uint32_t>(r % s->capacity);
        const uint32_t first = std::min(take, s->capacity - at);
        std::memcpy(pull_.get(), s->ring.get() + static_cast<size_t>(at) * ch, sizeof(float) * first * ch);
        if (take > first) std::memcpy(pull_.get() + first * ch, s->ring.get(), sizeof(float) * (take - first) * ch);
        if (take < frames) std::memset(pull_.get() + take * ch, 0, sizeof(float) * (frames - take) * ch);
        float gains[kMaxChannels];
        const float v = s->mute.load(std::memory_order_relaxed) ? 0.0f : bitsf(s->volume.load(std::memory_order_relaxed));
        for (uint32_t c = 0; c < ch; ++c) gains[c] = v * bitsf(s->ch_volume[c].load(std::memory_order_relaxed));
        mix_add(s->mx, pull_.get(), gains, out, frames);

        s->pos_seq.fetch_add(1, std::memory_order_acq_rel);
        s->pull_start.store(r, std::memory_order_relaxed);
        s->pull_frames.store(take, std::memory_order_relaxed);
        s->pull_ns.store(now, std::memory_order_relaxed);
        s->pos_seq.fetch_add(1, std::memory_order_acq_rel);
        s->read_pos.store(r + take, std::memory_order_release);
        if (take < frames && s->primed.load(std::memory_order_relaxed)) {
            s->underruns.fetch_add(1, std::memory_order_relaxed);
            s->underrun_frames.fetch_add(frames - take, std::memory_order_relaxed);
        }
        if (clocked) s->clocked.fetch_add(take, std::memory_order_relaxed);
        if (s->waiters.load(std::memory_order_relaxed) > 0) semaphore_signal(s->sem);
    }
    const uint32_t limited = limiter_.process(out, frames, channels);
    if (limited) limiter_frames_.fetch_add(limited, std::memory_order_relaxed);
    if (tap_on_.load(std::memory_order_acquire)) {
        const uint64_t w = tap_w_.load(std::memory_order_relaxed);
        const uint64_t r = tap_r_.load(std::memory_order_acquire);
        if (w - r + frames > tap_capacity_) {
            tap_dropped_.fetch_add(frames, std::memory_order_relaxed);
        } else {
            const uint32_t tc = tap_channels_;
            for (uint32_t f = 0; f < frames; ++f) {
                float* dst = tap_ring_.get() + ((w + f) % tap_capacity_) * tc;
                for (uint32_t c = 0; c < tc; ++c) dst[c] = c < channels ? out[f * channels + c] : 0.0f;
            }
            if (w == 0) tap_first_ns_.store(now, std::memory_order_relaxed);
            tap_w_.store(w + frames, std::memory_order_release);
        }
    }
    const float g = master_mute_.load(std::memory_order_relaxed) ? 0.0f : bitsf(master_.load(std::memory_order_relaxed));
    if (g != 1.0f)
        for (uint32_t i = 0; i < frames * channels; ++i) out[i] *= g;
}

void Output::device_render(float* out, uint32_t frames, uint32_t channels) {
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    const uint64_t t0 = host_now_ns();
    int expected = 0;
    if (channels != out_channels_.load(std::memory_order_relaxed) ||
        !consumer_.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
        std::memset(out, 0, sizeof(float) * frames * channels);
        epoch_.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    last_device_ns_.store(t0, std::memory_order_relaxed);
    for (uint32_t done = 0; done < frames;) {
        const uint32_t n = std::min(frames - done, kScratchFrames);
        mix(out + static_cast<size_t>(done) * channels, n, channels, t0, false);
        done += n;
    }
    consumer_.store(0, std::memory_order_release);
    const uint64_t t1 = host_now_ns();
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    cb_frames_.fetch_add(frames, std::memory_order_relaxed);
    cb_ns_total_.fetch_add(t1 - t0, std::memory_order_relaxed);
    cb_hist_.add((t1 - t0) / 1000);
    if (last_cb_start_ns_) interval_hist_.add((t0 - last_cb_start_ns_) / 1000);
    last_cb_start_ns_ = t0;
    epoch_.fetch_add(1, std::memory_order_acq_rel);
}

void Output::clock_tick(uint64_t now) {
    bool any = false;
    for (auto& slot : slots_) {
        Stream* s = slot.load();
        any = any || (s && s->running.load());
    }
    const bool device_live = device_running_.load() && now - last_device_ns_.load() < kDeviceStallNs;
    if (!any || device_live) {
        if (clock_active_ && device_live)
            std::fprintf(stderr, "[audio_out] device pulls again: host clock stopped\n");
        clock_active_ = false;
        return;
    }
    if (!clock_active_) {
        clock_active_ = true;
        clock_anchor_ns_ = now;
        clock_done_ = 0;
        if (mode_ == GTA6RT_AUDIO_ON)
            std::fprintf(stderr, "[audio_out] no device pulling: consuming on the host clock\n");
        return;
    }
    uint64_t due = (now - clock_anchor_ns_) * kRate / 1'000'000'000ull - clock_done_;
    if (due > 9600) {
        clock_done_ += due - 9600;
        due = 9600;
    }
    const uint32_t ch = out_channels_.load();
    while (due > 0) {
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(due, kClockChunk));
        int expected = 0;
        if (!consumer_.compare_exchange_strong(expected, 2)) break;
        mix(clock_buf_.get(), n, ch, now, true);
        consumer_.store(0);
        clock_done_ += n;
        due -= n;
    }
}

bool Output::device_changed() {
    if (!device_ok_.load()) return true;
    uint32_t id = 0, ch = 0, rate = 0;
    if (!Device::query_default(&id, &ch, &rate)) return true;
    return id != info_.id || ch != info_.channels || rate != info_.rate;
}

void Output::reconfigure() {
    device_running_.store(false);
    device_->close();
    device_ok_.store(false);
    DeviceInfo info;
    std::string err;
    if (!device_->prepare(kRate, &render_thunk, this, &change_thunk, this, &info, &err)) {
        std::fprintf(stderr, "[audio_out] device open failed (%s): consuming on the host clock, retry in 5 s\n",
                     err.c_str());
        next_retry_ns_ = host_now_ns() + 5'000'000'000ull;
        return;
    }
    for (int i = 0; i < 200; ++i) {
        int expected = 0;
        if (consumer_.compare_exchange_strong(expected, 3)) break;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        apply_layout_locked(info.layout);
        limiter_.configure(0.98f, kRate);
    }
    consumer_.store(0);
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        info_ = info;
    }
    const uint32_t lat = info.rate ? static_cast<uint32_t>(uint64_t(info.latency_frames) * kRate / info.rate) : 0;
    latency_.store(lat);
    if (!device_->start(&err)) {
        std::fprintf(stderr, "[audio_out] device start failed (%s): consuming on the host clock\n", err.c_str());
        device_->close();
        next_retry_ns_ = host_now_ns() + 5'000'000'000ull;
        return;
    }
    device_ok_.store(true);
    last_device_ns_.store(host_now_ns());
    device_running_.store(true);
    device_changes_.fetch_add(1);
    std::string labels;
    for (uint32_t i = 0; i < info.layout.channels; ++i) {
        static const char* names[] = {"FL", "FR", "FC", "LFE", "BL", "BR", "FLC", "FRC", "BC", "SL", "SR",
                                      "TC", "TFL", "TFC", "TFR", "TBL", "TBC", "TBR"};
        const uint8_t sp = info.layout.speaker[i];
        labels += i ? "," : "";
        labels += sp < 18 ? names[sp] : "-";
    }
    std::fprintf(stderr,
                 "[audio_out] device \"%s\" (id %u): %u ch [%s], %u Hz%s, io %u frames, latency %u frames, max slice %u\n",
                 info.name.c_str(), info.id, info.channels, labels.c_str(), info.rate,
                 info.rate != kRate ? " (the output unit converts from 48000)" : "", info.io_frames, info.latency_frames,
                 info.max_frames);
}

int Output::set_tap(const char* path) {
    std::lock_guard<std::mutex> lock(tap_mutex_);
    tap_on_.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    if (wav_.is_open()) {
        const uint64_t w = tap_w_.load();
        uint64_t r = tap_r_.load();
        while (r < w) {
            const uint64_t at = r % tap_capacity_;
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(w - r, tap_capacity_ - at));
            wav_.append(tap_ring_.get() + at * tap_channels_, n);
            r += n;
        }
        tap_r_.store(r);
        std::fprintf(stderr, "[audio_out] tap closed: %llu frames\n", static_cast<unsigned long long>(wav_.frames()));
        wav_.close();
    }
    if (!path) return 0;
    tap_channels_ = out_channels_.load();
    tap_capacity_ = kRate * 4;
    tap_ring_.reset(new float[static_cast<size_t>(tap_capacity_) * tap_channels_]());
    mlock(tap_ring_.get(), sizeof(float) * tap_capacity_ * tap_channels_);
    tap_w_.store(0);
    tap_r_.store(0);
    tap_first_ns_.store(0);
    tap_first_logged_ = false;
    if (!wav_.open(path, kRate, tap_channels_)) {
        std::fprintf(stderr, "[audio_out] tap %s: cannot open\n", path);
        return -1;
    }
    std::fprintf(stderr, "[audio_out] tap: %s (%u ch float, before master gain)\n", path, tap_channels_);
    tap_on_.store(true);
    return 0;
}

void Output::drain_tap() {
    std::lock_guard<std::mutex> lock(tap_mutex_);
    if (!wav_.is_open()) return;
    if (!tap_first_logged_ && tap_first_ns_.load()) {
        tap_first_logged_ = true;
        timespec wall{};
        clock_gettime(CLOCK_REALTIME, &wall);
        const uint64_t host = host_now_ns();
        std::fprintf(stderr,
                     "[audio_out] tap frame 0 mixed at host %llu ns (mach_absolute_time); output latency %u frames; clock "
                     "pair host %llu ns = epoch %lld.%09ld s\n",
                     static_cast<unsigned long long>(tap_first_ns_.load()), latency_.load(),
                     static_cast<unsigned long long>(host), static_cast<long long>(wall.tv_sec), wall.tv_nsec);
    }
    const uint64_t w = tap_w_.load(std::memory_order_acquire);
    uint64_t r = tap_r_.load();
    while (r < w) {
        const uint64_t at = r % tap_capacity_;
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(w - r, tap_capacity_ - at));
        wav_.append(tap_ring_.get() + at * tap_channels_, n);
        r += n;
    }
    tap_r_.store(r, std::memory_order_release);
}

void Output::service_loop() {
    service_port_.store(pthread_mach_thread_np(pthread_self()));
    uint64_t last_drain = 0;
    while (!stop_.load()) {
        semaphore_timedwait(wake_sem_, mach_timespec_t{0, 2'000'000});
        const uint64_t now = host_now_ns();
        clock_tick(host_now_ns());
        free_retired(false);
        if (now - last_drain > 50'000'000ull) {
            drain_tap();
            last_drain = now;
        }
        if (stats_every_s_ && now > next_stats_ns_) {
            if (next_stats_ns_) log_summary("periodic");
            next_stats_ns_ = now + uint64_t(stats_every_s_) * 1'000'000'000ull;
        }
    }
}

// Device open, start, rebuild and close happen here so that the service thread keeps the host clock running while
// CoreAudio takes its time.
void Output::device_loop() {
    while (!stop_.load()) {
        semaphore_timedwait(device_sem_, mach_timespec_t{0, 100'000'000});
        if (stop_.load()) break;
        const uint64_t now = host_now_ns();
        const bool notified = reconfigure_.exchange(false);
        const bool forced = force_reconfigure_.exchange(false);
        const bool retry = !device_ok_.load() && next_retry_ns_ && now > next_retry_ns_;
        if (forced || retry || (notified && device_changed())) {
            next_retry_ns_ = 0;
            reconfigure();
        }
    }
}

void Output::shutdown() {
    {
        std::lock_guard<std::mutex> lock(service_mutex_);
        if (!service_.joinable()) return;
        stop_.store(true);
        wake();
        if (device_sem_ != MACH_PORT_NULL) semaphore_signal(device_sem_);
    }
    service_.join();
    if (device_thread_.joinable()) device_thread_.join();
    if (device_) device_->close();
    device_running_.store(false);
    device_ok_.store(false);
    drain_tap();
    set_tap(nullptr);
    free_retired(true);
    device_.reset();
}

void Output::stats(gta6rt_audio_device_stats* o) {
    std::memset(o, 0, sizeof *o);
    std::lock_guard<std::mutex> lock(info_mutex_);
    o->callbacks = callbacks_.load();
    o->frames = cb_frames_.load();
    o->device_changes = device_changes_.load();
    o->tap_frames = tap_w_.load();
    o->tap_dropped = tap_dropped_.load();
    o->limiter_frames = limiter_frames_.load();
    o->device_channels = out_channels_.load();
    o->device_rate = device_ok_.load() ? info_.rate : 0;
    o->io_frames = device_ok_.load() ? info_.io_frames : 0;
    o->latency_frames = device_ok_.load() ? info_.latency_frames : 0;
    o->callback_p50_us = cb_hist_.pct(0.5);
    o->callback_p90_us = cb_hist_.pct(0.9);
    o->callback_p99_us = cb_hist_.pct(0.99);
    o->callback_p999_us = cb_hist_.pct(0.999);
    o->callback_max_us = static_cast<double>(cb_hist_.max_us.load());
    o->callback_interval_p50_us = interval_hist_.pct(0.5);
    o->callback_interval_p99_us = interval_hist_.pct(0.99);
    o->callback_interval_max_us = static_cast<double>(interval_hist_.max_us.load());
    o->callback_cpu_us_total = cb_ns_total_.load() / 1000.0;
    o->service_cpu_us_total = thread_cpu_us(service_port_.load());
}

void Output::log_summary(const char* why) {
    gta6rt_audio_device_stats d;
    stats(&d);
    std::string name = "(none)";
    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        if (device_ok_.load()) name = info_.name;
    }
    std::fprintf(stderr,
                 "[audio_out] %s: mode %s, device %s %u ch %u Hz io %u, callbacks %llu (%llu frames), callback us "
                 "p50 %.0f p99 %.0f p99.9 %.0f max %.0f, interval us p50 %.0f p99 %.0f max %.0f, callback time %.1f ms "
                 "total, service thread CPU %.1f ms, limiter frames %llu, tap %llu frames (%llu dropped), device changes %llu\n",
                 why, gta6rt_audio_mode_name(mode_), name.c_str(), d.device_channels,
                 d.device_rate, d.io_frames, (unsigned long long)d.callbacks, (unsigned long long)d.frames,
                 d.callback_p50_us, d.callback_p99_us, d.callback_p999_us, d.callback_max_us, d.callback_interval_p50_us,
                 d.callback_interval_p99_us, d.callback_interval_max_us, d.callback_cpu_us_total / 1000.0,
                 d.service_cpu_us_total / 1000.0, (unsigned long long)d.limiter_frames, (unsigned long long)d.tap_frames,
                 (unsigned long long)d.tap_dropped, (unsigned long long)d.device_changes);
    for (auto& slot : slots_) {
        Stream* s = slot.load();
        if (!s) continue;
        std::fprintf(stderr,
                     "[audio_out] %s: stream %s %s: written %llu consumed %llu (host clock %llu) padding %llu, dropped "
                     "%llu, underruns %llu (%llu frames)\n",
                     why, s->name.c_str(), s->running.load() ? "running" : "stopped",
                     (unsigned long long)s->write_pos.load(), (unsigned long long)s->read_pos.load(),
                     (unsigned long long)s->clocked.load(),
                     (unsigned long long)(s->write_pos.load() - s->read_pos.load()),
                     (unsigned long long)s->frames_dropped.load(), (unsigned long long)s->underruns.load(),
                     (unsigned long long)s->underrun_frames.load());
    }
}

}  // namespace

extern "C" {

gta6rt_audio_mode gta6rt_audio_out_mode(void) {
    const char* v = env("GTA6RT_AUDIO");
    if (!v) return GTA6RT_AUDIO_OFF;
    if (!strcasecmp(v, "on") || !strcasecmp(v, "1") || !strcasecmp(v, "coreaudio")) return GTA6RT_AUDIO_ON;
    if (!strcasecmp(v, "null") || !strcasecmp(v, "capture")) return GTA6RT_AUDIO_NULL;
    return GTA6RT_AUDIO_OFF;
}

const char* gta6rt_audio_mode_name(gta6rt_audio_mode m) {
    switch (m) {
        case GTA6RT_AUDIO_ON: return "on (CoreAudio)";
        case GTA6RT_AUDIO_NULL: return "null (host clock, no device)";
        default: return "off";
    }
}

int gta6rt_audio_stream_open(const gta6rt_audio_stream_desc* desc, gta6rt_audio_stream** out) {
    return Output::get().open_stream(desc, out);
}

void gta6rt_audio_stream_close(gta6rt_audio_stream* s) { Output::get().close_stream(s); }

int gta6rt_audio_stream_start(gta6rt_audio_stream* s) {
    if (!s) return -1;
    s->running.store(true, std::memory_order_release);
    return 0;
}

int gta6rt_audio_stream_stop(gta6rt_audio_stream* s) {
    if (!s) return -1;
    s->running.store(false, std::memory_order_release);
    return 0;
}

int gta6rt_audio_stream_reset(gta6rt_audio_stream* s) {
    if (!s || s->running.load()) return -1;
    const uint64_t w = s->write_pos.load();
    s->read_pos.store(w);
    s->base.store(w);
    s->primed.store(false);
    return 0;
}

uint32_t gta6rt_audio_stream_capacity(const gta6rt_audio_stream* s) { return s ? s->capacity : 0; }

uint32_t gta6rt_audio_stream_padding(const gta6rt_audio_stream* s) {
    if (!s) return 0;
    const uint64_t r = s->read_pos.load(std::memory_order_acquire);
    return static_cast<uint32_t>(s->write_pos.load(std::memory_order_relaxed) - r);
}

static uint32_t write_frames(gta6rt_audio_stream* s, const float* frames, uint32_t count) {
    if (!s) return 0;
    const uint64_t w = s->write_pos.load(std::memory_order_relaxed);
    const uint64_t r = s->read_pos.load(std::memory_order_acquire);
    const uint32_t room = static_cast<uint32_t>(s->capacity - (w - r));
    const uint32_t n = std::min(room, count);
    if (n < count) s->frames_dropped.fetch_add(count - n, std::memory_order_relaxed);
    const uint32_t ch = s->channels;
    const uint32_t at = static_cast<uint32_t>(w % s->capacity);
    const uint32_t first = std::min(n, s->capacity - at);
    float* ring = s->ring.get();
    if (frames) {
        std::memcpy(ring + static_cast<size_t>(at) * ch, frames, sizeof(float) * first * ch);
        if (n > first) std::memcpy(ring, frames + static_cast<size_t>(first) * ch, sizeof(float) * (n - first) * ch);
    } else {
        std::memset(ring + static_cast<size_t>(at) * ch, 0, sizeof(float) * first * ch);
        if (n > first) std::memset(ring, 0, sizeof(float) * (n - first) * ch);
    }
    s->write_pos.store(w + n, std::memory_order_release);
    if (n) s->primed.store(true, std::memory_order_relaxed);
    return n;
}

uint32_t gta6rt_audio_stream_write(gta6rt_audio_stream* s, const float* frames, uint32_t count) {
    return frames ? write_frames(s, frames, count) : 0;
}

uint32_t gta6rt_audio_stream_write_silence(gta6rt_audio_stream* s, uint32_t count) {
    return write_frames(s, nullptr, count);
}

static uint64_t position_raw(gta6rt_audio_stream* s, uint64_t* host_ns);

uint64_t gta6rt_audio_stream_position(gta6rt_audio_stream* s, uint64_t* host_ns) {
    if (!s) return 0;
    const uint64_t base = s->base.load(std::memory_order_acquire);
    const uint64_t p = position_raw(s, host_ns);
    return p > base ? p - base : 0;
}

static uint64_t position_raw(gta6rt_audio_stream* s, uint64_t* host_ns) {
    const uint64_t now = host_now_ns();
    if (host_ns) *host_ns = now;
    for (;;) {
        const uint32_t q0 = s->pos_seq.load(std::memory_order_acquire);
        if (q0 & 1) continue;
        const uint64_t start = s->pull_start.load(std::memory_order_relaxed);
        const uint64_t frames = s->pull_frames.load(std::memory_order_relaxed);
        const uint64_t at = s->pull_ns.load(std::memory_order_relaxed);
        const uint64_t done = s->read_pos.load(std::memory_order_acquire);
        if (s->pos_seq.load(std::memory_order_acquire) != q0) continue;
        if (!s->running.load(std::memory_order_relaxed) || at == 0 || start + frames != done) return done;
        const uint64_t elapsed = now > at ? (now - at) * kRate / 1'000'000'000ull : 0;
        return start + std::min(elapsed, frames);
    }
}

uint32_t gta6rt_audio_stream_latency(const gta6rt_audio_stream*) { return Output::get().latency_frames(); }

void gta6rt_audio_stream_set_volume(gta6rt_audio_stream* s, float volume, int mute) {
    if (!s) return;
    s->volume.store(fbits(std::clamp(volume, 0.0f, 1.0f)));
    s->mute.store(mute ? 1 : 0);
}

void gta6rt_audio_stream_set_channel_volume(gta6rt_audio_stream* s, uint32_t channel, float volume) {
    if (!s || channel >= kMaxChannels) return;
    s->ch_volume[channel].store(fbits(std::clamp(volume, 0.0f, 1.0f)));
}

int gta6rt_audio_stream_wait_progress(gta6rt_audio_stream* s, uint32_t timeout_ms) {
    if (!s) return 0;
    s->waiters.fetch_add(1);
    const kern_return_t kr =
        semaphore_timedwait(s->sem, mach_timespec_t{timeout_ms / 1000, static_cast<clock_res_t>((timeout_ms % 1000) * 1'000'000)});
    s->waiters.fetch_sub(1);
    return kr == KERN_SUCCESS ? 1 : 0;
}

void gta6rt_audio_stream_stats(const gta6rt_audio_stream* s, gta6rt_audio_stats* o) {
    if (!o) return;
    std::memset(o, 0, sizeof *o);
    if (!s) return;
    o->frames_written = s->write_pos.load();
    o->frames_consumed = s->read_pos.load();
    o->frames_dropped = s->frames_dropped.load();
    o->underruns = s->underruns.load();
    o->underrun_frames = s->underrun_frames.load();
    o->clocked_frames = s->clocked.load();
}

void gta6rt_audio_out_streams_stats(gta6rt_audio_stats* out) {
    if (out) Output::get().streams_stats(out);
}

void gta6rt_audio_device_stats_get(gta6rt_audio_device_stats* out) {
    if (out) Output::get().stats(out);
}

double gta6rt_audio_out_thread_cpu_us(void) {
    const mach_port_t self = mach_thread_self();
    const double us = thread_cpu_us(self);
    mach_port_deallocate(mach_task_self(), self);
    return us;
}

void gta6rt_audio_out_log_summary(const char* why) { Output::get().log_summary(why ? why : "summary"); }

void gta6rt_audio_out_set_master(float gain, int mute) { Output::get().set_master(gain, mute); }

int gta6rt_audio_out_set_tap(const char* path) { return Output::get().set_tap(path); }

void gta6rt_audio_out_force_reconfigure(void) { Output::get().force_reconfigure(); }

void gta6rt_audio_out_shutdown(void) { Output::get().shutdown(); }

}  // extern "C"
