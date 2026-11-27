#pragma once

/* Structures shared by the api-set headers (Windows x64 layouts, asserted in compat/tests). */

#include <winnt.h>

typedef struct _SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef struct _OVERLAPPED_ENTRY {
    ULONG_PTR lpCompletionKey;
    LPOVERLAPPED lpOverlapped;
    ULONG_PTR Internal;
    DWORD dwNumberOfBytesTransferred;
} OVERLAPPED_ENTRY, *LPOVERLAPPED_ENTRY;

typedef void(WINAPI* LPOVERLAPPED_COMPLETION_ROUTINE)(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered,
                                                      LPOVERLAPPED lpOverlapped);

typedef struct _SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[MAX_PATH];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *PWIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

typedef enum _FINDEX_INFO_LEVELS { FindExInfoStandard, FindExInfoBasic, FindExInfoMaxInfoLevel } FINDEX_INFO_LEVELS;
typedef enum _FINDEX_SEARCH_OPS {
    FindExSearchNameMatch,
    FindExSearchLimitToDirectories,
    FindExSearchLimitToDevices,
    FindExSearchMaxSearchOp
} FINDEX_SEARCH_OPS;
#define FIND_FIRST_EX_CASE_SENSITIVE 0x00000001
#define FIND_FIRST_EX_LARGE_FETCH 0x00000002

typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel } GET_FILEEX_INFO_LEVELS;

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *PBY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;

typedef struct _RTL_CRITICAL_SECTION_DEBUG* PRTL_CRITICAL_SECTION_DEBUG;
typedef struct _RTL_CRITICAL_SECTION {
    PRTL_CRITICAL_SECTION_DEBUG DebugInfo;
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
    HANDLE LockSemaphore;
    ULONG_PTR SpinCount;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;
typedef RTL_CRITICAL_SECTION CRITICAL_SECTION, *PCRITICAL_SECTION, *LPCRITICAL_SECTION;

typedef struct _RTL_SRWLOCK {
    PVOID Ptr;
} RTL_SRWLOCK, SRWLOCK, *PSRWLOCK;
#define RTL_SRWLOCK_INIT {0}
#define SRWLOCK_INIT RTL_SRWLOCK_INIT

typedef struct _RTL_CONDITION_VARIABLE {
    PVOID Ptr;
} RTL_CONDITION_VARIABLE, CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT {0}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1

typedef union _RTL_RUN_ONCE {
    PVOID Ptr;
} RTL_RUN_ONCE, INIT_ONCE, *PINIT_ONCE, *LPINIT_ONCE;
#define INIT_ONCE_STATIC_INIT {0}
#define INIT_ONCE_CHECK_ONLY 0x00000001UL
#define INIT_ONCE_ASYNC 0x00000002UL
#define INIT_ONCE_INIT_FAILED 0x00000004UL
typedef BOOL(WINAPI* PINIT_ONCE_FN)(PINIT_ONCE InitOnce, PVOID Parameter, PVOID* Context);

typedef struct _RTL_BARRIER {
    DWORD Reserved1;
    DWORD Reserved2;
    ULONG_PTR Reserved3[2];
    DWORD Reserved4;
    DWORD Reserved5;
} RTL_BARRIER, SYNCHRONIZATION_BARRIER, *PSYNCHRONIZATION_BARRIER;

typedef DWORD(WINAPI* PTHREAD_START_ROUTINE)(LPVOID lpThreadParameter);
typedef PTHREAD_START_ROUTINE LPTHREAD_START_ROUTINE;
typedef void(NTAPI* PFLS_CALLBACK_FUNCTION)(PVOID lpFlsData);
typedef void(NTAPI* PAPCFUNC)(ULONG_PTR Parameter);
typedef void(CALLBACK* PTIMERAPCROUTINE)(LPVOID lpArgToCompletionRoutine, DWORD dwTimerLowValue,
                                         DWORD dwTimerHighValue);

#define WAIT_FAILED ((DWORD)0xFFFFFFFF)
#define WAIT_OBJECT_0 ((STATUS_WAIT_0) + 0)
#define WAIT_ABANDONED ((STATUS_ABANDONED_WAIT_0) + 0)
#define WAIT_ABANDONED_0 ((STATUS_ABANDONED_WAIT_0) + 0)
#define WAIT_IO_COMPLETION STATUS_USER_APC
#define WAIT_TIMEOUT 258L
#define MAXIMUM_WAIT_OBJECTS 64
#define INFINITE 0xFFFFFFFF
#define STILL_ACTIVE STATUS_PENDING

#define LMEM_FIXED 0x0000
#define LMEM_ZEROINIT 0x0040
#define LPTR (LMEM_FIXED | LMEM_ZEROINIT)
#define GMEM_FIXED 0x0000
#define GMEM_ZEROINIT 0x0040
#define GPTR (GMEM_FIXED | GMEM_ZEROINIT)
