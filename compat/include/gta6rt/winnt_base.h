#pragma once

/*
 * Character and core NT types. WCHAR is UTF-16 (char16_t), matching what the guest passes.
 * Consequence for ported code: L"..." literals are wchar_t (4 bytes) and do not convert to LPCWSTR;
 * use u"..." (or the TEXT()/_T() macros below, which expand to u"..." under UNICODE). std::wstring
 * paths that meet Win32 W APIs become std::u16string. Never build with -fshort-wchar.
 */

#include <minwindef.h>
#include <guiddef.h>

typedef void* PVOID;
typedef void* PVOID64;
typedef char CHAR, *PCHAR, *LPCH, *PCH;
typedef short SHORT, *PSHORT;
typedef long long LONGLONG, *PLONGLONG;
typedef unsigned long long ULONGLONG, *PULONGLONG;
typedef LONGLONG USN;
typedef BYTE BOOLEAN, *PBOOLEAN;
typedef char CCHAR;
typedef DWORD LCID, *PLCID;
typedef WORD LANGID;
typedef LONG HRESULT;
typedef LONG NTSTATUS, *PNTSTATUS;
typedef DWORD ACCESS_MASK, *PACCESS_MASK;
typedef PVOID PSECURITY_DESCRIPTOR;
typedef DWORD EXECUTION_STATE;
typedef LONG KPRIORITY;

#ifdef __cplusplus
typedef char16_t WCHAR;
#else
typedef uint_least16_t WCHAR;
#endif
typedef WCHAR *PWCHAR, *LPWCH, *PWCH, *NWPSTR, *LPWSTR, *PWSTR;
typedef const WCHAR *LPCWCH, *PCWCH, *LPCWSTR, *PCWSTR;
typedef CHAR *NPSTR, *LPSTR, *PSTR;
typedef const CHAR *LPCCH, *PCCH, *LPCSTR, *PCSTR;
typedef WCHAR* PZZWSTR;
typedef const WCHAR* PCZZWSTR;
typedef CHAR* PZZSTR;
typedef const CHAR* PCZZSTR;
typedef WCHAR OLECHAR, *LPOLESTR;
typedef const WCHAR* LPCOLESTR;
typedef WCHAR* BSTR;

#if defined(UNICODE)
typedef WCHAR TCHAR, *PTCHAR, TBYTE, *PTBYTE;
typedef LPWSTR LPTCH, PTCH, PTSTR, LPTSTR;
typedef LPCWSTR PCTSTR, LPCTSTR;
#define __TEXT(q) u##q
#else
typedef char TCHAR, *PTCHAR;
typedef unsigned char TBYTE, *PTBYTE;
typedef LPSTR LPTCH, PTCH, PTSTR, LPTSTR;
typedef LPCSTR PCTSTR, LPCTSTR;
#define __TEXT(q) q
#endif
#define TEXT(q) __TEXT(q)
#ifndef _T
#define _T(q) __TEXT(q)
#endif
#ifndef _TEXT
#define _TEXT(q) __TEXT(q)
#endif

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    struct {
        DWORD LowPart;
        LONG HighPart;
    } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    struct {
        DWORD LowPart;
        DWORD HighPart;
    } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _LUID {
    DWORD LowPart;
    LONG HighPart;
} LUID, *PLUID;

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY* Flink;
    struct _LIST_ENTRY* Blink;
} LIST_ENTRY, *PLIST_ENTRY;

typedef struct _SINGLE_LIST_ENTRY {
    struct _SINGLE_LIST_ENTRY* Next;
} SINGLE_LIST_ENTRY, *PSINGLE_LIST_ENTRY;

typedef struct DECLSPEC_ALIGN(16) _SLIST_ENTRY {
    struct _SLIST_ENTRY* Next;
} SLIST_ENTRY, *PSLIST_ENTRY;

typedef union DECLSPEC_ALIGN(16) _SLIST_HEADER {
    struct {
        ULONGLONG Alignment;
        ULONGLONG Region;
    };
    struct {
        ULONGLONG Depth : 16;
        ULONGLONG Sequence : 48;
        ULONGLONG Reserved : 4;
        ULONGLONG NextEntry : 60;
    } HeaderX64;
} SLIST_HEADER, *PSLIST_HEADER;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
typedef const UNICODE_STRING* PCUNICODE_STRING;

typedef struct _STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} STRING, ANSI_STRING, *PSTRING, *PANSI_STRING;

typedef struct _GROUP_AFFINITY {
    KAFFINITY Mask;
    WORD Group;
    WORD Reserved[3];
} GROUP_AFFINITY, *PGROUP_AFFINITY;

typedef struct _PROCESSOR_NUMBER {
    WORD Group;
    BYTE Number;
    BYTE Reserved;
} PROCESSOR_NUMBER, *PPROCESSOR_NUMBER;

#define MINCHAR 0x80
#define MAXCHAR 0x7f
#define MINSHORT 0x8000
#define MAXSHORT 0x7fff
#define MINLONG 0x80000000
#define MAXLONG 0x7fffffff
#define MAXBYTE 0xff
#define MAXWORD 0xffff
#define MAXDWORD 0xffffffff
#define MAXLONGLONG (0x7fffffffffffffffLL)
#define MAXULONGLONG (~(ULONGLONG)0)

#define ANYSIZE_ARRAY 1

#define RtlEqualMemory(d, s, n) (memcmp((d), (s), (n)) == 0)
#define RtlMoveMemory(d, s, n) memmove((d), (s), (n))
#define RtlCopyMemory(d, s, n) memcpy((d), (s), (n))
#define RtlFillMemory(d, n, f) memset((d), (f), (n))
#define RtlZeroMemory(d, n) memset((d), 0, (n))
static inline PVOID RtlSecureZeroMemory(PVOID d, SIZE_T n) {
    volatile char* p = (volatile char*)d;
    while (n--) *p++ = 0;
    return d;
}
#define MoveMemory RtlMoveMemory
#define CopyMemory RtlCopyMemory
#define FillMemory RtlFillMemory
#define ZeroMemory RtlZeroMemory
#define SecureZeroMemory RtlSecureZeroMemory

#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))
#define LANG_NEUTRAL 0x00
#define SUBLANG_DEFAULT 0x01
#define LOCALE_NAME_MAX_LENGTH 85

#define DEFINE_ENUM_FLAG_OPERATORS_GTA6RT_IMPL(T, U)                                       \
    extern "C++" {                                                                     \
    inline constexpr T operator|(T a, T b) { return T(((U)a) | ((U)b)); }              \
    inline T& operator|=(T& a, T b) { return (T&)(((U&)a) |= ((U)b)); }                \
    inline constexpr T operator&(T a, T b) { return T(((U)a) & ((U)b)); }              \
    inline T& operator&=(T& a, T b) { return (T&)(((U&)a) &= ((U)b)); }                \
    inline constexpr T operator~(T a) { return T(~((U)a)); }                           \
    inline constexpr T operator^(T a, T b) { return T(((U)a) ^ ((U)b)); }              \
    inline T& operator^=(T& a, T b) { return (T&)(((U&)a) ^= ((U)b)); }                \
    }
#ifdef __cplusplus
extern "C++" {
template <__SIZE_TYPE__ S> struct _ENUM_FLAG_INTEGER_FOR_SIZE;
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<1> { typedef int8_t type; };
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<2> { typedef int16_t type; };
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<4> { typedef int32_t type; };
template <> struct _ENUM_FLAG_INTEGER_FOR_SIZE<8> { typedef int64_t type; };
template <class T> struct _ENUM_FLAG_SIZED_INTEGER {
    typedef typename _ENUM_FLAG_INTEGER_FOR_SIZE<sizeof(T)>::type type;
};
}
#define DEFINE_ENUM_FLAG_OPERATORS(T) DEFINE_ENUM_FLAG_OPERATORS_GTA6RT_IMPL(T, _ENUM_FLAG_SIZED_INTEGER<T>::type)
#else
#define DEFINE_ENUM_FLAG_OPERATORS(T)
#endif
