#pragma once

#include <cstdint>

namespace rt::crt {

// The callback the exe registered with _register_thread_local_exe_atexit_callback (0: none).
uint64_t thread_exit_callback();
// Runs it with (exe base, DLL_THREAD_DETACH, 0) through the ISA.
void run_thread_exit_callback();

}  // namespace rt::crt
