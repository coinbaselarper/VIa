#include "internal.h"

#include <deque>
#include <map>
#include <vector>

using namespace gta6rt_os;

namespace {
enum class TpKind { Work, Timer, Wait, Simple };
}

struct TpObj {
    TpKind kind;
    gta6rt_gva cb = 0;
    void* ctx = nullptr;
    int refs = 1;
    int pending = 0;
    int running = 0;
    bool closed = false;
};

struct _TP_WORK : TpObj {};
struct _TP_TIMER : TpObj {
    uint64_t due = 0;
    uint64_t period_ns = 0;
    bool armed = false;
};
struct _TP_WAIT : TpObj {
    gta6rt_waitable* w = nullptr;
    uint64_t deadline = UINT64_MAX;
    bool armed = false;
};
struct _TP_POOL {
    int dummy;
};

struct _TP_CALLBACK_INSTANCE {
    TpObj* obj;
    bool disassociated;
    HANDLE set_event;
    HANDLE release_sem;
    LONG sem_count;
    HANDLE release_mutex;
    PCRITICAL_SECTION leave_cs;
};

namespace {

struct Item {
    TpObj* obj;
    gta6rt_gva simple_cb;
    void* simple_ctx;
    DWORD wait_result;
};

pthread_mutex_t g_tp_mx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_tp_cv = PTHREAD_COND_INITIALIZER;
pthread_cond_t g_tp_done_cv = PTHREAD_COND_INITIALIZER;
pthread_cond_t g_tp_timer_cv = PTHREAD_COND_INITIALIZER;
std::deque<Item>* g_q;
int g_workers, g_idle;
bool g_timer_started, g_wait_started;
std::multimap<uint64_t, _TP_TIMER*>* g_tp_timers;
std::vector<_TP_WAIT*>* g_waits;
gta6rt_waitable* g_wait_ctl;

void unref_locked(TpObj* o) {
    if (--o->refs == 0) {
        if (o->kind == TpKind::Wait && ((_TP_WAIT*)o)->w) gta6rt_waitable_release(((_TP_WAIT*)o)->w);
        switch (o->kind) {
        case TpKind::Work: delete (_TP_WORK*)o; break;
        case TpKind::Timer: delete (_TP_TIMER*)o; break;
        case TpKind::Wait: delete (_TP_WAIT*)o; break;
        default: delete o;
        }
    }
}

void worker(void*) {
    gta6rt_thread_set_name(gta6rt_thread_current(), "gta6rt-tp");
    for (;;) {
        pthread_mutex_lock(&g_tp_mx);
        while (g_q->empty()) {
            g_idle++;
            pthread_cond_wait(&g_tp_cv, &g_tp_mx);
            g_idle--;
        }
        Item it = g_q->front();
        g_q->pop_front();
        if (it.obj) {
            it.obj->pending--;
            it.obj->running++;
        }
        pthread_mutex_unlock(&g_tp_mx);
        _TP_CALLBACK_INSTANCE inst = {};
        inst.obj = it.obj;
        uint64_t ip = (uint64_t)(uintptr_t)&inst;
        if (!it.obj) {
            uint64_t a[2] = {ip, (uint64_t)(uintptr_t)it.simple_ctx};
            gta6rt_os_invoke(it.simple_cb, a, 2);
        } else {
            uint64_t a[4] = {ip, (uint64_t)(uintptr_t)it.obj->ctx, (uint64_t)(uintptr_t)it.obj, it.wait_result};
            gta6rt_os_invoke(it.obj->cb, a, it.obj->kind == TpKind::Wait ? 4 : 3);
        }
        if (inst.leave_cs) LeaveCriticalSection(inst.leave_cs);
        if (inst.release_mutex) ReleaseMutex(inst.release_mutex);
        if (inst.release_sem) ReleaseSemaphore(inst.release_sem, inst.sem_count, nullptr);
        if (inst.set_event) SetEvent(inst.set_event);
        pthread_mutex_lock(&g_tp_mx);
        if (it.obj) {
            if (!inst.disassociated) it.obj->running--;
            unref_locked(it.obj);
        }
        pthread_cond_broadcast(&g_tp_done_cv);
        pthread_mutex_unlock(&g_tp_mx);
    }
}

void enqueue_locked(const Item& it) {
    if (!g_q) g_q = new std::deque<Item>();
    if (it.obj) {
        it.obj->pending++;
        it.obj->refs++;
    }
    g_q->push_back(it);
    int max = (int)num_cpus() * 2;
    if (max < 16) max = 16;
    if (g_idle == 0 && g_workers < max) {
        gta6rt_thread_desc d = {};
        d.fn = worker;
        d.name = "gta6rt-tp";
        d.guest = 1;
        d.stack_size = 1u << 20;
        if (gta6rt_thread_create(&d, nullptr) == GTA6RT_OK) g_workers++;
    }
    pthread_cond_signal(&g_tp_cv);
}

void cancel_queued_locked(TpObj* o) {
    if (!g_q) return;
    for (auto it = g_q->begin(); it != g_q->end();) {
        if (it->obj == o) {
            o->pending--;
            unref_locked(o);
            it = g_q->erase(it);
        } else {
            ++it;
        }
    }
}

void wait_callbacks_locked(TpObj* o, bool cancel) {
    if (cancel) cancel_queued_locked(o);
    while (o->pending + o->running > 0) pthread_cond_wait(&g_tp_done_cv, &g_tp_mx);
}

uint64_t due_from_filetime(const FILETIME* ft) {
    int64_t v = (int64_t)(((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime);
    uint64_t now = monotonic_ns();
    if (v < 0) return now + (uint64_t)(-v) * 100;
    if (v == 0) return now;
    int64_t delta = v - (int64_t)filetime_now();
    return delta > 0 ? now + (uint64_t)delta * 100 : now;
}

void timer_disarm_locked(_TP_TIMER* t) {
    if (!t->armed) return;
    for (auto it = g_tp_timers->begin(); it != g_tp_timers->end(); ++it)
        if (it->second == t) {
            g_tp_timers->erase(it);
            break;
        }
    t->armed = false;
    unref_locked(t);
}

void* timer_main(void*) {
    pthread_setname_np("gta6rt-tp-timer");
    pthread_mutex_lock(&g_tp_mx);
    for (;;) {
        uint64_t now = monotonic_ns();
        while (!g_tp_timers->empty() && g_tp_timers->begin()->first <= now) {
            _TP_TIMER* t = g_tp_timers->begin()->second;
            g_tp_timers->erase(g_tp_timers->begin());
            t->armed = false;
            if (!t->closed) enqueue_locked(Item{t, 0, nullptr, 0});
            if (t->period_ns && !t->closed) {
                t->due = now + t->period_ns;
                t->armed = true;
                g_tp_timers->emplace(t->due, t);
            } else {
                unref_locked(t);
            }
        }
        if (g_tp_timers->empty()) {
            pthread_cond_wait(&g_tp_timer_cv, &g_tp_mx);
        } else {
            uint64_t d = g_tp_timers->begin()->first - now;
            struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
            pthread_cond_timedwait_relative_np(&g_tp_timer_cv, &g_tp_mx, &ts);
        }
    }
    return nullptr;
}

void* wait_main(void*) {
    pthread_setname_np("gta6rt-tp-wait");
    for (;;) {
        std::vector<_TP_WAIT*> set;
        std::vector<gta6rt_waitable*> objs;
        uint64_t deadline = UINT64_MAX;
        pthread_mutex_lock(&g_tp_mx);
        objs.push_back(g_wait_ctl);
        for (_TP_WAIT* w : *g_waits) {
            if (objs.size() >= MAXIMUM_WAIT_OBJECTS) break;
            w->refs++;
            set.push_back(w);
            objs.push_back(w->w);
            if (w->deadline < deadline) deadline = w->deadline;
        }
        pthread_mutex_unlock(&g_tp_mx);
        uint64_t now = monotonic_ns();
        uint64_t timeout = deadline == UINT64_MAX ? GTA6RT_WAIT_INFINITE : (deadline > now ? deadline - now : 0);
        uint32_t idx = 0;
        gta6rt_status st = gta6rt_wait_any(objs.data(), (uint32_t)objs.size(), timeout, &idx);
        pthread_mutex_lock(&g_tp_mx);
        now = monotonic_ns();
        for (size_t i = 0; i < set.size(); i++) {
            _TP_WAIT* w = set[i];
            bool fired = (st >= 0 || st == GTA6RT_E_ABANDONED) && idx == i + 1;
            bool timed = !fired && w->deadline <= now;
            if (w->armed && (fired || timed)) {
                w->armed = false;
                for (auto it = g_waits->begin(); it != g_waits->end(); ++it)
                    if (*it == w) {
                        g_waits->erase(it);
                        break;
                    }
                if (!w->closed) enqueue_locked(Item{w, 0, nullptr, fired ? (DWORD)WAIT_OBJECT_0 : (DWORD)WAIT_TIMEOUT});
                unref_locked(w);
            }
            unref_locked(w);
        }
        pthread_mutex_unlock(&g_tp_mx);
    }
    return nullptr;
}

void start_service(bool* started, void* (*fn)(void*)) {
    if (*started) return;
    *started = true;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, fn, nullptr);
    pthread_attr_destroy(&at);
}

void wait_disarm_locked(_TP_WAIT* w) {
    if (!w->armed) return;
    w->armed = false;
    for (auto it = g_waits->begin(); it != g_waits->end(); ++it)
        if (*it == w) {
            g_waits->erase(it);
            break;
        }
    unref_locked(w);
    gta6rt_event_set(g_wait_ctl);
}

} // namespace

extern "C" {

PTP_WORK WINAPI CreateThreadpoolWork(PTP_WORK_CALLBACK pfnwk, PVOID pv, PTP_CALLBACK_ENVIRON) {
    _TP_WORK* w = new _TP_WORK();
    w->kind = TpKind::Work;
    w->cb = (gta6rt_gva)(uintptr_t)pfnwk;
    w->ctx = pv;
    return w;
}

void WINAPI SubmitThreadpoolWork(PTP_WORK pwk) {
    pthread_mutex_lock(&g_tp_mx);
    enqueue_locked(Item{pwk, 0, nullptr, 0});
    pthread_mutex_unlock(&g_tp_mx);
}

void WINAPI WaitForThreadpoolWorkCallbacks(PTP_WORK pwk, BOOL fCancelPendingCallbacks) {
    pthread_mutex_lock(&g_tp_mx);
    wait_callbacks_locked(pwk, fCancelPendingCallbacks != 0);
    pthread_mutex_unlock(&g_tp_mx);
}

void WINAPI CloseThreadpoolWork(PTP_WORK pwk) {
    pthread_mutex_lock(&g_tp_mx);
    pwk->closed = true;
    unref_locked(pwk);
    pthread_mutex_unlock(&g_tp_mx);
}

BOOL WINAPI TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK pfns, PVOID pv, PTP_CALLBACK_ENVIRON) {
    pthread_mutex_lock(&g_tp_mx);
    enqueue_locked(Item{nullptr, (gta6rt_gva)(uintptr_t)pfns, pv, 0});
    pthread_mutex_unlock(&g_tp_mx);
    return TRUE;
}

PTP_TIMER WINAPI CreateThreadpoolTimer(PTP_TIMER_CALLBACK pfnti, PVOID pv, PTP_CALLBACK_ENVIRON) {
    _TP_TIMER* t = new _TP_TIMER();
    t->kind = TpKind::Timer;
    t->cb = (gta6rt_gva)(uintptr_t)pfnti;
    t->ctx = pv;
    return t;
}

void WINAPI SetThreadpoolTimer(PTP_TIMER pti, PFILETIME pftDueTime, DWORD msPeriod, DWORD) {
    pthread_mutex_lock(&g_tp_mx);
    if (!g_tp_timers) g_tp_timers = new std::multimap<uint64_t, _TP_TIMER*>();
    timer_disarm_locked(pti);
    if (pftDueTime) {
        pti->due = due_from_filetime(pftDueTime);
        pti->period_ns = (uint64_t)msPeriod * 1000000ull;
        pti->armed = true;
        pti->refs++;
        g_tp_timers->emplace(pti->due, pti);
        start_service(&g_timer_started, timer_main);
        pthread_cond_signal(&g_tp_timer_cv);
    }
    pthread_mutex_unlock(&g_tp_mx);
}

BOOL WINAPI IsThreadpoolTimerSet(PTP_TIMER pti) {
    pthread_mutex_lock(&g_tp_mx);
    bool r = pti->armed;
    pthread_mutex_unlock(&g_tp_mx);
    return r;
}

void WINAPI WaitForThreadpoolTimerCallbacks(PTP_TIMER pti, BOOL fCancelPendingCallbacks) {
    pthread_mutex_lock(&g_tp_mx);
    if (fCancelPendingCallbacks && g_tp_timers) timer_disarm_locked(pti);
    wait_callbacks_locked(pti, fCancelPendingCallbacks != 0);
    pthread_mutex_unlock(&g_tp_mx);
}

void WINAPI CloseThreadpoolTimer(PTP_TIMER pti) {
    pthread_mutex_lock(&g_tp_mx);
    pti->closed = true;
    if (g_tp_timers) timer_disarm_locked(pti);
    unref_locked(pti);
    pthread_mutex_unlock(&g_tp_mx);
}

PTP_WAIT WINAPI CreateThreadpoolWait(PTP_WAIT_CALLBACK pfnwa, PVOID pv, PTP_CALLBACK_ENVIRON) {
    _TP_WAIT* w = new _TP_WAIT();
    w->kind = TpKind::Wait;
    w->cb = (gta6rt_gva)(uintptr_t)pfnwa;
    w->ctx = pv;
    return w;
}

void WINAPI SetThreadpoolWait(PTP_WAIT pwa, HANDLE h, PFILETIME pftTimeout) {
    gta6rt_waitable* obj = h ? handle_to_waitable(h) : nullptr;
    pthread_mutex_lock(&g_tp_mx);
    if (!g_waits) {
        g_waits = new std::vector<_TP_WAIT*>();
        g_wait_ctl = waitable_new_event(false, false);
    }
    wait_disarm_locked(pwa);
    if (pwa->w) gta6rt_waitable_release(pwa->w);
    pwa->w = obj;
    if (obj) {
        pwa->deadline = pftTimeout ? due_from_filetime(pftTimeout) : UINT64_MAX;
        pwa->armed = true;
        pwa->refs++;
        g_waits->push_back(pwa);
        start_service(&g_wait_started, wait_main);
        gta6rt_event_set(g_wait_ctl);
    }
    pthread_mutex_unlock(&g_tp_mx);
}

void WINAPI WaitForThreadpoolWaitCallbacks(PTP_WAIT pwa, BOOL fCancelPendingCallbacks) {
    pthread_mutex_lock(&g_tp_mx);
    if (fCancelPendingCallbacks && g_waits) wait_disarm_locked(pwa);
    wait_callbacks_locked(pwa, fCancelPendingCallbacks != 0);
    pthread_mutex_unlock(&g_tp_mx);
}

void WINAPI CloseThreadpoolWait(PTP_WAIT pwa) {
    pthread_mutex_lock(&g_tp_mx);
    pwa->closed = true;
    if (g_waits) wait_disarm_locked(pwa);
    unref_locked(pwa);
    pthread_mutex_unlock(&g_tp_mx);
}

PTP_POOL WINAPI CreateThreadpool(PVOID) { return new _TP_POOL(); }
void WINAPI CloseThreadpool(PTP_POOL p) { delete p; }
BOOL WINAPI SetThreadpoolThreadMinimum(PTP_POOL, DWORD) { return TRUE; }
void WINAPI SetThreadpoolThreadMaximum(PTP_POOL, DWORD) {}

void WINAPI DisassociateCurrentThreadFromCallback(PTP_CALLBACK_INSTANCE pci) {
    if (!pci || pci->disassociated) return;
    pthread_mutex_lock(&g_tp_mx);
    pci->disassociated = true;
    if (pci->obj) pci->obj->running--;
    pthread_cond_broadcast(&g_tp_done_cv);
    pthread_mutex_unlock(&g_tp_mx);
}

void WINAPI FreeLibraryWhenCallbackReturns(PTP_CALLBACK_INSTANCE, HMODULE) {}
void WINAPI SetEventWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci, HANDLE evt) { pci->set_event = evt; }
void WINAPI ReleaseSemaphoreWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci, HANDLE sem, DWORD crel) {
    pci->release_sem = sem;
    pci->sem_count = (LONG)crel;
}
void WINAPI ReleaseMutexWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci, HANDLE mut) { pci->release_mutex = mut; }
void WINAPI LeaveCriticalSectionWhenCallbackReturns(PTP_CALLBACK_INSTANCE pci, PCRITICAL_SECTION pcs) {
    pci->leave_cs = pcs;
}

} // extern "C"
