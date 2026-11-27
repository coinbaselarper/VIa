#pragma once
#include <minwinbase.h>

/* Implemented by os/ (identity encoding). */
GTA6RT_BEGIN_C
PVOID WINAPI EncodePointer(PVOID Ptr);
PVOID WINAPI DecodePointer(PVOID Ptr);
GTA6RT_END_C
