#pragma once
#include <minwinbase.h>

#define HEAP_NO_SERIALIZE 0x00000001
#define HEAP_GENERATE_EXCEPTIONS 0x00000004
#define HEAP_ZERO_MEMORY 0x00000008
#define HEAP_REALLOC_IN_PLACE_ONLY 0x00000010
#define HEAP_CREATE_ENABLE_EXECUTE 0x00040000

/* Implemented by os/. */
GTA6RT_BEGIN_C
HANDLE WINAPI GetProcessHeap(void);
HANDLE WINAPI HeapCreate(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize);
BOOL WINAPI HeapDestroy(HANDLE hHeap);
LPVOID WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
LPVOID WINAPI HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes);
BOOL WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);
SIZE_T WINAPI HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem);
HLOCAL WINAPI LocalAlloc(UINT uFlags, SIZE_T uBytes);
HLOCAL WINAPI LocalFree(HLOCAL hMem);
HGLOBAL WINAPI GlobalAlloc(UINT uFlags, SIZE_T dwBytes);
HGLOBAL WINAPI GlobalFree(HGLOBAL hMem);
GTA6RT_END_C
