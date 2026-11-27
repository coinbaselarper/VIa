#pragma once

#include <gta6rt/abi.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rt::media {

using HRESULT = int32_t;

constexpr HRESULT hr(uint32_t v) { return static_cast<HRESULT>(v); }

constexpr HRESULT kSOk = 0;
constexpr HRESULT kSFalse = 1;
constexpr HRESULT kENotImpl = hr(0x80004001u);
constexpr HRESULT kENoInterface = hr(0x80004002u);
constexpr HRESULT kEPointer = hr(0x80004003u);
constexpr HRESULT kEFail = hr(0x80004005u);
constexpr HRESULT kEOutOfMemory = hr(0x8007000Eu);
constexpr HRESULT kEInvalidArg = hr(0x80070057u);
constexpr HRESULT kENotSufficientBuffer = hr(0x8007007Au);
constexpr HRESULT kEFileNotFound = hr(0x80070002u);
constexpr HRESULT kEPathNotFound = hr(0x80070003u);

constexpr HRESULT kXAudio2EInvalidCall = hr(0x88960001u);
constexpr HRESULT kXAudio2EXmaDecoderError = hr(0x88960002u);
constexpr HRESULT kXAudio2EXapoCreationFailed = hr(0x88960003u);
constexpr HRESULT kXAudio2EDeviceInvalidated = hr(0x88960004u);

constexpr HRESULT kMfEInvalidType = hr(0xC00D36B4u);
constexpr HRESULT kMfEUnsupportedScheme = hr(0xC00D36C3u);
constexpr HRESULT kMfEUnsupportedByteStreamType = hr(0xC00D36C4u);
constexpr HRESULT kMfEBadStartupVersion = hr(0xC00D36E3u);
constexpr HRESULT kMfEAttributeNotFound = hr(0xC00D36E6u);
constexpr HRESULT kMfEShutdown = hr(0xC00D3E85u);

struct Guid {
    uint32_t data1;
    uint16_t data2, data3;
    uint8_t data4[8];
    bool operator==(const Guid& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
    bool operator!=(const Guid& o) const { return !(*this == o); }
    bool operator<(const Guid& o) const { return std::memcmp(this, &o, sizeof *this) < 0; }
};
static_assert(sizeof(Guid) == 16);

constexpr Guid kIidIUnknown{0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

// Every guest-visible COM object starts with its vtable pointer; the vtables hold GTA6RT_GUEST_ABI functions.
struct GuestObject {
    const void* const* vtbl;
};

// ---- XAudio2 2.9 (xaudio2.h packs every structure to 1 byte) ----
#pragma pack(push, 1)
struct WaveFormatEx {
    uint16_t wFormatTag;
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
    uint16_t cbSize;
};
struct WaveFormatExtensible {
    WaveFormatEx Format;
    uint16_t wValidBitsPerSample;
    uint32_t dwChannelMask;
    Guid SubFormat;
};
struct XAudio2Buffer {
    uint32_t Flags;
    uint32_t AudioBytes;
    uint64_t pAudioData;
    uint32_t PlayBegin;
    uint32_t PlayLength;
    uint32_t LoopBegin;
    uint32_t LoopLength;
    uint32_t LoopCount;
    uint64_t pContext;
};
struct XAudio2BufferWma {
    uint64_t pDecodedPacketCumulativeBytes;
    uint32_t PacketCount;
};
struct XAudio2VoiceState {
    uint64_t pCurrentBufferContext;
    uint32_t BuffersQueued;
    uint64_t SamplesPlayed;
};
struct XAudio2VoiceDetails {
    uint32_t CreationFlags;
    uint32_t ActiveFlags;
    uint32_t InputChannels;
    uint32_t InputSampleRate;
};
struct XAudio2SendDescriptor {
    uint32_t Flags;
    uint64_t pOutputVoice;
};
struct XAudio2VoiceSends {
    uint32_t SendCount;
    uint64_t pSends;
};
struct XAudio2EffectDescriptor {
    uint64_t pEffect;
    int32_t InitialState;
    uint32_t OutputChannels;
};
struct XAudio2EffectChain {
    uint32_t EffectCount;
    uint64_t pEffectDescriptors;
};
struct XAudio2FilterParameters {
    uint32_t Type;
    float Frequency;
    float OneOverQ;
};
struct XAudio2PerformanceData {
    uint64_t AudioCyclesSinceLastQuery;
    uint64_t TotalCyclesSinceLastQuery;
    uint32_t MinimumCyclesPerQuantum;
    uint32_t MaximumCyclesPerQuantum;
    uint32_t MemoryUsageInBytes;
    uint32_t CurrentLatencyInSamples;
    uint32_t GlitchesSinceEngineStarted;
    uint32_t ActiveSourceVoiceCount;
    uint32_t TotalSourceVoiceCount;
    uint32_t ActiveSubmixVoiceCount;
    uint32_t ActiveResamplerCount;
    uint32_t ActiveMatrixMixCount;
    uint32_t ActiveXmaSourceVoices;
    uint32_t ActiveXmaStreams;
};
struct XAudio2DebugConfiguration {
    uint32_t TraceMask;
    uint32_t BreakMask;
    int32_t LogThreadID;
    int32_t LogFileline;
    int32_t LogFunctionName;
    int32_t LogTiming;
};
#pragma pack(pop)

static_assert(sizeof(WaveFormatEx) == 18);
static_assert(sizeof(WaveFormatExtensible) == 40);
static_assert(sizeof(XAudio2Buffer) == 44 && offsetof(XAudio2Buffer, pContext) == 36);
static_assert(sizeof(XAudio2BufferWma) == 12);
static_assert(sizeof(XAudio2VoiceState) == 20 && offsetof(XAudio2VoiceState, BuffersQueued) == 8);
static_assert(sizeof(XAudio2VoiceDetails) == 16);
static_assert(sizeof(XAudio2SendDescriptor) == 12);
static_assert(sizeof(XAudio2VoiceSends) == 12);
static_assert(sizeof(XAudio2EffectDescriptor) == 16);
static_assert(sizeof(XAudio2EffectChain) == 12);
static_assert(sizeof(XAudio2FilterParameters) == 12);
static_assert(sizeof(XAudio2PerformanceData) == 64);
static_assert(sizeof(XAudio2DebugConfiguration) == 24);

constexpr uint32_t kXa2CommitNow = 0;
constexpr uint32_t kXa2CommitAll = 0;
constexpr uint32_t kXa2MaxBufferBytes = 0x80000000u;
constexpr uint32_t kXa2MaxQueuedBuffers = 64;
constexpr uint32_t kXa2MaxAudioChannels = 64;
constexpr uint32_t kXa2MinSampleRate = 1000;
constexpr uint32_t kXa2MaxSampleRate = 200000;
constexpr float kXa2MaxVolumeLevel = 16777216.0f;
constexpr float kXa2MinFreqRatio = 1.0f / 1024.0f;
constexpr float kXa2MaxFreqRatio = 1024.0f;
constexpr float kXa2DefaultFreqRatio = 2.0f;
constexpr uint32_t kXa2MaxLoopCount = 254;
constexpr uint32_t kXa2LoopInfinite = 255;

constexpr uint32_t kXa2DebugEngine = 0x0001;
constexpr uint32_t kXa2VoiceNoPitch = 0x0002;
constexpr uint32_t kXa2VoiceNoSrc = 0x0004;
constexpr uint32_t kXa2VoiceUseFilter = 0x0008;
constexpr uint32_t kXa2PlayTails = 0x0020;
constexpr uint32_t kXa2EndOfStream = 0x0040;
constexpr uint32_t kXa2SendUseFilter = 0x0080;
constexpr uint32_t kXa2VoiceNoSamplesPlayed = 0x0100;
constexpr uint32_t kXa2StopEngineWhenIdle = 0x2000;
constexpr uint32_t kXa21024Quantum = 0x8000;
constexpr uint32_t kXa2NoVirtualAudioClient = 0x10000;

constexpr uint16_t kWaveFormatPcm = 0x0001;
constexpr uint16_t kWaveFormatAdpcm = 0x0002;
constexpr uint16_t kWaveFormatIeeeFloat = 0x0003;
constexpr uint16_t kWaveFormatWmaudio2 = 0x0161;
constexpr uint16_t kWaveFormatWmaudio3 = 0x0162;
constexpr uint16_t kWaveFormatXma2 = 0x0166;
constexpr uint16_t kWaveFormatExtensible = 0xFFFE;

constexpr Guid kSubtypePcm{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr Guid kSubtypeFloat{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// IID_IXAudio2 (2.9) and IID_IXAudio2Extension (xaudio2.h).
constexpr Guid kIidIXAudio2{0x2B02E3CF, 0x2E0B, 0x4ec3, {0xBE, 0x45, 0x1B, 0x2A, 0x3F, 0xE7, 0x21, 0x0D}};
constexpr Guid kIidIXAudio2Extension{0x84ac29bb, 0xd619, 0x44d2, {0xb1, 0x97, 0xe4, 0xac, 0xf7, 0xdf, 0x3e, 0xd6}};

// ---- Media Foundation ----
#pragma pack(push, 8)
struct PropVariant {
    uint16_t vt;
    uint16_t r1, r2, r3;
    union {
        uint32_t ulVal;
        uint64_t uhVal;
        double dblVal;
        uint64_t ptr;
        struct {
            uint32_t cElems;
            uint32_t pad;
            uint64_t pElems;
        } caub;
    };
};
#pragma pack(pop)
static_assert(sizeof(PropVariant) == 24 && offsetof(PropVariant, caub.pElems) == 16);

constexpr uint16_t kVtEmpty = 0;
constexpr uint16_t kVtR8 = 5;
constexpr uint16_t kVtUnknown = 13;
constexpr uint16_t kVtI8 = 20;
constexpr uint16_t kVtUi4 = 19;
constexpr uint16_t kVtUi8 = 21;
constexpr uint16_t kVtLpwstr = 31;
constexpr uint16_t kVtClsid = 72;
constexpr uint16_t kVtVectorUi1 = 0x1011;

constexpr uint32_t kMfVersion = 0x00020070;  // MF_SDK_VERSION 0x0002 << 16 | MF_API_VERSION 0x0070
constexpr uint32_t kMfStartupFull = 0;
constexpr uint32_t kMfStartupLite = 1;

constexpr uint32_t kMfResolutionMediaSource = 0x00000001;
constexpr uint32_t kMfResolutionByteStream = 0x00000002;

}  // namespace rt::media
