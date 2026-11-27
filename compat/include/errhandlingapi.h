#pragma once
#include <minwinbase.h>

/* Implemented by os/ (last error) and seh/ (raise, vectored handlers, filters). */
GTA6RT_BEGIN_C
DWORD WINAPI GetLastError(void);
void WINAPI SetLastError(DWORD dwErrCode);
void WINAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments,
                           const ULONG_PTR* lpArguments);
PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
ULONG WINAPI RemoveVectoredExceptionHandler(PVOID Handle);
PVOID WINAPI AddVectoredContinueHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
ULONG WINAPI RemoveVectoredContinueHandler(PVOID Handle);
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter);
LONG WINAPI UnhandledExceptionFilter(struct _EXCEPTION_POINTERS* ExceptionInfo);
UINT WINAPI SetErrorMode(UINT uMode);
UINT WINAPI GetErrorMode(void);
DECLSPEC_NORETURN void WINAPI FatalAppExitA(UINT uAction, LPCSTR lpMessageText);
GTA6RT_END_C

#define SEM_FAILCRITICALERRORS 0x0001
#define SEM_NOGPFAULTERRORBOX 0x0002
#define SEM_NOOPENFILEERRORBOX 0x8000
