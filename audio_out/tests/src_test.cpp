#include "check.h"

#include <AudioToolbox/AudioToolbox.h>

#include <cmath>
#include <vector>

namespace {

struct Feed {
    const float* data;
    UInt32 frames, at, channels;
};

OSStatus feed(AudioConverterRef, UInt32* packets, AudioBufferList* io, AudioStreamPacketDescription**, void* ref) {
    auto* f = static_cast<Feed*>(ref);
    const UInt32 n = std::min(*packets, f->frames - f->at);
    io->mBuffers[0].mData = const_cast<float*>(f->data + size_t(f->at) * f->channels);
    io->mBuffers[0].mDataByteSize = n * 4 * f->channels;
    io->mBuffers[0].mNumberChannels = f->channels;
    f->at += n;
    *packets = n;
    return noErr;
}

AudioStreamBasicDescription fmt(double rate, UInt32 ch) {
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = ch;
    f.mBitsPerChannel = 32;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = f.mBytesPerPacket = 4 * ch;
    return f;
}

// Fits a*sin + b*cos at f over the middle of x; returns amplitude and the residual RMS.
void fit(const std::vector<float>& x, UInt32 ch, double f, double rate, double* amp, double* resid) {
    const size_t n = x.size() / ch, a = n / 4, b = 3 * n / 4;
    double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;
    for (size_t i = a; i < b; ++i) {
        const double s = std::sin(2 * M_PI * f * i / rate), c = std::cos(2 * M_PI * f * i / rate);
        ss += s * s, sc += s * c, cc += c * c, xs += x[i * ch] * s, xc += x[i * ch] * c;
    }
    const double det = ss * cc - sc * sc, A = (xs * cc - xc * sc) / det, B = (xc * ss - xs * sc) / det;
    *amp = std::sqrt(A * A + B * B);
    double e = 0;
    for (size_t i = a; i < b; ++i) {
        const double r = x[i * ch] - A * std::sin(2 * M_PI * f * i / rate) - B * std::cos(2 * M_PI * f * i / rate);
        e += r * r;
    }
    *resid = std::sqrt(e / (b - a));
}

}  // namespace

int main() {
    const UInt32 ch = 2, n = 48000;
    std::vector<float> in(size_t(n) * ch);
    for (UInt32 i = 0; i < n; ++i)
        for (UInt32 c = 0; c < ch; ++c) in[size_t(i) * ch + c] = 0.5f * static_cast<float>(std::sin(2 * M_PI * 1000.0 * i / 48000));
    for (double out_rate : {44100.0, 96000.0, 32000.0}) {
        const AudioStreamBasicDescription src = fmt(48000, ch), dst = fmt(out_rate, ch);
        AudioConverterRef conv = nullptr;
        CHECK(AudioConverterNew(&src, &dst, &conv) == noErr, "AudioConverterNew 48000 -> %.0f", out_rate);
        if (!conv) continue;
        UInt32 q = kAudioConverterQuality_High;
        AudioConverterSetProperty(conv, kAudioConverterSampleRateConverterQuality, sizeof q, &q);
        Feed f{in.data(), n, 0, ch};
        std::vector<float> out;
        std::vector<float> chunk(512 * ch);
        for (;;) {
            UInt32 packets = 512;
            AudioBufferList bl{};
            bl.mNumberBuffers = 1;
            bl.mBuffers[0].mNumberChannels = ch;
            bl.mBuffers[0].mDataByteSize = 512 * 4 * ch;
            bl.mBuffers[0].mData = chunk.data();
            if (AudioConverterFillComplexBuffer(conv, feed, &f, &packets, &bl, nullptr) != noErr || packets == 0) break;
            out.insert(out.end(), chunk.begin(), chunk.begin() + packets * ch);
        }
        AudioConverterDispose(conv);
        double amp = 0, resid = 0;
        fit(out, ch, 1000.0, out_rate, &amp, &resid);
        const double snr = 20 * std::log10(amp / std::sqrt(2) / resid);
        std::printf("48000 -> %.0f Hz: %zu frames out (expect %.0f), 1 kHz amplitude %.5f (in 0.5), residual %.2e, SNR %.1f dB\n",
                    out_rate, out.size() / ch, n * out_rate / 48000, amp, resid, snr);
        CHECK(std::fabs(double(out.size() / ch) - n * out_rate / 48000) < 64, "frame count");
        CHECK(std::fabs(amp - 0.5) < 0.005, "1 kHz passes at unity gain");
        CHECK(snr > 90, "SNR above 90 dB (%.1f)", snr);
    }
    return finish("audio_out_src_test");
}
