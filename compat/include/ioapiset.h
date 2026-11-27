#pragma once
#include <minwinbase.h>

/* Implemented by os/. */
GTA6RT_BEGIN_C
BOOL WINAPI GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, BOOL bWait);
BOOL WINAPI GetOverlappedResultEx(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred,
                                  DWORD dwMilliseconds, BOOL bAlertable);
BOOL WINAPI CancelIo(HANDLE hFile);
BOOL WINAPI CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped);
HANDLE WINAPI CreateIoCompletionPort(HANDLE FileHandle, HANDLE ExistingCompletionPort, ULONG_PTR CompletionKey,
                                     DWORD NumberOfConcurrentThreads);
BOOL WINAPI GetQueuedCompletionStatus(HANDLE CompletionPort, LPDWORD lpNumberOfBytesTransferred, PULONG_PTR lpCompletionKey,
                                      LPOVERLAPPED* lpOverlapped, DWORD dwMilliseconds);
BOOL WINAPI PostQueuedCompletionStatus(HANDLE CompletionPort, DWORD dwNumberOfBytesTransferred, ULONG_PTR dwCompletionKey,
                                       LPOVERLAPPED lpOverlapped);
BOOL WINAPI DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize,
                            LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped);
/* Added by os/ (S1.1). */
BOOL WINAPI GetQueuedCompletionStatusEx(HANDLE CompletionPort, LPOVERLAPPED_ENTRY lpCompletionPortEntries, ULONG ulCount,
                                        PULONG ulNumEntriesRemoved, DWORD dwMilliseconds, BOOL fAlertable);
GTA6RT_END_C
#define HasOverlappedIoCompleted(lpOverlapped) (((DWORD)(lpOverlapped)->Internal) != STATUS_PENDING)
