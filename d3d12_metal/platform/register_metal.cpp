#include <gta6rt/d3d12_backend.h>
#include <gta6rt/shims.h>

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
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

// GTA6RT_METAL_CRASH_MEMGRAPH=<file>: at a SIGABRT, `leaks --outputGraph` writes a memory graph of the process (with
// the malloc stack logs when GTA6RT_METAL_STACK_LOG_AT_S turned them on), for `malloc_history <file> <address>` of
// the block a malloc error names. Waits for it, at most 600 s.
void write_memgraph() {
    const char* path = std::getenv("GTA6RT_METAL_CRASH_MEMGRAPH");
    if (!path || !*path) return;
    char pid[16], out[1100], log[1100];
    std::snprintf(pid, sizeof pid, "%d", int(getpid()));
    std::snprintf(out, sizeof out, "--outputGraph=%s", path);
    std::snprintf(log, sizeof log, "%s.txt", path);
    std::fprintf(stderr, "[d3m-crash] writing a memory graph to %s\n", path);
    const pid_t child = fork();
    if (child == 0) {
        const int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        execl("/usr/bin/leaks", "leaks", pid, out, "--quiet", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) return;
    int status = 0;
    for (int i = 0; i < 6000; ++i) {
        if (waitpid(child, &status, WNOHANG) == child) {
            std::fprintf(stderr, "[d3m-crash] memory graph written (leaks exit %d after %.1f s)\n",
                         WIFEXITED(status) ? WEXITSTATUS(status) : -1, i / 10.0);
            return;
        }
        usleep(100000);
    }
    kill(child, SIGKILL);
    std::fprintf(stderr, "[d3m-crash] memory graph not written in 600 s\n");
}

// With malloc stack logging on: the block a malloc error names (MallocDebugReport=stderr prints "object 0x..." just
// before the abort, into this process's log) gets its allocation and free stacks from malloc_history.
void history_of_reported_block() {
    char path[1024] = {};
    if (fcntl(2, F_GETPATH, path) == -1) return;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char tail[8192] = {};
    const off_t end = lseek(fd, 0, SEEK_END);
    const off_t at = end > off_t(sizeof tail - 1) ? end - off_t(sizeof tail - 1) : 0;
    const ssize_t n = pread(fd, tail, sizeof tail - 1, at);
    close(fd);
    if (n <= 0) return;
    const char* hit = nullptr;
    for (const char* p = tail; (p = std::strstr(p, "object 0x")); ++p) hit = p;
    if (!hit) return;
    char address[32] = {};
    std::sscanf(hit + 7, "%31[0-9a-fx]", address);
    char pid[16];
    std::snprintf(pid, sizeof pid, "%d", int(getpid()));
    std::fprintf(stderr, "[d3m-crash] malloc_history of the reported block %s:\n", address);
    std::fflush(stderr);
    const pid_t child = fork();
    if (child == 0) {
        dup2(2, 1);
        execl("/usr/bin/malloc_history", "malloc_history", pid, address, static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) return;
    int status = 0;
    for (int i = 0; i < 1200; ++i) {
        if (waitpid(child, &status, WNOHANG) == child) return;
        usleep(100000);
    }
    kill(child, SIGKILL);
}

void on_abort(int sig, siginfo_t* si, void* uc) {
    if (g_in_crash.exchange(1) == 0) {
        print_thread("SIGABRT");
        walk_context(uc);
        print_backtrace();
        std::fflush(stderr);
        history_of_reported_block();
        write_memgraph();
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

// A host-code fault near address 0 (a null or freed object's field) is fatal: its stack is printed before the
// handler in front (seh, isa, the host's) takes it. Guest faults and the guarded copies' faults pass untouched.
struct sigaction g_prev_segv{}, g_prev_bus{};

void on_fault(int sig, siginfo_t* si, void* uc) {
    std::uint64_t pc = 0;
#if defined(__x86_64__)
    if (uc) pc = static_cast<ucontext_t*>(uc)->uc_mcontext->__ss.__rip;
#endif
    Dl_info info{};
    const bool near_null = si && reinterpret_cast<std::uintptr_t>(si->si_addr) < 0x10000;
    const bool host = near_null && pc && dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname &&
                      std::strstr(info.dli_fname, "gta6rt");
    if (host && g_in_crash.exchange(1) == 0) {
        print_thread(sig == SIGSEGV ? "SIGSEGV near 0 in host code" : "SIGBUS near 0 in host code");
        walk_context(uc);
        std::fflush(stderr);
    }
    const struct sigaction& prev = sig == SIGSEGV ? g_prev_segv : g_prev_bus;
    if ((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction) {
        prev.sa_sigaction(sig, si, uc);
        return;
    }
    if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler) {
        prev.sa_handler(sig);
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
    struct sigaction fa{};
    fa.sa_sigaction = on_fault;
    fa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&fa.sa_mask);
    sigaction(SIGSEGV, &fa, &g_prev_segv);
    sigaction(SIGBUS, &fa, &g_prev_bus);
    g_prev_terminate = std::set_terminate(on_terminate);
    std::fprintf(stderr, "[rt] d3d12-metal: SIGABRT and std::terminate print a backtrace ([d3m-crash]; "
                         "GTA6RT_METAL_CRASH_BACKTRACE=0 turns it off)\n");
}

std::atomic<std::uint64_t> g_arena_reserved{0}, g_arena_reserve_end{0};

// GTA6RT_METAL_STACK_LOG_AT_S=<s>: malloc stack logging (lite) is turned on that many seconds after registration, so
// a memory graph written at a crash names who allocated and freed a block (from the start it slows the boot ~5x).
void start_stack_logging_later() {
    const char* v = std::getenv("GTA6RT_METAL_STACK_LOG_AT_S");
    if (!v || !*v) return;
    const double at = std::strtod(v, nullptr);
    std::thread([at] {
        std::this_thread::sleep_for(std::chrono::duration<double>(at));
        using Fn = bool (*)(int);
        auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "turn_on_stack_logging"));
        const bool on = fn && fn(4);
        std::fprintf(stderr, "[d3m-crash] malloc stack logging (lite) %s at %.0f s\n", on ? "on" : "NOT on", at);
    }).detach();
}

// The GPU address arena's console VAs are its offsets [0, LEONIDA_ARENA_MB) (default 256 MiB), no mapping of this
// process; the title reserves memory with a low address requirement at boot, before the device exists, first fit from
// 8 MiB, and the arena then has to keep out of it (title_overlap_end) until it is full. Reserved here, before the
// title runs: no access, no memory. GTA6RT_METAL_ARENA_RESERVE_VA=0: not reserved.
void reserve_arena_vas() {
    const char* v = std::getenv("GTA6RT_METAL_ARENA_RESERVE_VA");
    if (v && !std::strcmp(v, "0")) return;
    const char* mb = std::getenv("LEONIDA_ARENA_MB");
    long long size_mb = mb && *mb ? std::strtoll(mb, nullptr, 10) : 256;
    size_mb = std::min(4095ll, std::max(16ll, size_mb));
    constexpr std::uint64_t chunk = 1ull << 20;
    const std::uint64_t end = std::uint64_t(size_mb) << 20;
    std::uint64_t reserved = 0;
    for (std::uint64_t at = chunk; at < end; at += chunk) {
        mach_vm_address_t a = at;
        if (mach_vm_map(mach_task_self(), &a, chunk, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE,
                        VM_PROT_NONE, VM_INHERIT_NONE) == KERN_SUCCESS)
            reserved += chunk;
    }
    g_arena_reserved = reserved;
    g_arena_reserve_end = end;
}

}  // namespace

namespace d3m {
std::uint64_t early_arena_reservation(std::uint64_t* end) {
    if (end) *end = g_arena_reserve_end.load();
    return g_arena_reserved.load();
}
}  // namespace d3m

// platform/register.cpp of d3d12_vulkan, compiled into this module, defines these two over this module's device; the
// prelink keeps them private, so these are the only names the host sees.
extern "C" gta6rt_status gta6rt_d3d12_metal_register(void) {
    install_crash_reports();
    start_stack_logging_later();
    reserve_arena_vas();
    return gta6rt_d3d12_vulkan_register();
}
extern "C" gta6rt_status gta6rt_d3d12_metal_at_exit(uint32_t wait_ms) { return gta6rt_d3d12_vulkan_at_exit(wait_ms); }
