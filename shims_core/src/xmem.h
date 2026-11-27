// xmem.dll on macOS: the GDK's tracked allocator and its physical-page windows, built on os.h's virtual memory.
//
// Physical pages are 64 KiB slots of one pool (reserved once, committed per allocation, never moved), numbered
// slot * 16 as the title expects. A window is a reserve-only XMemVirtualAlloc region in a guest page mode; mapping a
// page there is gta6rt_vm_alias of the slot onto the window page, so one slot can be mapped at several addresses and
// keeps its contents while unmapped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace rt::xmem {

constexpr uint64_t kPhysicalPageSize = 0x10000;

// The host address of a physical page's backing in the pool (the CPU view the D3D12 layer reads), or null for a page
// number that is not live.
void* physical_page_host_address(uint64_t page);

// CopyPageMappingsBatchX (Xbox queue slot 31): maps at `destination` the physical pages that are mapped at `source`
// now, `pages` 64 KiB pages one after another, or the one page at `source` every time with `repeat_first`. A source page
// with nothing mapped leaves its destination page unmapped; a null `source` unmaps them all. False, with the last error
// set, for an unaligned address or a destination outside a window.
bool copy_page_mappings(void* destination, const void* source, uint64_t pages, bool repeat_first);
using CopyPageMappingsFn = bool (*)(void* destination, const void* source, std::uint64_t pages, bool repeat_first);
extern CopyPageMappingsFn copy_page_mappings_fn;

// Copy a validated DirectStorage staging buffer through the page table: one frame number per 64 KiB page, all pages
// resolved before a write starts.
bool transfer_physical(std::span<const std::uint64_t> pages, std::uint16_t offset, std::span<std::uint8_t> staging, bool write);

// The title's 32-bit reservations (below 4 GiB) and the run of pool pages each holds: fresh pages mapped there get the
// slot of their address, so with `contiguous` the reservation [base, base+size) is the same bytes as
// [host_stretch, host_stretch+size) of the pool, for one no-copy GPU buffer over it.
struct LowReservation {
    void* base;
    uint64_t size;
    void* host_stretch;
    bool contiguous;
};
std::vector<LowReservation> low_reservations();
// 32-bit reservations served above 4 GiB: LEONIDA_XMEM_32BIT=anywhere, or auto in a process with no address space
// below 4 GiB (arm64; x86_64 linked without -pagezero_size 0x10000).
uint64_t low32_placed_above();

// The physical pool for the host's memory budget: its host range (null before the first allocation), live pages,
// slots ever handed out and slots on the free list (decommitted when freed).
struct PoolInfo {
    void* base;
    uint64_t size;
    uint64_t live_pages;
    uint64_t slots_used;
    uint64_t free_slots;
};
PoolInfo pool_info();
// Host ranges of the windows physical pages are mapped into (aliases of pool slots).
std::vector<std::pair<uintptr_t, uint64_t>> window_ranges();

// Live physical pages, windows and mapped pages (tests and diagnostics).
size_t live_physical_pages();
size_t window_count();
size_t mapped_page_count();

}  // namespace rt::xmem
