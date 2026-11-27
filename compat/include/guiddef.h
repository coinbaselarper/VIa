#pragma once

/*
 * GUID, DEFINE_GUID and the __uuidof emulation. The __CRT_UUID_DECL/__uuidof template scheme follows
 * DirectX-Headers include/wsl/stubs/rpcndr.h (Copyright (c) Microsoft Corporation, MIT License), so
 * DirectX-Headers' own __CRT_UUID_DECL lines and dxguids.h work unchanged.
 */

#include <minwindef.h>
#include <string.h>

#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} GUID;
#endif

typedef GUID IID;
typedef GUID CLSID;
typedef GUID UUID;
typedef GUID FMTID;
typedef GUID* LPGUID;
typedef const GUID* LPCGUID;
typedef IID* LPIID;
typedef CLSID* LPCLSID;

#ifdef INITGUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    EXTERN_C const GUID DECLSPEC_SELECTANY name = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}
#else
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) EXTERN_C const GUID name
#endif

#ifdef __cplusplus
#define REFGUID const GUID&
#define REFIID const IID&
#define REFCLSID const IID&
#define REFFMTID const IID&

inline int InlineIsEqualGUID(REFGUID a, REFGUID b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }
inline int IsEqualGUID(REFGUID a, REFGUID b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }
inline bool operator==(REFGUID a, REFGUID b) { return IsEqualGUID(a, b) != 0; }
inline bool operator!=(REFGUID a, REFGUID b) { return !(a == b); }
#else
#define REFGUID const GUID*
#define REFIID const IID*
#define REFCLSID const IID*
#define REFFMTID const IID*
#define InlineIsEqualGUID(a, b) (memcmp((a), (b), sizeof(GUID)) == 0)
#define IsEqualGUID(a, b) (memcmp((a), (b), sizeof(GUID)) == 0)
#endif
#define IsEqualIID(a, b) IsEqualGUID(a, b)
#define IsEqualCLSID(a, b) IsEqualGUID(a, b)
#define IsEqualFMTID(a, b) IsEqualGUID(a, b)

#if defined(__cplusplus)
extern "C++" {
template <typename T> struct __wsl_stub_uuidof_s;
template <typename T> constexpr const GUID& __wsl_stub_uuidof();
}

#define __CRT_UUID_DECL(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8)                       \
    extern "C++" {                                                                         \
    template <> struct __wsl_stub_uuidof_s<type> {                                         \
        static constexpr IID __uuid_inst = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}; \
    };                                                                                     \
    template <> constexpr const GUID& __wsl_stub_uuidof<type>() {                          \
        return __wsl_stub_uuidof_s<type>::__uuid_inst;                                     \
    }                                                                                      \
    template <> constexpr const GUID& __wsl_stub_uuidof<type*>() {                         \
        return __wsl_stub_uuidof_s<type>::__uuid_inst;                                     \
    }                                                                                      \
    }
#define __uuidof(type) __wsl_stub_uuidof<__typeof(type)>()
#else
#define __CRT_UUID_DECL(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8)
#endif

/* Declares an interface's IID for __uuidof from its string form's parts, e.g. GTA6RT_UUID_DECL(IFoo, 0x..., ...). */
#define GTA6RT_UUID_DECL __CRT_UUID_DECL
