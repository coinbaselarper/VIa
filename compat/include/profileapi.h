#pragma once
#include <minwinbase.h>

/* Implemented by os/ on gta6rt_time_* (10 MHz QPC frequency, as on current Windows). */
GTA6RT_BEGIN_C
BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount);
BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency);
GTA6RT_END_C
