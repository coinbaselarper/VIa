#pragma once
#include <windows.h>
#include <mmreg.h>
#include <audiosessiontypes.h>
#include <unknwn.h>

#ifndef _REFERENCE_TIME_
#define _REFERENCE_TIME_
typedef LONGLONG REFERENCE_TIME;
#endif

#define AUDCLNT_ERR_BASE_ 0x88890000L
#define AUDCLNT_E_NOT_INITIALIZED _HRESULT_TYPEDEF_(0x88890001L)
#define AUDCLNT_E_ALREADY_INITIALIZED _HRESULT_TYPEDEF_(0x88890002L)
#define AUDCLNT_E_WRONG_ENDPOINT_TYPE _HRESULT_TYPEDEF_(0x88890003L)
#define AUDCLNT_E_DEVICE_INVALIDATED _HRESULT_TYPEDEF_(0x88890004L)
#define AUDCLNT_E_NOT_STOPPED _HRESULT_TYPEDEF_(0x88890005L)
#define AUDCLNT_E_BUFFER_TOO_LARGE _HRESULT_TYPEDEF_(0x88890006L)
#define AUDCLNT_E_OUT_OF_ORDER _HRESULT_TYPEDEF_(0x88890007L)
#define AUDCLNT_E_UNSUPPORTED_FORMAT _HRESULT_TYPEDEF_(0x88890008L)
#define AUDCLNT_E_INVALID_SIZE _HRESULT_TYPEDEF_(0x88890009L)
#define AUDCLNT_E_DEVICE_IN_USE _HRESULT_TYPEDEF_(0x8889000AL)
#define AUDCLNT_E_BUFFER_OPERATION_PENDING _HRESULT_TYPEDEF_(0x8889000BL)
#define AUDCLNT_E_THREAD_NOT_REGISTERED _HRESULT_TYPEDEF_(0x8889000CL)
#define AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED _HRESULT_TYPEDEF_(0x8889000EL)
#define AUDCLNT_E_ENDPOINT_CREATE_FAILED _HRESULT_TYPEDEF_(0x8889000FL)
#define AUDCLNT_E_SERVICE_NOT_RUNNING _HRESULT_TYPEDEF_(0x88890010L)
#define AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED _HRESULT_TYPEDEF_(0x88890011L)
#define AUDCLNT_E_EXCLUSIVE_MODE_ONLY _HRESULT_TYPEDEF_(0x88890012L)
#define AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL _HRESULT_TYPEDEF_(0x88890013L)
#define AUDCLNT_E_EVENTHANDLE_NOT_SET _HRESULT_TYPEDEF_(0x88890014L)
#define AUDCLNT_E_INCORRECT_BUFFER_SIZE _HRESULT_TYPEDEF_(0x88890015L)
#define AUDCLNT_E_BUFFER_SIZE_ERROR _HRESULT_TYPEDEF_(0x88890016L)
#define AUDCLNT_E_CPUUSAGE_EXCEEDED _HRESULT_TYPEDEF_(0x88890017L)
#define AUDCLNT_E_BUFFER_ERROR _HRESULT_TYPEDEF_(0x88890018L)
#define AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED _HRESULT_TYPEDEF_(0x88890019L)
#define AUDCLNT_E_INVALID_DEVICE_PERIOD _HRESULT_TYPEDEF_(0x88890020L)
#define AUDCLNT_S_BUFFER_EMPTY _HRESULT_TYPEDEF_(0x08890001L)
#define AUDCLNT_S_THREAD_ALREADY_REGISTERED _HRESULT_TYPEDEF_(0x08890002L)
#define AUDCLNT_S_POSITION_STALLED _HRESULT_TYPEDEF_(0x08890003L)

typedef enum _AUDCLNT_BUFFERFLAGS {
    AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY = 0x1,
    AUDCLNT_BUFFERFLAGS_SILENT = 0x2,
    AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR = 0x4
} AUDCLNT_BUFFERFLAGS;

#define AUDIOCLOCK_CHARACTERISTIC_FIXED_FREQ 0x00000001

typedef struct AudioClientProperties {
    UINT32 cbSize;
    BOOL bIsOffload;
    AUDIO_STREAM_CATEGORY eCategory;
    AUDCLNT_STREAMOPTIONS Options;
} AudioClientProperties;

struct IAudioClient : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE ShareMode, DWORD StreamFlags,
                                                 REFERENCE_TIME hnsBufferDuration, REFERENCE_TIME hnsPeriodicity,
                                                 const WAVEFORMATEX* pFormat, LPCGUID AudioSessionGuid) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* pNumBufferFrames) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* phnsLatency) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* pNumPaddingFrames) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE ShareMode, const WAVEFORMATEX* pFormat,
                                                        WAVEFORMATEX** ppClosestMatch) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** ppDeviceFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* phnsDefaultDevicePeriod,
                                                      REFERENCE_TIME* phnsMinimumDevicePeriod) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE eventHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetService(REFIID riid, void** ppv) = 0;
};
__CRT_UUID_DECL(IAudioClient, 0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2)

struct IAudioClient2 : public IAudioClient {
    virtual HRESULT STDMETHODCALLTYPE IsOffloadCapable(AUDIO_STREAM_CATEGORY Category, BOOL* pbOffloadCapable) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetClientProperties(const AudioClientProperties* pProperties) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBufferSizeLimits(const WAVEFORMATEX* pFormat, BOOL bEventDriven,
                                                          REFERENCE_TIME* phnsMinBufferDuration,
                                                          REFERENCE_TIME* phnsMaxBufferDuration) = 0;
};
__CRT_UUID_DECL(IAudioClient2, 0x726778cd, 0xf60a, 0x4eda, 0x82, 0xde, 0xe4, 0x76, 0x10, 0xcd, 0x78, 0xaa)

struct IAudioClient3 : public IAudioClient2 {
    virtual HRESULT STDMETHODCALLTYPE GetSharedModeEnginePeriod(const WAVEFORMATEX* pFormat,
                                                                UINT32* pDefaultPeriodInFrames,
                                                                UINT32* pFundamentalPeriodInFrames,
                                                                UINT32* pMinPeriodInFrames,
                                                                UINT32* pMaxPeriodInFrames) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSharedModeEnginePeriod(WAVEFORMATEX** ppFormat,
                                                                       UINT32* pCurrentPeriodInFrames) = 0;
    virtual HRESULT STDMETHODCALLTYPE InitializeSharedAudioStream(DWORD StreamFlags, UINT32 PeriodInFrames,
                                                                  const WAVEFORMATEX* pFormat,
                                                                  LPCGUID AudioSessionGuid) = 0;
};
__CRT_UUID_DECL(IAudioClient3, 0x7ed4ee07, 0x8e67, 0x4cd4, 0x8c, 0x1a, 0x2b, 0x7a, 0x59, 0x87, 0xad, 0x42)

struct IAudioRenderClient : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 NumFramesRequested, BYTE** ppData) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 NumFramesWritten, DWORD dwFlags) = 0;
};
__CRT_UUID_DECL(IAudioRenderClient, 0xf294acfc, 0x3146, 0x4483, 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2)

struct IAudioClock : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* pu64Frequency) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPosition(UINT64* pu64Position, UINT64* pu64QPCPosition) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* pdwCharacteristics) = 0;
};
__CRT_UUID_DECL(IAudioClock, 0xcd63314f, 0x3fba, 0x4a1b, 0x81, 0x2c, 0xef, 0x96, 0x35, 0x87, 0x28, 0xe7)

struct IAudioStreamVolume : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetChannelCount(UINT32* pdwCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetChannelVolume(UINT32 dwIndex, const float fLevel) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelVolume(UINT32 dwIndex, float* pfLevel) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAllVolumes(UINT32 dwCount, const float* pfVolumes) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAllVolumes(UINT32 dwCount, float* pfVolumes) = 0;
};
__CRT_UUID_DECL(IAudioStreamVolume, 0x93014887, 0x242d, 0x4068, 0x8a, 0x15, 0xcf, 0x5e, 0x93, 0xb9, 0x0f, 0xe3)

struct ISimpleAudioVolume : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetMasterVolume(float fLevel, LPCGUID EventContext) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMasterVolume(float* pfLevel) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMute(const BOOL bMute, LPCGUID EventContext) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMute(BOOL* pbMute) = 0;
};
__CRT_UUID_DECL(ISimpleAudioVolume, 0x87ce5498, 0x68d6, 0x44e5, 0x92, 0x15, 0x6d, 0xa4, 0x7e, 0xf8, 0x83, 0xd8)
