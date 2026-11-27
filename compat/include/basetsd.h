#pragma once

/* Fixed-width and pointer-precision types with Windows x64 (LLP64) widths. */

#include <stddef.h>
#include <stdint.h>
#include <gta6rt/wincompat_config.h>

typedef int8_t INT8, *PINT8;
typedef int16_t INT16, *PINT16;
typedef int32_t INT32, *PINT32;
typedef int64_t INT64, *PINT64;
typedef uint8_t UINT8, *PUINT8;
typedef uint16_t UINT16, *PUINT16;
typedef uint32_t UINT32, *PUINT32;
typedef uint64_t UINT64, *PUINT64;

typedef int32_t LONG32, *PLONG32;
typedef uint32_t ULONG32, *PULONG32;
typedef uint32_t DWORD32, *PDWORD32;
typedef int64_t LONG64, *PLONG64;
typedef uint64_t ULONG64, *PULONG64;
typedef uint64_t DWORD64, *PDWORD64;

/* Pointer-sized types use the host's intptr_t/size_t so &size_t_var converts to PSIZE_T. */
typedef intptr_t INT_PTR, *PINT_PTR;
typedef uintptr_t UINT_PTR, *PUINT_PTR;
typedef intptr_t LONG_PTR, *PLONG_PTR;
typedef uintptr_t ULONG_PTR, *PULONG_PTR;
typedef ULONG_PTR DWORD_PTR, *PDWORD_PTR;
typedef size_t SIZE_T, *PSIZE_T;
typedef intptr_t SSIZE_T, *PSSIZE_T;
typedef ULONG_PTR KAFFINITY, *PKAFFINITY;

#define MAXUINT_PTR (~((UINT_PTR)0))
#define MAXINT_PTR ((INT_PTR)(MAXUINT_PTR >> 1))
#define MININT_PTR (~MAXINT_PTR)
#define MAXULONG_PTR (~((ULONG_PTR)0))
#define MAXLONG_PTR ((LONG_PTR)(MAXULONG_PTR >> 1))
#define MAXUHALF_PTR ((UHALF_PTR)~0)
#define MAXSIZE_T ((SIZE_T)~((SIZE_T)0))
#define MAXSSIZE_T ((SSIZE_T)(MAXSIZE_T >> 1))
#define MAXUINT8 ((UINT8)~((UINT8)0))
#define MAXINT8 ((INT8)(MAXUINT8 >> 1))
#define MAXUINT16 ((UINT16)~((UINT16)0))
#define MAXINT16 ((INT16)(MAXUINT16 >> 1))
#define MAXUINT32 ((UINT32)~((UINT32)0))
#define MAXINT32 ((INT32)(MAXUINT32 >> 1))
#define MAXUINT64 ((UINT64)~((UINT64)0))
#define MAXINT64 ((INT64)(MAXUINT64 >> 1))
#define MAXULONG32 ((ULONG32)~((ULONG32)0))
#define MAXLONG32 ((LONG32)(MAXULONG32 >> 1))
#define MAXDWORD32 ((DWORD32)~((DWORD32)0))
#define MAXDWORD64 ((DWORD64)~((DWORD64)0))
#define MAXULONG64 ((ULONG64)~((ULONG64)0))
#define MAXLONG64 ((LONG64)(MAXULONG64 >> 1))

typedef int32_t HALF_PTR, *PHALF_PTR;
typedef uint32_t UHALF_PTR, *PUHALF_PTR;

#define HandleToULong(h) ((ULONG)(ULONG_PTR)(h))
#define HandleToLong(h) ((LONG)(LONG_PTR)(h))
#define ULongToHandle(ul) ((HANDLE)(ULONG_PTR)(ul))
#define LongToHandle(h) ((HANDLE)(LONG_PTR)(h))
#define PtrToUlong(p) ((ULONG)(ULONG_PTR)(p))
#define PtrToLong(p) ((LONG)(LONG_PTR)(p))
#define PtrToUint(p) ((UINT)(UINT_PTR)(p))
#define PtrToInt(p) ((INT)(INT_PTR)(p))
#define UlongToPtr(ul) ((void*)(ULONG_PTR)(ul))
#define UintToPtr(ui) ((void*)(UINT_PTR)(ui))
#define IntToPtr(i) ((void*)(INT_PTR)(i))
#define LongToPtr(l) ((void*)(LONG_PTR)(l))
