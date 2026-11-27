#pragma once
#include <minwinbase.h>

#define TH32CS_SNAPHEAPLIST 0x00000001
#define TH32CS_SNAPPROCESS 0x00000002
#define TH32CS_SNAPTHREAD 0x00000004
#define TH32CS_SNAPMODULE 0x00000008

typedef struct tagTHREADENTRY32 {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ThreadID;
    DWORD th32OwnerProcessID;
    LONG tpBasePri;
    LONG tpDeltaPri;
    DWORD dwFlags;
} THREADENTRY32, *PTHREADENTRY32, *LPTHREADENTRY32;

/* Implemented by os/ (thread snapshot over the os thread registry). */
GTA6RT_BEGIN_C
HANDLE WINAPI CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID);
BOOL WINAPI Thread32First(HANDLE hSnapshot, LPTHREADENTRY32 lpte);
BOOL WINAPI Thread32Next(HANDLE hSnapshot, LPTHREADENTRY32 lpte);
GTA6RT_END_C
