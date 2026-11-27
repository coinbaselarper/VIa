#include "check.h"

#include <gta6rt/audio_out.h>

#include <AudioToolbox/AudioToolbox.h>

#include <chrono>
#include <cmath>
#include <vector>

namespace {

struct Pcm {
    const float* data;
    UInt32 frames, at, ch;
};

OSStatus feed_pcm(AudioConverterRef, UInt32* packets, AudioBufferList* io, AudioStreamPacketDescription**, void* ref) {
    auto* p = static_cast<Pcm*>(ref);
    const UInt32 n = std::min(*packets, p->frames - p->at);
    io->mBuffers[0].mData = const_cast<float*>(p->data + size_t(p->at) * p->ch);
    io->mBuffers[0].mDataByteSize = n * 4 * p->ch;
    io->mBuffers[0].mNumberChannels = p->ch;
    p->at += n;
    *packets = n;
    return noErr;
}

// AudioToolbox's Opus encoder, 20 ms packets.
bool encode(const std::vector<float>& pcm, UInt32 ch, std::vector<std::vector<uint8_t>>* packets) {
    AudioStreamBasicDescription in{};
    in.mSampleRate = 48000;
    in.mFormatID = kAudioFormatLinearPCM;
    in.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    in.mChannelsPerFrame = ch;
    in.mBitsPerChannel = 32;
    in.mFramesPerPacket = 1;
    in.mBytesPerFrame = in.mBytesPerPacket = 4 * ch;
    AudioStreamBasicDescription out{};
    out.mSampleRate = 48000;
    out.mFormatID = kAudioFormatOpus;
    out.mChannelsPerFrame = ch;
    out.mFramesPerPacket = 960;
    AudioConverterRef c = nullptr;
    if (AudioConverterNew(&in, &out, &c) != noErr) return false;
    UInt32 bitrate = 128000 * ch;
    AudioConverterSetProperty(c, kAudioConverterEncodeBitRate, sizeof bitrate, &bitrate);
    UInt32 max = 0, size = sizeof max;
    AudioConverterGetProperty(c, kAudioConverterPropertyMaximumOutputPacketSize, &size, &max);
    if (!max) max = 4000;
    Pcm p{pcm.data(), static_cast<UInt32>(pcm.size() / ch), 0, ch};
    std::vector<uint8_t> buf(max * 8);
    AudioStreamPacketDescription descs[8];
    for (;;) {
        UInt32 n = 8;
        AudioBufferList bl{};
        bl.mNumberBuffers = 1;
        bl.mBuffers[0].mNumberChannels = ch;
        bl.mBuffers[0].mDataByteSize = static_cast<UInt32>(buf.size());
        bl.mBuffers[0].mData = buf.data();
        if (AudioConverterFillComplexBuffer(c, feed_pcm, &p, &n, &bl, descs) != noErr || n == 0) break;
        for (UInt32 i = 0; i < n; ++i)
            packets->emplace_back(buf.begin() + descs[i].mStartOffset, buf.begin() + descs[i].mStartOffset + descs[i].mDataByteSize);
    }
    AudioConverterDispose(c);
    return !packets->empty();
}

double amp_at(const std::vector<float>& x, UInt32 ch, UInt32 c, size_t a, size_t n, double f) {
    double re = 0, im = 0;
    for (size_t i = 0; i < n; ++i) {
        re += x[(a + i) * ch + c] * std::cos(2 * M_PI * f * i / 48000);
        im += x[(a + i) * ch + c] * std::sin(2 * M_PI * f * i / 48000);
    }
    return 2 * std::sqrt(re * re + im * im) / n;
}

}  // namespace

int main() {
    for (UInt32 ch : {1u, 2u}) {
        const UInt32 frames = 48000;
        std::vector<float> pcm(size_t(frames) * ch, 0.0f);
        for (UInt32 i = 0; i < frames; ++i)
            for (UInt32 c = 0; c < ch; ++c)
                pcm[size_t(i) * ch + c] = (i >= 4800 ? 0.4f : 0.0f) * static_cast<float>(std::sin(2 * M_PI * (1000.0 + 500 * c) * i / 48000));
        std::vector<std::vector<uint8_t>> packets;
        CHECK(encode(pcm, ch, &packets), "AudioToolbox Opus encoder (%u ch) for the test input", ch);
        size_t biggest = 0;
        for (auto& p : packets) biggest = std::max(biggest, p.size());
        gta6rt_opus_decoder* d = nullptr;
        CHECK(gta6rt_opus_decoder_create(48000, ch, &d) == 0 && d, "decoder %u ch", ch);
        if (!d) continue;
        std::vector<float> out;
        std::vector<float> buf(960 * ch);
        int not960 = 0, errors = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (auto& p : packets) {
            const int n = gta6rt_opus_decode(d, p.data(), static_cast<uint32_t>(p.size()), buf.data(), 960);
            if (n < 0) ++errors;
            else if (n != 960) ++not960;
            if (n > 0) out.insert(out.end(), buf.begin(), buf.begin() + size_t(n) * ch);
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        std::printf("opus %u ch: decode %.1f us per 20 ms packet (%.2f ms CPU per second of audio)\n", ch,
                    us / packets.size(), us / packets.size() * 50 / 1000);
        gta6rt_opus_decoder_destroy(d);
        size_t onset = 0;
        while (onset < out.size() / ch && std::fabs(out[onset * ch]) < 0.05f) ++onset;
        const size_t a = onset + 4800;
        const double a0 = amp_at(out, ch, 0, a, 9600, 1000.0);
        const double a1 = ch > 1 ? amp_at(out, ch, 1, a, 9600, 1500.0) : 0.4;
        std::printf("opus %u ch: %zu packets (max %zu bytes), %zu frames decoded, %d calls not 960 frames, %d errors, onset at "
                    "frame %zu (input onset 4800: decoder+encoder delay %ld frames), 1 kHz amplitude %.4f%s\n",
                    ch, packets.size(), biggest, out.size() / ch, not960, errors, onset, long(onset) - 4800, a0,
                    ch > 1 ? "" : "");
        if (ch > 1) std::printf("opus 2 ch: right channel 1.5 kHz amplitude %.4f\n", a1);
        CHECK(errors == 0, "no decode errors");
        CHECK(not960 <= 1, "one 20 ms packet gives 960 frames (%d calls did not)", not960);
        CHECK(std::fabs(a0 - 0.4) < 0.02 && std::fabs(a1 - 0.4) < 0.02, "tone level survives the round trip (%.4f %.4f)", a0, a1);
    }
    return finish("audio_out_opus_test");
}
