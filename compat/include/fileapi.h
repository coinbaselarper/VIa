#pragma once
#include <minwinbase.h>

#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_FLAG_POSIX_SEMANTICS 0x01000000
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000
#define FILE_FLAG_OPEN_NO_RECALL 0x00100000
#define FILE_TYPE_UNKNOWN 0x0000
#define FILE_TYPE_DISK 0x0001
#define FILE_TYPE_CHAR 0x0002
#define FILE_TYPE_PIPE 0x0003

typedef union _FILE_SEGMENT_ELEMENT {
    PVOID64 Buffer;
    ULONGLONG Alignment;
} FILE_SEGMENT_ELEMENT, *PFILE_SEGMENT_ELEMENT;

typedef struct _CREATEFILE2_EXTENDED_PARAMETERS {
    DWORD dwSize;
    DWORD dwFileAttributes;
    DWORD dwFileFlags;
    DWORD dwSecurityQosFlags;
    LPSECURITY_ATTRIBUTES lpSecurityAttributes;
    HANDLE hTemplateFile;
} CREATEFILE2_EXTENDED_PARAMETERS, *PCREATEFILE2_EXTENDED_PARAMETERS, *LPCREATEFILE2_EXTENDED_PARAMETERS;

typedef enum _FILE_INFO_BY_HANDLE_CLASS {
    FileBasicInfo,
    FileStandardInfo,
    FileNameInfo,
    FileRenameInfo,
    FileDispositionInfo,
    FileAllocationInfo,
    FileEndOfFileInfo,
    FileStreamInfo,
    FileCompressionInfo,
    FileAttributeTagInfo,
    FileIdBothDirectoryInfo,
    FileIdBothDirectoryRestartInfo,
    FileIoPriorityHintInfo,
    FileRemoteProtocolInfo,
    FileFullDirectoryInfo,
    FileFullDirectoryRestartInfo,
    FileStorageInfo,
    FileAlignmentInfo,
    FileIdInfo,
    FileIdExtdDirectoryInfo,
    FileIdExtdDirectoryRestartInfo,
    FileDispositionInfoEx,
    FileRenameInfoEx,
    MaximumFileInfoByHandleClass
} FILE_INFO_BY_HANDLE_CLASS;

typedef struct _FILE_STANDARD_INFO {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    DWORD NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
} FILE_STANDARD_INFO, *PFILE_STANDARD_INFO;

/* Implemented by os/ (paths: UTF-16 guest/Windows form, mapped by shims_core file_system). */
GTA6RT_BEGIN_C
HANDLE WINAPI CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                          LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                          DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
HANDLE WINAPI CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                          LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                          DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
HANDLE WINAPI CreateFile2(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwCreationDisposition,
                          LPCREATEFILE2_EXTENDED_PARAMETERS pCreateExParams);
BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead,
                     LPOVERLAPPED lpOverlapped);
BOOL WINAPI ReadFileEx(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPOVERLAPPED lpOverlapped,
                       LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
BOOL WINAPI ReadFileScatter(HANDLE hFile, FILE_SEGMENT_ELEMENT aSegmentArray[], DWORD nNumberOfBytesToRead,
                            LPDWORD lpReserved, LPOVERLAPPED lpOverlapped);
BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten,
                      LPOVERLAPPED lpOverlapped);
BOOL WINAPI WriteFileEx(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPOVERLAPPED lpOverlapped,
                        LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
BOOL WINAPI FlushFileBuffers(HANDLE hFile);
BOOL WINAPI GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize);
DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
DWORD WINAPI GetFileType(HANDLE hFile);
BOOL WINAPI SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer,
                             DWORD dwMoveMethod);
DWORD WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod);
BOOL WINAPI SetEndOfFile(HANDLE hFile);
BOOL WINAPI GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime);
BOOL WINAPI SetFileTime(HANDLE hFile, const FILETIME* lpCreationTime, const FILETIME* lpLastAccessTime,
                        const FILETIME* lpLastWriteTime);
BOOL WINAPI GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation);
BOOL WINAPI GetFileInformationByHandleEx(HANDLE hFile, FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
                                         LPVOID lpFileInformation, DWORD dwBufferSize);
DWORD WINAPI GetFileAttributesA(LPCSTR lpFileName);
DWORD WINAPI GetFileAttributesW(LPCWSTR lpFileName);
BOOL WINAPI GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
BOOL WINAPI SetFileAttributesW(LPCWSTR lpFileName, DWORD dwFileAttributes);
BOOL WINAPI DeleteFileA(LPCSTR lpFileName);
BOOL WINAPI DeleteFileW(LPCWSTR lpFileName);
BOOL WINAPI CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
BOOL WINAPI CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
BOOL WINAPI RemoveDirectoryW(LPCWSTR lpPathName);
HANDLE WINAPI FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
HANDLE WINAPI FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData);
HANDLE WINAPI FindFirstFileExA(LPCSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData,
                               FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags);
HANDLE WINAPI FindFirstFileExW(LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData,
                               FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags);
BOOL WINAPI FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
BOOL WINAPI FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData);
BOOL WINAPI FindClose(HANDLE hFindFile);
DWORD WINAPI GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR* lpFilePart);
DWORD WINAPI GetTempPathW(DWORD nBufferLength, LPWSTR lpBuffer);
BOOL WINAPI GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller,
                                PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes);
BOOL WINAPI FileTimeToLocalFileTime(const FILETIME* lpFileTime, LPFILETIME lpLocalFileTime);
LONG WINAPI CompareFileTime(const FILETIME* lpFileTime1, const FILETIME* lpFileTime2);
/* Added by os/ (S1.1). */
BOOL WINAPI LocalFileTimeToFileTime(const FILETIME* lpLocalFileTime, LPFILETIME lpFileTime);
DWORD WINAPI GetFinalPathNameByHandleW(HANDLE hFile, LPWSTR lpszFilePath, DWORD cchFilePath, DWORD dwFlags);
DWORD WINAPI GetLongPathNameW(LPCWSTR lpszShortPath, LPWSTR lpszLongPath, DWORD cchBuffer);
BOOL WINAPI SetFileInformationByHandle(HANDLE hFile, FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
                                       LPVOID lpFileInformation, DWORD dwBufferSize);
BOOL WINAPI GetVolumeInformationW(LPCWSTR lpRootPathName, LPWSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
                                  LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength,
                                  LPDWORD lpFileSystemFlags, LPWSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize);
BOOL WINAPI LockFile(HANDLE hFile, DWORD dwFileOffsetLow, DWORD dwFileOffsetHigh, DWORD nNumberOfBytesToLockLow,
                     DWORD nNumberOfBytesToLockHigh);
BOOL WINAPI UnlockFile(HANDLE hFile, DWORD dwFileOffsetLow, DWORD dwFileOffsetHigh, DWORD nNumberOfBytesToUnlockLow,
                       DWORD nNumberOfBytesToUnlockHigh);
GTA6RT_END_C

#if defined(UNICODE)
#define CreateFile CreateFileW
#define GetFileAttributes GetFileAttributesW
#define GetFileAttributesEx GetFileAttributesExW
#define SetFileAttributes SetFileAttributesW
#define DeleteFile DeleteFileW
#define CreateDirectory CreateDirectoryW
#define RemoveDirectory RemoveDirectoryW
#define FindFirstFile FindFirstFileW
#define FindFirstFileEx FindFirstFileExW
#define FindNextFile FindNextFileW
#define GetFullPathName GetFullPathNameW
#define GetTempPath GetTempPathW
typedef WIN32_FIND_DATAW WIN32_FIND_DATA;
typedef LPWIN32_FIND_DATAW LPWIN32_FIND_DATA;
#else
#define CreateFile CreateFileA
#define GetFileAttributes GetFileAttributesA
#define DeleteFile DeleteFileA
#define CreateDirectory CreateDirectoryA
#define FindFirstFile FindFirstFileA
#define FindFirstFileEx FindFirstFileExA
#define FindNextFile FindNextFileA
typedef WIN32_FIND_DATAA WIN32_FIND_DATA;
typedef LPWIN32_FIND_DATAA LPWIN32_FIND_DATA;
#endif
