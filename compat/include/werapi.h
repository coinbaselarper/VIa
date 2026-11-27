#pragma once
#include <minwinbase.h>

typedef enum _WER_REGISTER_FILE_TYPE {
    WerRegFileTypeUserDocument = 1,
    WerRegFileTypeOther = 2,
    WerRegFileTypeMax
} WER_REGISTER_FILE_TYPE;

#define WER_FILE_DELETE_WHEN_DONE 1
#define WER_FILE_ANONYMOUS_DATA 2

/* Implemented by os/ (accepted and logged; there is no error reporting service). */
GTA6RT_BEGIN_C
HRESULT WINAPI WerRegisterFile(PCWSTR pwzFile, WER_REGISTER_FILE_TYPE regFileType, DWORD dwFlags);
HRESULT WINAPI WerUnregisterFile(PCWSTR pwzFilePath);
GTA6RT_END_C
