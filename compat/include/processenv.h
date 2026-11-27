#pragma once
#include <minwinbase.h>

#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)

/* Implemented by os/. Environment strings are UTF-8 on the host side; W variants convert. */
GTA6RT_BEGIN_C
DWORD WINAPI GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);
DWORD WINAPI GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize);
BOOL WINAPI SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue);
BOOL WINAPI SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue);
DWORD WINAPI ExpandEnvironmentStringsW(LPCWSTR lpSrc, LPWSTR lpDst, DWORD nSize);
LPWCH WINAPI GetEnvironmentStringsW(void);
BOOL WINAPI FreeEnvironmentStringsW(LPWCH penv);
LPSTR WINAPI GetCommandLineA(void);
LPWSTR WINAPI GetCommandLineW(void);
HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
BOOL WINAPI SetStdHandle(DWORD nStdHandle, HANDLE hHandle);
DWORD WINAPI GetCurrentDirectoryW(DWORD nBufferLength, LPWSTR lpBuffer);
BOOL WINAPI SetCurrentDirectoryW(LPCWSTR lpPathName);
GTA6RT_END_C

#if defined(UNICODE)
#define GetEnvironmentVariable GetEnvironmentVariableW
#define SetEnvironmentVariable SetEnvironmentVariableW
#define ExpandEnvironmentStrings ExpandEnvironmentStringsW
#define GetCommandLine GetCommandLineW
#define GetCurrentDirectory GetCurrentDirectoryW
#define SetCurrentDirectory SetCurrentDirectoryW
#else
#define GetEnvironmentVariable GetEnvironmentVariableA
#define SetEnvironmentVariable SetEnvironmentVariableA
#define GetCommandLine GetCommandLineA
#endif
