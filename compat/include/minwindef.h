#pragma once

/*
 * Base Windows types with LLP64 widths: LONG/ULONG/DWORD are 32-bit on this LP64 host.
 * BOOL is int (as on Windows). This conflicts with Objective-C's BOOL: .m/.mm files must not include windows.h.
 */

#include <basetsd.h>

typedef unsigned int ULONG, *PULONG;
typedef unsigned short USHORT, *PUSHORT;
typedef unsigned char UCHAR, *PUCHAR;
typedef char* PSZ;
typedef int LONG, *PLONG, *LPLONG;
typedef unsigned int DWORD, *PDWORD, *LPDWORD;
typedef int BOOL, *PBOOL, *LPBOOL;
typedef unsigned char BYTE, *PBYTE, *LPBYTE;
typedef unsigned short WORD, *PWORD, *LPWORD;
typedef float FLOAT, *PFLOAT;
typedef double DOUBLE;
typedef int INT, *PINT, *LPINT;
typedef unsigned int UINT, *PUINT;
typedef void* LPVOID;
typedef const void* LPCVOID;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void*)0)
#endif
#endif

#define MAX_PATH 260

typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef WORD ATOM;

typedef void* HANDLE;
typedef HANDLE* PHANDLE, *LPHANDLE;

#define DECLARE_HANDLE(name) \
    struct name##__ {        \
        int unused;          \
    };                       \
    typedef struct name##__* name

DECLARE_HANDLE(HINSTANCE);
typedef HINSTANCE HMODULE;
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HKEY);
typedef HKEY* PHKEY;
DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HMONITOR);
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HICON);
typedef HICON HCURSOR;
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HGDIOBJ);
DECLARE_HANDLE(HRSRC);
DECLARE_HANDLE(HRGN);
DECLARE_HANDLE(HPALETTE);
DECLARE_HANDLE(HGLRC);
DECLARE_HANDLE(HKL);
DECLARE_HANDLE(HDESK);
DECLARE_HANDLE(HWINSTA);
typedef HANDLE HGLOBAL;
typedef HANDLE HLOCAL;
typedef int HFILE;

typedef INT_PTR(WINAPI* FARPROC)(void);
typedef INT_PTR(WINAPI* NEARPROC)(void);
typedef INT_PTR(WINAPI* PROC)(void);

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

#define MAKEWORD(a, b) ((WORD)(((BYTE)(((DWORD_PTR)(a)) & 0xff)) | ((WORD)((BYTE)(((DWORD_PTR)(b)) & 0xff))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)(((DWORD_PTR)(a)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(b)) & 0xffff))) << 16))
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
