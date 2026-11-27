#include "internal.h"

#include <sched.h>

using namespace gta6rt_os;

namespace {

struct ThreadCond {
    pthread_cond_t cv;
    ThreadCond() { pthread_cond_init(&cv, nullptr); }
    ~ThreadCond() { pthread_cond_destroy(&cv); }
};
thread_local ThreadCond t_cond;

struct Parker {
    const void* addr;
    pthread_cond_t* cv;
    bool woken;
    Parker* next;
};

struct alignas(64) Bucket {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    Parker* head = nullptr;
    Parker* tail = nullptr;
};

constexpr size_t kBuckets = 1024;
Bucket g_buckets[kBuckets];

Bucket& bucket_for(const void* a) {
    uintptr_t x = (uintptr_t)a;
    x ^= x >> 17;
    x *= 0x9E3779B97F4A7C15ull;
    return g_buckets[(x >> 40) & (kBuckets - 1)];
}

void remove_parker(Bucket& b, Parker* p) {
    Parker* prev = nullptr;
    for (Parker* c = b.head; c; prev = c, c = c->next)
        if (c == p) {
            if (prev) prev->next = c->next;
            else b.head = c->next;
            if (b.tail == c) b.tail = prev;
            return;
        }
}

// Blocks while validate() holds, until woken or the deadline passes. Returns false on timeout.
template <class V> bool park(const void* addr, V validate, uint64_t deadline) {
    Bucket& b = bucket_for(addr);
    pthread_mutex_lock(&b.m);
    if (!validate()) {
        pthread_mutex_unlock(&b.m);
        return true;
    }
    Parker p{addr, &t_cond.cv, false, nullptr};
    if (b.tail) b.tail->next = &p;
    else b.head = &p;
    b.tail = &p;
    bool ok = true;
    while (!p.woken) {
        if (deadline == UINT64_MAX) {
            pthread_cond_wait(p.cv, &b.m);
            continue;
        }
        uint64_t now = monotonic_ns();
        if (now >= deadline) {
            remove_parker(b, &p);
            ok = false;
            break;
        }
        uint64_t d = deadline - now;
        struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
        pthread_cond_timedwait_relative_np(p.cv, &b.m, &ts);
    }
    pthread_mutex_unlock(&b.m);
    return ok;
}

int unpark(const void* addr, int count) {
    Bucket& b = bucket_for(addr);
    int n = 0;
    pthread_mutex_lock(&b.m);
    Parker* prev = nullptr;
    Parker* c = b.head;
    while (c && (count < 0 || n < count)) {
        Parker* next = c->next;
        if (c->addr == addr) {
            if (prev) prev->next = next;
            else b.head = next;
            if (b.tail == c) b.tail = prev;
            c->woken = true;
            pthread_cond_signal(c->cv);
            n++;
        } else {
            prev = c;
        }
        c = next;
    }
    pthread_mutex_unlock(&b.m);
    return n;
}

// ---- SRW lock: bit0 writer, bit1 waiters, readers counted in steps of 4 ----
constexpr uint64_t kWriter = 1, kWait = 2, kReader = 4;

std::atomic<uint64_t>* srw_word(PSRWLOCK l) { return reinterpret_cast<std::atomic<uint64_t>*>(&l->Ptr); }

void srw_lock_excl(PSRWLOCK l) {
    auto* w = srw_word(l);
    for (int spin = 0; spin < 100; spin++) {
        uint64_t s = w->load(std::memory_order_relaxed);
        if (s == 0 && w->compare_exchange_weak(s, kWriter, std::memory_order_acquire)) return;
#if defined(__x86_64__)
        __builtin_ia32_pause();
#else
        __asm__ volatile("yield");
#endif
    }
    for (;;) {
        uint64_t s = w->load(std::memory_order_relaxed);
        if ((s & ~kWait) == 0) {
            if (w->compare_exchange_weak(s, s | kWriter, std::memory_order_acquire)) return;
            continue;
        }
        if (!(s & kWait) && !w->compare_exchange_weak(s, s | kWait, std::memory_order_relaxed)) continue;
        uint64_t expect = s | kWait;
        park(w, [&] { return w->load(std::memory_order_relaxed) == expect; }, UINT64_MAX);
    }
}

void srw_unlock_excl(PSRWLOCK l) {
    auto* w = srw_word(l);
    uint64_t old = w->exchange(0, std::memory_order_release);
    if (old & kWait) unpark(w, -1);
}

void srw_lock_shared(PSRWLOCK l) {
    auto* w = srw_word(l);
    for (;;) {
        uint64_t s = w->load(std::memory_order_relaxed);
        if (!(s & (kWriter | kWait))) {
            if (w->compare_exchange_weak(s, s + kReader, std::memory_order_acquire)) return;
            continue;
        }
        if (!(s & kWait) && !w->compare_exchange_weak(s, s | kWait, std::memory_order_relaxed)) continue;
        uint64_t expect = s | kWait;
        park(w, [&] { return w->load(std::memory_order_relaxed) == expect; }, UINT64_MAX);
    }
}

void srw_unlock_shared(PSRWLOCK l) {
    auto* w = srw_word(l);
    for (;;) {
        uint64_t s = w->load(std::memory_order_relaxed);
        uint64_t ns = s - kReader;
        bool wake = false;
        if ((ns >> 2) == 0 && (ns & kWait)) {
            ns &= ~kWait;
            wake = true;
        }
        if (w->compare_exchange_weak(s, ns, std::memory_order_release)) {
            if (wake) unpark(w, -1);
            return;
        }
    }
}

// ---- critical section: futex word in LockSemaphore (0 free, 1 held, 2 contended) ----
std::atomic<uint32_t>* cs_word(LPCRITICAL_SECTION cs) {
    return reinterpret_cast<std::atomic<uint32_t>*>(&cs->LockSemaphore);
}
std::atomic<uint64_t>* cs_owner(LPCRITICAL_SECTION cs) {
    return reinterpret_cast<std::atomic<uint64_t>*>(&cs->OwningThread);
}

void cs_enter(LPCRITICAL_SECTION cs, uint32_t tid) {
    auto* w = cs_word(cs);
    uint32_t spins = (uint32_t)(cs->SpinCount & 0xFFFFFF);
    if (spins > 4000) spins = 4000;
    for (uint32_t i = 0;; i++) {
        uint32_t z = 0;
        if (w->compare_exchange_weak(z, 1, std::memory_order_acquire)) goto got;
        if (i >= spins) break;
#if defined(__x86_64__)
        __builtin_ia32_pause();
#else
        __asm__ volatile("yield");
#endif
    }
    {
        uint32_t c = w->exchange(2, std::memory_order_acquire);
        while (c != 0) {
            park(w, [&] { return w->load(std::memory_order_relaxed) == 2; }, UINT64_MAX);
            c = w->exchange(2, std::memory_order_acquire);
        }
    }
got:
    cs_owner(cs)->store(tid, std::memory_order_relaxed);
    cs->RecursionCount = 1;
    cs->LockCount = -2;
}

void cs_leave(LPCRITICAL_SECTION cs) {
    if (--cs->RecursionCount > 0) return;
    cs->RecursionCount = 0;
    cs->LockCount = -1;
    cs_owner(cs)->store(0, std::memory_order_relaxed);
    auto* w = cs_word(cs);
    if (w->fetch_sub(1, std::memory_order_release) != 1) {
        w->store(0, std::memory_order_release);
        unpark(w, 1);
    }
}

std::atomic<uint32_t>* cv_word(PCONDITION_VARIABLE cv) { return reinterpret_cast<std::atomic<uint32_t>*>(&cv->Ptr); }

std::atomic<uintptr_t>* once_word(PINIT_ONCE io) { return reinterpret_cast<std::atomic<uintptr_t>*>(&io->Ptr); }

bool addr_equal(const volatile void* a, const void* b, size_t n) {
    switch (n) {
    case 1: return __atomic_load_n((const volatile uint8_t*)a, __ATOMIC_ACQUIRE) == *(const uint8_t*)b;
    case 2: return __atomic_load_n((const volatile uint16_t*)a, __ATOMIC_ACQUIRE) == *(const uint16_t*)b;
    case 4: return __atomic_load_n((const volatile uint32_t*)a, __ATOMIC_ACQUIRE) == *(const uint32_t*)b;
    case 8: return __atomic_load_n((const volatile uint64_t*)a, __ATOMIC_ACQUIRE) == *(const uint64_t*)b;
    default: return false;
    }
}

typedef unsigned __int128 u128;

u128 slist_load(PSLIST_HEADER h) {
    u128 v = 0;
    __atomic_compare_exchange_n((u128*)h, &v, v, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE);
    return v;
}

} // namespace

extern "C" {

gta6rt_status gta6rt_wait_on_address(const volatile void* addr, const void* compare, size_t size, uint64_t timeout_ns) {
    if (!addr || !compare || (size != 1 && size != 2 && size != 4 && size != 8)) return GTA6RT_E_INVALID_ARG;
    bool ok = park((const void*)addr, [&] { return addr_equal(addr, compare, size); }, deadline_from_timeout(timeout_ns));
    return ok ? GTA6RT_OK : GTA6RT_E_TIMEOUT;
}

void gta6rt_wake_address(const void* addr, int all) { unpark(addr, all ? -1 : 1); }

BOOL WINAPI WaitOnAddress(volatile void* Address, PVOID CompareAddress, SIZE_T AddressSize, DWORD dwMilliseconds) {
    gta6rt_status st = gta6rt_wait_on_address(Address, CompareAddress, AddressSize, ms_to_ns(dwMilliseconds));
    if (st == GTA6RT_OK) return TRUE;
    set_error(st == GTA6RT_E_TIMEOUT ? ERROR_TIMEOUT : ERROR_INVALID_PARAMETER);
    return FALSE;
}

void WINAPI WakeByAddressSingle(PVOID Address) { unpark(Address, 1); }
void WINAPI WakeByAddressAll(PVOID Address) { unpark(Address, -1); }

void WINAPI InitializeSRWLock(PSRWLOCK SRWLock) { SRWLock->Ptr = nullptr; }
void WINAPI AcquireSRWLockExclusive(PSRWLOCK SRWLock) { srw_lock_excl(SRWLock); }
void WINAPI AcquireSRWLockShared(PSRWLOCK SRWLock) { srw_lock_shared(SRWLock); }
void WINAPI ReleaseSRWLockExclusive(PSRWLOCK SRWLock) { srw_unlock_excl(SRWLock); }
void WINAPI ReleaseSRWLockShared(PSRWLOCK SRWLock) { srw_unlock_shared(SRWLock); }

BOOLEAN WINAPI TryAcquireSRWLockExclusive(PSRWLOCK SRWLock) {
    auto* w = srw_word(SRWLock);
    uint64_t s = 0;
    return w->compare_exchange_strong(s, kWriter, std::memory_order_acquire) ? 1 : 0;
}

BOOLEAN WINAPI TryAcquireSRWLockShared(PSRWLOCK SRWLock) {
    auto* w = srw_word(SRWLock);
    uint64_t s = w->load(std::memory_order_relaxed);
    while (!(s & (kWriter | kWait)))
        if (w->compare_exchange_weak(s, s + kReader, std::memory_order_acquire)) return 1;
    return 0;
}

BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD dwSpinCount, DWORD) {
    cs->DebugInfo = (PRTL_CRITICAL_SECTION_DEBUG)(intptr_t)-1;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = nullptr;
    cs->LockSemaphore = nullptr;
    cs->SpinCount = dwSpinCount & 0x00FFFFFF;
    return TRUE;
}

void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs) { InitializeCriticalSectionEx(cs, 0, 0); }

BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD dwSpinCount) {
    return InitializeCriticalSectionEx(cs, dwSpinCount, 0);
}

void WINAPI EnterCriticalSection(LPCRITICAL_SECTION cs) {
    uint32_t tid = current_tid();
    if (cs_owner(cs)->load(std::memory_order_relaxed) == tid) {
        cs->RecursionCount++;
        return;
    }
    cs_enter(cs, tid);
}

BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION cs) {
    uint32_t tid = current_tid();
    if (cs_owner(cs)->load(std::memory_order_relaxed) == tid) {
        cs->RecursionCount++;
        return TRUE;
    }
    uint32_t z = 0;
    if (!cs_word(cs)->compare_exchange_strong(z, 1, std::memory_order_acquire)) return FALSE;
    cs_owner(cs)->store(tid, std::memory_order_relaxed);
    cs->RecursionCount = 1;
    cs->LockCount = -2;
    return TRUE;
}

void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION cs) { cs_leave(cs); }

void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION cs) {
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = nullptr;
    cs->LockSemaphore = nullptr;
}

void WINAPI InitializeConditionVariable(PCONDITION_VARIABLE cv) { cv->Ptr = nullptr; }

BOOL WINAPI SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD dwMilliseconds) {
    auto* w = cv_word(cv);
    uint32_t seq = w->load(std::memory_order_relaxed);
    LONG rec = cs->RecursionCount;
    uint32_t tid = current_tid();
    cs->RecursionCount = 1;
    cs_leave(cs);
    bool ok = park(w, [&] { return w->load(std::memory_order_relaxed) == seq; }, deadline_from_timeout(ms_to_ns(dwMilliseconds)));
    if (cs_owner(cs)->load(std::memory_order_relaxed) == tid) cs->RecursionCount++;
    else cs_enter(cs, tid);
    cs->RecursionCount = rec;
    if (!ok) {
        set_error(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD dwMilliseconds, ULONG Flags) {
    auto* w = cv_word(cv);
    uint32_t seq = w->load(std::memory_order_relaxed);
    bool shared = (Flags & CONDITION_VARIABLE_LOCKMODE_SHARED) != 0;
    if (shared) srw_unlock_shared(l);
    else srw_unlock_excl(l);
    bool ok = park(w, [&] { return w->load(std::memory_order_relaxed) == seq; }, deadline_from_timeout(ms_to_ns(dwMilliseconds)));
    if (shared) srw_lock_shared(l);
    else srw_lock_excl(l);
    if (!ok) {
        set_error(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

void WINAPI WakeConditionVariable(PCONDITION_VARIABLE cv) {
    cv_word(cv)->fetch_add(1, std::memory_order_release);
    unpark(cv_word(cv), 1);
}

void WINAPI WakeAllConditionVariable(PCONDITION_VARIABLE cv) {
    cv_word(cv)->fetch_add(1, std::memory_order_release);
    unpark(cv_word(cv), -1);
}

void WINAPI InitOnceInitialize(PINIT_ONCE InitOnce) { InitOnce->Ptr = nullptr; }

BOOL WINAPI InitOnceBeginInitialize(LPINIT_ONCE io, DWORD dwFlags, PBOOL fPending, LPVOID* lpContext) {
    auto* w = once_word(io);
    for (;;) {
        uintptr_t s = w->load(std::memory_order_acquire);
        if ((s & 3) == 2) {
            if (fPending) *fPending = FALSE;
            if (lpContext) *lpContext = (void*)(s & ~(uintptr_t)3);
            return TRUE;
        }
        if (dwFlags & INIT_ONCE_CHECK_ONLY) {
            if (fPending) *fPending = TRUE;
            set_error(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if (s == 0) {
            if (w->compare_exchange_weak(s, 1, std::memory_order_acquire)) {
                if (fPending) *fPending = TRUE;
                return TRUE;
            }
            continue;
        }
        park(w, [&] { return w->load(std::memory_order_relaxed) == s; }, UINT64_MAX);
    }
}

BOOL WINAPI InitOnceComplete(LPINIT_ONCE io, DWORD dwFlags, LPVOID lpContext) {
    auto* w = once_word(io);
    if (dwFlags & INIT_ONCE_INIT_FAILED) w->store(0, std::memory_order_release);
    else w->store(((uintptr_t)lpContext & ~(uintptr_t)3) | 2, std::memory_order_release);
    unpark(w, -1);
    return TRUE;
}

BOOL WINAPI InitOnceExecuteOnce(PINIT_ONCE io, PINIT_ONCE_FN InitFn, PVOID Parameter, LPVOID* Context) {
    BOOL pending = FALSE;
    void* ctx = nullptr;
    if (!InitOnceBeginInitialize(io, 0, &pending, &ctx)) return FALSE;
    if (!pending) {
        if (Context) *Context = ctx;
        return TRUE;
    }
    uint64_t a[3] = {(uint64_t)(uintptr_t)io, (uint64_t)(uintptr_t)Parameter, (uint64_t)(uintptr_t)&ctx};
    BOOL ok = (BOOL)gta6rt_os_invoke((gta6rt_gva)(uintptr_t)InitFn, a, 3);
    InitOnceComplete(io, ok ? 0 : INIT_ONCE_INIT_FAILED, ctx);
    if (ok && Context) *Context = ctx;
    return ok;
}

void WINAPI InitializeSListHead(PSLIST_HEADER ListHead) {
    ListHead->Alignment = 0;
    ListHead->Region = 0;
}

PSLIST_ENTRY WINAPI InterlockedPushEntrySList(PSLIST_HEADER h, PSLIST_ENTRY e) {
    u128 old = slist_load(h);
    for (;;) {
        uint64_t lo = (uint64_t)old, hi = (uint64_t)(old >> 64);
        e->Next = (PSLIST_ENTRY)(uintptr_t)(hi & ~0xFull);
        uint64_t depth = (lo & 0xFFFF) + 1, seq = ((lo >> 16) + 1) & 0xFFFFFFFFFFFFull;
        u128 nv = ((u128)(uint64_t)(uintptr_t)e << 64) | (u128)((depth & 0xFFFF) | (seq << 16));
        if (__atomic_compare_exchange_n((u128*)h, &old, nv, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return (PSLIST_ENTRY)(uintptr_t)(hi & ~0xFull);
    }
}

PSLIST_ENTRY WINAPI InterlockedPopEntrySList(PSLIST_HEADER h) {
    u128 old = slist_load(h);
    for (;;) {
        uint64_t lo = (uint64_t)old, hi = (uint64_t)(old >> 64);
        PSLIST_ENTRY first = (PSLIST_ENTRY)(uintptr_t)(hi & ~0xFull);
        if (!first) return nullptr;
        PSLIST_ENTRY next = first->Next;
        uint64_t depth = (lo & 0xFFFF) - 1, seq = ((lo >> 16) + 1) & 0xFFFFFFFFFFFFull;
        u128 nv = ((u128)(uint64_t)(uintptr_t)next << 64) | (u128)((depth & 0xFFFF) | (seq << 16));
        if (__atomic_compare_exchange_n((u128*)h, &old, nv, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return first;
    }
}

PSLIST_ENTRY WINAPI InterlockedFlushSList(PSLIST_HEADER h) {
    u128 old = slist_load(h);
    for (;;) {
        uint64_t lo = (uint64_t)old, hi = (uint64_t)(old >> 64);
        if (!(hi & ~0xFull)) return nullptr;
        uint64_t seq = ((lo >> 16) + 1) & 0xFFFFFFFFFFFFull;
        u128 nv = (u128)(seq << 16);
        if (__atomic_compare_exchange_n((u128*)h, &old, nv, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return (PSLIST_ENTRY)(uintptr_t)(hi & ~0xFull);
    }
}

USHORT WINAPI QueryDepthSList(PSLIST_HEADER h) { return (USHORT)(slist_load(h) & 0xFFFF); }

} // extern "C"
