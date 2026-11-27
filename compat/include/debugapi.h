#pragma once
#include <minwinbase.h>

/* Implemented by os/. */
GTA6RT_BEGIN_C
BOOL WINAPI IsDebuggerPresent(void);
void WINAPI DebugBreak(void);
void WINAPI OutputDebugStringA(LPCSTR lpOutputString);
void WINAPI OutputDebugStringW(LPCWSTR lpOutputString);
GTA6RT_END_C
#if defined(UNICODE)
#define OutputDebugString OutputDebugStringW
#else
#define OutputDebugString OutputDebugStringA
#endif
