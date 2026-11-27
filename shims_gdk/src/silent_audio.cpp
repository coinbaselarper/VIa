// The silent output endpoint (silent_audio.h): IMMDeviceEnumerator, IMMDevice, IPropertyStore, IAudioClient3 and its
// services, and ISpatialAudioClient2 with its object render stream, all in-process. Nothing here opens a host audio device; the samples a title writes go to a scratch buffer.
//
// No function-local statics or thread_local here: like com_shim.cpp, this runs on threads the title made.
#include "silent_audio.h"

#include "gdk_env.h"
#include "gdk_platform.h"
#include "gdk_thread.h"

#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <propsys.h>
#include <spatialaudioclient.h>

#if defined(GTA6RT_HAVE_AUDIO_OUT)
#include <gta6rt/audio_out.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <cstring>
#include <iterator>
#include <string>
#include <new>
#include <vector>

namespace rt::silent_audio {
namespace {

constexpr HRESULT kNotFound = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);  // 0x80070490

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT and KSDATAFORMAT_SUBTYPE_PCM.
constexpr GUID kSubtypeFloat{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypePcm{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Property keys (functiondiscoverykeys_devpkey.h, mmdeviceapi.h), spelled out so no INITGUID unit is needed.
constexpr GUID kDeviceFmtid{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}};
constexpr GUID kInterfaceFmtid{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}};
constexpr GUID kEngineFmtid{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}};
constexpr GUID kOemFmtid{0xe4870e26, 0x3cc5, 0x4cd2, {0xba, 0x46, 0xca, 0x0a, 0x9a, 0x70, 0xed, 0x04}};
constexpr GUID kEndpointFmtid{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}};
constexpr PROPERTYKEY kKeyFriendlyName{kDeviceFmtid, 14};        // PKEY_Device_FriendlyName
constexpr PROPERTYKEY kKeyDeviceDesc{kDeviceFmtid, 2};           // PKEY_Device_DeviceDesc
constexpr PROPERTYKEY kKeyInterfaceName{kInterfaceFmtid, 2};     // PKEY_DeviceInterface_FriendlyName
constexpr PROPERTYKEY kKeyDeviceFormat{kEngineFmtid, 0};         // PKEY_AudioEngine_DeviceFormat
constexpr PROPERTYKEY kKeyOemFormat{kOemFmtid, 3};               // PKEY_AudioEngine_OEMFormat
constexpr PROPERTYKEY kKeyFormFactor{kEndpointFmtid, 0};         // PKEY_AudioEndpoint_FormFactor
constexpr PROPERTYKEY kKeyPhysicalSpeakers{kEndpointFmtid, 3};   // PKEY_AudioEndpoint_PhysicalSpeakers
constexpr PROPERTYKEY kKeyEndpointGuid{kEndpointFmtid, 4};       // PKEY_AudioEndpoint_GUID
constexpr PROPERTYKEY kKeys[] = {kKeyFriendlyName, kKeyDeviceDesc, kKeyInterfaceName, kKeyDeviceFormat,
                                 kKeyOemFormat, kKeyFormFactor, kKeyPhysicalSpeakers, kKeyEndpointGuid};
constexpr uint32_t kFormFactorSpeakers = 1;
constexpr WCHAR kEndpointGuid[] = u"{5a4c2a5e-1e0d-4c1b-9a51-6c656f6e6964}";

constexpr uint32_t kMaxChannels = 32;

LONG g_served = 0;
LONG g_logged = 0;
SRWLOCK g_iid_lock = SRWLOCK_INIT;
GUID g_iids_logged[16]{};
uint32_t g_iid_count = 0;

bool same_key(const PROPERTYKEY& a, const PROPERTYKEY& b) {
    return a.pid == b.pid && IsEqualGUID(a.fmtid, b.fmtid);
}

// One [rt] line per interface the silent endpoint does not have.
void log_missing_iid_once(const char* where, REFIID riid) {
    AcquireSRWLockExclusive(&g_iid_lock);
    bool seen = false;
    for (uint32_t i = 0; i < g_iid_count; ++i) seen = seen || IsEqualGUID(g_iids_logged[i], riid);
    if (!seen && g_iid_count < 16) g_iids_logged[g_iid_count++] = riid;
    ReleaseSRWLockExclusive(&g_iid_lock);
    if (seen) return;
    char text[40];
    std::snprintf(text, sizeof text, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", riid.Data1, riid.Data2, riid.Data3,
                  riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3], riid.Data4[4], riid.Data4[5],
                  riid.Data4[6], riid.Data4[7]);
    std::fprintf(stderr, "[rt] audio: the silent output endpoint has no %s (%s); E_NOINTERFACE\n", text, where);
}

void note_served(const char* reason) {
    InterlockedIncrement(&g_served);
    if (InterlockedExchange(&g_logged, 1) == 0) {
        std::fprintf(stderr,
                     "[rt] audio: serving the silent output endpoint (%s): 48 kHz float 7.1, every sample discarded, "
                     "no host audio device opened\n",
                     reason);
    }
}

void mix_format(WAVEFORMATEXTENSIBLE* f) {
    std::memset(f, 0, sizeof *f);
    f->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f->Format.nChannels = kChannels;
    f->Format.nSamplesPerSec = kSampleRate;
    f->Format.wBitsPerSample = 32;
    f->Format.nBlockAlign = static_cast<WORD>(kChannels * 4);
    f->Format.nAvgBytesPerSec = kSampleRate * kChannels * 4;
    f->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f->Samples.wValidBitsPerSample = 32;
    f->dwChannelMask = kChannelMask;
    f->SubFormat = kSubtypeFloat;
}

WAVEFORMATEX* alloc_mix_format() {
    auto* f = static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (f) mix_format(f);
    return reinterpret_cast<WAVEFORMATEX*>(f);
}

bool is_float(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    return f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22 &&
           IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat, kSubtypeFloat);
}

// A format a shared-mode stream could carry: PCM or float, 1..32 channels, 8..384 kHz, a consistent block.
bool valid_format(const WAVEFORMATEX* f) {
    if (f->nChannels == 0 || f->nChannels > kMaxChannels) return false;
    if (f->nSamplesPerSec < 8000 || f->nSamplesPerSec > 384000) return false;
    if (f->wBitsPerSample == 0 || f->wBitsPerSample % 8 != 0 || f->wBitsPerSample > 64) return false;
    if (f->nBlockAlign != f->nChannels * (f->wBitsPerSample / 8)) return false;
    if (f->wFormatTag == WAVE_FORMAT_PCM || f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag != WAVE_FORMAT_EXTENSIBLE || f->cbSize < 22) return false;
    const GUID& sub = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat;
    return IsEqualGUID(sub, kSubtypeFloat) || IsEqualGUID(sub, kSubtypePcm);
}

bool is_mix_format(const WAVEFORMATEX* f) {
    return f->nSamplesPerSec == kSampleRate && f->nChannels == kChannels && f->wBitsPerSample == 32 && is_float(f);
}

int64_t qpc_now() { return rt::gdk::qpc_now(); }

int64_t qpc_frequency() { return rt::gdk::qpc_frequency(); }

bool wide_equal(const WCHAR* a, const WCHAR* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

WCHAR* co_task_string(const WCHAR* s) {
    const size_t n = std::char_traits<WCHAR>::length(s) + 1;
    auto* out = static_cast<WCHAR*>(CoTaskMemAlloc(n * sizeof(WCHAR)));
    if (out) std::char_traits<WCHAR>::copy(out, s, n);
    return out;
}

// IUnknown for an object with one interface.
template <class Iface>
class Unknown : public Iface {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!IsEqualIID(riid, __uuidof(IUnknown)) && !answers(riid)) return E_NOINTERFACE;
        *ppv = static_cast<Iface*>(this);
        AddRef();
        return S_OK;
    }

protected:
    virtual ~Unknown() = default;
    virtual bool answers(REFIID riid) const { return IsEqualIID(riid, __uuidof(Iface)); }

private:
    LONG refs_ = 1;
};

// Calls `tick(context)` every period on its own thread, on a fixed schedule so a late wake-up does not push the
// next ones back, until stop() (or destruction).
class PeriodicThread {
public:
    using Tick = void (*)(void*);
    ~PeriodicThread() { stop(); }
    bool start(int64_t period_hns, Tick tick, void* context) {
        if (thread_.joinable()) return true;
        period_hns_ = period_hns;
        tick_ = tick;
        context_ = context;
        stopping_ = false;
        return rt::gdk::Thread::start(thread_, [this] { run(); }, "silent audio");
    }
    void stop() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

private:
    void run() {
        const int64_t f = qpc_frequency();
        const int64_t step = period_hns_ * f / 10000000;
        int64_t next = qpc_now() + step;
        for (;;) {
            const int64_t now = qpc_now();
            const auto period = std::chrono::nanoseconds(period_hns_ * 100);
            std::chrono::nanoseconds wait{0};
            if (next > now) wait = std::min(std::chrono::nanoseconds((next - now) * 1000000000 / f), period);
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (wake_.wait_for(lock, wait, [this] { return stopping_; })) break;
            }
            if (qpc_now() < next) continue;
            next += step;
            if (next < qpc_now()) next = qpc_now() + step;
            tick_(context_);
        }
    }

    int64_t period_hns_ = kDefaultPeriodHns;
    Tick tick_ = nullptr;
    void* context_ = nullptr;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    rt::gdk::Thread thread_;
};

#if defined(GTA6RT_HAVE_AUDIO_OUT)
// GTA6RT_AUDIO=on|null: streams hand their frames to gta6rt_audio_out and their events follow its consumption.
bool host_output() { return gta6rt_audio_out_mode() != GTA6RT_AUDIO_OFF; }

uint32_t target_frames() {
    const std::string v = rt::gdk::env_or("GTA6RT_AUDIO_TARGET_MS");
    const long ms = v.empty() ? 50 : std::strtol(v.c_str(), nullptr, 10);
    return static_cast<uint32_t>(std::clamp<long>(ms, 10, 500) * (kSampleRate / 1000));
}

// Calls `tick(context)` each time the output consumed frames, or every 5 ms, on its own thread.
class PacedThread {
public:
    using Tick = void (*)(void*);
    ~PacedThread() { stop(); }
    bool start(gta6rt_audio_stream* s, Tick tick, void* context) {
        if (thread_.joinable()) return true;
        stream_ = s;
        tick_ = tick;
        context_ = context;
        stopping_.store(false);
        return rt::gdk::Thread::start(thread_, [this] { run(); }, "audio pacer");
    }
    void stop() {
        if (!thread_.joinable()) return;
        stopping_.store(true);
        thread_.join();
    }

private:
    void run() {
        for (uint32_t n = 1; !stopping_.load(); ++n) {
            gta6rt_audio_stream_wait_progress(stream_, 5);
            if (!stopping_.load()) tick_(context_);
            if (n % 256 == 0) cpu_us_.store(gta6rt_audio_out_thread_cpu_us());
        }
    }

public:
    double cpu_us() const { return cpu_us_.load(); }

private:
    std::atomic<double> cpu_us_{0.0};
    gta6rt_audio_stream* stream_ = nullptr;
    Tick tick_ = nullptr;
    void* context_ = nullptr;
    std::atomic<bool> stopping_{false};
    rt::gdk::Thread thread_;
};
#endif

// ---- IAudioClient3 --------------------------------------------------------------------------------------------

class AudioClient final : public Unknown<IAudioClient3> {
public:
    // IAudioClient
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME buffer_hns,
                                         REFERENCE_TIME, const WAVEFORMATEX* format, LPCGUID) override {
        return init(mode, flags, buffer_hns, 0, format);
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override {
        if (!frames) return E_POINTER;
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        *frames = buffer_frames_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency) override {
        if (!latency) return E_POINTER;
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        *latency = kDefaultPeriodHns;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (out_) *latency += static_cast<REFERENCE_TIME>(gta6rt_audio_stream_latency(out_)) * 10000000 / rate_;
#endif
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* padding) override {
        if (!padding) return E_POINTER;
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        advance(qpc_now());
        *padding = static_cast<UINT32>(written_ - played_);
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (out_) *padding = gta6rt_audio_stream_padding(out_);
#endif
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE mode, const WAVEFORMATEX* format,
                                                WAVEFORMATEX** closest) override {
        if (closest) *closest = nullptr;
        if (!format) return E_POINTER;
        if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (mode != AUDCLNT_SHAREMODE_SHARED) return E_INVALIDARG;
        if (!closest) return E_POINTER;
        if (!valid_format(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (is_mix_format(format)) return S_OK;
        *closest = alloc_mix_format();
        return *closest ? S_FALSE : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override {
        if (!format) return E_POINTER;
        *format = alloc_mix_format();
        return *format ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* default_period, REFERENCE_TIME* minimum) override {
        if (!default_period && !minimum) return E_POINTER;
        if (default_period) *default_period = kDefaultPeriodHns;
        if (minimum) *minimum = kMinimumPeriodHns;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Start() override {
        HANDLE signal = nullptr;
        {
            Lock l(lock_);
            if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
            if (running_) return AUDCLNT_E_NOT_STOPPED;
            if (event_mode_ && !event_) return AUDCLNT_E_EVENTHANDLE_NOT_SET;
            anchor_qpc_ = qpc_now();
            played_at_anchor_ = played_;
            running_ = true;
            signal = event_;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
            if (out_) gta6rt_audio_stream_start(out_);
#endif
        }
        if (signal) SetEvent(signal);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override {
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!running_) return S_FALSE;
        advance(qpc_now());
        running_ = false;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (out_) gta6rt_audio_stream_stop(out_);
#endif
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        if (running_) return AUDCLNT_E_NOT_STOPPED;
        if (pending_) return AUDCLNT_E_BUFFER_OPERATION_PENDING;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (out_) {
            const bool empty = gta6rt_audio_stream_padding(out_) == 0 && gta6rt_audio_stream_position(out_, nullptr) == 0;
            gta6rt_audio_stream_reset(out_);
            return empty ? S_FALSE : S_OK;
        }
#endif
        if (written_ == 0 && played_ == 0) return S_FALSE;
        written_ = played_ = played_at_anchor_ = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event) override {
        if (!event || event == INVALID_HANDLE_VALUE) return E_INVALIDARG;
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!event_mode_) return AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
        event_ = event;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetService(REFIID riid, void** ppv) override;

    // IAudioClient2
    HRESULT STDMETHODCALLTYPE IsOffloadCapable(AUDIO_STREAM_CATEGORY, BOOL* capable) override {
        if (!capable) return E_POINTER;
        *capable = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetClientProperties(const AudioClientProperties* properties) override {
        if (!properties) return E_POINTER;
        Lock l(lock_);
        return initialized_ ? AUDCLNT_E_ALREADY_INITIALIZED : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBufferSizeLimits(const WAVEFORMATEX* format, BOOL, REFERENCE_TIME* min_hns,
                                                  REFERENCE_TIME* max_hns) override {
        if (!format || !min_hns || !max_hns) return E_POINTER;
        if (!valid_format(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        *min_hns = kDefaultPeriodHns;
        *max_hns = 20000000;  // 2 s
        return S_OK;
    }

    // IAudioClient3: one engine period only, 10 ms.
    HRESULT STDMETHODCALLTYPE GetSharedModeEnginePeriod(const WAVEFORMATEX* format, UINT32* default_frames,
                                                        UINT32* fundamental, UINT32* min_frames,
                                                        UINT32* max_frames) override {
        if (!format || !default_frames || !fundamental || !min_frames || !max_frames) return E_POINTER;
        if (!valid_format(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        const UINT32 period = static_cast<UINT32>(format->nSamplesPerSec / 100);
        *default_frames = *fundamental = *min_frames = *max_frames = period;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentSharedModeEnginePeriod(WAVEFORMATEX** format, UINT32* frames) override {
        if (!format || !frames) return E_POINTER;
        *format = alloc_mix_format();
        if (!*format) return E_OUTOFMEMORY;
        *frames = kSampleRate / 100;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InitializeSharedAudioStream(DWORD flags, UINT32 period_frames,
                                                          const WAVEFORMATEX* format, LPCGUID) override {
        if (!format) return E_POINTER;
        const REFERENCE_TIME hns = format->nSamplesPerSec
                                       ? static_cast<REFERENCE_TIME>(period_frames) * 10000000 / format->nSamplesPerSec
                                       : 0;
        return init(AUDCLNT_SHAREMODE_SHARED, flags, 0, hns, format);
    }

    // For the services.
    HRESULT get_buffer(UINT32 frames, BYTE** data) {
        if (!data) return E_POINTER;
        *data = nullptr;
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        if (pending_) return AUDCLNT_E_OUT_OF_ORDER;
        advance(qpc_now());
        uint64_t padding = written_ - played_;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (out_) padding = gta6rt_audio_stream_padding(out_);
#endif
        if (frames > buffer_frames_ - padding) return AUDCLNT_E_BUFFER_TOO_LARGE;
        pending_ = true;
        pending_frames_ = frames;
        *data = frames ? scratch_.data() : nullptr;
        return S_OK;
    }
    HRESULT release_buffer(UINT32 frames, DWORD flags) {
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
        if (!pending_) return frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
        if (frames > pending_frames_) return AUDCLNT_E_INVALID_SIZE;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (out_) {
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                gta6rt_audio_stream_write_silence(out_, frames);
            } else {
                to_float(scratch_.data(), frames);
                gta6rt_audio_stream_write(out_, conv_.data(), frames);
            }
            pending_ = false;
            pending_frames_ = 0;
            return S_OK;
        }
#endif
        // A write after the stream ran dry plays from now on (advance re-anchors), as a device fed silence in the gap.
        advance(qpc_now());
        written_ += frames;
        pending_ = false;
        pending_frames_ = 0;
        return S_OK;
    }
    void position(UINT64* frames, UINT64* qpc_hns) {
        Lock l(lock_);
        const int64_t now = qpc_now();
        advance(now);
        *frames = played_;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (out_) *frames = gta6rt_audio_stream_position(out_, nullptr);
#endif
        if (qpc_hns) {
            const int64_t f = qpc_frequency();
            *qpc_hns = static_cast<UINT64>((now / f) * 10000000 + (now % f) * 10000000 / f);
        }
    }
    uint32_t rate() const { return rate_; }
    uint32_t channels() const { return channels_; }
    float volume(uint32_t i) {
        Lock l(lock_);
        return i < kMaxChannels ? channel_volume_[i] : 0.0f;
    }
    void set_volume(uint32_t i, float v) {
        Lock l(lock_);
        if (i < kMaxChannels) channel_volume_[i] = v;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (out_) gta6rt_audio_stream_set_channel_volume(out_, i, v);
#endif
    }
    void volume_changed() {
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (out_) gta6rt_audio_stream_set_volume(out_, master_, mute_);
#endif
    }
    float master_ = 1.0f;
    BOOL mute_ = FALSE;

private:
    struct Lock {
        explicit Lock(SRWLOCK& s) : s_(s) { AcquireSRWLockExclusive(&s_); }
        ~Lock() { ReleaseSRWLockExclusive(&s_); }
        SRWLOCK& s_;
    };

    ~AudioClient() override {
        clock_.stop();
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        pacer_.stop();
        if (out_) gta6rt_audio_stream_close(out_);
#endif
    }

    // The client's frames as interleaved float; PCM 16/24/32 and float 32 (the formats open_host takes).
    void to_float(const BYTE* src, UINT32 frames) {
        const size_t n = static_cast<size_t>(frames) * channels_;
        float* dst = conv_.data();
        if (float_) {
            std::memcpy(dst, src, n * 4);
        } else if (bits_ == 16) {
            for (size_t i = 0; i < n; ++i) dst[i] = static_cast<int16_t>(src[2 * i] | (src[2 * i + 1] << 8)) / 32768.0f;
        } else if (bits_ == 24) {
            for (size_t i = 0; i < n; ++i) {
                const int32_t v = (src[3 * i] << 8) | (src[3 * i + 1] << 16) | (src[3 * i + 2] << 24);
                dst[i] = static_cast<float>(v) / 2147483648.0f;
            }
        } else {
            for (size_t i = 0; i < n; ++i) {
                int32_t v;
                std::memcpy(&v, src + 4 * i, 4);
                dst[i] = static_cast<float>(v) / 2147483648.0f;
            }
        }
    }

#if defined(GTA6RT_HAVE_AUDIO_OUT)
    // The host output for a 48 kHz PCM/float stream of 1..8 channels; anything else stays on the silent clock.
    bool open_host(const WAVEFORMATEX* format) {
        if (!host_output()) return false;
        const bool ext = format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22;
        const uint32_t mask = ext ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->dwChannelMask : 0;
        float_ = is_float(format);
        bits_ = format->wBitsPerSample;
        if (rate_ != kSampleRate || channels_ > 8 || (float_ && bits_ != 32) || (!float_ && bits_ != 16 && bits_ != 24 && bits_ != 32)) {
            std::fprintf(stderr,
                         "[rt] audio: host output for a %u ch %u Hz %u-bit %s stream not implemented: this stream stays "
                         "silent\n",
                         channels_, rate_, bits_, float_ ? "float" : "PCM");
            return false;
        }
        gta6rt_audio_stream_desc d{kSampleRate, channels_, mask, buffer_frames_, "IAudioClient"};
        if (gta6rt_audio_stream_open(&d, &out_) != 0) return false;
        conv_.assign(static_cast<size_t>(buffer_frames_) * channels_, 0.0f);
        if (event_mode_ && !pacer_.start(out_, &AudioClient::pace, this)) {
            gta6rt_audio_stream_close(out_);
            out_ = nullptr;
            return false;
        }
        return true;
    }

    static void pace(void* self) {
        auto* c = static_cast<AudioClient*>(self);
        HANDLE signal = nullptr;
        {
            Lock l(c->lock_);
            if (!c->running_) return;
            if (gta6rt_audio_stream_padding(c->out_) + c->period_frames() <= c->buffer_frames_) signal = c->event_;
        }
        if (signal) SetEvent(signal);
    }
    uint32_t period_frames() const { return static_cast<uint32_t>(period_hns_ * rate_ / 10000000); }
#endif

    bool answers(REFIID riid) const override {
        return IsEqualIID(riid, __uuidof(IAudioClient)) || IsEqualIID(riid, __uuidof(IAudioClient2)) ||
               IsEqualIID(riid, __uuidof(IAudioClient3));
    }

    HRESULT init(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME buffer_hns, REFERENCE_TIME period_hns,
                 const WAVEFORMATEX* format) {
        if (!format) return E_POINTER;
        if (mode == AUDCLNT_SHAREMODE_EXCLUSIVE) return AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
        if (mode != AUDCLNT_SHAREMODE_SHARED) return E_INVALIDARG;
        if (flags & AUDCLNT_STREAMFLAGS_LOOPBACK) return AUDCLNT_E_WRONG_ENDPOINT_TYPE;
        // Any PCM or float format is taken, not only the mix format: the samples are thrown away, so there is
        // nothing to convert, and a title never fails here for asking without AUTOCONVERTPCM.
        if (!valid_format(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        Lock l(lock_);
        if (initialized_) return AUDCLNT_E_ALREADY_INITIALIZED;
        rate_ = format->nSamplesPerSec;
        channels_ = format->nChannels;
        block_ = format->nBlockAlign;
        period_hns_ = period_hns > 0 ? period_hns : kDefaultPeriodHns;
        const REFERENCE_TIME hns = buffer_hns > 2 * period_hns_ ? buffer_hns : 2 * period_hns_;
        buffer_frames_ = static_cast<uint32_t>((hns * rate_ + 9999999) / 10000000);
        scratch_.assign(static_cast<size_t>(buffer_frames_) * block_, 0);
        for (float& v : channel_volume_) v = 1.0f;
        event_mode_ = (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
        bool host = false;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        host = open_host(format);
#endif
        if (!host && event_mode_ && !clock_.start(period_hns_, &AudioClient::tick, this)) return E_OUTOFMEMORY;
        initialized_ = true;
        if (InterlockedExchange(&g_stream_logged, 1) == 0) {
            std::fprintf(stderr, "[rt] audio: %s stream: %u ch, %u Hz, %u-byte frames, %u-frame buffer, %s\n",
                         host ? "host" : "silent", channels_, rate_, block_, buffer_frames_,
                         event_mode_ ? "event-driven" : "polled");
        }
        return S_OK;
    }

    // Frames the device has consumed since Start, at the stream's rate, clamped to what was written.
    void advance(int64_t now) {
        if (!running_) return;
        const int64_t f = qpc_frequency();
        const int64_t elapsed = now > anchor_qpc_ ? now - anchor_qpc_ : 0;
        const uint64_t frames = static_cast<uint64_t>((elapsed / f) * rate_ + (elapsed % f) * rate_ / f);
        const uint64_t candidate = played_at_anchor_ + frames;
        if (candidate >= written_) {
            // Ran dry: the device plays silence until more is written.
            played_ = written_;
            anchor_qpc_ = now;
            played_at_anchor_ = written_;
        } else {
            played_ = candidate;
        }
    }

    static void tick(void* self) {
        auto* c = static_cast<AudioClient*>(self);
        HANDLE signal = nullptr;
        {
            Lock l(c->lock_);
            if (!c->running_) return;
            c->advance(qpc_now());
            signal = c->event_;
        }
        if (signal) SetEvent(signal);
    }

    static inline LONG g_stream_logged = 0;

    SRWLOCK lock_ = SRWLOCK_INIT;
    bool initialized_ = false;
    bool event_mode_ = false;
    bool running_ = false;
    bool pending_ = false;
    uint32_t pending_frames_ = 0;
    uint32_t rate_ = kSampleRate;
    uint32_t channels_ = kChannels;
    uint32_t block_ = kChannels * 4;
    uint32_t buffer_frames_ = 0;
    REFERENCE_TIME period_hns_ = kDefaultPeriodHns;
    std::vector<BYTE> scratch_;
    HANDLE event_ = nullptr;  // the title's; not owned
    PeriodicThread clock_;
    uint64_t written_ = 0;
    uint64_t played_ = 0;
    uint64_t played_at_anchor_ = 0;
    int64_t anchor_qpc_ = 0;
    float channel_volume_[kMaxChannels]{};
    bool float_ = true;
    uint32_t bits_ = 32;
    std::vector<float> conv_;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
    gta6rt_audio_stream* out_ = nullptr;
    PacedThread pacer_;
#endif
};

// The services hold their client.
template <class Iface>
class Service : public Unknown<Iface> {
public:
    explicit Service(AudioClient* client) : client_(client) { client_->AddRef(); }

protected:
    ~Service() override { client_->Release(); }
    AudioClient* client_;
};

class RenderClient final : public Service<IAudioRenderClient> {
public:
    using Service::Service;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames, BYTE** data) override { return client_->get_buffer(frames, data); }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames, DWORD flags) override {
        return client_->release_buffer(frames, flags);
    }
};

class Clock final : public Service<IAudioClock> {
public:
    using Service::Service;
    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* frequency) override {
        if (!frequency) return E_POINTER;
        *frequency = client_->rate();  // the position counts frames
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPosition(UINT64* position, UINT64* qpc_hns) override {
        if (!position) return E_POINTER;
        client_->position(position, qpc_hns);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* characteristics) override {
        if (!characteristics) return E_POINTER;
        *characteristics = AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ;
        return S_OK;
    }
};

class SimpleVolume final : public Service<ISimpleAudioVolume> {
public:
    using Service::Service;
    HRESULT STDMETHODCALLTYPE SetMasterVolume(float level, LPCGUID) override {
        if (level < 0.0f || level > 1.0f) return E_INVALIDARG;
        client_->master_ = level;
        client_->volume_changed();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMasterVolume(float* level) override {
        if (!level) return E_POINTER;
        *level = client_->master_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMute(BOOL mute, LPCGUID) override {
        client_->mute_ = mute;
        client_->volume_changed();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMute(BOOL* mute) override {
        if (!mute) return E_POINTER;
        *mute = client_->mute_;
        return S_OK;
    }
};

class StreamVolume final : public Service<IAudioStreamVolume> {
public:
    using Service::Service;
    HRESULT STDMETHODCALLTYPE GetChannelCount(UINT32* count) override {
        if (!count) return E_POINTER;
        *count = client_->channels();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetChannelVolume(UINT32 index, const float level) override {
        if (index >= client_->channels() || level < 0.0f || level > 1.0f) return E_INVALIDARG;
        client_->set_volume(index, level);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetChannelVolume(UINT32 index, float* level) override {
        if (!level) return E_POINTER;
        if (index >= client_->channels()) return E_INVALIDARG;
        *level = client_->volume(index);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetAllVolumes(UINT32 count, const float* levels) override {
        if (!levels) return E_POINTER;
        if (count != client_->channels()) return E_INVALIDARG;
        for (UINT32 i = 0; i < count; ++i) {
            if (levels[i] < 0.0f || levels[i] > 1.0f) return E_INVALIDARG;
        }
        for (UINT32 i = 0; i < count; ++i) client_->set_volume(i, levels[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAllVolumes(UINT32 count, float* levels) override {
        if (!levels) return E_POINTER;
        if (count != client_->channels()) return E_INVALIDARG;
        for (UINT32 i = 0; i < count; ++i) levels[i] = client_->volume(i);
        return S_OK;
    }
};

HRESULT AudioClient::GetService(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    {
        Lock l(lock_);
        if (!initialized_) return AUDCLNT_E_NOT_INITIALIZED;
    }
    IUnknown* made = nullptr;
    if (IsEqualIID(riid, __uuidof(IAudioRenderClient))) made = new (std::nothrow) RenderClient(this);
    else if (IsEqualIID(riid, __uuidof(IAudioClock))) made = new (std::nothrow) Clock(this);
    else if (IsEqualIID(riid, __uuidof(ISimpleAudioVolume))) made = new (std::nothrow) SimpleVolume(this);
    else if (IsEqualIID(riid, __uuidof(IAudioStreamVolume))) made = new (std::nothrow) StreamVolume(this);
    else {
        log_missing_iid_once("IAudioClient::GetService", riid);
        return E_NOINTERFACE;
    }
    if (!made) return E_OUTOFMEMORY;
    const HRESULT hr = made->QueryInterface(riid, ppv);
    made->Release();
    return hr;
}

// ---- ISpatialAudioClient2 -------------------------------------------------------------------------------------
//
// A spatial endpoint with a 7.1 bed (the static objects of the mix format) and no dynamic objects, as an endpoint
// with no spatial sound format (Windows Sonic, Dolby Atmos) turned on. Objects are mono 48 kHz float; one pass is
// 10 ms (480 frames), and the stream's event is signalled every pass while it runs.

constexpr uint32_t kBedMask = AudioObjectType_FrontLeft | AudioObjectType_FrontRight | AudioObjectType_FrontCenter |
                              AudioObjectType_LowFrequency | AudioObjectType_SideLeft | AudioObjectType_SideRight |
                              AudioObjectType_BackLeft | AudioObjectType_BackRight;
constexpr uint32_t kStaticTypes = 0xFFFFE;  // FrontLeft .. StereoRight
constexpr uint32_t kObjectFrames = kSampleRate / 100;

void object_format(WAVEFORMATEX* f) {
    *f = WAVEFORMATEX{WAVE_FORMAT_IEEE_FLOAT, 1, kSampleRate, kSampleRate * 4, 4, 32, 0};
}

bool is_object_format(const WAVEFORMATEX* f) {
    return f && f->nChannels == 1 && f->nSamplesPerSec == kSampleRate && f->wBitsPerSample == 32 &&
           f->nBlockAlign == 4 && is_float(f);
}

bool one_type(uint32_t t) { return t != 0 && (t & (t - 1)) == 0; }

class FormatEnumerator final : public Unknown<IAudioFormatEnumerator> {
public:
    FormatEnumerator() { object_format(&format_); }
    HRESULT STDMETHODCALLTYPE GetCount(UINT32* count) override {
        if (!count) return E_POINTER;
        *count = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFormat(UINT32 index, WAVEFORMATEX** format) override {
        if (!format) return E_POINTER;
        *format = nullptr;
        if (index != 0) return E_INVALIDARG;
        *format = &format_;  // owned by the enumerator, as the system's is
        return S_OK;
    }

private:
    WAVEFORMATEX format_{};
};

class SpatialStream;

class SpatialObject final : public Unknown<ISpatialAudioObject> {
public:
    SpatialObject(SpatialStream* stream, uint32_t type);
    HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** buffer, UINT32* length) override;
    HRESULT STDMETHODCALLTYPE SetEndOfStream(UINT32 frames) override;
    HRESULT STDMETHODCALLTYPE IsActive(BOOL* active) override {
        if (!active) return E_POINTER;
        *active = InterlockedCompareExchange(&active_, 0, 0) ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAudioObjectType(AudioObjectType* type) override {
        if (!type) return E_POINTER;
        *type = static_cast<AudioObjectType>(type_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPosition(float, float, float) override { return S_OK; }  // static objects only
    HRESULT STDMETHODCALLTYPE SetVolume(float volume) override {
        if (!(volume >= 0.0f && volume <= 1.0f)) return E_INVALIDARG;
        volume_.store(volume);
        return S_OK;
    }
    // The stream ends the object at the end of the pass it called SetEndOfStream in.
    void make_inactive() { InterlockedExchange(&active_, 0); }
    // True once: whoever takes the type frees it in the stream.
    bool take_type() { return InterlockedExchange(&type_held_, 0) != 0; }
    uint32_t type() const { return type_; }
    // Host output: the object's own pass buffer, mixed into the bed at EndUpdatingAudioObjects.
    float samples_[kSampleRate / 100] = {};
    bool written_ = false;
    std::atomic<float> volume_{1.0f};

private:
    ~SpatialObject() override;
    SpatialStream* stream_;
    uint32_t type_;
    LONG active_ = 1;
    LONG type_held_ = 1;
    LONG ending_ = 0;
};

class SpatialStream final : public Unknown<ISpatialAudioObjectRenderStream> {
public:
    SpatialStream(HANDLE event, uint32_t mask) : event_(event), mask_(mask) {
        scratch_.assign(static_cast<size_t>(kObjectFrames) * 4, 0);
    }
    bool start_clock() {
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (host_output()) {
            target_ = target_frames();
            gta6rt_audio_stream_desc d{kSampleRate, kChannels, kChannelMask, std::max<uint32_t>(4 * target_, 9600),
                                       "spatial bed"};
            if (gta6rt_audio_stream_open(&d, &out_) == 0) {
                bed_.assign(static_cast<size_t>(kObjectFrames) * kChannels, 0.0f);
                if (pacer_.start(out_, &SpatialStream::pace, this)) return true;
                gta6rt_audio_stream_close(out_);
                out_ = nullptr;
            }
            std::fprintf(stderr, "[rt] audio: host output for the spatial stream failed to open: stays silent\n");
        }
#endif
        return clock_.start(kDefaultPeriodHns, &SpatialStream::tick, this);
    }
    bool host() const {
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        return out_ != nullptr;
#else
        return false;
#endif
    }
    uint32_t target() const { return target_; }

    HRESULT STDMETHODCALLTYPE GetAvailableDynamicObjectCount(UINT32* value) override {
        if (!value) return E_POINTER;
        *value = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetService(REFIID riid, void** service) override {
        if (!service) return E_POINTER;
        *service = nullptr;
        log_missing_iid_once("ISpatialAudioObjectRenderStream::GetService", riid);
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Start() override {
        {
            Lock l(lock_);
            if (running_) return S_OK;
            running_ = true;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
            if (out_) gta6rt_audio_stream_start(out_);
#endif
        }
        SetEvent(event_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override {
        Lock l(lock_);
        running_ = false;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (out_) gta6rt_audio_stream_stop(out_);
#endif
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        Lock l(lock_);
        if (running_) return SPTLAUDCLNT_E_STREAM_NOT_STOPPED;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        if (out_) gta6rt_audio_stream_reset(out_);
#endif
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginUpdatingAudioObjects(UINT32* available, UINT32* frames) override {
        if (!available || !frames) return E_POINTER;
        Lock l(lock_);
        if (updating_) return SPTLAUDCLNT_E_OUT_OF_ORDER;
        updating_ = true;
        *available = 0;
        *frames = kObjectFrames;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndUpdatingAudioObjects() override {
        std::vector<SpatialObject*> ended;
        bool more = false;
        {
            Lock l(lock_);
            if (!updating_) return SPTLAUDCLNT_E_OUT_OF_ORDER;
            updating_ = false;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
            if (out_) more = render_pass_locked();
#endif
            ended.swap(ending_);
            for (SpatialObject* o : ended) {
                o->make_inactive();
                if (o->take_type()) active_types_ &= ~o->type();
            }
        }
        for (SpatialObject* o : ended) o->Release();
        if (more) SetEvent(event_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateSpatialAudioObject(AudioObjectType type, ISpatialAudioObject** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        const uint32_t t = static_cast<uint32_t>(type);
        if (t == AudioObjectType_Dynamic) return SPTLAUDCLNT_E_NO_MORE_OBJECTS;
        if (!one_type(t) || (t & ~kStaticTypes)) return E_INVALIDARG;
        {
            Lock l(lock_);
            if (!(mask_ & t)) return SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE;
            if (active_types_ & t) return SPTLAUDCLNT_E_OBJECT_ALREADY_ACTIVE;
            active_types_ |= t;
        }
        auto* o = new (std::nothrow) SpatialObject(this, t);
        if (!o) {
            release_type(t);
            return E_OUTOFMEMORY;
        }
        *object = o;
        return S_OK;
    }

    // For the objects.
    HRESULT buffer(SpatialObject* o, BYTE** data, UINT32* length) {
        Lock l(lock_);
        if (!updating_) return SPTLAUDCLNT_E_OUT_OF_ORDER;
        if (host()) {
            if (!o->written_ && !(first_buffer_logged_ & o->type())) {
                first_buffer_logged_ |= o->type();
                std::fprintf(stderr, "[rt] audio: spatial object 0x%x: first GetBuffer (host output)\n", o->type());
            }
            o->written_ = true;
            std::memset(o->samples_, 0, sizeof o->samples_);
            *data = reinterpret_cast<BYTE*>(o->samples_);
            *length = static_cast<UINT32>(sizeof o->samples_);
            return S_OK;
        }
        *data = scratch_.data();
        *length = static_cast<UINT32>(scratch_.size());
        return S_OK;
    }
    void add_object(SpatialObject* o) {
        Lock l(lock_);
        objects_.push_back(o);
    }
    void remove_object(SpatialObject* o) {
        Lock l(lock_);
        objects_.erase(std::remove(objects_.begin(), objects_.end(), o), objects_.end());
    }
    HRESULT end_of_stream(SpatialObject* o) {
        Lock l(lock_);
        if (!updating_) return SPTLAUDCLNT_E_OUT_OF_ORDER;
        o->AddRef();
        ending_.push_back(o);
        return S_OK;
    }
    void release_type(uint32_t t) {
        Lock l(lock_);
        active_types_ &= ~t;
    }

private:
    struct Lock {
        explicit Lock(SRWLOCK& s) : s_(s) { AcquireSRWLockExclusive(&s_); }
        ~Lock() { ReleaseSRWLockExclusive(&s_); }
        SRWLOCK& s_;
    };
    ~SpatialStream() override {  // ending_ is empty: each entry holds the stream
        clock_.stop();
#if defined(GTA6RT_HAVE_AUDIO_OUT)
        pacer_.stop();
        if (out_) gta6rt_audio_stream_close(out_);
#endif
    }

#if defined(GTA6RT_HAVE_AUDIO_OUT)
    // Bed channel(s) of a static object type in the 7.1 mix format (FL FR FC LFE BL BR SL SR).
    static int bed_targets(uint32_t type, int ch[2], float gain[2]) {
        const float h = 0.70710678f;
        switch (type) {
            case AudioObjectType_FrontLeft: case 0x40000: ch[0] = 0, gain[0] = 1; return 1;
            case AudioObjectType_FrontRight: case 0x80000: ch[0] = 1, gain[0] = 1; return 1;
            case AudioObjectType_FrontCenter: ch[0] = 2, gain[0] = 1; return 1;
            case AudioObjectType_LowFrequency: ch[0] = 3, gain[0] = 1; return 1;
            case AudioObjectType_BackLeft: ch[0] = 4, gain[0] = 1; return 1;
            case AudioObjectType_BackRight: ch[0] = 5, gain[0] = 1; return 1;
            case AudioObjectType_SideLeft: ch[0] = 6, gain[0] = 1; return 1;
            case AudioObjectType_SideRight: ch[0] = 7, gain[0] = 1; return 1;
            case AudioObjectType_TopFrontLeft: case AudioObjectType_BottomFrontLeft: ch[0] = 0, gain[0] = h; return 1;
            case AudioObjectType_TopFrontRight: case AudioObjectType_BottomFrontRight: ch[0] = 1, gain[0] = h; return 1;
            case AudioObjectType_TopBackLeft: case AudioObjectType_BottomBackLeft: ch[0] = 4, gain[0] = h; return 1;
            case AudioObjectType_TopBackRight: case AudioObjectType_BottomBackRight: ch[0] = 5, gain[0] = h; return 1;
            case AudioObjectType_BackCenter: ch[0] = 4, ch[1] = 5, gain[0] = gain[1] = h; return 2;
            default: return 0;
        }
    }

    // Mixes the objects written this pass into one 7.1 frame block and queues it; true when the output still wants
    // another pass.
    bool render_pass_locked() {
        const auto t0 = std::chrono::steady_clock::now();
        std::fill(bed_.begin(), bed_.end(), 0.0f);
        float peak = 0.0f;
        double energy = 0.0;
        for (SpatialObject* o : objects_) {
            if (!o->written_) continue;
            o->written_ = false;
            int ch[2];
            float gain[2];
            const int n = bed_targets(o->type(), ch, gain);
            if (n == 0 && !(unplaced_logged_ & o->type())) {
                unplaced_logged_ |= o->type();
                std::fprintf(stderr, "[rt] audio: spatial object type 0x%x has no 7.1 bed position: not rendered\n",
                             o->type());
            }
            const float v = o->volume_.load();
            for (uint32_t f = 0; f < kObjectFrames; ++f) {
                const float x = o->samples_[f];
                peak = std::max(peak, std::fabs(x));
                energy += double(x) * x;
                for (int k = 0; k < n; ++k) bed_[f * kChannels + ch[k]] += x * v * gain[k];
            }
        }
        gta6rt_audio_stream_write(out_, bed_.data(), kObjectFrames);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        render_us_ += us;
        render_max_us_ = std::max(render_max_us_, us);
        ++passes_;
        if (peak > 1e-6f) ++loud_passes_;
        window_peak_ = std::max(window_peak_, peak);
        window_energy_ += energy;
        window_samples_ += kObjectFrames * 2;
        if (passes_ % 1000 == 0) {
            std::fprintf(stderr,
                         "[rt] audio: spatial stream: %llu passes, %llu with signal; last 1000: object peak %.4f, rms "
                         "%.5f (2 ch), mix+queue %.1f us mean %.1f max; pacer thread CPU %.1f ms\n",
                         static_cast<unsigned long long>(passes_), static_cast<unsigned long long>(loud_passes_),
                         window_peak_, std::sqrt(window_energy_ / std::max<uint64_t>(window_samples_, 1)),
                         render_us_ / 1000.0, render_max_us_, pacer_.cpu_us() / 1000.0);
            render_us_ = 0.0;
            render_max_us_ = 0.0;
            window_peak_ = 0.0f;
            window_energy_ = 0.0;
            window_samples_ = 0;
        }
        return running_ && gta6rt_audio_stream_padding(out_) + kObjectFrames <= target_;
    }

    static void pace(void* self) {
        auto* s = static_cast<SpatialStream*>(self);
        bool signal = false;
        {
            Lock l(s->lock_);
            signal = s->running_ && !s->updating_ && gta6rt_audio_stream_padding(s->out_) + kObjectFrames <= s->target_;
        }
        if (signal) SetEvent(s->event_);
    }
#endif

    static void tick(void* self) {
        auto* s = static_cast<SpatialStream*>(self);
        bool running = false;
        {
            Lock l(s->lock_);
            running = s->running_;
        }
        if (running) SetEvent(s->event_);
    }

    SRWLOCK lock_ = SRWLOCK_INIT;
    HANDLE event_;  // the title's; not owned
    uint32_t mask_;
    uint32_t active_types_ = 0;
    bool running_ = false;
    bool updating_ = false;
    std::vector<BYTE> scratch_;
    std::vector<SpatialObject*> ending_;
    std::vector<SpatialObject*> objects_;
    PeriodicThread clock_;
    uint32_t target_ = 0;
    uint32_t first_buffer_logged_ = 0;
    uint32_t unplaced_logged_ = 0;
#if defined(GTA6RT_HAVE_AUDIO_OUT)
    gta6rt_audio_stream* out_ = nullptr;
    PacedThread pacer_;
    std::vector<float> bed_;
    uint64_t passes_ = 0, loud_passes_ = 0, window_samples_ = 0;
    float window_peak_ = 0.0f;
    double window_energy_ = 0.0;
    double render_us_ = 0.0, render_max_us_ = 0.0;
#endif
};

SpatialObject::SpatialObject(SpatialStream* stream, uint32_t type) : stream_(stream), type_(type) {
    stream_->AddRef();
    stream_->add_object(this);
}
SpatialObject::~SpatialObject() {
    stream_->remove_object(this);
    if (take_type()) stream_->release_type(type_);
    stream_->Release();
}
HRESULT SpatialObject::GetBuffer(BYTE** buffer, UINT32* length) {
    if (!buffer || !length) return E_POINTER;
    *buffer = nullptr;
    *length = 0;
    if (!InterlockedCompareExchange(&active_, 0, 0)) return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    return stream_->buffer(this, buffer, length);
}
HRESULT SpatialObject::SetEndOfStream(UINT32) {
    if (!InterlockedCompareExchange(&active_, 0, 0)) return SPTLAUDCLNT_E_RESOURCES_INVALIDATED;
    if (InterlockedCompareExchange(&ending_, 1, 0) != 0) return S_OK;
    const HRESULT hr = stream_->end_of_stream(this);
    if (FAILED(hr)) InterlockedExchange(&ending_, 0);
    return hr;
}

class SpatialClient final : public Unknown<ISpatialAudioClient2> {
public:
    HRESULT STDMETHODCALLTYPE GetStaticObjectPosition(AudioObjectType type, float* x, float* y, float* z) override {
        if (!x || !y || !z) return E_POINTER;
        // Unit directions: x right, y up, z back (the listener faces -z).
        const float d = 0.70710678f;
        float p[3]{};
        switch (static_cast<uint32_t>(type)) {
            case AudioObjectType_FrontLeft: p[0] = -d, p[2] = -d; break;
            case AudioObjectType_FrontRight: p[0] = d, p[2] = -d; break;
            case AudioObjectType_FrontCenter: p[2] = -1; break;
            case AudioObjectType_LowFrequency: break;
            case AudioObjectType_SideLeft: p[0] = -1; break;
            case AudioObjectType_SideRight: p[0] = 1; break;
            case AudioObjectType_BackLeft: p[0] = -d, p[2] = d; break;
            case AudioObjectType_BackRight: p[0] = d, p[2] = d; break;
            default: return E_INVALIDARG;
        }
        *x = p[0];
        *y = p[1];
        *z = p[2];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNativeStaticObjectTypeMask(AudioObjectType* mask) override {
        if (!mask) return E_POINTER;
        *mask = static_cast<AudioObjectType>(kBedMask);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaxDynamicObjectCount(UINT32* value) override {
        if (!value) return E_POINTER;
        *value = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSupportedAudioObjectFormatEnumerator(IAudioFormatEnumerator** e) override {
        if (!e) return E_POINTER;
        *e = new (std::nothrow) FormatEnumerator();
        return *e ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetMaxFrameCount(const WAVEFORMATEX* format, UINT32* frames) override {
        if (!format || !frames) return E_POINTER;
        if (!is_object_format(format)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        *frames = kObjectFrames;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsAudioObjectFormatSupported(const WAVEFORMATEX* format) override {
        if (!format) return E_POINTER;
        return is_object_format(format) ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    HRESULT STDMETHODCALLTYPE IsSpatialAudioStreamAvailable(REFIID stream, const PROPVARIANT*) override {
        return IsEqualIID(stream, __uuidof(ISpatialAudioObjectRenderStream)) ? S_OK
                                                                             : SPTLAUDCLNT_E_STREAM_NOT_AVAILABLE;
    }
    HRESULT STDMETHODCALLTYPE ActivateSpatialAudioStream(const PROPVARIANT* params, REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!params) return E_INVALIDARG;
        if (!IsEqualIID(riid, __uuidof(ISpatialAudioObjectRenderStream))) {
            log_missing_iid_once("ISpatialAudioClient::ActivateSpatialAudioStream", riid);
            return E_NOINTERFACE;
        }
        if (params->vt != VT_BLOB || !params->blob.pBlobData ||
            params->blob.cbSize < sizeof(SpatialAudioObjectRenderStreamActivationParams)) {
            return E_INVALIDARG;
        }
        SpatialAudioObjectRenderStreamActivationParams p{};
        std::memcpy(&p, params->blob.pBlobData, sizeof p);
        if (!p.ObjectFormat || !p.EventHandle) return E_INVALIDARG;
        if (!is_object_format(p.ObjectFormat)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        const uint32_t mask = static_cast<uint32_t>(p.StaticObjectTypeMask);
        if (mask & ~kStaticTypes) return E_INVALIDARG;  // AudioObjectType_Dynamic is not a static type
        if (p.MinDynamicObjectCount > p.MaxDynamicObjectCount) return E_INVALIDARG;
        // Dynamic objects asked for are granted as none available (GetAvailableDynamicObjectCount is 0), so a
        // minimum above 0 is accepted rather than refused.
        auto* s = new (std::nothrow) SpatialStream(p.EventHandle, mask);
        if (!s) return E_OUTOFMEMORY;
        if (!s->start_clock()) {
            s->Release();
            return E_OUTOFMEMORY;
        }
        if (InterlockedExchange(&g_spatial_logged, 1) == 0) {
            std::fprintf(stderr,
                         "[rt] audio: %s spatial stream: static objects 0x%x, dynamic objects %u..%u asked "
                         "(0 available), %u frames a pass\n",
                         s->host() ? "host" : "silent", mask, p.MinDynamicObjectCount, p.MaxDynamicObjectCount,
                         kObjectFrames);
            if (s->host())
                std::fprintf(stderr,
                             "[rt] audio: host spatial stream: objects mixed into the 7.1 bed, event follows the output "
                             "(target padding %u frames)\n",
                             s->target());
        }
        const HRESULT hr = s->QueryInterface(riid, out);
        s->Release();
        return hr;
    }
    // ISpatialAudioClient2
    HRESULT STDMETHODCALLTYPE IsOffloadCapable(AUDIO_STREAM_CATEGORY, BOOL* capable) override {
        if (!capable) return E_POINTER;
        *capable = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMaxFrameCountForCategory(AUDIO_STREAM_CATEGORY, BOOL, const WAVEFORMATEX* format,
                                                          UINT32* frames) override {
        return GetMaxFrameCount(format, frames);
    }

private:
    bool answers(REFIID riid) const override {
        return IsEqualIID(riid, __uuidof(ISpatialAudioClient)) || IsEqualIID(riid, __uuidof(ISpatialAudioClient2));
    }
    static inline LONG g_spatial_logged = 0;
};

// ---- IPropertyStore -------------------------------------------------------------------------------------------

class PropertyStore final : public Unknown<IPropertyStore> {
public:
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) override {
        if (!count) return E_POINTER;
        *count = static_cast<DWORD>(sizeof kKeys / sizeof kKeys[0]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD index, PROPERTYKEY* key) override {
        if (!key) return E_POINTER;
        if (index >= sizeof kKeys / sizeof kKeys[0]) return E_INVALIDARG;
        *key = kKeys[index];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override {
        if (!value) return E_POINTER;
        PropVariantInit(value);
        if (same_key(key, kKeyFriendlyName) || same_key(key, kKeyDeviceDesc) || same_key(key, kKeyInterfaceName) ||
            same_key(key, kKeyEndpointGuid)) {
            value->pwszVal = co_task_string(same_key(key, kKeyEndpointGuid) ? kEndpointGuid : kFriendlyName);
            if (!value->pwszVal) return E_OUTOFMEMORY;
            value->vt = VT_LPWSTR;
        } else if (same_key(key, kKeyDeviceFormat) || same_key(key, kKeyOemFormat)) {
            value->blob.pBlobData = reinterpret_cast<BYTE*>(alloc_mix_format());
            if (!value->blob.pBlobData) return E_OUTOFMEMORY;
            value->blob.cbSize = sizeof(WAVEFORMATEXTENSIBLE);
            value->vt = VT_BLOB;
        } else if (same_key(key, kKeyFormFactor) || same_key(key, kKeyPhysicalSpeakers)) {
            value->ulVal = same_key(key, kKeyFormFactor) ? kFormFactorSpeakers : kChannelMask;
            value->vt = VT_UI4;
        }
        return S_OK;  // any other key: VT_EMPTY, as a host store answers a key it does not hold
    }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Commit() override { return STG_E_ACCESSDENIED; }
};

// ---- IMMDevice ------------------------------------------------------------------------------------------------

class Device final : public IMMDevice, public IMMEndpoint {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IMMDevice))) {
            *ppv = static_cast<IMMDevice*>(this);
        } else if (IsEqualIID(riid, __uuidof(IMMEndpoint))) {
            *ppv = static_cast<IMMEndpoint*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    // IMMDevice
    HRESULT STDMETHODCALLTYPE Activate(REFIID riid, DWORD, PROPVARIANT*, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, __uuidof(ISpatialAudioClient)) || IsEqualIID(riid, __uuidof(ISpatialAudioClient2))) {
            auto* spatial = new (std::nothrow) SpatialClient();
            if (!spatial) return E_OUTOFMEMORY;
            const HRESULT hr = spatial->QueryInterface(riid, ppv);
            spatial->Release();
            return hr;
        }
        if (!IsEqualIID(riid, __uuidof(IAudioClient)) && !IsEqualIID(riid, __uuidof(IAudioClient2)) &&
            !IsEqualIID(riid, __uuidof(IAudioClient3))) {
            log_missing_iid_once("IMMDevice::Activate", riid);
            return E_NOINTERFACE;
        }
        auto* client = new (std::nothrow) AudioClient();
        if (!client) return E_OUTOFMEMORY;
        const HRESULT hr = client->QueryInterface(riid, ppv);
        client->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD access, IPropertyStore** store) override {
        if (!store) return E_POINTER;
        *store = nullptr;
        if (access != STGM_READ && access != STGM_WRITE && access != STGM_READWRITE) return E_INVALIDARG;
        if (access != STGM_READ) return E_ACCESSDENIED;
        *store = new (std::nothrow) PropertyStore();
        return *store ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR* id) override {
        if (!id) return E_POINTER;
        *id = co_task_string(kDeviceId);
        return *id ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetState(DWORD* state) override {
        if (!state) return E_POINTER;
        *state = DEVICE_STATE_ACTIVE;
        return S_OK;
    }

    // IMMEndpoint
    HRESULT STDMETHODCALLTYPE GetDataFlow(EDataFlow* flow) override {
        if (!flow) return E_POINTER;
        *flow = eRender;
        return S_OK;
    }

private:
    ~Device() = default;
    LONG refs_ = 1;
};

IMMDevice* make_device(const char* reason) {
    auto* d = new (std::nothrow) Device();
    if (d) note_served(reason);
    return d;
}

// ---- IMMDeviceCollection --------------------------------------------------------------------------------------

class Collection final : public Unknown<IMMDeviceCollection> {
public:
    void add(IMMDevice* device) { devices_.push_back(device); }  // takes the reference
    HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = static_cast<UINT>(devices_.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Item(UINT index, IMMDevice** device) override {
        if (!device) return E_POINTER;
        *device = nullptr;
        if (index >= devices_.size()) return E_INVALIDARG;
        *device = devices_[index];
        (*device)->AddRef();
        return S_OK;
    }

private:
    ~Collection() override {
        for (IMMDevice* d : devices_) d->Release();
    }
    std::vector<IMMDevice*> devices_;
};

// Appends the host's devices of one flow; returns the result of the host's call.
HRESULT append_host(IMMDeviceEnumerator* host, EDataFlow flow, DWORD mask, Collection* out) {
    IMMDeviceCollection* c = nullptr;
    const HRESULT hr = host->EnumAudioEndpoints(flow, mask, &c);
    if (FAILED(hr) || !c) return hr;
    UINT n = 0;
    c->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* d = nullptr;
        if (SUCCEEDED(c->Item(i, &d)) && d) out->add(d);
    }
    c->Release();
    return hr;
}

// ---- IMMDeviceEnumerator --------------------------------------------------------------------------------------

class Enumerator final : public Unknown<IMMDeviceEnumerator> {
public:
    Enumerator(IMMDeviceEnumerator* host, HRESULT host_hr, Mode mode, HostFactory factory)
        : host_(host), host_hr_(host_hr), mode_(mode), factory_(factory) {
        if (host_) host_->AddRef();
    }

    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow flow, DWORD mask, IMMDeviceCollection** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (flow != eRender && flow != eCapture && flow != eAll) return E_INVALIDARG;
        if (mask & ~DEVICE_STATEMASK_ALL) return E_INVALIDARG;
        auto* c = new (std::nothrow) Collection();
        if (!c) return E_OUTOFMEMORY;
        if (flow == eRender || flow == eAll) {
            char reason[96];
            if (!host_render(reason, sizeof reason)) {
                if (mask & DEVICE_STATE_ACTIVE) {
                    if (IMMDevice* d = make_device(reason)) c->add(d);
                }
            } else {
                append_host(host_, eRender, mask, c);
            }
        }
        if (flow == eCapture || flow == eAll) {
            if (IMMDeviceEnumerator* h = host()) append_host(h, eCapture, mask, c);
        }
        *out = c;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (role != eConsole && role != eMultimedia && role != eCommunications) return E_INVALIDARG;
        if (flow == eCapture) {
            IMMDeviceEnumerator* h = host();
            return h ? h->GetDefaultAudioEndpoint(flow, role, out) : kNotFound;
        }
        if (flow != eRender) return E_INVALIDARG;
        char reason[96];
        if (mode_ == Mode::Auto && host_) {
            const HRESULT hr = host_->GetDefaultAudioEndpoint(eRender, role, out);
            if (SUCCEEDED(hr) && *out) return hr;
            if (*out) (*out)->Release();
            *out = nullptr;
            std::snprintf(reason, sizeof reason, "no host render endpoint: GetDefaultAudioEndpoint 0x%08x",
                          static_cast<unsigned>(hr));
        } else {
            why_silent(reason, sizeof reason);
        }
        *out = make_device(reason);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, IMMDevice** out) override {
        if (!id || !out) return E_POINTER;
        *out = nullptr;
        if (wide_equal(id, kDeviceId)) {
            char reason[96];
            why_silent(reason, sizeof reason);
            *out = make_device(reason);
            return *out ? S_OK : E_OUTOFMEMORY;
        }
        IMMDeviceEnumerator* h = host();
        return h ? h->GetDevice(id, out) : kNotFound;
    }
    // Accepted and never called: the silent endpoint never changes, and no host notification is forwarded.
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* client) override {
        return client ? S_OK : E_POINTER;
    }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* client) override {
        return client ? S_OK : E_POINTER;
    }

private:
    ~Enumerator() override {
        if (host_) host_->Release();
    }

    // The host's enumerator for capture questions: made on first use in Mode::Silent.
    IMMDeviceEnumerator* host() {
        AcquireSRWLockExclusive(&lock_);
        if (!host_ && factory_ && !factory_tried_) {
            factory_tried_ = true;
            host_ = factory_();
        }
        IMMDeviceEnumerator* h = host_;
        ReleaseSRWLockExclusive(&lock_);
        return h;
    }

    void why_silent(char* reason, size_t size) const {
        if (mode_ == Mode::Silent) std::snprintf(reason, size, "LEONIDA_AUDIO=silent");
        else if (!host_) std::snprintf(reason, size, "no host device enumerator: 0x%08x",
                                       static_cast<unsigned>(host_hr_));
        else std::snprintf(reason, size, "no host render endpoint");
    }

    // True when render questions go to the host: Mode::Auto and a host with a default render endpoint.
    bool host_render(char* reason, size_t size) {
        if (mode_ == Mode::Auto && host_) {
            IMMDevice* d = nullptr;
            const HRESULT hr = host_->GetDefaultAudioEndpoint(eRender, eConsole, &d);
            if (d) d->Release();
            if (SUCCEEDED(hr) && d) return true;
            std::snprintf(reason, size, "no host render endpoint: GetDefaultAudioEndpoint 0x%08x",
                          static_cast<unsigned>(hr));
            return false;
        }
        why_silent(reason, size);
        return false;
    }

    SRWLOCK lock_ = SRWLOCK_INIT;
    IMMDeviceEnumerator* host_;
    HRESULT host_hr_;
    Mode mode_;
    HostFactory factory_;
    bool factory_tried_ = false;
};

// macOS has no host MMDevice enumerator. A CoreAudio-backed one would be returned here.
IMMDeviceEnumerator* make_host_enumerator() { return nullptr; }

}  // namespace

Mode mode_from_env() {
    const std::string v = rt::gdk::env_or("LEONIDA_AUDIO");
    return !v.empty() && v.size() < 16 && _stricmp(v.c_str(), "silent") == 0 ? Mode::Silent : Mode::Auto;
}

HRESULT serve_enumerator(IMMDeviceEnumerator* host, HRESULT host_hr, Mode mode, HostFactory factory, REFIID riid,
                         void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (mode == Mode::Auto && host) {
        IMMDevice* d = nullptr;
        const HRESULT hr = host->GetDefaultAudioEndpoint(eRender, eConsole, &d);
        if (d) d->Release();
        if (SUCCEEDED(hr) && d) return host->QueryInterface(riid, ppv);  // the host has an output: unchanged
    }
    auto* e = new (std::nothrow) Enumerator(host, host ? S_OK : host_hr, mode, factory);
    if (!e) return E_OUTOFMEMORY;
    const HRESULT hr = e->QueryInterface(riid, ppv);
    e->Release();
    return hr;
}

HRESULT create_enumerator(DWORD, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    const Mode mode = mode_from_env();
    if (mode == Mode::Silent) return serve_enumerator(nullptr, S_OK, mode, &make_host_enumerator, riid, ppv);
    return serve_enumerator(nullptr, REGDB_E_CLASSNOTREG, mode, nullptr, riid, ppv);
}

uint32_t devices_served() { return static_cast<uint32_t>(InterlockedCompareExchange(&g_served, 0, 0)); }

}  // namespace rt::silent_audio
