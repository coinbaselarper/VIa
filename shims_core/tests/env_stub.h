#pragma once

#include <cstdint>

#include <gta6rt/isa.h>

namespace stub {

struct ThunkRecord {
    gta6rt_thunk_kind kind;
    const void* fn;
    void* user;
    const char* name;
};

const ThunkRecord* thunk(gta6rt_gva addr);
size_t thunk_count();

void set_stub_vm_failures(bool on);
// Non-zero: gta6rt_vm_lowest_mappable returns it and reserve_range refuses any range ending at or below it (a process
// with no address space below that, like arm64 macOS below 4 GiB).
void set_lowest_mappable(uint64_t address);

}  // namespace stub
