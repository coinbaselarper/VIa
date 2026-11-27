// shims_core_import_bench (x86_64/Rosetta, real gta6rt_os + gta6rt_isa): ns per call of the host imports the
// title's frame loop calls most (top of the S1 MoltenVK trace), through the guest-callable thunk the resolver
// binds (the path the exe takes) and directly (the host function alone). Then the global wait lock under 1-8
// threads. Each figure is the best of 3 runs.

#include <windows.h>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include <gta6rt/isa.h>
#include <gta6rt/os.h>
#include <gta6rt/shims.h>

#include "shims.h"

namespace {

using F4 = uint64_t(GTA6RT_GUEST_ABI*)(uint64_t, uint64_t, uint64_t, uint64_t);
using F6 = uint64_t(GTA6RT_GUEST_ABI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using F7 = uint64_t(GTA6RT_GUEST_ABI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using FF = float(GTA6RT_GUEST_ABI*)(float);

struct Pair {
    F4 thunk = nullptr, direct = nullptr;
};

Pair bind(const char* dll, const char* name) {
    gta6rt_import_ref r{};
    r.dll = dll;
    r.name = name;
    r.kind_guess = GTA6RT_IMPORT_CODE;
    gta6rt_import_binding b{};
    Pair p;
    if (gta6rt_shims_import_resolver(nullptr, &r, &b) == GTA6RT_OK) p.thunk = reinterpret_cast<F4>(b.address);
    std::string d = dll;
    for (char& c : d) c = static_cast<char>(tolower(c));
    p.direct = reinterpret_cast<F4>(rt::shims::find(d, name));
    if (!p.thunk || !p.direct) std::fprintf(stderr, "bench: %s!%s not bound\n", dll, name);
    return p;
}

uint64_t now_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

double best_ns(int iters, const std::function<void()>& body) {
    double best = 1e30;
    for (int r = 0; r < 3; ++r) {
        const uint64_t t0 = now_ns();
        for (int i = 0; i < iters; ++i) body();
        best = std::min(best, double(now_ns() - t0) / iters);
    }
    return best;
}

struct Row {
    std::string name;
    double thunk, direct;
};
std::vector<Row> g_rows;

void row(const char* name, int iters, const std::function<void(bool)>& body) {
    std::fprintf(stderr, "bench: %s\n", name);
    const double t = best_ns(iters, [&] { body(true); });
    const double d = best_ns(iters, [&] { body(false); });
    g_rows.push_back({name, t, d});
}

GTA6RT_GUEST_ABI int cmp_int(const void* a, const void* b) {
    const int x = *static_cast<const int*>(a), y = *static_cast<const int*>(b);
    return (x > y) - (x < y);
}

struct ContArgs {
    F4 wait;
    HANDLE ev;
    int iters;
    double ns;
};

void* cont_thread(void* p) {
    auto* a = static_cast<ContArgs*>(p);
    gta6rt_thread* t = nullptr;
    gta6rt_thread_adopt_current(0, &t);
    const uint64_t t0 = now_ns();
    for (int i = 0; i < a->iters; ++i) a->wait(reinterpret_cast<uint64_t>(a->ev), 0, 0, 0);
    a->ns = double(now_ns() - t0) / a->iters;
    return nullptr;
}

struct PingArgs {
    F4 wait, set;
    uint64_t ping, pong;
    int rounds;
};

void* ping_thread(void* p) {
    auto* a = static_cast<PingArgs*>(p);
    gta6rt_thread* t = nullptr;
    gta6rt_thread_adopt_current(0, &t);
    for (int i = 0; i < a->rounds; ++i) {
        a->wait(a->ping, 0xffffffff, 0, 0);
        a->set(a->pong, 0, 0, 0);
    }
    return nullptr;
}

struct SemArgs {
    F4 wait, release;
    uint64_t sem;
    int iters;
    double ns;
};

void* sem_thread(void* p) {
    auto* a = static_cast<SemArgs*>(p);
    gta6rt_thread* t = nullptr;
    gta6rt_thread_adopt_current(0, &t);
    const uint64_t t0 = now_ns();
    for (int i = 0; i < a->iters; ++i) {
        a->release(a->sem, 1, 0, 0);
        a->wait(a->sem, 0, 0, 0);
    }
    a->ns = double(now_ns() - t0) / a->iters;
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    if (std::strcmp(gta6rt_isa_backend_name(), "rosetta") != 0) {
        std::printf("shims_core_import_bench: SKIP (backend %s)\n", gta6rt_isa_backend_name());
        return 0;
    }
    const int scale = argc > 1 ? std::max(1, std::atoi(argv[1])) : 1;
    gta6rt_isa_config cfg{};
    cfg.teb_mode = GTA6RT_TEB_SHARE;
    gta6rt_isa_init(&cfg);
    static uint8_t peb[0x400];
    gta6rt_isa_set_peb(peb);
    gta6rt_teb_desc d{};
    d.peb = peb;
    d.process_id = static_cast<uint32_t>(getpid());
    d.thread_id = 0x104;
    d.stack_base = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    d.stack_limit = d.stack_base - pthread_get_stacksize_np(pthread_self());
    gta6rt_isa_thread_attach(&d, nullptr);
    gta6rt_thread* self = nullptr;
    gta6rt_thread_adopt_current(1, &self);
    gta6rt_shims_init();
    const char* tmp = "/tmp/gta6rt_import_bench";
    mkdir(tmp, 0755);
    mkdir("/tmp/gta6rt_import_bench/game", 0755);
    mkdir("/tmp/gta6rt_import_bench/overlay", 0755);
    gta6rt_shims_configure_paths((std::string(tmp) + "/game").c_str(), (std::string(tmp) + "/overlay").c_str());

    const int N = 200000 * scale;
    const char* K = "kernel32.dll";
    const char* U = "ucrtbase.dll";

    Pair sleep_ = bind(K, "Sleep"), qpc = bind(K, "QueryPerformanceCounter"), tid = bind(K, "GetCurrentThreadId"),
         gle = bind(K, "GetLastError"), tlsget = bind(K, "TlsGetValue"), tlsalloc = bind(K, "TlsAlloc"),
         initcs = bind(K, "InitializeCriticalSection"), ecs = bind(K, "EnterCriticalSection"),
         lcs = bind(K, "LeaveCriticalSection"), asrw = bind(K, "AcquireSRWLockExclusive"),
         rsrw = bind(K, "ReleaseSRWLockExclusive"), heap = bind(K, "GetProcessHeap"), halloc = bind(K, "HeapAlloc"),
         hfree = bind(K, "HeapFree"), cevent = bind(K, "CreateEventW"), wfso = bind(K, "WaitForSingleObject"),
         setev = bind(K, "SetEvent"), cmutex = bind(K, "CreateMutexW"), rmutex = bind(K, "ReleaseMutex"),
         csem = bind(K, "CreateSemaphoreW"), rsem = bind(K, "ReleaseSemaphore"), strcmp_ = bind(U, "strcmp"),
         strnicmp = bind(U, "_strnicmp"), strlen_ = bind(U, "strlen"), strncmp_ = bind(U, "strncmp"),
         stricmp = bind(U, "_stricmp"), exp2f_ = bind(U, "exp2f"), log10f_ = bind(U, "log10f"),
         cosf_ = bind(U, "cosf"), qsort_ = bind(U, "qsort"), vsprintf = bind(U, "__stdio_common_vsprintf"),
         strtod_ = bind(U, "strtod"), createf = bind(K, "CreateFileW"), writef = bind(K, "WriteFile"),
         readf = bind(K, "ReadFile"), setfp = bind(K, "SetFilePointerEx"), xalloc = bind("xmem.dll", "XMemAllocatePhysicalPages"),
         xva = bind("xmem.dll", "XMemVirtualAlloc"), xmap = bind("xmem.dll", "XMemMapPhysicalPages");

    auto pick = [](const Pair& p, bool t) { return t ? p.thunk : p.direct; };

    const bool waits_only = argc > 2 && std::strcmp(argv[2], "waits") == 0;
    if (!waits_only) {
    row("Sleep(0)", N / 4, [&](bool t) { pick(sleep_, t)(0, 0, 0, 0); });
    LARGE_INTEGER li;
    row("QueryPerformanceCounter", N, [&](bool t) { pick(qpc, t)(reinterpret_cast<uint64_t>(&li), 0, 0, 0); });
    row("GetCurrentThreadId", N, [&](bool t) { pick(tid, t)(0, 0, 0, 0); });
    row("GetLastError", N, [&](bool t) { pick(gle, t)(0, 0, 0, 0); });
    const uint64_t slot = tlsalloc.direct(0, 0, 0, 0);
    row("TlsGetValue", N, [&](bool t) { pick(tlsget, t)(slot, 0, 0, 0); });
    alignas(16) static uint8_t cs[40];
    initcs.direct(reinterpret_cast<uint64_t>(cs), 0, 0, 0);
    row("Enter+LeaveCriticalSection", N, [&](bool t) {
        pick(ecs, t)(reinterpret_cast<uint64_t>(cs), 0, 0, 0);
        pick(lcs, t)(reinterpret_cast<uint64_t>(cs), 0, 0, 0);
    });
    static uint64_t srw = 0;
    row("Acquire+ReleaseSRWLockExclusive", N, [&](bool t) {
        pick(asrw, t)(reinterpret_cast<uint64_t>(&srw), 0, 0, 0);
        pick(rsrw, t)(reinterpret_cast<uint64_t>(&srw), 0, 0, 0);
    });
    const uint64_t ph = heap.direct(0, 0, 0, 0);
    row("HeapAlloc(64)+HeapFree", N, [&](bool t) {
        const uint64_t p = pick(halloc, t)(ph, 0, 64, 0);
        pick(hfree, t)(ph, 0, p, 0);
    });
    const uint64_t ev = cevent.direct(0, 1, 1, 0);
    row("WaitForSingleObject(set event, 0)", N, [&](bool t) { pick(wfso, t)(ev, 0, 0, 0); });
    row("SetEvent(set event)", N, [&](bool t) { pick(setev, t)(ev, 0, 0, 0); });
    const uint64_t mx = cmutex.direct(0, 0, 0, 0);
    row("WaitForSingleObject(mutex)+ReleaseMutex", N, [&](bool t) {
        pick(wfso, t)(mx, 0xffffffff, 0, 0);
        pick(rmutex, t)(mx, 0, 0, 0);
    });
    const uint64_t sem = csem.direct(0, 0, 0x7fffffff, 0);
    row("ReleaseSemaphore+WaitForSingleObject", N, [&](bool t) {
        pick(rsem, t)(sem, 1, 0, 0);
        pick(wfso, t)(sem, 0, 0, 0);
    });
    const char* a = "render_target_shadow_cascade_03";
    const char* b = "render_target_shadow_cascade_04";
    row("strcmp(31 chars)", N, [&](bool t) { pick(strcmp_, t)(reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), 0, 0); });
    row("_strnicmp(16)", N, [&](bool t) { pick(strnicmp, t)(reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), 16, 0); });
    row("_stricmp(31 chars)", N, [&](bool t) { pick(stricmp, t)(reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), 0, 0); });
    row("strncmp(16)", N, [&](bool t) { pick(strncmp_, t)(reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), 16, 0); });
    row("strlen(31 chars)", N, [&](bool t) { pick(strlen_, t)(reinterpret_cast<uint64_t>(a), 0, 0, 0); });
    volatile float sink = 0;
    row("exp2f", N, [&](bool t) { sink = sink + reinterpret_cast<FF>(pick(exp2f_, t))(1.5f); });
    row("log10f", N, [&](bool t) { sink = sink + reinterpret_cast<FF>(pick(log10f_, t))(3.5f); });
    row("cosf", N, [&](bool t) { sink = sink + reinterpret_cast<FF>(pick(cosf_, t))(0.7f); });
    int arr[16];
    row("qsort(16 ints)", N / 10, [&](bool t) {
        for (int i = 0; i < 16; ++i) arr[i] = (i * 7919) % 16;
        pick(qsort_, t)(reinterpret_cast<uint64_t>(arr), 16, 4, reinterpret_cast<uint64_t>(&cmp_int));
    });
    char out[64];
    uint64_t va[2] = {42, reinterpret_cast<uint64_t>("shadow")};
    row("__stdio_common_vsprintf(\"%d %s\")", N / 4, [&](bool t) {
        reinterpret_cast<F6>(pick(vsprintf, t))(0, reinterpret_cast<uint64_t>(out), sizeof out,
                                                reinterpret_cast<uint64_t>("%d %s"), 0, reinterpret_cast<uint64_t>(va));
    });
    row("strtod", N, [&](bool t) { pick(strtod_, t)(reinterpret_cast<uint64_t>("3.25e2"), 0, 0, 0); });

    static const char16_t path[] = u"T:\\bench.bin";
    const uint64_t fh = reinterpret_cast<F7>(createf.direct)(reinterpret_cast<uint64_t>(path), 0xC0000000u, 0, 0, 2, 0x80, 0);
    if (fh == 0 || fh == ~0ull) std::fprintf(stderr, "bench: CreateFileW failed (%u)\n", static_cast<unsigned>(gle.direct(0, 0, 0, 0)));
    std::vector<uint8_t> block(4096, 7);
    DWORD done = 0;
    for (int i = 0; i < 256; ++i)
        reinterpret_cast<F6>(writef.direct)(fh, reinterpret_cast<uint64_t>(block.data()), block.size(), reinterpret_cast<uint64_t>(&done), 0, 0);
    int nread = 0;
    row("ReadFile(4 KiB)", N / 10, [&](bool t) {
        if ((nread++ & 255) == 0) setfp.direct(fh, 0, 0, 0);
        reinterpret_cast<F6>(pick(readf, t))(fh, reinterpret_cast<uint64_t>(block.data()), block.size(), reinterpret_cast<uint64_t>(&done), 0, 0);
    });

    uint64_t count = 1, page = 0;
    xalloc.direct(1, reinterpret_cast<uint64_t>(&count), reinterpret_cast<uint64_t>(&page), 0);
    const uint64_t win = reinterpret_cast<F6>(xva.direct)(0, 0x10000, 0x20402000u, 1, 4, 0);
    if (!win || !page) std::fprintf(stderr, "bench: xmem window %#llx page %#llx\n", (unsigned long long)win, (unsigned long long)page);
    row("XMemMapPhysicalPages(map 1)+(unmap 1)", N / 10, [&](bool t) {
        pick(xmap, t)(win, 1, reinterpret_cast<uint64_t>(&page), 0);
        pick(xmap, t)(win, 1, 0, 0);
    });

    std::printf("%-42s %12s %12s %12s\n", "import (per call or pair)", "thunk ns", "direct ns", "thunk extra");
    for (const Row& r : g_rows) std::printf("%-42s %12.1f %12.1f %12.1f\n", r.name.c_str(), r.thunk, r.direct, r.thunk - r.direct);
    }

    for (int threads : {1, 2, 4, 8}) {
        std::vector<ContArgs> args(threads);
        std::vector<pthread_t> th(threads);
        for (int i = 0; i < threads; ++i) {
            args[i] = {wfso.thunk, reinterpret_cast<HANDLE>(cevent.direct(0, 1, 1, 0)), N, 0};
            pthread_create(&th[i], nullptr, cont_thread, &args[i]);
        }
        double sum = 0;
        for (int i = 0; i < threads; ++i) {
            pthread_join(th[i], nullptr);
            sum += args[i].ns;
        }
        std::printf("WaitForSingleObject(own set event, 0), %d thread(s): %.1f ns per call per thread\n", threads, sum / threads);
    }
    {
        PingArgs pa{wfso.thunk, setev.thunk, cevent.direct(0, 0, 0, 0), cevent.direct(0, 0, 0, 0), N / 10};
        pthread_t pt;
        pthread_create(&pt, nullptr, ping_thread, &pa);
        const uint64_t t0 = now_ns();
        for (int i = 0; i < pa.rounds; ++i) {
            pa.set(pa.ping, 0, 0, 0);
            pa.wait(pa.pong, 0xffffffff, 0, 0);
        }
        const double ns = double(now_ns() - t0) / pa.rounds;
        pthread_join(pt, nullptr);
        std::printf("SetEvent -> WaitForSingleObject(INFINITE) ping-pong, 2 threads: %.1f ns per round trip\n", ns);
    }
    for (int threads : {2, 4, 8}) {
        const uint64_t sem = csem.direct(0, 0, 0x7fffffff, 0);
        std::vector<SemArgs> args(threads);
        std::vector<pthread_t> th(threads);
        for (int i = 0; i < threads; ++i) {
            args[i] = {wfso.thunk, rsem.thunk, sem, N / 4, 0};
            pthread_create(&th[i], nullptr, sem_thread, &args[i]);
        }
        double sum = 0;
        for (int i = 0; i < threads; ++i) {
            pthread_join(th[i], nullptr);
            sum += args[i].ns;
        }
        std::printf("ReleaseSemaphore+WaitForSingleObject(0) on one shared semaphore, %d threads: %.1f ns per pair per thread\n",
                    threads, sum / threads);
    }
    std::printf("shims_core_import_bench: done\n");
    return 0;
}
