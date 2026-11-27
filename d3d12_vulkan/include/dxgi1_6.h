#pragma once

/*
 * DXGI 1.0-1.6 interfaces for the macOS host (DirectX-Headers ships none). Method order follows the Windows SDK
 * dxgi.idl .. dxgi1_6.idl; IIDs are checked against Wine's and DXMT's dxgi.dll (tools/check_dxgi_iids.py).
 */

#include <windows.h>
#include <unknwn.h>
#include <dxgicommon.h>
#include <dxgiformat.h>

#ifndef DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE
#define DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE _HRESULT_TYPEDEF_(0x887A000CL)
#endif
#ifndef DXGI_ERROR_REMOTE_CLIENT_DISCONNECTED
#define DXGI_ERROR_REMOTE_CLIENT_DISCONNECTED _HRESULT_TYPEDEF_(0x887A0023L)
#endif
#ifndef DXGI_ERROR_REMOTE_OUTOFMEMORY
#define DXGI_ERROR_REMOTE_OUTOFMEMORY _HRESULT_TYPEDEF_(0x887A0024L)
#endif
#ifndef DXGI_ERROR_MODE_CHANGE_IN_PROGRESS
#define DXGI_ERROR_MODE_CHANGE_IN_PROGRESS _HRESULT_TYPEDEF_(0x887A0025L)
#endif
#ifndef DXGI_ERROR_WAIT_TIMEOUT
#define DXGI_ERROR_WAIT_TIMEOUT _HRESULT_TYPEDEF_(0x887A0027L)
#endif
#ifndef DXGI_ERROR_SESSION_DISCONNECTED
#define DXGI_ERROR_SESSION_DISCONNECTED _HRESULT_TYPEDEF_(0x887A0028L)
#endif
#ifndef DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE
#define DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE _HRESULT_TYPEDEF_(0x887A0029L)
#endif
#ifndef DXGI_ERROR_CANNOT_PROTECT_CONTENT
#define DXGI_ERROR_CANNOT_PROTECT_CONTENT _HRESULT_TYPEDEF_(0x887A002AL)
#endif
#ifndef DXGI_ERROR_ACCESS_DENIED
#define DXGI_ERROR_ACCESS_DENIED _HRESULT_TYPEDEF_(0x887A002BL)
#endif
#ifndef DXGI_ERROR_NAME_ALREADY_EXISTS
#define DXGI_ERROR_NAME_ALREADY_EXISTS _HRESULT_TYPEDEF_(0x887A002CL)
#endif
#ifndef DXGI_ERROR_NOT_CURRENT
#define DXGI_ERROR_NOT_CURRENT _HRESULT_TYPEDEF_(0x887A002EL)
#endif
#ifndef DXGI_ERROR_HW_PROTECTION_OUTOFMEMORY
#define DXGI_ERROR_HW_PROTECTION_OUTOFMEMORY _HRESULT_TYPEDEF_(0x887A0030L)
#endif
#ifndef DXGI_ERROR_DYNAMIC_CODE_POLICY_VIOLATION
#define DXGI_ERROR_DYNAMIC_CODE_POLICY_VIOLATION _HRESULT_TYPEDEF_(0x887A0031L)
#endif
#ifndef DXGI_ERROR_NON_COMPOSITED_UI
#define DXGI_ERROR_NON_COMPOSITED_UI _HRESULT_TYPEDEF_(0x887A0032L)
#endif
#ifndef DXGI_ERROR_CACHE_CORRUPT
#define DXGI_ERROR_CACHE_CORRUPT _HRESULT_TYPEDEF_(0x887A0033L)
#endif
#ifndef DXGI_ERROR_CACHE_FULL
#define DXGI_ERROR_CACHE_FULL _HRESULT_TYPEDEF_(0x887A0034L)
#endif
#ifndef DXGI_ERROR_CACHE_HASH_COLLISION
#define DXGI_ERROR_CACHE_HASH_COLLISION _HRESULT_TYPEDEF_(0x887A0035L)
#endif
#ifndef DXGI_ERROR_ALREADY_EXISTS
#define DXGI_ERROR_ALREADY_EXISTS _HRESULT_TYPEDEF_(0x887A0036L)
#endif
#ifndef DXGI_STATUS_CLIPPED
#define DXGI_STATUS_CLIPPED _HRESULT_TYPEDEF_(0x087A0002L)
#define DXGI_STATUS_NO_REDIRECTION _HRESULT_TYPEDEF_(0x087A0004L)
#define DXGI_STATUS_NO_DESKTOP_ACCESS _HRESULT_TYPEDEF_(0x087A0005L)
#define DXGI_STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE _HRESULT_TYPEDEF_(0x087A0006L)
#define DXGI_STATUS_MODE_CHANGED _HRESULT_TYPEDEF_(0x087A0007L)
#define DXGI_STATUS_MODE_CHANGE_IN_PROGRESS _HRESULT_TYPEDEF_(0x087A0008L)
#define DXGI_STATUS_UNOCCLUDED _HRESULT_TYPEDEF_(0x087A0009L)
#define DXGI_STATUS_DDA_WAS_STILL_DRAWING _HRESULT_TYPEDEF_(0x087A000AL)
#define DXGI_STATUS_PRESENT_REQUIRED _HRESULT_TYPEDEF_(0x087A002FL)
#endif

#define DXGI_CPU_ACCESS_NONE 0
#define DXGI_CPU_ACCESS_DYNAMIC 1
#define DXGI_CPU_ACCESS_READ_WRITE 2
#define DXGI_CPU_ACCESS_SCRATCH 3
#define DXGI_CPU_ACCESS_FIELD 15
#define DXGI_USAGE_SHADER_INPUT 0x00000010UL
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x00000020UL
#define DXGI_USAGE_BACK_BUFFER 0x00000040UL
#define DXGI_USAGE_SHARED 0x00000080UL
#define DXGI_USAGE_READ_ONLY 0x00000100UL
#define DXGI_USAGE_DISCARD_ON_PRESENT 0x00000200UL
#define DXGI_USAGE_UNORDERED_ACCESS 0x00000400UL
typedef UINT DXGI_USAGE;

#define DXGI_RESOURCE_PRIORITY_MINIMUM 0x28000000
#define DXGI_RESOURCE_PRIORITY_LOW 0x50000000
#define DXGI_RESOURCE_PRIORITY_NORMAL 0x78000000
#define DXGI_RESOURCE_PRIORITY_HIGH 0xa0000000
#define DXGI_RESOURCE_PRIORITY_MAXIMUM 0xc8000000

#define DXGI_MAP_READ 1UL
#define DXGI_MAP_WRITE 2UL
#define DXGI_MAP_DISCARD 4UL

#define DXGI_ENUM_MODES_INTERLACED 1UL
#define DXGI_ENUM_MODES_SCALING 2UL
#define DXGI_ENUM_MODES_STEREO 4UL
#define DXGI_ENUM_MODES_DISABLED_STEREO 8UL

#define DXGI_PRESENT_TEST 0x00000001UL
#define DXGI_PRESENT_DO_NOT_SEQUENCE 0x00000002UL
#define DXGI_PRESENT_RESTART 0x00000004UL
#define DXGI_PRESENT_DO_NOT_WAIT 0x00000008UL
#define DXGI_PRESENT_STEREO_PREFER_RIGHT 0x00000010UL
#define DXGI_PRESENT_STEREO_TEMPORARY_MONO 0x00000020UL
#define DXGI_PRESENT_RESTRICT_TO_OUTPUT 0x00000040UL
#define DXGI_PRESENT_USE_DURATION 0x00000100UL
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL

#define DXGI_MWA_NO_WINDOW_CHANGES (1 << 0)
#define DXGI_MWA_NO_ALT_ENTER (1 << 1)
#define DXGI_MWA_NO_PRINT_SCREEN (1 << 2)
#define DXGI_MWA_VALID 0x7

#define DXGI_MAX_SWAP_CHAIN_BUFFERS 16
#define DXGI_CREATE_FACTORY_DEBUG 0x1

/* dxgitype.h */
typedef struct DXGI_RGB {
    float Red;
    float Green;
    float Blue;
} DXGI_RGB;

#ifndef D3DCOLORVALUE_DEFINED
typedef struct _D3DCOLORVALUE {
    float r;
    float g;
    float b;
    float a;
} D3DCOLORVALUE;
#define D3DCOLORVALUE_DEFINED
#endif
typedef D3DCOLORVALUE DXGI_RGBA;

typedef struct DXGI_GAMMA_CONTROL {
    DXGI_RGB Scale;
    DXGI_RGB Offset;
    DXGI_RGB GammaCurve[1025];
} DXGI_GAMMA_CONTROL;

typedef struct DXGI_GAMMA_CONTROL_CAPABILITIES {
    BOOL ScaleAndOffsetSupported;
    float MaxConvertedValue;
    float MinConvertedValue;
    UINT NumGammaControlPoints;
    float ControlPointPositions[1025];
} DXGI_GAMMA_CONTROL_CAPABILITIES;

typedef enum DXGI_MODE_SCANLINE_ORDER {
    DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED = 0,
    DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE = 1,
    DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST = 2,
    DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST = 3
} DXGI_MODE_SCANLINE_ORDER;

typedef enum DXGI_MODE_SCALING {
    DXGI_MODE_SCALING_UNSPECIFIED = 0,
    DXGI_MODE_SCALING_CENTERED = 1,
    DXGI_MODE_SCALING_STRETCHED = 2
} DXGI_MODE_SCALING;

typedef enum DXGI_MODE_ROTATION {
    DXGI_MODE_ROTATION_UNSPECIFIED = 0,
    DXGI_MODE_ROTATION_IDENTITY = 1,
    DXGI_MODE_ROTATION_ROTATE90 = 2,
    DXGI_MODE_ROTATION_ROTATE180 = 3,
    DXGI_MODE_ROTATION_ROTATE270 = 4
} DXGI_MODE_ROTATION;

typedef struct DXGI_MODE_DESC {
    UINT Width;
    UINT Height;
    DXGI_RATIONAL RefreshRate;
    DXGI_FORMAT Format;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
} DXGI_MODE_DESC;

typedef struct DXGI_JPEG_DC_HUFFMAN_TABLE {
    BYTE CodeCounts[12];
    BYTE CodeValues[12];
} DXGI_JPEG_DC_HUFFMAN_TABLE;
typedef struct DXGI_JPEG_AC_HUFFMAN_TABLE {
    BYTE CodeCounts[16];
    BYTE CodeValues[162];
} DXGI_JPEG_AC_HUFFMAN_TABLE;
typedef struct DXGI_JPEG_QUANTIZATION_TABLE {
    BYTE Elements[64];
} DXGI_JPEG_QUANTIZATION_TABLE;

/* dxgi.h */
typedef struct DXGI_FRAME_STATISTICS {
    UINT PresentCount;
    UINT PresentRefreshCount;
    UINT SyncRefreshCount;
    LARGE_INTEGER SyncQPCTime;
    LARGE_INTEGER SyncGPUTime;
} DXGI_FRAME_STATISTICS;

typedef struct DXGI_MAPPED_RECT {
    INT Pitch;
    BYTE* pBits;
} DXGI_MAPPED_RECT;

typedef struct DXGI_ADAPTER_DESC {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
} DXGI_ADAPTER_DESC;

typedef struct DXGI_OUTPUT_DESC {
    WCHAR DeviceName[32];
    RECT DesktopCoordinates;
    BOOL AttachedToDesktop;
    DXGI_MODE_ROTATION Rotation;
    HMONITOR Monitor;
} DXGI_OUTPUT_DESC;

typedef struct DXGI_SHARED_RESOURCE {
    HANDLE Handle;
} DXGI_SHARED_RESOURCE;

typedef enum DXGI_RESIDENCY {
    DXGI_RESIDENCY_FULLY_RESIDENT = 1,
    DXGI_RESIDENCY_RESIDENT_IN_SHARED_MEMORY = 2,
    DXGI_RESIDENCY_EVICTED_TO_DISK = 3
} DXGI_RESIDENCY;

typedef struct DXGI_SURFACE_DESC {
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
} DXGI_SURFACE_DESC;

typedef enum DXGI_SWAP_EFFECT {
    DXGI_SWAP_EFFECT_DISCARD = 0,
    DXGI_SWAP_EFFECT_SEQUENTIAL = 1,
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
    DXGI_SWAP_EFFECT_FLIP_DISCARD = 4
} DXGI_SWAP_EFFECT;

typedef enum DXGI_SWAP_CHAIN_FLAG {
    DXGI_SWAP_CHAIN_FLAG_NONPREROTATED = 1,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 2,
    DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE = 4,
    DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT = 8,
    DXGI_SWAP_CHAIN_FLAG_RESTRICT_SHARED_RESOURCE_DRIVER = 16,
    DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY = 32,
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT = 64,
    DXGI_SWAP_CHAIN_FLAG_FOREGROUND_LAYER = 128,
    DXGI_SWAP_CHAIN_FLAG_FULLSCREEN_VIDEO = 256,
    DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO = 512,
    DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED = 1024,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 2048,
    DXGI_SWAP_CHAIN_FLAG_RESTRICTED_TO_ALL_HOLOGRAPHIC_DISPLAYS = 4096
} DXGI_SWAP_CHAIN_FLAG;

typedef struct DXGI_SWAP_CHAIN_DESC {
    DXGI_MODE_DESC BufferDesc;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE BufferUsage;
    UINT BufferCount;
    HWND OutputWindow;
    BOOL Windowed;
    DXGI_SWAP_EFFECT SwapEffect;
    UINT Flags;
} DXGI_SWAP_CHAIN_DESC;

typedef enum DXGI_ADAPTER_FLAG {
    DXGI_ADAPTER_FLAG_NONE = 0,
    DXGI_ADAPTER_FLAG_REMOTE = 1,
    DXGI_ADAPTER_FLAG_SOFTWARE = 2,
    DXGI_ADAPTER_FLAG_FORCE_DWORD = 0xffffffff
} DXGI_ADAPTER_FLAG;

typedef struct DXGI_ADAPTER_DESC1 {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
    UINT Flags;
} DXGI_ADAPTER_DESC1;

typedef struct DXGI_DISPLAY_COLOR_SPACE {
    FLOAT PrimaryCoordinates[8][2];
    FLOAT WhitePoints[16][2];
} DXGI_DISPLAY_COLOR_SPACE;

#ifdef __cplusplus

struct IDXGIOutput;
struct IDXGIAdapter;
struct IDXGIAdapter1;
struct IDXGISurface;
struct IDXGISurface2;
struct IDXGISwapChain;
struct IDXGISwapChain1;
struct IDXGIResource;
struct IDXGIOutputDuplication;

extern "C++" {

MIDL_INTERFACE("aec22fb8-76f3-4639-9be0-28eb43a67a2e")
IDXGIObject : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) = 0;
};

MIDL_INTERFACE("3d3e0379-f9de-4d58-bb6c-18d62992f1a6")
IDXGIDeviceSubObject : public IDXGIObject {
public:
    virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) = 0;
};

MIDL_INTERFACE("035f3ab4-482e-4e50-b41f-8a7f8bd8960b")
IDXGIResource : public IDXGIDeviceSubObject {
public:
    virtual HRESULT STDMETHODCALLTYPE GetSharedHandle(HANDLE* pSharedHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetUsage(DXGI_USAGE* pUsage) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetEvictionPriority(UINT* pEvictionPriority) = 0;
};

MIDL_INTERFACE("9d8e1289-d7b3-465f-8126-250e349af85d")
IDXGIKeyedMutex : public IDXGIDeviceSubObject {
public:
    virtual HRESULT STDMETHODCALLTYPE AcquireSync(UINT64 Key, DWORD dwMilliseconds) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseSync(UINT64 Key) = 0;
};

MIDL_INTERFACE("cafcb56c-6ac3-4889-bf47-9e23bbd260ec")
IDXGISurface : public IDXGIDeviceSubObject {
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SURFACE_DESC* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE Map(DXGI_MAPPED_RECT* pLockedRect, UINT MapFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE Unmap(void) = 0;
};

MIDL_INTERFACE("4AE63092-6327-4c1b-80AE-BFE12EA32B86")
IDXGISurface1 : public IDXGISurface {
public:
    virtual HRESULT STDMETHODCALLTYPE GetDC(BOOL Discard, HDC* phdc) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseDC(RECT* pDirtyRect) = 0;
};

MIDL_INTERFACE("2411e7e1-12ac-4ccf-bd14-9798e8534dc0")
IDXGIAdapter : public IDXGIObject {
public:
    virtual HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput** ppOutput) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion) = 0;
};

MIDL_INTERFACE("ae02eedb-c735-4690-8d52-5a8dc20213aa")
IDXGIOutput : public IDXGIObject {
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT EnumFormat, UINT Flags, UINT* pNumModes,
                                                         DXGI_MODE_DESC* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* pModeToMatch,
                                                              DXGI_MODE_DESC* pClosestMatch,
                                                              IUnknown* pConcernedDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE WaitForVBlank(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown* pDevice, BOOL Exclusive) = 0;
    virtual void STDMETHODCALLTYPE ReleaseOwnership(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* pGammaCaps) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL* pArray) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL* pArray) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface* pScanoutSurface) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface* pDestination) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) = 0;
};

MIDL_INTERFACE("310d36a0-d2e7-4c0a-aa04-6a9d23b8886a")
IDXGISwapChain : public IDXGIDeviceSubObject {
public:
    virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                    UINT SwapChainFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) = 0;
};

MIDL_INTERFACE("7b7166ec-21c7-44ae-b21a-c9ae321ae369")
IDXGIFactory : public IDXGIObject {
public:
    virtual HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) = 0;
    virtual HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* pWindowHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                      IDXGISwapChain** ppSwapChain) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) = 0;
};

MIDL_INTERFACE("54ec77fa-1377-44e6-8c32-88fd5f44c84c")
IDXGIDevice : public IDXGIObject {
public:
    virtual HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** pAdapter) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC* pDesc, UINT NumSurfaces, DXGI_USAGE Usage,
                                                    const DXGI_SHARED_RESOURCE* pSharedResource,
                                                    IDXGISurface** ppSurface) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown* const* ppResources,
                                                             DXGI_RESIDENCY* pResidencyStatus,
                                                             UINT NumResources) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* pPriority) = 0;
};

MIDL_INTERFACE("770aae78-f26f-4dba-a829-253c83d1b387")
IDXGIFactory1 : public IDXGIFactory {
public:
    virtual HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) = 0;
    virtual BOOL STDMETHODCALLTYPE IsCurrent(void) = 0;
};

MIDL_INTERFACE("29038f61-3839-4626-91fd-086879011a05")
IDXGIAdapter1 : public IDXGIAdapter {
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* pDesc) = 0;
};

MIDL_INTERFACE("77db970f-6276-48ba-ba28-070143b4392c")
IDXGIDevice1 : public IDXGIDevice {
public:
    virtual HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) = 0;
};

}  // extern "C++"

__CRT_UUID_DECL(IDXGIObject, 0xaec22fb8, 0x76f3, 0x4639, 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e)
__CRT_UUID_DECL(IDXGIDeviceSubObject, 0x3d3e0379, 0xf9de, 0x4d58, 0xbb, 0x6c, 0x18, 0xd6, 0x29, 0x92, 0xf1, 0xa6)
__CRT_UUID_DECL(IDXGIResource, 0x035f3ab4, 0x482e, 0x4e50, 0xb4, 0x1f, 0x8a, 0x7f, 0x8b, 0xd8, 0x96, 0x0b)
__CRT_UUID_DECL(IDXGIKeyedMutex, 0x9d8e1289, 0xd7b3, 0x465f, 0x81, 0x26, 0x25, 0x0e, 0x34, 0x9a, 0xf8, 0x5d)
__CRT_UUID_DECL(IDXGISurface, 0xcafcb56c, 0x6ac3, 0x4889, 0xbf, 0x47, 0x9e, 0x23, 0xbb, 0xd2, 0x60, 0xec)
__CRT_UUID_DECL(IDXGISurface1, 0x4ae63092, 0x6327, 0x4c1b, 0x80, 0xae, 0xbf, 0xe1, 0x2e, 0xa3, 0x2b, 0x86)
__CRT_UUID_DECL(IDXGIAdapter, 0x2411e7e1, 0x12ac, 0x4ccf, 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0)
__CRT_UUID_DECL(IDXGIOutput, 0xae02eedb, 0xc735, 0x4690, 0x8d, 0x52, 0x5a, 0x8d, 0xc2, 0x02, 0x13, 0xaa)
__CRT_UUID_DECL(IDXGISwapChain, 0x310d36a0, 0xd2e7, 0x4c0a, 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a)
__CRT_UUID_DECL(IDXGIFactory, 0x7b7166ec, 0x21c7, 0x44ae, 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69)
__CRT_UUID_DECL(IDXGIDevice, 0x54ec77fa, 0x1377, 0x44e6, 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c)
__CRT_UUID_DECL(IDXGIFactory1, 0x770aae78, 0xf26f, 0x4dba, 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87)
__CRT_UUID_DECL(IDXGIAdapter1, 0x29038f61, 0x3839, 0x4626, 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05)
__CRT_UUID_DECL(IDXGIDevice1, 0x77db970f, 0x6276, 0x48ba, 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c)

#endif  /* __cplusplus */

/* dxgi1_2.h */
typedef enum DXGI_OUTDUPL_POINTER_SHAPE_TYPE {
    DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME = 0x1,
    DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR = 0x2,
    DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR = 0x4
} DXGI_OUTDUPL_POINTER_SHAPE_TYPE;

typedef struct DXGI_OUTDUPL_MOVE_RECT {
    POINT SourcePoint;
    RECT DestinationRect;
} DXGI_OUTDUPL_MOVE_RECT;

typedef struct DXGI_OUTDUPL_DESC {
    DXGI_MODE_DESC ModeDesc;
    DXGI_MODE_ROTATION Rotation;
    BOOL DesktopImageInSystemMemory;
} DXGI_OUTDUPL_DESC;

typedef struct DXGI_OUTDUPL_POINTER_POSITION {
    POINT Position;
    BOOL Visible;
} DXGI_OUTDUPL_POINTER_POSITION;

typedef struct DXGI_OUTDUPL_POINTER_SHAPE_INFO {
    UINT Type;
    UINT Width;
    UINT Height;
    UINT Pitch;
    POINT HotSpot;
} DXGI_OUTDUPL_POINTER_SHAPE_INFO;

typedef struct DXGI_OUTDUPL_FRAME_INFO {
    LARGE_INTEGER LastPresentTime;
    LARGE_INTEGER LastMouseUpdateTime;
    UINT AccumulatedFrames;
    BOOL RectsCoalesced;
    BOOL ProtectedContentMaskedOut;
    DXGI_OUTDUPL_POINTER_POSITION PointerPosition;
    UINT TotalMetadataBufferSize;
    UINT PointerShapeBufferSize;
} DXGI_OUTDUPL_FRAME_INFO;

typedef enum DXGI_ALPHA_MODE {
    DXGI_ALPHA_MODE_UNSPECIFIED = 0,
    DXGI_ALPHA_MODE_PREMULTIPLIED = 1,
    DXGI_ALPHA_MODE_STRAIGHT = 2,
    DXGI_ALPHA_MODE_IGNORE = 3,
    DXGI_ALPHA_MODE_FORCE_DWORD = 0xffffffff
} DXGI_ALPHA_MODE;

typedef enum DXGI_OFFER_RESOURCE_PRIORITY {
    DXGI_OFFER_RESOURCE_PRIORITY_LOW = 1,
    DXGI_OFFER_RESOURCE_PRIORITY_NORMAL = 2,
    DXGI_OFFER_RESOURCE_PRIORITY_HIGH = 3
} DXGI_OFFER_RESOURCE_PRIORITY;

typedef struct DXGI_MODE_DESC1 {
    UINT Width;
    UINT Height;
    DXGI_RATIONAL RefreshRate;
    DXGI_FORMAT Format;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
    BOOL Stereo;
} DXGI_MODE_DESC1;

typedef enum DXGI_SCALING {
    DXGI_SCALING_STRETCH = 0,
    DXGI_SCALING_NONE = 1,
    DXGI_SCALING_ASPECT_RATIO_STRETCH = 2
} DXGI_SCALING;

typedef struct DXGI_SWAP_CHAIN_DESC1 {
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    BOOL Stereo;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE BufferUsage;
    UINT BufferCount;
    DXGI_SCALING Scaling;
    DXGI_SWAP_EFFECT SwapEffect;
    DXGI_ALPHA_MODE AlphaMode;
    UINT Flags;
} DXGI_SWAP_CHAIN_DESC1;

typedef struct DXGI_SWAP_CHAIN_FULLSCREEN_DESC {
    DXGI_RATIONAL RefreshRate;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
    BOOL Windowed;
} DXGI_SWAP_CHAIN_FULLSCREEN_DESC;

typedef struct DXGI_PRESENT_PARAMETERS {
    UINT DirtyRectsCount;
    RECT* pDirtyRects;
    RECT* pScrollRect;
    POINT* pScrollOffset;
} DXGI_PRESENT_PARAMETERS;

typedef enum DXGI_GRAPHICS_PREEMPTION_GRANULARITY {
    DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY = 0,
    DXGI_GRAPHICS_PREEMPTION_PRIMITIVE_BOUNDARY = 1,
    DXGI_GRAPHICS_PREEMPTION_TRIANGLE_BOUNDARY = 2,
    DXGI_GRAPHICS_PREEMPTION_PIXEL_BOUNDARY = 3,
    DXGI_GRAPHICS_PREEMPTION_INSTRUCTION_BOUNDARY = 4
} DXGI_GRAPHICS_PREEMPTION_GRANULARITY;

typedef enum DXGI_COMPUTE_PREEMPTION_GRANULARITY {
    DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY = 0,
    DXGI_COMPUTE_PREEMPTION_DISPATCH_BOUNDARY = 1,
    DXGI_COMPUTE_PREEMPTION_THREAD_GROUP_BOUNDARY = 2,
    DXGI_COMPUTE_PREEMPTION_THREAD_BOUNDARY = 3,
    DXGI_COMPUTE_PREEMPTION_INSTRUCTION_BOUNDARY = 4
} DXGI_COMPUTE_PREEMPTION_GRANULARITY;

typedef struct DXGI_ADAPTER_DESC2 {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
    UINT Flags;
    DXGI_GRAPHICS_PREEMPTION_GRANULARITY GraphicsPreemptionGranularity;
    DXGI_COMPUTE_PREEMPTION_GRANULARITY ComputePreemptionGranularity;
} DXGI_ADAPTER_DESC2;

/* dxgi1_3.h */
#define DXGI_CREATE_FACTORY_DEBUG 0x1
typedef struct DXGI_MATRIX_3X2_F {
    FLOAT _11;
    FLOAT _12;
    FLOAT _21;
    FLOAT _22;
    FLOAT _31;
    FLOAT _32;
} DXGI_MATRIX_3X2_F;

typedef enum DXGI_OVERLAY_SUPPORT_FLAG {
    DXGI_OVERLAY_SUPPORT_FLAG_DIRECT = 0x1,
    DXGI_OVERLAY_SUPPORT_FLAG_SCALING = 0x2
} DXGI_OVERLAY_SUPPORT_FLAG;

/* dxgi1_4.h */
typedef enum DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG {
    DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT = 0x1,
    DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_OVERLAY_PRESENT = 0x2
} DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG;

typedef enum DXGI_OVERLAY_COLOR_SPACE_SUPPORT_FLAG {
    DXGI_OVERLAY_COLOR_SPACE_SUPPORT_FLAG_PRESENT = 0x1
} DXGI_OVERLAY_COLOR_SPACE_SUPPORT_FLAG;

typedef enum DXGI_MEMORY_SEGMENT_GROUP {
    DXGI_MEMORY_SEGMENT_GROUP_LOCAL = 0,
    DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1
} DXGI_MEMORY_SEGMENT_GROUP;

typedef struct DXGI_QUERY_VIDEO_MEMORY_INFO {
    UINT64 Budget;
    UINT64 CurrentUsage;
    UINT64 AvailableForReservation;
    UINT64 CurrentReservation;
} DXGI_QUERY_VIDEO_MEMORY_INFO;

/* dxgi1_5.h */
typedef enum DXGI_OUTDUPL_FLAG {
    DXGI_OUTDUPL_COMPOSITED_UI_CAPTURE_ONLY = 1
} DXGI_OUTDUPL_FLAG;

typedef enum DXGI_HDR_METADATA_TYPE {
    DXGI_HDR_METADATA_TYPE_NONE = 0,
    DXGI_HDR_METADATA_TYPE_HDR10 = 1,
    DXGI_HDR_METADATA_TYPE_HDR10PLUS = 2
} DXGI_HDR_METADATA_TYPE;

typedef struct DXGI_HDR_METADATA_HDR10 {
    UINT16 RedPrimary[2];
    UINT16 GreenPrimary[2];
    UINT16 BluePrimary[2];
    UINT16 WhitePoint[2];
    UINT MaxMasteringLuminance;
    UINT MinMasteringLuminance;
    UINT16 MaxContentLightLevel;
    UINT16 MaxFrameAverageLightLevel;
} DXGI_HDR_METADATA_HDR10;

typedef enum DXGI_OFFER_RESOURCE_FLAGS {
    DXGI_OFFER_RESOURCE_FLAG_ALLOW_DECOMMIT = 0x1
} DXGI_OFFER_RESOURCE_FLAGS;

typedef enum DXGI_RECLAIM_RESOURCE_RESULTS {
    DXGI_RECLAIM_RESOURCE_RESULT_OK = 0,
    DXGI_RECLAIM_RESOURCE_RESULT_DISCARDED = 1,
    DXGI_RECLAIM_RESOURCE_RESULT_NOT_COMMITTED = 2
} DXGI_RECLAIM_RESOURCE_RESULTS;

typedef enum DXGI_FEATURE {
    DXGI_FEATURE_PRESENT_ALLOW_TEARING = 0
} DXGI_FEATURE;

/* dxgi1_6.h */
typedef enum DXGI_ADAPTER_FLAG3 {
    DXGI_ADAPTER_FLAG3_NONE = 0,
    DXGI_ADAPTER_FLAG3_REMOTE = 1,
    DXGI_ADAPTER_FLAG3_SOFTWARE = 2,
    DXGI_ADAPTER_FLAG3_ACG_COMPATIBLE = 4,
    DXGI_ADAPTER_FLAG3_SUPPORT_MONITORED_FENCES = 8,
    DXGI_ADAPTER_FLAG3_SUPPORT_NON_MONITORED_FENCES = 0x10,
    DXGI_ADAPTER_FLAG3_KEYED_MUTEX_CONFORMANCE = 0x20,
    DXGI_ADAPTER_FLAG3_FORCE_DWORD = 0xffffffff
} DXGI_ADAPTER_FLAG3;

typedef struct DXGI_ADAPTER_DESC3 {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
    DXGI_ADAPTER_FLAG3 Flags;
    DXGI_GRAPHICS_PREEMPTION_GRANULARITY GraphicsPreemptionGranularity;
    DXGI_COMPUTE_PREEMPTION_GRANULARITY ComputePreemptionGranularity;
} DXGI_ADAPTER_DESC3;

typedef struct DXGI_OUTPUT_DESC1 {
    WCHAR DeviceName[32];
    RECT DesktopCoordinates;
    BOOL AttachedToDesktop;
    DXGI_MODE_ROTATION Rotation;
    HMONITOR Monitor;
    UINT BitsPerColor;
    DXGI_COLOR_SPACE_TYPE ColorSpace;
    FLOAT RedPrimary[2];
    FLOAT GreenPrimary[2];
    FLOAT BluePrimary[2];
    FLOAT WhitePoint[2];
    FLOAT MinLuminance;
    FLOAT MaxLuminance;
    FLOAT MaxFullFrameLuminance;
} DXGI_OUTPUT_DESC1;

typedef enum DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAGS {
    DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_FULLSCREEN = 1,
    DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED = 2,
    DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_CURSOR_STRETCHED = 4
} DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAGS;

typedef enum DXGI_GPU_PREFERENCE {
    DXGI_GPU_PREFERENCE_UNSPECIFIED = 0,
    DXGI_GPU_PREFERENCE_MINIMUM_POWER = 1,
    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE = 2
} DXGI_GPU_PREFERENCE;

#ifdef __cplusplus
extern "C++" {

MIDL_INTERFACE("ea9dbf1a-c88e-4486-854a-98aa0138f30c")
IDXGIDisplayControl : public IUnknown {
public:
    virtual BOOL STDMETHODCALLTYPE IsStereoEnabled(void) = 0;
    virtual void STDMETHODCALLTYPE SetStereoEnabled(BOOL enabled) = 0;
};

MIDL_INTERFACE("191cfac3-a341-470d-b26e-a864f428319c")
IDXGIOutputDuplication : public IDXGIObject {
public:
    virtual void STDMETHODCALLTYPE GetDesc(DXGI_OUTDUPL_DESC* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE AcquireNextFrame(UINT TimeoutInMilliseconds, DXGI_OUTDUPL_FRAME_INFO* pFrameInfo,
                                                       IDXGIResource** ppDesktopResource) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameDirtyRects(UINT DirtyRectsBufferSize, RECT* pDirtyRectsBuffer,
                                                         UINT* pDirtyRectsBufferSizeRequired) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameMoveRects(UINT MoveRectsBufferSize,
                                                        DXGI_OUTDUPL_MOVE_RECT* pMoveRectBuffer,
                                                        UINT* pMoveRectsBufferSizeRequired) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFramePointerShape(UINT PointerShapeBufferSize, void* pPointerShapeBuffer,
                                                           UINT* pPointerShapeBufferSizeRequired,
                                                           DXGI_OUTDUPL_POINTER_SHAPE_INFO* pPointerShapeInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE MapDesktopSurface(DXGI_MAPPED_RECT* pLockedRect) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnMapDesktopSurface(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseFrame(void) = 0;
};

MIDL_INTERFACE("aba496dd-b617-4cb8-a866-bc44d7eb1fa2")
IDXGISurface2 : public IDXGISurface1 {
public:
    virtual HRESULT STDMETHODCALLTYPE GetResource(REFIID riid, void** ppParentResource, UINT* pSubresourceIndex) = 0;
};

MIDL_INTERFACE("30961379-4609-4a41-998e-54fe567ee0c1")
IDXGIResource1 : public IDXGIResource {
public:
    virtual HRESULT STDMETHODCALLTYPE CreateSubresourceSurface(UINT index, IDXGISurface2** ppSurface) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSharedHandle(const SECURITY_ATTRIBUTES* pAttributes, DWORD dwAccess,
                                                         LPCWSTR lpName, HANDLE* pHandle) = 0;
};

MIDL_INTERFACE("05008617-fbfd-4051-a790-144884b4f6a9")
IDXGIDevice2 : public IDXGIDevice1 {
public:
    virtual HRESULT STDMETHODCALLTYPE OfferResources(UINT NumResources, IDXGIResource* const* ppResources,
                                                     DXGI_OFFER_RESOURCE_PRIORITY Priority) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReclaimResources(UINT NumResources, IDXGIResource* const* ppResources,
                                                       BOOL* pDiscarded) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE hEvent) = 0;
};

MIDL_INTERFACE("790a45f7-0d42-4876-983a-0a55cfe6f4aa")
IDXGISwapChain1 : public IDXGISwapChain {
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) = 0;
    virtual HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                               const DXGI_PRESENT_PARAMETERS* pPresentParameters) = 0;
    virtual BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) = 0;
};

MIDL_INTERFACE("50c83a1c-e072-4c48-87b0-3630fa36a6d0")
IDXGIFactory2 : public IDXGIFactory1 {
public:
    virtual BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd,
                                                             const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                             IDXGIOutput* pRestrictToOutput,
                                                             IDXGISwapChain1** ppSwapChain) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow,
                                                                   const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                   IDXGIOutput* pRestrictToOutput,
                                                                   IDXGISwapChain1** ppSwapChain) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) = 0;
    virtual void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg,
                                                                    DWORD* pdwCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) = 0;
    virtual void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* pDevice,
                                                                    const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                    IDXGIOutput* pRestrictToOutput,
                                                                    IDXGISwapChain1** ppSwapChain) = 0;
};

MIDL_INTERFACE("0AA1AE0A-FA0E-4B84-8644-E05FF8E5ACB5")
IDXGIAdapter2 : public IDXGIAdapter1 {
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2* pDesc) = 0;
};

MIDL_INTERFACE("00cddea8-939b-4b83-a340-a685226666cc")
IDXGIOutput1 : public IDXGIOutput {
public:
    virtual HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT EnumFormat, UINT Flags, UINT* pNumModes,
                                                          DXGI_MODE_DESC1* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1* pModeToMatch,
                                                               DXGI_MODE_DESC1* pClosestMatch,
                                                               IUnknown* pConcernedDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource* pDestination) = 0;
    virtual HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown* pDevice,
                                                      IDXGIOutputDuplication** ppOutputDuplication) = 0;
};

MIDL_INTERFACE("6007896c-3244-4afd-bf18-a6d3beda5023")
IDXGIDevice3 : public IDXGIDevice2 {
public:
    virtual void STDMETHODCALLTYPE Trim(void) = 0;
};

MIDL_INTERFACE("a8be2ac4-199f-4946-b331-79599fb98de7")
IDXGISwapChain2 : public IDXGISwapChain1 {
public:
    virtual HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) = 0;
    virtual HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) = 0;
};

MIDL_INTERFACE("595e39d1-2724-4663-99b1-da969de28364")
IDXGIOutput2 : public IDXGIOutput1 {
public:
    virtual BOOL STDMETHODCALLTYPE SupportsOverlays(void) = 0;
};

MIDL_INTERFACE("25483823-cd46-4c7d-86ca-47aa95b837bd")
IDXGIFactory3 : public IDXGIFactory2 {
public:
    virtual UINT STDMETHODCALLTYPE GetCreationFlags(void) = 0;
};

MIDL_INTERFACE("8a6bb301-7e7e-41F4-a8e0-5b32f7f99b18")
IDXGIOutput3 : public IDXGIOutput2 {
public:
    virtual HRESULT STDMETHODCALLTYPE CheckOverlaySupport(DXGI_FORMAT EnumFormat, IUnknown* pConcernedDevice,
                                                          UINT* pFlags) = 0;
};

MIDL_INTERFACE("94d99bdb-f1f8-4ab0-b236-7da0170edab1")
IDXGISwapChain3 : public IDXGISwapChain2 {
public:
    virtual UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                             UINT* pColorSpaceSupport) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                                     UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                                     IUnknown* const* ppPresentQueue) = 0;
};

MIDL_INTERFACE("dc7dca35-2196-414d-9F53-617884032a60")
IDXGIOutput4 : public IDXGIOutput3 {
public:
    virtual HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT Format,
                                                                    DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                    IUnknown* pConcernedDevice, UINT* pFlags) = 0;
};

MIDL_INTERFACE("1bc6ea02-ef36-464f-bf0c-21ca39e5168a")
IDXGIFactory4 : public IDXGIFactory3 {
public:
    virtual HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** ppvAdapter) = 0;
};

MIDL_INTERFACE("645967A4-1392-4310-A798-8053CE3E93FD")
IDXGIAdapter3 : public IDXGIAdapter2 {
public:
    virtual HRESULT STDMETHODCALLTYPE RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE hEvent,
                                                                                           DWORD* pdwCookie) = 0;
    virtual void STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD dwCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT NodeIndex,
                                                           DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
                                                           DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT NodeIndex,
                                                                DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
                                                                UINT64 Reservation) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE hEvent,
                                                                                       DWORD* pdwCookie) = 0;
    virtual void STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD dwCookie) = 0;
};

MIDL_INTERFACE("80A07424-AB52-42EB-833C-0C42FD282D98")
IDXGIOutput5 : public IDXGIOutput4 {
public:
    virtual HRESULT STDMETHODCALLTYPE DuplicateOutput1(IUnknown* pDevice, UINT Flags, UINT SupportedFormatsCount,
                                                       const DXGI_FORMAT* pSupportedFormats,
                                                       IDXGIOutputDuplication** ppOutputDuplication) = 0;
};

MIDL_INTERFACE("3D585D5A-BD4A-489E-B1F4-3DBCB6452FFB")
IDXGISwapChain4 : public IDXGISwapChain3 {
public:
    virtual HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) = 0;
};

MIDL_INTERFACE("95B4F95F-D8DA-4CA4-9EE6-3B76D5968A10")
IDXGIDevice4 : public IDXGIDevice3 {
public:
    virtual HRESULT STDMETHODCALLTYPE OfferResources1(UINT NumResources, IDXGIResource* const* ppResources,
                                                      DXGI_OFFER_RESOURCE_PRIORITY Priority, UINT Flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReclaimResources1(UINT NumResources, IDXGIResource* const* ppResources,
                                                        DXGI_RECLAIM_RESOURCE_RESULTS* pResults) = 0;
};

MIDL_INTERFACE("7632e1f5-ee65-4dca-87fd-84cd75f8838d")
IDXGIFactory5 : public IDXGIFactory4 {
public:
    virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void* pFeatureSupportData,
                                                          UINT FeatureSupportDataSize) = 0;
};

MIDL_INTERFACE("3c8d99d1-4fbf-4181-a82c-af66bf7bd24e")
IDXGIAdapter4 : public IDXGIAdapter3 {
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3* pDesc) = 0;
};

MIDL_INTERFACE("068346e8-aaec-4b84-add7-137f513f77a1")
IDXGIOutput6 : public IDXGIOutput5 {
public:
    virtual HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1* pDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE CheckHardwareCompositionSupport(UINT* pFlags) = 0;
};

MIDL_INTERFACE("c1b6694f-ff09-44a9-b03c-77900a0a1d17")
IDXGIFactory6 : public IDXGIFactory5 {
public:
    virtual HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference,
                                                                 REFIID riid, void** ppvAdapter) = 0;
};

MIDL_INTERFACE("a4966eed-76db-44da-84c1-ee9a7afb20a8")
IDXGIFactory7 : public IDXGIFactory6 {
public:
    virtual HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD* pdwCookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD dwCookie) = 0;
};

}  // extern "C++"

__CRT_UUID_DECL(IDXGIDisplayControl, 0xea9dbf1a, 0xc88e, 0x4486, 0x85, 0x4a, 0x98, 0xaa, 0x01, 0x38, 0xf3, 0x0c)
__CRT_UUID_DECL(IDXGIOutputDuplication, 0x191cfac3, 0xa341, 0x470d, 0xb2, 0x6e, 0xa8, 0x64, 0xf4, 0x28, 0x31, 0x9c)
__CRT_UUID_DECL(IDXGISurface2, 0xaba496dd, 0xb617, 0x4cb8, 0xa8, 0x66, 0xbc, 0x44, 0xd7, 0xeb, 0x1f, 0xa2)
__CRT_UUID_DECL(IDXGIResource1, 0x30961379, 0x4609, 0x4a41, 0x99, 0x8e, 0x54, 0xfe, 0x56, 0x7e, 0xe0, 0xc1)
__CRT_UUID_DECL(IDXGIDevice2, 0x05008617, 0xfbfd, 0x4051, 0xa7, 0x90, 0x14, 0x48, 0x84, 0xb4, 0xf6, 0xa9)
__CRT_UUID_DECL(IDXGISwapChain1, 0x790a45f7, 0x0d42, 0x4876, 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa)
__CRT_UUID_DECL(IDXGIFactory2, 0x50c83a1c, 0xe072, 0x4c48, 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0)
__CRT_UUID_DECL(IDXGIAdapter2, 0x0aa1ae0a, 0xfa0e, 0x4b84, 0x86, 0x44, 0xe0, 0x5f, 0xf8, 0xe5, 0xac, 0xb5)
__CRT_UUID_DECL(IDXGIOutput1, 0x00cddea8, 0x939b, 0x4b83, 0xa3, 0x40, 0xa6, 0x85, 0x22, 0x66, 0x66, 0xcc)
__CRT_UUID_DECL(IDXGIDevice3, 0x6007896c, 0x3244, 0x4afd, 0xbf, 0x18, 0xa6, 0xd3, 0xbe, 0xda, 0x50, 0x23)
__CRT_UUID_DECL(IDXGISwapChain2, 0xa8be2ac4, 0x199f, 0x4946, 0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7)
__CRT_UUID_DECL(IDXGIOutput2, 0x595e39d1, 0x2724, 0x4663, 0x99, 0xb1, 0xda, 0x96, 0x9d, 0xe2, 0x83, 0x64)
__CRT_UUID_DECL(IDXGIFactory3, 0x25483823, 0xcd46, 0x4c7d, 0x86, 0xca, 0x47, 0xaa, 0x95, 0xb8, 0x37, 0xbd)
__CRT_UUID_DECL(IDXGIOutput3, 0x8a6bb301, 0x7e7e, 0x41f4, 0xa8, 0xe0, 0x5b, 0x32, 0xf7, 0xf9, 0x9b, 0x18)
__CRT_UUID_DECL(IDXGISwapChain3, 0x94d99bdb, 0xf1f8, 0x4ab0, 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1)
__CRT_UUID_DECL(IDXGIOutput4, 0xdc7dca35, 0x2196, 0x414d, 0x9f, 0x53, 0x61, 0x78, 0x84, 0x03, 0x2a, 0x60)
__CRT_UUID_DECL(IDXGIFactory4, 0x1bc6ea02, 0xef36, 0x464f, 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a)
__CRT_UUID_DECL(IDXGIAdapter3, 0x645967a4, 0x1392, 0x4310, 0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd)
__CRT_UUID_DECL(IDXGIOutput5, 0x80a07424, 0xab52, 0x42eb, 0x83, 0x3c, 0x0c, 0x42, 0xfd, 0x28, 0x2d, 0x98)
__CRT_UUID_DECL(IDXGISwapChain4, 0x3d585d5a, 0xbd4a, 0x489e, 0xb1, 0xf4, 0x3d, 0xbc, 0xb6, 0x45, 0x2f, 0xfb)
__CRT_UUID_DECL(IDXGIDevice4, 0x95b4f95f, 0xd8da, 0x4ca4, 0x9e, 0xe6, 0x3b, 0x76, 0xd5, 0x96, 0x8a, 0x10)
__CRT_UUID_DECL(IDXGIFactory5, 0x7632e1f5, 0xee65, 0x4dca, 0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d)
__CRT_UUID_DECL(IDXGIAdapter4, 0x3c8d99d1, 0x4fbf, 0x4181, 0xa8, 0x2c, 0xaf, 0x66, 0xbf, 0x7b, 0xd2, 0x4e)
__CRT_UUID_DECL(IDXGIOutput6, 0x068346e8, 0xaaec, 0x4b84, 0xad, 0xd7, 0x13, 0x7f, 0x51, 0x3f, 0x77, 0xa1)
__CRT_UUID_DECL(IDXGIFactory6, 0xc1b6694f, 0xff09, 0x44a9, 0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17)
__CRT_UUID_DECL(IDXGIFactory7, 0xa4966eed, 0x76db, 0x44da, 0x84, 0xc1, 0xee, 0x9a, 0x7a, 0xfb, 0x20, 0xa8)

extern "C" {
/* No host dxgi.dll on macOS: these are served by d3d12_vulkan (CreateDXGIFactory* return a Vulkan-backed factory
   only once a device exists; see platform/win32_host.cpp). */
HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory);
HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory);
HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory);
}

#endif  /* __cplusplus */
