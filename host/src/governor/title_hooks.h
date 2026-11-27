#pragma once
#include <atomic>
#include <cstdint>

namespace host::governor::hooks {

// What the title gives back, counted at its VirtualAlloc/VirtualFree/XMemVirtualAlloc/XMemFreePhysicalPages calls.
struct Counters {
    std::atomic<uint64_t> reset_calls{0}, reset_bytes{0}, reset_undo_calls{0};
    std::atomic<uint64_t> advised_calls{0}, advised_bytes{0};
    std::atomic<uint64_t> decommit_calls{0}, decommit_bytes{0}, release_calls{0};
    std::atomic<uint64_t> freed_page_calls{0}, freed_pages{0};
    std::atomic<uint64_t> alloc_calls{0}, free_calls{0};
};

unsigned install();
bool installed();
const Counters& counters();
// MEM_RESET from now on also tells the kernel the pages may be discarded (MADV_FREE through gta6rt_vm_reset).
void set_reset_advice(bool on);
bool reset_advice();

}  // namespace host::governor::hooks
