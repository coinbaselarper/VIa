#pragma once
#include <propidl.h>

typedef struct _tagpropertykey {
    GUID fmtid;
    DWORD pid;
} PROPERTYKEY;
#ifdef __cplusplus
#define REFPROPERTYKEY const PROPERTYKEY&
#else
#define REFPROPERTYKEY const PROPERTYKEY*
#endif

struct IPropertyStore : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(DWORD* cProps) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAt(DWORD iProp, PROPERTYKEY* pkey) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY key, REFPROPVARIANT propvar) = 0;
    virtual HRESULT STDMETHODCALLTYPE Commit(void) = 0;
};
__CRT_UUID_DECL(IPropertyStore, 0x886d8eeb, 0x8cf2, 0x4446, 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99)

#define STGM_READ 0x00000000L
#define STGM_WRITE 0x00000001L
#define STGM_READWRITE 0x00000002L
