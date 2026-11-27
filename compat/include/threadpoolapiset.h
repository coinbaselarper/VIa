#pragma once
#include <minwinbase.h>

typedef struct _TP_CALLBACK_INSTANCE TP_CALLBACK_INSTANCE, *PTP_CALLBACK_INSTANCE;
typedef struct _TP_WORK TP_WORK, *PTP_WORK;
typedef struct _TP_WAIT TP_WAIT, *PTP_WAIT;
typedef struct _TP_TIMER TP_TIMER, *PTP_TIMER;
typedef struct _TP_POOL TP_POOL, *PTP_POOL;
typedef struct _TP_CALLBACK_ENVIRON_V3 TP_CALLBACK_ENVIRON, *PTP_CALLBACK_ENVIRON;
typedef DWORD TP_WAIT_RESULT;
typedef void(CALLBACK* PTP_WORK_CALLBACK)(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WORK Work);
typedef void(CALLBACK* PTP_WAIT_CALLBACK)(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WAIT Wait,
                                          TP_WAIT_RESULT WaitResult);
typedef void(CALLBACK* PTP_TIMER_CALLBACK)(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_TIMER Timer);
typedef void(CALLBACK* PTP_SIMPLE_CALLBACK)(PTP_CALLBACK_INSTANCE Instance, PVOID Context);

/* Implemented by os/ (thread pool over gta6rt threads). */
GTA6RT_BEGIN_C
PTP_WORK WINAPI CreateThreadpoolWork(PTP_WORK_CALLBACK pfnwk, PVOID pv, PTP_CALLBACK_ENVIRON pcbe);
void WINAPI SubmitThreadpoolWork(PTP_WORK pwk);
void WINAPI WaitForThreadpoolWorkCallbacks(PTP_WORK pwk, BOOL fCancelPendingCallbacks);
void WINAPI CloseThreadpoolWork(PTP_WORK pwk);
PTP_WAIT WINAPI CreateThreadpoolWait(PTP_WAIT_CALLBACK pfnwa, PVOID pv, PTP_CALLBACK_ENVIRON pcbe);
void WINAPI SetThreadpoolWait(PTP_WAIT pwa, HANDLE h, PFILETIME pftTimeout);
void WINAPI WaitForThreadpoolWaitCallbacks(PTP_WAIT pwa, BOOL fCancelPendingCallbacks);
void WINAPI CloseThreadpoolWait(PTP_WAIT pwa);
PTP_TIMER WINAPI CreateThreadpoolTimer(PTP_TIMER_CALLBACK pfnti, PVOID pv, PTP_CALLBACK_ENVIRON pcbe);
void WINAPI SetThreadpoolTimer(PTP_TIMER pti, PFILETIME pftDueTime, DWORD msPeriod, DWORD msWindowLength);
void WINAPI WaitForThreadpoolTimerCallbacks(PTP_TIMER pti, BOOL fCancelPendingCallbacks);
void WINAPI CloseThreadpoolTimer(PTP_TIMER pti);
BOOL WINAPI TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK pfns, PVOID pv, PTP_CALLBACK_ENVIRON pcbe);
PTP_POOL WINAPI CreateThreadpool(PVOID reserved);
void WINAPI CloseThreadpool(PTP_POOL ptpp);
void WINAPI DisassociateCurrentThreadFromCallback(PTP_CALLBACK_INSTANCE pci);
void WINAPI CallbackMayRunLong(PTP_CALLBACK_INSTANCE pci);
BOOL WINAPI SetThreadpoolThreadMinimum(PTP_POOL ptpp, DWORD cthrdMic);
void WINAPI SetThreadpoolThreadMaximum(PTP_POOL ptpp, DWORD cthrdMost);
/* Added by os/ (S1.1). */
BOOL WINAPI IsThreadpoolTimerSet(PTP_TIMER pti);
void WINAPI FreeLibraryWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci, HMODULE mod);
void WINAPI SetEventWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci, HANDLE evt);
void WINAPI ReleaseSemaphoreWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci, HANDLE sem, DWORD crel);
void WINAPI ReleaseMutexWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci, HANDLE mut);
void WINAPI LeaveCriticalSectionWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci, PCRITICAL_SECTION pcs);
GTA6RT_END_C
