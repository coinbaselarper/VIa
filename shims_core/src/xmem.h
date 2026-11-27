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

// GTA6RT_METAL_TITLE_POOL=1 (the title pool): the pool is one Mach memory entry mapped once at a 16 KiB-aligned
// host range that is never moved, protected or unmapped, title pages are views of it (physical pages in windows,
// and committed XMemVirtualAlloc / MEM_GRAPHICS VirtualAlloc memory as runs of slots), so a GPU buffer made over the
// host range reaches title memory with no copy. False / empty everywhere while it is off (the default).
//
// The pool bytes behind title address `va`: `host` is va's byte in the host range, `run_bytes` how many bytes from
// va on are the same consecutive pool bytes (at most 256 MiB). False when no pool page backs va now (not title
// pool memory, unmapped, decommitted).
bool title_pool_view(std::uint64_t va, void** host, std::uint64_t* run_bytes);
// Bumped by every change of what title addresses the pool backs.
std::uint64_t title_pool_generation();
// The title ranges whose backing went or changed (unmapped, remapped, decommitted, released) after generation
// `since`, into `out` ({0, UINT64_MAX} when the log no longer reaches back that far); returns the generation now.
std::uint64_t title_pool_removals(std::uint64_t since, std::vector<std::pair<std::uint64_t, std::uint64_t>>& out);
// The title ranges that became pool views (mapped, committed) after generation `since`, as title_pool_removals.
std::uint64_t title_pool_additions(std::uint64_t since, std::vector<std::pair<std::uint64_t, std::uint64_t>>& out);
// The pool's host range, false when the title pool is off.
bool title_pool_range(void** base, std::uint64_t* size);
// Diagnostics: per 16 KiB of the pool's host range, 1 where a live physical page or a committed page of a virtual run
// is backed, else 0 (a slot of a reservation the title never committed, a free slot). Empty when the pool is off.
void title_pool_live_map(std::vector<std::uint8_t>& out);
struct TitlePoolStats {
    bool on = false;
    std::uint64_t runs = 0, run_bytes = 0, runs_refused = 0, plain_commits = 0, plain_commit_bytes = 0;
    std::uint64_t generation = 0, removals = 0;
    // multi-page XMemAllocatePhysicalPages calls given consecutive slots / not
    std::uint64_t contiguous_allocations = 0, scattered_allocations = 0;
};
TitlePoolStats title_pool_stats();
// Whether the title pool is on for this process: GTA6RT_METAL_TITLE_POOL=1 / =0, else on when GTA6RT_GPU=metal.
bool title_pool_wanted();
// The range below 4 GiB held from process start for the title's 32-bit reservations (LEONIDA_XMEM_LOW_HOLD_MB):
// what is left of it, and what was asked.
struct LowHoldInfo {
    void* base = nullptr;
    std::uint64_t size = 0, asked = 0;
};
LowHoldInfo low_hold_info();

}  // namespace rt::xmem
