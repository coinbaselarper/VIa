#pragma once
#include <minwinbase.h>

#define PROCESSOR_ARCHITECTURE_AMD64 9
#define PROCESSOR_ARCHITECTURE_ARM64 12
#define PROCESSOR_AMD_X8664 8664

typedef struct _SYSTEM_INFO {
    union {
        DWORD dwOemId;
        struct {
            WORD wProcessorArchitecture;
            WORD wReserved;
        };
    };
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

typedef struct _MEMORYSTATUSEX {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    ULONGLONG ullTotalPhys;
    ULONGLONG ullAvailPhys;
    ULONGLONG ullTotalPageFile;
    ULONGLONG ullAvailPageFile;
    ULONGLONG ullTotalVirtual;
    ULONGLONG ullAvailVirtual;
    ULONGLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

typedef struct _OSVERSIONINFOEXW {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    WCHAR szCSDVersion[128];
    WORD wServicePackMajor;
    WORD wServicePackMinor;
    WORD wSuiteMask;
    BYTE wProductType;
    BYTE wReserved;
} OSVERSIONINFOEXW, *POSVERSIONINFOEXW, *LPOSVERSIONINFOEXW;

/* Implemented by os/. Guest-visible values (page size 4 KiB, granularity 64 KiB) are os policy, not host values. */
GTA6RT_BEGIN_C
void WINAPI GetSystemInfo(LPSYSTEM_INFO lpSystemInfo);
void WINAPI GetNativeSystemInfo(LPSYSTEM_INFO lpSystemInfo);
BOOL WINAPI GlobalMemoryStatusEx(LPMEMORYSTATUSEX lpBuffer);
DWORD WINAPI GetTickCount(void);
ULONGLONG WINAPI GetTickCount64(void);
void WINAPI GetSystemTime(LPSYSTEMTIME lpSystemTime);
void WINAPI GetLocalTime(LPSYSTEMTIME lpSystemTime);
void WINAPI GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime);
void WINAPI GetSystemTimePreciseAsFileTime(LPFILETIME lpSystemTimeAsFileTime);
BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME* lpSystemTime, LPFILETIME lpFileTime);
BOOL WINAPI FileTimeToSystemTime(const FILETIME* lpFileTime, LPSYSTEMTIME lpSystemTime);
BOOL WINAPI GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer, PDWORD ReturnedLength);
DWORD WINAPI GetActiveProcessorCount(WORD GroupNumber);
WORD WINAPI GetActiveProcessorGroupCount(void);
BOOL WINAPI GetVersionExW(LPOSVERSIONINFOEXW lpVersionInformation);
UINT WINAPI GetSystemDirectoryW(LPWSTR lpBuffer, UINT uSize);
UINT WINAPI GetWindowsDirectoryW(LPWSTR lpBuffer, UINT uSize);
/* Added by os/ (S1.1); realtimeapiset.h in the SDK. */
BOOL WINAPI QueryUnbiasedInterruptTime(PULONGLONG UnbiasedTime);
GTA6RT_END_C
