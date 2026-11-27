#include <atomic>
#include <mutex>

#include <mach/mach.h>

#include <gta6rt/abi.h>

#include "seh_internal.h"

namespace seh {

namespace {

// Slots are append-only so lock-free readers (fault paths, backtraces of other threads) never see a slot reused.
constexpr uint32_t kMaxTables = 8192;

struct table_entry {
    std::atomic<int> state;  // 0 never used, 1 active, 2 removed
    uint64_t base;
    uint64_t lo, hi;
    const rf_t* table;
    uint32_t count;
    bool sorted;
    bool is_image;
    uint64_t callback;
    uint64_t callback_ctx;
    uint64_t identifier;
};

table_entry g_tables[kMaxTables];
std::atomic<uint32_t> g_used{0};
std::mutex g_mutex;

const rf_t* search(const table_entry& t, uint64_t pc) {
    const uint64_t rva = pc - t.base;
    if (rva >> 32) return nullptr;
    const uint32_t r = static_cast<uint32_t>(rva);
    if (t.sorted) {
        uint32_t lo = 0, hi = t.count;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            const rf_t* f = &t.table[mid];
            if (r < f->begin_rva) hi = mid;
            else if (r >= f->end_rva) lo = mid + 1;
            else return f;
        }
        return nullptr;
    }
    for (uint32_t i = 0; i < t.count; ++i)
        if (r >= t.table[i].begin_rva && r < t.table[i].end_rva) return &t.table[i];
    return nullptr;
}

#if defined(__x86_64__)
using guest_callback = const rf_t*(__attribute__((ms_abi))*)(uint64_t pc, uint64_t ctx);
#endif

const rf_t* resolve_indirect(const rf_t* f, uint64_t base) {
    for (int i = 0; f && (f->unwind_info_rva & 1) && i < 4; ++i)
        f = reinterpret_cast<const rf_t*>(base + (f->unwind_info_rva & ~1u));
    return f;
}

}  // namespace

bool safe_read(uint64_t addr, void* out, size_t n) {
    if (!n) return true;
    if (addr + n < addr) return false;
    vm_size_t got = 0;
    const kern_return_t kr = vm_read_overwrite(mach_task_self(), static_cast<vm_address_t>(addr), n,
                                               reinterpret_cast<vm_address_t>(out), &got);
    return kr == KERN_SUCCESS && got == n;
}

bool table_add(uint64_t base, const rf_t* table, uint32_t count, uint64_t image_size, bool is_image) {
    if (!table || !count) return false;
    std::lock_guard<std::mutex> l(g_mutex);
    const uint32_t i = g_used.load(std::memory_order_relaxed);
    if (i >= kMaxTables) return false;
    table_entry& t = g_tables[i];
    t.base = base;
    t.table = table;
    t.count = count;
    t.is_image = is_image;
    t.callback = t.callback_ctx = t.identifier = 0;
    bool sorted = true;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t k = 0; k < count; ++k) {
        if (table[k].begin_rva < lo) lo = table[k].begin_rva;
        if (table[k].end_rva > hi) hi = table[k].end_rva;
        if (k && table[k].begin_rva < table[k - 1].end_rva) sorted = false;
    }
    t.sorted = sorted;
    if (is_image && image_size) {
        t.lo = base;
        t.hi = base + image_size;
    } else {
        t.lo = base + lo;
        t.hi = base + hi;
    }
    t.state.store(1, std::memory_order_release);
    g_used.store(i + 1, std::memory_order_release);
    return true;
}

bool table_add_callback(uint64_t identifier, uint64_t base, uint32_t length, uint64_t callback, uint64_t context) {
    if (!callback || !length) return false;
    std::lock_guard<std::mutex> l(g_mutex);
    const uint32_t i = g_used.load(std::memory_order_relaxed);
    if (i >= kMaxTables) return false;
    table_entry& t = g_tables[i];
    t.base = base;
    t.lo = base;
    t.hi = base + length;
    t.table = nullptr;
    t.count = 0;
    t.sorted = false;
    t.is_image = false;
    t.callback = callback;
    t.callback_ctx = context;
    t.identifier = identifier;
    t.state.store(1, std::memory_order_release);
    g_used.store(i + 1, std::memory_order_release);
    return true;
}

bool table_remove(const void* key) {
    std::lock_guard<std::mutex> l(g_mutex);
    const uint32_t n = g_used.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
        table_entry& t = g_tables[i];
        if (t.state.load(std::memory_order_relaxed) != 1) continue;
        const bool match = t.callback ? t.identifier == reinterpret_cast<uint64_t>(key) : t.table == key;
        if (match) {
            t.state.store(2, std::memory_order_release);
            return true;
        }
    }
    return false;
}

const rf_t* lookup(uint64_t pc, uint64_t* out_base) {
    const uint32_t n = g_used.load(std::memory_order_acquire);
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < n; ++i) {
            const table_entry& t = g_tables[i];
            if (t.state.load(std::memory_order_acquire) != 1) continue;
            if (t.is_image != (pass == 0)) continue;
            if (pc < t.lo || pc >= t.hi) continue;
            const rf_t* f = nullptr;
            if (t.callback) {
#if defined(__x86_64__)
                f = reinterpret_cast<guest_callback>(t.callback)(pc, t.callback_ctx);
#endif
            } else {
                f = search(t, pc);
            }
            if (f) {
                if (out_base) *out_base = t.base;
                return resolve_indirect(f, t.base);
            }
            if (t.is_image) return nullptr;
        }
    }
    return nullptr;
}

bool pc_in_guest_image(uint64_t pc) {
    const uint32_t n = g_used.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i) {
        const table_entry& t = g_tables[i];
        if (t.state.load(std::memory_order_acquire) == 1 && pc >= t.lo && pc < t.hi) return true;
    }
    return false;
}

uint64_t image_base_of(uint64_t addr) {
    const uint32_t n = g_used.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i) {
        const table_entry& t = g_tables[i];
        if (t.state.load(std::memory_order_acquire) == 1 && t.is_image && addr >= t.lo && addr < t.hi) return t.base;
    }
    return 0;
}

}  // namespace seh
