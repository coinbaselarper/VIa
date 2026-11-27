#include <gta6rt/d3d12_backend.h>
#include <gta6rt/shims.h>

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <typeinfo>

namespace {

struct sigaction g_prev_abort{};
std::terminate_handler g_prev_terminate = nullptr;
std::atomic<int> g_in_crash{0};

bool safe_read(std::uint64_t at, std::uint64_t& out) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), at, sizeof out, reinterpret_cast<mach_vm_address_t>(&out), &got) ==
               KERN_SUCCESS &&
           got == sizeof out;
}

void print_frame(int i, std::uint64_t pc) {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
        const char* file = std::strrchr(info.dli_fname, '/');
        int status = 0;
        char* demangled = info.dli_sname ? abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status) : nullptr;
        std::fprintf(stderr, "[d3m-crash]   #%02d %#018" PRIx64 " %s %s+%#" PRIx64 "\n", i, pc, file ? file + 1 : info.dli_fname,
                     demangled ? demangled : info.dli_sname ? info.dli_sname : "?",
                     pc - reinterpret_cast<std::uint64_t>(info.dli_saddr ? info.dli_saddr : info.dli_fbase));
        std::free(demangled);
    } else {
        std::fprintf(stderr, "[d3m-crash]   #%02d %#018" PRIx64 " (no image; exe+%#" PRIx64 " if the title's)\n", i, pc,
                     pc - 0x140000000ull);
    }
}

void print_thread(const char* why) {
    char name[64] = {};
    pthread_getname_np(pthread_self(), name, sizeof name);
    std::uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    std::fprintf(stderr, "[d3m-crash] %s on thread %" PRIu64 " '%s'\n", why, tid, name);
}

void print_backtrace() {
    void* frames[96];
    const int n = backtrace(frames, 96);
    std::fprintf(stderr, "[d3m-crash] backtrace (%d frames):\n", n);
    for (int i = 0; i < n; ++i) print_frame(i, reinterpret_cast<std::uint64_t>(frames[i]));
}

void walk_context(void* uc) {
#if defined(__x86_64__)
    if (!uc) return;
    const auto* mc = static_cast<ucontext_t*>(uc)->uc_mcontext;
    std::uint64_t pc = mc->__ss.__rip, fp = mc->__ss.__rbp;
    std::fprintf(stderr, "[d3m-crash] interrupted context: rip %#" PRIx64 " rsp %#" PRIx64 " rbp %#" PRIx64 "\n", pc,
                 std::uint64_t(mc->__ss.__rsp), fp);
    print_frame(0, pc);
    for (int i = 1; i < 64 && fp; ++i) {
        std::uint64_t next = 0, ret = 0;
        if (!safe_read(fp, next) || !safe_read(fp + 8, ret) || !ret) break;
        print_frame(i, ret);
        if (next <= fp) break;
        fp = next;
    }
#else
    (void)uc;
#endif
}

void on_abort(int sig, siginfo_t* si, void* uc) {
    if (g_in_crash.exchange(1) == 0) {
        print_thread("SIGABRT");
        walk_context(uc);
        print_backtrace();
        std::fflush(stderr);
    }
    if ((g_prev_abort.sa_flags & SA_SIGINFO) && g_prev_abort.sa_sigaction) {
        g_prev_abort.sa_sigaction(sig, si, uc);
        return;
    }
    if (g_prev_abort.sa_handler != SIG_DFL && g_prev_abort.sa_handler != SIG_IGN && g_prev_abort.sa_handler) {
        g_prev_abort.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void on_terminate() {
    if (g_in_crash.exchange(1) == 0) {
        print_thread("std::terminate");
        if (const std::type_info* t = abi::__cxa_current_exception_type()) {
            int status = 0;
            char* name = abi::__cxa_demangle(t->name(), nullptr, nullptr, &status);
            std::fprintf(stderr, "[d3m-crash] uncaught exception of type %s\n", name ? name : t->name());
            std::free(name);
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[d3m-crash] what(): %s\n", e.what());
            } catch (...) {
            }
        } else {
            std::fprintf(stderr, "[d3m-crash] no current exception\n");
        }
        print_backtrace();
        std::fflush(stderr);
        g_in_crash.store(0);
    }
    if (g_prev_terminate) g_prev_terminate();
    std::abort();
}

void install_crash_reports() {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    const char* v = std::getenv("GTA6RT_METAL_CRASH_BACKTRACE");
    if (v && !std::strcmp(v, "0")) return;
    struct sigaction sa{};
    sa.sa_sigaction = on_abort;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGABRT, &sa, &g_prev_abort);
    g_prev_terminate = std::set_terminate(on_terminate);
    std::fprintf(stderr, "[rt] d3d12-metal: SIGABRT and std::terminate print a backtrace ([d3m-crash]; "
                         "GTA6RT_METAL_CRASH_BACKTRACE=0 turns it off)\n");
}

}  // namespace

// platform/register.cpp of d3d12_vulkan, compiled into this module, defines these two over this module's device; the
// prelink keeps them private, so these are the only names the host sees.
extern "C" gta6rt_status gta6rt_d3d12_metal_register(void) {
    install_crash_reports();
    return gta6rt_d3d12_vulkan_register();
}
extern "C" gta6rt_status gta6rt_d3d12_metal_at_exit(uint32_t wait_ms) { return gta6rt_d3d12_vulkan_at_exit(wait_ms); }
