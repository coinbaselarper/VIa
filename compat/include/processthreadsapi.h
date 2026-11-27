#pragma once
#include <minwinbase.h>

#define CREATE_SUSPENDED 0x00000004
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#define DETACHED_PROCESS 0x00000008
#define CREATE_NEW_CONSOLE 0x00000010
#define CREATE_NO_WINDOW 0x08000000
#define CREATE_UNICODE_ENVIRONMENT 0x00000400
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_ERROR_RETURN (MAXLONG)
#define TLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)
#define FLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)
#define STARTF_USESHOWWINDOW 0x00000001
#define STARTF_USESTDHANDLES 0x00000100

typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION, *PPROCESS_INFORMATION, *LPPROCESS_INFORMATION;

typedef struct _STARTUPINFOW {
    DWORD cb;
    LPWSTR lpReserved;
    LPWSTR lpDesktop;
    LPWSTR lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOW, *LPSTARTUPINFOW;

typedef struct _STARTUPINFOA {
    DWORD cb;
    LPSTR lpReserved;
    LPSTR lpDesktop;
    LPSTR lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;

typedef enum _THREAD_INFORMATION_CLASS {
    ThreadMemoryPriority,
    ThreadAbsoluteCpuPriority,
    ThreadDynamicCodePolicy,
    ThreadPowerThrottling,
    ThreadInformationClassMax
} THREAD_INFORMATION_CLASS;

/* Implemented by os/. Threads created here get a TEB through the os thread-start hook (INTERFACES.md). */
GTA6RT_BEGIN_C
HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                           LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags,
                           LPDWORD lpThreadId);
DECLSPEC_NORETURN void WINAPI ExitThread(DWORD dwExitCode);
BOOL WINAPI TerminateThread(HANDLE hThread, DWORD dwExitCode);
BOOL WINAPI GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);
DWORD WINAPI ResumeThread(HANDLE hThread);
DWORD WINAPI SuspendThread(HANDLE hThread);
HANDLE WINAPI OpenThread(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwThreadId);
HANDLE WINAPI GetCurrentThread(void);
DWORD WINAPI GetCurrentThreadId(void);
DWORD WINAPI GetThreadId(HANDLE Thread);
HANDLE WINAPI GetCurrentProcess(void);
DWORD WINAPI GetCurrentProcessId(void);
DWORD WINAPI GetProcessId(HANDLE Process);
DECLSPEC_NORETURN void WINAPI ExitProcess(UINT uExitCode);
BOOL WINAPI TerminateProcess(HANDLE hProcess, UINT uExitCode);
BOOL WINAPI GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode);
BOOL WINAPI CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
                           LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags,
                           LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
                           LPPROCESS_INFORMATION lpProcessInformation);
BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority);
int WINAPI GetThreadPriority(HANDLE hThread);
DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask);
BOOL WINAPI SetThreadGroupAffinity(HANDLE hThread, const GROUP_AFFINITY* GroupAffinity, PGROUP_AFFINITY PreviousGroupAffinity);
BOOL WINAPI GetThreadGroupAffinity(HANDLE hThread, PGROUP_AFFINITY GroupAffinity);
DWORD WINAPI SetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor);
HRESULT WINAPI SetThreadDescription(HANDLE hThread, PCWSTR lpThreadDescription);
HRESULT WINAPI GetThreadDescription(HANDLE hThread, PWSTR* ppszThreadDescription);
BOOL WINAPI GetThreadContext(HANDLE hThread, LPCONTEXT lpContext);
BOOL WINAPI SetThreadContext(HANDLE hThread, const CONTEXT* lpContext);
BOOL WINAPI SwitchToThread(void);
DWORD WINAPI QueueUserAPC(PAPCFUNC pfnAPC, HANDLE hThread, ULONG_PTR dwData);
BOOL WINAPI GetThreadTimes(HANDLE hThread, LPFILETIME lpCreationTime, LPFILETIME lpExitTime, LPFILETIME lpKernelTime,
                           LPFILETIME lpUserTime);
BOOL WINAPI GetProcessTimes(HANDLE hProcess, LPFILETIME lpCreationTime, LPFILETIME lpExitTime, LPFILETIME lpKernelTime,
                            LPFILETIME lpUserTime);
DWORD WINAPI GetCurrentProcessorNumber(void);
void WINAPI GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER ProcNumber);
DWORD WINAPI TlsAlloc(void);
LPVOID WINAPI TlsGetValue(DWORD dwTlsIndex);
BOOL WINAPI TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue);
BOOL WINAPI TlsFree(DWORD dwTlsIndex);
DWORD WINAPI FlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback);
PVOID WINAPI FlsGetValue(DWORD dwFlsIndex);
BOOL WINAPI FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData);
BOOL WINAPI FlsFree(DWORD dwFlsIndex);
void WINAPI GetStartupInfoW(LPSTARTUPINFOW lpStartupInfo);
BOOL WINAPI IsProcessorFeaturePresent(DWORD ProcessorFeature);
BOOL WINAPI OpenProcessToken(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle);
BOOL WINAPI OpenThreadToken(HANDLE ThreadHandle, DWORD DesiredAccess, BOOL OpenAsSelf, PHANDLE TokenHandle);
BOOL WINAPI FlushProcessWriteBuffers(void);
/* Implemented by os/ (the calling thread's stack bounds, gta6rt_thread_stack). */
void WINAPI GetCurrentThreadStackLimits(PULONG_PTR LowLimit, PULONG_PTR HighLimit);
/* Added by os/ (S1.1). */
BOOL WINAPI SetThreadIdealProcessorEx(HANDLE hThread, PPROCESSOR_NUMBER lpIdealProcessor,
                                      PPROCESSOR_NUMBER lpPreviousIdealProcessor);
BOOL WINAPI GetThreadIdealProcessorEx(HANDLE hThread, PPROCESSOR_NUMBER lpIdealProcessor);
BOOL WINAPI SetProcessPriorityBoost(HANDLE hProcess, BOOL bDisablePriorityBoost);
BOOL WINAPI GetProcessPriorityBoost(HANDLE hProcess, PBOOL pDisablePriorityBoost);
GTA6RT_END_C

#define PF_XMMI64_INSTRUCTIONS_AVAILABLE 10
#define PF_SSE3_INSTRUCTIONS_AVAILABLE 13
#define PF_AVX_INSTRUCTIONS_AVAILABLE 39
#define PF_AVX2_INSTRUCTIONS_AVAILABLE 40
#define PF_AVX512F_INSTRUCTIONS_AVAILABLE 41

#if defined(UNICODE)
#define CreateProcess CreateProcessW
#define GetStartupInfo GetStartupInfoW
typedef STARTUPINFOW STARTUPINFO;
typedef LPSTARTUPINFOW LPSTARTUPINFO;
#endif
