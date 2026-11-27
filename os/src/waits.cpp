#include "internal.h"

#include <os/lock.h>
#include <os/os_sync_wait_on_address.h>

#include <cerrno>
#include <map>
#include <unordered_map>

using namespace gta6rt_os;

enum class WKind : uint8_t { Event, Semaphore, Mutex, Timer };

struct WaitLink {
    WaitLink* prev;
    WaitLink* next;
    std::atomic<uint32_t>* word;
};

// Object state is atomic so an uncontended wait or signal takes no lock. Each object's waiter list has its own
// lock; g_wait_mx guards names, timers, the mutex list, APC state and wait-all checks, and is taken before a list
// lock, never after. Nothing sleeps holding either: a blocked waiter sleeps on its own thread wake word. It is
// linked (and counted in `waiters`) before its last re-check, and a signaller takes the list lock to bump the
// linked words only when that count is nonzero (both sides seq_cst, so either the signaller sees the waiter or the
// waiter's re-check sees the new state).
struct gta6rt_waitable {
    std::atomic<int> refs{1};
    WKind kind;
    bool manual = false;
    std::atomic<bool> signaled{false};
    std::atomic<int32_t> count{0};
    int32_t max = 0;
    std::atomic<uint32_t> owner_tid{0};
    uint32_t recursion = 0;  // only the owner touches it (and abandonment, after the owner has gone)
    std::atomic<bool> abandoned{false};
    std::atomic<uint32_t> waiters{0};
    os_unfair_lock list_lock = OS_UNFAIR_LOCK_INIT;
    gta6rt_waitable* mutex_next = nullptr;
    WaitLink* head = nullptr;
    std::u16string* name = nullptr;
    // timer
    bool armed = false;
    uint64_t due = 0;
    uint64_t period_ns = 0;
    gta6rt_gva apc_fn = 0;
    uint64_t apc_arg = 0;
    gta6rt_thread* apc_thread = nullptr;
};

namespace gta6rt_os {
pthread_mutex_t g_wait_mx = PTHREAD_MUTEX_INITIALIZER;
}

namespace {

gta6rt_waitable* g_mutexes; // all mutexes, for abandonment
std::unordered_map<std::u16string, gta6rt_waitable*>* g_names;
std::multimap<uint64_t, gta6rt_waitable*>* g_timers;
pthread_cond_t g_timer_cv = PTHREAD_COND_INITIALIZER;
bool g_timer_thread_started;

bool is_signaled(gta6rt_waitable* w, uint32_t tid) {
    switch (w->kind) {
    case WKind::Event:
    case WKind::Timer: return w->signaled.load();
    case WKind::Semaphore: return w->count.load() > 0;
    case WKind::Mutex: {
        const uint32_t o = w->owner_tid.load();
        return o == 0 || o == tid;
    }
    }
    return false;
}

enum Take { TAKE_NONE = 0, TAKE_OK = 1, TAKE_ABANDONED = 2 };

Take try_take(gta6rt_waitable* w, uint32_t tid) {
    switch (w->kind) {
    case WKind::Event:
    case WKind::Timer: {
        if (w->manual) return w->signaled.load() ? TAKE_OK : TAKE_NONE;
        bool expect = true;
        return w->signaled.compare_exchange_strong(expect, false) ? TAKE_OK : TAKE_NONE;
    }
    case WKind::Semaphore: {
        int32_t c = w->count.load();
        while (c > 0)
            if (w->count.compare_exchange_weak(c, c - 1)) return TAKE_OK;
        return TAKE_NONE;
    }
    case WKind::Mutex: {
        uint32_t o = w->owner_tid.load();
        if (o == tid) {
            w->recursion++;
            return TAKE_OK;
        }
        if (o != 0 || !w->owner_tid.compare_exchange_strong(o, tid)) return TAKE_NONE;
        w->recursion = 1;
        return w->abandoned.exchange(false) ? TAKE_ABANDONED : TAKE_OK;
    }
    }
    return TAKE_NONE;
}

void wake_all_locked(gta6rt_waitable* w) {
    os_unfair_lock_lock(&w->list_lock);
    for (WaitLink* l = w->head; l; l = l->next) wake_word(l->word);
    os_unfair_lock_unlock(&w->list_lock);
}

// The words are bumped under the lock (the links live on the waiters' stacks); the wake system calls run after it,
// where a waiter that has already left only sees a spurious wake on its thread word.
void wake_if_waiters(gta6rt_waitable* w) {
    if (w->waiters.load() == 0) return;
    constexpr int kBatch = 16;
    std::atomic<uint32_t>* words[kBatch];
    int n = 0;
    os_unfair_lock_lock(&w->list_lock);
    for (WaitLink* l = w->head; l; l = l->next) {
        if (n < kBatch) {
            l->word->fetch_add(1);
            words[n++] = l->word;
        } else {
            wake_word(l->word);
        }
    }
    os_unfair_lock_unlock(&w->list_lock);
    for (int i = 0; i < n; i++) os_sync_wake_by_address_any(words[i], sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

// Gives back what try_take took (a wait-all that lost a race with a lock-free taker). Called with g_wait_mx held.
void untake_locked(gta6rt_waitable* w, Take t) {
    switch (w->kind) {
    case WKind::Event:
    case WKind::Timer:
        if (!w->manual) w->signaled.store(true);
        break;
    case WKind::Semaphore: w->count.fetch_add(1); break;
    case WKind::Mutex:
        if (w->recursion > 1) {
            w->recursion--;
        } else {
            w->recursion = 0;
            if (t == TAKE_ABANDONED) w->abandoned.store(true);
            w->owner_tid.store(0);
        }
        break;
    }
    wake_all_locked(w);
}

void link(gta6rt_waitable* w, WaitLink* l) {
    os_unfair_lock_lock(&w->list_lock);
    l->prev = nullptr;
    l->next = w->head;
    if (w->head) w->head->prev = l;
    w->head = l;
    w->waiters.fetch_add(1);
    os_unfair_lock_unlock(&w->list_lock);
}

void unlink(gta6rt_waitable* w, WaitLink* l) {
    os_unfair_lock_lock(&w->list_lock);
    if (l->prev) l->prev->next = l->next;
    else w->head = l->next;
    if (l->next) l->next->prev = l->prev;
    w->waiters.fetch_sub(1);
    os_unfair_lock_unlock(&w->list_lock);
}

gta6rt_waitable* make(WKind k) {
    gta6rt_waitable* w = new gta6rt_waitable();
    w->kind = k;
    return w;
}

void destroy(gta6rt_waitable* w) {
    pthread_mutex_lock(&g_wait_mx);
    if (w->kind == WKind::Mutex) {
        for (gta6rt_waitable** pp = &g_mutexes; *pp; pp = &(*pp)->mutex_next)
            if (*pp == w) {
                *pp = w->mutex_next;
                break;
            }
    }
    if (w->name && g_names) {
        auto it = g_names->find(*w->name);
        if (it != g_names->end() && it->second == w) g_names->erase(it);
    }
    if (w->armed && g_timers) {
        for (auto it = g_timers->begin(); it != g_timers->end(); ++it)
            if (it->second == w) {
                g_timers->erase(it);
                break;
            }
    }
    pthread_mutex_unlock(&g_wait_mx);
    if (w->apc_thread) gta6rt_thread_release(w->apc_thread);
    delete w->name;
    delete w;
}

void w_release(void* p) { gta6rt_waitable_release((gta6rt_waitable*)p); }
void w_retain(void* p) { gta6rt_waitable_retain((gta6rt_waitable*)p); }

HANDLE to_handle(gta6rt_waitable* w) { return HV(handle_alloc_internal(GTA6RT_HANDLE_WAITABLE, w, w_release, w_retain)); }

gta6rt_waitable* ref_kind(HANDLE h, WKind k) {
    void (*rel)(void*) = nullptr;
    gta6rt_waitable* w = (gta6rt_waitable*)handle_ref(H(h), GTA6RT_HANDLE_WAITABLE, &rel);
    if (w && w->kind != k) {
        gta6rt_waitable_release(w);
        w = nullptr;
    }
    if (!w) set_error(ERROR_INVALID_HANDLE);
    return w;
}

// A waitable kept alive by its handle entry (pin) or by its own reference (thread and file handles).
struct Pinned {
    gta6rt_waitable* w = nullptr;
    void* pin = nullptr;
    void drop() {
        if (pin) handle_unpin(pin);
        else if (w) gta6rt_waitable_release(w);
        w = nullptr;
        pin = nullptr;
    }
};

bool pin_any(HANDLE hh, Pinned* out) {
    const uint64_t h = H(hh);
    if (h == kPseudoThread) {
        out->w = gta6rt_thread_waitable(gta6rt_thread_current());
        gta6rt_waitable_retain(out->w);
        return true;
    }
    gta6rt_handle_kind kind;
    void* obj = nullptr;
    void* pin = handle_pin(h, &kind, &obj);
    if (!pin) return false;
    switch (kind) {
    case GTA6RT_HANDLE_WAITABLE:
        out->w = (gta6rt_waitable*)obj;
        out->pin = pin;
        return true;
    case GTA6RT_HANDLE_THREAD:
        out->w = gta6rt_thread_waitable((gta6rt_thread*)obj);
        gta6rt_waitable_retain(out->w);
        break;
    case GTA6RT_HANDLE_FILE: out->w = file_waitable((gta6rt_file*)obj); break;
    default: break;
    }
    handle_unpin(pin);
    return out->w != nullptr;
}

bool pin_kind(HANDLE h, WKind k, Pinned* out) {
    gta6rt_handle_kind kind;
    void* obj = nullptr;
    void* pin = handle_pin(H(h), &kind, &obj);
    if (pin && kind == GTA6RT_HANDLE_WAITABLE && ((gta6rt_waitable*)obj)->kind == k) {
        out->w = (gta6rt_waitable*)obj;
        out->pin = pin;
        return true;
    }
    if (pin) handle_unpin(pin);
    set_error(ERROR_INVALID_HANDLE);
    return false;
}

// Named objects: returns an existing object of the same name (retained) or registers w.
gta6rt_waitable* named(const char16_t* name, gta6rt_waitable* w, bool* existed, bool* wrong_kind) {
    *existed = false;
    *wrong_kind = false;
    if (!name || !*name) return w;
    std::u16string key(name);
    pthread_mutex_lock(&g_wait_mx);
    if (!g_names) g_names = new std::unordered_map<std::u16string, gta6rt_waitable*>();
    auto it = g_names->find(key);
    if (it != g_names->end() && it->second->refs.load() > 0) {
        gta6rt_waitable* e = it->second;
        if (e->kind != w->kind) {
            *wrong_kind = true;
            pthread_mutex_unlock(&g_wait_mx);
            return nullptr;
        }
        e->refs.fetch_add(1);
        *existed = true;
        pthread_mutex_unlock(&g_wait_mx);
        return e;
    }
    w->name = new std::u16string(key);
    (*g_names)[key] = w;
    pthread_mutex_unlock(&g_wait_mx);
    return w;
}

HANDLE create_named(gta6rt_waitable* w, const char16_t* name) {
    bool existed, wrong;
    gta6rt_waitable* r = named(name, w, &existed, &wrong);
    if (r != w) {
        if (w->kind == WKind::Mutex) {
            pthread_mutex_lock(&g_wait_mx);
            for (gta6rt_waitable** pp = &g_mutexes; *pp; pp = &(*pp)->mutex_next)
                if (*pp == w) {
                    *pp = w->mutex_next;
                    break;
                }
            pthread_mutex_unlock(&g_wait_mx);
        }
        delete w;
    }
    if (wrong) {
        set_error(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    set_error(existed ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return to_handle(r);
}

void timer_arm_locked(gta6rt_waitable* w);

void* timer_main(void*) {
    pthread_setname_np("gta6rt-timers");
    pthread_mutex_lock(&g_wait_mx);
    for (;;) {
        uint64_t now = monotonic_ns();
        while (!g_timers->empty() && g_timers->begin()->first <= now) {
            gta6rt_waitable* w = g_timers->begin()->second;
            g_timers->erase(g_timers->begin());
            w->armed = false;
            w->signaled.store(true);
            wake_all_locked(w);
            if (w->apc_fn && w->apc_thread) {
                Apc* a = new Apc();
                a->fn = w->apc_fn;
                uint64_t ft = filetime_now();
                a->args[0] = w->apc_arg;
                a->args[1] = (uint32_t)ft;
                a->args[2] = (uint32_t)(ft >> 32);
                a->nargs = 3;
                if (!queue_apc_locked(w->apc_thread, a)) delete a;
            }
            if (w->period_ns) {
                w->due += w->period_ns;
                if (w->due < now) w->due = now + w->period_ns;
                timer_arm_locked(w);
            }
        }
        if (g_timers->empty()) {
            pthread_cond_wait(&g_timer_cv, &g_wait_mx);
        } else {
            uint64_t d = g_timers->begin()->first - now;
            struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
            pthread_cond_timedwait_relative_np(&g_timer_cv, &g_wait_mx, &ts);
        }
    }
    return nullptr;
}

void timer_arm_locked(gta6rt_waitable* w) {
    if (!g_timers) g_timers = new std::multimap<uint64_t, gta6rt_waitable*>();
    if (!g_timer_thread_started) {
        g_timer_thread_started = true;
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &at, timer_main, nullptr);
        pthread_attr_destroy(&at);
    }
    w->armed = true;
    g_timers->emplace(w->due, w);
    pthread_cond_signal(&g_timer_cv);
}

void timer_disarm_locked(gta6rt_waitable* w) {
    if (!w->armed || !g_timers) return;
    for (auto it = g_timers->begin(); it != g_timers->end(); ++it)
        if (it->second == w) {
            g_timers->erase(it);
            break;
        }
    w->armed = false;
}

enum WaitRc { RC_OK, RC_ABANDONED, RC_TIMEOUT, RC_APC, RC_INVALID };

WaitRc wait_core(gta6rt_waitable* const* objs, uint32_t n, bool all, uint64_t timeout_ns, bool alertable,
                 uint32_t* out_index) {
    if (n == 0 || n > MAXIMUM_WAIT_OBJECTS) return RC_INVALID;
    if (all)
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t j = i + 1; j < n; j++)
                if (objs[i] == objs[j]) return RC_INVALID;
    gta6rt_thread* self = gta6rt_thread_current();
    uint32_t tid = gta6rt_thread_id(self);
    if (!alertable && (!all || n == 1)) {
        for (uint32_t i = 0; i < n; i++) {
            const Take t = try_take(objs[i], tid);
            if (t != TAKE_NONE) {
                *out_index = i;
                return t == TAKE_ABANDONED ? RC_ABANDONED : RC_OK;
            }
        }
    }
    const uint64_t deadline = deadline_from_timeout(timeout_ns);
    std::atomic<uint32_t>* word = thread_wake_word(self);
    WaitLink links[MAXIMUM_WAIT_OBJECTS];
    bool linked = false;
    WaitRc rc = RC_TIMEOUT;
    const bool locked_check = alertable || (all && n > 1);
    for (;;) {
        const uint32_t seen = word->load();
        bool done = false;
        if (locked_check) pthread_mutex_lock(&g_wait_mx);
        if (alertable && has_apcs_locked(self)) {
            rc = RC_APC;
            done = true;
        } else if (all && n > 1) {
            uint32_t k = 0;
            while (k < n && is_signaled(objs[k], tid)) k++;
            if (k == n) {
                Take took[MAXIMUM_WAIT_OBJECTS];
                uint32_t got = 0;
                while (got < n && (took[got] = try_take(objs[got], tid)) != TAKE_NONE) got++;
                if (got == n) {
                    rc = RC_OK;
                    *out_index = 0;
                    for (uint32_t i = 0; i < n; i++)
                        if (took[i] == TAKE_ABANDONED) {
                            rc = RC_ABANDONED;
                            *out_index = i;
                            break;
                        }
                    done = true;
                } else {
                    while (got > 0) {
                        got--;
                        untake_locked(objs[got], took[got]);
                    }
                }
            }
        } else {
            for (uint32_t i = 0; i < n && !done; i++) {
                const Take t = try_take(objs[i], tid);
                if (t != TAKE_NONE) {
                    rc = t == TAKE_ABANDONED ? RC_ABANDONED : RC_OK;
                    *out_index = i;
                    done = true;
                }
            }
        }
        if (locked_check) pthread_mutex_unlock(&g_wait_mx);
        if (done) break;
        const uint64_t now = monotonic_ns();
        if (timeout_ns == 0 || now >= deadline) {
            rc = RC_TIMEOUT;
            break;
        }
        if (!linked) {
            for (uint32_t i = 0; i < n; i++) {
                links[i].word = word;
                link(objs[i], &links[i]);
            }
            if (alertable) {
                pthread_mutex_lock(&g_wait_mx);
                set_alertable_locked(self, true);
                pthread_mutex_unlock(&g_wait_mx);
            }
            linked = true;
            continue;
        }
        sleep_on_word(word, seen, deadline == UINT64_MAX ? UINT64_MAX : deadline - now);
    }
    if (linked) {
        for (uint32_t i = 0; i < n; i++) unlink(objs[i], &links[i]);
        if (alertable) {
            pthread_mutex_lock(&g_wait_mx);
            set_alertable_locked(self, false);
            pthread_mutex_unlock(&g_wait_mx);
        }
    }
    return rc;
}

DWORD wait_handles(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, BOOL alertable) {
    if (n == 0 || n > MAXIMUM_WAIT_OBJECTS || !hs) {
        set_error(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    Pinned ps[MAXIMUM_WAIT_OBJECTS];
    gta6rt_waitable* ws[MAXIMUM_WAIT_OBJECTS];
    for (DWORD i = 0; i < n; i++) {
        if (!pin_any(hs[i], &ps[i])) {
            for (DWORD j = 0; j < i; j++) ps[j].drop();
            OS_LOG(GTA6RT_LOG_WARN, "wait on invalid handle %#llx", (unsigned long long)H(hs[i]));
            set_error(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
        ws[i] = ps[i].w;
    }
    DWORD r = wait_win32(ws, n, all, ms_to_ns(ms), alertable);
    for (DWORD i = 0; i < n; i++) ps[i].drop();
    return r;
}

} // namespace

namespace gta6rt_os {

void waitable_init_event(gta6rt_waitable* w, bool manual, bool set) {
    w->kind = WKind::Event;
    w->manual = manual;
    w->signaled.store(set);
}

gta6rt_waitable* waitable_new_event(bool manual, bool set) {
    gta6rt_waitable* w = make(WKind::Event);
    waitable_init_event(w, manual, set);
    return w;
}

void waitable_set_locked(gta6rt_waitable* w) {
    w->signaled.store(true);
    wake_all_locked(w);
}

void waitable_reset_locked(gta6rt_waitable* w) { w->signaled.store(false); }

void abandon_mutexes_locked(gta6rt_thread* t) {
    uint32_t tid = gta6rt_thread_id(t);
    for (gta6rt_waitable* m = g_mutexes; m; m = m->mutex_next)
        if (m->owner_tid.load() == tid) {
            m->recursion = 0;
            m->abandoned.store(true);
            m->owner_tid.store(0);
            wake_all_locked(m);
        }
}

DWORD wait_win32(gta6rt_waitable* const* objs, uint32_t n, bool all, uint64_t timeout_ns, bool alertable) {
    uint32_t idx = 0;
    switch (wait_core(objs, n, all, timeout_ns, alertable, &idx)) {
    case RC_OK: return WAIT_OBJECT_0 + idx;
    case RC_ABANDONED: return WAIT_ABANDONED_0 + idx;
    case RC_TIMEOUT: return WAIT_TIMEOUT;
    case RC_APC: run_apcs(); return WAIT_IO_COMPLETION;
    default: set_error(ERROR_INVALID_PARAMETER); return WAIT_FAILED;
    }
}

gta6rt_waitable* handle_to_waitable(HANDLE hh) {
    uint64_t h = H(hh);
    if (h == kPseudoThread) {
        gta6rt_waitable* w = gta6rt_thread_waitable(gta6rt_thread_current());
        gta6rt_waitable_retain(w);
        return w;
    }
    void (*rel)(void*) = nullptr;
    gta6rt_handle_kind kind;
    void* obj = handle_ref_kind(h, &kind, &rel);
    if (!obj) return nullptr;
    switch (kind) {
    case GTA6RT_HANDLE_WAITABLE: return (gta6rt_waitable*)obj;
    case GTA6RT_HANDLE_THREAD: {
        gta6rt_waitable* w = gta6rt_thread_waitable((gta6rt_thread*)obj);
        gta6rt_waitable_retain(w);
        if (rel) rel(obj);
        return w;
    }
    case GTA6RT_HANDLE_FILE: {
        gta6rt_waitable* w = file_waitable((gta6rt_file*)obj);
        if (rel) rel(obj);
        return w;
    }
    default:
        if (rel) rel(obj);
        return nullptr;
    }
}

} // namespace gta6rt_os

extern "C" {

gta6rt_status gta6rt_event_create(int manual_reset, int initially_set, gta6rt_waitable** out) {
    if (!out) return GTA6RT_E_INVALID_ARG;
    *out = waitable_new_event(manual_reset != 0, initially_set != 0);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_event_set(gta6rt_waitable* e) {
    if (!e || (e->kind != WKind::Event && e->kind != WKind::Timer)) return GTA6RT_E_INVALID_ARG;
    if (!e->signaled.exchange(true)) wake_if_waiters(e);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_event_reset(gta6rt_waitable* e) {
    if (!e || (e->kind != WKind::Event && e->kind != WKind::Timer)) return GTA6RT_E_INVALID_ARG;
    e->signaled.store(false);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_semaphore_create(int32_t initial, int32_t maximum, gta6rt_waitable** out) {
    if (!out || maximum <= 0 || initial < 0 || initial > maximum) return GTA6RT_E_INVALID_ARG;
    gta6rt_waitable* w = make(WKind::Semaphore);
    w->count.store(initial);
    w->max = maximum;
    *out = w;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_semaphore_release(gta6rt_waitable* s, int32_t count, int32_t* out_previous) {
    if (!s || s->kind != WKind::Semaphore || count <= 0) return GTA6RT_E_INVALID_ARG;
    int32_t c = s->count.load();
    do {
        if (out_previous) *out_previous = c;
        if ((int64_t)c + count > s->max) return GTA6RT_E_BUSY;
    } while (!s->count.compare_exchange_weak(c, c + count));
    wake_if_waiters(s);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_mutex_create(int initially_owned, gta6rt_waitable** out) {
    if (!out) return GTA6RT_E_INVALID_ARG;
    gta6rt_waitable* w = make(WKind::Mutex);
    uint32_t tid = initially_owned ? gta6rt_thread_id(gta6rt_thread_current()) : 0;
    if (initially_owned) {
        w->recursion = 1;
        w->owner_tid.store(tid);
    }
    pthread_mutex_lock(&g_wait_mx);
    w->mutex_next = g_mutexes;
    g_mutexes = w;
    pthread_mutex_unlock(&g_wait_mx);
    *out = w;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_mutex_release(gta6rt_waitable* m) {
    if (!m || m->kind != WKind::Mutex) return GTA6RT_E_INVALID_ARG;
    uint32_t tid = gta6rt_thread_id(gta6rt_thread_current());
    if (m->owner_tid.load() != tid || m->recursion == 0) return GTA6RT_E_ACCESS;
    if (--m->recursion == 0) {
        m->owner_tid.store(0);
        wake_if_waiters(m);
    }
    return GTA6RT_OK;
}

void gta6rt_waitable_retain(gta6rt_waitable* w) {
    if (w) w->refs.fetch_add(1);
}

void gta6rt_waitable_release(gta6rt_waitable* w) {
    if (w && w->refs.fetch_sub(1) == 1) destroy(w);
}

gta6rt_status gta6rt_wait_any(gta6rt_waitable* const* objs, uint32_t count, uint64_t timeout_ns, uint32_t* out_index) {
    uint32_t idx = 0;
    WaitRc rc = wait_core(objs, count, false, timeout_ns, false, &idx);
    if (out_index) *out_index = idx;
    switch (rc) {
    case RC_OK: return (gta6rt_status)idx;
    case RC_ABANDONED: return GTA6RT_E_ABANDONED;
    case RC_TIMEOUT: return GTA6RT_E_TIMEOUT;
    default: return GTA6RT_E_INVALID_ARG;
    }
}

gta6rt_status gta6rt_wait_all(gta6rt_waitable* const* objs, uint32_t count, uint64_t timeout_ns) {
    uint32_t idx = 0;
    switch (wait_core(objs, count, true, timeout_ns, false, &idx)) {
    case RC_OK: return GTA6RT_OK;
    case RC_ABANDONED: return GTA6RT_E_ABANDONED;
    case RC_TIMEOUT: return GTA6RT_E_TIMEOUT;
    default: return GTA6RT_E_INVALID_ARG;
    }
}

// ---- Win32 ----

HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES, BOOL bManualReset, BOOL bInitialState, LPCWSTR lpName) {
    return create_named(waitable_new_event(bManualReset != 0, bInitialState != 0), lpName);
}

HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES a, BOOL bManualReset, BOOL bInitialState, LPCSTR lpName) {
    std::u16string n = utf8_to_u16(lpName);
    return CreateEventW(a, bManualReset, bInitialState, lpName ? n.c_str() : nullptr);
}

HANDLE WINAPI CreateEventExW(LPSECURITY_ATTRIBUTES a, LPCWSTR lpName, DWORD dwFlags, DWORD) {
    return CreateEventW(a, (dwFlags & CREATE_EVENT_MANUAL_RESET) != 0, (dwFlags & CREATE_EVENT_INITIAL_SET) != 0,
                        lpName);
}

HANDLE WINAPI OpenEventW(DWORD, BOOL, LPCWSTR lpName) {
    if (!lpName) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    pthread_mutex_lock(&g_wait_mx);
    gta6rt_waitable* w = nullptr;
    if (g_names) {
        auto it = g_names->find(std::u16string(lpName));
        if (it != g_names->end() && it->second->kind == WKind::Event) {
            w = it->second;
            w->refs.fetch_add(1);
        }
    }
    pthread_mutex_unlock(&g_wait_mx);
    if (!w) {
        set_error(ERROR_FILE_NOT_FOUND);
        return nullptr;
    }
    return to_handle(w);
}

BOOL WINAPI SetEvent(HANDLE hEvent) {
    Pinned p;
    if (!pin_kind(hEvent, WKind::Event, &p)) return FALSE;
    gta6rt_event_set(p.w);
    p.drop();
    return TRUE;
}

BOOL WINAPI ResetEvent(HANDLE hEvent) {
    Pinned p;
    if (!pin_kind(hEvent, WKind::Event, &p)) return FALSE;
    gta6rt_event_reset(p.w);
    p.drop();
    return TRUE;
}

HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES, BOOL bInitialOwner, LPCWSTR lpName) {
    gta6rt_waitable* w = nullptr;
    gta6rt_mutex_create(0, &w);
    HANDLE h = create_named(w, lpName);
    if (!h) return nullptr;
    DWORD err = GetLastError();
    if (bInitialOwner && err != ERROR_ALREADY_EXISTS) {
        gta6rt_waitable* r = ref_kind(h, WKind::Mutex);
        uint32_t idx;
        if (r) {
            wait_core(&r, 1, false, 0, false, &idx);
            gta6rt_waitable_release(r);
        }
    }
    set_error(err);
    return h;
}

HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES a, BOOL bInitialOwner, LPCSTR lpName) {
    std::u16string n = utf8_to_u16(lpName);
    return CreateMutexW(a, bInitialOwner, lpName ? n.c_str() : nullptr);
}

HANDLE WINAPI CreateMutexExW(LPSECURITY_ATTRIBUTES a, LPCWSTR lpName, DWORD dwFlags, DWORD) {
    return CreateMutexW(a, (dwFlags & CREATE_MUTEX_INITIAL_OWNER) != 0, lpName);
}

BOOL WINAPI ReleaseMutex(HANDLE hMutex) {
    Pinned p;
    if (!pin_kind(hMutex, WKind::Mutex, &p)) return FALSE;
    gta6rt_status st = gta6rt_mutex_release(p.w);
    p.drop();
    if (st != GTA6RT_OK) {
        set_error(ERROR_NOT_OWNER);
        return FALSE;
    }
    return TRUE;
}

HANDLE WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES, LONG lInitialCount, LONG lMaximumCount, LPCWSTR lpName) {
    gta6rt_waitable* w = nullptr;
    if (gta6rt_semaphore_create(lInitialCount, lMaximumCount, &w) != GTA6RT_OK) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    return create_named(w, lpName);
}

HANDLE WINAPI CreateSemaphoreExW(LPSECURITY_ATTRIBUTES a, LONG lInitialCount, LONG lMaximumCount, LPCWSTR lpName,
                                 DWORD, DWORD) {
    return CreateSemaphoreW(a, lInitialCount, lMaximumCount, lpName);
}

BOOL WINAPI ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, LPLONG lpPreviousCount) {
    Pinned p;
    if (!pin_kind(hSemaphore, WKind::Semaphore, &p)) return FALSE;
    int32_t prev = 0;
    gta6rt_status st = gta6rt_semaphore_release(p.w, lReleaseCount, &prev);
    p.drop();
    if (lpPreviousCount) *lpPreviousCount = prev;
    if (st != GTA6RT_OK) {
        set_error(st == GTA6RT_E_BUSY ? ERROR_TOO_MANY_POSTS : ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

HANDLE WINAPI CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES, LPCWSTR lpTimerName, DWORD dwFlags, DWORD) {
    gta6rt_waitable* w = make(WKind::Timer);
    w->manual = (dwFlags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0;
    return create_named(w, lpTimerName);
}

HANDLE WINAPI CreateWaitableTimerW(LPSECURITY_ATTRIBUTES a, BOOL bManualReset, LPCWSTR lpTimerName) {
    return CreateWaitableTimerExW(a, lpTimerName, bManualReset ? CREATE_WAITABLE_TIMER_MANUAL_RESET : 0, 0);
}

BOOL WINAPI SetWaitableTimer(HANDLE hTimer, const LARGE_INTEGER* lpDueTime, LONG lPeriod,
                             PTIMERAPCROUTINE pfnCompletionRoutine, LPVOID lpArgToCompletionRoutine, BOOL) {
    if (!lpDueTime || lPeriod < 0) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    gta6rt_waitable* w = ref_kind(hTimer, WKind::Timer);
    if (!w) return FALSE;
    int64_t due = lpDueTime->QuadPart;
    uint64_t now = monotonic_ns();
    uint64_t at;
    if (due < 0) {
        at = now + (uint64_t)(-due) * 100;
    } else {
        int64_t delta = due - (int64_t)filetime_now();
        at = delta > 0 ? now + (uint64_t)delta * 100 : now;
    }
    gta6rt_thread* self = pfnCompletionRoutine ? gta6rt_thread_current() : nullptr;
    if (self) thread_retain(self);
    pthread_mutex_lock(&g_wait_mx);
    timer_disarm_locked(w);
    gta6rt_thread* old = w->apc_thread;
    w->signaled.store(false);
    w->due = at;
    w->period_ns = (uint64_t)lPeriod * 1000000ull;
    w->apc_fn = (gta6rt_gva)(uintptr_t)pfnCompletionRoutine;
    w->apc_arg = (uint64_t)(uintptr_t)lpArgToCompletionRoutine;
    w->apc_thread = self;
    timer_arm_locked(w);
    pthread_mutex_unlock(&g_wait_mx);
    if (old) gta6rt_thread_release(old);
    gta6rt_waitable_release(w);
    return TRUE;
}

BOOL WINAPI CancelWaitableTimer(HANDLE hTimer) {
    gta6rt_waitable* w = ref_kind(hTimer, WKind::Timer);
    if (!w) return FALSE;
    pthread_mutex_lock(&g_wait_mx);
    timer_disarm_locked(w);
    pthread_mutex_unlock(&g_wait_mx);
    gta6rt_waitable_release(w);
    return TRUE;
}

DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds) {
    return wait_handles(1, &hHandle, FALSE, dwMilliseconds, FALSE);
}

DWORD WINAPI WaitForSingleObjectEx(HANDLE hHandle, DWORD dwMilliseconds, BOOL bAlertable) {
    return wait_handles(1, &hHandle, FALSE, dwMilliseconds, bAlertable);
}

DWORD WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds) {
    return wait_handles(nCount, lpHandles, bWaitAll, dwMilliseconds, FALSE);
}

DWORD WINAPI WaitForMultipleObjectsEx(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds,
                                      BOOL bAlertable) {
    return wait_handles(nCount, lpHandles, bWaitAll, dwMilliseconds, bAlertable);
}

DWORD WINAPI SignalObjectAndWait(HANDLE hObjectToSignal, HANDLE hObjectToWaitOn, DWORD dwMilliseconds,
                                 BOOL bAlertable) {
    void (*rel)(void*) = nullptr;
    gta6rt_waitable* s = (gta6rt_waitable*)handle_ref(H(hObjectToSignal), GTA6RT_HANDLE_WAITABLE, &rel);
    if (!s) {
        set_error(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    gta6rt_status st = GTA6RT_OK;
    if (s->kind == WKind::Event) st = gta6rt_event_set(s);
    else if (s->kind == WKind::Semaphore) st = gta6rt_semaphore_release(s, 1, nullptr);
    else if (s->kind == WKind::Mutex) st = gta6rt_mutex_release(s);
    else st = GTA6RT_E_INVALID_ARG;
    gta6rt_waitable_release(s);
    if (st != GTA6RT_OK) {
        set_error(st == GTA6RT_E_ACCESS ? ERROR_NOT_OWNER : ERROR_TOO_MANY_POSTS);
        return WAIT_FAILED;
    }
    return wait_handles(1, &hObjectToWaitOn, FALSE, dwMilliseconds, bAlertable);
}

DWORD WINAPI SleepEx(DWORD dwMilliseconds, BOOL bAlertable) {
    if (!bAlertable) {
        gta6rt_sleep_ns(ms_to_ns(dwMilliseconds));
        return 0;
    }
    static gta6rt_waitable* never = waitable_new_event(true, false);
    DWORD r = wait_win32(&never, 1, false, ms_to_ns(dwMilliseconds), true);
    return r == WAIT_IO_COMPLETION ? WAIT_IO_COMPLETION : 0;
}

void WINAPI Sleep(DWORD dwMilliseconds) { gta6rt_sleep_ns(ms_to_ns(dwMilliseconds)); }

} // extern "C"
