#pragma once

/*
 * Core macros for the Windows compatibility layer (see compat/README.md).
 *
 * Calling convention: every Win64-ABI entry point (WINAPI, CALLBACK, APIENTRY, NTAPI, STDMETHODCALLTYPE,
 * __stdcall, __cdecl, ...) expands to GTA6RT_WINABI.
 *   x86_64: __attribute__((ms_abi)). The guest (run by Rosetta) calls host functions directly with the
 *           Windows x64 convention, so guest-facing exports must use it. Host-to-host calls through the
 *           same declarations also use ms_abi; clang handles the mixed-ABI call.
 *   arm64:  empty (host AAPCS64). Guest code never calls host code natively there: isa/fexcore marshals
 *           the guest register file through GTA6RT_THUNK_FRAME thunks (include/gta6rt/isa.h).
 * _WIN32 is never defined.
 */

#if defined(_WIN32)
#error "gta6rt compat headers are for the macOS host; _WIN32 must not be defined"
#endif

#if defined(__x86_64__)
#define GTA6RT_ARCH_X86_64 1
#define GTA6RT_WINABI __attribute__((ms_abi))
#define GTA6RT_WINABI_VARARGS 1
#elif defined(__aarch64__) || defined(__arm64__)
#define GTA6RT_ARCH_ARM64 1
#define GTA6RT_WINABI
#define GTA6RT_WINABI_VARARGS 0
#else
#error "unsupported host architecture"
#endif

#ifdef __cplusplus
#define GTA6RT_EXTERN_C extern "C"
#define GTA6RT_BEGIN_C extern "C" {
#define GTA6RT_END_C }
#else
#define GTA6RT_EXTERN_C extern
#define GTA6RT_BEGIN_C
#define GTA6RT_END_C
#endif

#ifndef _WIN64
#define _WIN64 1
#endif
#ifndef _M_AMD64_GUEST
#define _M_AMD64_GUEST 1
#endif

#define WINAPI GTA6RT_WINABI
#define WINAPIV GTA6RT_WINABI
#define APIENTRY GTA6RT_WINABI
#define CALLBACK GTA6RT_WINABI
#define NTAPI GTA6RT_WINABI
#define NTAPI_INLINE
#define STDMETHODCALLTYPE GTA6RT_WINABI
#define STDMETHODVCALLTYPE GTA6RT_WINABI
#define STDAPICALLTYPE GTA6RT_WINABI
#define STDAPIVCALLTYPE GTA6RT_WINABI
#define WINAPI_INLINE GTA6RT_WINABI
#define PASCAL GTA6RT_WINABI
#define __stdcall GTA6RT_WINABI
#define __cdecl GTA6RT_WINABI
#define CDECL GTA6RT_WINABI
#define _cdecl GTA6RT_WINABI
#define __fastcall GTA6RT_WINABI
#define __thiscall GTA6RT_WINABI
#define __vectorcall GTA6RT_WINABI
#define __clrcall
#define _CRTIMP
#define _ACRTIMP
#define _VCRTIMP
#define _CRTRESTRICT
#define WINBASEAPI
#define WINADVAPI
#define WINUSERAPI
#define WINGDIAPI
#define NTSYSAPI
#define NTSYSCALLAPI
#define DECLSPEC_IMPORT
#define DECLSPEC_EXPORT __attribute__((visibility("default")))

#define EXTERN_C GTA6RT_EXTERN_C
#define EXTERN_C_START GTA6RT_BEGIN_C
#define EXTERN_C_END GTA6RT_END_C
#define STDAPI EXTERN_C HRESULT STDAPICALLTYPE
#define STDAPI_(type) EXTERN_C type STDAPICALLTYPE
#define STDMETHODIMP HRESULT STDMETHODCALLTYPE
#define STDMETHODIMP_(type) type STDMETHODCALLTYPE

/* __declspec(x) -> GTA6RT_DECLSPEC_x. Unknown specifiers fail to compile, which is intended. */
#define __declspec(x) GTA6RT_DECLSPEC_##x
#define GTA6RT_DECLSPEC_dllexport __attribute__((visibility("default"), used))
#define GTA6RT_DECLSPEC_dllimport
#define GTA6RT_DECLSPEC_noinline __attribute__((noinline))
#define GTA6RT_DECLSPEC_noreturn __attribute__((noreturn))
#define GTA6RT_DECLSPEC_nothrow __attribute__((nothrow))
#define GTA6RT_DECLSPEC_novtable
#define GTA6RT_DECLSPEC_noalias
#define GTA6RT_DECLSPEC_restrict
#define GTA6RT_DECLSPEC_safebuffers
#define GTA6RT_DECLSPEC_selectany __attribute__((weak))
#define GTA6RT_DECLSPEC_thread __thread
#define GTA6RT_DECLSPEC_naked __attribute__((naked))
#define GTA6RT_DECLSPEC_deprecated __attribute__((deprecated))
#define GTA6RT_DECLSPEC_align(n) __attribute__((aligned(n)))
#define GTA6RT_DECLSPEC_uuid(s)
#define GTA6RT_DECLSPEC_property(...)
/* __declspec(allocate(".CRT$XCU")) has no Mach-O equivalent; port such sites to __attribute__((constructor)). */

#define DECLSPEC_NOINLINE __attribute__((noinline))
#define DECLSPEC_NORETURN __attribute__((noreturn))
#define DECLSPEC_NOTHROW __attribute__((nothrow))
#define DECLSPEC_NOVTABLE
#define DECLSPEC_SELECTANY __attribute__((weak))
#define DECLSPEC_ALIGN(n) __attribute__((aligned(n)))
#define DECLSPEC_UUID(x)
#define DECLSPEC_XFGVIRT(base, func)
#define DECLSPEC_GUARDNOCF
#define DECLSPEC_GUARD_SUPPRESS
#define DECLSPEC_CACHEALIGN __attribute__((aligned(64)))
#define DECLSPEC_DEPRECATED __attribute__((deprecated))
#define DECLSPEC_ADDRSAFE
#define DECLSPEC_SAFEBUFFERS
#define MIDL_INTERFACE(x) struct
#define DECLSPEC_HOTPATCH

#define __int8 char
#define __int16 short
#define __int32 int
#define __int64 long long
#define __forceinline inline __attribute__((always_inline))
#define FORCEINLINE __forceinline
#define CFORCEINLINE __forceinline
#define __unaligned
#define UNALIGNED
#define UNALIGNED64
#define __ptr32
#define __ptr64
#define POINTER_32
#define POINTER_64
#define __w64
#define __noop(...) ((void)0)
#define __assume(x) __builtin_assume(x)
#define __pragma(x) _Pragma(#x)
#define __identifier(x) x
#define __alignof(x) _Alignof(x)

#define FAR
#define NEAR
#ifndef CONST
#define CONST const
#endif
#ifndef VOID
#define VOID void
#endif
#ifndef IN
#define IN
#endif
#ifndef OUT
#define OUT
#endif
#ifndef OPTIONAL
#define OPTIONAL
#endif

#define UNREFERENCED_PARAMETER(P) ((void)(P))
#define DBG_UNREFERENCED_PARAMETER(P) ((void)(P))
#define DBG_UNREFERENCED_LOCAL_VARIABLE(V) ((void)(V))
#define C_ASSERT(e) _Static_assert((e), #e)
#ifdef __cplusplus
#undef C_ASSERT
#define C_ASSERT(e) static_assert((e), #e)
#endif

#ifndef _countof
#ifdef __cplusplus
extern "C++" template <typename T, __SIZE_TYPE__ N> char (*gta6rt_countof_helper(T (&)[N]))[N];
#define _countof(a) (sizeof(*gta6rt_countof_helper(a)))
#else
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#endif
#define ARRAYSIZE(a) _countof(a)
#define RTL_NUMBER_OF(a) _countof(a)

#define FIELD_OFFSET(type, field) ((LONG)__builtin_offsetof(type, field))
#define RTL_FIELD_SIZE(type, field) (sizeof(((type*)0)->field))
#define CONTAINING_RECORD(address, type, field) ((type*)((char*)(address) - __builtin_offsetof(type, field)))
