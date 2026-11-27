#pragma once

#include <cstdint>

#include <gta6rt/os.h>

namespace rt::k32 {

// The gta6rt_thread behind a Win32 thread HANDLE (-2 is the current thread), or null.
gta6rt_thread* thread_of_handle(void* handle);
// Affinity mask naming every logical CPU the host has (at most 64).
uint64_t affinity_mask();
// FILETIME ticks (100 ns since 1601) for a Unix time.
uint64_t filetime_from_unix(int64_t seconds, int64_t nanos);

}  // namespace rt::k32
