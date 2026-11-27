#pragma once
#include <windows.h>
#include <mmreg.h>
#include <audioclient.h>
#include <propidl.h>

#define SPTLAUDCLNT_E_DESTROYED _HRESULT_TYPEDEF_(0x889A0000L)
#define SPTLAUDCLNT_E_OUT_OF_ORDER _HRESULT_TYPEDEF_(0x889A0001L)
#define SPTLAUDCLNT_E_RESOURCES_INVALIDATED _HRESULT_TYPEDEF_(0x889A0002L)
#define SPTLAUDCLNT_E_NO_MORE_OBJECTS _HRESULT_TYPEDEF_(0x889A0003L)
#define SPTLAUDCLNT_E_PROPERTY_NOT_SUPPORTED _HRESULT_TYPEDEF_(0x889A0004L)
#define SPTLAUDCLNT_E_ERRORS_IN_OBJECT_CALLS _HRESULT_TYPEDEF_(0x889A0005L)
#define SPTLAUDCLNT_E_METADATA_FORMAT_NOT_SUPPORTED _HRESULT_TYPEDEF_(0x889A0006L)
#define SPTLAUDCLNT_E_STREAM_NOT_AVAILABLE _HRESULT_TYPEDEF_(0x889A0007L)
#define SPTLAUDCLNT_E_INVALID_LICENSE _HRESULT_TYPEDEF_(0x889A0009L)
#define SPTLAUDCLNT_E_STREAM_NOT_STOPPED _HRESULT_TYPEDEF_(0x889A000BL)
#define SPTLAUDCLNT_E_STATIC_OBJECT_NOT_AVAILABLE _HRESULT_TYPEDEF_(0x889A000CL)
#define SPTLAUDCLNT_E_OBJECT_ALREADY_ACTIVE _HRESULT_TYPEDEF_(0x889A000DL)
#define SPTLAUDCLNT_E_INTERNAL _HRESULT_TYPEDEF_(0x889A000EL)

typedef enum {
    AudioObjectType_None = 0,
    AudioObjectType_Dynamic = 0x1,
    AudioObjectType_FrontLeft = 0x2,
    AudioObjectType_FrontRight = 0x4,
    AudioObjectType_FrontCenter = 0x8,
    AudioObjectType_LowFrequency = 0x10,
    AudioObjectType_SideLeft = 0x20,
    AudioObjectType_SideRight = 0x40,
    AudioObjectType_BackLeft = 0x80,
    AudioObjectType_BackRight = 0x100,
    AudioObjectType_TopFrontLeft = 0x200,
    AudioObjectType_TopFrontRight = 0x400,
    AudioObjectType_TopBackLeft = 0x800,
    AudioObjectType_TopBackRight = 0x1000,
    AudioObjectType_BottomFrontLeft = 0x2000,
    AudioObjectType_BottomFrontRight = 0x4000,
    AudioObjectType_BottomBackLeft = 0x8000,
    AudioObjectType_BottomBackRight = 0x10000,
    AudioObjectType_BackCenter = 0x20000
} AudioObjectType;

struct ISpatialAudioObjectRenderStreamNotify;

typedef struct SpatialAudioObjectRenderStreamActivationParams {
    const WAVEFORMATEX* ObjectFormat;
    AudioObjectType StaticObjectTypeMask;
    UINT32 MinDynamicObjectCount;
    UINT32 MaxDynamicObjectCount;
    AUDIO_STREAM_CATEGORY Category;
    HANDLE EventHandle;
    ISpatialAudioObjectRenderStreamNotify* NotifyObject;
} SpatialAudioObjectRenderStreamActivationParams;

struct IAudioFormatEnumerator : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT32* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFormat(UINT32 index, WAVEFORMATEX** format) = 0;
};
__CRT_UUID_DECL(IAudioFormatEnumerator, 0xdcdaa858, 0x895a, 0x4a22, 0xa5, 0xeb, 0x67, 0xbd, 0xa5, 0x06, 0x09, 0x6d)

struct ISpatialAudioObjectBase : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** buffer, UINT32* bufferLength) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndOfStream(UINT32 frameCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsActive(BOOL* isActive) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAudioObjectType(AudioObjectType* audioObjectType) = 0;
};
__CRT_UUID_DECL(ISpatialAudioObjectBase, 0xcce0b8f2, 0x8d4d, 0x4efb, 0xa8, 0xcf, 0x3d, 0x6e, 0xcf, 0x1c, 0x30, 0xe0)

struct ISpatialAudioObject : public ISpatialAudioObjectBase {
    virtual HRESULT STDMETHODCALLTYPE SetPosition(float x, float y, float z) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVolume(float volume) = 0;
};
__CRT_UUID_DECL(ISpatialAudioObject, 0xdde28967, 0x521b, 0x46e5, 0x8f, 0x00, 0xbd, 0x6f, 0x2f, 0x8b, 0xd9, 0x63)

struct ISpatialAudioObjectRenderStreamBase : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAvailableDynamicObjectCount(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetService(REFIID riid, void** service) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginUpdatingAudioObjects(UINT32* availableDynamicObjectCount,
                                                                UINT32* frameCountPerBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndUpdatingAudioObjects(void) = 0;
};
__CRT_UUID_DECL(ISpatialAudioObjectRenderStreamBase, 0xfeaaf403, 0xc1d8, 0x450d, 0xaa, 0x05, 0xe0, 0xcc, 0xee, 0x75, 0x02, 0xa8)

struct ISpatialAudioObjectRenderStream : public ISpatialAudioObjectRenderStreamBase {
    virtual HRESULT STDMETHODCALLTYPE ActivateSpatialAudioObject(AudioObjectType type, ISpatialAudioObject** audioObject) = 0;
};
__CRT_UUID_DECL(ISpatialAudioObjectRenderStream, 0xbab5f473, 0xb423, 0x477b, 0x85, 0xf5, 0xb5, 0xa3, 0x32, 0xa0, 0x41, 0x53)

struct ISpatialAudioClient : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetStaticObjectPosition(AudioObjectType type, float* x, float* y, float* z) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNativeStaticObjectTypeMask(AudioObjectType* mask) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMaxDynamicObjectCount(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSupportedAudioObjectFormatEnumerator(IAudioFormatEnumerator** enumerator) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMaxFrameCount(const WAVEFORMATEX* objectFormat, UINT32* frameCountPerBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsAudioObjectFormatSupported(const WAVEFORMATEX* objectFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsSpatialAudioStreamAvailable(REFIID streamUuid, const PROPVARIANT* auxiliaryInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE ActivateSpatialAudioStream(const PROPVARIANT* activationParams, REFIID riid,
                                                                 void** stream) = 0;
};
__CRT_UUID_DECL(ISpatialAudioClient, 0xbbf8e066, 0xaaaa, 0x49be, 0x9a, 0x4d, 0xfd, 0x2a, 0x85, 0x8e, 0xa2, 0x7f)

struct ISpatialAudioClient2 : public ISpatialAudioClient {
    virtual HRESULT STDMETHODCALLTYPE IsOffloadCapable(AUDIO_STREAM_CATEGORY category, BOOL* isOffloadCapable) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMaxFrameCountForCategory(AUDIO_STREAM_CATEGORY category, BOOL offloadEnabled,
                                                                  const WAVEFORMATEX* objectFormat,
                                                                  UINT32* frameCountPerBuffer) = 0;
};
__CRT_UUID_DECL(ISpatialAudioClient2, 0xcaabe452, 0xa66a, 0x4bee, 0xa9, 0x3e, 0xe3, 0x20, 0x46, 0x3f, 0x6a, 0x53)
