#pragma once
#include <windows.h>
#include <objbase.h>
#include <guiddef.h>
#include <string.h>

/* PROPVARIANT: 24 bytes on x64 (vt, three reserved words, a 16-byte value union). Only the members the host shims
 * touch are declared in the union; the size and offsets are what the guest relies on. */
#ifndef _TAGBLOB_DEFINED
#define _TAGBLOB_DEFINED
typedef struct tagBLOB {
    ULONG cbSize;
    BYTE* pBlobData;
} BLOB, *LPBLOB;
#endif

typedef WORD VARTYPE;
enum VARENUM {
    VT_EMPTY = 0,
    VT_NULL = 1,
    VT_I2 = 2,
    VT_I4 = 3,
    VT_R4 = 4,
    VT_R8 = 5,
    VT_BSTR = 8,
    VT_BOOL = 11,
    VT_I1 = 16,
    VT_UI1 = 17,
    VT_UI2 = 18,
    VT_UI4 = 19,
    VT_I8 = 20,
    VT_UI8 = 21,
    VT_LPSTR = 30,
    VT_LPWSTR = 31,
    VT_BLOB = 65,
    VT_CLSID = 72
};

struct tagPROPVARIANT {
    VARTYPE vt;
    WORD wReserved1;
    WORD wReserved2;
    WORD wReserved3;
    union {
        CHAR cVal;
        UCHAR bVal;
        SHORT iVal;
        USHORT uiVal;
        LONG lVal;
        ULONG ulVal;
        LONGLONG hVal;
        ULONGLONG uhVal;
        float fltVal;
        double dblVal;
        LPSTR pszVal;
        LPWSTR pwszVal;
        GUID* puuid;
        BLOB blob;
    };
};
typedef struct tagPROPVARIANT PROPVARIANT;
typedef PROPVARIANT* LPPROPVARIANT;
#ifdef __cplusplus
#define REFPROPVARIANT const PROPVARIANT&
#else
#define REFPROPVARIANT const PROPVARIANT*
#endif

static inline void PropVariantInit(PROPVARIANT* pvar) { memset(pvar, 0, sizeof(PROPVARIANT)); }
