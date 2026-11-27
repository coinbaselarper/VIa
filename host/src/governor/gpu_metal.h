#pragma once
#include <cstdint>

namespace host::governor::gpu {

struct Info {
    bool have_device = false, unified = false, have_system = false;
    uint64_t allocated = 0;          // MTLDevice currentAllocatedSize: this process's Metal allocations
    uint64_t working_set_limit = 0;  // recommendedMaxWorkingSetSize
    uint64_t max_buffer = 0;
    uint64_t system_alloc = 0, system_in_use = 0, system_driver = 0;  // IOAccelerator PerformanceStatistics, all processes
    uint64_t recoveries = 0;                                         // GPU hang recoveries since boot
};

bool init();
Info sample(bool with_system);

}  // namespace host::governor::gpu
