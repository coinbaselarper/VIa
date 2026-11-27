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
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
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
    uint64_t physical = 0;  // the slots physical pages may take: [1, physical / 64 KiB]; above them, virtual runs
    mach_port_t entry = MACH_PORT_NULL;
};

// GTA6RT_METAL_TITLE_POOL=1: the pool is one Mach memory entry mapped once over its reservation and never moved,
// protected or unmapped while the process lives, so a GPU buffer made over the pool's host range stays valid
// whatever the title does to its own views. Freed slots are zeroed when handed out again instead of decommitted
// (a decommit maps fresh memory over the host range), and committed XMemVirtualAlloc and MEM_GRAPHICS VirtualAlloc
// memory is a run of pool slots the title's range is a view of.
// On by default on the Metal path (GTA6RT_GPU=metal); GTA6RT_METAL_TITLE_POOL=1 / =0 forces it either way.
bool entry_pool_default(const char* pool, const char* gpu) {
    if (pool && *pool) return std::strcmp(pool, "1") == 0;
    return gpu && std::strcmp(gpu, "metal") == 0;
}
bool entry_pool_wanted() {
    static const bool on = entry_pool_default(gta6rt_env_get("GTA6RT_METAL_TITLE_POOL"), gta6rt_env_get("GTA6RT_GPU"));
    return on;
}

// Physical slots on the free list, as a bitmap: a multi-page allocation can then take consecutive slots (one pool
// stretch for the GPU side), and any other takes the lowest free one.
class FreeSlots {
public:
    void push_back(uint64_t slot) {
        if (slot / 64 >= words_.size()) words_.resize(slot / 64 + 1, 0);
        uint64_t& w = words_[slot / 64];
        const uint64_t m = uint64_t{1} << (slot & 63);
        if (w & m) return;
        w |= m;
        ++count_;
        low_ = std::min<size_t>(low_, slot / 64);
    }
    bool empty() const { return !count_; }
    size_t size() const { return count_; }
    uint64_t take_any() {
        while (low_ < words_.size() && !words_[low_]) ++low_;
        if (low_ >= words_.size()) return 0;
        const uint64_t slot = low_ * 64 + std::countr_zero(words_[low_]);
        clear(slot);
        return slot;
    }
    // The first run of n consecutive free slots, taken; 0 when there is none.
    uint64_t take_run(uint64_t n) {
        if (n > count_) return 0;
        uint64_t start = 0, len = 0;
        for (size_t wi = low_; wi < words_.size(); ++wi) {
            const uint64_t w = words_[wi];
            if (!w) {
                len = 0;
                continue;
            }
            if (w == ~uint64_t{0} && (len || n >= 64)) {
                if (!len) start = wi * 64;
                len += 64;
                if (len >= n) return claim(start, n);
                continue;
            }
            for (unsigned b = 0; b < 64; ++b) {
                if (!(w >> b & 1)) {
                    len = 0;
                    continue;
                }
                if (!len) start = wi * 64 + b;
                if (++len >= n) return claim(start, n);
            }
        }
        return 0;
    }

private:
    void clear(uint64_t slot) {
        words_[slot / 64] &= ~(uint64_t{1} << (slot & 63));
        --count_;
    }
    uint64_t claim(uint64_t start, uint64_t n) {
        for (uint64_t s = start; s < start + n; ++s) clear(s);
        return start;
    }
    std::vector<uint64_t> words_;
    size_t count_ = 0, low_ = 0;
};

// A range below 4 GiB reserved (no access, no memory) from process start for the title's 32-bit reservations: large
// "anywhere" mappings made before the title asks (Metal's binary archive shards with the pipeline tier on) otherwise
// take the low 4 GiB of a process with a small page zero, and the title's 185 MB reservation found no room (fr10-c..g).
// LEONIDA_XMEM_LOW_HOLD_MB (256 by default on the Metal path, GTA6RT_GPU=metal; none elsewhere unless set; 0: none).
struct LowVaHold {
    uintptr_t base = 0;
    uint64_t size = 0;
    uint64_t asked = 0;
    bool tried = false;
};
LowVaHold g_low_va;
bool g_base_hint_defaulted = false;  // GTA6RT_VM_BASE_HINT set by Registration (title pool default)

void hold_low_va() {
    g_low_va.tried = true;
    const char* v = std::getenv("LEONIDA_XMEM_LOW_HOLD_MB");
    const char* gpu = std::getenv("GTA6RT_GPU");
    const bool metal = gpu && std::strcmp(gpu, "metal") == 0;
    const uint64_t mb = v && *v ? std::strtoull(v, nullptr, 10) : metal ? 256 : 0;
    if (!mb || mb >= 4096) return;
    const uint64_t bytes = mb << 20, top = uint64_t{1} << 32;
    g_low_va.asked = bytes;
    uint64_t best = 0;
    mach_vm_address_t at = kPage64k;
    uint64_t gap_start = kPage64k;
    for (int i = 0; i < 4096 && gap_start < top; ++i) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const bool found = mach_vm_region(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64,
                                          reinterpret_cast<vm_region_info_t>(&info), &n, &object) == KERN_SUCCESS;
        const uint64_t gap_end = std::min<uint64_t>(found ? at : top, top);
        if (gap_end > gap_start && gap_end - gap_start >= bytes) {
            const uint64_t place = (gap_end - bytes) & ~(kPage64k - 1);
            if (place >= gap_start) best = place;
        }
        if (!found) break;
        gap_start = at + size;
        at = gap_start;
    }
    if (!best) return;
    mach_vm_address_t place = best;
    if (mach_vm_map(mach_task_self(), &place, bytes, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE,
                    VM_PROT_NONE, VM_INHERIT_NONE) != KERN_SUCCESS)
        return;
    g_low_va.base = static_cast<uintptr_t>(place);
    g_low_va.size = bytes;
}

// [at, at + size) carved from the top of the held range and given back to the system, so the caller reserves it at
// once with a fixed address; 0 when the hold is too small. g_mutex held.
uintptr_t take_low_va(uint64_t size, uint64_t align) {
    if (!g_low_va.size || size > g_low_va.size) return 0;
    const uint64_t a = align > kPage64k ? align : kPage64k;
    const uintptr_t end = g_low_va.base + g_low_va.size;
    const uintptr_t at = (end - size) & ~(uintptr_t(a) - 1);
    if (at < g_low_va.base) return 0;
    if (mach_vm_deallocate(mach_task_self(), at, end - at) != KERN_SUCCESS) return 0;
    g_low_va.size = at - g_low_va.base;
    return at;
}

// What holds the address space below 4 GiB when the title asks for it: bytes by mapping kind (file-backed mappings by
// file name), the largest first. Once per process.
void report_low_va() {
    static bool done = false;
    if (done) return;
    done = true;
    std::map<std::string, std::pair<uint64_t, uint64_t>> kinds;  // name -> (bytes, mappings)
    uint64_t total = 0, free_bytes = 0, largest_gap = 0, gap_start = kPage64k;
    mach_vm_address_t at = kPage64k;
    const uint64_t top = uint64_t{1} << 32;
    for (int i = 0; i < 65536; ++i) {
        mach_vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_info_data_64_t info{};
        mach_msg_type_number_t n = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &at, &size, &depth, reinterpret_cast<vm_region_recurse_info_t>(&info),
                                   &n) != KERN_SUCCESS || at >= top) {
            const uint64_t gap = top > gap_start ? top - gap_start : 0;
            free_bytes += gap;
            largest_gap = std::max(largest_gap, gap);
            break;
        }
        if (at > gap_start) {
            free_bytes += at - gap_start;
            largest_gap = std::max<uint64_t>(largest_gap, at - gap_start);
        }
        const uint64_t bytes = std::min<uint64_t>(at + size, top) - at;
        char name[96];
        char path[1024] = {};
        if (info.share_mode != SM_EMPTY && proc_regionfilename(getpid(), at, path, sizeof path) > 0) {
            const char* slash = std::strrchr(path, '/');
            const char* base = slash ? slash + 1 : path;
            const char* dash = std::strstr(base, ".metalarchive");
            std::snprintf(name, sizeof name, "file %s", dash ? "*.metalarchive" : base);
        } else if (g_low_va.size && at >= g_low_va.base && at < g_low_va.base + g_low_va.size) {
            std::snprintf(name, sizeof name, "xmem low hold");
        } else {
            std::snprintf(name, sizeof name, "tag %u prot %d/%d", info.user_tag, info.protection, info.max_protection);
        }
        auto& k = kinds[name];
        k.first += bytes;
        ++k.second;
        total += bytes;
        gap_start = at + size;
        at = gap_start;
    }
    std::vector<std::pair<uint64_t, std::string>> order;
    for (const auto& [name, k] : kinds) order.emplace_back(k.first, name + " " + std::to_string(k.first >> 20) + " MiB in " +
                                                                    std::to_string(k.second));
    std::sort(order.rbegin(), order.rend());
    std::string line;
    for (size_t i = 0; i < order.size() && i < 10; ++i) line += (i ? "; " : "") + order[i].second;
    RTLOG(INFO, "xmem: below 4 GiB when the title asks: %llu MiB mapped, %llu MiB free, largest gap %llu MiB; held for "
                "it %llu MiB at %p (asked %llu); by kind: %s",
          static_cast<unsigned long long>(total >> 20), static_cast<unsigned long long>(free_bytes >> 20),
          static_cast<unsigned long long>(largest_gap >> 20), static_cast<unsigned long long>(g_low_va.size >> 20),
          reinterpret_cast<void*>(g_low_va.base), static_cast<unsigned long long>(g_low_va.asked >> 20), line.c_str());
}

constexpr uint64_t kPageSlotLimit = uint64_t{1} << 21;
constexpr uint64_t kFramesPerPage = rt::xmem::kPhysicalPageSize / 0x1000;

std::mutex g_mutex;
std::map<uintptr_t, Window> g_windows;       // base -> window (reserve-only regions physical pages map into)
std::map<uintptr_t, uint64_t> g_placeholders;  // base -> size, the title's MEM_RESERVE_PLACEHOLDER reservations
std::unordered_map<uint64_t, uint64_t> g_pages;  // live page number -> pool slot backing it
std::unordered_set<uint64_t> g_fresh;            // live pages never mapped, written or handed to the host as memory
std::vector<uint64_t> g_free_ids;
uint64_t g_next_id = 1;
FreeSlots g_free_slots;
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
bool g_entry = false;  // the pool is a memory entry (entry_pool_wanted and it could be made)

// Entry pool only: a title range [base, base + size) whose committed pages are views of `slots` consecutive pool
// slots from `first_slot` (committed XMemVirtualAlloc / VirtualAlloc memory). Per 4 KiB page: whether it is a view
// now, and whether the title decommitted it since (no access, zeroed when committed again).
struct VirtualRun {
    uint64_t size = 0, first_slot = 0, slots = 0;
    std::vector<uint8_t> state;  // per 4 KiB page: kViewBit | kDecommittedBit
};
constexpr uint8_t kViewBit = 1, kDecommittedBit = 2;
constexpr uint64_t kSmallPage = 0x1000;
std::map<uintptr_t, VirtualRun> g_runs;                    // by title base
std::vector<std::pair<uint64_t, uint64_t>> g_free_runs;    // (first slot, slots) released runs, zeroed on reuse
uint64_t g_next_run_slot = 0;                              // bump allocation above the physical slots
std::unordered_set<uint64_t> g_dirty_free;                 // free physical slots the title wrote (zeroed on reuse)
uint64_t g_run_bytes = 0, g_runs_made = 0, g_runs_refused = 0, g_plain_commits = 0, g_plain_commit_bytes = 0;
uint64_t g_contiguous_allocations = 0, g_scattered_allocations = 0;
// Entry pool: time spent making title views of pool slots (commit_in_run) and zeroing reused slots, and how often.
uint64_t g_view_ns = 0, g_views = 0, g_zero_ns = 0, g_zeroed_bytes = 0;
uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
}
void note_view_time(uint64_t ns) {
    g_view_ns += ns;
    const uint64_t n = ++g_views;
    if (!(n & (n - 1)) && n >= 256)
        RTLOG(INFO, "xmem: title pool: %llu commits made views of pool slots in %.1f ms; %.1f MiB of reused slots zeroed in "
                    "%.1f ms",
              static_cast<unsigned long long>(n), double(g_view_ns) / 1e6, double(g_zeroed_bytes) / 1048576.0,
              double(g_zero_ns) / 1e6);
}
// LEONIDA_XMEM_CONTIG_SLACK_MB (256): how much committed memory may sit on the free list while a multi-page
// allocation still takes fresh slots for a consecutive run instead of scattered free ones.
uint64_t contiguous_slack_slots() {
    static const uint64_t slots = [] {
        const char* v = gta6rt_env_get("LEONIDA_XMEM_CONTIG_SLACK_MB");
        const uint64_t mb = v && *v ? std::strtoull(v, nullptr, 10) : 256;
        return (mb << 20) / rt::xmem::kPhysicalPageSize;
    }();
    return slots;
}

// Entry pool only: what changed in the title-address -> pool table, for a GPU layer that caches translations.
// Every change bumps g_generation; a removal (unmap, remap, decommit, release) also records its title range.
std::atomic<uint64_t> g_generation{0};  // written under g_mutex, read without it
std::deque<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> g_removals;  // (generation, [lo, hi))
constexpr size_t kRemovalLog = 8192;
uint64_t g_removals_dropped = 0;  // the newest generation whose removal is no longer in g_removals

void note_added() {
    if (g_entry) g_generation.fetch_add(1, std::memory_order_release);
}
void note_removed(uint64_t lo, uint64_t hi) {
    if (!g_entry || lo >= hi) return;
    const uint64_t generation = g_generation.fetch_add(1, std::memory_order_release) + 1;
    if (!g_removals.empty() && g_removals.back().second.second == lo) {
        g_removals.back().second.second = hi;
        g_removals.back().first = generation;
        return;
    }
    g_removals.push_back({generation, {lo, hi}});
    if (g_removals.size() > kRemovalLog) {
        g_removals_dropped = g_removals.front().first;
        g_removals.pop_front();
    }
}

uint64_t pool_bytes() {
    uint64_t gib = rt::shims::console_profile().pool_gib;
    if (const char* v = gta6rt_env_get("LEONIDA_XMEM_POOL_GIB")) {
        const unsigned long long n = std::strtoull(v, nullptr, 10);
        if (n >= 1 && n <= 128) gib = n;
    }
    return gib << 30;
}

// The entry pool's room for virtual runs above the physical slots (VA only until the title touches it).
uint64_t virtual_run_bytes() {
    uint64_t gib = 8;
    if (const char* v = gta6rt_env_get("GTA6RT_XMEM_POOL_VIRTUAL_GIB")) {
        const unsigned long long n = std::strtoull(v, nullptr, 10);
        if (n <= 64) gib = n;
    }
    return gib << 30;
}

bool map_entry_over(void* base, uint64_t size) {
    memory_object_size_t entry_size = size;
    mach_port_t entry = MACH_PORT_NULL;
    kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &entry_size, 0,
                                                 MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &entry, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS || entry_size < size) {
        RTLOG(ERROR, "xmem: title pool: mach_make_memory_entry_64 of %llu bytes failed (%d)", static_cast<unsigned long long>(size), kr);
        if (entry != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), entry);
        return false;
    }
    mach_vm_address_t at = reinterpret_cast<mach_vm_address_t>(base);
    kr = mach_vm_map(mach_task_self(), &at, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry, 0, FALSE,
                     VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || at != reinterpret_cast<mach_vm_address_t>(base)) {
        RTLOG(ERROR, "xmem: title pool: mapping the memory entry over %p failed (%d); the reservation is lost", base, kr);
        mach_port_deallocate(mach_task_self(), entry);
        return false;
    }
    g_pool.entry = entry;
    return true;
}

bool ensure_pool() {
    if (g_pool.base) return true;
    const uint64_t physical = std::min<uint64_t>(pool_bytes(), kPageSlotLimit * rt::xmem::kPhysicalPageSize);
    const bool entry = entry_pool_wanted();
    const uint64_t size = entry ? std::min<uint64_t>(physical + virtual_run_bytes(), kPageSlotLimit * rt::xmem::kPhysicalPageSize)
                                : physical;
    void* base = nullptr;
    if (gta6rt_vm_reserve(nullptr, size, 0, &base) != GTA6RT_OK) {
        RTLOG(ERROR, "xmem: cannot reserve the %llu GiB physical pool", static_cast<unsigned long long>(size >> 30));
        return false;
    }
    if (entry) {
        if (reinterpret_cast<uintptr_t>(base) & 0x3fff) {
            RTLOG(ERROR, "xmem: title pool: the reservation at %p is not 16 KiB aligned; GTA6RT_METAL_TITLE_POOL is ignored", base);
        } else if (map_entry_over(base, size)) {
            g_entry = true;
        } else {
            RTLOG(ERROR, "xmem: title pool: no memory entry; GTA6RT_METAL_TITLE_POOL is ignored, plain pool");
            gta6rt_vm_release(base);
            base = nullptr;
            if (gta6rt_vm_reserve(nullptr, physical, 0, &base) != GTA6RT_OK) {
                RTLOG(ERROR, "xmem: cannot reserve the %llu GiB physical pool", static_cast<unsigned long long>(physical >> 30));
                return false;
            }
        }
    }
    g_pool.base = base;
    g_pool.size = g_entry ? size : physical;
    g_pool.physical = physical;
    g_next_run_slot = physical / rt::xmem::kPhysicalPageSize + 1;
    if (g_entry)
        RTLOG(INFO, "xmem: title pool: one memory entry of %llu GiB at %p (physical pages %llu GiB, virtual runs %llu GiB); "
                    "title pages are views of it; GTA6RT_VM_BASE_HINT %s%s",
              static_cast<unsigned long long>(size >> 30), base, static_cast<unsigned long long>(physical >> 30),
              static_cast<unsigned long long>((size - physical) >> 30),
              std::getenv("GTA6RT_VM_BASE_HINT") ? std::getenv("GTA6RT_VM_BASE_HINT") : "unset",
              g_base_hint_defaulted ? " (the title pool's default)" : "");
    else
        RTLOG(INFO, "xmem: physical pool %llu GiB at %p", static_cast<unsigned long long>(physical >> 30), base);
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

void release_slot(uint64_t slot, bool written = true) {
    if (g_entry) {
        if (written) g_dirty_free.insert(slot);
    } else {
        gta6rt_vm_decommit(slot_address(slot), rt::xmem::kPhysicalPageSize);
    }
    if (hold_of_slot(slot)) g_held_owner.erase(slot);
    else g_free_slots.push_back(slot);
}

// Entry pool: a slot taken from the free list holds what its last page held; zero it before it is handed out.
void zero_if_dirty(uint64_t slot) {
    if (g_entry && g_dirty_free.erase(slot)) {
        const uint64_t t0 = now_ns();
        std::memset(slot_address(slot), 0, rt::xmem::kPhysicalPageSize);
        g_zero_ns += now_ns() - t0;
        g_zeroed_bytes += rt::xmem::kPhysicalPageSize;
    }
}

// ---- entry pool: committed virtual memory as runs of pool slots

uint64_t take_run_slots(uint64_t slots) {
    for (auto it = g_free_runs.begin(); it != g_free_runs.end(); ++it) {
        if (it->second < slots) continue;
        const uint64_t first = it->first;
        const uint64_t t0 = now_ns();
        std::memset(slot_address(first), 0, slots * rt::xmem::kPhysicalPageSize);
        g_zero_ns += now_ns() - t0;
        g_zeroed_bytes += slots * rt::xmem::kPhysicalPageSize;
        if (it->second == slots) g_free_runs.erase(it);
        else *it = {first + slots, it->second - slots};
        return first;
    }
    const uint64_t end = g_pool.size / rt::xmem::kPhysicalPageSize + 1;
    if (g_next_run_slot + slots > end) return 0;
    const uint64_t first = g_next_run_slot;
    g_next_run_slot += slots;
    return first;
}

// A pool-backed range for [base, base + size) of a fresh reservation: false (plain memory follows) when the entry
// pool is off or has no room left for it.
bool make_run(uintptr_t base, uint64_t size) {
    if (!g_entry || !size || (base & (kSmallPage - 1)) || (size & (kSmallPage - 1))) return false;
    const uint64_t slots = (size + kPage64k - 1) / kPage64k;
    const uint64_t first = take_run_slots(slots);
    if (!first) {
        if (!(g_runs_refused++ & 63))
            RTLOG(WARN, "xmem: title pool: no room for a %llu-byte virtual run (GTA6RT_XMEM_POOL_VIRTUAL_GIB); plain memory "
                        "(%llu refused so far)",
                  static_cast<unsigned long long>(size), static_cast<unsigned long long>(g_runs_refused));
        return false;
    }
    VirtualRun run;
    run.size = size;
    run.first_slot = first;
    run.slots = slots;
    run.state.assign(size / kSmallPage, 0);
    g_runs[base] = std::move(run);
    g_run_bytes += slots * kPage64k;
    const uint64_t n = ++g_runs_made;
    if (n <= 4 || !(n & (n - 1)))
        RTLOG(INFO, "xmem: title pool: virtual run %p (%llu bytes) is pool slots %llu..%llu (%llu runs, %llu MiB of pool)",
              reinterpret_cast<void*>(base), static_cast<unsigned long long>(size), static_cast<unsigned long long>(first),
              static_cast<unsigned long long>(first + slots - 1), static_cast<unsigned long long>(n),
              static_cast<unsigned long long>(g_run_bytes >> 20));
    return true;
}

VirtualRun* run_of(uintptr_t address, uintptr_t* base_out = nullptr) {
    if (g_runs.empty()) return nullptr;
    auto it = g_runs.upper_bound(address);
    if (it == g_runs.begin()) return nullptr;
    --it;
    if (address - it->first >= it->second.size) return nullptr;
    if (base_out) *base_out = it->first;
    return &it->second;
}

// Commits [address, address + size) inside a run as views of its slots: pages the title decommitted are zeroed first
// (a new commit reads zeros), committed ones keep their bytes. False when the range is not inside one run.
bool commit_in_run(uintptr_t address, uint64_t size, uint32_t vm_prot) {
    uintptr_t base = 0;
    VirtualRun* run = run_of(address, &base);
    if (!run || address + size > base + run->size) return false;
    const uintptr_t lo = address & ~(kSmallPage - 1);
    const uintptr_t hi = std::min<uintptr_t>((address + size + kSmallPage - 1) & ~(kSmallPage - 1), base + run->size);
    if (lo >= hi) return false;
    const uint64_t t0 = now_ns();
    uint8_t* host = slot_address(run->first_slot) + (lo - base);
    for (uintptr_t p = lo; p < hi; p += kSmallPage) {
        uint8_t& st = run->state[(p - base) / kSmallPage];
        if (st & kDecommittedBit) std::memset(host + (p - lo), 0, kSmallPage);
    }
    if (gta6rt_vm_alias(reinterpret_cast<void*>(lo), host, hi - lo, vm_prot) != GTA6RT_OK) {
        RTLOG(ERROR, "xmem: title pool: view of %p (%llu bytes) on its pool slots failed", reinterpret_cast<void*>(lo),
              static_cast<unsigned long long>(hi - lo));
        return false;
    }
    for (uintptr_t p = lo; p < hi; p += kSmallPage) run->state[(p - base) / kSmallPage] = kViewBit;
    note_added();
    note_view_time(now_ns() - t0);
    return true;
}

// MEM_DECOMMIT inside a run: the views become no-access and stay views (decommitting would map fresh memory there,
// and the title would then write memory the GPU's view of the pool does not see). False: not inside one run.
bool decommit_in_run(uintptr_t address, uint64_t size) {
    uintptr_t base = 0;
    VirtualRun* run = run_of(address, &base);
    if (!run) return false;
    const uintptr_t lo = address & ~(kSmallPage - 1);
    const uintptr_t hi = std::min<uintptr_t>((address + size + kSmallPage - 1) & ~(kSmallPage - 1), base + run->size);
    uintptr_t p = lo;
    while (p < hi) {
        const bool view = run->state[(p - base) / kSmallPage] & kViewBit;
        uintptr_t q = p;
        while (q < hi && bool(run->state[(q - base) / kSmallPage] & kViewBit) == view) q += kSmallPage;
        if (view) {
            uint32_t old = 0;
            gta6rt_vm_protect(reinterpret_cast<void*>(p), q - p, GTA6RT_PROT_NONE, &old);
            for (uintptr_t k = p; k < q; k += kSmallPage) run->state[(k - base) / kSmallPage] |= kDecommittedBit;
        }
        p = q;
    }
    note_removed(lo, hi);
    return true;
}

void release_run(uintptr_t base) {
    auto it = g_runs.find(base);
    if (it == g_runs.end()) return;
    note_removed(base, base + it->second.size);
    g_free_runs.emplace_back(it->second.first_slot, it->second.slots);
    g_run_bytes -= it->second.slots * kPage64k;
    g_runs.erase(it);
}

// Plain decommits over pages of a run (a range that runs past it): those pages are no views any more.
void forget_views(uintptr_t lo, uintptr_t hi) {
    for (auto& [base, run] : g_runs) {
        if (base >= hi || base + run.size <= lo) continue;
        const uintptr_t from = std::max<uintptr_t>(lo, base) & ~(kSmallPage - 1);
        const uintptr_t to = std::min<uintptr_t>(hi, base + run.size);
        for (uintptr_t p = from; p < to; p += kSmallPage) run.state[(p - base) / kSmallPage] = 0;
        note_removed(from, to);
    }
}

void note_plain_commit(uint64_t size) {
    if (!g_entry) return;
    ++g_plain_commits;
    g_plain_commit_bytes += size;
    if (!(g_plain_commits & (g_plain_commits - 1)))
        RTLOG(INFO, "xmem: title pool: %llu commits (%llu MiB) stay plain memory (not pool-backed)",
              static_cast<unsigned long long>(g_plain_commits), static_cast<unsigned long long>(g_plain_commit_bytes >> 20));
}

void hold_low_reservation(uintptr_t base, uint64_t size) {
    if (!ensure_pool()) return;
    const uint64_t slots = (size + kPage64k - 1) / kPage64k;
    if (g_next_slot + slots > kPageSlotLimit || (g_next_slot - 1 + slots) * rt::xmem::kPhysicalPageSize > g_pool.physical) {
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
        release_slot(have, false);
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
    if (g_entry && it != g_mapped.end() && it->first < end) note_removed(start, end);
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
        st = GTA6RT_E_NO_MEMORY;
        if (range->low32) {
            report_low_va();
            if (const uintptr_t at = take_low_va(size, align)) {
                st = gta6rt_vm_reserve(reinterpret_cast<void*>(at), size, flags | GTA6RT_VM_FIXED, &base);
                if (st == GTA6RT_OK)
                    RTLOG(INFO, "xmem: 32-bit reservation of %llu bytes placed at %p, in the low range held since process "
                                "start (%llu MiB of it left)",
                          static_cast<unsigned long long>(size), reinterpret_cast<void*>(at),
                          static_cast<unsigned long long>(g_low_va.size >> 20));
                else
                    RTLOG(WARN, "xmem: 32-bit reservation of %llu bytes could not be placed at %p (status %d) after the "
                                "low range held since process start gave it back; searching below 4 GiB",
                          static_cast<unsigned long long>(size), reinterpret_cast<void*>(at), int(st));
            } else if (g_low_va.tried) {
                RTLOG(WARN, "xmem: 32-bit reservation of %llu bytes: the low range held since process start has %llu MiB "
                            "(LEONIDA_XMEM_LOW_HOLD_MB asked %llu); searching below 4 GiB",
                      static_cast<unsigned long long>(size), static_cast<unsigned long long>(g_low_va.size >> 20),
                      static_cast<unsigned long long>(g_low_va.asked >> 20));
            }
        }
        if (st != GTA6RT_OK) st = gta6rt_vm_reserve_range(range->lowest, range->highest_end, size, align, flags, &base);
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
    if (entry_pool_wanted()) ensure_pool();
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
            if (g_entry && (run_of(start) || (!hold_of_range(start, size) && make_run(start, size))) &&
                commit_in_run(start, size, vm_prot))
                return address;
            note_plain_commit(size);
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
    bool fresh = false;
    if (host_type & (kMemReserve) || (!address && (host_type & kMemCommit))) {
        base = reserve_region(address, size, host_type | kMemReserve, page_mode == kXmemLargePages ? kPage2m : base_granularity,
                              page_mode == kXmemLargePages, &range, attributes);
        if (!base) return nullptr;
        fresh = true;
    }
    const uintptr_t b = reinterpret_cast<uintptr_t>(base);
    if (fresh && g_entry) make_run(b, (size + kSmallPage - 1) & ~(kSmallPage - 1));
    if (host_type & kMemCommit) {
        if (g_entry && commit_in_run(b, size, vm_prot)) return base;
        note_plain_commit(size);
        if (gta6rt_vm_commit(base, size, vm_prot) != GTA6RT_OK) {
            gta6rt_last_error_set(kErrNotEnoughMemory);
            if (host_type & kMemReserve) {
                release_run(b);
                gta6rt_vm_release(base);
            }
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

// Entry pool: which of the title's kernel32 VirtualAllocs become virtual runs: MEM_GRAPHICS ones (GPU memory on the
// console), or every one with GTA6RT_XMEM_POOL_VIRTUALALLOC=all.
bool pool_backs_virtualalloc(uint32_t allocation_type) {
    static const bool all = [] {
        const char* v = gta6rt_env_get("GTA6RT_XMEM_POOL_VIRTUALALLOC");
        return v && std::strcmp(v, "all") == 0;
    }();
    return all || (allocation_type & kMemGraphics);
}

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
    if (entry_pool_wanted()) ensure_pool();
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
    bool fresh = false;
    if ((type & kMemReserve) || (!address && (type & kMemCommit))) {
        void* reserved = reserve_region(address ? base : nullptr, length, type | kMemReserve, large ? kPage2m : kPage64k, large);
        if (!reserved) return nullptr;
        base = reserved;
        fresh = true;
    }
    const uintptr_t b = reinterpret_cast<uintptr_t>(base);
    if (fresh && g_entry && pool_backs_virtualalloc(allocation_type)) make_run(b, (length + kSmallPage - 1) & ~(kSmallPage - 1));
    if (type & kMemCommit) {
        if (g_entry && commit_in_run(b, length, vm_prot)) return address && !(type & kMemReserve) ? address : base;
        note_plain_commit(length);
        if (gta6rt_vm_commit(base, length, vm_prot) != GTA6RT_OK) {
            gta6rt_last_error_set(kErrNotEnoughMemory);
            if ((type & kMemReserve) || !address) {
                release_run(b);
                gta6rt_vm_release(base);
            }
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
        if (g_entry) release_run(a);
        uint64_t extent = 0;
        if (auto ph = g_placeholders.find(a); ph != g_placeholders.end()) {
            extent = ph->second;
            g_placeholders.erase(ph);
        } else if (auto w = g_windows.find(a); w != g_windows.end()) {
            extent = w->second.size;
        }
        if (extent) {
            if (g_entry) note_removed(a, a + extent);
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
        if (g_entry) {
            const std::lock_guard lock(g_mutex);
            const uintptr_t a = reinterpret_cast<uintptr_t>(address);
            uintptr_t run_base = 0;
            if (const VirtualRun* run = run_of(a, &run_base); run && a + size <= run_base + run->size) {
                decommit_in_run(a, size);
                return 1;
            }
            forget_views(a, a + size);
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
    if (g_entry && count > 1) {
        // Consecutive slots, so a window the pages are mapped into in order is one pool stretch: a free run first,
        // else fresh slots while the free list holds at most contiguous_slack_slots() of committed memory.
        uint64_t first = g_free_slots.take_run(count);
        if (!first && g_free_slots.size() <= contiguous_slack_slots() &&
            (g_next_slot - 1 + count) * rt::xmem::kPhysicalPageSize <= g_pool.physical) {
            first = g_next_slot;
            g_next_slot += count;
        }
        const uint64_t n = first ? ++g_contiguous_allocations : ++g_scattered_allocations;
        if (first)
            for (uint64_t i = 0; i < count; ++i) slots.push_back(first + i);
        if (!(n & (n - 1)) && n >= 64)
            RTLOG(INFO, "xmem: title pool: multi-page physical allocations: %llu given consecutive slots, %llu scattered "
                        "(free list %zu slots)",
                  static_cast<unsigned long long>(g_contiguous_allocations),
                  static_cast<unsigned long long>(g_scattered_allocations), g_free_slots.size());
    }
    for (uint64_t i = slots.size(); i < count; ++i) {
        uint64_t slot;
        if (!g_free_slots.empty()) {
            slot = g_free_slots.take_any();
        } else {
            slot = g_next_slot++;
        }
        if ((slot - 1) * rt::xmem::kPhysicalPageSize + rt::xmem::kPhysicalPageSize > g_pool.physical) {
            g_free_slots.push_back(slot);
            for (uint64_t s : slots) g_free_slots.push_back(s);
            *page_count = 0;
            gta6rt_last_error_set(kErrNotEnoughMemory);
            return 0;
        }
        slots.push_back(slot);
    }
    if (g_entry && !g_dirty_free.empty())
        for (uint64_t s : slots) zero_if_dirty(s);
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
    note_added();
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

// At process start, before any thread or framework maps memory: the low range for the title's 32-bit reservation, and
// with the title pool on, GTA6RT_VM_BASE_HINT=0x2000000000 unless set (port/os places "anywhere" title reservations
// there, far above the shader VAs Metal hands the pool's chunks: without it chunks collided with title memory, tpool-b).
struct Registration {
    Registration() {
        rt::xmem::copy_page_mappings_fn = &rt::xmem::copy_page_mappings;
        hold_low_va();
        if (entry_pool_default(std::getenv("GTA6RT_METAL_TITLE_POOL"), std::getenv("GTA6RT_GPU")) &&
            !std::getenv("GTA6RT_VM_BASE_HINT")) {
            setenv("GTA6RT_VM_BASE_HINT", "0x2000000000", 0);
            g_base_hint_defaulted = true;
        }
    }
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

bool title_pool_view(std::uint64_t va, void** host, std::uint64_t* run_bytes) {
    constexpr uint64_t kMaxRun = 256ull << 20;
    const std::lock_guard lock(g_mutex);
    if (!g_entry) return false;
    const uintptr_t page = static_cast<uintptr_t>(va) & ~uintptr_t(kPhysicalPageSize - 1);
    if (auto it = g_mapped.find(page); it != g_mapped.end()) {
        const uint64_t slot = slot_of(it->second);
        if (!slot) return false;
        uint64_t n = 1;
        for (auto nx = std::next(it); nx != g_mapped.end() && n * kPhysicalPageSize < kMaxRun; ++nx, ++n)
            if (nx->first != page + n * kPhysicalPageSize || slot_of(nx->second) != slot + n) break;
        *host = slot_address(slot) + (va - page);
        *run_bytes = n * kPhysicalPageSize - (va - page);
        return true;
    }
    uintptr_t base = 0;
    const VirtualRun* run = run_of(static_cast<uintptr_t>(va), &base);
    if (!run) return false;
    size_t i = (va - base) / kSmallPage;
    if (run->state[i] != kViewBit) return false;
    size_t j = i + 1;
    while (j < run->state.size() && run->state[j] == kViewBit && (j - i) * kSmallPage < kMaxRun) ++j;
    *host = slot_address(run->first_slot) + (va - base);
    *run_bytes = j * kSmallPage - (va - base);
    return true;
}

std::uint64_t title_pool_generation() { return g_generation.load(std::memory_order_acquire); }

std::uint64_t title_pool_removals(std::uint64_t since, std::vector<std::pair<std::uint64_t, std::uint64_t>>& out) {
    const std::lock_guard lock(g_mutex);
    out.clear();
    const uint64_t now = g_generation.load(std::memory_order_acquire);
    if (!g_entry || since >= now) return now;
    if (since < g_removals_dropped) {
        out.emplace_back(0, UINT64_MAX);
        return now;
    }
    for (auto it = g_removals.rbegin(); it != g_removals.rend() && it->first > since; ++it) out.push_back(it->second);
    return now;
}

bool title_pool_range(void** base, std::uint64_t* size) {
    const std::lock_guard lock(g_mutex);
    if (!g_entry) return false;
    *base = g_pool.base;
    *size = g_pool.size;
    return true;
}

TitlePoolStats title_pool_stats() {
    const std::lock_guard lock(g_mutex);
    TitlePoolStats s;
    s.on = g_entry;
    s.runs = g_runs.size();
    s.run_bytes = g_run_bytes;
    s.runs_refused = g_runs_refused;
    s.plain_commits = g_plain_commits;
    s.plain_commit_bytes = g_plain_commit_bytes;
    s.generation = g_generation.load(std::memory_order_relaxed);
    s.removals = g_removals.size();
    s.contiguous_allocations = g_contiguous_allocations;
    s.scattered_allocations = g_scattered_allocations;
    return s;
}

bool title_pool_wanted() { return entry_pool_wanted(); }

LowHoldInfo low_hold_info() {
    const std::lock_guard lock(g_mutex);
    return {reinterpret_cast<void*>(g_low_va.base), g_low_va.size, g_low_va.asked};
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
