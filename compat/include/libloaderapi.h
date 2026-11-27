#pragma once
#include <minwinbase.h>

#define DONT_RESOLVE_DLL_REFERENCES 0x00000001
#define LOAD_LIBRARY_AS_DATAFILE 0x00000002
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x00000008
#define LOAD_IGNORE_CODE_AUTHZ_LEVEL 0x00000010
#define LOAD_LIBRARY_AS_IMAGE_RESOURCE 0x00000020
#define LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE 0x00000040
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#define LOAD_LIBRARY_SEARCH_APPLICATION_DIR 0x00000200
#define LOAD_LIBRARY_SEARCH_USER_DIRS 0x00000400
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x00000001
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004

#define MAKEINTRESOURCEA(i) ((LPSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCEW(i) ((LPWSTR)((ULONG_PTR)((WORD)(i))))
#define IS_INTRESOURCE(r) ((((ULONG_PTR)(r)) >> 16) == 0)

/* Implemented by os/ over the module table (gta6rt_module_*), which loader/ and shims_core/ populate. */
GTA6RT_BEGIN_C
HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName);
HMODULE WINAPI GetModuleHandleW(LPCWSTR lpModuleName);
BOOL WINAPI GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule);
BOOL WINAPI GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule);
DWORD WINAPI GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize);
DWORD WINAPI GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
HMODULE WINAPI LoadLibraryA(LPCSTR lpLibFileName);
HMODULE WINAPI LoadLibraryW(LPCWSTR lpLibFileName);
HMODULE WINAPI LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
HMODULE WINAPI LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
BOOL WINAPI FreeLibrary(HMODULE hLibModule);
DECLSPEC_NORETURN void WINAPI FreeLibraryAndExitThread(HMODULE hLibModule, DWORD dwExitCode);
FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
BOOL WINAPI DisableThreadLibraryCalls(HMODULE hLibModule);
BOOL WINAPI SetDefaultDllDirectories(DWORD DirectoryFlags);
HRSRC WINAPI FindResourceW(HMODULE hModule, LPCWSTR lpName, LPCWSTR lpType);
HGLOBAL WINAPI LoadResource(HMODULE hModule, HRSRC hResInfo);
LPVOID WINAPI LockResource(HGLOBAL hResData);
DWORD WINAPI SizeofResource(HMODULE hModule, HRSRC hResInfo);
GTA6RT_END_C

#if defined(UNICODE)
#define GetModuleHandle GetModuleHandleW
#define GetModuleHandleEx GetModuleHandleExW
#define GetModuleFileName GetModuleFileNameW
#define LoadLibrary LoadLibraryW
#define LoadLibraryEx LoadLibraryExW
#define MAKEINTRESOURCE MAKEINTRESOURCEW
#else
#define GetModuleHandle GetModuleHandleA
#define GetModuleHandleEx GetModuleHandleExA
#define GetModuleFileName GetModuleFileNameA
#define LoadLibrary LoadLibraryA
#define LoadLibraryEx LoadLibraryExA
#define MAKEINTRESOURCE MAKEINTRESOURCEA
#endif
