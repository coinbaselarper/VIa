#include "t.h"

#include <atomic>
#include <vector>

static HANDLE spawn(LPTHREAD_START_ROUTINE fn, void* arg) { return CreateThread(nullptr, 0, fn, arg, 0, nullptr); }

static void join(HANDLE h) {
    CHECK_EQ(WaitForSingleObject(h, 20000), WAIT_OBJECT_0);
    CloseHandle(h);
}

static void test_events() {
    SECTION("events");
    HANDLE m = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE a = CreateEventW(nullptr, FALSE, TRUE, nullptr);
    CHECK(m && a);
    CHECK_EQ(WaitForSingleObject(m, 0), WAIT_TIMEOUT);
    CHECK(SetEvent(m));
    CHECK_EQ(WaitForSingleObject(m, 0), WAIT_OBJECT_0);
    CHECK_EQ(WaitForSingleObject(m, 0), WAIT_OBJECT_0);
    CHECK(ResetEvent(m));
    CHECK_EQ(WaitForSingleObject(m, 0), WAIT_TIMEOUT);
    CHECK_EQ(WaitForSingleObject(a, 0), WAIT_OBJECT_0);
    CHECK_EQ(WaitForSingleObject(a, 0), WAIT_TIMEOUT);
    uint64_t t0 = gta6rt_time_monotonic_ns();
    CHECK_EQ(WaitForSingleObject(a, 60), WAIT_TIMEOUT);
    double ms = t_ms_since(t0);
    CHECK(ms >= 55 && ms < 2000);
    CHECK(SetEvent(a) && SetEvent(a));
    CHECK_EQ(WaitForSingleObject(a, 0), WAIT_OBJECT_0);
    CHECK_EQ(WaitForSingleObject(a, 0), WAIT_TIMEOUT);

    HANDLE n1 = CreateEventA(nullptr, TRUE, FALSE, "gta6rt-test-ev");
    CHECK_EQ(GetLastError(), ERROR_SUCCESS);
    HANDLE n2 = CreateEventW(nullptr, TRUE, FALSE, u"gta6rt-test-ev");
    CHECK_EQ(GetLastError(), ERROR_ALREADY_EXISTS);
    HANDLE n3 = OpenEventW(0, FALSE, u"gta6rt-test-ev");
    CHECK(n1 && n2 && n3 && n1 != n2);
    SetEvent(n2);
    CHECK_EQ(WaitForSingleObject(n3, 0), WAIT_OBJECT_0);
    CHECK(CreateSemaphoreW(nullptr, 0, 1, u"gta6rt-test-ev") == nullptr);
    CloseHandle(n1);
    CloseHandle(n2);
    CloseHandle(n3);
    CHECK(OpenEventW(0, FALSE, u"gta6rt-test-ev") == nullptr);

    HANDLE d = nullptr;
    CHECK(DuplicateHandle(GetCurrentProcess(), m, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS));
    CHECK(CompareObjectHandles(m, d));
    CloseHandle(m);
    CHECK(SetEvent(d));
    CHECK_EQ(WaitForSingleObject(d, 0), WAIT_OBJECT_0);
    CloseHandle(d);
    CloseHandle(a);
    CHECK(CloseHandle(a) == FALSE);
    CHECK_EQ(GetLastError(), ERROR_INVALID_HANDLE);
    CHECK_EQ(WaitForSingleObject((HANDLE)0x7777770, 0), WAIT_FAILED);
    CHECK_EQ(GetLastError(), ERROR_INVALID_HANDLE);
    CHECK(((uintptr_t)d & 3) == 0 && (uintptr_t)d >= 0x1000 && (uintptr_t)d < 0x100000);
}

static void test_semaphore_mutex() {
    SECTION("semaphores and mutexes");
    HANDLE s = CreateSemaphoreExW(nullptr, 2, 3, nullptr, 0, 0);
    CHECK(s);
    CHECK_EQ(WaitForSingleObject(s, 0), WAIT_OBJECT_0);
    CHECK_EQ(WaitForSingleObject(s, 0), WAIT_OBJECT_0);
    CHECK_EQ(WaitForSingleObject(s, 0), WAIT_TIMEOUT);
    LONG prev = -1;
    CHECK(ReleaseSemaphore(s, 3, &prev));
    CHECK_EQ(prev, 0);
    CHECK(ReleaseSemaphore(s, 1, &prev) == FALSE);
    CHECK_EQ(GetLastError(), ERROR_TOO_MANY_POSTS);
    CHECK_EQ(prev, 3);
    CHECK(CreateSemaphoreW(nullptr, 5, 3, nullptr) == nullptr);
    CloseHandle(s);

    HANDLE m = CreateMutexA(nullptr, TRUE, nullptr);
    CHECK_EQ(WaitForSingleObject(m, 0), WAIT_OBJECT_0);
    CHECK(ReleaseMutex(m));
    CHECK(ReleaseMutex(m));
    CHECK(ReleaseMutex(m) == FALSE);
    CHECK_EQ(GetLastError(), ERROR_NOT_OWNER);

    struct Ctx {
        HANDLE m;
        HANDLE ready;
    } c{m, CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE t = spawn(
        [](LPVOID p) GTA6RT_GUEST_ABI -> DWORD {
            Ctx* c = (Ctx*)p;
            WaitForSingleObject(c->m, INFINITE);
            SetEvent(c->ready);
            return 0;
        },
        &c);
    (void)t;
    CHECK_EQ(WaitForSingleObject(c.ready, 10000), WAIT_OBJECT_0);
    CHECK(ReleaseMutex(m) == FALSE);
    join(t);
    CHECK_EQ(WaitForSingleObject(m, 5000), WAIT_ABANDONED_0);
    CHECK_EQ(WaitForSingleObject(m, 0), WAIT_OBJECT_0);
    CHECK(ReleaseMutex(m) && ReleaseMutex(m));
    CloseHandle(c.ready);
    CloseHandle(m);
}

static void test_wait_multiple() {
    SECTION("WaitForMultipleObjects");
    HANDLE e[3];
    for (auto& h : e) h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK_EQ(WaitForMultipleObjects(3, e, FALSE, 0), WAIT_TIMEOUT);
    SetEvent(e[2]);
    CHECK_EQ(WaitForMultipleObjects(3, e, FALSE, 0), WAIT_OBJECT_0 + 2);
    SetEvent(e[1]);
    CHECK_EQ(WaitForMultipleObjects(3, e, FALSE, 0), WAIT_OBJECT_0 + 1);
    CHECK_EQ(WaitForMultipleObjects(3, e, TRUE, 0), WAIT_TIMEOUT);
    HANDLE th = spawn(
        [](LPVOID p) GTA6RT_GUEST_ABI -> DWORD {
            Sleep(30);
            SetEvent((HANDLE)p);
            return 0;
        },
        e[0]);
    CHECK_EQ(WaitForMultipleObjects(3, e, TRUE, 10000), WAIT_OBJECT_0);
    join(th);
    HANDLE dup[2] = {e[0], e[0]};
    CHECK_EQ(WaitForMultipleObjects(2, dup, TRUE, 0), WAIT_FAILED);
    CHECK_EQ(WaitForMultipleObjects(0, e, FALSE, 0), WAIT_FAILED);

    HANDLE a = CreateEventW(nullptr, FALSE, TRUE, nullptr), b = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE ab[2] = {a, b};
    CHECK_EQ(WaitForMultipleObjects(2, ab, TRUE, 0), WAIT_TIMEOUT);
    CHECK_EQ(WaitForSingleObject(a, 0), WAIT_OBJECT_0);
    CloseHandle(a);
    CloseHandle(b);
    for (auto& h : e) CloseHandle(h);

    gta6rt_waitable *w1, *w2;
    gta6rt_event_create(0, 0, &w1);
    gta6rt_semaphore_create(1, 1, &w2);
    gta6rt_waitable* ws[2] = {w1, w2};
    uint32_t idx = 9;
    CHECK_EQ(gta6rt_wait_any(ws, 2, 0, &idx), 1);
    CHECK_EQ(idx, 1);
    CHECK_EQ(gta6rt_wait_any(ws, 2, 1000000, &idx), GTA6RT_E_TIMEOUT);
    gta6rt_event_set(w1);
    gta6rt_semaphore_release(w2, 1, nullptr);
    CHECK_EQ(gta6rt_wait_all(ws, 2, 0), GTA6RT_OK);
    gta6rt_waitable_release(w1);
    gta6rt_waitable_release(w2);
}

struct Shared {
    SRWLOCK srw = SRWLOCK_INIT;
    CRITICAL_SECTION cs;
    long counter = 0;
    std::atomic<int> readers{0}, max_readers{0};
};

static DWORD WINAPI srw_worker(LPVOID p) {
    Shared* s = (Shared*)p;
    for (int i = 0; i < 20000; i++) {
        AcquireSRWLockExclusive(&s->srw);
        s->counter++;
        ReleaseSRWLockExclusive(&s->srw);
        if ((i & 63) == 0) {
            AcquireSRWLockShared(&s->srw);
            int r = ++s->readers;
            int m = s->max_readers.load();
            while (r > m && !s->max_readers.compare_exchange_weak(m, r)) {
            }
            for (int k = 0; k < 200; k++) __asm__ volatile("" ::: "memory");
            --s->readers;
            ReleaseSRWLockShared(&s->srw);
        }
    }
    return 0;
}

static DWORD WINAPI cs_worker(LPVOID p) {
    Shared* s = (Shared*)p;
    for (int i = 0; i < 20000; i++) {
        EnterCriticalSection(&s->cs);
        EnterCriticalSection(&s->cs);
        s->counter++;
        LeaveCriticalSection(&s->cs);
        LeaveCriticalSection(&s->cs);
    }
    return 0;
}

static void test_locks() {
    SECTION("SRW locks and critical sections");
    Shared s;
    const int n = 6;
    HANDLE th[n];
    for (auto& h : th) h = spawn(srw_worker, &s);
    CHECK_EQ(WaitForMultipleObjects(n, th, TRUE, 60000), WAIT_OBJECT_0);
    for (auto& h : th) CloseHandle(h);
    CHECK_EQ(s.counter, n * 20000);
    CHECK(s.srw.Ptr == nullptr);
    CHECK(TryAcquireSRWLockExclusive(&s.srw));
    CHECK(!TryAcquireSRWLockShared(&s.srw));
    ReleaseSRWLockExclusive(&s.srw);
    CHECK(TryAcquireSRWLockShared(&s.srw));
    CHECK(TryAcquireSRWLockShared(&s.srw));
    CHECK(!TryAcquireSRWLockExclusive(&s.srw));
    ReleaseSRWLockShared(&s.srw);
    ReleaseSRWLockShared(&s.srw);

    InitializeCriticalSectionAndSpinCount(&s.cs, 4000);
    s.counter = 0;
    for (auto& h : th) h = spawn(cs_worker, &s);
    CHECK_EQ(WaitForMultipleObjects(n, th, TRUE, 60000), WAIT_OBJECT_0);
    for (auto& h : th) CloseHandle(h);
    CHECK_EQ(s.counter, n * 20000);
    CHECK(TryEnterCriticalSection(&s.cs));
    CHECK_EQ((uintptr_t)s.cs.OwningThread, GetCurrentThreadId());
    CHECK_EQ(s.cs.RecursionCount, 1);
    LeaveCriticalSection(&s.cs);
    CHECK(s.cs.OwningThread == nullptr);
    DeleteCriticalSection(&s.cs);
}

struct Queue {
    CRITICAL_SECTION cs;
    SRWLOCK srw = SRWLOCK_INIT;
    CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
    int items = 0, consumed = 0;
    bool use_srw = false;
};

static DWORD WINAPI consumer(LPVOID p) {
    Queue* q = (Queue*)p;
    for (;;) {
        if (q->use_srw) AcquireSRWLockExclusive(&q->srw);
        else EnterCriticalSection(&q->cs);
        while (q->items == 0) {
            if (q->use_srw) SleepConditionVariableSRW(&q->cv, &q->srw, INFINITE, 0);
            else SleepConditionVariableCS(&q->cv, &q->cs, INFINITE);
        }
        bool stop = q->items < 0;
        if (!stop) {
            q->items--;
            q->consumed++;
        }
        if (q->use_srw) ReleaseSRWLockExclusive(&q->srw);
        else LeaveCriticalSection(&q->cs);
        if (stop) return 0;
    }
}

static void run_cv(bool srw) {
    Queue q;
    q.use_srw = srw;
    InitializeCriticalSection(&q.cs);
    const int nc = 4, total = 20000;
    HANDLE th[nc];
    for (auto& h : th) h = spawn(consumer, &q);
    for (int i = 0; i < total; i++) {
        if (srw) AcquireSRWLockExclusive(&q.srw);
        else EnterCriticalSection(&q.cs);
        q.items++;
        if (srw) ReleaseSRWLockExclusive(&q.srw);
        else LeaveCriticalSection(&q.cs);
        WakeConditionVariable(&q.cv);
    }
    for (;;) {
        if (srw) AcquireSRWLockExclusive(&q.srw);
        else EnterCriticalSection(&q.cs);
        bool done = q.consumed == total;
        if (done) q.items = -1;
        if (srw) ReleaseSRWLockExclusive(&q.srw);
        else LeaveCriticalSection(&q.cs);
        if (done) break;
        Sleep(1);
    }
    WakeAllConditionVariable(&q.cv);
    CHECK_EQ(WaitForMultipleObjects(nc, th, TRUE, 30000), WAIT_OBJECT_0);
    for (auto& h : th) CloseHandle(h);
    CHECK_EQ(q.consumed, total);
}

static void test_condvars() {
    SECTION("condition variables");
    run_cv(false);
    run_cv(true);
    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
    CONDITION_VARIABLE cv;
    InitializeConditionVariable(&cv);
    EnterCriticalSection(&cs);
    EnterCriticalSection(&cs);
    uint64_t t0 = gta6rt_time_monotonic_ns();
    CHECK(SleepConditionVariableCS(&cv, &cs, 40) == FALSE);
    CHECK_EQ(GetLastError(), ERROR_TIMEOUT);
    CHECK(t_ms_since(t0) >= 35);
    CHECK_EQ(cs.RecursionCount, 2);
    LeaveCriticalSection(&cs);
    LeaveCriticalSection(&cs);
    SRWLOCK l = SRWLOCK_INIT;
    AcquireSRWLockShared(&l);
    CHECK(SleepConditionVariableSRW(&cv, &l, 10, CONDITION_VARIABLE_LOCKMODE_SHARED) == FALSE);
    ReleaseSRWLockShared(&l);
    CHECK(l.Ptr == nullptr);
}

static std::atomic<int> g_once_calls;

static BOOL WINAPI once_fn(PINIT_ONCE, PVOID param, PVOID* ctx) {
    g_once_calls++;
    Sleep(20);
    *ctx = param;
    return TRUE;
}

static DWORD WINAPI once_worker(LPVOID p) {
    void* ctx = nullptr;
    InitOnceExecuteOnce((PINIT_ONCE)p, once_fn, (PVOID)0x1230, &ctx);
    return ctx == (void*)0x1230 ? 1 : 0;
}

static void test_init_once() {
    SECTION("InitOnce");
    INIT_ONCE io = INIT_ONCE_STATIC_INIT;
    HANDLE th[8];
    for (auto& h : th) h = spawn(once_worker, &io);
    CHECK_EQ(WaitForMultipleObjects(8, th, TRUE, 30000), WAIT_OBJECT_0);
    for (auto& h : th) {
        DWORD code = 0;
        GetExitCodeThread(h, &code);
        CHECK_EQ(code, 1);
        CloseHandle(h);
    }
    CHECK_EQ(g_once_calls.load(), 1);
    BOOL pending = TRUE;
    void* ctx = nullptr;
    CHECK(InitOnceBeginInitialize(&io, INIT_ONCE_CHECK_ONLY, &pending, &ctx));
    CHECK(!pending && ctx == (void*)0x1230);
    INIT_ONCE io2;
    InitOnceInitialize(&io2);
    CHECK(InitOnceBeginInitialize(&io2, 0, &pending, &ctx) && pending);
    CHECK(InitOnceComplete(&io2, INIT_ONCE_INIT_FAILED, nullptr));
    CHECK(InitOnceBeginInitialize(&io2, 0, &pending, &ctx) && pending);
    CHECK(InitOnceComplete(&io2, 0, (void*)0x40));
    CHECK(InitOnceBeginInitialize(&io2, 0, &pending, &ctx) && !pending && ctx == (void*)0x40);
}

static volatile uint32_t g_word;
static volatile uint8_t g_byte;

static void test_wait_on_address() {
    SECTION("WaitOnAddress");
    g_word = 0;
    uint32_t cmp = 0;
    uint64_t t0 = gta6rt_time_monotonic_ns();
    CHECK(WaitOnAddress(&g_word, &cmp, 4, 30) == FALSE);
    CHECK_EQ(GetLastError(), ERROR_TIMEOUT);
    CHECK(t_ms_since(t0) >= 25);
    cmp = 5;
    CHECK(WaitOnAddress(&g_word, &cmp, 4, INFINITE));
    HANDLE th = spawn(
        [](LPVOID) GTA6RT_GUEST_ABI -> DWORD {
            Sleep(20);
            g_word = 1;
            WakeByAddressSingle((PVOID)&g_word);
            Sleep(20);
            g_byte = 7;
            WakeByAddressAll((PVOID)&g_byte);
            return 0;
        },
        nullptr);
    cmp = 0;
    while (g_word == 0) WaitOnAddress(&g_word, &cmp, 4, 10000);
    CHECK_EQ(g_word, 1);
    uint8_t c8 = 0;
    while (g_byte == 0) WaitOnAddress(&g_byte, &c8, 1, 10000);
    CHECK_EQ(g_byte, 7);
    join(th);
    CHECK(WaitOnAddress(&g_word, &cmp, 3, 0) == FALSE);
    CHECK_EQ(GetLastError(), ERROR_INVALID_PARAMETER);
}

struct alignas(16) Node {
    SLIST_ENTRY e;
    int v;
};

static SLIST_HEADER g_list;

static DWORD WINAPI slist_pusher(LPVOID p) {
    Node* nodes = (Node*)p;
    for (int i = 0; i < 5000; i++) InterlockedPushEntrySList(&g_list, &nodes[i].e);
    return 0;
}

static void test_slist() {
    SECTION("SList");
    InitializeSListHead(&g_list);
    CHECK(InterlockedPopEntrySList(&g_list) == nullptr);
    std::vector<Node> nodes(4 * 5000);
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = spawn(slist_pusher, &nodes[i * 5000]);
    CHECK_EQ(WaitForMultipleObjects(4, th, TRUE, 30000), WAIT_OBJECT_0);
    for (auto& h : th) CloseHandle(h);
    CHECK_EQ(QueryDepthSList(&g_list), 20000);
    int n = 0;
    while (InterlockedPopEntrySList(&g_list)) n++;
    CHECK_EQ(n, 20000);
    InterlockedPushEntrySList(&g_list, &nodes[0].e);
    InterlockedPushEntrySList(&g_list, &nodes[1].e);
    PSLIST_ENTRY f = InterlockedFlushSList(&g_list);
    CHECK(f == &nodes[1].e && f->Next == &nodes[0].e);
    CHECK_EQ(QueryDepthSList(&g_list), 0);
}

static std::atomic<int> g_apc_runs;
static void WINAPI apc_fn(ULONG_PTR d) { g_apc_runs += (int)d; }
static void WINAPI timer_apc(LPVOID arg, DWORD, DWORD) { (*(std::atomic<int>*)arg)++; }

static void test_timers_and_apcs() {
    SECTION("waitable timers and APCs");
    HANDLE t = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    LARGE_INTEGER due;
    due.QuadPart = -40 * 10000;
    uint64_t t0 = gta6rt_time_monotonic_ns();
    CHECK(SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE));
    CHECK_EQ(WaitForSingleObject(t, 0), WAIT_TIMEOUT);
    CHECK_EQ(WaitForSingleObject(t, 5000), WAIT_OBJECT_0);
    double ms = t_ms_since(t0);
    CHECK(ms >= 35 && ms < 3000);
    CHECK_EQ(WaitForSingleObject(t, 0), WAIT_OBJECT_0);
    CloseHandle(t);

    HANDLE p = CreateWaitableTimerExW(nullptr, nullptr, 0, 0);
    due.QuadPart = -10 * 10000;
    CHECK(SetWaitableTimer(p, &due, 15, nullptr, nullptr, FALSE));
    for (int i = 0; i < 3; i++) CHECK_EQ(WaitForSingleObject(p, 5000), WAIT_OBJECT_0);
    CHECK(CancelWaitableTimer(p));
    CloseHandle(p);

    std::atomic<int> fired{0};
    HANDLE a = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    due.QuadPart = -10 * 10000;
    SetWaitableTimer(a, &due, 0, timer_apc, &fired, FALSE);
    DWORD r = SleepEx(5000, TRUE);
    CHECK_EQ(r, WAIT_IO_COMPLETION);
    CHECK_EQ(fired.load(), 1);
    CloseHandle(a);

    CHECK(QueueUserAPC(apc_fn, GetCurrentThread(), 3));
    CHECK_EQ(SleepEx(0, FALSE), 0);
    CHECK_EQ(g_apc_runs.load(), 0);
    CHECK_EQ(SleepEx(1000, TRUE), WAIT_IO_COMPLETION);
    CHECK_EQ(g_apc_runs.load(), 3);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    QueueUserAPC(apc_fn, GetCurrentThread(), 4);
    CHECK_EQ(WaitForSingleObjectEx(ev, 1000, TRUE), WAIT_IO_COMPLETION);
    CHECK_EQ(g_apc_runs.load(), 7);
    CHECK_EQ(WaitForSingleObjectEx(ev, 0, TRUE), WAIT_TIMEOUT);
    CloseHandle(ev);
}

struct StressCtx {
    HANDLE sem, items_done, ping, pong, mx, ev_a, ev_b;
    std::atomic<int> consumed{0}, in_mutex{0}, mutex_overlap{0}, all_hits{0};
    long counter = 0;
    int per_thread;
};

static DWORD WINAPI stress_consumer(LPVOID p) {
    auto* c = (StressCtx*)p;
    for (int i = 0; i < c->per_thread; i++) {
        if (WaitForSingleObject(c->sem, 10000) != WAIT_OBJECT_0) return 1;
        c->consumed++;
    }
    return 0;
}

static DWORD WINAPI stress_mutex(LPVOID p) {
    auto* c = (StressCtx*)p;
    for (int i = 0; i < c->per_thread; i++) {
        if (WaitForSingleObject(c->mx, 10000) != WAIT_OBJECT_0) return 1;
        if (c->in_mutex.fetch_add(1) != 0) c->mutex_overlap++;
        c->counter++;
        c->in_mutex.fetch_sub(1);
        ReleaseMutex(c->mx);
    }
    return 0;
}

static DWORD WINAPI stress_pong(LPVOID p) {
    auto* c = (StressCtx*)p;
    for (int i = 0; i < c->per_thread; i++) {
        if (WaitForSingleObject(c->ping, 10000) != WAIT_OBJECT_0) return 1;
        SetEvent(c->pong);
    }
    return 0;
}

static DWORD WINAPI stress_wait_all(LPVOID p) {
    auto* c = (StressCtx*)p;
    HANDLE hs[2] = {c->ev_a, c->ev_b};
    for (int i = 0; i < c->per_thread; i++) {
        if (WaitForMultipleObjects(2, hs, TRUE, 10000) != WAIT_OBJECT_0) return 1;
        c->all_hits++;
    }
    return 0;
}

static void test_lock_free_paths() {
    SECTION("lock-free wait/signal paths under contention");
    StressCtx c;
    c.per_thread = 20000;
    c.sem = CreateSemaphoreW(nullptr, 0, 0x7fffffff, nullptr);
    HANDLE th[8];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(nullptr, 0, stress_consumer, &c, 0, nullptr);
    for (int i = 0; i < 4 * c.per_thread; i++) ReleaseSemaphore(c.sem, 1, nullptr);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(WaitForSingleObject(th[i], 20000), WAIT_OBJECT_0);
        DWORD code = 9;
        GetExitCodeThread(th[i], &code);
        CHECK_EQ(code, 0);
        CloseHandle(th[i]);
    }
    CHECK_EQ(c.consumed.load(), 4 * c.per_thread);
    CHECK_EQ(WaitForSingleObject(c.sem, 0), WAIT_TIMEOUT);

    c.mx = CreateMutexW(nullptr, FALSE, nullptr);
    for (int i = 0; i < 4; i++) th[i] = CreateThread(nullptr, 0, stress_mutex, &c, 0, nullptr);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(WaitForSingleObject(th[i], 20000), WAIT_OBJECT_0);
        CloseHandle(th[i]);
    }
    CHECK_EQ(c.counter, 4L * c.per_thread);
    CHECK_EQ(c.mutex_overlap.load(), 0);

    c.ping = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    c.pong = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE pt = CreateThread(nullptr, 0, stress_pong, &c, 0, nullptr);
    int rounds = 0;
    for (int i = 0; i < c.per_thread; i++) {
        SetEvent(c.ping);
        if (WaitForSingleObject(c.pong, 10000) != WAIT_OBJECT_0) break;
        rounds++;
    }
    CHECK_EQ(rounds, c.per_thread);
    CHECK_EQ(WaitForSingleObject(pt, 20000), WAIT_OBJECT_0);
    CloseHandle(pt);

    c.ev_a = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    c.ev_b = CreateSemaphoreW(nullptr, 0, 0x7fffffff, nullptr);
    c.per_thread = 5000;
    HANDLE wa = CreateThread(nullptr, 0, stress_wait_all, &c, 0, nullptr);
    int stolen = 0;
    for (int i = 0; i < c.per_thread; i++) {
        ReleaseSemaphore(c.ev_b, 1, nullptr);
        SetEvent(c.ev_a);
        if (WaitForSingleObject(c.ev_a, 0) == WAIT_OBJECT_0) {
            stolen++;
            SetEvent(c.ev_a);
        }
        while (c.all_hits.load() <= i && WaitForSingleObject(wa, 0) == WAIT_TIMEOUT) SwitchToThread();
    }
    CHECK_EQ(WaitForSingleObject(wa, 20000), WAIT_OBJECT_0);
    CHECK_EQ(c.all_hits.load(), c.per_thread);
    CHECK_EQ(WaitForSingleObject(c.ev_b, 0), WAIT_TIMEOUT);
    fprintf(stderr, "   wait-all rounds %d, main thread took the auto event first in %d\n", c.per_thread, stolen);
    CloseHandle(wa);
    CloseHandle(c.sem);
    CloseHandle(c.mx);
    CloseHandle(c.ping);
    CloseHandle(c.pong);
    CloseHandle(c.ev_a);
    CloseHandle(c.ev_b);
}

static std::atomic<uint64_t> g_churn_handle{0};
static std::atomic<bool> g_churn_stop{false};

static DWORD WINAPI churn_waiter(LPVOID p) {
    auto* bad = (std::atomic<int>*)p;
    while (!g_churn_stop.load()) {
        HANDLE h = (HANDLE)(uintptr_t)g_churn_handle.load();
        if (!h) continue;
        DWORD r = WaitForSingleObject(h, 0);
        if (r != WAIT_OBJECT_0 && r != WAIT_FAILED) (*bad)++;
        SetEvent(h);
    }
    return 0;
}

static void test_handle_churn() {
    SECTION("handle lookups racing CloseHandle");
    std::atomic<int> bad{0};
    HANDLE th[3];
    for (int i = 0; i < 3; i++) th[i] = CreateThread(nullptr, 0, churn_waiter, &bad, 0, nullptr);
    int made = 0;
    for (int i = 0; i < 30000; i++) {
        HANDLE e = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (!e) break;
        g_churn_handle.store((uint64_t)(uintptr_t)e);
        CloseHandle(e);
        made++;
    }
    g_churn_stop.store(true);
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(WaitForSingleObject(th[i], 20000), WAIT_OBJECT_0);
        CloseHandle(th[i]);
    }
    CHECK_EQ(made, 30000);
    CHECK_EQ(bad.load(), 0);
}

int main() {
    test_events();
    test_semaphore_mutex();
    test_wait_multiple();
    test_locks();
    test_lock_free_paths();
    test_handle_churn();
    test_condvars();
    test_init_once();
    test_wait_on_address();
    test_slist();
    test_timers_and_apcs();
    return t_finish("os_test_sync");
}
