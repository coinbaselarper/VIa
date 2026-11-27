// Backend-independent part of gta6rt_isa: configuration, CPUID policy, per-thread TEB records, implicit TLS
// arrays (with growth for late-loaded images), TlsAlloc slots, guest code ranges.

#include "isa_internal.h"

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace gta6rt::isa {

namespace {

struct TlsImage {
    gta6rt_tls_block_ctor ctor;
    gta6rt_tls_block_dtor dtor;
    void* user;
};

std::mutex g_mu;  // guards everything below
std::vector<ThreadRec*> g_threads;
std::map<uint32_t, TlsImage> g_tls_images;
uint64_t g_slot_bitmap[(GTA6RT_ISA_TLS_SLOTS_MAX + 63) / 64];
constexpr int kMaxRanges = 64;
uint64_t g_ranges[kMaxRanges][2];
std::atomic<int> g_nranges{0};

gta6rt_teb_mode g_mode = GTA6RT_TEB_SHARE;
uint64_t g_features = GTA6RT_CPUID_DEFAULT;
std::atomic<bool> g_inited{false};
void* g_peb = nullptr;
gta6rt_foreign_thread_hook g_foreign_hook = nullptr;
void* g_foreign_user = nullptr;
std::atomic<uint32_t> g_next_tid{0x10000};

pthread_key_t g_key;
pthread_once_t g_key_once = PTHREAD_ONCE_INIT;

void thread_exit_destructor(void* p);

void make_key() {
    pthread_key_create(&g_key, thread_exit_destructor);
}

uint32_t tls_needed_entries_locked() {
    return g_tls_images.empty() ? 0 : g_tls_images.rbegin()->first + 1;
}

void** alloc_tls_array(uint32_t cap) {
    return static_cast<void**>(calloc(cap ? cap : 1, sizeof(void*)));
}

// Grows rec's array to hold `need` entries; the old array stays readable (the thread may be using it).
void grow_tls_locked(ThreadRec* r, uint32_t need) {
    if (r->tls_owned && r->tls_cap >= need) return;
    uint32_t cap = std::max<uint32_t>(need, 8);
    cap = (cap + 7) & ~7u;
    void** a = alloc_tls_array(cap);
    if (r->tls) {
        const uint32_t n = std::min(r->tls_owned ? r->tls_cap : r->foreign_n, cap);
        for (uint32_t i = 0; i < n; ++i) a[i] = r->tls[i];
        if (r->tls_owned) r->tls_retired.push_back(r->tls);
    }
    r->tls = a;
    r->tls_cap = cap;
    r->tls_owned = true;
    teb_put(r->teb, GTA6RT_TEB_TLS_POINTER, reinterpret_cast<uint64_t>(a));
    backend_publish(r, GTA6RT_TEB_TLS_POINTER, reinterpret_cast<uint64_t>(a));
}

void free_thread_locked(ThreadRec* r) {
    if (r->tls_owned && r->tls) {
        for (auto& [idx, img] : g_tls_images)
            if (idx < r->tls_cap && r->tls[idx] && img.dtor) img.dtor(r->tls[idx], img.user);
        free(r->tls);
    }
    for (void** a : r->tls_retired) free(a);
    auto* exp = reinterpret_cast<void**>(teb_get(r->teb, GTA6RT_TEB_TLS_EXPANSION));
    free(exp);
    munmap(r->teb, kTebAlloc);
    g_threads.erase(std::remove(g_threads.begin(), g_threads.end(), r), g_threads.end());
    delete r;
}

void detach_rec(ThreadRec* r) {
    backend_unplant(r);
    pthread_setspecific(g_key, nullptr);
    std::lock_guard<std::mutex> lk(g_mu);
    free_thread_locked(r);
}

void thread_exit_destructor(void* p) {
    auto* r = static_cast<ThreadRec*>(p);
    if (r) detach_rec(r);
}

}  // namespace

gta6rt_teb_mode configured_teb_mode() {
    return g_mode;
}

uint64_t configured_features() {
    return g_features;
}

ThreadRec* current_thread() {
    pthread_once(&g_key_once, make_key);
    return static_cast<ThreadRec*>(pthread_getspecific(g_key));
}

void log_line(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[isa] %s\n", buf);
}

void log_signal_safe(const char* s) {
    ssize_t r = write(2, s, strlen(s));
    (void)r;
}

bool guest_code_contains(uint64_t pc, bool thread_has_teb) {
    const int n = g_nranges.load(std::memory_order_acquire);
    if (!n) return thread_has_teb;
    for (int i = 0; i < n; ++i)
        if (pc >= g_ranges[i][0] && pc < g_ranges[i][1]) return true;
    return false;
}

gta6rt_status ensure_thread_attached() {
    if (current_thread()) return GTA6RT_OK;
    gta6rt_status st;
    if (g_foreign_hook) {
        st = g_foreign_hook(g_foreign_user);
    } else {
        gta6rt_teb_desc d{};
        pthread_t self = pthread_self();
        auto top = reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(self));
        d.stack_base = top;
        d.stack_limit = top - pthread_get_stacksize_np(self);
        d.peb = g_peb;
        d.process_id = static_cast<uint32_t>(getpid());
        d.thread_id = 0;
        void* teb;
        st = gta6rt_isa_thread_attach(&d, &teb);
    }
    if (st == GTA6RT_OK) {
        ThreadRec* r = current_thread();
        if (!r) return GTA6RT_E_NOT_FOUND;
        r->auto_attached = true;
    }
    return st;
}

}  // namespace gta6rt::isa

using namespace gta6rt::isa;

extern "C" {

gta6rt_status gta6rt_isa_init(const gta6rt_isa_config* cfg) {
    pthread_once(&g_key_once, make_key);
    if (cfg) {
        if (cfg->teb_mode != GTA6RT_TEB_SHARE && cfg->teb_mode != GTA6RT_TEB_SWAP) return GTA6RT_E_INVALID_ARG;
        g_mode = cfg->teb_mode;
        g_features = cfg->cpuid_features ? cfg->cpuid_features : GTA6RT_CPUID_DEFAULT;
    }
    gta6rt_status st = backend_init(cfg);
    if (st == GTA6RT_OK) g_inited = true;
    return st;
}

gta6rt_status gta6rt_isa_cpuid(uint32_t leaf, uint32_t subleaf, gta6rt_cpuid_regs* out) {
    if (!out) return GTA6RT_E_INVALID_ARG;
    backend_cpuid_raw(leaf, subleaf, out);
    cpuid_apply_policy(leaf, subleaf, g_features, out);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_isa_set_peb(void* peb) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_peb = peb;
    for (ThreadRec* r : g_threads) {
        teb_put(r->teb, GTA6RT_TEB_PEB, reinterpret_cast<uint64_t>(peb));
        backend_publish(r, GTA6RT_TEB_PEB, reinterpret_cast<uint64_t>(peb));
    }
    return GTA6RT_OK;
}

gta6rt_status gta6rt_isa_thread_attach(const gta6rt_teb_desc* desc, void** out_teb) {
    if (!desc) return GTA6RT_E_INVALID_ARG;
    pthread_once(&g_key_once, make_key);
    if (ThreadRec* cur = current_thread()) {
        if (out_teb) *out_teb = cur->teb;
        return GTA6RT_OK;
    }
    uint64_t base = desc->stack_base, limit = desc->stack_limit;
    if (!base) {
        pthread_t self = pthread_self();
        base = reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(self));
        limit = base - pthread_get_stacksize_np(self);
    }
    void* mem = mmap(nullptr, kTebAlloc, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) return GTA6RT_E_NO_MEMORY;
    auto* teb = static_cast<uint8_t*>(mem);
    auto* r = new ThreadRec;
    r->teb = teb;
    r->thread_id = desc->thread_id ? desc->thread_id : g_next_tid.fetch_add(4);
    void* peb = desc->peb ? desc->peb : g_peb;
    teb_put(teb, GTA6RT_TEB_STACK_BASE, base);
    teb_put(teb, GTA6RT_TEB_STACK_LIMIT, limit);
    teb_put(teb, 0x20, 0x1e00);  // NtTib.Version (FiberData union) as on Windows
    teb_put(teb, GTA6RT_TEB_SELF, reinterpret_cast<uint64_t>(teb));
    teb_put(teb, GTA6RT_TEB_CLIENT_ID_PROCESS, desc->process_id ? desc->process_id : static_cast<uint32_t>(getpid()));
    teb_put(teb, GTA6RT_TEB_CLIENT_ID_THREAD, r->thread_id);
    teb_put(teb, GTA6RT_TEB_PEB, reinterpret_cast<uint64_t>(peb));
    TebPrivate* p = teb_private(teb);
    p->magic = kTebMagic;
    p->thread_id = r->thread_id;
    p->guest_gs = reinterpret_cast<uint64_t>(teb);
    p->rec = r;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (ThreadRec* o : g_threads) {
            if (teb_get(o->teb, GTA6RT_TEB_STACK_BASE) == base) {
                log_line("thread_attach: StackBase 0x%llx already used by thread %u", (unsigned long long)base,
                         o->thread_id);
                munmap(mem, kTebAlloc);
                delete r;
                return GTA6RT_E_ALREADY_EXISTS;
            }
        }
        if (desc->tls_pointer) {
            r->tls = static_cast<void**>(desc->tls_pointer);
            r->tls_owned = false;
            r->foreign_n = tls_needed_entries_locked();
            teb_put(teb, GTA6RT_TEB_TLS_POINTER, reinterpret_cast<uint64_t>(r->tls));
        } else {
            const uint32_t need = tls_needed_entries_locked();
            uint32_t cap = std::max<uint32_t>((need + 7) & ~7u, 8);
            r->tls = alloc_tls_array(cap);
            r->tls_cap = cap;
            r->tls_owned = true;
            for (auto& [idx, img] : g_tls_images) r->tls[idx] = img.ctor ? img.ctor(idx, r->thread_id, img.user) : nullptr;
            teb_put(teb, GTA6RT_TEB_TLS_POINTER, reinterpret_cast<uint64_t>(r->tls));
        }
    }

    gta6rt_status st = backend_plant(r, g_mode);
    if (st != GTA6RT_OK) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (r->tls_owned) {
            for (auto& [idx, img] : g_tls_images)
                if (idx < r->tls_cap && r->tls[idx] && img.dtor) img.dtor(r->tls[idx], img.user);
            free(r->tls);
        }
        munmap(mem, kTebAlloc);
        delete r;
        return st;
    }
    p->mode = r->mode;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_threads.push_back(r);
    }
    pthread_setspecific(g_key, r);
    if (out_teb) *out_teb = teb;
    return GTA6RT_OK;
}

void gta6rt_isa_thread_detach(void) {
    ThreadRec* r = current_thread();
    if (r) detach_rec(r);
}

void* gta6rt_isa_current_teb(void) {
    ThreadRec* r = current_thread();
    return r ? r->teb : nullptr;
}

int gta6rt_isa_thread_teb_mode(void) {
    ThreadRec* r = current_thread();
    return r ? static_cast<int>(r->mode) : -1;
}

gta6rt_status gta6rt_isa_set_tls_pointer(void* tls_pointer) {
    ThreadRec* r = current_thread();
    if (!r) return GTA6RT_E_NOT_FOUND;
    std::lock_guard<std::mutex> lk(g_mu);
    if (r->tls_owned && r->tls) r->tls_retired.push_back(r->tls);
    r->tls = static_cast<void**>(tls_pointer);
    r->tls_owned = false;
    r->tls_cap = 0;
    r->foreign_n = tls_needed_entries_locked();
    teb_put(r->teb, GTA6RT_TEB_TLS_POINTER, reinterpret_cast<uint64_t>(tls_pointer));
    backend_publish(r, GTA6RT_TEB_TLS_POINTER, reinterpret_cast<uint64_t>(tls_pointer));
    return GTA6RT_OK;
}

gta6rt_status gta6rt_isa_tls_register_image(uint32_t tls_index, gta6rt_tls_block_ctor ctor, gta6rt_tls_block_dtor dtor,
                                             void* user) {
    if (tls_index > 4096) return GTA6RT_E_INVALID_ARG;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_tls_images.count(tls_index)) return GTA6RT_E_ALREADY_EXISTS;
    g_tls_images[tls_index] = TlsImage{ctor, dtor, user};
    for (ThreadRec* r : g_threads) {
        if (!r->tls_owned || tls_index >= r->tls_cap) grow_tls_locked(r, tls_index + 1);
        void* blk = ctor ? ctor(tls_index, r->thread_id, user) : nullptr;
        __atomic_store_n(&r->tls[tls_index], blk, __ATOMIC_RELEASE);
    }
    return GTA6RT_OK;
}

gta6rt_status gta6rt_isa_tls_unregister_image(uint32_t tls_index) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_tls_images.find(tls_index);
    if (it == g_tls_images.end()) return GTA6RT_E_NOT_FOUND;
    for (ThreadRec* r : g_threads) {
        if (!r->tls_owned || tls_index >= r->tls_cap) continue;
        void* blk = __atomic_exchange_n(&r->tls[tls_index], nullptr, __ATOMIC_ACQ_REL);
        if (blk && it->second.dtor) it->second.dtor(blk, it->second.user);
    }
    g_tls_images.erase(it);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_isa_tls_slot_alloc(uint32_t* out_index) {
    if (!out_index) return GTA6RT_E_INVALID_ARG;
    std::lock_guard<std::mutex> lk(g_mu);
    for (uint32_t i = 0; i < GTA6RT_ISA_TLS_SLOTS_MAX; ++i) {
        if (!(g_slot_bitmap[i / 64] & (1ull << (i % 64)))) {
            g_slot_bitmap[i / 64] |= 1ull << (i % 64);
            *out_index = i;
            return GTA6RT_OK;
        }
    }
    return GTA6RT_E_NO_MEMORY;
}

static void** slot_ref(uint8_t* teb, uint32_t index, bool create) {
    if (index < GTA6RT_TEB_TLS_SLOT_COUNT)
        return reinterpret_cast<void**>(teb + GTA6RT_TEB_TLS_SLOTS) + index;
    auto* exp = reinterpret_cast<void**>(teb_get(teb, GTA6RT_TEB_TLS_EXPANSION));
    if (!exp) {
        if (!create) return nullptr;
        exp = static_cast<void**>(calloc(1024, sizeof(void*)));
        if (!exp) return nullptr;
        teb_put(teb, GTA6RT_TEB_TLS_EXPANSION, reinterpret_cast<uint64_t>(exp));
    }
    return exp + (index - GTA6RT_TEB_TLS_SLOT_COUNT);
}

gta6rt_status gta6rt_isa_tls_slot_free(uint32_t index) {
    if (index >= GTA6RT_ISA_TLS_SLOTS_MAX) return GTA6RT_E_INVALID_ARG;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!(g_slot_bitmap[index / 64] & (1ull << (index % 64)))) return GTA6RT_E_NOT_FOUND;
    g_slot_bitmap[index / 64] &= ~(1ull << (index % 64));
    for (ThreadRec* r : g_threads)
        if (void** s = slot_ref(r->teb, index, false)) __atomic_store_n(s, nullptr, __ATOMIC_RELEASE);
    return GTA6RT_OK;
}

void* gta6rt_isa_tls_slot_get(uint32_t index) {
    ThreadRec* r = current_thread();
    if (!r || index >= GTA6RT_ISA_TLS_SLOTS_MAX) return nullptr;
    void** s = slot_ref(r->teb, index, false);
    return s ? __atomic_load_n(s, __ATOMIC_ACQUIRE) : nullptr;
}

gta6rt_status gta6rt_isa_tls_slot_set(uint32_t index, void* value) {
    ThreadRec* r = current_thread();
    if (!r) return GTA6RT_E_NOT_FOUND;
    if (index >= GTA6RT_ISA_TLS_SLOTS_MAX) return GTA6RT_E_INVALID_ARG;
    void** s = slot_ref(r->teb, index, true);
    if (!s) return GTA6RT_E_NO_MEMORY;
    __atomic_store_n(s, value, __ATOMIC_RELEASE);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_isa_set_foreign_thread_hook(gta6rt_foreign_thread_hook fn, void* user) {
    g_foreign_user = user;
    g_foreign_hook = fn;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_isa_register_guest_code(gta6rt_gva base, size_t size) {
    if (!size) return GTA6RT_E_INVALID_ARG;
    std::lock_guard<std::mutex> lk(g_mu);
    const int n = g_nranges.load();
    if (n >= kMaxRanges) return GTA6RT_E_NO_MEMORY;
    g_ranges[n][0] = base;
    g_ranges[n][1] = base + size;
    g_nranges.store(n + 1, std::memory_order_release);
    return GTA6RT_OK;
}

}  // extern "C"
