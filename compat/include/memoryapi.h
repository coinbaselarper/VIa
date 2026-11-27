#pragma once
#include <minwinbase.h>

#define FILE_MAP_WRITE 0x0002
#define FILE_MAP_READ 0x0004
#define FILE_MAP_ALL_ACCESS 0x000F001F
#define FILE_MAP_EXECUTE 0x0020
#define FILE_MAP_COPY 0x00000001

typedef struct _WIN32_MEMORY_RANGE_ENTRY {
    PVOID VirtualAddress;
    SIZE_T NumberOfBytes;
} WIN32_MEMORY_RANGE_ENTRY, *PWIN32_MEMORY_RANGE_ENTRY;

/* Implemented by os/ on top of gta6rt_vm_* (include/gta6rt/os.h). */
GTA6RT_BEGIN_C
LPVOID WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
LPVOID WINAPI VirtualAllocEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL WINAPI VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
BOOL WINAPI VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect);
SIZE_T WINAPI VirtualQuery(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength);
BOOL WINAPI VirtualLock(LPVOID lpAddress, SIZE_T dwSize);
BOOL WINAPI VirtualUnlock(LPVOID lpAddress, SIZE_T dwSize);
UINT WINAPI GetWriteWatch(DWORD dwFlags, PVOID lpBaseAddress, SIZE_T dwRegionSize, PVOID* lpAddresses,
                          ULONG_PTR* lpdwCount, LPDWORD lpdwGranularity);
UINT WINAPI ResetWriteWatch(LPVOID lpBaseAddress, SIZE_T dwRegionSize);
BOOL WINAPI ReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize,
                              SIZE_T* lpNumberOfBytesRead);
BOOL WINAPI WriteProcessMemory(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize,
                               SIZE_T* lpNumberOfBytesWritten);
BOOL WINAPI FlushInstructionCache(HANDLE hProcess, LPCVOID lpBaseAddress, SIZE_T dwSize);
BOOL WINAPI PrefetchVirtualMemory(HANDLE hProcess, ULONG_PTR NumberOfEntries, PWIN32_MEMORY_RANGE_ENTRY VirtualAddresses,
                                  ULONG Flags);
HANDLE WINAPI CreateFileMappingW(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect,
                                 DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCWSTR lpName);
LPVOID WINAPI MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
                            DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
LPVOID WINAPI MapViewOfFileEx(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
                              DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap, LPVOID lpBaseAddress);
BOOL WINAPI UnmapViewOfFile(LPCVOID lpBaseAddress);
BOOL WINAPI FlushViewOfFile(LPCVOID lpBaseAddress, SIZE_T dwNumberOfBytesToFlush);
GTA6RT_END_C

/* Added by os/ (S1.1): VirtualAlloc2 / MapViewOfFile3 placeholder API and the remaining views. */
typedef enum MEM_EXTENDED_PARAMETER_TYPE {
    MemExtendedParameterInvalidType = 0,
    MemExtendedParameterAddressRequirements,
    MemExtendedParameterNumaNode,
    MemExtendedParameterPartitionHandle,
    MemExtendedParameterUserPhysicalHandle,
    MemExtendedParameterAttributeFlags,
    MemExtendedParameterImageMachine,
    MemExtendedParameterMax
} MEM_EXTENDED_PARAMETER_TYPE, *PMEM_EXTENDED_PARAMETER_TYPE;

typedef struct DECLSPEC_ALIGN(8) MEM_EXTENDED_PARAMETER {
    struct {
        DWORD64 Type : 8;
        DWORD64 Reserved : 56;
    };
    union {
        DWORD64 ULong64;
        PVOID Pointer;
        SIZE_T Size;
        HANDLE Handle;
        DWORD ULong;
    };
} MEM_EXTENDED_PARAMETER, *PMEM_EXTENDED_PARAMETER;

typedef struct _MEM_ADDRESS_REQUIREMENTS {
    PVOID LowestStartingAddress;
    PVOID HighestEndingAddress;
    SIZE_T Alignment;
} MEM_ADDRESS_REQUIREMENTS, *PMEM_ADDRESS_REQUIREMENTS;

GTA6RT_BEGIN_C
SIZE_T WINAPI VirtualQueryEx(HANDLE hProcess, LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength);
PVOID WINAPI VirtualAlloc2(HANDLE Process, PVOID BaseAddress, SIZE_T Size, ULONG AllocationType, ULONG PageProtection,
                           MEM_EXTENDED_PARAMETER* ExtendedParameters, ULONG ParameterCount);
PVOID WINAPI MapViewOfFile3(HANDLE FileMapping, HANDLE Process, PVOID BaseAddress, ULONG64 Offset, SIZE_T ViewSize,
                            ULONG AllocationType, ULONG PageProtection, MEM_EXTENDED_PARAMETER* ExtendedParameters,
                            ULONG ParameterCount);
BOOL WINAPI UnmapViewOfFile2(HANDLE Process, PVOID BaseAddress, ULONG UnmapFlags);
HANDLE WINAPI CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect,
                                 DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName);
HANDLE WINAPI OpenFileMappingW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpName);
GTA6RT_END_C
#define CreateFileMapping CreateFileMappingW
