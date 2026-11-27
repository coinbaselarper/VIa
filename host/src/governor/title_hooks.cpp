#include "title_hooks.h"

#include <gta6rt/abi.h>
#include <gta6rt/os.h>

#include <atomic>
#include <cstdio>

#include "shims.h"

namespace host::governor::hooks {

namespace {

constexpr uint32_t kMemDecommit = 0x4000, kMemRelease = 0x8000, kMemReset = 0x80000, kMemResetUndo = 0x1000000;

Counters g_counters;
std::atomic<bool> g_advise{false};
bool g_installed = false;

void note_reset(void* address, size_t size) {
    g_counters.reset_calls.fetch_add(1, std::memory_order_relaxed);
    g_counters.reset_bytes.fetch_add(size, std::memory_order_relaxed);
    if (!g_advise.load(std::memory_order_relaxed) || !address || !size) return;
    if (gta6rt_vm_reset(address, size) == GTA6RT_OK) {
        g_counters.advised_calls.fetch_add(1, std::memory_order_relaxed);
        g_counters.advised_bytes.fetch_add(size, std::memory_order_relaxed);
    }
}

#if defined(__x86_64__)
using VirtualAllocFn = GTA6RT_GUEST_ABI void* (*)(void*, size_t, uint32_t, uint32_t);
using XMemVirtualAllocFn = GTA6RT_GUEST_ABI void* (*)(void*, size_t, uint32_t, uint64_t, uint32_t);
using VirtualFreeFn = GTA6RT_GUEST_ABI int (*)(void*, size_t, uint32_t);
using XMemFreePhysicalPagesFn = GTA6RT_GUEST_ABI int (*)(uint64_t, const uint64_t*);

VirtualAllocFn g_virtual_alloc = nullptr;
XMemVirtualAllocFn g_xmem_virtual_alloc = nullptr;
VirtualFreeFn g_virtual_free = nullptr;
XMemFreePhysicalPagesFn g_xmem_free_pages = nullptr;

GTA6RT_GUEST_ABI void* wrap_virtual_alloc(void* address, size_t size, uint32_t type, uint32_t protect) {
    g_counters.alloc_calls.fetch_add(1, std::memory_order_relaxed);
    void* r = g_virtual_alloc(address, size, type, protect);
    if (r && (type & kMemReset)) note_reset(address, size);
    if (type & kMemResetUndo) g_counters.reset_undo_calls.fetch_add(1, std::memory_order_relaxed);
    return r;
}

GTA6RT_GUEST_ABI void* wrap_xmem_virtual_alloc(void* address, size_t size, uint32_t type, uint64_t attributes, uint32_t protect) {
    g_counters.alloc_calls.fetch_add(1, std::memory_order_relaxed);
    void* r = g_xmem_virtual_alloc(address, size, type, attributes, protect);
    if (r && (type & kMemReset)) note_reset(address, size);
    if (type & kMemResetUndo) g_counters.reset_undo_calls.fetch_add(1, std::memory_order_relaxed);
    return r;
}

GTA6RT_GUEST_ABI int wrap_virtual_free(void* address, size_t size, uint32_t type) {
    g_counters.free_calls.fetch_add(1, std::memory_order_relaxed);
    const int r = g_virtual_free(address, size, type);
    if (r && (type & kMemDecommit) && !(type & kMemRelease)) {
        g_counters.decommit_calls.fetch_add(1, std::memory_order_relaxed);
        g_counters.decommit_bytes.fetch_add(size, std::memory_order_relaxed);
    } else if (r && (type & kMemRelease)) {
        g_counters.release_calls.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

GTA6RT_GUEST_ABI int wrap_xmem_free_pages(uint64_t count, const uint64_t* pages) {
    const int r = g_xmem_free_pages(count, pages);
    if (r) {
        g_counters.freed_page_calls.fetch_add(1, std::memory_order_relaxed);
        g_counters.freed_pages.fetch_add(count, std::memory_order_relaxed);
    }
    return r;
}

template <class F>
bool wrap(const char* dll, const char* name, F* original, void* replacement) {
    const rt::shims::Found found = rt::shims::find_entry(dll, name);
    if (!found.fn) {
        std::fprintf(stderr, "[gov] title hook %s!%s: no native entry to wrap; not counted\n", dll, name);
        return false;
    }
    *original = reinterpret_cast<F>(found.fn);
    rt::shims::Found f;
    f.fn = replacement;
    rt::shims::register_entry(dll, name, f);
    return true;
}
#endif

}  // namespace

const Counters& counters() { return g_counters; }
void set_reset_advice(bool on) { g_advise.store(on, std::memory_order_relaxed); }
bool reset_advice() { return g_advise.load(std::memory_order_relaxed); }
bool installed() { return g_installed; }

unsigned install() {
#if defined(__x86_64__)
    unsigned n = 0;
    n += wrap("kernel32.dll", "VirtualAlloc", &g_virtual_alloc, reinterpret_cast<void*>(&wrap_virtual_alloc));
    n += wrap("xmem.dll", "XMemVirtualAlloc", &g_xmem_virtual_alloc, reinterpret_cast<void*>(&wrap_xmem_virtual_alloc));
    n += wrap("kernel32.dll", "VirtualFree", &g_virtual_free, reinterpret_cast<void*>(&wrap_virtual_free));
    n += wrap("xmem.dll", "XMemFreePhysicalPages", &g_xmem_free_pages, reinterpret_cast<void*>(&wrap_xmem_free_pages));
    g_installed = n > 0;
    return n;
#else
    std::fprintf(stderr, "[gov] title hooks: not on arm64 (frame adapters); MEM_RESET/MEM_DECOMMIT not counted\n");
    return 0;
#endif
}

}  // namespace host::governor::hooks
