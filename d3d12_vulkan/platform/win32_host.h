#pragma once

// Force-included into the ported leonida sources of this module. Every kernel32/user32 call they make is renamed
// to an rthost_ function (win32_host.cpp), so the archive references no Win32 symbol of os/ and links before os/
// exists. Handles: objects of os/ (guest events, threads) are used through include/gta6rt/os.h; when os/ returns
// GTA6RT_E_NOT_IMPLEMENTED the call falls back to a private host object whose HANDLE carries kPrivateHandleTag.
// wchar_t (UTF-32 on macOS) and WCHAR (char16_t) strings are both accepted, since leonida passes L"..." literals.

// SDK headers that use identifiers compat/sal.h turns into macros (__reserved) must be parsed first.
#include <malloc/malloc.h>
#include <mach/mach_types.h>

#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <unknwn.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <threadpoolapiset.h>
#include <intrin.h>
#include <malloc.h>
#include <process.h>
#include <io.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>

#ifdef __cplusplus

// A wide-string argument in any of the spellings leonida passes: L"..." (UTF-32 wchar_t here), WCHAR (UTF-16) or a
// std::filesystem::path::c_str(), which is UTF-8 char on macOS.
struct rthost_wstr {
    const wchar_t* w = nullptr;
    const char16_t* u = nullptr;
    const char* a = nullptr;
    rthost_wstr(std::nullptr_t) {}
    rthost_wstr(const wchar_t* s) : w(s) {}
    rthost_wstr(const char16_t* s) : u(s) {}
    rthost_wstr(const char* s) : a(s) {}
    bool null() const { return !w && !u && !a; }
    std::string utf8(int count = -1) const;
};

struct rthost_wbuf {
    wchar_t* w = nullptr;
    char16_t* u = nullptr;
    rthost_wbuf(std::nullptr_t) {}
    rthost_wbuf(wchar_t* s) : w(s) {}
    rthost_wbuf(char16_t* s) : u(s) {}
    bool null() const { return !w && !u; }
};

constexpr std::uintptr_t kRthostPrivateHandleTag = std::uintptr_t(1) << 62;

DWORD rthost_GetLastError();
void rthost_SetLastError(DWORD code);
DWORD rthost_GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size);
DWORD rthost_GetEnvironmentVariableW(rthost_wstr name, rthost_wbuf buffer, DWORD size);

BOOL rthost_QueryPerformanceCounter(LARGE_INTEGER* out);
BOOL rthost_QueryPerformanceFrequency(LARGE_INTEGER* out);
ULONGLONG rthost_GetTickCount64();
DWORD rthost_GetTickCount();
void rthost_GetSystemTimeAsFileTime(LPFILETIME out);
void rthost_GetLocalTime(LPSYSTEMTIME out);
void rthost_Sleep(DWORD ms);
DWORD rthost_SleepEx(DWORD ms, BOOL alertable);
BOOL rthost_SwitchToThread();

DWORD rthost_GetCurrentThreadId();
DWORD rthost_GetCurrentProcessId();
HANDLE rthost_GetCurrentThread();
HANDLE rthost_GetCurrentProcess();
HANDLE rthost_CreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID param,
                           DWORD flags, LPDWORD id);
BOOL rthost_SetThreadPriority(HANDLE thread, int priority);
HRESULT rthost_SetThreadDescription(HANDLE thread, rthost_wstr name);

void rthost_InitializeSRWLock(PSRWLOCK lock);
void rthost_AcquireSRWLockExclusive(PSRWLOCK lock);
void rthost_AcquireSRWLockShared(PSRWLOCK lock);
void rthost_ReleaseSRWLockExclusive(PSRWLOCK lock);
void rthost_ReleaseSRWLockShared(PSRWLOCK lock);
BOOLEAN rthost_TryAcquireSRWLockExclusive(PSRWLOCK lock);
BOOLEAN rthost_TryAcquireSRWLockShared(PSRWLOCK lock);

HANDLE rthost_CreateEventW(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial, rthost_wstr name);
HANDLE rthost_CreateEventA(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial, LPCSTR name);
BOOL rthost_SetEvent(HANDLE event);
BOOL rthost_ResetEvent(HANDLE event);
HANDLE rthost_CreateSemaphoreW(LPSECURITY_ATTRIBUTES attributes, LONG initial, LONG maximum, rthost_wstr name);
BOOL rthost_ReleaseSemaphore(HANDLE semaphore, LONG count, LPLONG previous);
DWORD rthost_WaitForSingleObject(HANDLE handle, DWORD ms);
DWORD rthost_WaitForSingleObjectEx(HANDLE handle, DWORD ms, BOOL alertable);
DWORD rthost_WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL wait_all, DWORD ms);
BOOL rthost_CloseHandle(HANDLE handle);
BOOL rthost_DuplicateHandle(HANDLE source_process, HANDLE source, HANDLE target_process, LPHANDLE target, DWORD access,
                            BOOL inherit, DWORD options);
BOOL rthost_GetHandleInformation(HANDLE handle, LPDWORD flags);
HANDLE rthost_CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES attributes, rthost_wstr name, DWORD flags, DWORD access);
BOOL rthost_SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE routine,
                             LPVOID arg, BOOL resume);
PTP_WAIT rthost_CreateThreadpoolWait(PTP_WAIT_CALLBACK callback, PVOID context, PTP_CALLBACK_ENVIRON environment);
void rthost_SetThreadpoolWait(PTP_WAIT wait, HANDLE handle, PFILETIME timeout);
void rthost_CloseThreadpoolWait(PTP_WAIT wait);
void rthost_WaitForThreadpoolWaitCallbacks(PTP_WAIT wait, BOOL cancel_pending);

SIZE_T rthost_VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION out, SIZE_T length);
BOOL rthost_VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old_protect);
LPVOID rthost_VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect);
BOOL rthost_VirtualFree(LPVOID address, SIZE_T size, DWORD type);
UINT rthost_GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID* addresses, ULONG_PTR* count, LPDWORD granularity);
BOOL rthost_ReadProcessMemory(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T size, SIZE_T* read);
BOOL rthost_IsBadReadPtr(const void* address, UINT_PTR size);
void rthost_GetSystemInfo(LPSYSTEM_INFO out);
BOOL rthost_GlobalMemoryStatusEx(LPMEMORYSTATUSEX out);
BOOL rthost_GetProcessMemoryInfo(HANDLE process, PPROCESS_MEMORY_COUNTERS out, DWORD size);
HANDLE rthost_GetProcessHeap();
HANDLE rthost_HeapCreate(DWORD options, SIZE_T initial, SIZE_T maximum);
LPVOID rthost_HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
BOOL rthost_HeapFree(HANDLE heap, DWORD flags, LPVOID memory);
PVOID rthost_AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
ULONG rthost_RemoveVectoredExceptionHandler(PVOID handle);

HANDLE rthost_CreateFileW(rthost_wstr name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes,
                          DWORD disposition, DWORD flags, HANDLE template_file);
HANDLE rthost_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes,
                          DWORD disposition, DWORD flags, HANDLE template_file);
BOOL rthost_ReadFile(HANDLE file, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped);
BOOL rthost_WriteFile(HANDLE file, LPCVOID buffer, DWORD bytes, LPDWORD written, LPOVERLAPPED overlapped);
BOOL rthost_ReadFileEx(HANDLE file, LPVOID buffer, DWORD bytes, LPOVERLAPPED overlapped,
                       LPOVERLAPPED_COMPLETION_ROUTINE routine);
BOOL rthost_ReadFileScatter(HANDLE file, FILE_SEGMENT_ELEMENT segments[], DWORD bytes, LPDWORD reserved,
                            LPOVERLAPPED overlapped);
BOOL rthost_SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER new_pointer, DWORD method);
BOOL rthost_SetEndOfFile(HANDLE file);
BOOL rthost_GetFileSizeEx(HANDLE file, PLARGE_INTEGER size);
BOOL rthost_FlushFileBuffers(HANDLE file);
BOOL rthost_DeleteFileW(rthost_wstr name);
BOOL rthost_MoveFileExW(rthost_wstr from, rthost_wstr to, DWORD flags);
DWORD rthost_GetFileAttributesW(rthost_wstr name);
BOOL rthost_CreateDirectoryW(rthost_wstr name, LPSECURITY_ATTRIBUTES attributes);
HANDLE rthost_CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protect, DWORD size_high,
                                 DWORD size_low, rthost_wstr name);
LPVOID rthost_MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T bytes);
BOOL rthost_UnmapViewOfFile(LPCVOID address);
BOOL rthost_FlushViewOfFile(LPCVOID address, SIZE_T bytes);
HANDLE rthost_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data);
BOOL rthost_FindNextFileA(HANDLE find, LPWIN32_FIND_DATAA data);
BOOL rthost_FindClose(HANDLE find);
HANDLE rthost_GetStdHandle(DWORD which);

HMODULE rthost_GetModuleHandleW(rthost_wstr name);
HMODULE rthost_GetModuleHandleA(LPCSTR name);
HMODULE rthost_LoadLibraryW(rthost_wstr name);
HMODULE rthost_LoadLibraryA(LPCSTR name);
HMODULE rthost_LoadLibraryExW(rthost_wstr name, HANDLE file, DWORD flags);
HMODULE rthost_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags);
FARPROC rthost_GetProcAddress(HMODULE module, LPCSTR name);
BOOL rthost_FreeLibrary(HMODULE module);
DWORD rthost_GetModuleFileNameW(HMODULE module, rthost_wbuf buffer, DWORD size);
DWORD rthost_GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size);

int rthost_MultiByteToWideChar(UINT code_page, DWORD flags, LPCCH text, int bytes, rthost_wbuf out, int out_chars);
int rthost_WideCharToMultiByte(UINT code_page, DWORD flags, rthost_wstr text, int chars, LPSTR out, int out_bytes,
                               LPCCH default_char, LPBOOL used_default);
void rthost_OutputDebugStringA(LPCSTR text);
void rthost_OutputDebugStringW(rthost_wstr text);

BOOL rthost_OpenProcessToken(HANDLE process, DWORD access, PHANDLE token);
BOOL rthost_OpenThreadToken(HANDLE thread, DWORD access, BOOL self, PHANDLE token);
BOOL rthost_LookupPrivilegeValueW(rthost_wstr system, rthost_wstr name, PLUID luid);
BOOL rthost_PrivilegeCheck(HANDLE token, PPRIVILEGE_SET privileges, LPBOOL result);

HRESULT rthost_CoInitializeEx(LPVOID reserved, DWORD flags);
void rthost_CoUninitialize();
HRESULT rthost_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID riid, LPVOID* out);

// user32, the part this module needs, over platform/mac_window.h (an HWND is a gta6rt_mac_window*).
BOOL rthost_GetClientRect(HWND window, LPRECT rect);
BOOL rthost_IsWindow(HWND window);
BOOL rthost_IsWindowVisible(HWND window);

#define GetClientRect rthost_GetClientRect
#define IsWindow rthost_IsWindow
#define IsWindowVisible rthost_IsWindowVisible
#define GetLastError rthost_GetLastError
#define SetLastError rthost_SetLastError
#define GetEnvironmentVariableA rthost_GetEnvironmentVariableA
#define GetEnvironmentVariableW rthost_GetEnvironmentVariableW
#define QueryPerformanceCounter rthost_QueryPerformanceCounter
#define QueryPerformanceFrequency rthost_QueryPerformanceFrequency
#define GetTickCount64 rthost_GetTickCount64
#define GetTickCount rthost_GetTickCount
#define GetSystemTimeAsFileTime rthost_GetSystemTimeAsFileTime
#define GetLocalTime rthost_GetLocalTime
#define Sleep rthost_Sleep
#define SleepEx rthost_SleepEx
#define SwitchToThread rthost_SwitchToThread
#define GetCurrentThreadId rthost_GetCurrentThreadId
#define GetCurrentProcessId rthost_GetCurrentProcessId
#define GetCurrentThread rthost_GetCurrentThread
#define GetCurrentProcess rthost_GetCurrentProcess
#define CreateThread rthost_CreateThread
#define SetThreadPriority rthost_SetThreadPriority
#define SetThreadDescription rthost_SetThreadDescription
#define InitializeSRWLock rthost_InitializeSRWLock
#define AcquireSRWLockExclusive rthost_AcquireSRWLockExclusive
#define AcquireSRWLockShared rthost_AcquireSRWLockShared
#define ReleaseSRWLockExclusive rthost_ReleaseSRWLockExclusive
#define ReleaseSRWLockShared rthost_ReleaseSRWLockShared
#define TryAcquireSRWLockExclusive rthost_TryAcquireSRWLockExclusive
#define TryAcquireSRWLockShared rthost_TryAcquireSRWLockShared
#define CreateEventW rthost_CreateEventW
#define CreateEventA rthost_CreateEventA
#define SetEvent rthost_SetEvent
#define ResetEvent rthost_ResetEvent
#define CreateSemaphoreW rthost_CreateSemaphoreW
#define ReleaseSemaphore rthost_ReleaseSemaphore
#define WaitForSingleObject rthost_WaitForSingleObject
#define WaitForSingleObjectEx rthost_WaitForSingleObjectEx
#define WaitForMultipleObjects rthost_WaitForMultipleObjects
#define CloseHandle rthost_CloseHandle
#define DuplicateHandle rthost_DuplicateHandle
#define GetHandleInformation rthost_GetHandleInformation
#define CreateWaitableTimerExW rthost_CreateWaitableTimerExW
#define SetWaitableTimer rthost_SetWaitableTimer
#define CreateThreadpoolWait rthost_CreateThreadpoolWait
#define SetThreadpoolWait rthost_SetThreadpoolWait
#define CloseThreadpoolWait rthost_CloseThreadpoolWait
#define WaitForThreadpoolWaitCallbacks rthost_WaitForThreadpoolWaitCallbacks
#define VirtualQuery rthost_VirtualQuery
#define VirtualProtect rthost_VirtualProtect
#define VirtualAlloc rthost_VirtualAlloc
#define VirtualFree rthost_VirtualFree
#define GetWriteWatch rthost_GetWriteWatch
#define ReadProcessMemory rthost_ReadProcessMemory
#define IsBadReadPtr rthost_IsBadReadPtr
#define GetSystemInfo rthost_GetSystemInfo
#define GlobalMemoryStatusEx rthost_GlobalMemoryStatusEx
#define GetProcessMemoryInfo rthost_GetProcessMemoryInfo
#define K32GetProcessMemoryInfo rthost_GetProcessMemoryInfo
#define GetProcessHeap rthost_GetProcessHeap
#define HeapCreate rthost_HeapCreate
#define HeapAlloc rthost_HeapAlloc
#define HeapFree rthost_HeapFree
#define AddVectoredExceptionHandler rthost_AddVectoredExceptionHandler
#define RemoveVectoredExceptionHandler rthost_RemoveVectoredExceptionHandler
#define CreateFileW rthost_CreateFileW
#define CreateFileA rthost_CreateFileA
#define ReadFile rthost_ReadFile
#define WriteFile rthost_WriteFile
#define ReadFileEx rthost_ReadFileEx
#define ReadFileScatter rthost_ReadFileScatter
#define SetFilePointerEx rthost_SetFilePointerEx
#define SetEndOfFile rthost_SetEndOfFile
#define GetFileSizeEx rthost_GetFileSizeEx
#define FlushFileBuffers rthost_FlushFileBuffers
#define DeleteFileW rthost_DeleteFileW
#define MoveFileExW rthost_MoveFileExW
#define GetFileAttributesW rthost_GetFileAttributesW
#define CreateDirectoryW rthost_CreateDirectoryW
#define CreateFileMappingW rthost_CreateFileMappingW
#define MapViewOfFile rthost_MapViewOfFile
#define UnmapViewOfFile rthost_UnmapViewOfFile
#define FlushViewOfFile rthost_FlushViewOfFile
#define FindFirstFileA rthost_FindFirstFileA
#define FindNextFileA rthost_FindNextFileA
#define FindClose rthost_FindClose
#define GetStdHandle rthost_GetStdHandle
#define GetModuleHandleW rthost_GetModuleHandleW
#define GetModuleHandleA rthost_GetModuleHandleA
#define LoadLibraryW rthost_LoadLibraryW
#define LoadLibraryA rthost_LoadLibraryA
#define LoadLibraryExW rthost_LoadLibraryExW
#define LoadLibraryExA rthost_LoadLibraryExA
#define GetProcAddress rthost_GetProcAddress
#define FreeLibrary rthost_FreeLibrary
#define GetModuleFileNameW rthost_GetModuleFileNameW
#define GetModuleFileNameA rthost_GetModuleFileNameA
#define MultiByteToWideChar rthost_MultiByteToWideChar
#define WideCharToMultiByte rthost_WideCharToMultiByte
#define OutputDebugStringA rthost_OutputDebugStringA
#define OutputDebugStringW rthost_OutputDebugStringW
#define OpenProcessToken rthost_OpenProcessToken
#define OpenThreadToken rthost_OpenThreadToken
#define LookupPrivilegeValueW rthost_LookupPrivilegeValueW
#define PrivilegeCheck rthost_PrivilegeCheck
#define CoInitializeEx rthost_CoInitializeEx
#define CoUninitialize rthost_CoUninitialize
#define CoCreateInstance rthost_CoCreateInstance

namespace rthost {
// Copies [from, from + bytes) unless a page of either range is not accessible; false then, nothing faults.
bool safe_copy(void* to, const void* from, std::size_t bytes);
bool readable(const void* from, std::size_t bytes);
bool writable(void* to, std::size_t bytes);
// platform/fault_guard: a copy or compare that a fault ends with false (-1) instead of killing the process.
bool guarded_copy(void* to, const void* from, std::size_t bytes);
int guarded_compare(const void* a, const void* b, std::size_t bytes);
std::uint64_t fault_guard_recovered();
bool guarded_gather(void* to, const void* from, std::size_t count, std::size_t stride);
std::string to_utf8(const wchar_t* text, int count = -1);
std::string to_utf8(const char16_t* text, int count = -1);
std::wstring to_wide(const char* utf8);
// Windows-style path (backslashes, "Z:" prefixes) as a host path, through os/ when it translates paths.
std::string host_path(const std::string& path);
}  // namespace rthost

#endif  // __cplusplus
