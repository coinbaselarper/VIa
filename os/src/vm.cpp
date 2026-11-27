#include "internal.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <pthread.h>

using namespace gta6rt_os;

namespace {

constexpr size_t kPage = GTA6RT_GUEST_PAGE_SIZE;
constexpr size_t kGran = GTA6RT_GUEST_ALLOCATION_GRANULARITY;

enum : uint8_t { ST_RESERVED = 1, ST_COMMITTED = 2 };

struct Attr {
    uint8_t state;
    uint8_t prot;
    bool operator==(const Attr& o) const { return state == o.state && prot == o.prot; }
};

enum class RType : uint8_t { Private, Mapped, Image };

struct Region {
    uintptr_t base;
    size_t size;      // logical, multiple of 4 KiB
    size_t host_size; // what is mapped, multiple of the host page
    uint32_t alloc_prot;
    uint32_t flags;
    RType type;
    bool view_from_placeholder = false;
    std::map<size_t, Attr> runs; // page index -> attr of the run starting there
    std::vector<uint8_t> dirty;  // write watch, per guest page

    size_t npages() const { return size / kPage; }
    bool ww() const { return (flags & GTA6RT_VM_WRITE_WATCH) != 0; }

    std::map<size_t, Attr>::iterator run_at(size_t i) {
        auto it = runs.upper_bound(i);
        return --it;
    }
    Attr get(size_t i) { return run_at(i)->second; }
    void split(size_t i) {
        if (i >= npages() || runs.count(i)) return;
        Attr a = get(i);
        runs[i] = a;
    }
    void set(size_t lo, size_t hi, Attr a) {
        if (lo >= hi) return;
        split(lo);
        split(hi);
        auto it = runs.find(lo);
        while (it != runs.end() && it->first < hi) it = runs.erase(it);
        runs[lo] = a;
        auto cur = runs.find(lo);
        if (cur != runs.begin()) {
            auto prev = std::prev(cur);
            if (prev->second == a) {
                runs.erase(cur);
                cur = prev;
            }
        }
        auto next = std::next(cur);
        if (next != runs.end() && next->second == a) runs.erase(next);
    }
    // Calls fn(lo, hi, attr) for each run overlapping [lo, hi).
    template <class F> void each(size_t lo, size_t hi, F fn) {
        auto it = run_at(lo);
        while (it != runs.end() && it->first < hi) {
            auto nx = std::next(it);
            size_t rlo = it->first > lo ? it->first : lo;
            size_t rhi = nx == runs.end() ? npages() : nx->first;
            if (rhi > hi) rhi = hi;
            fn(rlo, rhi, it->second);
            it = nx;
        }
    }
};

size_t g_host_page;
pthread_mutex_t g_vm_mx = PTHREAD_MUTEX_INITIALIZER;
std::map<uintptr_t, Region*>* g_regions;
thread_local int t_in_vm;

struct VmLock {
    VmLock() {
        if (!g_host_page) g_host_page = (size_t)getpagesize();
        pthread_mutex_lock(&g_vm_mx);
        t_in_vm++;
        if (!g_regions) g_regions = new std::map<uintptr_t, Region*>();
    }
    ~VmLock() {
        t_in_vm--;
        pthread_mutex_unlock(&g_vm_mx);
    }
};

size_t round_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }
uintptr_t round_down(uintptr_t v, size_t a) { return v & ~(uintptr_t)(a - 1); }

Region* find_region(uintptr_t a) {
    auto it = g_regions->upper_bound(a);
    if (it == g_regions->begin()) return nullptr;
    --it;
    Region* r = it->second;
    return a < r->base + r->size ? r : nullptr;
}

int page_host_prot(const Region* r, Attr a, size_t page) {
    if (a.state != ST_COMMITTED || (a.prot & GTA6RT_PROT_GUARD)) return PROT_NONE;
    int p = 0;
    if (a.prot & GTA6RT_PROT_READ) p |= PROT_READ;
    if (a.prot & GTA6RT_PROT_WRITE) p |= PROT_READ | PROT_WRITE;
#if defined(__x86_64__)
    if (a.prot & GTA6RT_PROT_EXEC) p |= PROT_READ | PROT_EXEC;
#else
    if (a.prot & GTA6RT_PROT_EXEC) p |= PROT_READ; // guest code runs through the JIT, never natively
#endif
    if (r->ww() && (p & PROT_WRITE) && !r->dirty[page]) p &= ~PROT_WRITE;
    return p;
}

// Recompute and apply host protection for every host page overlapping guest pages [lo, hi).
void apply_host_prot(Region* r, size_t lo, size_t hi) {
    if (lo >= hi) return;
    const size_t sub = g_host_page / kPage;
    size_t hlo = (lo * kPage) / g_host_page;
    size_t hhi = round_up(hi * kPage, g_host_page) / g_host_page;
    size_t cur_start = hlo;
    int cur_prot = -1;
    auto flush = [&](size_t end) {
        if (cur_prot >= 0 && end > cur_start)
            if (mprotect((void*)(r->base + cur_start * g_host_page), (end - cur_start) * g_host_page, cur_prot) != 0)
                OS_LOG(GTA6RT_LOG_ERROR, "mprotect %#lx+%#zx prot %d failed: %s",
                       (unsigned long)(r->base + cur_start * g_host_page), (end - cur_start) * g_host_page, cur_prot,
                       strerror(errno));
    };
    size_t h = hlo;
    while (h < hhi) {
        size_t p0 = h * sub;
        int prot = 0;
        size_t span = 1;
        if (p0 >= r->npages()) {
            prot = PROT_NONE;
        } else if (sub == 1 && !r->ww()) {
            auto it = r->run_at(p0);
            auto nx = std::next(it);
            size_t rend = nx == r->runs.end() ? r->npages() : nx->first;
            if (rend > hhi) rend = hhi;
            span = rend - h;
            prot = page_host_prot(r, it->second, p0);
        } else {
            for (size_t k = 0; k < sub && p0 + k < r->npages(); k++) prot |= page_host_prot(r, r->get(p0 + k), p0 + k);
        }
        if (prot != cur_prot) {
            flush(h);
            cur_start = h;
            cur_prot = prot;
        }
        h += span;
    }
    flush(hhi);
}

bool map_fresh(uintptr_t a, size_t n) {
    void* p = mmap((void*)a, n, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED | MAP_NORESERVE, -1, 0);
    return p != MAP_FAILED;
}

uintptr_t g_anywhere_hint;

uintptr_t anywhere_hint() {
    if (!g_anywhere_hint) {
        g_anywhere_hint = 0x300000000ull;
        if (const char* e = getenv("GTA6RT_VM_BASE_HINT")) g_anywhere_hint = strtoull(e, nullptr, 0);
    }
    return g_anywhere_hint;
}

gta6rt_status reserve_raw(uintptr_t addr, size_t host_size, size_t align, bool fixed, uintptr_t* out) {
    mach_vm_address_t a = addr;
    kern_return_t kr;
    if (fixed) {
        kr = mach_vm_map(mach_task_self(), &a, host_size, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE,
                         VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_COPY);
        if (kr == KERN_NO_SPACE || kr == KERN_INVALID_ADDRESS) return GTA6RT_E_ADDRESS_IN_USE;
    } else {
        a = addr ? addr : anywhere_hint();
        mach_vm_offset_t mask = align > g_host_page ? align - 1 : 0;
        kr = mach_vm_map(mach_task_self(), &a, host_size, mask, VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0, FALSE,
                         VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_COPY);
        if (kr != KERN_SUCCESS) {
            a = 0;
            kr = mach_vm_map(mach_task_self(), &a, host_size, mask, VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0, FALSE,
                             VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_COPY);
        }
    }
    if (kr != KERN_SUCCESS) return GTA6RT_E_NO_MEMORY;
    *out = (uintptr_t)a;
    return GTA6RT_OK;
}

Region* new_region(uintptr_t base, size_t size, size_t host_size, uint32_t flags, uint32_t alloc_prot, RType type,
                   Attr init) {
    Region* r = new Region();
    r->base = base;
    r->size = size;
    r->host_size = host_size;
    r->flags = flags;
    r->alloc_prot = alloc_prot;
    r->type = type;
    r->runs[0] = init;
    if (r->ww()) r->dirty.assign(r->npages(), 0);
    (*g_regions)[base] = r;
    return r;
}

// Validates that [a, a+size) lies in one region; returns region and page range.
Region* range_region(void* addr, size_t size, size_t* lo, size_t* hi) {
    uintptr_t a = round_down((uintptr_t)addr, kPage);
    uintptr_t e = round_up((uintptr_t)addr + (size ? size : 1), kPage);
    Region* r = find_region(a);
    if (!r || e > r->base + r->size) return nullptr;
    *lo = (a - r->base) / kPage;
    *hi = (e - r->base) / kPage;
    return r;
}

struct sigaction g_old_segv, g_old_bus;
std::atomic<bool> g_handler_installed{false};

void chain(int sig, siginfo_t* si, void* uc) {
    struct sigaction* old = sig == SIGBUS ? &g_old_bus : &g_old_segv;
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) {
            old->sa_sigaction(sig, si, uc);
            return;
        }
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
}

void fault_handler(int sig, siginfo_t* si, void* uc) {
    gta6rt_vm_fault_result r = gta6rt_vm_handle_fault(si->si_addr, -1);
    if (r == GTA6RT_VM_FAULT_RETRY) return;
    if (r == GTA6RT_VM_FAULT_GUARD_PAGE) {
        gta6rt_log_signal_safe("[W os] guard page hit by host code; guard cleared\n");
        return;
    }
    chain(sig, si, uc);
}


void* memory_reporter(void* arg) {
    const unsigned secs = (unsigned)(uintptr_t)arg;
    for (;;) {
        sleep(secs);
        gta6rt_os_memory_report("periodic", 64ull << 20, 12);
    }
    return nullptr;
}

void start_memory_reporter() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    const char* e = getenv("GTA6RT_MEMORY_REPORT_S");
    const unsigned long secs = e ? strtoul(e, nullptr, 10) : 0;
    if (!secs || secs > 3600) return;
    if (const char* o = getenv("GTA6RT_MEMORY_REPORT_OWNER"); o && !strcmp(o, "host")) return;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &at, memory_reporter, (void*)(uintptr_t)secs);
    pthread_attr_destroy(&at);
}
} // namespace

namespace gta6rt_os {

size_t host_page() {
    if (!g_host_page) g_host_page = (size_t)getpagesize();
    return g_host_page;
}

uint32_t win32_to_prot(DWORD protect, bool* ok) {
    *ok = true;
    uint32_t g = (protect & PAGE_GUARD) ? GTA6RT_PROT_GUARD : 0;
    switch (protect & 0xFF) {
    case PAGE_NOACCESS: return GTA6RT_PROT_NONE | g;
    case PAGE_READONLY: return GTA6RT_PROT_READ | g;
    case PAGE_READWRITE:
    case PAGE_WRITECOPY: return GTA6RT_PROT_READ | GTA6RT_PROT_WRITE | g;
    case PAGE_EXECUTE: return GTA6RT_PROT_EXEC | g;
    case PAGE_EXECUTE_READ: return GTA6RT_PROT_READ | GTA6RT_PROT_EXEC | g;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return GTA6RT_PROT_READ | GTA6RT_PROT_WRITE | GTA6RT_PROT_EXEC | g;
    default: *ok = false; return GTA6RT_PROT_NONE;
    }
}

DWORD prot_to_win32(uint32_t p) {
    DWORD g = (p & GTA6RT_PROT_GUARD) ? PAGE_GUARD : 0;
    p &= GTA6RT_PROT_READ | GTA6RT_PROT_WRITE | GTA6RT_PROT_EXEC;
    switch (p) {
    case 0: return PAGE_NOACCESS | g;
    case GTA6RT_PROT_READ: return PAGE_READONLY | g;
    case GTA6RT_PROT_WRITE:
    case GTA6RT_PROT_READ | GTA6RT_PROT_WRITE: return PAGE_READWRITE | g;
    case GTA6RT_PROT_EXEC: return PAGE_EXECUTE | g;
    case GTA6RT_PROT_READ | GTA6RT_PROT_EXEC: return PAGE_EXECUTE_READ | g;
    default: return PAGE_EXECUTE_READWRITE | g;
    }
}

void vm_register_view(void* base, size_t size, uint32_t prot) {
    VmLock lk;
    host_page();
    size_t lsize = round_up(size, kPage);
    bool from_ph = false;
    auto it = g_regions->find((uintptr_t)base);
    if (it != g_regions->end()) {
        from_ph = (it->second->flags & GTA6RT_VM_PLACEHOLDER) != 0;
        delete it->second;
        g_regions->erase(it);
    }
    Region* r = new_region((uintptr_t)base, lsize, round_up(size, g_host_page), 0, prot, RType::Mapped,
                           Attr{ST_COMMITTED, (uint8_t)prot});
    r->view_from_placeholder = from_ph;
}

bool vm_unregister_view(void* base, size_t* out_size) {
    VmLock lk;
    auto it = g_regions->find((uintptr_t)base);
    if (it == g_regions->end() || it->second->type != RType::Mapped) return false;
    Region* r = it->second;
    *out_size = r->host_size;
    g_regions->erase(it);
    delete r;
    return true;
}

int vm_region_type(const void* addr) {
    VmLock lk;
    Region* r = find_region((uintptr_t)addr);
    if (!r) return 0;
    return r->type == RType::Private ? 1 : r->type == RType::Mapped ? 2 : 3;
}

bool vm_is_placeholder(void* base, size_t size) {
    VmLock lk;
    auto it = g_regions->find((uintptr_t)base);
    return it != g_regions->end() && (it->second->flags & GTA6RT_VM_PLACEHOLDER) && it->second->size == size;
}

} // namespace gta6rt_os

extern "C" {

size_t gta6rt_vm_host_page_size(void) { return host_page(); }

gta6rt_status gta6rt_vm_reserve(void* addr, size_t size, uint32_t flags, void** out_base) {
    if (!size || !out_base) return GTA6RT_E_INVALID_ARG;
    host_page();
    size_t align = kGran;
    if (flags & GTA6RT_VM_LARGE_PAGES) align = 2u << 20;
    if (align < g_host_page) align = g_host_page;
    uintptr_t a = (uintptr_t)addr;
    if (a) a = round_down(a, kGran);
    size_t lsize = round_up(size + ((uintptr_t)addr - a), kPage);
    size_t hsize = round_up(lsize, g_host_page);
    VmLock lk;
    uintptr_t base = 0;
    gta6rt_status st;
    if (a && (flags & GTA6RT_VM_FIXED)) {
        st = reserve_raw(a, hsize, align, true, &base);
    } else if (a) {
        st = reserve_raw(a, hsize, align, true, &base);
        if (st != GTA6RT_OK) st = reserve_raw(0, hsize, align, false, &base);
    } else {
        st = reserve_raw(0, hsize, align, false, &base);
    }
    if (st != GTA6RT_OK) return st;
    RType type = (flags & GTA6RT_VM_IMAGE) ? RType::Image : RType::Private;
    new_region(base, lsize, hsize, flags & ~GTA6RT_VM_FIXED, GTA6RT_PROT_NONE, type, Attr{ST_RESERVED, 0});
    start_memory_reporter();
    *out_base = (void*)base;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_commit(void* addr, size_t size, uint32_t prot) {
    if (!size) return GTA6RT_E_INVALID_ARG;
    VmLock lk;
    size_t lo, hi;
    Region* r = range_region(addr, size, &lo, &hi);
    if (!r) return GTA6RT_E_NOT_FOUND;
    if (r->flags & GTA6RT_VM_PLACEHOLDER) return GTA6RT_E_ACCESS;
    if (r->type == RType::Mapped) return GTA6RT_E_ACCESS;
    r->set(lo, hi, Attr{ST_COMMITTED, (uint8_t)prot});
    if (r->alloc_prot == GTA6RT_PROT_NONE && lo == 0) r->alloc_prot = prot;
    apply_host_prot(r, lo, hi);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_decommit(void* addr, size_t size) {
    VmLock lk;
    size_t lo, hi;
    Region* r = range_region(addr, size, &lo, &hi);
    if (!r) return GTA6RT_E_NOT_FOUND;
    if (r->type == RType::Mapped || (r->flags & GTA6RT_VM_PLACEHOLDER)) return GTA6RT_E_ACCESS;
    r->set(lo, hi, Attr{ST_RESERVED, 0});
    if (r->ww())
        for (size_t i = lo; i < hi; i++) r->dirty[i] = 0;
    const size_t sub = g_host_page / kPage;
    size_t hlo = (lo * kPage) / g_host_page, hhi = round_up(hi * kPage, g_host_page) / g_host_page;
    auto host_page_live = [&](size_t h) {
        for (size_t k = 0; k < sub; k++) {
            size_t p = h * sub + k;
            if (p < r->npages() && r->get(p).state == ST_COMMITTED) return true;
        }
        return false;
    };
    auto zero_part = [&](size_t h) {
        uintptr_t hp = r->base + h * g_host_page;
        mprotect((void*)hp, g_host_page, PROT_READ | PROT_WRITE);
        for (size_t k = 0; k < sub; k++) {
            size_t p = h * sub + k;
            if (p >= lo && p < hi) memset((void*)(r->base + p * kPage), 0, kPage);
        }
    };
    size_t ilo = hlo, ihi = hhi;
    if (sub > 1) {
        if (host_page_live(hlo)) {
            zero_part(hlo);
            ilo = hlo + 1;
        }
        if (hhi - 1 >= ilo && host_page_live(hhi - 1)) {
            zero_part(hhi - 1);
            ihi = hhi - 1;
        }
    }
    if (ihi > ilo) map_fresh(r->base + ilo * g_host_page, (ihi - ilo) * g_host_page);
    apply_host_prot(r, hlo * sub, hhi * sub);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_protect(void* addr, size_t size, uint32_t prot, uint32_t* out_old_prot) {
    if (!size) return GTA6RT_E_INVALID_ARG;
    VmLock lk;
    size_t lo, hi;
    Region* r = range_region(addr, size, &lo, &hi);
    if (!r) {
        uintptr_t a = round_down((uintptr_t)addr, host_page());
        uintptr_t e = round_up((uintptr_t)addr + size, g_host_page);
        int p = 0;
        if (prot & GTA6RT_PROT_READ) p |= PROT_READ;
        if (prot & GTA6RT_PROT_WRITE) p |= PROT_READ | PROT_WRITE;
        if (prot & GTA6RT_PROT_EXEC) p |= PROT_READ | PROT_EXEC;
        if (out_old_prot) {
            mach_vm_address_t ra = a;
            mach_vm_size_t rs = 0;
            vm_region_basic_info_data_64_t info;
            mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj;
            *out_old_prot = GTA6RT_PROT_READ | GTA6RT_PROT_WRITE;
            if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt,
                               &obj) == KERN_SUCCESS &&
                ra <= a) {
                uint32_t o = 0;
                if (info.protection & VM_PROT_READ) o |= GTA6RT_PROT_READ;
                if (info.protection & VM_PROT_WRITE) o |= GTA6RT_PROT_WRITE;
                if (info.protection & VM_PROT_EXECUTE) o |= GTA6RT_PROT_EXEC;
                *out_old_prot = o;
            }
        }
        return mprotect((void*)a, e - a, p) == 0 ? GTA6RT_OK : errno_to_status(errno);
    }
    bool all_committed = true;
    r->each(lo, hi, [&](size_t, size_t, Attr a) {
        if (a.state != ST_COMMITTED) all_committed = false;
    });
    if (!all_committed) return GTA6RT_E_NOT_FOUND;
    if (out_old_prot) *out_old_prot = r->get(lo).prot;
    r->set(lo, hi, Attr{ST_COMMITTED, (uint8_t)prot});
    apply_host_prot(r, lo, hi);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_release(void* allocation_base) {
    VmLock lk;
    auto it = g_regions->find((uintptr_t)allocation_base);
    if (it == g_regions->end()) return GTA6RT_E_NOT_FOUND;
    Region* r = it->second;
    if (r->type == RType::Mapped) return GTA6RT_E_INVALID_ARG;
    munmap((void*)r->base, r->host_size);
    g_regions->erase(it);
    delete r;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_query(const void* addr, gta6rt_vm_region* out) {
    if (!out) return GTA6RT_E_INVALID_ARG;
    memset(out, 0, sizeof *out);
    uintptr_t page = round_down((uintptr_t)addr, kPage);
    {
        VmLock lk;
        Region* r = find_region(page);
        if (r) {
            size_t i = (page - r->base) / kPage;
            auto it = r->run_at(i);
            auto nx = std::next(it);
            size_t end = nx == r->runs.end() ? r->npages() : nx->first;
            out->base = (void*)page;
            out->allocation_base = (void*)r->base;
            out->size = (end - i) * kPage;
            out->allocation_size = r->size;
            out->state = it->second.state == ST_COMMITTED ? GTA6RT_VM_STATE_COMMITTED : GTA6RT_VM_STATE_RESERVED;
            out->prot = it->second.state == ST_COMMITTED ? it->second.prot : 0;
            out->allocation_prot = r->alloc_prot;
            out->is_image = r->type == RType::Image;
            return GTA6RT_OK;
        }
        uintptr_t next = UINT64_MAX;
        auto nit = g_regions->upper_bound(page);
        if (nit != g_regions->end()) next = nit->first;
        mach_vm_address_t ra = page;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        kern_return_t kr = mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &cnt, &obj);
        out->base = (void*)page;
        if (kr == KERN_SUCCESS && ra <= page) {
            uintptr_t rend = ra + rs;
            if (rend > next) rend = next;
            out->allocation_base = (void*)(uintptr_t)ra;
            out->size = rend - page;
            out->allocation_size = rs;
            out->state = GTA6RT_VM_STATE_COMMITTED;
            uint32_t p = 0;
            if (info.protection & VM_PROT_READ) p |= GTA6RT_PROT_READ;
            if (info.protection & VM_PROT_WRITE) p |= GTA6RT_PROT_WRITE;
            if (info.protection & VM_PROT_EXECUTE) p |= GTA6RT_PROT_EXEC;
            out->prot = p;
            out->allocation_prot = p;
            return GTA6RT_OK;
        }
        uintptr_t fend = kr == KERN_SUCCESS ? (uintptr_t)ra : (uintptr_t)0x800000000000ull;
        if (fend > next) fend = next;
        if (fend <= page) fend = page + kPage;
        out->state = GTA6RT_VM_STATE_FREE;
        out->size = fend - page;
        return page >= 0x800000000000ull ? GTA6RT_E_INVALID_ARG : GTA6RT_OK;
    }
}

gta6rt_status gta6rt_vm_alias(void* dst, const void* src, size_t size, uint32_t prot) {
    if (!size || ((uintptr_t)dst | (uintptr_t)src | size) & (host_page() - 1)) return GTA6RT_E_INVALID_ARG;
    VmLock lk;
    mach_vm_address_t d = (uintptr_t)dst;
    vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &d, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                     mach_task_self(), (mach_vm_address_t)(uintptr_t)src, FALSE, &cur, &max,
                                     VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        OS_LOG(GTA6RT_LOG_ERROR, "mach_vm_remap %p <- %p size %#zx failed: %d", dst, src, size, kr);
        return GTA6RT_E_NO_MEMORY;
    }
    size_t lo, hi;
    Region* r = range_region(dst, size, &lo, &hi);
    if (r) {
        r->set(lo, hi, Attr{ST_COMMITTED, (uint8_t)prot});
        apply_host_prot(r, lo, hi);
    } else {
        int p = 0;
        if (prot & GTA6RT_PROT_READ) p |= PROT_READ;
        if (prot & GTA6RT_PROT_WRITE) p |= PROT_READ | PROT_WRITE;
        mprotect(dst, size, p);
    }
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_get_write_watch(void* base, size_t size, int reset, void** addrs, size_t* inout_count) {
    if (!inout_count) return GTA6RT_E_INVALID_ARG;
    VmLock lk;
    size_t lo, hi;
    Region* r = range_region(base, size, &lo, &hi);
    if (!r || !r->ww()) return GTA6RT_E_INVALID_ARG;
    size_t cap = addrs ? *inout_count : 0, n = 0;
    size_t reset_hi = hi;
    for (size_t i = lo; i < hi; i++) {
        if (!r->dirty[i]) continue;
        if (n == cap && addrs) {
            reset_hi = i;
            break;
        }
        if (addrs) addrs[n] = (void*)(r->base + i * kPage);
        n++;
    }
    *inout_count = addrs ? n : 0;
    if (reset) {
        for (size_t i = lo; i < reset_hi; i++) r->dirty[i] = 0;
        apply_host_prot(r, lo, reset_hi);
    }
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_reset(void* addr, size_t size) {
    VmLock lk;
    size_t lo, hi;
    Region* r = range_region(addr, size, &lo, &hi);
    if (!r) return GTA6RT_E_NOT_FOUND;
    uintptr_t a = round_up(r->base + lo * kPage, g_host_page);
    uintptr_t e = round_down(r->base + hi * kPage, g_host_page);
    if (e > a) madvise((void*)a, e - a, MADV_FREE);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_placeholder_split(void* addr, size_t size) {
    VmLock lk;
    uintptr_t a = (uintptr_t)addr;
    if (!size || (a | size) & (kGran - 1)) return GTA6RT_E_INVALID_ARG;
    Region* r = find_region(a);
    if (!r || !(r->flags & GTA6RT_VM_PLACEHOLDER) || a + size > r->base + r->size) return GTA6RT_E_INVALID_ARG;
    if (a == r->base && size == r->size) return GTA6RT_OK;
    uintptr_t rb = r->base, re = r->base + r->size;
    uint32_t flags = r->flags;
    g_regions->erase(rb);
    delete r;
    auto mk = [&](uintptr_t b, uintptr_t e) {
        if (e > b) new_region(b, e - b, e - b, flags, 0, RType::Private, Attr{ST_RESERVED, 0});
    };
    mk(rb, a);
    mk(a, a + size);
    mk(a + size, re);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_placeholder_coalesce(void* addr, size_t size) {
    VmLock lk;
    uintptr_t a = (uintptr_t)addr, e = a + size;
    auto it = g_regions->find(a);
    if (it == g_regions->end()) return GTA6RT_E_INVALID_ARG;
    uintptr_t cur = a;
    std::vector<uintptr_t> parts;
    uint32_t flags = it->second->flags;
    while (cur < e) {
        auto jt = g_regions->find(cur);
        if (jt == g_regions->end() || !(jt->second->flags & GTA6RT_VM_PLACEHOLDER)) return GTA6RT_E_INVALID_ARG;
        parts.push_back(cur);
        cur += jt->second->size;
    }
    if (cur != e) return GTA6RT_E_INVALID_ARG;
    for (uintptr_t p : parts) {
        delete (*g_regions)[p];
        g_regions->erase(p);
    }
    new_region(a, size, size, flags, 0, RType::Private, Attr{ST_RESERVED, 0});
    return GTA6RT_OK;
}

gta6rt_status gta6rt_vm_placeholder_replace(void* addr, size_t size, uint32_t flags) {
    VmLock lk;
    auto it = g_regions->find((uintptr_t)addr);
    if (it == g_regions->end()) return GTA6RT_E_INVALID_ARG;
    Region* r = it->second;
    if (!(r->flags & GTA6RT_VM_PLACEHOLDER) || r->size != round_up(size, kPage)) return GTA6RT_E_INVALID_ARG;
    r->flags = (flags & ~(GTA6RT_VM_PLACEHOLDER | GTA6RT_VM_FIXED));
    if (r->ww()) r->dirty.assign(r->npages(), 0);
    return GTA6RT_OK;
}

gta6rt_vm_fault_result gta6rt_vm_handle_fault(const void* addr, int is_write) {
    (void)is_write;
    if (t_in_vm || !g_regions) return GTA6RT_VM_FAULT_NOT_MINE;
    VmLock lk;
    uintptr_t a = (uintptr_t)addr;
    Region* r = find_region(a);
    if (!r) return GTA6RT_VM_FAULT_NOT_MINE;
    size_t i = (a - r->base) / kPage;
    Attr at = r->get(i);
    if (at.state != ST_COMMITTED) return GTA6RT_VM_FAULT_NOT_MINE;
    const size_t sub = g_host_page / kPage;
    size_t h0 = (i / sub) * sub;
    if (at.prot & GTA6RT_PROT_GUARD) {
        r->set(i, i + 1, Attr{ST_COMMITTED, (uint8_t)(at.prot & ~GTA6RT_PROT_GUARD)});
        apply_host_prot(r, h0, h0 + sub);
        return GTA6RT_VM_FAULT_GUARD_PAGE;
    }
    if (r->ww() && (at.prot & GTA6RT_PROT_WRITE) && !r->dirty[i]) {
        for (size_t k = h0; k < h0 + sub && k < r->npages(); k++)
            if (r->get(k).state == ST_COMMITTED) r->dirty[k] = 1;
        apply_host_prot(r, h0, h0 + sub);
        return GTA6RT_VM_FAULT_RETRY;
    }
    return GTA6RT_VM_FAULT_NOT_MINE;
}

gta6rt_status gta6rt_os_install_fault_handler(void) {
    bool expected = false;
    if (!g_handler_installed.compare_exchange_strong(expected, true)) return GTA6RT_OK;
    host_page();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
    return GTA6RT_OK;
}


gta6rt_status gta6rt_vm_reserve_range(uint64_t lowest, uint64_t highest_end, size_t size, size_t alignment, uint32_t flags,
                                      void** out_base) {
    if (!size || !out_base || (alignment & (alignment - 1))) return GTA6RT_E_INVALID_ARG;
    host_page();
    size_t align = alignment ? alignment : kGran;
    if (flags & GTA6RT_VM_LARGE_PAGES) align = align < (2u << 20) ? (2u << 20) : align;
    if (align < kGran) align = kGran;
    if (align < g_host_page) align = g_host_page;
    const size_t lsize = round_up(size, kPage), hsize = round_up(lsize, g_host_page);
    const uint64_t start = round_up(lowest ? lowest : anywhere_hint(), align);
    if (highest_end <= start || highest_end - start < hsize) return GTA6RT_E_NO_MEMORY;
    VmLock lk;
    mach_vm_address_t a = start;
    if (mach_vm_map(mach_task_self(), &a, hsize, align - 1, VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE,
                    VM_PROT_ALL, VM_INHERIT_COPY) != KERN_SUCCESS)
        return GTA6RT_E_NO_MEMORY;
    if (a < start || a + hsize > highest_end) {
        mach_vm_deallocate(mach_task_self(), a, hsize);
        return GTA6RT_E_NO_MEMORY;
    }
    RType type = (flags & GTA6RT_VM_IMAGE) ? RType::Image : RType::Private;
    new_region((uintptr_t)a, lsize, hsize, flags & ~GTA6RT_VM_FIXED, GTA6RT_PROT_NONE, type, Attr{ST_RESERVED, 0});
    start_memory_reporter();
    *out_base = (void*)a;
    return GTA6RT_OK;
}

uint64_t gta6rt_vm_lowest_mappable(void) {
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; ++i) {
        const auto* h = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
        if (!h || h->filetype != MH_EXECUTE) continue;
        const auto* lc = reinterpret_cast<const load_command*>(h + 1);
        for (uint32_t c = 0; c < h->ncmds; ++c, lc = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(lc) + lc->cmdsize)) {
            if (lc->cmd != LC_SEGMENT_64) continue;
            const auto* seg = reinterpret_cast<const segment_command_64*>(lc);
            if (strcmp(seg->segname, "__PAGEZERO") == 0) return seg->vmaddr + seg->vmsize;
        }
        return 0;
    }
    return 0;
}

gta6rt_status gta6rt_os_memory_stats(gta6rt_memory_stats* out) {
    if (!out) return GTA6RT_E_INVALID_ARG;
    memset(out, 0, sizeof *out);
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS) return GTA6RT_E_IO;
    out->resident = vi.resident_size;
    out->footprint = vi.phys_footprint;
    out->compressed = vi.compressed;
    out->internal = vi.internal;
    out->virtual_size = vi.virtual_size;
    VmLock lk;
    for (auto& kv : *g_regions) {
        Region* r = kv.second;
        out->regions++;
        out->reserved += r->size;
        r->each(0, r->npages(), [&](size_t lo, size_t hi, Attr a) {
            if (a.state == ST_COMMITTED) out->committed += (hi - lo) * kPage;
        });
    }
    return GTA6RT_OK;
}

gta6rt_status gta6rt_os_system_memory(gta6rt_system_memory* out) {
    if (!out) return GTA6RT_E_INVALID_ARG;
    memset(out, 0, sizeof *out);
    size_t n = sizeof out->physical;
    sysctlbyname("hw.memsize", &out->physical, &n, nullptr, 0);
    vm_size_t ps = 0;
    host_page_size(mach_host_self(), &ps);
    out->page_size = ps;
    vm_statistics64_data_t vs;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &cnt) != KERN_SUCCESS) return GTA6RT_E_IO;
    out->free_bytes = (uint64_t)vs.free_count * ps;
    out->speculative_bytes = (uint64_t)vs.speculative_count * ps;
    out->wired_bytes = (uint64_t)vs.wire_count * ps;
    out->active_bytes = (uint64_t)vs.active_count * ps;
    out->inactive_bytes = (uint64_t)vs.inactive_count * ps;
    out->file_backed_bytes = (uint64_t)vs.external_page_count * ps;
    out->purgeable_bytes = (uint64_t)vs.purgeable_count * ps;
    out->compressor_bytes = (uint64_t)vs.compressor_page_count * ps;
    out->compressed_bytes = (uint64_t)vs.total_uncompressed_pages_in_compressor * ps;
    out->pageins = vs.pageins;
    out->pageouts = vs.pageouts;
    out->swapins = vs.swapins;
    out->swapouts = vs.swapouts;
    out->compressions = vs.compressions;
    out->decompressions = vs.decompressions;
    out->purges = vs.purges;
    xsw_usage sw{};
    n = sizeof sw;
    if (sysctlbyname("vm.swapusage", &sw, &n, nullptr, 0) == 0) {
        out->swap_total = sw.xsu_total;
        out->swap_used = sw.xsu_used;
    }
    int level = 0;
    n = sizeof level;
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &n, nullptr, 0) == 0) out->pressure_level = (uint32_t)level;
    double la[3] = {0, 0, 0};
    if (getloadavg(la, 3) > 0) out->load1 = la[0];
    return GTA6RT_OK;
}

void gta6rt_os_memory_report_regions(const char* why, uint64_t min_bytes, uint32_t max_regions,
                                     gta6rt_memory_region_usage* out, size_t cap, size_t* out_count) {
    const uint64_t t0 = monotonic_ns();
    if (out_count) *out_count = 0;
    gta6rt_memory_stats st;
    if (gta6rt_os_memory_stats(&st) != GTA6RT_OK) {
        OS_LOG(GTA6RT_LOG_ERROR, "memory: task_info(TASK_VM_INFO) failed; no report");
        return;
    }
    struct Line {
        gta6rt_memory_region_usage u;
        const char* type;
    };
    std::vector<Line> all;
    std::vector<uint64_t> host_sizes;
    {
        VmLock lk;
        all.reserve(g_regions->size());
        for (auto& kv : *g_regions) {
            Region* r = kv.second;
            Line l{};
            l.u.base = r->base;
            l.u.size = r->size;
            const bool ph = (r->flags & GTA6RT_VM_PLACEHOLDER) != 0;
            l.u.kind = r->type == RType::Image ? 3 : r->type == RType::Mapped ? 2 : ph ? 4 : 1;
            l.type = r->type == RType::Image ? "image" : r->type == RType::Mapped ? "view" : ph ? "placeholder" : "private";
            r->each(0, r->npages(), [&](size_t lo, size_t hi, Attr a) {
                if (a.state == ST_COMMITTED) l.u.committed += (hi - lo) * kPage;
            });
            all.push_back(l);
            host_sizes.push_back(r->host_size);
        }
    }
    // The kernel walk runs without the vm lock (it takes tens of ms with the title's ~375 regions); a region
    // released meanwhile is reported from whatever is mapped there now.
    std::vector<Line> lines;
    uint64_t res_sum = 0, comp_sum = 0, released_sum = 0, entries = 0;
    for (size_t i = 0; i < all.size(); i++) {
        Line& l = all[i];
        mach_vm_address_t a = l.u.base;
        const mach_vm_address_t end = l.u.base + host_sizes[i];
        while (a < end) {
            mach_vm_size_t sz = 0;
            vm_region_extended_info_data_t ei;
            mach_msg_type_number_t cnt = VM_REGION_EXTENDED_INFO_COUNT;
            mach_port_t obj = MACH_PORT_NULL;
            mach_vm_address_t q = a;
            if (mach_vm_region(mach_task_self(), &q, &sz, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&ei, &cnt, &obj) !=
                    KERN_SUCCESS ||
                q >= end)
                break;
            const uint64_t lo = q < a ? a : q, hi = q + sz > end ? end : q + sz;
            if (hi > lo && sz) {
                const double share = double(hi - lo) / double(sz);
                const uint64_t res = (uint64_t)(double(ei.pages_resident) * double(g_host_page) * share);
                const uint64_t cmp = (uint64_t)(double(ei.pages_swapped_out) * double(g_host_page) * share);
                l.u.resident += res;
                l.u.compressed += cmp;
                if (ei.protection == VM_PROT_NONE) l.u.released_touched += res + cmp;
                l.u.kernel_entries++;
            }
            a = q + sz;
        }
        res_sum += l.u.resident;
        comp_sum += l.u.compressed;
        released_sum += l.u.released_touched;
        entries += l.u.kernel_entries;
        if (out && i < cap) out[i] = l.u;
        if (l.u.size >= min_bytes || l.u.resident >= min_bytes) lines.push_back(l);
    }
    if (out_count) *out_count = all.size();
    std::sort(lines.begin(), lines.end(), [](const Line& x, const Line& y) { return x.u.resident > y.u.resident; });
    gta6rt_system_memory sys;
    const bool have_sys = gta6rt_os_system_memory(&sys) == GTA6RT_OK;
    const double M = 1024.0 * 1024.0;
    char sys_text[320] = "";
    if (have_sys)
        snprintf(sys_text, sizeof sys_text,
                 "; machine: pressure %s, swap %.0f of %.0f MiB, free+speculative %.0f MiB, wired %.0f MiB, file cache %.0f "
                 "MiB, compressor %.0f MiB holding %.0f MiB, load %.1f",
                 sys.pressure_level >= 4 ? "critical" : sys.pressure_level >= 2 ? "warn" : "normal", sys.swap_used / M,
                 sys.swap_total / M, (sys.free_bytes + sys.speculative_bytes) / M, sys.wired_bytes / M,
                 sys.file_backed_bytes / M, sys.compressor_bytes / M, sys.compressed_bytes / M, sys.load1);
    OS_LOG(GTA6RT_LOG_INFO,
           "memory: %s: resident %.0f MiB, footprint %.0f MiB, compressed %.0f MiB, anonymous %.0f MiB, virtual %.1f GiB; "
           "vm table: %llu regions, %.1f GiB reserved, %.0f MiB committed, %.0f MiB resident and %.0f MiB compressed in "
           "them (aliased views counted per view), %.0f MiB resident elsewhere, %.0f MiB held in released pages "
           "(%llu kernel entries); report took %.1f ms%s",
           why ? why : "report", st.resident / M, st.footprint / M, st.compressed / M, st.internal / M,
           st.virtual_size / (M * 1024.0), (unsigned long long)st.regions, st.reserved / (M * 1024.0), st.committed / M,
           res_sum / M, comp_sum / M, st.resident > res_sum ? (st.resident - res_sum) / M : 0.0, released_sum / M,
           (unsigned long long)entries, (monotonic_ns() - t0) / 1e6, sys_text);
    uint32_t k = 0;
    for (const Line& l : lines) {
        if (k++ >= max_regions) break;
        OS_LOG(GTA6RT_LOG_INFO, "memory:   %#014lx %9.1f MiB %-11s committed %8.1f MiB resident %8.1f MiB compressed %8.1f MiB",
               (unsigned long)l.u.base, l.u.size / M, l.type, l.u.committed / M, l.u.resident / M, l.u.compressed / M);
    }
}

void gta6rt_os_memory_report(const char* why, uint64_t min_bytes, uint32_t max_regions) {
    gta6rt_os_memory_report_regions(why, min_bytes, max_regions, nullptr, 0, nullptr);
}

} // extern "C"
