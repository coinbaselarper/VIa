// End-to-end exception dispatch through seh with the packaged vcruntime140.dll / vcruntime140_1.dll mapped unchanged.
// Guest code: tests/fixture/guest_eh.cpp compiled by clang for x86_64-pc-windows-msvc (real MSVC-ABI FH3 tables and
// SEH scope tables), linked in memory by coff_link.h; and, when exe/ is present, the title's own FH4 function at
// rva 0x55f3b70 (catch-by-type HRESULT translator). Host functions stand in for the CRT/kernel32 imports vcruntime
// needs; Rtl*/RaiseException/filters are seh's own exports. x86_64 (Rosetta) only.
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gta6rt/isa.h>
#include <gta6rt/seh.h>

#include "coff_link.h"
#include "pe_image.h"

#define MSABI __attribute__((ms_abi))

namespace {

int g_fail = 0, g_checks = 0;

#define CHECK(c)                                                  \
    do {                                                          \
        ++g_checks;                                               \
        if (!(c)) {                                               \
            ++g_fail;                                             \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                         \
    } while (0)

// ---- host stand-ins for what vcruntime140(_1) imports -------------------------------------------------------------

std::vector<std::pair<int, long long>> g_events;
bool g_in_child = false;
thread_local uint32_t t_last_error;
thread_local void* t_fls[128];
uint32_t g_fls_next = 1;

MSABI void host_note(int what, long long v) { g_events.push_back({what, v}); }
MSABI void host_delete(void* p, size_t) { free(p); }
MSABI void* x_malloc(size_t n) { return malloc(n); }
MSABI void* x_calloc(size_t n, size_t m) { return calloc(n, m); }
MSABI void x_free(void* p) { free(p); }
MSABI int x_strncmp(const char* a, const char* b, size_t n) { return strncmp(a, b, n); }
MSABI int x_strcpy_s(char* d, size_t n, const char* s) {
    if (!d || !n || !s) return 22;
    const size_t l = strlen(s);
    if (l >= n) {
        d[0] = 0;
        return 34;
    }
    memcpy(d, s, l + 1);
    return 0;
}
MSABI long x_atol(const char* s) { return atol(s); }
MSABI int x_vsprintf_s(uint64_t, char* buf, size_t n, const char* fmt, void*, void*) {
    if (buf && n) snprintf(buf, n, "%s", fmt ? fmt : "");
    return buf && n ? static_cast<int>(strlen(buf)) : -1;
}
MSABI void x_terminate() {
    fprintf(stderr, "[test] terminate() called\n");
    if (g_in_child) _exit(42);
    abort();
}
MSABI void x_abort() {
    fprintf(stderr, "[test] abort() called by guest\n");
    abort();
}
MSABI void* x_encode(void* p) { return p; }
MSABI uint32_t x_get_last_error() { return t_last_error; }
MSABI void x_set_last_error(uint32_t e) { t_last_error = e; }
std::mutex g_slist_mutex;
MSABI void* x_slist_push(void* head, void* entry) {
    std::lock_guard<std::mutex> l(g_slist_mutex);
    void* old;
    memcpy(&old, head, 8);
    memcpy(entry, &old, 8);
    memcpy(head, &entry, 8);
    return old;
}
MSABI void* x_slist_flush(void* head) {
    std::lock_guard<std::mutex> l(g_slist_mutex);
    void* old;
    memcpy(&old, head, 8);
    memset(head, 0, 16);
    return old;
}
MSABI int x_init_cs(void* cs, uint32_t, uint32_t) {
    auto* m = new pthread_mutex_t;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    memset(cs, 0, 40);
    memcpy(cs, &m, 8);
    return 1;
}
pthread_mutex_t* cs_mutex(void* cs) {
    pthread_mutex_t* m;
    memcpy(&m, cs, 8);
    return m;
}
MSABI void x_enter_cs(void* cs) { pthread_mutex_lock(cs_mutex(cs)); }
MSABI void x_leave_cs(void* cs) { pthread_mutex_unlock(cs_mutex(cs)); }
MSABI void x_delete_cs(void* cs) { pthread_mutex_destroy(cs_mutex(cs)); }
MSABI uint32_t x_fls_alloc(void*) { return g_fls_next < 128 ? g_fls_next++ : 0xffffffffu; }
MSABI void* x_fls_get(uint32_t i) { return i < 128 ? t_fls[i] : nullptr; }
MSABI int x_fls_set(uint32_t i, void* v) {
    if (i >= 128) return 0;
    t_fls[i] = v;
    return 1;
}
MSABI int x_fls_free(uint32_t) { return 1; }
MSABI void* x_null_handle(const void*, void*, uint32_t) { return nullptr; }
MSABI void* x_get_module_handle(const void*) { return nullptr; }
MSABI uint32_t x_get_module_file_name(void*, void*, uint32_t) { return 0; }
MSABI int x_qpc(int64_t* v) {
    *v = static_cast<int64_t>(mach_absolute_time());
    return 1;
}
MSABI void x_filetime(uint64_t* ft) { *ft = 133000000000000000ull + static_cast<uint64_t>(time(nullptr)) * 10000000ull; }
MSABI uint32_t x_tid() { return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self())); }
MSABI uint32_t x_pid() { return static_cast<uint32_t>(getpid()); }
MSABI void* x_current_process() { return reinterpret_cast<void*>(~0ull); }
MSABI int x_terminate_process(void*, uint32_t code) {
    fprintf(stderr, "[test] TerminateProcess(%u)\n", code);
    _exit(static_cast<int>(code & 0xff));
}
MSABI int x_feature(uint32_t) { return 0; }

struct named {
    const char* name;
    uint64_t addr;
};
#define E(n, f) {n, reinterpret_cast<uint64_t>(&f)}
const named g_env[] = {
    E("malloc", x_malloc), E("calloc", x_calloc), E("free", x_free), E("strncmp", x_strncmp), E("strcpy_s", x_strcpy_s),
    E("atol", x_atol), E("__stdio_common_vsprintf_s", x_vsprintf_s), E("terminate", x_terminate), E("abort", x_abort),
    E("EncodePointer", x_encode), E("GetLastError", x_get_last_error), E("SetLastError", x_set_last_error),
    E("InterlockedPushEntrySList", x_slist_push), E("InterlockedFlushSList", x_slist_flush),
    E("InitializeCriticalSectionEx", x_init_cs), E("EnterCriticalSection", x_enter_cs),
    E("LeaveCriticalSection", x_leave_cs), E("DeleteCriticalSection", x_delete_cs), E("FlsAlloc", x_fls_alloc),
    E("FlsGetValue", x_fls_get), E("FlsSetValue", x_fls_set), E("FlsFree", x_fls_free),
    E("LoadLibraryExW", x_null_handle), E("GetModuleHandleW", x_get_module_handle),
    E("GetModuleFileNameW", x_get_module_file_name), E("QueryPerformanceCounter", x_qpc),
    E("GetSystemTimeAsFileTime", x_filetime), E("GetCurrentThreadId", x_tid), E("GetCurrentProcessId", x_pid),
    E("GetCurrentProcess", x_current_process), E("TerminateProcess", x_terminate_process),
    E("IsProcessorFeaturePresent", x_feature),
};
#undef E

// Vectored handler in the style of leonida's fault fix-ups: emulate the faulting load and resume.
MSABI long veh_fixup(void* pointers) {
    auto** p = static_cast<void**>(pointers);
    const auto* rec = static_cast<const gta6rt_x64_exception_record*>(p[0]);
    auto* ctx = static_cast<gta6rt_x64_context*>(p[1]);
    if (rec->code != 0xC0000005u || rec->n_params < 2 || rec->params[1] != 0x20) return 0;
    const auto* ip = reinterpret_cast<const uint8_t*>(ctx->rip);
    if (ip[0] != 0x8b || ip[1] != 0x01) return 0;
    ctx->rax = 77;
    ctx->rip += 2;
    host_note(12, static_cast<long long>(rec->params[1]));
    return -1;
}

void own_segv_handler(int sig, siginfo_t* si, void* uc) {
    if (!gta6rt_seh_handle_signal(sig, si, uc)) {
        signal(sig, SIG_DFL);
        return;
    }
}

uint8_t* g_traps;
uint32_t g_ntraps;

MSABI void unbound_import(const char* name) {
    fprintf(stderr, "[test] unbound import called: %s\n", name);
    abort();
}

uint64_t make_trap(const char* dll, const char* fn) {
    if (!g_traps)
        g_traps = static_cast<uint8_t*>(
            mmap(nullptr, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0));
    char* name = static_cast<char*>(malloc(256));
    snprintf(name, 256, "%s!%s", dll, fn ? fn : "(ordinal)");
    uint8_t* t = g_traps + 32 * g_ntraps++;
    const uint64_t n = reinterpret_cast<uint64_t>(name), f = reinterpret_cast<uint64_t>(&unbound_import);
    t[0] = 0x48;
    t[1] = 0xb9;
    memcpy(t + 2, &n, 8);
    t[10] = 0x48;
    t[11] = 0xb8;
    memcpy(t + 12, &f, 8);
    t[20] = 0xff;
    t[21] = 0xe0;
    return reinterpret_cast<uint64_t>(t);
}

uint64_t env_lookup(const char* name) {
    if (!name) return 0;
    if (const uint64_t s = gta6rt_seh_native_export(name)) return s;
    for (const named& n : g_env)
        if (!strcmp(n.name, name)) return n.addr;
    return 0;
}

// ---- images --------------------------------------------------------------------------------------------------------

struct world {
    pe::image vc, vc1, exe;
    coff::image fx, fx_fallback;
    bool have_exe = false, have_fallback = false;
};
world W;

bool register_image(uint64_t base, uint32_t size, const void* pdata, uint32_t count) {
    gta6rt_isa_register_guest_code(base, size);
    return gta6rt_seh_add_function_table(base, static_cast<const gta6rt_runtime_function*>(pdata), count, size) ==
           GTA6RT_OK;
}

bool register_pe(const pe::image& im) {
    return register_image(im.base, im.size, im.mem + im.dir_rva(3), im.dir_size(3) / 12);
}

using dllmain_fn = int(MSABI*)(void*, uint32_t, void*);

bool load_dll(const char* path, pe::image* im, const pe::image* vc) {
    if (!pe::map(path, 0, im) && !pe::map(path, ~0ull, im)) return false;
    pe::relocate(*im);
    pe::bind_imports(
        *im,
        [&](const char* dll, const char* fn, uint32_t) -> uint64_t {
            if (vc && !strcasecmp(dll, "VCRUNTIME140.dll")) return fn ? pe::export_addr(*vc, fn) : 0;
            return env_lookup(fn);
        },
        make_trap);
    if (!register_pe(*im)) return false;
    const auto entry = reinterpret_cast<dllmain_fn>(im->base + im->entry_rva());
    const int ok = entry(im->mem, 1, nullptr);
    printf("  %s mapped at %#llx (preferred %#llx), DllMain -> %d\n", path, static_cast<unsigned long long>(im->base),
           static_cast<unsigned long long>(im->preferred), ok);
    return ok != 0;
}

// MSVC links type_info's vtable statically into each image (it is not a vcruntime140 export); EH type matching
// only reads the TypeDescriptor name, so a stand-in whose slots trap is enough.
uint64_t g_type_info_vtable[4];

uint64_t vc_export(const char* n) {
    if (!strcmp(n, "??_7type_info@@6B@")) {
        if (!g_type_info_vtable[0])
            for (int i = 0; i < 4; ++i) g_type_info_vtable[i] = make_trap("type_info", "vtable slot");
        return reinterpret_cast<uint64_t>(&g_type_info_vtable[0]);
    }
    if (const uint64_t a = pe::export_addr(W.vc, n)) return a;
    return pe::export_addr(W.vc1, n);
}

bool link_fixture(const char* obj, bool fallback, coff::image* out) {
    std::string err;
    const bool ok = coff::link(
        obj,
        [&](const std::string& n) -> uint64_t {
            if (n == "host_note") return reinterpret_cast<uint64_t>(&host_note);
            if (n == "??3@YAXPEAX_K@Z") return reinterpret_cast<uint64_t>(&host_delete);
            if (fallback)
                if (const uint64_t f = gta6rt_seh_vcruntime_fallback(n.c_str())) return f;
            if (const uint64_t s = gta6rt_seh_native_export(n.c_str())) return s;
            return vc_export(n.c_str());
        },
        out, &err);
    if (!ok) {
        printf("  fixture link failed: %s\n", err.c_str());
        return false;
    }
    return register_image(out->base, out->size, out->pdata, out->pdata_count);
}

uint64_t fx(const coff::image& im, const char* name) {
    const uint64_t a = im.sym(name);
    if (!a) {
        printf("  missing fixture symbol %s\n", name);
        exit(1);
    }
    return a;
}

using fn0 = long long(MSABI*)();
using fn1 = long long(MSABI*)(uint64_t);
using fn2 = long long(MSABI*)(uint64_t, uint64_t);

long long call0(const coff::image& im, const char* n) { return reinterpret_cast<fn0>(fx(im, n))(); }
long long call1(const coff::image& im, const char* n, uint64_t a) { return reinterpret_cast<fn1>(fx(im, n))(a); }
long long call2(const coff::image& im, const char* n, uint64_t a, uint64_t b) {
    return reinterpret_cast<fn2>(fx(im, n))(a, b);
}

std::string events() {
    std::string s;
    for (auto& e : g_events) {
        char b[48];
        snprintf(b, sizeof b, "%s%d:%lld", s.empty() ? "" : " ", e.first, e.second);
        s += b;
    }
    return s;
}

void expect(const char* what, long long got, long long want, const char* want_events = nullptr) {
    const std::string ev = events();
    const bool ok = got == want && (!want_events || ev == want_events);
    ++g_checks;
    if (!ok) ++g_fail;
    printf("  %-44s -> %#llx %s  [events %s]\n", what, static_cast<unsigned long long>(got), ok ? "ok" : "FAIL",
           ev.c_str());
    if (!ok) printf("      expected %#llx [events %s]\n", static_cast<unsigned long long>(want), want_events ? want_events : "any");
    g_events.clear();
}

struct child_out {
    int status = 0;
    std::string err;
};

child_out run_child(const std::function<void()>& body, unsigned timeout_s = 60) {
    child_out r;
    int p[2];
    if (pipe(p)) return r;
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 2);
        close(p[0]);
        g_in_child = true;
        alarm(timeout_s);
        body();
        _exit(0);
    }
    close(p[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(p[0], buf, sizeof buf)) > 0) r.err.append(buf, static_cast<size_t>(n));
    close(p[0]);
    waitpid(pid, &r.status, 0);
    return r;
}

bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

// Frame lines of a fatal report: "#N  0x..." -> addresses.
std::vector<uint64_t> frames_of(const std::string& log, uint64_t* throw_site) {
    std::vector<uint64_t> out;
    size_t pos = 0;
    *throw_site = 0;
    while ((pos = log.find("    #", pos)) != std::string::npos) {
        const size_t x = log.find("0x", pos);
        const size_t eol = log.find('\n', pos);
        if (x == std::string::npos || x > eol) break;
        const uint64_t a = strtoull(log.c_str() + x + 2, nullptr, 16);
        out.push_back(a);
        if (log.substr(pos, eol - pos).find("<- throw site") != std::string::npos) *throw_site = a;
        pos = eol;
    }
    return out;
}

bool in_function(const coff::image& im, const char* name, uint64_t a) {
    const uint64_t b = im.sym(name);
    for (uint32_t i = 0; i < im.pdata_count; ++i)
        if (im.base + im.pdata[i].begin_rva == b) return a >= b && a < im.base + im.pdata[i].end_rva;
    return false;
}

void show_child(const char* what, const child_out& c) {
    printf("  child '%s': %s %d\n", what, WIFSIGNALED(c.status) ? "signal" : "exit",
           WIFSIGNALED(c.status) ? WTERMSIG(c.status) : WEXITSTATUS(c.status));
    size_t start = 0, lines = 0;
    while (start < c.err.size() && lines < 40) {
        size_t e = c.err.find('\n', start);
        if (e == std::string::npos) e = c.err.size();
        printf("      | %s\n", c.err.substr(start, e - start).c_str());
        start = e + 1;
        ++lines;
    }
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* pkg = argc > 1 ? argv[1] : SEH_PACKAGE_DIR;
    const char* obj = argc > 2 ? argv[2] : SEH_FIXTURE_OBJ;
    const char* exe = argc > 3 ? argv[3] : SEH_EXE;
    gta6rt_isa_config cfg{GTA6RT_TEB_SHARE, 0};
    CHECK(gta6rt_isa_init(&cfg) == GTA6RT_OK);
    CHECK(gta6rt_seh_init(GTA6RT_SEH_DISPATCH) == GTA6RT_OK);
    CHECK(gta6rt_seh_get_policy() == GTA6RT_SEH_DISPATCH);

    printf("loading\n");
    std::string p0 = std::string(pkg) + "/vcruntime140.dll", p1 = std::string(pkg) + "/vcruntime140_1.dll";
    if (!load_dll(p0.c_str(), &W.vc, nullptr) || !load_dll(p1.c_str(), &W.vc1, &W.vc)) {
        printf("seh_dispatch_test: FAIL (could not load the packaged vcruntime)\n");
        return 1;
    }
    if (!link_fixture(obj, false, &W.fx)) {
        printf("seh_dispatch_test: FAIL (fixture)\n");
        return 1;
    }
    printf("  fixture %s linked at %#llx, %u RUNTIME_FUNCTIONs\n", obj, static_cast<unsigned long long>(W.fx.base),
           W.fx.pdata_count);
    W.have_fallback = link_fixture(obj, true, &W.fx_fallback);
    if (pe::map(exe, 0, &W.exe)) {
        pe::bind_imports(
            W.exe,
            [&](const char* dll, const char* fn, uint32_t) -> uint64_t {
                if (fn && (!strcasecmp(dll, "VCRUNTIME140.dll") || !strcasecmp(dll, "VCRUNTIME140_1.dll")))
                    return vc_export(fn);
                return 0;
            },
            make_trap);
        W.have_exe = register_pe(W.exe);
        printf("  exe mapped at %#llx, %u RUNTIME_FUNCTIONs, imports bound (vcruntime only, %u trap thunks)\n",
               static_cast<unsigned long long>(W.exe.base), W.exe.dir_size(3) / 12, g_ntraps);
    } else {
        printf("  exe not mapped: the FH4 scenario is skipped\n");
    }

    printf("C++ exceptions through __CxxFrameHandler3 (packaged vcruntime140.dll)\n");
    expect("throw int, catch(int) two frames up", call0(W.fx, "fx_catch_int"), 42, "1:200 1:100 2:100 2:200");
    expect("throw derived, catch(base&)", call0(W.fx, "fx_catch_base_by_ref"), 9);
    expect("catch(my_error) by value", call0(W.fx, "fx_catch_by_value"), 17);
    expect("catch(...)", call0(W.fx, "fx_catch_all"), 1);
    expect("rethrow from catch, caught outside", call0(W.fx, "fx_rethrow"), 1007);
    expect("throw and catch inside a catch block", call0(W.fx, "fx_throw_in_catch"), 4207);
    expect("inner try does not match, outer does", call0(W.fx, "fx_outer_catches"), 43);
    {
        const uint64_t t0 = mach_absolute_time();
        const long long s = call1(W.fx, "fx_loop", 2000);
        const uint64_t t1 = mach_absolute_time();
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        const double us = static_cast<double>(t1 - t0) * tb.numer / tb.denom / 1000.0 / 2000.0;
        g_events.clear();
        expect("2000 throw/catch iterations", s, 1000 * 42 + 1000 * 7);
        printf("    %.1f us per throw/catch\n", us);
    }

    printf("SEH through __C_specific_handler (packaged vcruntime140.dll)\n");
    expect("RaiseException, __except filter function", call0(W.fx, "fx_seh_raise"), 5, "8:3758096450");
    expect("RaiseException, filter -> CONTINUE_EXECUTION", call0(W.fx, "fx_seh_continue"), 11, "8:3758096452 10:1");
    expect("__finally runs during the unwind", call0(W.fx, "fx_seh_finally"), 7, "5:1");
    expect("hardware fault (read of 0x10) -> __except", call1(W.fx, "fx_seh_av", 0x10), 6, "8:3221225477");
    {
        const uint64_t fn = W.fx.sym("fx_av_through_cxx");
        uint32_t magic = 0, ehflags = 0;
        for (uint32_t i = 0; i < W.fx.pdata_count; ++i) {
            if (W.fx.base + W.fx.pdata[i].begin_rva != fn) continue;
            const uint8_t* ui = W.fx.mem + W.fx.pdata[i].unwind_info_rva;
            const uint32_t fi = pe::u32(ui + 4 + 2 * ((ui[2] + 1) & ~1) + 4);
            magic = pe::u32(W.fx.mem + fi);
            ehflags = pe::u32(W.fx.mem + fi + 36);
        }
        printf("    fx_av_through_cxx FuncInfo magic %#x EHFlags %#x (bit 0 = /EHs: FH3 ignores non-C++ codes)\n", magic,
               ehflags);
        expect("hardware fault unwinds an /EHs C++ frame", call1(W.fx, "fx_seh_av_unwinds_cxx", 0x10), 8,
               (ehflags & 1) ? "1:300" : "1:300 2:300");
    }

    {
        void* cookie = nullptr;
        CHECK(gta6rt_seh_add_vectored_handler(1, reinterpret_cast<uint64_t>(&veh_fixup), &cookie) == GTA6RT_OK);
        expect("vectored handler fixes a fault and resumes", call1(W.fx, "fx_seh_av", 0x20), 77, "12:32");
        expect("same VEH ignores other faults -> __except", call1(W.fx, "fx_seh_av", 0x10), 6, "8:3221225477");
        CHECK(gta6rt_seh_remove_vectored_handler(cookie) == GTA6RT_OK);
    }

    if (W.have_exe) {
        printf("FH4: the title's own catch-by-type translator (exe rva 0x55f3b70, packaged vcruntime140_1.dll)\n");
        const uint64_t t = W.exe.base + 0x55f3b70;
        auto hr = [](uint32_t v) { return static_cast<long long>(static_cast<int32_t>(v)); };
        expect("std::bad_alloc rethrown into exe catch", call2(W.fx, "fx_exe_translate", t, 3), hr(0x8007000E));
        expect("long (HRESULT) rethrown into exe catch(long)", call2(W.fx, "fx_exe_translate", t, 4), hr(0x80001234));
        expect("int rethrown into exe catch(...)", call2(W.fx, "fx_exe_translate", t, 0), hr(0x80004005));
        expect("my_error rethrown into exe catch(...)", call2(W.fx, "fx_exe_translate", t, 1), hr(0x80004005));
    }
    expect("after FH4, FH3 still works", call0(W.fx, "fx_catch_int"), 42);

    printf("abort paths (forked children)\n");
    {
        child_out c = run_child([] {
            gta6rt_seh_set_policy(GTA6RT_SEH_LOG_AND_ABORT);
            call0(W.fx, "fx_catch_int");
        });
        show_child("LOG_AND_ABORT, throw int", c);
        uint64_t site = 0;
        const std::vector<uint64_t> fr = frames_of(c.err, &site);
        CHECK(WIFSIGNALED(c.status) && WTERMSIG(c.status) == SIGABRT);
        CHECK(contains(c.err, "SEH policy LOG_AND_ABORT") && contains(c.err, "0xe06d7363 C++ exception"));
        CHECK(contains(c.err, "C++ type int"));
        CHECK(in_function(W.fx, "?fx_thrower@@YAXH@Z", site));
        bool saw_middle = false, saw_caller = false;
        for (uint64_t a : fr) {
            saw_middle |= in_function(W.fx, "?fx_middle@@YAXH@Z", a);
            saw_caller |= in_function(W.fx, "fx_catch_int", a);
        }
        CHECK(saw_middle && saw_caller);
    }
    {
        child_out c = run_child([] {
            setenv("GTA6RT_SEH_POLICY", "log_and_abort", 1);
            gta6rt_seh_init(GTA6RT_SEH_DISPATCH);
            if (gta6rt_seh_get_policy() != GTA6RT_SEH_LOG_AND_ABORT) _exit(3);
            call0(W.fx, "fx_catch_base_by_ref");
        });
        show_child("GTA6RT_SEH_POLICY=log_and_abort, throw derived_error", c);
        CHECK(WIFSIGNALED(c.status) && WTERMSIG(c.status) == SIGABRT);
        CHECK(contains(c.err, "derived_error") && contains(c.err, "catchable as:") && contains(c.err, "my_error"));
    }
    {
        child_out c = run_child([] {
            gta6rt_seh_set_policy(GTA6RT_SEH_LOG_AND_ABORT);
            call1(W.fx, "fx_seh_av", 0x10);
        });
        show_child("LOG_AND_ABORT, hardware fault", c);
        CHECK(WIFSIGNALED(c.status) && WTERMSIG(c.status) == SIGABRT);
        CHECK(contains(c.err, "EXCEPTION_ACCESS_VIOLATION") && contains(c.err, "read at address 0x10"));
    }
    {
        child_out c = run_child([] { call1(W.fx, "fx_uncaught", 1); });
        show_child("DISPATCH, uncaught my_error", c);
        CHECK(WIFSIGNALED(c.status) && WTERMSIG(c.status) == SIGABRT);
        CHECK(contains(c.err, "unhandled exception") && contains(c.err, "my_error"));
    }
    {
        // clang implements noexcept with a cleanup that calls __std_terminate in phase 2; the caller's try was
        // removed (the call cannot throw), so phase 1 finds no handler, as it would on Windows.
        child_out c = run_child([] { call0(W.fx, "fx_terminate"); });
        show_child("DISPATCH, throw through a clang noexcept function", c);
        uint64_t site = 0;
        bool saw = false;
        for (uint64_t a : frames_of(c.err, &site)) saw |= in_function(W.fx, "?fx_noexcept@@YAXH@Z", a);
        CHECK(WIFSIGNALED(c.status) && WTERMSIG(c.status) == SIGABRT && contains(c.err, "unhandled exception") && saw);
    }
    {
        // A destructor throws during phase 2: vcruntime's __FrameUnwindToState __try/__except catches the nested
        // exception (dispatched while the outer unwind is in progress) and calls terminate().
        child_out c = run_child([] { call0(W.fx, "fx_dtor_throws"); });
        show_child("DISPATCH, destructor throws during unwind", c);
        CHECK(WIFEXITED(c.status) && WEXITSTATUS(c.status) == 42 && contains(c.err, "terminate() called"));
    }
    {
        // gta6rt_seh_handle_signal from a host SIGSEGV handler instead of isa's (keeps the YMM upper halves).
        child_out c = run_child([] {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = own_segv_handler;
            sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sigaction(SIGSEGV, &sa, nullptr);
            sigaction(SIGBUS, &sa, nullptr);
            void* cookie = nullptr;
            gta6rt_seh_add_vectored_handler(1, reinterpret_cast<uint64_t>(&veh_fixup), &cookie);
            const long long a = call1(W.fx, "fx_seh_av", 0x10), b = call1(W.fx, "fx_seh_av", 0x20);
            fprintf(stderr, "[test] own handler: __except %lld, resumed %lld\n", a, b);
            _exit(a == 6 && b == 77 ? 0 : 1);
        });
        show_child("host SIGSEGV handler -> gta6rt_seh_handle_signal", c);
        CHECK(WIFEXITED(c.status) && WEXITSTATUS(c.status) == 0);
    }
    if (W.have_fallback) {
        child_out c = run_child([] { call0(W.fx_fallback, "fx_catch_int"); });
        show_child("vcruntime stand-ins (not mapped), DISPATCH", c);
        CHECK(WIFSIGNALED(c.status) && WTERMSIG(c.status) == SIGABRT);
        CHECK(contains(c.err, "__CxxFrameHandler3 stand-in called") && contains(c.err, "C++ type int"));
        uint64_t site = 0;
        frames_of(c.err, &site);
        CHECK(in_function(W.fx_fallback, "?fx_thrower@@YAXH@Z", site));
    }

    printf("seh_dispatch_test: %d checks, %d failed: %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
