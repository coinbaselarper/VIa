#include <gta6rt/audio_out.h>

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

struct gta6rt_opus_decoder {
    AudioConverterRef conv = nullptr;
    uint32_t channels = 1;
    const void* packet = nullptr;
    uint32_t bytes = 0;
    bool fed = false;
    AudioStreamPacketDescription desc{};
};

namespace {

constexpr OSStatus kNoMoreInput = 'nmor';

OSStatus feed(AudioConverterRef, UInt32* packets, AudioBufferList* io, AudioStreamPacketDescription** desc, void* ref) {
    auto* d = static_cast<gta6rt_opus_decoder*>(ref);
    if (d->fed) {
        *packets = 0;
        return kNoMoreInput;
    }
    d->fed = true;
    io->mNumberBuffers = 1;
    io->mBuffers[0].mData = const_cast<void*>(d->packet);
    io->mBuffers[0].mDataByteSize = d->bytes;
    io->mBuffers[0].mNumberChannels = d->channels;
    d->desc = AudioStreamPacketDescription{0, 0, d->bytes};
    if (desc) *desc = &d->desc;
    *packets = 1;
    return noErr;
}

}  // namespace

extern "C" {

int gta6rt_opus_decoder_create(uint32_t rate, uint32_t channels, gta6rt_opus_decoder** out) {
    if (!out) return -1;
    *out = nullptr;
    if (channels < 1 || channels > 2) {
        std::fprintf(stderr, "[audio_out] opus: %u channels not implemented (1..2)\n", channels);
        return -2;
    }
    AudioStreamBasicDescription in{};
    in.mSampleRate = rate;
    in.mFormatID = kAudioFormatOpus;
    in.mChannelsPerFrame = channels;
    in.mFramesPerPacket = 960;
    AudioStreamBasicDescription pcm{};
    pcm.mSampleRate = rate;
    pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    pcm.mChannelsPerFrame = channels;
    pcm.mBitsPerChannel = 32;
    pcm.mFramesPerPacket = 1;
    pcm.mBytesPerFrame = pcm.mBytesPerPacket = 4 * channels;
    auto* d = new (std::nothrow) gta6rt_opus_decoder();
    if (!d) return -3;
    const OSStatus st = AudioConverterNew(&in, &pcm, &d->conv);
    if (st != noErr) {
        std::fprintf(stderr, "[audio_out] opus: AudioConverterNew failed, OSStatus %d\n", static_cast<int>(st));
        delete d;
        return -4;
    }
    d->channels = channels;
    *out = d;
    return 0;
}

int gta6rt_opus_decode(gta6rt_opus_decoder* d, const void* packet, uint32_t bytes, float* out, uint32_t max_frames) {
    if (!d || !packet || !bytes || !out) return -1;
    d->packet = packet;
    d->bytes = bytes;
    d->fed = false;
    UInt32 frames = max_frames;
    AudioBufferList bl{};
    bl.mNumberBuffers = 1;
    bl.mBuffers[0].mNumberChannels = d->channels;
    bl.mBuffers[0].mDataByteSize = max_frames * 4 * d->channels;
    bl.mBuffers[0].mData = out;
    const OSStatus st = AudioConverterFillComplexBuffer(d->conv, feed, d, &frames, &bl, nullptr);
    if (st != noErr && st != kNoMoreInput) return -static_cast<int>(st > 0 ? 2 : 3);
    return static_cast<int>(frames);
}

void gta6rt_opus_decoder_reset(gta6rt_opus_decoder* d) {
    if (d) AudioConverterReset(d->conv);
}

void gta6rt_opus_decoder_destroy(gta6rt_opus_decoder* d) {
    if (!d) return;
    AudioConverterDispose(d->conv);
    delete d;
}

}  // extern "C"
