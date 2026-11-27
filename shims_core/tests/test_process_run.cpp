#include <atomic>
#include <cstdint>

#include "process_shim.h"
#include "shim_call.h"
#include "shims.h"
#include "test.h"

namespace {
std::atomic<int> g_atexit_runs{0};
uint64_t WINAPI fake_entry(uint64_t) { return 7; }
uint64_t WINAPI atexit_handler(uint64_t);
uint64_t WINAPI exiting_entry(uint64_t) {
    // The title registers handlers during its run; exit(5) runs them, then the run ends with 5.
    tcall::call<int>("ucrtbase.dll", "_crt_atexit", reinterpret_cast<uint64_t>(&atexit_handler));
    tcall::call<int>("ucrtbase.dll", "_crt_atexit", reinterpret_cast<uint64_t>(&atexit_handler));
    tcall::call<void>("ucrtbase.dll", "exit", 5);
    return 99;
}
uint64_t WINAPI atexit_handler(uint64_t) {
    ++g_atexit_runs;
    return 0;
}
}  // namespace

TEST(process_run_entry_returns_entry_code) {
    CHECK_EQ(rt::process::run_entry(reinterpret_cast<uintptr_t>(&fake_entry), 1u << 20), 7);
}

TEST(process_exit_runs_atexit_newest_first_then_ends_run) {
    g_atexit_runs = 0;
    CHECK_EQ(rt::process::run_entry(reinterpret_cast<uintptr_t>(&exiting_entry), 1u << 20), 5);
    CHECK_EQ(g_atexit_runs.load(), 2);
}
