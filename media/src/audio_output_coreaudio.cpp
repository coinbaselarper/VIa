#include "audio_output.h"
#include "media_common.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace rt::xa2 {
namespace {

class CoreAudioOutput final : public AudioOutput {
public:
    ~CoreAudioOutput() override {
        stop();
        if (sem_) dispatch_release(sem_);
    }

    bool start(uint32_t rate, uint32_t channels, uint32_t quantum) override {
        rate_ = rate;
        channels_ = channels;
        quantum_ = quantum;
        AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                       kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) return fail("AudioComponentFindNext(DefaultOutput) found nothing", 0);
        OSStatus st = AudioComponentInstanceNew(comp, &unit_);
        if (st != noErr) return fail("AudioComponentInstanceNew", st);

        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = rate;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        fmt.mBytesPerPacket = 4 * channels;
        fmt.mFramesPerPacket = 1;
        fmt.mBytesPerFrame = 4 * channels;
        fmt.mChannelsPerFrame = channels;
        fmt.mBitsPerChannel = 32;
        st = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt);
        if (st != noErr) return fail("set StreamFormat", st);
        AURenderCallbackStruct cb{&CoreAudioOutput::render, this};
        st = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                                  sizeof cb);
        if (st != noErr) return fail("set RenderCallback", st);
        st = AudioUnitInitialize(unit_);
        if (st != noErr) return fail("AudioUnitInitialize", st);
        initialized_ = true;

        AudioDeviceID dev = 0;
        UInt32 sz = sizeof dev;
        if (AudioUnitGetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, &sz) ==
            noErr) {
            device_ = dev;
            io_frames_ = device_u32(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
            latency_ = device_u32(kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput);
            safety_ = device_u32(kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput);
            Float64 nominal = 0;
            AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
            UInt32 n = sizeof nominal;
            if (AudioObjectGetPropertyData(device_, &a, 0, nullptr, &n, &nominal) == noErr) device_rate_ = nominal;
        }
        if (io_frames_ == 0) io_frames_ = 512;
        const double scale = device_rate_ > 0 ? rate_ / device_rate_ : 1.0;
        target_ = static_cast<uint32_t>(io_frames_ * scale) + quantum_;
        ring_.assign(static_cast<size_t>(std::max<uint32_t>(target_ * 4, 8192)) * channels_, 0.0f);
        ring_frames_ = static_cast<uint32_t>(ring_.size() / channels_);
        read_.store(0);
        write_.store(0);
        if (!sem_) sem_ = dispatch_semaphore_create(0);
        stopping_.store(false);
        st = AudioOutputUnitStart(unit_);
        if (st != noErr) return fail("AudioOutputUnitStart", st);
        started_ = true;
        MLOG(INFO, "xaudio2",
             "CoreAudio output: %u Hz %u ch (device %.0f Hz, io buffer %u, latency %u, safety %u frames; ring "
             "target %u frames)",
             rate_, channels_, device_rate_, io_frames_, latency_, safety_, target_);
        return true;
    }

    void stop() override {
        stopping_.store(true);
        if (sem_) dispatch_semaphore_signal(sem_);
        if (unit_) {
            if (started_) AudioOutputUnitStop(unit_);
            if (initialized_) AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
        started_ = initialized_ = false;
    }

    bool wait_ready() override {
        while (!stopping_.load(std::memory_order_acquire)) {
            const uint64_t fill = write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
            if (fill < target_ && ring_frames_ - fill >= quantum_) return true;
            dispatch_semaphore_wait(sem_, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC));
        }
        return false;
    }

    void write(const float* src, uint32_t frames) override {
        uint64_t w = write_.load(std::memory_order_relaxed);
        for (uint32_t f = 0; f < frames; ++f, ++w) {
            float* d = &ring_[static_cast<size_t>(w % ring_frames_) * channels_];
            std::memcpy(d, src + static_cast<size_t>(f) * channels_, channels_ * sizeof(float));
        }
        write_.store(w, std::memory_order_release);
    }

    uint32_t latency_frames() const override {
        const double scale = device_rate_ > 0 ? rate_ / device_rate_ : 1.0;
        return target_ + static_cast<uint32_t>((latency_ + safety_ + io_frames_) * scale);
    }
    uint32_t glitches() const override { return glitches_.load(std::memory_order_relaxed); }
    const char* name() const override { return "coreaudio"; }

    void info(CoreAudioInfo* i) const {
        i->device_rate = device_rate_;
        i->io_buffer_frames = io_frames_;
        i->device_latency_frames = latency_;
        i->safety_offset_frames = safety_;
        i->ring_target_frames = target_;
        i->callbacks = callbacks_.load();
        i->frames_rendered = rendered_.load();
        i->first_callback_ns = first_cb_ns_.load();
        i->last_callback_ns = last_cb_ns_.load();
    }

private:
    static OSStatus render(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                           AudioBufferList* io) {
        auto* self = static_cast<CoreAudioOutput*>(ref);
        float* out = static_cast<float*>(io->mBuffers[0].mData);
        const uint32_t ch = self->channels_;
        uint64_t r = self->read_.load(std::memory_order_relaxed);
        const uint64_t avail = self->write_.load(std::memory_order_acquire) - r;
        const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(avail, frames));
        for (uint32_t f = 0; f < take; ++f, ++r)
            std::memcpy(out + static_cast<size_t>(f) * ch, &self->ring_[static_cast<size_t>(r % self->ring_frames_) * ch],
                        ch * sizeof(float));
        if (take < frames) {
            std::memset(out + static_cast<size_t>(take) * ch, 0, static_cast<size_t>(frames - take) * ch * sizeof(float));
            if (self->rendered_.load(std::memory_order_relaxed) != 0) self->glitches_.fetch_add(1, std::memory_order_relaxed);
        }
        self->read_.store(r, std::memory_order_release);
        const uint64_t now = media::now_ns();
        if (self->callbacks_.fetch_add(1, std::memory_order_relaxed) == 0) self->first_cb_ns_.store(now);
        self->last_cb_ns_.store(now, std::memory_order_relaxed);
        self->rendered_.fetch_add(take, std::memory_order_relaxed);
        dispatch_semaphore_signal(self->sem_);
        return noErr;
    }

    uint32_t device_u32(AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) const {
        AudioObjectPropertyAddress a{sel, scope, kAudioObjectPropertyElementMain};
        UInt32 v = 0, n = sizeof v;
        if (AudioObjectGetPropertyData(device_, &a, 0, nullptr, &n, &v) != noErr) return 0;
        return v;
    }

    bool fail(const char* what, OSStatus st) {
        MLOG(ERROR, "xaudio2", "CoreAudio output unavailable: %s failed (OSStatus %d)", what, static_cast<int>(st));
        stop();
        return false;
    }

    AudioUnit unit_ = nullptr;
    AudioDeviceID device_ = 0;
    bool initialized_ = false, started_ = false;
    uint32_t rate_ = 48000, channels_ = 2, quantum_ = 480;
    double device_rate_ = 0;
    uint32_t io_frames_ = 0, latency_ = 0, safety_ = 0, target_ = 0;
    std::vector<float> ring_;
    uint32_t ring_frames_ = 0;
    std::atomic<uint64_t> read_{0}, write_{0};
    std::atomic<uint32_t> glitches_{0};
    std::atomic<uint64_t> callbacks_{0}, rendered_{0}, first_cb_ns_{0}, last_cb_ns_{0};
    std::atomic<bool> stopping_{false};
    dispatch_semaphore_t sem_ = nullptr;
};

}  // namespace

std::unique_ptr<AudioOutput> make_coreaudio_output() { return std::make_unique<CoreAudioOutput>(); }

bool coreaudio_info(const AudioOutput* out, CoreAudioInfo* info) {
    auto* ca = dynamic_cast<const CoreAudioOutput*>(out);
    if (!ca || !info) return false;
    ca->info(info);
    return true;
}

}  // namespace rt::xa2
