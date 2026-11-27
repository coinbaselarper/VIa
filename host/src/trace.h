#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <gta6rt/abi.h>

namespace host::trace {

struct Options {
    bool enabled = false;
    uint64_t log_first = 0;
    const char* match = nullptr;
    uint64_t log_match_max = 2000;
};

void configure(const Options& opt, gta6rt_gva exe_base, uint64_t exe_size);
bool enabled();
void set_exe_range(gta6rt_gva base, uint64_t size);

// Returns a guest-callable address that counts and logs the call and then jumps to target. Without tracing (or on a
// backend that cannot tail-jump natively) returns target unchanged. Same target and name give the same wrapper.
gta6rt_gva wrap(gta6rt_gva target, const char* name, bool unimplemented);
void note_unimplemented_binding(const char* name);
void note_missing_proc(const char* module, const char* name);

uint64_t total_calls();
uint64_t exe_calls();

// Summary: totals, distinct imports, unimplemented imports in order of first call, top counts, last N calls.
void dump(FILE* out, const char* why, size_t last_n = 30);
// Same, through write(2) only where possible (fatal signal path).
void dump_fatal(const char* why);

}  // namespace host::trace
