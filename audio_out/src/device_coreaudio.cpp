#include "device.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace gta6rt::audio_out {

uint64_t host_now_ns() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    const uint64_t t = mach_absolute_time();
    return tb.numer == tb.denom ? t : static_cast<uint64_t>(static_cast<__uint128_t>(t) * tb.numer / tb.denom);
}

namespace {

AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) {
    return AudioObjectPropertyAddress{sel, scope, kAudioObjectPropertyElementMain};
}

template <class T>
bool get(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, T* out) {
    const AudioObjectPropertyAddress a = addr(sel, scope);
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, out) == noErr;
}

bool get_bytes(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope,
               std::vector<uint8_t>* out) {
    const AudioObjectPropertyAddress a = addr(sel, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(obj, &a, 0, nullptr, &size) != noErr || size == 0) return false;
    out->assign(size, 0);
    return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, out->data()) == noErr;
}

std::string device_name(AudioObjectID dev) {
    CFStringRef s = nullptr;
    if (!get(dev, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &s) || !s) return "?";
    char buf[256] = "?";
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(s);
    return buf;
}

Layout device_layout(AudioObjectID dev, uint32_t channels) {
    std::vector<uint8_t> bytes;
    if (get_bytes(dev, kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput, &bytes) &&
        bytes.size() >= offsetof(AudioChannelLayout, mChannelDescriptions)) {
        auto* l = reinterpret_cast<AudioChannelLayout*>(bytes.data());
        std::vector<uint8_t> expanded;
        if (l->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions) {
            UInt32 size = 0;
            const AudioChannelLayoutTag tag = l->mChannelLayoutTag;
            if (AudioFormatGetPropertyInfo(kAudioFormatProperty_ChannelLayoutForTag, sizeof tag, &tag, &size) == noErr &&
                size > 0) {
                expanded.assign(size, 0);
                if (AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutForTag, sizeof tag, &tag, &size,
                                           expanded.data()) == noErr) {
                    l = reinterpret_cast<AudioChannelLayout*>(expanded.data());
                }
            }
        }
        if (l->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions && l->mNumberChannelDescriptions) {
            uint32_t labels[kMaxChannels] = {};
            const uint32_t n = l->mNumberChannelDescriptions < channels ? l->mNumberChannelDescriptions : channels;
            for (uint32_t i = 0; i < n && i < kMaxChannels; ++i) labels[i] = l->mChannelDescriptions[i].mChannelLabel;
            Layout out = layout_from_labels(labels, n < kMaxChannels ? n : kMaxChannels);
            out.channels = channels;
            uint32_t known = 0;
            for (uint32_t i = 0; i < channels; ++i) known += out.speaker[i] != kNoSpeaker;
            if (known > 0) return out;
        }
    }
    if (channels == 1) return layout_from_mask(0x4, 1);
    if (channels == 2) return layout_from_mask(0x3, 2);
    Layout out;
    out.channels = channels;
    out.speaker[0] = kFL;
    out.speaker[1] = kFR;
    return out;
}

}  // namespace

struct Device::Impl {
    AudioUnit unit = nullptr;
    AudioObjectID dev = kAudioObjectUnknown;
    RenderFn render = nullptr;
    void* ctx = nullptr;
    ChangeFn changed = nullptr;
    void* change_ctx = nullptr;
    uint32_t channels = 0;
    bool system_listener = false;
    bool device_listeners = false;

    static OSStatus render_cb(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*, UInt32, UInt32 frames,
                              AudioBufferList* io) {
        auto* d = static_cast<Impl*>(ref);
        if (io && io->mNumberBuffers == 1 && io->mBuffers[0].mNumberChannels == d->channels &&
            io->mBuffers[0].mDataByteSize >= frames * d->channels * sizeof(float)) {
            d->render(d->ctx, static_cast<float*>(io->mBuffers[0].mData), frames, d->channels);
            return noErr;
        }
        if (io)
            for (UInt32 b = 0; b < io->mNumberBuffers; ++b) std::memset(io->mBuffers[b].mData, 0, io->mBuffers[b].mDataByteSize);
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
        return noErr;
    }

    static OSStatus listener(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* ref) {
        auto* d = static_cast<Impl*>(ref);
        if (d->changed) d->changed(d->change_ctx);
        return noErr;
    }

    void listen(bool on) {
        const AudioObjectPropertyAddress sys = addr(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
        const AudioObjectPropertyAddress dev_addrs[] = {
            addr(kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal),
            addr(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput),
            addr(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal),
        };
        if (on) {
            if (!system_listener)
                system_listener = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &sys, &listener, this) == noErr;
            if (!device_listeners && dev != kAudioObjectUnknown) {
                for (const auto& a : dev_addrs) AudioObjectAddPropertyListener(dev, &a, &listener, this);
                device_listeners = true;
            }
        } else {
            if (device_listeners && dev != kAudioObjectUnknown)
                for (const auto& a : dev_addrs) AudioObjectRemovePropertyListener(dev, &a, &listener, this);
            device_listeners = false;
        }
    }
};

Device::Device() : impl_(new Impl) {}

Device::~Device() {
    close();
    if (impl_->system_listener) {
        const AudioObjectPropertyAddress sys = addr(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &sys, &Impl::listener, impl_);
    }
    delete impl_;
}

bool Device::open() const { return impl_->unit != nullptr; }

bool Device::query_default(uint32_t* id, uint32_t* channels, uint32_t* rate) {
    AudioObjectID dev = kAudioObjectUnknown;
    if (!get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, &dev) ||
        dev == kAudioObjectUnknown)
        return false;
    std::vector<uint8_t> cfg;
    uint32_t n = 0;
    if (get_bytes(dev, kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, &cfg)) {
        const auto* bl = reinterpret_cast<const AudioBufferList*>(cfg.data());
        for (UInt32 i = 0; i < bl->mNumberBuffers; ++i) n += bl->mBuffers[i].mNumberChannels;
    }
    Float64 r = 0;
    get(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &r);
    UInt32 alive = 1;
    get(dev, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, &alive);
    *id = alive ? dev : 0;
    *channels = n < kMaxChannels ? n : kMaxChannels;
    *rate = static_cast<uint32_t>(r + 0.5);
    return true;
}

bool Device::prepare(uint32_t client_rate, RenderFn render, void* ctx, ChangeFn changed, void* change_ctx,
                     DeviceInfo* info, std::string* error) {
    close();
    Impl& d = *impl_;
    d.render = render;
    d.ctx = ctx;
    d.changed = changed;
    d.change_ctx = change_ctx;
    char msg[160];
    auto fail = [&](const char* step, OSStatus st) {
        std::snprintf(msg, sizeof msg, "%s: OSStatus %d", step, static_cast<int>(st));
        *error = msg;
        if (d.unit) {
            AudioComponentInstanceDispose(d.unit);
            d.unit = nullptr;
        }
        d.listen(true);
        return false;
    };

    AudioObjectID dev = kAudioObjectUnknown;
    if (!get(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, &dev) ||
        dev == kAudioObjectUnknown) {
        d.dev = kAudioObjectUnknown;
        return fail("no default output device", -1);
    }
    d.dev = dev;

    std::vector<uint8_t> cfg;
    uint32_t dev_channels = 0;
    if (get_bytes(dev, kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, &cfg)) {
        const auto* bl = reinterpret_cast<const AudioBufferList*>(cfg.data());
        for (UInt32 i = 0; i < bl->mNumberBuffers; ++i) dev_channels += bl->mBuffers[i].mNumberChannels;
    }
    if (dev_channels == 0) return fail("default device has no output channels", -1);
    const uint32_t channels = dev_channels < kMaxChannels ? dev_channels : kMaxChannels;

    Float64 rate = 0;
    get(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate);
    UInt32 io = 0, lat = 0, safety = 0, stream_lat = 0;
    get(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, &io);
    get(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &lat);
    get(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &safety);
    std::vector<uint8_t> streams;
    if (get_bytes(dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, &streams) && streams.size() >= 4) {
        AudioStreamID sid;
        std::memcpy(&sid, streams.data(), sizeof sid);
        get(sid, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, &stream_lat);
    }

    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) return fail("HAL output unit not found", -1);
    OSStatus st = AudioComponentInstanceNew(comp, &d.unit);
    if (st != noErr) return fail("AudioComponentInstanceNew", st);
    st = AudioUnitSetProperty(d.unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev);
    if (st != noErr) return fail("set CurrentDevice", st);

    AudioStreamBasicDescription f{};
    f.mSampleRate = client_rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 32;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * channels;
    f.mBytesPerPacket = f.mBytesPerFrame;
    st = AudioUnitSetProperty(d.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (st != noErr) return fail("set client StreamFormat", st);

    AURenderCallbackStruct cb{&Impl::render_cb, &d};
    st = AudioUnitSetProperty(d.unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (st != noErr) return fail("set render callback", st);
    UInt32 max_frames = 0, size = sizeof max_frames;
    AudioUnitGetProperty(d.unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, &size);
    st = AudioUnitInitialize(d.unit);
    if (st != noErr) return fail("AudioUnitInitialize", st);

    d.channels = channels;
    info->id = dev;
    info->channels = channels;
    info->rate = static_cast<uint32_t>(rate + 0.5);
    info->io_frames = io;
    info->latency_frames = lat + safety + stream_lat + io;
    info->max_frames = max_frames;
    info->layout = device_layout(dev, channels);
    info->name = device_name(dev);
    d.listen(true);
    return true;
}

bool Device::start(std::string* error) {
    if (!impl_->unit) return false;
    const OSStatus st = AudioOutputUnitStart(impl_->unit);
    if (st != noErr) {
        char msg[64];
        std::snprintf(msg, sizeof msg, "AudioOutputUnitStart: OSStatus %d", static_cast<int>(st));
        *error = msg;
        return false;
    }
    return true;
}

void Device::close() {
    Impl& d = *impl_;
    d.listen(false);
    if (!d.unit) return;
    AudioOutputUnitStop(d.unit);
    AudioUnitUninitialize(d.unit);
    AudioComponentInstanceDispose(d.unit);
    d.unit = nullptr;
}

}  // namespace gta6rt::audio_out
