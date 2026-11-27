#pragma once
#include <windows.h>
#include <objbase.h>
#include <propsys.h>

typedef enum { eRender = 0, eCapture = 1, eAll = 2, EDataFlow_enum_count = 3 } EDataFlow;
typedef enum { eConsole = 0, eMultimedia = 1, eCommunications = 2, ERole_enum_count = 3 } ERole;

#define DEVICE_STATE_ACTIVE 0x00000001
#define DEVICE_STATE_DISABLED 0x00000002
#define DEVICE_STATE_NOTPRESENT 0x00000004
#define DEVICE_STATE_UNPLUGGED 0x00000008
#define DEVICE_STATEMASK_ALL 0x0000000f

struct IMMDevice;
struct IMMDeviceCollection;

struct IMMNotificationClient : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) = 0;
};
__CRT_UUID_DECL(IMMNotificationClient, 0x7991eec9, 0x7e89, 0x4d85, 0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0)

struct IMMDevice : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD dwClsCtx, PROPVARIANT* pActivationParams,
                                               void** ppInterface) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD stgmAccess, IPropertyStore** ppProperties) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetId(LPWSTR* ppstrId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetState(DWORD* pdwState) = 0;
};
__CRT_UUID_DECL(IMMDevice, 0xd666063f, 0x1587, 0x4e43, 0x81, 0xf1, 0xb9, 0x48, 0xe8, 0x07, 0x36, 0x3f)

struct IMMDeviceCollection : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT* pcDevices) = 0;
    virtual HRESULT STDMETHODCALLTYPE Item(UINT nDevice, IMMDevice** ppDevice) = 0;
};
__CRT_UUID_DECL(IMMDeviceCollection, 0x0bd7a1be, 0x7a1a, 0x44db, 0x83, 0x97, 0xcc, 0x53, 0x92, 0x38, 0x7b, 0x5e)

struct IMMEndpoint : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetDataFlow(EDataFlow* pDataFlow) = 0;
};
__CRT_UUID_DECL(IMMEndpoint, 0x1be09788, 0x6894, 0x4089, 0x85, 0x86, 0x9a, 0x2a, 0x6c, 0x26, 0x5a, 0xc5)

struct IMMDeviceEnumerator : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow dataFlow, DWORD dwStateMask,
                                                         IMMDeviceCollection** ppDevices) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role,
                                                              IMMDevice** ppEndpoint) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR pwstrId, IMMDevice** ppDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* pClient) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* pClient) = 0;
};
__CRT_UUID_DECL(IMMDeviceEnumerator, 0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6)
