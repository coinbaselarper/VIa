// xapu.dll Opus decode (command type 3) as the title drives it (exe+0x4f20ad0 builds the command, exe+0x4eaa840 reads
// the results): an AudioToolbox-encoded 1 kHz tone, one 20 ms packet per command through the slot's input, the float
// PCM back from the slot's output with the byte count at result +0x34.
#include <windows.h>

#include <AudioToolbox/AudioToolbox.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <gta6rt/shims.h>

#include "shims.h"

namespace {

using ConnectFn = HRESULT(__stdcall*)(void*, void*, void**);
using DisconnectFn = HRESULT(__stdcall*)(void*);
using EnqueueFn = HRESULT(__stdcall*)(void*, const uint32_t*, uint64_t);
using CountFn = uint32_t(__stdcall*)(void*, uint64_t);
using DequeueFn = HRESULT(__stdcall*)(void*, void*);

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Pcm {
    const float* data;
    UInt32 frames, at;
};

OSStatus feed(AudioConverterRef, UInt32* packets, AudioBufferList* io, AudioStreamPacketDescription**, void* ref) {
    auto* p = static_cast<Pcm*>(ref);
    const UInt32 n = std::min(*packets, p->frames - p->at);
    io->mBuffers[0].mData = const_cast<float*>(p->data + p->at);
    io->mBuffers[0].mDataByteSize = n * 4;
    io->mBuffers[0].mNumberChannels = 1;
    p->at += n;
    *packets = n;
    return noErr;
}

std::vector<std::vector<uint8_t>> encode(const std::vector<float>& pcm) {
    AudioStreamBasicDescription in{48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 4, 1, 4, 1, 32, 0};
    AudioStreamBasicDescription out{};
    out.mSampleRate = 48000;
    out.mFormatID = kAudioFormatOpus;
    out.mChannelsPerFrame = 1;
    out.mFramesPerPacket = 960;
    std::vector<std::vector<uint8_t>> packets;
    AudioConverterRef c = nullptr;
    if (AudioConverterNew(&in, &out, &c) != noErr) return packets;
    UInt32 bitrate = 64000;
    AudioConverterSetProperty(c, kAudioConverterEncodeBitRate, sizeof bitrate, &bitrate);
    Pcm p{pcm.data(), static_cast<UInt32>(pcm.size()), 0};
    std::vector<uint8_t> buf(8 * 1500);
    AudioStreamPacketDescription descs[8];
    for (;;) {
        UInt32 n = 8;
        AudioBufferList bl{};
        bl.mNumberBuffers = 1;
        bl.mBuffers[0].mNumberChannels = 1;
        bl.mBuffers[0].mDataByteSize = static_cast<UInt32>(buf.size());
        bl.mBuffers[0].mData = buf.data();
        if (AudioConverterFillComplexBuffer(c, feed, &p, &n, &bl, descs) != noErr || n == 0) break;
        for (UInt32 i = 0; i < n; ++i)
            packets.emplace_back(buf.begin() + descs[i].mStartOffset, buf.begin() + descs[i].mStartOffset + descs[i].mDataByteSize);
    }
    AudioConverterDispose(c);
    return packets;
}

#pragma pack(push, 4)
struct Decode {
    uint32_t type, slot;
    uint8_t zero[0x14];
    float rate;
    uint32_t pre_skip, frames, bytes;
    uint64_t input;
    uint32_t output_bytes;
    uint64_t output;
};
#pragma pack(pop)

}  // namespace

int main() {
    GDK_TEST_REGISTER();
    setenv("GTA6RT_XAPU_OPUS", "1", 1);
    const auto connect = reinterpret_cast<ConnectFn>(rt::shims::find("xapu.dll", "XApuConnect"));
    const auto disconnect = reinterpret_cast<DisconnectFn>(rt::shims::find("xapu.dll", "XApuDisconnect"));
    const auto enqueue = reinterpret_cast<EnqueueFn>(rt::shims::find("xapu.dll", "XApuEnqueueCommand"));
    const auto count = reinterpret_cast<CountFn>(rt::shims::find("xapu.dll", "XApuGetResultCount"));
    const auto dequeue = reinterpret_cast<DequeueFn>(rt::shims::find("xapu.dll", "XApuDequeueResult"));
    check(connect && disconnect && enqueue && count && dequeue, "xapu.dll exports");
    if (!connect) return 1;

    std::vector<float> tone(48000, 0.0f);
    for (int i = 4800; i < 48000; ++i) tone[i] = 0.4f * static_cast<float>(std::sin(2 * M_PI * 1000.0 * (i - 4800) / 48000));
    const auto packets = encode(tone);
    check(packets.size() >= 50, "AudioToolbox Opus encoder made the test packets");

    uint8_t block24[24] = {}, info[16] = {};
    void* h = nullptr;
    check(connect(block24, info, &h) == S_OK && h, "XApuConnect");
    uint8_t* base = nullptr;
    std::memcpy(&base, info, 8);
    const uint32_t slot = 5;
    const uint32_t open[16] = {1, slot, 0, 0, 0, 0, 1};
    enqueue(h, open, 0);
    uint8_t res[0x50] = {};
    check(count(h, 0) == 1 && dequeue(h, res) == S_OK && res[0] == 1 && res[4] == slot, "open: one type 1 result");

    std::vector<float> out;
    int bad = 0, nonzero_results = 0;
    uint32_t first_bytes = 0;
    for (const auto& p : packets) {
        if (p.size() > 0x280) {
            ++bad;
            continue;
        }
        std::memcpy(base + slot * 0x280, p.data(), p.size());
        Decode d{};
        d.type = 3;
        d.slot = slot;
        d.rate = 48000.0f;
        d.pre_skip = out.empty() && !first_bytes ? 312 : 0;
        d.frames = 960;
        d.bytes = static_cast<uint32_t>(p.size());
        d.input = reinterpret_cast<uint64_t>(base + slot * 0x280);
        d.output_bytes = 0xf00;
        d.output = reinterpret_cast<uint64_t>(base + 0x14000 + slot * 0x1e00);
        enqueue(h, reinterpret_cast<const uint32_t*>(&d), 0);
        std::memset(res, 0, sizeof res);
        if (count(h, 0) != 1 || dequeue(h, res) != S_OK) {
            ++bad;
            continue;
        }
        uint32_t type, rslot, bytes;
        std::memcpy(&type, res, 4);
        std::memcpy(&rslot, res + 4, 4);
        std::memcpy(&bytes, res + 0x34, 4);
        if (type != 3 || rslot != slot || (bytes & 0x1c) || bytes > 0xf00) ++bad;
        if (!first_bytes) first_bytes = bytes;
        if (bytes) ++nonzero_results;
        const float* f = reinterpret_cast<const float*>(base + 0x14000 + slot * 0x1e00);
        out.insert(out.end(), f, f + bytes / 4);
    }
    std::printf("xapu opus: %zu packets, %d bad results, %d with PCM, first result %u bytes, %zu samples back\n",
                packets.size(), bad, nonzero_results, first_bytes, out.size());
    check(bad == 0, "every decode gives a type 3 result for the slot with a 32-byte multiple at +0x34");
    check(first_bytes == (960 - 312) * 4, "pre-skip 312 applied to the first packet");
    size_t onset = 0;
    while (onset < out.size() && std::fabs(out[onset]) < 0.02f) ++onset;
    double re = 0, im = 0;
    const size_t a = 9600, n = 24000;
    for (size_t i = 0; i < n && a + i < out.size(); ++i) {
        re += out[a + i] * std::cos(2 * M_PI * 1000.0 * i / 48000);
        im += out[a + i] * std::sin(2 * M_PI * 1000.0 * i / 48000);
    }
    const double amp = 2 * std::sqrt(re * re + im * im) / n;
    std::printf("xapu opus: tone onset at sample %zu (written at 4800), 1 kHz amplitude %.4f (written 0.4)\n", onset, amp);
    check(onset >= 4795 && onset <= 4810, "output aligned with the input after the pre-skip");
    check(std::fabs(amp - 0.4) < 0.02, "tone level");

    const uint32_t close[16] = {2, slot};
    enqueue(h, close, 0);
    check(count(h, 0) == 1 && dequeue(h, res) == S_OK && res[0] == 2, "close: one type 2 result");
    setenv("GTA6RT_XAPU_OPUS", "0", 1);
    Decode d{};
    d.type = 3;
    d.slot = slot;
    enqueue(h, reinterpret_cast<const uint32_t*>(&d), 0);
    check(count(h, 0) == 0, "GTA6RT_XAPU_OPUS=0: no decode result (the previous behaviour)");
    disconnect(h);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
