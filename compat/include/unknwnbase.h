#pragma once

#include <windows.h>

#define interface struct
#define BEGIN_INTERFACE
#define END_INTERFACE
#define PURE = 0
#define THIS_
#define THIS void
#define STDMETHOD(m) virtual HRESULT STDMETHODCALLTYPE m
#define STDMETHOD_(t, m) virtual t STDMETHODCALLTYPE m
#define IFACEMETHOD(m) STDMETHOD(m)
#define IFACEMETHOD_(t, m) STDMETHOD_(t, m)
#define IFACEMETHODIMP STDMETHODIMP
#define IFACEMETHODIMP_(t) STDMETHODIMP_(t)
#define DECLARE_INTERFACE(i) struct DECLSPEC_NOVTABLE i
#define DECLARE_INTERFACE_(i, b) struct DECLSPEC_NOVTABLE i : public b
#define DECLARE_INTERFACE_IID_(i, b, s) DECLARE_INTERFACE_(i, b)

#ifdef __cplusplus
struct IUnknown;
typedef IUnknown* LPUNKNOWN;

extern "C++" {
struct IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef(void) = 0;
    virtual ULONG STDMETHODCALLTYPE Release(void) = 0;
    template <class Q> HRESULT STDMETHODCALLTYPE QueryInterface(Q** pp) { return QueryInterface(__uuidof(Q), (void**)pp); }
};
}
__CRT_UUID_DECL(IUnknown, 0x00000000, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)

extern "C++" {
template <typename T> void** IID_PPV_ARGS_Helper(T** pp) {
    (void)static_cast<IUnknown*>(*pp);
    return reinterpret_cast<void**>(pp);
}
}
#define IID_PPV_ARGS(pp) __uuidof(**(pp)), IID_PPV_ARGS_Helper(pp)

struct IClassFactory : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) = 0;
};
__CRT_UUID_DECL(IClassFactory, 0x00000001, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
#else
typedef struct IUnknown IUnknown;
typedef IUnknown* LPUNKNOWN;
#endif

DEFINE_GUID(IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
