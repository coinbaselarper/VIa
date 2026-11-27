#pragma once
#include <minwinbase.h>

#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define HANDLE_FLAG_INHERIT 0x00000001
#define HANDLE_FLAG_PROTECT_FROM_CLOSE 0x00000002

/* Implemented by os/ (handle table). */
GTA6RT_BEGIN_C
BOOL WINAPI CloseHandle(HANDLE hObject);
BOOL WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
                            LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions);
BOOL WINAPI GetHandleInformation(HANDLE hObject, LPDWORD lpdwFlags);
BOOL WINAPI SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags);
BOOL WINAPI CompareObjectHandles(HANDLE hFirstObjectHandle, HANDLE hSecondObjectHandle);
GTA6RT_END_C
