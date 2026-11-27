// The game's command line and exit, for `gta6rt --run` (process.cpp).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace rt::process {

// Set GetCommandLine to `program` then `args`, quoted where needed, before process-attach callbacks. The UTF-16 text
// holds the arguments exactly; the narrow text is UTF-8. Resets the CRT argv to argc=0/null: the startup configuration
// selects no arguments, the original arguments, or wildcard expansion for each width.
void set_command_line(const std::u16string& program, const std::vector<std::u16string>& args);

// Runs a mapped EXE's entry point on a new guest thread with `stack_bytes` of reserved stack and returns the game's
// exit code: the entry point's return value, or what it passed to exit/ExitProcess (3 for abort). Returns when the
// first of those happens; other game threads are left running (the host process ends them when it exits).
int run_entry(uint64_t entry, std::size_t stack_bytes = 8u << 20);

// Ends the run the way abort() does, with `code`. Never returns.
[[noreturn]] void abort_run(int code);

// The run's atexit table, for the CRT startup functions that register into it.
bool register_atexit_address(uint64_t guest_fn);

}  // namespace rt::process
