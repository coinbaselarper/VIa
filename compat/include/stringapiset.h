#pragma once
#include <minwinbase.h>

#define CP_ACP 0
#define CP_OEMCP 1
#define CP_THREAD_ACP 3
#define CP_UTF7 65000
#define CP_UTF8 65001
#define MB_PRECOMPOSED 0x00000001
#define MB_ERR_INVALID_CHARS 0x00000008
#define WC_NO_BEST_FIT_CHARS 0x00000400
#define WC_ERR_INVALID_CHARS 0x00000080
#define CSTR_LESS_THAN 1
#define CSTR_EQUAL 2
#define CSTR_GREATER_THAN 3
#define NORM_IGNORECASE 0x00000001
#define LINGUISTIC_IGNORECASE 0x00000010

/* Implemented by os/ (UTF-8 <-> UTF-16; CP_ACP is treated as UTF-8). */
GTA6RT_BEGIN_C
int WINAPI MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr, int cbMultiByte,
                               LPWSTR lpWideCharStr, int cchWideChar);
int WINAPI WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar,
                               LPSTR lpMultiByteStr, int cbMultiByte, LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar);
int WINAPI CompareStringOrdinal(LPCWCH lpString1, int cchCount1, LPCWCH lpString2, int cchCount2, BOOL bIgnoreCase);
int WINAPI CompareStringEx(LPCWSTR lpLocaleName, DWORD dwCmpFlags, LPCWCH lpString1, int cchCount1, LPCWCH lpString2,
                           int cchCount2, PVOID lpVersionInformation, LPVOID lpReserved, LPARAM lParam);
int WINAPI GetUserDefaultLocaleName(LPWSTR lpLocaleName, int cchLocaleName);
UINT WINAPI GetACP(void);
int WINAPI lstrlenW(LPCWSTR lpString);
int WINAPI lstrcmpiW(LPCWSTR lpString1, LPCWSTR lpString2);
GTA6RT_END_C
