#pragma once

/* winbase.h proper: the misc kernel32 surface not in an api-set header. Implemented by os/. */

#include <minwinbase.h>

#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_COPY_ALLOWED 0x00000002
#define MOVEFILE_DELAY_UNTIL_REBOOT 0x00000004
#define MOVEFILE_WRITE_THROUGH 0x00000008
#define COPY_FILE_FAIL_IF_EXISTS 0x00000001
#define COPY_FILE_RESTARTABLE 0x00000002
#define COPY_FILE_OPEN_SOURCE_FOR_WRITE 0x00000004
#define COPY_FILE_ALLOW_DECRYPTED_DESTINATION 0x00000008
#define COPY_FILE_COPY_SYMLINK 0x00000800
#define COPY_FILE_NO_BUFFERING 0x00001000
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x00000200
#define FORMAT_MESSAGE_FROM_STRING 0x00000400
#define FORMAT_MESSAGE_FROM_HMODULE 0x00000800
#define FORMAT_MESSAGE_FROM_SYSTEM 0x00001000
#define FORMAT_MESSAGE_ARGUMENT_ARRAY 0x00002000

typedef DWORD(WINAPI* LPPROGRESS_ROUTINE)(LARGE_INTEGER TotalFileSize, LARGE_INTEGER TotalBytesTransferred,
                                          LARGE_INTEGER StreamSize, LARGE_INTEGER StreamBytesTransferred,
                                          DWORD dwStreamNumber, DWORD dwCallbackReason, HANDLE hSourceFile,
                                          HANDLE hDestinationFile, LPVOID lpData);

typedef struct _COPYFILE2_EXTENDED_PARAMETERS {
    DWORD dwSize;
    DWORD dwCopyFlags;
    BOOL* pfCancel;
    void* pProgressRoutine;
    PVOID pvCallbackContext;
} COPYFILE2_EXTENDED_PARAMETERS;

typedef enum _POWER_REQUEST_TYPE {
    PowerRequestDisplayRequired,
    PowerRequestSystemRequired,
    PowerRequestAwayModeRequired,
    PowerRequestExecutionRequired
} POWER_REQUEST_TYPE;
#define POWER_REQUEST_CONTEXT_VERSION 0
#define POWER_REQUEST_CONTEXT_SIMPLE_STRING 0x00000001
typedef struct _REASON_CONTEXT {
    ULONG Version;
    DWORD Flags;
    union {
        struct {
            HMODULE LocalizedReasonModule;
            ULONG LocalizedReasonId;
            ULONG ReasonStringCount;
            LPWSTR* ReasonStrings;
        } Detailed;
        LPWSTR SimpleReasonString;
    } Reason;
} REASON_CONTEXT, *PREASON_CONTEXT;

GTA6RT_BEGIN_C
BOOL WINAPI MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags);
BOOL WINAPI CopyFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, BOOL bFailIfExists);
BOOL WINAPI CopyFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, LPPROGRESS_ROUTINE lpProgressRoutine,
                        LPVOID lpData, LPBOOL pbCancel, DWORD dwCopyFlags);
HRESULT WINAPI CopyFile2(PCWSTR pwszExistingFileName, PCWSTR pwszNewFileName,
                         COPYFILE2_EXTENDED_PARAMETERS* pExtendedParameters);
BOOL WINAPI CreateDirectoryExW(LPCWSTR lpTemplateDirectory, LPCWSTR lpNewDirectory,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes);
DWORD WINAPI FormatMessageW(DWORD dwFlags, LPCVOID lpSource, DWORD dwMessageId, DWORD dwLanguageId, LPWSTR lpBuffer,
                            DWORD nSize, va_list* Arguments);
DWORD WINAPI FormatMessageA(DWORD dwFlags, LPCVOID lpSource, DWORD dwMessageId, DWORD dwLanguageId, LPSTR lpBuffer,
                            DWORD nSize, va_list* Arguments);
HANDLE WINAPI PowerCreateRequest(PREASON_CONTEXT Context);
BOOL WINAPI PowerSetRequest(HANDLE PowerRequest, POWER_REQUEST_TYPE RequestType);
BOOL WINAPI PowerClearRequest(HANDLE PowerRequest, POWER_REQUEST_TYPE RequestType);
EXECUTION_STATE WINAPI SetThreadExecutionState(EXECUTION_STATE esFlags);
BOOL WINAPI LookupPrivilegeValueW(LPCWSTR lpSystemName, LPCWSTR lpName, PLUID lpLuid);
BOOL WINAPI AdjustTokenPrivileges(HANDLE TokenHandle, BOOL DisableAllPrivileges, PTOKEN_PRIVILEGES NewState,
                                  DWORD BufferLength, PTOKEN_PRIVILEGES PreviousState, PDWORD ReturnLength);
BOOL WINAPI PrivilegeCheck(HANDLE ClientToken, PPRIVILEGE_SET RequiredPrivileges, LPBOOL pfResult);
BOOL WINAPI IsBadReadPtr(const void* lp, UINT_PTR ucb);
BOOL WINAPI IsBadWritePtr(LPVOID lp, UINT_PTR ucb);
void WINAPI InitializeSListHead(PSLIST_HEADER ListHead);
PSLIST_ENTRY WINAPI InterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY ListEntry);
PSLIST_ENTRY WINAPI InterlockedPopEntrySList(PSLIST_HEADER ListHead);
PSLIST_ENTRY WINAPI InterlockedFlushSList(PSLIST_HEADER ListHead);
USHORT WINAPI QueryDepthSList(PSLIST_HEADER ListHead);
/* Added by os/ (S1.1). */
int WINAPI lstrcmpW(LPCWSTR lpString1, LPCWSTR lpString2);
BOOLEAN WINAPI CreateSymbolicLinkW(LPCWSTR lpSymlinkFileName, LPCWSTR lpTargetFileName, DWORD dwFlags);
BOOL WINAPI CreateHardLinkW(LPCWSTR lpFileName, LPCWSTR lpExistingFileName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
GTA6RT_END_C

#define SE_LOCK_MEMORY_NAME u"SeLockMemoryPrivilege"
#define ES_SYSTEM_REQUIRED ((DWORD)0x00000001)
#define ES_DISPLAY_REQUIRED ((DWORD)0x00000002)
#define ES_CONTINUOUS ((DWORD)0x80000000)

#if defined(UNICODE)
#define MoveFileEx MoveFileExW
#define CopyFile CopyFileW
#define CopyFileEx CopyFileExW
#define CreateDirectoryEx CreateDirectoryExW
#define FormatMessage FormatMessageW
#define LookupPrivilegeValue LookupPrivilegeValueW
#endif
