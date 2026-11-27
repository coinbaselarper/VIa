#pragma once
#include <minwinbase.h>

#define NMPWAIT_WAIT_FOREVER 0xffffffff
#define NMPWAIT_NOWAIT 0x00000001
#define NMPWAIT_USE_DEFAULT_WAIT 0x00000000

/* Implemented by os/ (no named pipes on the host: fail with ERROR_FILE_NOT_FOUND / ERROR_INVALID_HANDLE). */
GTA6RT_BEGIN_C
BOOL WINAPI SetNamedPipeHandleState(HANDLE hNamedPipe, LPDWORD lpMode, LPDWORD lpMaxCollectionCount,
                                    LPDWORD lpCollectDataTimeout);
BOOL WINAPI WaitNamedPipeW(LPCWSTR lpNamedPipeName, DWORD nTimeOut);
GTA6RT_END_C
