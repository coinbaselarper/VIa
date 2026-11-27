#include "internal.h"

#include <os/os_sync_wait_on_address.h>

#include <tlhelp32.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <unordered_map>
#include <algorithm>
#include <vector>

using namespace gta6rt_os;

namespace {
constexpr uint32_t kTlsSlots = 64 + 1024;
constexpr uint32_t kFlsSlots = 128;
} // namespace

struct gta6rt_thread {
    std::atomic<int> refs{1};
    pthread_t pt = nullptr;
    mach_port_t port = MACH_PORT_NULL;
    uint32_t tid = 0;
    uint64_t stack_base = 0, stack_limit = 0;
    void* slots[8] = {};
    char name[64] = {};
    int priority = 0;
    gta6rt_thread_fn fn = nullptr;
    void* arg = nullptr;
    bool guest = false, adopted = false, started = false, finished = false, hooks_ran = false;
    uint32_t start_suspend = 0;
    uint32_t suspend_count = 0;
    pthread_cond_t cv;
    pthread_cond_t gate_cv;
    gta6rt_waitable* done = nullptr;
    uint32_t exit_code = STILL_ACTIVE;
    Apc* apc_head = nullptr;
    Apc* apc_tail = nullptr;
    bool alertable = false;
    std::atomic<uint32_t> wake_seq{0};
    uint64_t affinity = 0;
    DWORD ideal = 0;
    std::u16string desc;
    gta6rt_gva win_start = 0;
    uint64_t win_param = 0;
    uint64_t create_ft = 0;
    void* tls[kTlsSlots] = {};
    void* fls[kFlsSlots] = {};
};

namespace {

thread_local gta6rt_thread* t_self;
std::atomic<uint32_t> g_next_tid{0x100};
pthread_mutex_t g_reg_mx = PTHREAD_MUTEX_INITIALIZER;
std::unordered_map<uint32_t, gta6rt_thread*>* g_registry;
gta6rt_thread_hooks g_hooks;
bool g_have_hooks;
pthread_key_t g_key;
pthread_once_t g_key_once = PTHREAD_ONCE_INIT;

pthread_mutex_t g_tls_mx = PTHREAD_MUTEX_INITIALIZER;
uint8_t g_tls_used[kTlsSlots];
uint8_t g_fls_used[kFlsSlots];
gta6rt_gva g_fls_cb[kFlsSlots];

uint64_t all_cpus_mask() {
    uint32_t n = num_cpus();
    return n >= 64 ? ~0ull : ((1ull << n) - 1);
}

gta6rt_thread* alloc_thread() {
    gta6rt_thread* t = new gta6rt_thread();
    pthread_cond_init(&t->cv, nullptr);
    pthread_cond_init(&t->gate_cv, nullptr);
    t->tid = g_next_tid.fetch_add(4);
    t->done = waitable_new_event(true, false);
    t->affinity = all_cpus_mask();
    t->create_ft = filetime_now();
    pthread_mutex_lock(&g_reg_mx);
    if (!g_registry) g_registry = new std::unordered_map<uint32_t, gta6rt_thread*>();
    (*g_registry)[t->tid] = t;
    pthread_mutex_unlock(&g_reg_mx);
    return t;
}

void fill_stack_bounds(gta6rt_thread* t) {
    pthread_t self = pthread_self();
    uint64_t hi = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(self);
    uint64_t sz = pthread_get_stacksize_np(self);
    t->stack_base = hi;
    t->stack_limit = hi - sz;
    t->pt = self;
    t->port = pthread_mach_thread_np(self);
}

void run_start_hooks(gta6rt_thread* t) {
    if (t->hooks_ran || !g_have_hooks || !g_hooks.on_start) return;
    t->hooks_ran = true;
    gta6rt_status st = g_hooks.on_start(t, g_hooks.user);
    if (st != GTA6RT_OK) OS_LOG(GTA6RT_LOG_ERROR, "thread %#x on_start hook failed: %s", t->tid, gta6rt_status_name(st));
}

void finish(gta6rt_thread* t) {
    if (t->finished) return;
    for (uint32_t i = 0; i < kFlsSlots; i++) {
        if (!t->fls[i]) continue;
        pthread_mutex_lock(&g_tls_mx);
        gta6rt_gva cb = g_fls_used[i] ? g_fls_cb[i] : 0;
        pthread_mutex_unlock(&g_tls_mx);
        void* v = t->fls[i];
        t->fls[i] = nullptr;
        if (cb) {
            uint64_t a = (uint64_t)(uintptr_t)v;
            gta6rt_os_invoke(cb, &a, 1);
        }
    }
    if (t->guest && t->hooks_ran && g_have_hooks && g_hooks.on_exit) g_hooks.on_exit(t, g_hooks.user);
    pthread_mutex_lock(&g_wait_mx);
    t->finished = true;
    abandon_mutexes_locked(t);
    waitable_set_locked(t->done);
    Apc* a = t->apc_head;
    t->apc_head = t->apc_tail = nullptr;
    pthread_mutex_unlock(&g_wait_mx);
    while (a) {
        Apc* n = a->next;
        delete a;
        a = n;
    }
}

void key_destructor(void* p) {
    gta6rt_thread* t = (gta6rt_thread*)p;
    if (!t) return;
    t_self = t;
    if (t->adopted) {
        if (t->exit_code == STILL_ACTIVE) t->exit_code = 0;
        finish(t);
    }
    t_self = nullptr;
    gta6rt_thread_release(t);
}

void make_key() { pthread_key_create(&g_key, key_destructor); }

void* trampoline(void* p) {
    gta6rt_thread* t = (gta6rt_thread*)p;
    t_self = t;
    pthread_once(&g_key_once, make_key);
    pthread_setspecific(g_key, t);
    fill_stack_bounds(t);
    if (t->name[0]) pthread_setname_np(t->name);
    pthread_mutex_lock(&g_wait_mx);
    while (t->start_suspend > 0) pthread_cond_wait(&t->gate_cv, &g_wait_mx);
    t->started = true;
    pthread_mutex_unlock(&g_wait_mx);
    // Names set while the thread was suspended, or the guest start address for unnamed title threads (profiling).
    char start_name[24];
    if (!t->name[0] && t->guest && t->win_start) {
        snprintf(start_name, sizeof start_name, "guest@%llx", (unsigned long long)t->win_start);
        pthread_setname_np(start_name);
    } else if (t->name[0]) {
        pthread_setname_np(t->name);
    }
    if (t->guest) run_start_hooks(t);
    t->fn(t->arg);
    if (t->exit_code == STILL_ACTIVE) t->exit_code = 0;
    finish(t);
    return nullptr;
}

gta6rt_thread* resolve(HANDLE h) {
    uint64_t v = H(h);
    if (v == kPseudoThread) {
        gta6rt_thread* t = gta6rt_thread_current();
        thread_retain(t);
        return t;
    }
    void (*rel)(void*) = nullptr;
    gta6rt_thread* t = (gta6rt_thread*)handle_ref(v, GTA6RT_HANDLE_THREAD, &rel);
    if (!t) set_error(ERROR_INVALID_HANDLE);
    return t;
}

void t_release(void* p) { gta6rt_thread_release((gta6rt_thread*)p); }
void t_retain(void* p) { thread_retain((gta6rt_thread*)p); }

HANDLE thread_handle(gta6rt_thread* t) { return HV(handle_alloc_internal(GTA6RT_HANDLE_THREAD, t, t_release, t_retain)); }

void win_entry(void* arg) {
    gta6rt_thread* t = (gta6rt_thread*)arg;
    uint64_t a = t->win_param;
    t->exit_code = (uint32_t)gta6rt_os_invoke(t->win_start, &a, 1);
}

uint64_t ft_from_tv(time_value_t tv) { return (uint64_t)tv.seconds * 10000000ull + (uint64_t)tv.microseconds * 10; }

} // namespace

namespace gta6rt_os {

Thread* current_thread() { return (Thread*)gta6rt_thread_current(); }
uint32_t current_tid() { return gta6rt_thread_current()->tid; }
void thread_retain(gta6rt_thread* t) {
    if (t) t->refs.fetch_add(1);
}
pthread_cond_t* thread_wait_cv(gta6rt_thread* t) { return &t->cv; }
std::atomic<uint32_t>* thread_wake_word(gta6rt_thread* t) { return &t->wake_seq; }

void wake_word(std::atomic<uint32_t>* w) {
    w->fetch_add(1);
    os_sync_wake_by_address_any(w, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

void sleep_on_word(std::atomic<uint32_t>* w, uint32_t seen, uint64_t timeout_ns) {
    int r;
    if (timeout_ns == UINT64_MAX) {
        r = os_sync_wait_on_address(w, seen, sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_NONE);
    } else {
        // The timeout is in nanoseconds for OS_CLOCK_MACH_ABSOLUTE_TIME, not in mach ticks.
        r = os_sync_wait_on_address_with_timeout(w, seen, sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                 OS_CLOCK_MACH_ABSOLUTE_TIME, timeout_ns ? timeout_ns : 1);
    }
    if (r < 0 && errno != ETIMEDOUT && errno != EINTR) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) OS_LOG(GTA6RT_LOG_ERROR, "os_sync_wait_on_address failed (errno %d); waits poll every 1 ms", errno);
        struct timespec ts = {0, (long)std::min<uint64_t>(timeout_ns, 1000000)};
        nanosleep(&ts, nullptr);
    }
}

gta6rt_thread* thread_by_id(uint32_t tid) {
    pthread_mutex_lock(&g_reg_mx);
    gta6rt_thread* t = nullptr;
    if (g_registry) {
        auto it = g_registry->find(tid);
        if (it != g_registry->end() && it->second->refs.load() > 0) {
            t = it->second;
            t->refs.fetch_add(1);
        }
    }
    pthread_mutex_unlock(&g_reg_mx);
    return t;
}

bool queue_apc_locked(gta6rt_thread* t, Apc* apc) {
    if (t->finished) return false;
    apc->next = nullptr;
    if (t->apc_tail) t->apc_tail->next = apc;
    else t->apc_head = apc;
    t->apc_tail = apc;
    if (t->alertable) wake_word(&t->wake_seq);
    return true;
}

bool queue_apc(gta6rt_thread* t, Apc* apc) {
    pthread_mutex_lock(&g_wait_mx);
    bool ok = queue_apc_locked(t, apc);
    pthread_mutex_unlock(&g_wait_mx);
    if (!ok) delete apc;
    return ok;
}

int run_apcs() {
    gta6rt_thread* t = gta6rt_thread_current();
    pthread_mutex_lock(&g_wait_mx);
    Apc* a = t->apc_head;
    t->apc_head = t->apc_tail = nullptr;
    pthread_mutex_unlock(&g_wait_mx);
    int n = 0;
    while (a) {
        Apc* next = a->next;
        if (a->host_fn) a->host_fn(a);
        else gta6rt_os_invoke(a->fn, a->args, a->nargs);
        delete a;
        a = next;
        n++;
    }
    return n;
}

bool has_apcs_locked(gta6rt_thread* t) { return t->apc_head != nullptr; }
void set_alertable_locked(gta6rt_thread* t, bool on) { t->alertable = on; }

} // namespace gta6rt_os

extern "C" {

gta6rt_status gta6rt_thread_set_hooks(const gta6rt_thread_hooks* hooks) {
    if (hooks) {
        g_hooks = *hooks;
        g_have_hooks = true;
    } else {
        g_have_hooks = false;
    }
    return GTA6RT_OK;
}

gta6rt_status gta6rt_thread_create(const gta6rt_thread_desc* desc, gta6rt_thread** out) {
    if (!desc || !desc->fn) return GTA6RT_E_INVALID_ARG;
    gta6rt_thread* t = alloc_thread();
    t->fn = desc->fn;
    t->arg = desc->arg;
    t->guest = desc->guest != 0;
    t->start_suspend = desc->suspended ? 1 : 0;
    if (desc->name) strlcpy(t->name, desc->name, sizeof t->name);
    size_t ss = desc->stack_size ? desc->stack_size : (8u << 20);
    if (ss < PTHREAD_STACK_MIN) ss = PTHREAD_STACK_MIN;
    size_t pg = (size_t)getpagesize();
    ss = (ss + pg - 1) & ~(pg - 1);
    if (out) t->refs.fetch_add(1);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, ss);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t pt;
    int r = pthread_create(&pt, &at, trampoline, t);
    pthread_attr_destroy(&at);
    if (r != 0) {
        if (out) t->refs.fetch_sub(1);
        gta6rt_thread_release(t);
        return errno_to_status(r);
    }
    if (out) *out = t;
    return GTA6RT_OK;
}

gta6rt_status gta6rt_thread_adopt_current(int guest, gta6rt_thread** out) {
    gta6rt_thread* t = t_self;
    if (!t) {
        t = alloc_thread();
        t->adopted = true;
        t->started = true;
        fill_stack_bounds(t);
        t_self = t;
        pthread_once(&g_key_once, make_key);
        pthread_setspecific(g_key, t);
    }
    if (guest && !t->guest) {
        t->guest = true;
        run_start_hooks(t);
    }
    if (out) {
        t->refs.fetch_add(1);
        *out = t;
    }
    return GTA6RT_OK;
}

gta6rt_status gta6rt_thread_resume(gta6rt_thread* t) {
    if (!t) return GTA6RT_E_INVALID_ARG;
    pthread_mutex_lock(&g_wait_mx);
    if (!t->started) {
        if (t->start_suspend > 0 && --t->start_suspend == 0) pthread_cond_signal(&t->gate_cv);
    } else if (t->suspend_count > 0) {
        t->suspend_count--;
        thread_resume(t->port);
    }
    pthread_mutex_unlock(&g_wait_mx);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_thread_join(gta6rt_thread* t, uint64_t timeout_ns) {
    if (!t) return GTA6RT_E_INVALID_ARG;
    gta6rt_waitable* w = t->done;
    gta6rt_status st = gta6rt_wait_any(&w, 1, timeout_ns, nullptr);
    return st >= 0 ? GTA6RT_OK : st;
}

void gta6rt_thread_release(gta6rt_thread* t) {
    if (!t || t->refs.fetch_sub(1) != 1) return;
    pthread_mutex_lock(&g_reg_mx);
    if (g_registry) {
        auto it = g_registry->find(t->tid);
        if (it != g_registry->end() && it->second == t) g_registry->erase(it);
    }
    pthread_mutex_unlock(&g_reg_mx);
    gta6rt_waitable_release(t->done);
    pthread_cond_destroy(&t->cv);
    pthread_cond_destroy(&t->gate_cv);
    delete t;
}

gta6rt_thread* gta6rt_thread_current(void) {
    if (!t_self) gta6rt_thread_adopt_current(0, nullptr);
    return t_self;
}

uint32_t gta6rt_thread_id(const gta6rt_thread* t) { return t ? t->tid : 0; }

void gta6rt_thread_stack(const gta6rt_thread* t, uint64_t* out_base, uint64_t* out_limit) {
    if (out_base) *out_base = t ? t->stack_base : 0;
    if (out_limit) *out_limit = t ? t->stack_limit : 0;
}

void* gta6rt_thread_user_slot(gta6rt_thread* t, int slot) {
    if (!t || slot < 0 || slot >= 8) return nullptr;
    return t->slots[slot];
}

void gta6rt_thread_set_user_slot(gta6rt_thread* t, int slot, void* value) {
    if (t && slot >= 0 && slot < 8) t->slots[slot] = value;
}

gta6rt_status gta6rt_thread_set_name(gta6rt_thread* t, const char* name) {
    if (!t || !name) return GTA6RT_E_INVALID_ARG;
    strlcpy(t->name, name, sizeof t->name);
    if (t == t_self) pthread_setname_np(t->name);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_thread_set_priority(gta6rt_thread* t, int win32_priority) {
    if (!t) return GTA6RT_E_INVALID_ARG;
    t->priority = win32_priority;
    return GTA6RT_OK;
}

void gta6rt_thread_yield(void) { sched_yield(); }

void gta6rt_sleep_ns(uint64_t ns) {
    if (ns == 0) {
        sched_yield();
        return;
    }
    if (ns == GTA6RT_WAIT_INFINITE) {
        for (;;) pause();
    }
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

gta6rt_waitable* gta6rt_thread_waitable(gta6rt_thread* t) { return t ? t->done : nullptr; }

// ---- Win32 threads ----

HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
                           LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId) {
    if (!lpStartAddress) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    size_t ss = dwStackSize;
    if (dwCreationFlags & STACK_SIZE_PARAM_IS_A_RESERVATION) {
        if (ss < 0x10000) ss = 0x10000;
    } else if (ss < (8u << 20)) {
        ss = 8u << 20;
    }
    ss = (ss + 0xFFFF) & ~(size_t)0xFFFF;
    gta6rt_thread_desc d = {};
    d.fn = win_entry;
    d.stack_size = ss;
    d.suspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
    d.guest = 1;
    gta6rt_thread* t = alloc_thread();
    t->fn = win_entry;
    t->arg = t;
    t->guest = true;
    t->start_suspend = d.suspended ? 1 : 0;
    t->win_start = (gta6rt_gva)(uintptr_t)lpStartAddress;
    t->win_param = (uint64_t)(uintptr_t)lpParameter;
    t->refs.fetch_add(1);
    HANDLE h = thread_handle(t);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, ss);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t pt;
    int r = pthread_create(&pt, &at, trampoline, t);
    pthread_attr_destroy(&at);
    if (r != 0) {
        gta6rt_thread_release(t);
        CloseHandle(h);
        set_error(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    if (lpThreadId) *lpThreadId = t->tid;
    OS_LOG(GTA6RT_LOG_DEBUG, "CreateThread start=%p param=%p flags=%#x -> tid %#x", (void*)lpStartAddress,
           lpParameter, dwCreationFlags, t->tid);
    return h;
}

void WINAPI ExitThread(DWORD dwExitCode) {
    gta6rt_thread* t = gta6rt_thread_current();
    t->exit_code = dwExitCode;
    finish(t);
    pthread_exit(nullptr);
}

BOOL WINAPI TerminateThread(HANDLE hThread, DWORD) {
    OS_LOG(GTA6RT_LOG_WARN, "TerminateThread(%#llx) not supported", (unsigned long long)H(hThread));
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return FALSE;
    pthread_mutex_lock(&g_wait_mx);
    DWORD c = t->finished ? t->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&g_wait_mx);
    if (lpExitCode) *lpExitCode = c;
    gta6rt_thread_release(t);
    return TRUE;
}

DWORD WINAPI ResumeThread(HANDLE hThread) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return (DWORD)-1;
    pthread_mutex_lock(&g_wait_mx);
    DWORD prev = t->started ? t->suspend_count : t->start_suspend;
    pthread_mutex_unlock(&g_wait_mx);
    gta6rt_thread_resume(t);
    gta6rt_thread_release(t);
    return prev;
}

DWORD WINAPI SuspendThread(HANDLE hThread) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return (DWORD)-1;
    pthread_mutex_lock(&g_wait_mx);
    DWORD prev;
    bool do_mach = false;
    if (t->finished) {
        pthread_mutex_unlock(&g_wait_mx);
        gta6rt_thread_release(t);
        set_error(ERROR_ACCESS_DENIED);
        return (DWORD)-1;
    }
    if (!t->started) {
        prev = t->start_suspend++;
    } else {
        prev = t->suspend_count++;
        do_mach = true;
    }
    mach_port_t port = t->port;
    pthread_mutex_unlock(&g_wait_mx);
    if (do_mach) thread_suspend(port);
    gta6rt_thread_release(t);
    return prev;
}

HANDLE WINAPI OpenThread(DWORD, BOOL, DWORD dwThreadId) {
    gta6rt_thread* t = thread_by_id(dwThreadId);
    if (!t) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    return thread_handle(t);
}

HANDLE WINAPI GetCurrentThread(void) { return HV(kPseudoThread); }
DWORD WINAPI GetCurrentThreadId(void) { return gta6rt_thread_current()->tid; }

DWORD WINAPI GetThreadId(HANDLE Thread) {
    gta6rt_thread* t = resolve(Thread);
    if (!t) return 0;
    DWORD id = t->tid;
    gta6rt_thread_release(t);
    return id;
}

HANDLE WINAPI GetCurrentProcess(void) { return HV(kPseudoProcess); }
DWORD WINAPI GetCurrentProcessId(void) { return (DWORD)getpid(); }
DWORD WINAPI GetProcessId(HANDLE Process) {
    if (H(Process) == kPseudoProcess) return (DWORD)getpid();
    set_error(ERROR_INVALID_HANDLE);
    return 0;
}

void WINAPI ExitProcess(UINT uExitCode) { exit((int)uExitCode); }

BOOL WINAPI TerminateProcess(HANDLE hProcess, UINT uExitCode) {
    if (H(hProcess) == kPseudoProcess) _exit((int)uExitCode);
    set_error(ERROR_ACCESS_DENIED);
    return FALSE;
}

BOOL WINAPI GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode) {
    if (H(hProcess) != kPseudoProcess) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (lpExitCode) *lpExitCode = STILL_ACTIVE;
    return TRUE;
}

BOOL WINAPI CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES,
                           LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) {
    OS_LOG(GTA6RT_LOG_WARN, "CreateProcessW(%s, %s) not supported",
           lpApplicationName ? u16_to_utf8(lpApplicationName).c_str() : "-",
           lpCommandLine ? u16_to_utf8(lpCommandLine).c_str() : "-");
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return FALSE;
    t->priority = nPriority;
    gta6rt_thread_release(t);
    return TRUE;
}

int WINAPI GetThreadPriority(HANDLE hThread) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return THREAD_PRIORITY_ERROR_RETURN;
    int p = t->priority;
    gta6rt_thread_release(t);
    return p;
}

DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask) {
    if (!(dwThreadAffinityMask & all_cpus_mask())) {
        set_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    gta6rt_thread* t = resolve(hThread);
    if (!t) return 0;
    DWORD_PTR prev = t->affinity;
    t->affinity = dwThreadAffinityMask & all_cpus_mask();
    gta6rt_thread_release(t);
    return prev;
}

BOOL WINAPI SetThreadGroupAffinity(HANDLE hThread, const GROUP_AFFINITY* GroupAffinity,
                                   PGROUP_AFFINITY PreviousGroupAffinity) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return FALSE;
    if (PreviousGroupAffinity) {
        memset(PreviousGroupAffinity, 0, sizeof *PreviousGroupAffinity);
        PreviousGroupAffinity->Mask = t->affinity;
    }
    if (GroupAffinity && GroupAffinity->Mask) t->affinity = GroupAffinity->Mask & all_cpus_mask();
    gta6rt_thread_release(t);
    return TRUE;
}

BOOL WINAPI GetThreadGroupAffinity(HANDLE hThread, PGROUP_AFFINITY GroupAffinity) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return FALSE;
    if (GroupAffinity) {
        memset(GroupAffinity, 0, sizeof *GroupAffinity);
        GroupAffinity->Mask = t->affinity;
    }
    gta6rt_thread_release(t);
    return TRUE;
}

DWORD WINAPI SetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return (DWORD)-1;
    DWORD prev = t->ideal;
    if (dwIdealProcessor != 64 /* MAXIMUM_PROCESSORS: query */) t->ideal = dwIdealProcessor;
    gta6rt_thread_release(t);
    return prev;
}

BOOL WINAPI SetThreadIdealProcessorEx(HANDLE hThread, PPROCESSOR_NUMBER lpIdealProcessor,
                                      PPROCESSOR_NUMBER lpPreviousIdealProcessor) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return FALSE;
    if (lpPreviousIdealProcessor) {
        memset(lpPreviousIdealProcessor, 0, sizeof *lpPreviousIdealProcessor);
        lpPreviousIdealProcessor->Number = (BYTE)t->ideal;
    }
    if (lpIdealProcessor) t->ideal = lpIdealProcessor->Number;
    gta6rt_thread_release(t);
    return TRUE;
}

BOOL WINAPI GetThreadIdealProcessorEx(HANDLE hThread, PPROCESSOR_NUMBER lpIdealProcessor) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return FALSE;
    if (lpIdealProcessor) {
        memset(lpIdealProcessor, 0, sizeof *lpIdealProcessor);
        lpIdealProcessor->Number = (BYTE)t->ideal;
    }
    gta6rt_thread_release(t);
    return TRUE;
}

HRESULT WINAPI SetThreadDescription(HANDLE hThread, PCWSTR lpThreadDescription) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return (HRESULT)0x80070006;
    t->desc = lpThreadDescription ? std::u16string(lpThreadDescription) : std::u16string();
    std::string n = u16_to_utf8(t->desc.c_str());
    strlcpy(t->name, n.c_str(), sizeof t->name);
    if (t == t_self) pthread_setname_np(t->name);
    gta6rt_thread_release(t);
    return S_OK;
}

HRESULT WINAPI GetThreadDescription(HANDLE hThread, PWSTR* ppszThreadDescription) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return (HRESULT)0x80070006;
    size_t n = t->desc.size();
    WCHAR* s = (WCHAR*)malloc((n + 1) * 2);
    memcpy(s, t->desc.data(), n * 2);
    s[n] = 0;
    *ppszThreadDescription = s;
    gta6rt_thread_release(t);
    return S_OK;
}

BOOL WINAPI GetThreadContext(HANDLE, LPCONTEXT) {
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI SetThreadContext(HANDLE, const CONTEXT*) {
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI SwitchToThread(void) {
    sched_yield();
    return TRUE;
}

DWORD WINAPI QueueUserAPC(PAPCFUNC pfnAPC, HANDLE hThread, ULONG_PTR dwData) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return 0;
    Apc* a = new Apc();
    a->fn = (gta6rt_gva)(uintptr_t)pfnAPC;
    a->args[0] = dwData;
    a->nargs = 1;
    bool ok = queue_apc(t, a);
    gta6rt_thread_release(t);
    if (!ok) set_error(ERROR_GEN_FAILURE);
    return ok;
}

BOOL WINAPI GetThreadTimes(HANDLE hThread, LPFILETIME lpCreationTime, LPFILETIME lpExitTime, LPFILETIME lpKernelTime,
                           LPFILETIME lpUserTime) {
    gta6rt_thread* t = resolve(hThread);
    if (!t) return FALSE;
    uint64_t user = 0, sys = 0;
    thread_basic_info_data_t info;
    mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
    if (!t->finished && t->port && thread_info(t->port, THREAD_BASIC_INFO, (thread_info_t)&info, &cnt) == KERN_SUCCESS) {
        user = ft_from_tv(info.user_time);
        sys = ft_from_tv(info.system_time);
    }
    auto put = [](LPFILETIME f, uint64_t v) {
        if (f) f->dwLowDateTime = (DWORD)v, f->dwHighDateTime = (DWORD)(v >> 32);
    };
    put(lpCreationTime, t->create_ft);
    put(lpExitTime, 0);
    put(lpKernelTime, sys);
    put(lpUserTime, user);
    gta6rt_thread_release(t);
    return TRUE;
}

BOOL WINAPI GetProcessTimes(HANDLE, LPFILETIME lpCreationTime, LPFILETIME lpExitTime, LPFILETIME lpKernelTime,
                            LPFILETIME lpUserTime) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    uint64_t user = (uint64_t)ru.ru_utime.tv_sec * 10000000ull + (uint64_t)ru.ru_utime.tv_usec * 10;
    uint64_t sys = (uint64_t)ru.ru_stime.tv_sec * 10000000ull + (uint64_t)ru.ru_stime.tv_usec * 10;
    uint64_t start = 0;
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    if (sysctl(mib, 4, &kp, &len, nullptr, 0) == 0)
        start = (uint64_t)kp.kp_proc.p_starttime.tv_sec * 10000000ull + (uint64_t)kp.kp_proc.p_starttime.tv_usec * 10 +
                116444736000000000ull;
    auto put = [](LPFILETIME f, uint64_t v) {
        if (f) f->dwLowDateTime = (DWORD)v, f->dwHighDateTime = (DWORD)(v >> 32);
    };
    put(lpCreationTime, start);
    put(lpExitTime, 0);
    put(lpKernelTime, sys);
    put(lpUserTime, user);
    return TRUE;
}

DWORD WINAPI GetCurrentProcessorNumber(void) { return (gta6rt_thread_current()->tid >> 2) % num_cpus(); }

void WINAPI GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER ProcNumber) {
    if (!ProcNumber) return;
    memset(ProcNumber, 0, sizeof *ProcNumber);
    ProcNumber->Number = (BYTE)GetCurrentProcessorNumber();
}

void WINAPI GetCurrentThreadStackLimits(PULONG_PTR LowLimit, PULONG_PTR HighLimit) {
    gta6rt_thread* t = gta6rt_thread_current();
    if (LowLimit) *LowLimit = t->stack_limit;
    if (HighLimit) *HighLimit = t->stack_base;
}

DWORD WINAPI TlsAlloc(void) {
    pthread_mutex_lock(&g_tls_mx);
    for (uint32_t i = 0; i < kTlsSlots; i++)
        if (!g_tls_used[i]) {
            g_tls_used[i] = 1;
            pthread_mutex_unlock(&g_tls_mx);
            pthread_mutex_lock(&g_reg_mx);
            if (g_registry)
                for (auto& kv : *g_registry) kv.second->tls[i] = nullptr;
            pthread_mutex_unlock(&g_reg_mx);
            return i;
        }
    pthread_mutex_unlock(&g_tls_mx);
    set_error(ERROR_NO_MORE_ITEMS);
    return TLS_OUT_OF_INDEXES;
}

LPVOID WINAPI TlsGetValue(DWORD dwTlsIndex) {
    if (dwTlsIndex >= kTlsSlots) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    set_error(ERROR_SUCCESS);
    return gta6rt_thread_current()->tls[dwTlsIndex];
}

BOOL WINAPI TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue) {
    if (dwTlsIndex >= kTlsSlots) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    gta6rt_thread_current()->tls[dwTlsIndex] = lpTlsValue;
    return TRUE;
}

BOOL WINAPI TlsFree(DWORD dwTlsIndex) {
    if (dwTlsIndex >= kTlsSlots) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pthread_mutex_lock(&g_tls_mx);
    bool was = g_tls_used[dwTlsIndex];
    g_tls_used[dwTlsIndex] = 0;
    pthread_mutex_unlock(&g_tls_mx);
    if (!was) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pthread_mutex_lock(&g_reg_mx);
    if (g_registry)
        for (auto& kv : *g_registry) kv.second->tls[dwTlsIndex] = nullptr;
    pthread_mutex_unlock(&g_reg_mx);
    return TRUE;
}

DWORD WINAPI FlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback) {
    pthread_mutex_lock(&g_tls_mx);
    for (uint32_t i = 0; i < kFlsSlots; i++)
        if (!g_fls_used[i]) {
            g_fls_used[i] = 1;
            g_fls_cb[i] = (gta6rt_gva)(uintptr_t)lpCallback;
            pthread_mutex_unlock(&g_tls_mx);
            return i;
        }
    pthread_mutex_unlock(&g_tls_mx);
    set_error(ERROR_NO_MORE_ITEMS);
    return FLS_OUT_OF_INDEXES;
}

PVOID WINAPI FlsGetValue(DWORD dwFlsIndex) {
    if (dwFlsIndex >= kFlsSlots) {
        set_error(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    set_error(ERROR_SUCCESS);
    return gta6rt_thread_current()->fls[dwFlsIndex];
}

BOOL WINAPI FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData) {
    if (dwFlsIndex >= kFlsSlots) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    gta6rt_thread_current()->fls[dwFlsIndex] = lpFlsData;
    return TRUE;
}

BOOL WINAPI FlsFree(DWORD dwFlsIndex) {
    if (dwFlsIndex >= kFlsSlots) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pthread_mutex_lock(&g_tls_mx);
    bool was = g_fls_used[dwFlsIndex];
    gta6rt_gva cb = g_fls_cb[dwFlsIndex];
    g_fls_used[dwFlsIndex] = 0;
    g_fls_cb[dwFlsIndex] = 0;
    pthread_mutex_unlock(&g_tls_mx);
    if (!was) {
        set_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    gta6rt_thread* self = gta6rt_thread_current();
    void* v = self->fls[dwFlsIndex];
    self->fls[dwFlsIndex] = nullptr;
    if (v && cb) {
        uint64_t a = (uint64_t)(uintptr_t)v;
        gta6rt_os_invoke(cb, &a, 1);
    }
    pthread_mutex_lock(&g_reg_mx);
    if (g_registry)
        for (auto& kv : *g_registry) kv.second->fls[dwFlsIndex] = nullptr;
    pthread_mutex_unlock(&g_reg_mx);
    return TRUE;
}

void WINAPI GetStartupInfoW(LPSTARTUPINFOW lpStartupInfo) {
    if (!lpStartupInfo) return;
    memset(lpStartupInfo, 0, sizeof *lpStartupInfo);
    lpStartupInfo->cb = sizeof *lpStartupInfo;
    lpStartupInfo->wShowWindow = 10;
}

BOOL WINAPI FlushProcessWriteBuffers(void) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return TRUE;
}

static BOOL g_priority_boost_disabled;
BOOL WINAPI SetProcessPriorityBoost(HANDLE hProcess, BOOL bDisablePriorityBoost) {
    if (H(hProcess) != kPseudoProcess) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    g_priority_boost_disabled = bDisablePriorityBoost;
    return TRUE;
}

BOOL WINAPI GetProcessPriorityBoost(HANDLE hProcess, PBOOL pDisablePriorityBoost) {
    if (H(hProcess) != kPseudoProcess) {
        set_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (pDisablePriorityBoost) *pDisablePriorityBoost = g_priority_boost_disabled;
    return TRUE;
}

// ---- toolhelp thread snapshots ----

namespace {
struct Snapshot : OtherObject {
    std::vector<uint32_t> tids;
    size_t next = 0;
    Snapshot() { kind = OtherKind::Misc; }
};
void snap_close(void* p) { delete (Snapshot*)p; }
void snap_noop(void*) {}

BOOL snap_fill(HANDLE h, LPTHREADENTRY32 e, bool first) {
    Snapshot* s = (Snapshot*)gta6rt_handle_lookup(H(h), GTA6RT_HANDLE_OTHER);
    if (!s || s->kind != OtherKind::Misc || !e || e->dwSize < sizeof(THREADENTRY32)) {
        set_error(s ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (first) s->next = 0;
    if (s->next >= s->tids.size()) {
        set_error(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    e->cntUsage = 0;
    e->th32ThreadID = s->tids[s->next++];
    e->th32OwnerProcessID = (DWORD)getpid();
    e->tpBasePri = 8;
    e->tpDeltaPri = 0;
    e->dwFlags = 0;
    return TRUE;
}
} // namespace

HANDLE WINAPI CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID) {
    if (th32ProcessID && th32ProcessID != (DWORD)getpid() && !(dwFlags & TH32CS_SNAPTHREAD)) {
        set_error(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    Snapshot* s = new Snapshot();
    if (dwFlags & TH32CS_SNAPTHREAD) {
        pthread_mutex_lock(&g_reg_mx);
        if (g_registry)
            for (auto& kv : *g_registry)
                if (!kv.second->finished) s->tids.push_back(kv.first);
        pthread_mutex_unlock(&g_reg_mx);
        std::sort(s->tids.begin(), s->tids.end());
    }
    return HV(handle_alloc_internal(GTA6RT_HANDLE_OTHER, s, snap_close, snap_noop));
}

BOOL WINAPI Thread32First(HANDLE hSnapshot, LPTHREADENTRY32 lpte) { return snap_fill(hSnapshot, lpte, true); }
BOOL WINAPI Thread32Next(HANDLE hSnapshot, LPTHREADENTRY32 lpte) { return snap_fill(hSnapshot, lpte, false); }

} // extern "C"
