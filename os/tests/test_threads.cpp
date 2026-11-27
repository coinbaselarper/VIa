#include "t.h"

#include <tlhelp32.h>

#include <atomic>
#include <set>
#include <vector>

static DWORD WINAPI ret_param(LPVOID p) { return (DWORD)(uintptr_t)p; }

static std::atomic<int> g_started;
static DWORD WINAPI mark_started(LPVOID p) {
    g_started++;
    return (DWORD)(uintptr_t)p;
}

static void test_create_and_exit_codes() {
    SECTION("CreateThread, exit codes, ids");
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, 0, ret_param, (LPVOID)(uintptr_t)1234, 0, &tid);
    CHECK(h != nullptr);
    CHECK(tid != 0);
    CHECK(((uintptr_t)h & 3) == 0);
    CHECK((uintptr_t)h < 0x100000000ull);
    CHECK_EQ(WaitForSingleObject(h, 10000), WAIT_OBJECT_0);
    DWORD code = 0;
    CHECK(GetExitCodeThread(h, &code));
    CHECK_EQ(code, 1234);
    CHECK_EQ(GetThreadId(h), tid);
    CHECK_EQ(WaitForSingleObject(h, 0), WAIT_OBJECT_0);
    CHECK(CloseHandle(h));
    CHECK(!CloseHandle(h));
    CHECK_EQ(GetLastError(), ERROR_INVALID_HANDLE);

    CHECK(GetCurrentThreadId() != 0);
    CHECK(GetCurrentThreadId() != tid);
    CHECK_EQ(GetThreadId(GetCurrentThread()), GetCurrentThreadId());
    CHECK_EQ(WaitForSingleObject(GetCurrentThread(), 0), WAIT_TIMEOUT);

    std::set<DWORD> ids;
    std::vector<HANDLE> hs;
    for (int i = 0; i < 64; i++) {
        DWORD id = 0;
        HANDLE t = CreateThread(nullptr, 0, ret_param, (LPVOID)(uintptr_t)i, 0, &id);
        CHECK(t != nullptr);
        ids.insert(id);
        hs.push_back(t);
    }
    CHECK_EQ(ids.size(), 64);
    for (size_t i = 0; i < hs.size(); i += 32) {
        DWORD n = (DWORD)(hs.size() - i < 32 ? hs.size() - i : 32);
        CHECK_EQ(WaitForMultipleObjects(n, &hs[i], TRUE, 20000), WAIT_OBJECT_0);
    }
    bool codes_ok = true;
    for (int i = 0; i < 64; i++) {
        DWORD c = 0;
        GetExitCodeThread(hs[i], &c);
        if (c != (DWORD)i) codes_ok = false;
        CloseHandle(hs[i]);
    }
    CHECK(codes_ok);
}

static DWORD WINAPI exits_early(LPVOID) {
    ExitThread(77);
    return 1;
}

static DWORD WINAPI wait_for_event(LPVOID p) {
    WaitForSingleObject((HANDLE)p, INFINITE);
    return 5;
}

static void test_exit_thread_and_open() {
    SECTION("ExitThread, OpenThread, STILL_ACTIVE, DuplicateHandle");
    HANDLE h = CreateThread(nullptr, 0, exits_early, nullptr, 0, nullptr);
    CHECK_EQ(WaitForSingleObject(h, 10000), WAIT_OBJECT_0);
    DWORD code = 0;
    CHECK(GetExitCodeThread(h, &code));
    CHECK_EQ(code, 77);
    CloseHandle(h);

    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD tid = 0;
    h = CreateThread(nullptr, 0, wait_for_event, ev, 0, &tid);
    CHECK(GetExitCodeThread(h, &code));
    CHECK_EQ(code, STILL_ACTIVE);
    HANDLE o = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
    CHECK(o != nullptr && o != h);
    CHECK_EQ(GetThreadId(o), tid);
    CHECK(OpenThread(THREAD_ALL_ACCESS, FALSE, 0xFFFFFFF0u) == nullptr);
    CHECK_EQ(WaitForSingleObject(o, 0), WAIT_TIMEOUT);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    CHECK(snap != INVALID_HANDLE_VALUE);
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    bool saw_worker = false, saw_self = false;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        saw_worker |= te.th32ThreadID == tid;
        saw_self |= te.th32ThreadID == GetCurrentThreadId();
        if (te.th32OwnerProcessID != GetCurrentProcessId()) saw_self = false;
    }
    CHECK_EQ(GetLastError(), ERROR_NO_MORE_FILES);
    CHECK(saw_worker && saw_self);
    CloseHandle(snap);
    SetEvent(ev);
    CHECK_EQ(WaitForSingleObject(o, 10000), WAIT_OBJECT_0);
    CHECK(GetExitCodeThread(h, &code));
    CHECK_EQ(code, 5);
    CloseHandle(h);
    CHECK(GetExitCodeThread(o, &code));
    CHECK_EQ(code, 5);
    CloseHandle(o);
    CloseHandle(ev);

    HANDLE real = nullptr;
    CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real, 0, FALSE,
                          DUPLICATE_SAME_ACCESS));
    CHECK(real != nullptr && real != GetCurrentThread());
    CHECK_EQ(GetThreadId(real), GetCurrentThreadId());
    CHECK_EQ(WaitForSingleObject(real, 0), WAIT_TIMEOUT);
    CloseHandle(real);
}

static std::atomic<uint64_t> g_spin;
static std::atomic<bool> g_stop;
static DWORD WINAPI spinner(LPVOID) {
    while (!g_stop.load(std::memory_order_relaxed)) g_spin.fetch_add(1, std::memory_order_relaxed);
    return 9;
}

static void test_suspend_resume() {
    SECTION("CREATE_SUSPENDED, SuspendThread, ResumeThread");
    g_started = 0;
    HANDLE h = CreateThread(nullptr, 0, mark_started, (LPVOID)3, CREATE_SUSPENDED, nullptr);
    Sleep(50);
    CHECK_EQ(g_started.load(), 0);
    CHECK_EQ(WaitForSingleObject(h, 0), WAIT_TIMEOUT);
    CHECK_EQ(SuspendThread(h), 1);
    CHECK_EQ(ResumeThread(h), 2);
    Sleep(20);
    CHECK_EQ(g_started.load(), 0);
    CHECK_EQ(ResumeThread(h), 1);
    CHECK_EQ(WaitForSingleObject(h, 10000), WAIT_OBJECT_0);
    CHECK_EQ(g_started.load(), 1);
    CHECK_EQ(ResumeThread(h), 0);
    CHECK_EQ(SuspendThread(h), (DWORD)-1);
    CloseHandle(h);

    g_stop = false;
    g_spin = 0;
    h = CreateThread(nullptr, 0, spinner, nullptr, 0, nullptr);
    uint64_t t0 = gta6rt_time_monotonic_ns();
    while (g_spin.load() == 0 && t_ms_since(t0) < 5000) Sleep(1);
    CHECK(g_spin.load() > 0);
    CHECK_EQ(SuspendThread(h), 0);
    CHECK_EQ(SuspendThread(h), 1);
    Sleep(10);
    uint64_t a = g_spin.load();
    Sleep(50);
    uint64_t b = g_spin.load();
    CHECK_EQ(a, b);
    CHECK_EQ(ResumeThread(h), 2);
    Sleep(20);
    CHECK_EQ(g_spin.load(), b);
    CHECK_EQ(ResumeThread(h), 1);
    t0 = gta6rt_time_monotonic_ns();
    while (g_spin.load() == b && t_ms_since(t0) < 5000) Sleep(1);
    CHECK(g_spin.load() > b);
    g_stop = true;
    CHECK_EQ(WaitForSingleObject(h, 10000), WAIT_OBJECT_0);
    CloseHandle(h);
}

static void test_priority_affinity() {
    SECTION("priority, affinity, ideal processor, description");
    HANDLE self = GetCurrentThread();
    CHECK_EQ(GetThreadPriority(self), THREAD_PRIORITY_NORMAL);
    CHECK(SetThreadPriority(self, THREAD_PRIORITY_HIGHEST));
    CHECK_EQ(GetThreadPriority(self), THREAD_PRIORITY_HIGHEST);
    CHECK(SetThreadPriority(self, THREAD_PRIORITY_NORMAL));
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD_PTR all = si.dwActiveProcessorMask;
    CHECK(all != 0);
    CHECK_EQ(SetThreadAffinityMask(self, 1), all);
    CHECK_EQ(SetThreadAffinityMask(self, all), 1);
    CHECK_EQ(SetThreadAffinityMask(self, 0), 0);
    CHECK_EQ(GetLastError(), ERROR_INVALID_PARAMETER);
    GROUP_AFFINITY ga;
    CHECK(GetThreadGroupAffinity(self, &ga));
    CHECK_EQ(ga.Mask, all);
    CHECK_EQ(SetThreadIdealProcessor(self, 2), 0);
    CHECK_EQ(SetThreadIdealProcessor(self, 64), 2);
    PROCESSOR_NUMBER pn;
    CHECK(GetThreadIdealProcessorEx(self, &pn));
    CHECK_EQ(pn.Number, 2);
    CHECK(GetCurrentProcessorNumber() < si.dwNumberOfProcessors);
    CHECK(SUCCEEDED(SetThreadDescription(self, u"gta6rt test thread")));
    PWSTR d = nullptr;
    CHECK(SUCCEEDED(GetThreadDescription(self, &d)));
    CHECK(d && memcmp(d, u"gta6rt test thread", 19 * 2) == 0);
    LocalFree(d);
    CHECK(SwitchToThread());
    CHECK(!TerminateThread(self, 0));
}

static DWORD g_tls;
static DWORD WINAPI tls_worker(LPVOID p) {
    if (TlsGetValue(g_tls) != nullptr) return 1;
    TlsSetValue(g_tls, p);
    Sleep(10);
    return TlsGetValue(g_tls) == p ? 0 : 2;
}

static std::atomic<int> g_fls_cb_calls;
static std::atomic<uintptr_t> g_fls_cb_value;
static void WINAPI fls_cb(PVOID v) {
    g_fls_cb_calls++;
    g_fls_cb_value = (uintptr_t)v;
}
static DWORD g_fls;
static DWORD WINAPI fls_worker(LPVOID p) {
    FlsSetValue(g_fls, p);
    return FlsGetValue(g_fls) == p ? 0 : 1;
}

static void test_tls_fls() {
    SECTION("TLS and FLS");
    g_tls = TlsAlloc();
    CHECK(g_tls != TLS_OUT_OF_INDEXES);
    CHECK(TlsSetValue(g_tls, (LPVOID)0x1111));
    HANDLE hs[8];
    for (int i = 0; i < 8; i++) hs[i] = CreateThread(nullptr, 0, tls_worker, (LPVOID)(uintptr_t)(0x100 + i), 0, nullptr);
    CHECK_EQ(WaitForMultipleObjects(8, hs, TRUE, 10000), WAIT_OBJECT_0);
    bool ok = true;
    for (HANDLE h : hs) {
        DWORD c = 9;
        GetExitCodeThread(h, &c);
        if (c) ok = false;
        CloseHandle(h);
    }
    CHECK(ok);
    CHECK(TlsGetValue(g_tls) == (LPVOID)0x1111);
    CHECK_EQ(GetLastError(), ERROR_SUCCESS);
    CHECK(TlsFree(g_tls));
    CHECK(!TlsFree(g_tls));
    DWORD again = TlsAlloc();
    CHECK(TlsGetValue(again) == nullptr);
    TlsFree(again);
    std::vector<DWORD> many;
    for (int i = 0; i < 1000; i++) {
        DWORD x = TlsAlloc();
        if (x == TLS_OUT_OF_INDEXES) break;
        many.push_back(x);
    }
    CHECK(many.size() >= 1000);
    for (DWORD x : many) TlsFree(x);

    g_fls = FlsAlloc(fls_cb);
    CHECK(g_fls != FLS_OUT_OF_INDEXES);
    g_fls_cb_calls = 0;
    HANDLE h = CreateThread(nullptr, 0, fls_worker, (LPVOID)0xABC0, 0, nullptr);
    CHECK_EQ(WaitForSingleObject(h, 10000), WAIT_OBJECT_0);
    CloseHandle(h);
    CHECK_EQ(g_fls_cb_calls.load(), 1);
    CHECK_EQ(g_fls_cb_value.load(), 0xABC0);
    CHECK(FlsSetValue(g_fls, (PVOID)0xDEF0));
    CHECK(FlsGetValue(g_fls) == (PVOID)0xDEF0);
    CHECK(FlsFree(g_fls));
    CHECK_EQ(g_fls_cb_calls.load(), 2);
    CHECK_EQ(g_fls_cb_value.load(), 0xDEF0);
}

static std::atomic<int> g_hook_start, g_hook_exit;
static std::atomic<bool> g_hook_slot_ok{true};
static gta6rt_status on_start(gta6rt_thread* t, void* user) {
    g_hook_start++;
    if (user != (void*)0x5150 || t != gta6rt_thread_current()) g_hook_slot_ok = false;
    gta6rt_thread_set_user_slot(t, 0, (void*)0x7E8);
    return GTA6RT_OK;
}
static void on_exit(gta6rt_thread* t, void*) {
    g_hook_exit++;
    if (gta6rt_thread_user_slot(t, 0) != (void*)0x7E8) g_hook_slot_ok = false;
}

static std::atomic<int> g_direct_runs;
static std::atomic<uintptr_t> g_direct_slot;
static void direct_fn(void* arg) {
    g_direct_runs++;
    g_direct_slot = (uintptr_t)gta6rt_thread_user_slot(gta6rt_thread_current(), 0);
    if (arg) gta6rt_sleep_ns(100000000ull);
}

static void test_hooks_and_direct_api() {
    SECTION("thread hooks, gta6rt_thread_* direct");
    gta6rt_thread_hooks hk = {on_start, on_exit, (void*)0x5150};
    CHECK_EQ(gta6rt_thread_set_hooks(&hk), GTA6RT_OK);
    HANDLE h = CreateThread(nullptr, 0, ret_param, nullptr, 0, nullptr);
    WaitForSingleObject(h, 10000);
    CloseHandle(h);
    CHECK_EQ(g_hook_start.load(), 1);
    CHECK_EQ(g_hook_exit.load(), 1);

    gta6rt_thread* t = nullptr;
    gta6rt_thread_desc d = {};
    d.fn = direct_fn;
    d.arg = (void*)1;
    d.stack_size = 1u << 20;
    d.name = "direct";
    d.suspended = 1;
    d.guest = 0;
    CHECK_EQ(gta6rt_thread_create(&d, &t), GTA6RT_OK);
    CHECK(gta6rt_thread_id(t) != 0);
    gta6rt_sleep_ns(20000000ull);
    CHECK_EQ(g_direct_runs.load(), 0);
    CHECK_EQ(gta6rt_thread_join(t, 0), GTA6RT_E_TIMEOUT);
    CHECK_EQ(gta6rt_thread_resume(t), GTA6RT_OK);
    CHECK_EQ(gta6rt_thread_join(t, 10000000ull), GTA6RT_E_TIMEOUT);
    CHECK_EQ(gta6rt_thread_join(t, GTA6RT_WAIT_INFINITE), GTA6RT_OK);
    CHECK_EQ(g_direct_runs.load(), 1);
    CHECK_EQ(g_direct_slot.load(), 0);
    CHECK_EQ(g_hook_start.load(), 1);
    uint64_t base = 0, limit = 0;
    gta6rt_thread_stack(t, &base, &limit);
    CHECK(base > limit);
    CHECK(base - limit >= (1u << 20));
    gta6rt_waitable* tw = gta6rt_thread_waitable(t);
    CHECK_EQ(gta6rt_wait_any(&tw, 1, 0, nullptr), 0);
    gta6rt_thread_release(t);

    d.arg = nullptr;
    d.suspended = 0;
    d.guest = 1;
    CHECK_EQ(gta6rt_thread_create(&d, &t), GTA6RT_OK);
    CHECK_EQ(gta6rt_thread_join(t, GTA6RT_WAIT_INFINITE), GTA6RT_OK);
    CHECK_EQ(g_direct_slot.load(), 0x7E8);
    CHECK_EQ(g_hook_start.load(), 2);
    CHECK_EQ(g_hook_exit.load(), 2);
    gta6rt_thread_release(t);
    CHECK(g_hook_slot_ok.load());
    gta6rt_thread_set_hooks(nullptr);

    gta6rt_thread* me = gta6rt_thread_current();
    gta6rt_thread* adopted = nullptr;
    CHECK_EQ(gta6rt_thread_adopt_current(0, &adopted), GTA6RT_OK);
    CHECK(adopted == me);
    gta6rt_thread_release(adopted);
    CHECK_EQ(gta6rt_thread_id(me), GetCurrentThreadId());
    int local = 0;
    gta6rt_thread_stack(me, &base, &limit);
    CHECK((uint64_t)(uintptr_t)&local < base && (uint64_t)(uintptr_t)&local > limit);
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    CHECK_EQ(lo, limit);
    CHECK_EQ(hi, base);
    gta6rt_thread_set_user_slot(me, 5, (void*)0x55);
    CHECK(gta6rt_thread_user_slot(me, 5) == (void*)0x55);
    CHECK(gta6rt_thread_user_slot(me, 8) == nullptr);
    CHECK_EQ(gta6rt_thread_set_name(me, "main-test"), GTA6RT_OK);
}

static std::atomic<int> g_invoked;
static uint64_t counting_invoker(gta6rt_gva fn, const uint64_t* a, uint32_t n, void* user) {
    g_invoked++;
    if (user != (void*)0x99) return 0xBAD;
    typedef uint64_t(GTA6RT_GUEST_ABI * F4)(uint64_t, uint64_t, uint64_t, uint64_t);
    uint64_t v[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < n && i < 4; i++) v[i] = a[i];
    return ((F4)(uintptr_t)fn)(v[0], v[1], v[2], v[3]);
}

static void test_invoker() {
    SECTION("callback invoker");
    CHECK_EQ(gta6rt_os_set_invoker(counting_invoker, (void*)0x99), GTA6RT_OK);
    HANDLE h = CreateThread(nullptr, 0, ret_param, (LPVOID)42, 0, nullptr);
    WaitForSingleObject(h, 10000);
    DWORD c = 0;
    GetExitCodeThread(h, &c);
    CloseHandle(h);
    CHECK_EQ(c, 42);
    CHECK_EQ(g_invoked.load(), 1);
    uint64_t arg = 7;
    CHECK_EQ(gta6rt_os_invoke((gta6rt_gva)(uintptr_t)ret_param, &arg, 1), 7);
    CHECK_EQ(g_invoked.load(), 2);
    gta6rt_os_set_invoker(nullptr, nullptr);
    CHECK_EQ(gta6rt_os_invoke((gta6rt_gva)(uintptr_t)ret_param, &arg, 1), 7);
    CHECK_EQ(g_invoked.load(), 2);
}

static std::atomic<int> g_apcs;
static void WINAPI apc(ULONG_PTR d) { g_apcs += (int)d; }
static DWORD WINAPI alertable_sleeper(LPVOID) {
    DWORD r = SleepEx(10000, TRUE);
    return r == WAIT_IO_COMPLETION ? 0 : 1;
}

static DWORD WINAPI takes_mutex(LPVOID p) {
    WaitForSingleObject((HANDLE)p, INFINITE);
    return 0;
}

static void test_apc_and_abandon() {
    SECTION("QueueUserAPC, abandoned mutex");
    g_apcs = 0;
    HANDLE h = CreateThread(nullptr, 0, alertable_sleeper, nullptr, 0, nullptr);
    Sleep(30);
    CHECK(QueueUserAPC(apc, h, 5));
    uint64_t t0 = gta6rt_time_monotonic_ns();
    CHECK_EQ(WaitForSingleObject(h, 10000), WAIT_OBJECT_0);
    CHECK(t_ms_since(t0) < 5000);
    DWORD c = 1;
    GetExitCodeThread(h, &c);
    CHECK_EQ(c, 0);
    CHECK_EQ(g_apcs.load(), 5);
    CHECK(!QueueUserAPC(apc, h, 1));
    CloseHandle(h);

    CHECK(QueueUserAPC(apc, GetCurrentThread(), 2));
    CHECK_EQ(g_apcs.load(), 5);
    CHECK_EQ(SleepEx(0, TRUE), WAIT_IO_COMPLETION);
    CHECK_EQ(g_apcs.load(), 7);

    HANDLE m = CreateMutexW(nullptr, FALSE, nullptr);
    h = CreateThread(nullptr, 0, takes_mutex, m, 0, nullptr);
    CHECK_EQ(WaitForSingleObject(h, 10000), WAIT_OBJECT_0);
    CloseHandle(h);
    CHECK_EQ(WaitForSingleObject(m, 1000), WAIT_ABANDONED_0);
    CHECK(ReleaseMutex(m));
    CHECK_EQ(WaitForSingleObject(m, 0), WAIT_OBJECT_0);
    CHECK(ReleaseMutex(m));
    CloseHandle(m);
}

static std::atomic<int> g_work_runs;
static void WINAPI work_cb(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WORK) {
    Sleep(1);
    g_work_runs += (int)(uintptr_t)ctx;
}
static std::atomic<int> g_timer_runs;
static void WINAPI timer_cb(PTP_CALLBACK_INSTANCE, PVOID, PTP_TIMER) { g_timer_runs++; }
static std::atomic<int> g_wait_obj, g_wait_timeout;
static void WINAPI wait_cb(PTP_CALLBACK_INSTANCE, PVOID, PTP_WAIT, TP_WAIT_RESULT r) {
    if (r == WAIT_OBJECT_0) g_wait_obj++;
    else if (r == WAIT_TIMEOUT) g_wait_timeout++;
}
static std::atomic<int> g_simple;
static HANDLE g_done_evt;
static void WINAPI simple_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx) {
    g_simple += (int)(uintptr_t)ctx;
    SetEventWhenCallbackReturns(inst, g_done_evt);
}

static void test_threadpool() {
    SECTION("thread pool: work, timer, wait, simple");
    PTP_WORK w = CreateThreadpoolWork(work_cb, (PVOID)1, nullptr);
    CHECK(w != nullptr);
    for (int i = 0; i < 100; i++) SubmitThreadpoolWork(w);
    WaitForThreadpoolWorkCallbacks(w, FALSE);
    CHECK_EQ(g_work_runs.load(), 100);
    CloseThreadpoolWork(w);

    PTP_TIMER t = CreateThreadpoolTimer(timer_cb, nullptr, nullptr);
    LARGE_INTEGER due;
    due.QuadPart = -10 * 10000;
    FILETIME ft = {(DWORD)due.QuadPart, (DWORD)(due.QuadPart >> 32)};
    SetThreadpoolTimer(t, &ft, 10, 0);
    CHECK(IsThreadpoolTimerSet(t));
    Sleep(200);
    SetThreadpoolTimer(t, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(t, TRUE);
    int fired = g_timer_runs.load();
    CHECK(fired >= 3);
    CHECK(!IsThreadpoolTimerSet(t));
    Sleep(50);
    CHECK_EQ(g_timer_runs.load(), fired);
    CloseThreadpoolTimer(t);

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    PTP_WAIT pw = CreateThreadpoolWait(wait_cb, nullptr, nullptr);
    SetThreadpoolWait(pw, ev, nullptr);
    Sleep(20);
    CHECK_EQ(g_wait_obj.load(), 0);
    SetEvent(ev);
    uint64_t t0 = gta6rt_time_monotonic_ns();
    while (g_wait_obj.load() == 0 && t_ms_since(t0) < 5000) Sleep(1);
    CHECK_EQ(g_wait_obj.load(), 1);
    due.QuadPart = -30 * 10000;
    ft = {(DWORD)due.QuadPart, (DWORD)(due.QuadPart >> 32)};
    SetThreadpoolWait(pw, ev, &ft);
    t0 = gta6rt_time_monotonic_ns();
    while (g_wait_timeout.load() == 0 && t_ms_since(t0) < 5000) Sleep(1);
    CHECK_EQ(g_wait_timeout.load(), 1);
    CHECK(t_ms_since(t0) >= 20);
    WaitForThreadpoolWaitCallbacks(pw, FALSE);
    CloseThreadpoolWait(pw);
    CloseHandle(ev);

    g_done_evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(TrySubmitThreadpoolCallback(simple_cb, (PVOID)4, nullptr));
    CHECK_EQ(WaitForSingleObject(g_done_evt, 10000), WAIT_OBJECT_0);
    CHECK_EQ(g_simple.load(), 4);
    CloseHandle(g_done_evt);
}

int main() {
    gta6rt_thread_adopt_current(0, nullptr);
    test_create_and_exit_codes();
    test_exit_thread_and_open();
    test_suspend_resume();
    test_priority_affinity();
    test_tls_fls();
    test_hooks_and_direct_api();
    test_invoker();
    test_apc_and_abandon();
    test_threadpool();
    return t_finish("os_test_threads");
}
