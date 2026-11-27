// xmem.dll: the GDK's tracked allocator, physical pages and the title's virtual memory, on os.h's vm functions.
//
// XMemAlloc/XMemFree (and the Default pair, which share the heap) hand out 64-byte-aligned blocks that remember their
// size, so XMemFree needs none. XMemVirtualAlloc takes the console's allocation types: committed 2 MiB and 64 KiB
// pages, reserve-only 64 KiB windows for physical pages, one MEM_RESERVE_PLACEHOLDER reservation and the
// MEM_REPLACE_PLACEHOLDER windows carved out of it, each then filled with XMemMapPhysicalPages. The attribute word is
// accepted and not decoded (every measured call passes a nonzero one). kernel32's VirtualAlloc/VirtualFree/
// VirtualProtect/VirtualQuery(Ex) are served here too, as the title calls them: MEM_GRAPHICS/MEM_TITLE and the
// PAGE_GRAPHICS_* bits are console-only hints and are dropped.
//
// Not ported from the Windows implementation (it ran its own vectored fault handler over placeholder reservations and
// section views): lazily committed physical pages (pages are committed whole at allocation, as under Wine), zero pages
// planted by the fault handler into window pages the title never mapped (such an access faults), write-watch shadows
// and XMemResetWriteWatch, the title-memory/label/geometry notifications for the D3D12 layer, and the stream-pool
// mirror of CopyPageMappingsBatchX. These are listed in STATUS.md.
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gta6rt/os.h>

#include "console_registry.h"
#include "shims.h"
#include "util.h"
#include "xmem.h"

namespace rt::xmem {
CopyPageMappingsFn copy_page_mappings_fn = nullptr;
}

namespace {

constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemReplacePlaceholder = 0x4000, kMemDecommit = 0x4000,
                   kMemRelease = 0x8000, kMemReservePlaceholder = 0x40000, kMemReset = 0x80000, kMemTopDown = 0x100000,
                   kMemWriteWatch = 0x200000, kMemLargePages = 0x20000000, kMemResetUndo = 0x1000000,
                   kMemPhysical = 0x400000, kMemGraphics = 0x10000000, kMemTitle = 0x40000000;
constexpr uint32_t kXmemLargePages = 0x20000000, kXmem64kPages = 0x20400000, kXmemPageModeMask = kXmem64kPages;
constexpr uint32_t kHostTypeMask = kMemCommit | kMemReserve | kMemReplacePlaceholder | kMemReservePlaceholder | kMemReset |
                                   kMemTopDown | kMemWriteWatch | kMemResetUndo;
constexpr uint64_t kPage64k = 0x10000, kPage2m = 0x200000;
constexpr uint32_t kPageGraphicsMask = 0x0007F800;

// XMem attribute bits (VIctory xmem.rs `address_range`, GPL-3.0-or-later, its decision 0001): bit 2 asks for an
// address below 4 GiB (the title's 185 MB graphics heap, attributes 7); bits 12-14 give an address width of
// 47 - field bits, and no field means 40 bits.
constexpr uint64_t kXmemAttr32Bit = 4;
constexpr unsigned kXmemAttrWidthShift = 12;

constexpr uint32_t kErrInvalidParameter = 87, kErrInvalidAddress = 487, kErrNotEnoughMemory = 8,
                   kErrNotSupported = 50;

constexpr size_t kAlign = 64;
constexpr size_t kNoSize = static_cast<size_t>(-1);

// ---- protection translation

constexpr uint32_t kPageNoAccess = 0x01, kPageReadOnly = 0x02, kPageReadWrite = 0x04, kPageWriteCopy = 0x08,
                   kPageExecute = 0x10, kPageExecuteRead = 0x20, kPageExecuteReadWrite = 0x40,
                   kPageExecuteWriteCopy = 0x80, kPageGuard = 0x100;

uint32_t to_vm_prot(uint32_t win) {
    win &= ~kPageGraphicsMask;
    uint32_t p = 0;
    switch (win & 0xFF) {
    case kPageNoAccess: case 0: p = GTA6RT_PROT_NONE; break;
    case kPageReadOnly: p = GTA6RT_PROT_READ; break;
    case kPageReadWrite: case kPageWriteCopy: p = GTA6RT_PROT_READ | GTA6RT_PROT_WRITE; break;
    case kPageExecute: p = GTA6RT_PROT_EXEC; break;
    case kPageExecuteRead: p = GTA6RT_PROT_READ | GTA6RT_PROT_EXEC; break;
    case kPageExecuteReadWrite: case kPageExecuteWriteCopy: p = GTA6RT_PROT_READ | GTA6RT_PROT_WRITE | GTA6RT_PROT_EXEC; break;
    default: p = GTA6RT_PROT_READ | GTA6RT_PROT_WRITE; break;
    }
    if (win & kPageGuard) p |= GTA6RT_PROT_GUARD;
    return p;
}

uint32_t to_win_prot(uint32_t vm) {
    const uint32_t rwx = vm & (GTA6RT_PROT_READ | GTA6RT_PROT_WRITE | GTA6RT_PROT_EXEC);
    uint32_t w;
    switch (rwx) {
    case 0: w = kPageNoAccess; break;
    case GTA6RT_PROT_READ: w = kPageReadOnly; break;
    case GTA6RT_PROT_READ | GTA6RT_PROT_WRITE: w = kPageReadWrite; break;
    case GTA6RT_PROT_EXEC: w = kPageExecute; break;
    case GTA6RT_PROT_READ | GTA6RT_PROT_EXEC: w = kPageExecuteRead; break;
    default: w = kPageExecuteReadWrite; break;
    }
    if (vm & GTA6RT_PROT_GUARD) w |= kPageGuard;
    return w;
}

// ---- the title heap

struct BlockHeader {
    void* raw;
    size_t size;
};

void* allocate_block(size_t size) {
    size = size ? size : 1;
    const size_t extra = kAlign + sizeof(BlockHeader);
    if (size > SIZE_MAX - extra) return nullptr;
    void* raw = std::malloc(size + extra);
    if (!raw) return nullptr;
    const uintptr_t user = (reinterpret_cast<uintptr_t>(raw) + sizeof(BlockHeader) + kAlign - 1) & ~(kAlign - 1);
    auto* header = reinterpret_cast<BlockHeader*>(user) - 1;
    header->raw = raw;
    header->size = size;
    return reinterpret_cast<void*>(user);
}

void free_block(void* p) {
    if (p) std::free((reinterpret_cast<BlockHeader*>(p) - 1)->raw);
}

void note_attributes(const char* api, uint64_t attributes, size_t size) {
    static std::mutex mutex;
    static std::set<std::pair<std::string, uint64_t>> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert({api, attributes}).second) return;
    }
    if (size != kNoSize)
        gta6rt_log(GTA6RT_LOG_DEBUG, "xmem", "%s attributes 0x%llx (first seen with size %zu)", api, static_cast<unsigned long long>(attributes), size);
    else
        gta6rt_log(GTA6RT_LOG_DEBUG, "xmem", "%s attributes 0x%llx", api, static_cast<unsigned long long>(attributes));
}

void* WINAPI XMemAlloc(size_t size, uint64_t attributes) {
    note_attributes("XMemAlloc", attributes, size);
    return allocate_block(size);
}
void WINAPI XMemFree(void* p, uint64_t attributes) {
    note_attributes("XMemFree", attributes, kNoSize);
    free_block(p);
}
void* WINAPI XMemAllocDefault(size_t size, uint64_t attributes) {
    note_attributes("XMemAllocDefault", attributes, size);
    return allocate_block(size);
}
void WINAPI XMemFreeDefault(void* p, uint64_t attributes) {
    note_attributes("XMemFreeDefault", attributes, kNoSize);
    free_block(p);
}

int WINAPI XMemIsToolsMemAvailable() {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        RTLOG(INFO, "xmem: XMemIsToolsMemAvailable -> FALSE (no devkit tools memory; logged once)");
    return 0;
}

// ---- virtual memory state

struct Window {
    uint64_t size;
    uint32_t vm_prot;
};

// A window the title gives the GPU but not the CPU (PAGE_NOACCESS plus PAGE_GRAPHICS_* bits, e.g. 0x801: the movie
// planes and other textures placed there) is read and written by the D3D12 layer, which is host code; its pages are
// mapped read/write. The title's own CPU code never touches such a window (it would fault on the console).
uint32_t window_vm_prot(uint32_t protect, const void* base) {
    const uint32_t vm = to_vm_prot(protect);
    if ((vm & (GTA6RT_PROT_READ | GTA6RT_PROT_WRITE)) || !(protect & kPageGraphicsMask)) return vm;
    RTLOG(INFO, "xmem: window %p protect 0x%x: CPU no-access, GPU-visible; mapped read/write for the D3D12 layer", base, protect);
    return GTA6RT_PROT_READ | GTA6RT_PROT_WRITE;
}

struct PhysicalPool {
    void* base = nullptr;
    uint64_t size = 0;
};

constexpr uint64_t kPageSlotLimit = uint64_t{1} << 21;
constexpr uint64_t kFramesPerPage = rt::xmem::kPhysicalPageSize / 0x1000;

std::mutex g_mutex;
std::map<uintptr_t, Window> g_windows;       // base -> window (reserve-only regions physical pages map into)
std::map<uintptr_t, uint64_t> g_placeholders;  // base -> size, the title's MEM_RESERVE_PLACEHOLDER reservations
std::unordered_map<uint64_t, uint64_t> g_pages;  // live page number -> pool slot backing it
std::unordered_set<uint64_t> g_fresh;            // live pages never mapped, written or handed to the host as memory
std::vector<uint64_t> g_free_ids;
uint64_t g_next_id = 1;
std::vector<uint64_t> g_free_slots;
uint64_t g_next_slot = 1;

// A 32-bit reservation holds a run of pool slots in its address order, so the pages the title maps there can be
// given the slot of their address and the reservation becomes one stretch of the pool (one host buffer for the GPU
// side). Only fresh pages move: their contents are zero either way, so which slot backs a number is not observable.
struct LowHold {
    uintptr_t base;
    uint64_t size, first_slot, slots;
    uint64_t moved = 0, not_fresh = 0;
    bool stretch_logged = false;
};
std::vector<LowHold> g_low;
std::unordered_map<uint64_t, uint64_t> g_held_owner;  // held slot -> page number it backs
std::map<uintptr_t, uint64_t> g_mapped;      // window page address -> page number
PhysicalPool g_pool;

uint64_t pool_bytes() {
    uint64_t gib = rt::shims::console_profile().pool_gib;
    if (const char* v = gta6rt_env_get("LEONIDA_XMEM_POOL_GIB")) {
        const unsigned long long n = std::strtoull(v, nullptr, 10);
        if (n >= 1 && n <= 128) gib = n;
    }
    return gib << 30;
}

bool ensure_pool() {
    if (g_pool.base) return true;
    const uint64_t size = std::min<uint64_t>(pool_bytes(), kPageSlotLimit * rt::xmem::kPhysicalPageSize);
    void* base = nullptr;
    if (gta6rt_vm_reserve(nullptr, size, 0, &base) != GTA6RT_OK) {
        RTLOG(ERROR, "xmem: cannot reserve the %llu GiB physical pool", static_cast<unsigned long long>(size >> 30));
        return false;
    }
    g_pool.base = base;
    g_pool.size = size;
    RTLOG(INFO, "xmem: physical pool %llu GiB at %p", static_cast<unsigned long long>(size >> 30), base);
    return true;
}

uint8_t* slot_address(uint64_t slot) { return static_cast<uint8_t*>(g_pool.base) + (slot - 1) * rt::xmem::kPhysicalPageSize; }

uint64_t slot_of(uint64_t page) {
    auto it = g_pages.find(page);
    return it == g_pages.end() ? 0 : it->second;
}

LowHold* hold_of_slot(uint64_t slot) {
    for (LowHold& h : g_low)
        if (slot >= h.first_slot && slot < h.first_slot + h.slots) return &h;
    return nullptr;
}

LowHold* hold_of_range(uintptr_t start, uint64_t bytes) {
    for (LowHold& h : g_low)
        if (start >= h.base && start + bytes <= h.base + h.size) return &h;
    return nullptr;
}

void release_slot(uint64_t slot) {
    gta6rt_vm_decommit(slot_address(slot), rt::xmem::kPhysicalPageSize);
    if (hold_of_slot(slot)) g_held_owner.erase(slot);
    else g_free_slots.push_back(slot);
}

void hold_low_reservation(uintptr_t base, uint64_t size) {
    if (!ensure_pool()) return;
    const uint64_t slots = (size + kPage64k - 1) / kPage64k;
    if (g_next_slot + slots > kPageSlotLimit || (g_next_slot - 1 + slots) * rt::xmem::kPhysicalPageSize > g_pool.size) {
        RTLOG(WARN, "xmem: 32-bit reservation %p: no room for %llu pool slots in address order; its pages stay where they are",
              reinterpret_cast<void*>(base), static_cast<unsigned long long>(slots));
        return;
    }
    g_low.push_back(LowHold{base, size, g_next_slot, slots});
    g_next_slot += slots;
    RTLOG(INFO, "xmem: 32-bit reservation %p size %llu holds pool slots %llu..%llu (host %p) for pages in address order",
          reinterpret_cast<void*>(base), static_cast<unsigned long long>(size), static_cast<unsigned long long>(g_low.back().first_slot),
          static_cast<unsigned long long>(g_low.back().first_slot + slots - 1), slot_address(g_low.back().first_slot));
}

void drop_low_hold(uintptr_t base) {
    for (auto it = g_low.begin(); it != g_low.end(); ++it) {
        if (it->base != base) continue;
        for (uint64_t s = it->first_slot; s < it->first_slot + it->slots; ++s)
            if (!g_held_owner.count(s)) g_free_slots.push_back(s);
        for (uint64_t s = it->first_slot; s < it->first_slot + it->slots; ++s) g_held_owner.erase(s);
        g_low.erase(it);
        return;
    }
}

// Gives the fresh pages about to be mapped at [start, ...) inside a 32-bit reservation the held slot of their address.
void arrange_low_pages(uintptr_t start, const uint64_t* pages, uint64_t count) {
    LowHold* h = hold_of_range(start, count * rt::xmem::kPhysicalPageSize);
    if (!h) return;
    uint64_t moved = 0;
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t page = pages[i];
        if (!page || !g_pages.count(page)) continue;
        const uint64_t want = h->first_slot + (start - h->base) / rt::xmem::kPhysicalPageSize + i;
        const uint64_t have = g_pages[page];
        if (have == want) continue;
        if (!g_fresh.count(page) || g_held_owner.count(want)) {
            h->not_fresh++;
            continue;
        }
        if (gta6rt_vm_commit(slot_address(want), rt::xmem::kPhysicalPageSize, GTA6RT_PROT_READ | GTA6RT_PROT_WRITE) != GTA6RT_OK) {
            h->not_fresh++;
            continue;
        }
        release_slot(have);
        g_pages[page] = want;
        g_held_owner[want] = page;
        ++moved;
    }
    h->moved += moved;
    if (moved)
        RTLOG(INFO, "xmem: 32-bit reservation %p: %llu page(s) mapped at %p given their address-order slot (%llu so far, %llu not fresh)",
              reinterpret_cast<void*>(h->base), static_cast<unsigned long long>(moved), reinterpret_cast<void*>(start),
              static_cast<unsigned long long>(h->moved), static_cast<unsigned long long>(h->not_fresh));
}

void note_low_stretch(uintptr_t start, uint64_t count) {
    LowHold* h = hold_of_range(start, count * rt::xmem::kPhysicalPageSize);
    if (!h || h->stretch_logged) return;
    const uint64_t n = h->size / rt::xmem::kPhysicalPageSize;
    for (uint64_t i = 0; i < n; ++i) {
        auto it = g_mapped.find(h->base + i * rt::xmem::kPhysicalPageSize);
        if (it == g_mapped.end() || slot_of(it->second) != h->first_slot + i) return;
    }
    h->stretch_logged = true;
    RTLOG(INFO, "xmem: 32-bit reservation %p (%llu bytes) is one stretch of the pool at host %p",
          reinterpret_cast<void*>(h->base), static_cast<unsigned long long>(h->size), slot_address(h->first_slot));
}

bool aligned_page(uintptr_t a) { return (a & (rt::xmem::kPhysicalPageSize - 1)) == 0; }

const Window* find_window(uintptr_t address, uint64_t bytes, uintptr_t* base_out = nullptr) {
    auto it = g_windows.upper_bound(address);
    if (it == g_windows.begin()) return nullptr;
    --it;
    if (address < it->first || address + bytes > it->first + it->second.size || address + bytes < address) return nullptr;
    if (base_out) *base_out = it->first;
    return &it->second;
}

void forget_range(std::map<uintptr_t, Window>& m, uintptr_t start, uintptr_t end) {
    for (auto it = m.begin(); it != m.end();) {
        if (it->first >= start && it->first < end) it = m.erase(it);
        else ++it;
    }
}

bool unmap_range_locked(uintptr_t start, uintptr_t end) {
    auto it = g_mapped.lower_bound(start);
    while (it != g_mapped.end() && it->first < end) it = g_mapped.erase(it);
    return gta6rt_vm_decommit(reinterpret_cast<void*>(start), end - start) == GTA6RT_OK;
}

// Maps `count` consecutive slots starting at `slot` onto [dst, ...) with `prot`; false on failure.
bool alias_run(uintptr_t dst, uint64_t slot, uint64_t count, uint32_t prot) {
    return gta6rt_vm_alias(reinterpret_cast<void*>(dst), slot_address(slot), count * rt::xmem::kPhysicalPageSize, prot) == GTA6RT_OK;
}

// ---- XMemVirtualAlloc

bool round_up(uint64_t value, uint64_t alignment, uint64_t& out) {
    if (alignment == 0 || (alignment & (alignment - 1)) || value > std::numeric_limits<uint64_t>::max() - (alignment - 1)) return false;
    out = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

struct AddressRange {
    uint64_t lowest, highest_end;
    bool low32;
};

AddressRange address_range(uint64_t attributes) {
    if (attributes & kXmemAttr32Bit) return {kPage64k, uint64_t{1} << 32, true};
    const unsigned field = (attributes >> kXmemAttrWidthShift) & 7;
    return {0, uint64_t{1} << (field ? 47 - field : 40), false};
}

enum class Low32Mode { Auto, Require, Anywhere };

Low32Mode low32_mode() {
    const char* v = gta6rt_env_get("LEONIDA_XMEM_32BIT");
    if (!v || !*v || std::strcmp(v, "auto") == 0) return Low32Mode::Auto;
    if (std::strcmp(v, "require") == 0) return Low32Mode::Require;
    if (std::strcmp(v, "anywhere") == 0) return Low32Mode::Anywhere;
    RTLOG(ERROR, "xmem: LEONIDA_XMEM_32BIT=%s is not auto, require or anywhere; using auto", v);
    return Low32Mode::Auto;
}

std::atomic<uint64_t> g_low32_above{0};

// `range` constrains a reservation at an address of our choice (XMemVirtualAlloc); null for kernel32 VirtualAlloc.
void* reserve_region(void* address, uint64_t size, uint32_t type, uint64_t alignment, bool large,
                     const AddressRange* range = nullptr, uint64_t attributes = 0) {
    uint32_t flags = 0;
    if (address) flags |= GTA6RT_VM_FIXED;
    if (type & kMemTopDown) flags |= GTA6RT_VM_TOP_DOWN;
    if (type & kMemWriteWatch) flags |= GTA6RT_VM_WRITE_WATCH;
    if (large) flags |= GTA6RT_VM_LARGE_PAGES;
    void* base = nullptr;
    gta6rt_status st;
    if (!address && range) {
        const uint64_t align = alignment > 1 ? alignment : kPage64k;
        st = gta6rt_vm_reserve_range(range->lowest, range->highest_end, size, align, flags, &base);
        if (st != GTA6RT_OK && range->low32) {
            const Low32Mode mode = low32_mode();
            const uint64_t lowest = gta6rt_vm_lowest_mappable();
            const bool no_low_space = lowest >= range->highest_end;
            if (mode == Low32Mode::Require || (mode == Low32Mode::Auto && !no_low_space)) {
                RTLOG(ERROR, "xmem: 32-bit reservation of %llu bytes (attributes 0x%llx) refused: no free range below 4 GiB "
                             "(lowest mappable address %#llx, LEONIDA_XMEM_32BIT=%s)",
                      static_cast<unsigned long long>(size), static_cast<unsigned long long>(attributes),
                      static_cast<unsigned long long>(lowest), mode == Low32Mode::Require ? "require" : "auto");
                gta6rt_last_error_set(kErrNotEnoughMemory);
                return nullptr;
            }
            const uint64_t n = g_low32_above.fetch_add(1, std::memory_order_relaxed) + 1;
            RTLOG(WARN, "xmem: 32-bit reservation of %llu bytes (attributes 0x%llx) placed above 4 GiB (%s, lowest mappable "
                        "address %#llx; %llu so far)",
                  static_cast<unsigned long long>(size), static_cast<unsigned long long>(attributes),
                  mode == Low32Mode::Anywhere ? "LEONIDA_XMEM_32BIT=anywhere" : "this process has no address space below 4 GiB",
                  static_cast<unsigned long long>(lowest), static_cast<unsigned long long>(n));
            const AddressRange any = address_range(attributes & ~kXmemAttr32Bit);
            st = gta6rt_vm_reserve_range(any.lowest, any.highest_end, size, align, flags, &base);
        }
        if (st != GTA6RT_OK && !range->low32)
            RTLOG(WARN, "xmem: reservation of %llu bytes (attributes 0x%llx) refused: nothing free below %#llx",
                  static_cast<unsigned long long>(size), static_cast<unsigned long long>(attributes),
                  static_cast<unsigned long long>(range->highest_end));
    } else {
        st = gta6rt_vm_reserve(address, size, flags, &base);
    }
    if (st != GTA6RT_OK) {
        gta6rt_last_error_set(st == GTA6RT_E_ADDRESS_IN_USE ? kErrInvalidAddress : st == GTA6RT_E_NO_MEMORY ? kErrNotEnoughMemory : kErrInvalidParameter);
        return nullptr;
    }
    if (!address && alignment > 1 && (reinterpret_cast<uintptr_t>(base) & (alignment - 1))) {
        RTLOG(ERROR, "xmem: the host returned %p for a %llu-byte reservation that needs %llu alignment", base,
              static_cast<unsigned long long>(size), static_cast<unsigned long long>(alignment));
        gta6rt_vm_release(base);
        gta6rt_last_error_set(kErrNotEnoughMemory);
        return nullptr;
    }
    return base;
}

void* xmem_virtual_alloc(void* address, uint64_t size, uint32_t allocation_type, uint64_t attributes, uint32_t protect) {
    if (size == 0) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return nullptr;
    }
    const uint32_t page_mode = allocation_type & kXmemPageModeMask;
    if (page_mode != 0 && page_mode != kXmem64kPages && page_mode != kXmemLargePages) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return nullptr;
    }
    if (allocation_type & ~(kHostTypeMask | kXmemPageModeMask)) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return nullptr;
    }
    uint32_t host_type = allocation_type & kHostTypeMask;
    if (host_type == 0) host_type = kMemReserve | kMemCommit;
    const uint32_t vm_prot = to_vm_prot(protect);

    const bool commit = (host_type & kMemCommit) != 0;
    const bool new_placeholder = (host_type & kMemReservePlaceholder) != 0;
    const bool replace = (host_type & kMemReplacePlaceholder) != 0;
    const bool window = page_mode != 0 && !commit && !new_placeholder;

    uint64_t host_page = static_cast<uint64_t>(gta6rt_vm_host_page_size());
    uint64_t page_granularity = host_page;
    uint64_t base_granularity = (host_type & kMemReserve) ? kPage64k : host_page;
    if (page_mode == kXmem64kPages) page_granularity = base_granularity = kPage64k;
    if (page_mode == kXmemLargePages) page_granularity = base_granularity = kPage2m;
    if (new_placeholder || replace) {
        page_granularity = std::max(page_granularity, kPage64k);
        base_granularity = std::max(base_granularity, kPage64k);
    }

    if (address) {
        const uintptr_t requested = reinterpret_cast<uintptr_t>(address);
        if (size > std::numeric_limits<uintptr_t>::max() - requested) {
            gta6rt_last_error_set(kErrInvalidParameter);
            return nullptr;
        }
        const uintptr_t requested_end = requested + size;
        const uintptr_t base = requested & ~(static_cast<uintptr_t>(base_granularity) - 1);
        uint64_t end = 0;
        if (!round_up(requested_end, page_granularity, end)) {
            gta6rt_last_error_set(kErrInvalidParameter);
            return nullptr;
        }
        address = reinterpret_cast<void*>(base);
        size = end - base;
    } else if (replace) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return nullptr;
    } else if (page_granularity > host_page && !round_up(size, page_granularity, size)) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return nullptr;
    }

    const std::lock_guard lock(g_mutex);
    if (replace) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        bool inside = false;
        for (auto& [pb, ps] : g_placeholders)
            if (start >= pb && start + size <= pb + ps) inside = true;
        if (!inside) {
            gta6rt_last_error_set(kErrInvalidAddress);
            return nullptr;
        }
        if (window) {
            forget_range(g_windows, start, start + size);
            g_windows[start] = Window{size, window_vm_prot(protect, address)};
            RTLOG(INFO, "xmem: window %p size %llu protect 0x%x (carved from a placeholder)", address, static_cast<unsigned long long>(size), protect);
            return address;
        }
        if (host_type & kMemCommit) {
            if (gta6rt_vm_commit(address, size, vm_prot) != GTA6RT_OK) {
                gta6rt_last_error_set(kErrNotEnoughMemory);
                return nullptr;
            }
        }
        return address;
    }
    const AddressRange range = address_range(attributes);
    if (new_placeholder || window) {
        void* base = reserve_region(address, size, host_type, base_granularity, page_mode == kXmemLargePages, &range, attributes);
        if (!base) return nullptr;
        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        if (!address && range.low32) hold_low_reservation(b, size);
        if (window) {
            forget_range(g_windows, b, b + size);
            g_windows[b] = Window{size, window_vm_prot(protect, base)};
            RTLOG(INFO, "xmem: window %p size %llu protect 0x%x", base, static_cast<unsigned long long>(size), protect);
        } else {
            g_placeholders[b] = size;
            RTLOG(INFO, "xmem: placeholder reserve %p size %llu", base, static_cast<unsigned long long>(size));
        }
        return base;
    }

    void* base = address;
    if (host_type & (kMemReserve) || (!address && (host_type & kMemCommit))) {
        base = reserve_region(address, size, host_type | kMemReserve, page_mode == kXmemLargePages ? kPage2m : base_granularity,
                              page_mode == kXmemLargePages, &range, attributes);
        if (!base) return nullptr;
    }
    if (host_type & kMemCommit) {
        if (gta6rt_vm_commit(base, size, vm_prot) != GTA6RT_OK) {
            gta6rt_last_error_set(kErrNotEnoughMemory);
            if (host_type & kMemReserve) gta6rt_vm_release(base);
            return nullptr;
        }
    }
    return base;
}

void* WINAPI XMemVirtualAlloc(void* address, size_t size, uint32_t allocation_type, uint64_t attributes, uint32_t protect) {
    note_attributes("XMemVirtualAlloc", attributes, size);
    return xmem_virtual_alloc(address, size, allocation_type, attributes, protect);
}

// ---- kernel32 VirtualAlloc & co, as the title calls them

void* WINAPI TitleVirtualAlloc(void* address, size_t size, uint32_t allocation_type, uint32_t protect) {
    const uint32_t type = allocation_type & ~(kMemGraphics | kMemTitle);
    if (size == 0 || (type & ~(kHostTypeMask | kMemLargePages | kMemPhysical))) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return nullptr;
    }
    const uint32_t vm_prot = to_vm_prot(protect);
    const bool large = (type & kMemLargePages) != 0;
    const std::lock_guard lock(g_mutex);
    if (type & kMemReset) return address;
    void* base = address;
    uint64_t length = size;
    if (address) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(address);
        const uintptr_t aligned = a & ~(static_cast<uintptr_t>(kPage64k) - 1);
        const uintptr_t pg = static_cast<uintptr_t>(gta6rt_vm_host_page_size());
        const uintptr_t end = (a + size + pg - 1) & ~(pg - 1);
        if (type & kMemReserve) {
            base = reinterpret_cast<void*>(aligned);
            length = end - aligned;
        } else {
            base = reinterpret_cast<void*>(a & ~(pg - 1));
            length = end - reinterpret_cast<uintptr_t>(base);
        }
    } else {
        uint64_t rounded = 0;
        if (!round_up(size, large ? kPage2m : gta6rt_vm_host_page_size(), rounded)) {
            gta6rt_last_error_set(kErrInvalidParameter);
            return nullptr;
        }
        length = rounded;
    }
    if ((type & kMemReserve) || (!address && (type & kMemCommit))) {
        void* reserved = reserve_region(address ? base : nullptr, length, type | kMemReserve, large ? kPage2m : kPage64k, large);
        if (!reserved) return nullptr;
        base = reserved;
    }
    if (type & kMemCommit) {
        if (gta6rt_vm_commit(base, length, vm_prot) != GTA6RT_OK) {
            gta6rt_last_error_set(kErrNotEnoughMemory);
            if ((type & kMemReserve) || !address) gta6rt_vm_release(base);
            return nullptr;
        }
    }
    return address && !(type & kMemReserve) ? address : base;
}

constexpr uint32_t kMemCoalescePlaceholders = 0x1, kMemPreservePlaceholder = 0x2;

// Placeholders split with MEM_PRESERVE_PLACEHOLDER stay one host reservation. Each piece records that reservation's
// base; releasing a piece decommits it, and the last released piece releases the reservation.
std::map<uintptr_t, uintptr_t> g_piece_owner;
std::map<uintptr_t, size_t> g_live_pieces;

int placeholder_release(void* address, size_t size, uint32_t free_type) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    const std::lock_guard lock(g_mutex);
    if (size == 0 || (a | size) & (kPage64k - 1)) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return 0;
    }
    if (free_type & kMemPreservePlaceholder) {
        auto it = g_placeholders.upper_bound(a);
        if (it == g_placeholders.begin()) {
            gta6rt_last_error_set(kErrInvalidParameter);
            return 0;
        }
        --it;
        const uintptr_t pb = it->first;
        const uint64_t ps = it->second;
        if (a + size > pb + ps) {
            gta6rt_last_error_set(kErrInvalidParameter);
            return 0;
        }
        if (a == pb && size == ps) return 1;
        const auto owned = g_piece_owner.find(pb);
        const uintptr_t owner = owned != g_piece_owner.end() ? owned->second : pb;
        if (owned == g_piece_owner.end()) g_live_pieces[owner] = 1;
        g_placeholders.erase(it);
        size_t pieces = 0;
        auto add = [&](uintptr_t b, uint64_t n) {
            if (!n) return;
            g_placeholders[b] = n;
            g_piece_owner[b] = owner;
            ++pieces;
        };
        add(pb, a - pb);
        add(a, size);
        add(a + size, pb + ps - (a + size));
        g_live_pieces[owner] += pieces - 1;
        return 1;
    }
    std::vector<uintptr_t> parts;
    uintptr_t cur = a;
    while (cur < a + size) {
        auto it = g_placeholders.find(cur);
        if (it == g_placeholders.end()) {
            gta6rt_last_error_set(kErrInvalidParameter);
            return 0;
        }
        parts.push_back(cur);
        cur += it->second;
    }
    if (cur != a + size || parts.empty()) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return 0;
    }
    const auto first = g_piece_owner.find(a);
    for (uintptr_t p : parts) {
        g_placeholders.erase(p);
        if (p != a) g_piece_owner.erase(p);
    }
    g_placeholders[a] = size;
    if (first != g_piece_owner.end()) g_live_pieces[first->second] -= parts.size() - 1;
    return 1;
}

int title_release(void* address, size_t size, uint32_t free_type) {
    if ((free_type & kMemRelease) && (free_type & (kMemPreservePlaceholder | kMemCoalescePlaceholders)))
        return placeholder_release(address, size, free_type);
    if (free_type & kMemRelease) {
        if (size != 0 || (free_type & kMemDecommit)) {
            gta6rt_last_error_set(kErrInvalidParameter);
            return 0;
        }
        const uintptr_t a = reinterpret_cast<uintptr_t>(address);
        const std::lock_guard lock(g_mutex);
        uint64_t extent = 0;
        if (auto ph = g_placeholders.find(a); ph != g_placeholders.end()) {
            extent = ph->second;
            g_placeholders.erase(ph);
        } else if (auto w = g_windows.find(a); w != g_windows.end()) {
            extent = w->second.size;
        }
        if (extent) {
            for (auto it = g_mapped.lower_bound(a); it != g_mapped.end() && it->first < a + extent;) it = g_mapped.erase(it);
            forget_range(g_windows, a, a + extent);
        }
        if (auto piece = g_piece_owner.find(a); piece != g_piece_owner.end()) {
            const uintptr_t owner = piece->second;
            g_piece_owner.erase(piece);
            if (--g_live_pieces[owner] > 0) {
                gta6rt_vm_decommit(address, extent);
                return 1;
            }
            g_live_pieces.erase(owner);
            address = reinterpret_cast<void*>(owner);
        }
        if (gta6rt_vm_release(address) != GTA6RT_OK) {
            gta6rt_last_error_set(kErrInvalidAddress);
            return 0;
        }
        if (!g_low.empty()) drop_low_hold(reinterpret_cast<uintptr_t>(address));
        return 1;
    }
    if (free_type & kMemDecommit) {
        if (size == 0) {
            gta6rt_last_error_set(kErrInvalidParameter);
            return 0;
        }
        if (gta6rt_vm_decommit(address, size) != GTA6RT_OK) {
            gta6rt_last_error_set(kErrInvalidAddress);
            return 0;
        }
        return 1;
    }
    gta6rt_last_error_set(kErrInvalidParameter);
    return 0;
}

int WINAPI TitleVirtualFree(void* address, size_t size, uint32_t free_type) { return title_release(address, size, free_type); }
int WINAPI TitleVirtualFreeEx(void* process, void* address, size_t size, uint32_t free_type) {
    const auto p = reinterpret_cast<intptr_t>(process);
    if (p != -1 && p != static_cast<intptr_t>(getpid())) {
        gta6rt_last_error_set(6);
        return 0;
    }
    return title_release(address, size, free_type);
}

int WINAPI TitleVirtualProtect(void* address, size_t size, uint32_t protect, uint32_t* old) {
    if (!old) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return 0;
    }
    uint32_t old_vm = 0;
    if (gta6rt_vm_protect(address, size, to_vm_prot(protect), &old_vm) != GTA6RT_OK) {
        gta6rt_last_error_set(kErrInvalidAddress);
        return 0;
    }
    *old = to_win_prot(old_vm);
    return 1;
}

// MEMORY_BASIC_INFORMATION, Windows x64 (48 bytes).
struct WinMbi {
    uint64_t BaseAddress, AllocationBase;
    uint32_t AllocationProtect;
    uint16_t PartitionId;
    uint64_t RegionSize;
    uint32_t State, Protect, Type;
};
static_assert(sizeof(WinMbi) == 48 && offsetof(WinMbi, RegionSize) == 24);

size_t query_into(const void* address, WinMbi* out) {
    gta6rt_vm_region r{};
    std::memset(out, 0, sizeof *out);
    if (gta6rt_vm_query(address, &r) != GTA6RT_OK) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return 0;
    }
    out->BaseAddress = reinterpret_cast<uintptr_t>(r.base);
    out->AllocationBase = reinterpret_cast<uintptr_t>(r.allocation_base);
    out->AllocationProtect = to_win_prot(r.allocation_prot);
    out->RegionSize = r.size;
    out->State = r.state == GTA6RT_VM_STATE_COMMITTED ? 0x1000 : r.state == GTA6RT_VM_STATE_RESERVED ? 0x2000 : 0x10000;
    out->Protect = r.state == GTA6RT_VM_STATE_COMMITTED ? to_win_prot(r.prot) : 0;
    out->Type = r.state == GTA6RT_VM_STATE_FREE ? 0 : (r.is_image ? 0x1000000 : 0x20000);
    return sizeof *out;
}

size_t WINAPI TitleVirtualQuery(const void* address, WinMbi* out, size_t length) {
    if (!out || length < sizeof *out) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return 0;
    }
    return query_into(address, out);
}

size_t WINAPI TitleVirtualQueryEx(void* process, const void* address, WinMbi* out, size_t length) {
    const auto p = reinterpret_cast<intptr_t>(process);
    if (p != -1 && p != static_cast<intptr_t>(getpid())) {
        gta6rt_last_error_set(6);
        return 0;
    }
    return TitleVirtualQuery(address, out, length);
}

constexpr size_t kVirtualQueryOutSize = 64;

size_t WINAPI XMemVirtualQuery(void* process, const void* address, void* out) {
    if (!out) return 0;
    std::memset(out, 0, kVirtualQueryOutSize);
    if (!address) return 0;
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        RTLOG(INFO, "xmem: XMemVirtualQuery: fields after MEMORY_BASIC_INFORMATION are unknown; the 64-byte output is zero-filled there");
    WinMbi info;
    const size_t n = TitleVirtualQueryEx(process ? process : reinterpret_cast<void*>(static_cast<intptr_t>(-1)), address, &info, sizeof info);
    if (!n) return 0;
    std::memcpy(out, &info, n);
    return n;
}

// BOOL XMemGetWorkingSetStatistics(UINT32 kind, XMEM_WORKING_SET_STATISTICS* stats): +0 the title's memory limit, +8 the
// working set (measured at the seven FSR call sites; kind 0x20000001 is not decoded). LEONIDA_TITLE_MEMORY_MB
// (2048..65536) overrides the limit.
int WINAPI XMemGetWorkingSetStatistics(uint32_t kind, void* stats) {
    (void)kind;
    if (!stats) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) RTLOG(WARN, "xmem: XMemGetWorkingSetStatistics(nullptr) -> FALSE");
        return 0;
    }
    uint64_t total = 0;
    size_t n = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &n, nullptr, 0) != 0 || total == 0) total = 12ull << 30;
    if (const char* env = gta6rt_env_get("LEONIDA_TITLE_MEMORY_MB")) {
        if (*env != '-' && *env != '+') {
            char* end = nullptr;
            errno = 0;
            unsigned long long mib = std::strtoull(env, &end, 10);
            if (errno == 0 && end != env && *end == '\0' && mib > 0) {
                mib = std::clamp<unsigned long long>(mib, 2048, 65536);
                total = mib << 20;
            }
        }
    }
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    uint64_t working_set = 0;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        working_set = info.resident_size;
    auto* bytes = static_cast<uint8_t*>(stats);
    std::memset(bytes, 0, 16);
    std::memcpy(bytes, &total, 8);
    std::memcpy(bytes + 8, &working_set, 8);
    return 1;
}

// ---- physical pages

int WINAPI XMemAllocatePhysicalPages(uint64_t attributes, uint64_t* page_count, uint64_t* page_array) {
    note_attributes("XMemAllocatePhysicalPages", attributes, kNoSize);
    if (!page_count || (*page_count && !page_array)) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return 0;
    }
    const uint64_t count = *page_count;
    if (count == 0) return 1;
    const std::lock_guard lock(g_mutex);
    if (count > g_free_slots.size() + (kPageSlotLimit - g_next_slot)) {
        *page_count = 0;
        gta6rt_last_error_set(kErrNotEnoughMemory);
        return 0;
    }
    if (!ensure_pool()) {
        *page_count = 0;
        gta6rt_last_error_set(kErrNotSupported);
        return 0;
    }
    std::vector<uint64_t> slots;
    slots.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t slot;
        if (!g_free_slots.empty()) {
            slot = g_free_slots.back();
            g_free_slots.pop_back();
        } else {
            slot = g_next_slot++;
        }
        if ((slot - 1) * rt::xmem::kPhysicalPageSize + rt::xmem::kPhysicalPageSize > g_pool.size) {
            g_free_slots.push_back(slot);
            for (uint64_t s : slots) g_free_slots.push_back(s);
            *page_count = 0;
            gta6rt_last_error_set(kErrNotEnoughMemory);
            return 0;
        }
        slots.push_back(slot);
    }
    for (uint64_t i = 0; i < count;) {
        uint64_t run = 1;
        while (i + run < count && slots[i + run] == slots[i] + run) ++run;
        if (gta6rt_vm_commit(slot_address(slots[i]), run * rt::xmem::kPhysicalPageSize, GTA6RT_PROT_READ | GTA6RT_PROT_WRITE) != GTA6RT_OK) {
            for (uint64_t s : slots) g_free_slots.push_back(s);
            *page_count = 0;
            gta6rt_last_error_set(kErrNotEnoughMemory);
            return 0;
        }
        i += run;
    }
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t id;
        if (!g_free_ids.empty()) {
            id = g_free_ids.back();
            g_free_ids.pop_back();
        } else {
            id = g_next_id++;
        }
        const uint64_t page = id * kFramesPerPage;
        g_pages[page] = slots[i];
        g_fresh.insert(page);
        page_array[i] = page;
    }
    return 1;
}

int WINAPI XMemFreePhysicalPages(uint64_t page_count, const uint64_t* page_array) {
    if (page_count && !page_array) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return 0;
    }
    const std::lock_guard lock(g_mutex);
    for (uint64_t i = 0; i < page_count; ++i) {
        auto it = g_pages.find(page_array[i]);
        if (it == g_pages.end()) {
            gta6rt_last_error_set(kErrInvalidParameter);
            return 0;
        }
        const uint64_t slot = it->second;
        g_pages.erase(it);
        g_fresh.erase(page_array[i]);
        release_slot(slot);
        g_free_ids.push_back(page_array[i] / kFramesPerPage);
    }
    return 1;
}

bool map_pages_locked(uintptr_t start, const uint64_t* pages, uint64_t count, uint32_t prot, uint64_t& done, uint32_t& error) {
    done = 0;
    if (!g_low.empty()) arrange_low_pages(start, pages, count);
    for (uint64_t i = 0; i < count;) {
        if (pages[i] == 0) {
            ++i;
            continue;
        }
        if (!g_pages.count(pages[i])) {
            error = kErrInvalidParameter;
            return false;
        }
        const uint64_t slot = g_pages[pages[i]];
        uint64_t run = 1;
        while (i + run < count && pages[i + run] && slot_of(pages[i + run]) == slot + run) ++run;
        if (!alias_run(start + i * rt::xmem::kPhysicalPageSize, slot, run, prot)) {
            error = kErrInvalidAddress;
            return false;
        }
        for (uint64_t k = 0; k < run; ++k) {
            g_mapped[start + (i + k) * rt::xmem::kPhysicalPageSize] = pages[i + k];
            g_fresh.erase(pages[i + k]);
        }
        i += run;
        done = i;
    }
    done = count;
    if (!g_low.empty()) note_low_stretch(start, count);
    return true;
}

void* WINAPI XMemMapPhysicalPages(void* address, uint64_t page_count, const uint64_t* page_array) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (!aligned_page(start) || page_count > std::numeric_limits<uintptr_t>::max() / rt::xmem::kPhysicalPageSize ||
        start > std::numeric_limits<uintptr_t>::max() - page_count * rt::xmem::kPhysicalPageSize) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return nullptr;
    }
    const uintptr_t end = start + page_count * rt::xmem::kPhysicalPageSize;
    const std::lock_guard lock(g_mutex);
    if (!page_array) {
        if (!unmap_range_locked(start, end)) {
            gta6rt_last_error_set(kErrInvalidAddress);
            return nullptr;
        }
        return address;
    }
    const Window* window = find_window(start, page_count * rt::xmem::kPhysicalPageSize);
    if (!window) {
        RTLOG(WARN, "xmem: map physical pages %p refused: the range is not in one window", address);
        gta6rt_last_error_set(kErrInvalidAddress);
        return nullptr;
    }
    if (!unmap_range_locked(start, end)) {
        gta6rt_last_error_set(kErrInvalidAddress);
        return nullptr;
    }
    uint64_t done = 0;
    uint32_t error = 0;
    if (!map_pages_locked(start, page_array, page_count, window->vm_prot, done, error)) {
        RTLOG(WARN, "xmem: map physical pages %p refused (error %u)", address, error);
        gta6rt_last_error_set(error);
        return nullptr;
    }
    return address;
}

int WINAPI XMemMapPhysicalPagesScatter(void* const* addresses, uint32_t page_count, const uint64_t* page_array) {
    const std::lock_guard lock(g_mutex);
    if (page_count && !addresses) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return 0;
    }
    for (uint32_t i = 0; i < page_count; ++i) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        if (!aligned_page(address)) {
            gta6rt_last_error_set(kErrInvalidParameter);
            return 0;
        }
        const uint64_t page = page_array ? page_array[i] : 0;
        if (page == 0) {
            if (!unmap_range_locked(address, address + rt::xmem::kPhysicalPageSize)) {
                gta6rt_last_error_set(kErrInvalidAddress);
                return 0;
            }
            continue;
        }
        const Window* window = find_window(address, rt::xmem::kPhysicalPageSize);
        if (!window) {
            gta6rt_last_error_set(kErrInvalidAddress);
            return 0;
        }
        unmap_range_locked(address, address + rt::xmem::kPhysicalPageSize);
        uint64_t done;
        uint32_t error = 0;
        if (!map_pages_locked(address, &page, 1, window->vm_prot, done, error)) {
            gta6rt_last_error_set(error);
            return 0;
        }
    }
    return 1;
}

// BOOL XMemTransferMemory(XMEM_TRANSFER_TYPE type, SIZE_T size): moves budget between the title's partition and the
// system's on the console. A Mac has one pool, so there is nothing to move: every direction and size returns TRUE and
// nothing is allocated (logged once).
int WINAPI XMemTransferMemory(uint32_t type, size_t size) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        RTLOG(INFO, "xmem: XMemTransferMemory(type %u, %zu bytes): no title/system memory partition on this host; returning TRUE without moving anything (logged once)",
              type, size);
    return 1;
}

struct Registration {
    Registration() { rt::xmem::copy_page_mappings_fn = &rt::xmem::copy_page_mappings; }
} g_registration;

}  // namespace

namespace rt::xmem {

void* physical_page_host_address(uint64_t page) {
    const std::lock_guard lock(g_mutex);
    if (!g_pool.base || !g_pages.count(page)) return nullptr;
    g_fresh.erase(page);
    return slot_address(g_pages[page]);
}

bool copy_page_mappings(void* destination, const void* source, uint64_t pages, bool repeat_first) {
    const uintptr_t dst = reinterpret_cast<uintptr_t>(destination), src = reinterpret_cast<uintptr_t>(source);
    if (!aligned_page(dst) || (source && !aligned_page(src))) {
        gta6rt_last_error_set(kErrInvalidParameter);
        return false;
    }
    const std::lock_guard lock(g_mutex);
    const Window* window = find_window(dst, pages * kPhysicalPageSize);
    if (!window) {
        gta6rt_last_error_set(kErrInvalidAddress);
        return false;
    }
    std::vector<uint64_t> numbers(pages, 0);
    if (source) {
        for (uint64_t i = 0; i < pages; ++i) {
            const uintptr_t at = repeat_first ? src : src + i * kPhysicalPageSize;
            auto it = g_mapped.find(at);
            if (it != g_mapped.end()) numbers[i] = it->second;
        }
    }
    if (!unmap_range_locked(dst, dst + pages * kPhysicalPageSize)) {
        gta6rt_last_error_set(kErrInvalidAddress);
        return false;
    }
    uint64_t done;
    uint32_t error = 0;
    if (!map_pages_locked(dst, numbers.data(), pages, window->vm_prot, done, error)) {
        gta6rt_last_error_set(error);
        return false;
    }
    return true;
}

bool transfer_physical(std::span<const uint64_t> pages, uint16_t offset, std::span<uint8_t> staging, bool write) {
    const std::lock_guard lock(g_mutex);
    uint64_t at = offset;
    for (uint64_t page : pages)
        if (!g_pages.count(page)) return false;
    if (at + staging.size() > pages.size() * kPhysicalPageSize) return false;
    size_t done = 0;
    while (done < staging.size()) {
        const uint64_t index = (at + done) / kPhysicalPageSize, within = (at + done) % kPhysicalPageSize;
        const size_t chunk = std::min<size_t>(staging.size() - done, kPhysicalPageSize - within);
        uint8_t* host = slot_address(g_pages[pages[index]]) + within;
        g_fresh.erase(pages[index]);
        if (write) std::memcpy(host, staging.data() + done, chunk);
        else std::memcpy(staging.data() + done, host, chunk);
        done += chunk;
    }
    return true;
}

std::vector<LowReservation> low_reservations() {
    const std::lock_guard lock(g_mutex);
    std::vector<LowReservation> out;
    for (const LowHold& h : g_low) out.push_back({reinterpret_cast<void*>(h.base), h.size, slot_address(h.first_slot), h.stretch_logged});
    return out;
}

PoolInfo pool_info() {
    const std::lock_guard lock(g_mutex);
    return {g_pool.base, g_pool.size, g_pages.size(), g_next_slot - 1, g_free_slots.size()};
}

std::vector<std::pair<uintptr_t, uint64_t>> window_ranges() {
    const std::lock_guard lock(g_mutex);
    std::vector<std::pair<uintptr_t, uint64_t>> out;
    out.reserve(g_windows.size());
    for (const auto& [base, w] : g_windows) out.emplace_back(base, w.size);
    return out;
}

size_t live_physical_pages() {
    const std::lock_guard lock(g_mutex);
    return g_pages.size();
}
uint64_t low32_placed_above() { return g_low32_above.load(std::memory_order_relaxed); }

size_t window_count() {
    const std::lock_guard lock(g_mutex);
    return g_windows.size();
}
size_t mapped_page_count() {
    const std::lock_guard lock(g_mutex);
    return g_mapped.size();
}

}  // namespace rt::xmem

namespace {
#define XM(n) SHIM("xmem.dll", #n, n)
}  // namespace

SHIM_TABLE(xmem,
    XM(XMemAlloc), XM(XMemFree), XM(XMemAllocDefault), XM(XMemFreeDefault), XM(XMemIsToolsMemAvailable),
    XM(XMemVirtualQuery), XM(XMemGetWorkingSetStatistics), XM(XMemVirtualAlloc), XM(XMemAllocatePhysicalPages),
    XM(XMemFreePhysicalPages), XM(XMemMapPhysicalPages), XM(XMemMapPhysicalPagesScatter), XM(XMemTransferMemory),
    SHIM("kernel32.dll", "VirtualAlloc", TitleVirtualAlloc), SHIM("kernel32.dll", "VirtualFree", TitleVirtualFree),
    SHIM("kernel32.dll", "VirtualFreeEx", TitleVirtualFreeEx), SHIM("kernel32.dll", "VirtualProtect", TitleVirtualProtect),
    SHIM("kernel32.dll", "VirtualQuery", TitleVirtualQuery), SHIM("kernel32.dll", "VirtualQueryEx", TitleVirtualQueryEx),
);
