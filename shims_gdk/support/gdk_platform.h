#pragma once

#include <gta6rt/os.h>

#include <chrono>
#include <cstdint>

namespace rt::gdk {

// A display-sleep assertion (IOPMAssertion on macOS). The handle is opaque; release ignores handles it did not issue.
void* display_deferral_acquire();
void display_deferral_release(void* handle);

// The guest's QueryPerformanceCounter time base (10 MHz) from the os module; a build without it falls back to the
// monotonic clock at the same frequency.
inline int64_t qpc_frequency() {
    const uint64_t f = gta6rt_time_qpc_frequency();
    return f ? static_cast<int64_t>(f) : 10'000'000;
}

inline int64_t qpc_now() {
    if (gta6rt_time_qpc_frequency()) return static_cast<int64_t>(gta6rt_time_qpc());
    const auto ns = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(ns).count() / 100;
}

}  // namespace rt::gdk
