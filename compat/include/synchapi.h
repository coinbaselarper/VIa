#pragma once
#include <minwinbase.h>

#define CREATE_EVENT_MANUAL_RESET 0x00000001
#define CREATE_EVENT_INITIAL_SET 0x00000002
#define CREATE_MUTEX_INITIAL_OWNER 0x00000001
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define CRITICAL_SECTION_NO_DEBUG_INFO 0x01000000

/* Implemented by os/. Object handles come from the os handle table; SRW/CS/CV/WaitOnAddress are in-memory. */
GTA6RT_BEGIN_C
void WINAPI InitializeSRWLock(PSRWLOCK SRWLock);
void WINAPI AcquireSRWLockExclusive(PSRWLOCK SRWLock);
void WINAPI AcquireSRWLockShared(PSRWLOCK SRWLock);
void WINAPI ReleaseSRWLockExclusive(PSRWLOCK SRWLock);
void WINAPI ReleaseSRWLockShared(PSRWLOCK SRWLock);
BOOLEAN WINAPI TryAcquireSRWLockExclusive(PSRWLOCK SRWLock);
BOOLEAN WINAPI TryAcquireSRWLockShared(PSRWLOCK SRWLock);

void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount);
BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount, DWORD Flags);
void WINAPI EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection);

void WINAPI InitializeConditionVariable(PCONDITION_VARIABLE ConditionVariable);
BOOL WINAPI SleepConditionVariableCS(PCONDITION_VARIABLE ConditionVariable, PCRITICAL_SECTION CriticalSection,
                                     DWORD dwMilliseconds);
BOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE ConditionVariable, PSRWLOCK SRWLock, DWORD dwMilliseconds,
                                      ULONG Flags);
void WINAPI WakeConditionVariable(PCONDITION_VARIABLE ConditionVariable);
void WINAPI WakeAllConditionVariable(PCONDITION_VARIABLE ConditionVariable);

void WINAPI InitOnceInitialize(PINIT_ONCE InitOnce);
BOOL WINAPI InitOnceExecuteOnce(PINIT_ONCE InitOnce, PINIT_ONCE_FN InitFn, PVOID Parameter, LPVOID* Context);
BOOL WINAPI InitOnceBeginInitialize(LPINIT_ONCE lpInitOnce, DWORD dwFlags, PBOOL fPending, LPVOID* lpContext);
BOOL WINAPI InitOnceComplete(LPINIT_ONCE lpInitOnce, DWORD dwFlags, LPVOID lpContext);

BOOL WINAPI WaitOnAddress(volatile void* Address, PVOID CompareAddress, SIZE_T AddressSize, DWORD dwMilliseconds);
void WINAPI WakeByAddressSingle(PVOID Address);
void WINAPI WakeByAddressAll(PVOID Address);

HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName);
HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCWSTR lpName);
HANDLE WINAPI CreateEventExW(LPSECURITY_ATTRIBUTES lpEventAttributes, LPCWSTR lpName, DWORD dwFlags, DWORD dwDesiredAccess);
HANDLE WINAPI OpenEventW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpName);
BOOL WINAPI SetEvent(HANDLE hEvent);
BOOL WINAPI ResetEvent(HANDLE hEvent);
HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName);
HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCWSTR lpName);
HANDLE WINAPI CreateMutexExW(LPSECURITY_ATTRIBUTES lpMutexAttributes, LPCWSTR lpName, DWORD dwFlags, DWORD dwDesiredAccess);
BOOL WINAPI ReleaseMutex(HANDLE hMutex);
HANDLE WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount,
                               LPCWSTR lpName);
HANDLE WINAPI CreateSemaphoreExW(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount,
                                 LPCWSTR lpName, DWORD dwFlags, DWORD dwDesiredAccess);
BOOL WINAPI ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, LPLONG lpPreviousCount);
HANDLE WINAPI CreateWaitableTimerW(LPSECURITY_ATTRIBUTES lpTimerAttributes, BOOL bManualReset, LPCWSTR lpTimerName);
HANDLE WINAPI CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES lpTimerAttributes, LPCWSTR lpTimerName, DWORD dwFlags,
                                     DWORD dwDesiredAccess);
BOOL WINAPI SetWaitableTimer(HANDLE hTimer, const LARGE_INTEGER* lpDueTime, LONG lPeriod,
                             PTIMERAPCROUTINE pfnCompletionRoutine, LPVOID lpArgToCompletionRoutine, BOOL fResume);
BOOL WINAPI CancelWaitableTimer(HANDLE hTimer);

DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD WINAPI WaitForSingleObjectEx(HANDLE hHandle, DWORD dwMilliseconds, BOOL bAlertable);
DWORD WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);
DWORD WINAPI WaitForMultipleObjectsEx(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds,
                                      BOOL bAlertable);
DWORD WINAPI SignalObjectAndWait(HANDLE hObjectToSignal, HANDLE hObjectToWaitOn, DWORD dwMilliseconds, BOOL bAlertable);
void WINAPI Sleep(DWORD dwMilliseconds);
DWORD WINAPI SleepEx(DWORD dwMilliseconds, BOOL bAlertable);
GTA6RT_END_C

#if defined(UNICODE)
#define CreateEvent CreateEventW
#define CreateMutex CreateMutexW
#define CreateSemaphore CreateSemaphoreW
#define CreateWaitableTimer CreateWaitableTimerW
#define OpenEvent OpenEventW
#else
#define CreateEvent CreateEventA
#define CreateMutex CreateMutexA
#endif
