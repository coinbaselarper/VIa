#pragma once
#include <minwinbase.h>

#define DATE_SHORTDATE 0x00000001
#define DATE_LONGDATE 0x00000002
#define TIME_NOMINUTESORSECONDS 0x00000001
#define TIME_NOSECONDS 0x00000002
#define TIME_NOTIMEMARKER 0x00000004
#define TIME_FORCE24HOURFORMAT 0x00000008

/* Implemented by os/ (en-US pictures only). */
GTA6RT_BEGIN_C
int WINAPI GetDateFormatEx(LPCWSTR lpLocaleName, DWORD dwFlags, const SYSTEMTIME* lpDate, LPCWSTR lpFormat,
                           LPWSTR lpDateStr, int cchDate, LPCWSTR lpCalendar);
int WINAPI GetTimeFormatEx(LPCWSTR lpLocaleName, DWORD dwFlags, const SYSTEMTIME* lpTime, LPCWSTR lpFormat,
                           LPWSTR lpTimeStr, int cchTime);
GTA6RT_END_C
