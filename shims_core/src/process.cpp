// Process-level imports of a mapped executable: its command line and its way out.
//
// The host calls a mapped EXE's entry point inside the gta6rt process. The EXE's CRT startup asks for its arguments
// and finally calls exit(); both go through the EXE's own import table, so they are answered here:
//   - the command line is the game's, and its CRT configures its own argv/argc at startup;
//   - exit(), _exit(), quick_exit(), ExitProcess(), abort() and terminate() from the game end the game's run, not
//     the host: the exit code is recorded, the run is signalled as finished and the calling thread parks until the
//     host exits;
//   - the game's atexit registrations (_crt_atexit) go to a table of the run's own instead of the host's. exit() runs
//     it newest first before the run ends; _exit(), _Exit(), quick_exit(), ExitProcess() and abort() do not.
//     _cexit() runs it and returns, _c_exit() returns. A second thread that calls exit(), _exit(), _Exit() or
//     quick_exit() while the first is leaving waits, as with the CRT's exit lock.
// Not covered: at_quick_exit() handlers; the msvcrt.dll (MinGW) entry names of the original, which a UCRT title never
// binds.
#include <windows.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <new>
#include <pthread.h>
#include <unistd.h>
#include <vector>

#include <gta6rt/isa.h>
#include <gta6rt/os.h>

#include "k32_shared.h"
#include "process_shim.h"
#include "shims.h"
#include "util.h"

namespace rt::process {
namespace {

std::vector<std::u16string> g_input_wargs;
std::vector<std::string> g_input_args;
std::vector<std::string> g_args;
std::vector<std::u16string> g_wargs;
std::vector<char*> g_argv;
std::vector<char16_t*> g_wargv;
std::string g_cmdline;
std::u16string g_wcmdline;
int g_argc = 0;
char** g_argv_ptr = nullptr;
char16_t** g_wargv_ptr = nullptr;

std::atomic<bool> g_running{false};
std::atomic<bool> g_exited{false};
std::atomic<int> g_exit_code{0};
gta6rt_waitable* g_done = nullptr;

uint32_t k32_thread_id() {
    if (gta6rt_thread* t = gta6rt_thread_current()) return gta6rt_thread_id(t);
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return tid ? static_cast<uint32_t>(tid) : 1u;
}

std::u16string quote(const std::u16string& a) {
    if (!a.empty() && a.find_first_of(u" \t\"") == std::u16string::npos) return a;
    std::u16string out = u"\"";
    size_t slashes = 0;
    for (char16_t c : a) {
        if (c == u'\\') {
            ++slashes;
        } else {
            if (c == u'"') out.append(slashes + 1, u'\\');
            slashes = 0;
        }
        out += c;
    }
    return out.append(slashes, u'\\') + u"\"";
}

[[noreturn]] void end_run(int code) {
    const bool running = g_running;
    bool expected = false;
    if (g_exited.compare_exchange_strong(expected, true)) {
        g_exit_code = code;
        if (g_done) gta6rt_event_set(g_done);
    }
    if (running)
        for (;;) gta6rt_sleep_ns(3600ull * 1000000000ull);
    std::fflush(nullptr);
    _exit(g_exit_code.load());
}

// ---- the game's atexit table
using AtexitAddr = uint64_t;
std::mutex g_atexit_lock;
std::vector<AtexitAddr> g_atexit;
std::atomic<uint32_t> g_exiting_thread{0};

void run_atexit_handlers() {
    for (;;) {
        AtexitAddr fn;
        {
            std::lock_guard<std::mutex> lock(g_atexit_lock);
            if (g_atexit.empty()) return;
            fn = g_atexit.back();
            g_atexit.pop_back();
        }
        gta6rt_isa_call_guest4(fn, 0, 0, 0, 0);
    }
}

int WINAPI game_crt_atexit(uint64_t fn) { return fn && register_atexit_address(fn) ? 0 : -1; }

bool take_exit_lock() {
    const uint32_t self = k32_thread_id();
    uint32_t owner = 0;
    if (g_exiting_thread.compare_exchange_strong(owner, self)) return true;
    if (owner != self)
        for (;;) gta6rt_sleep_ns(3600ull * 1000000000ull);
    return false;
}

void WINAPI game_exit(int code) {
    if (take_exit_lock()) run_atexit_handlers();
    end_run(code);
}
void WINAPI game_quick_exit(int code) {
    take_exit_lock();
    end_run(code);
}
void WINAPI game_cexit() {
    run_atexit_handlers();
    std::fflush(nullptr);
}
void WINAPI game_c_exit() {}
void WINAPI game_exit_process(UINT code) { end_run(static_cast<int>(code)); }

void WINAPI game_abort() {
    RTLOG(ERROR, "the game called abort()");
    end_run(3);
}
void WINAPI game_terminate() {
    RTLOG(ERROR, "the game called terminate()");
    end_run(3);
}

// ---- threads and processors
// The console has cores a PC may lack, and a title pins its threads to them with masks the host cannot honour. macOS
// has no affinity API at all, so every mask is accepted: the thread stays where it is and the call answers the mask
// the thread had (the process mask for a thread never pinned). An empty mask names no CPU on any machine and fails
// with ERROR_INVALID_PARAMETER, as does a bad handle with ERROR_INVALID_HANDLE.
std::mutex g_affinity_mutex;
std::map<uintptr_t, uint64_t> g_affinity;

DWORD_PTR WINAPI game_set_thread_affinity_mask(HANDLE thread, DWORD_PTR mask) {
    gta6rt_thread* t = k32::thread_of_handle(thread);
    if (!t) {
        gta6rt_last_error_set(6);
        return 0;
    }
    if (mask == 0) {
        gta6rt_last_error_set(87);
        return 0;
    }
    const uint64_t process = k32::affinity_mask();
    std::lock_guard lock(g_affinity_mutex);
    uint64_t& stored = g_affinity.try_emplace(reinterpret_cast<uintptr_t>(t), process).first->second;
    const uint64_t previous = stored;
    if (mask & process) stored = mask & process;
    static std::atomic<int> noted{0};
    if (++noted <= 8)
        RTLOG(INFO, "SetThreadAffinityMask(0x%llx): macOS has no thread affinity; the thread stays, answered its mask 0x%llx",
              static_cast<unsigned long long>(mask), static_cast<unsigned long long>(previous));
    gta6rt_last_error_set(0);
    return previous;
}

BOOL WINAPI game_set_thread_group_affinity(HANDLE thread, const GROUP_AFFINITY* requested, GROUP_AFFINITY* previous) {
    gta6rt_thread* t = k32::thread_of_handle(thread);
    if (!requested || requested->Mask == 0) {
        gta6rt_last_error_set(87);
        return FALSE;
    }
    if (!t) {
        gta6rt_last_error_set(6);
        return FALSE;
    }
    const uint64_t process = k32::affinity_mask();
    std::lock_guard lock(g_affinity_mutex);
    uint64_t& stored = g_affinity.try_emplace(reinterpret_cast<uintptr_t>(t), process).first->second;
    if (previous) {
        std::memset(previous, 0, sizeof *previous);
        previous->Mask = stored;
        previous->Group = 0;
    }
    if (requested->Mask & process) stored = requested->Mask & process;
    static std::atomic<int> noted{0};
    if (++noted <= 8)
        RTLOG(INFO, "SetThreadGroupAffinity(group %u, 0x%llx): macOS has no thread affinity; the thread stays",
              static_cast<unsigned>(requested->Group), static_cast<unsigned long long>(requested->Mask));
    gta6rt_last_error_set(0);
    return TRUE;
}

BOOL WINAPI game_set_threadpool_affinity_mask(void* pool, DWORD_PTR mask) {
    (void)pool;
    (void)mask;
    return TRUE;
}

// ---- arguments
int* WINAPI game_p_argc() { return &g_argc; }
char*** WINAPI game_p_argv() { return &g_argv_ptr; }
char16_t*** WINAPI game_p_wargv() { return &g_wargv_ptr; }

template <typename C>
int configure_argv(int mode, const std::vector<std::basic_string<C>>& input, std::vector<std::basic_string<C>>& strings,
                   std::vector<C*>& pointers, C**& published) {
    if (mode == 0) return 0;
    if (mode != 1 && mode != 2) return errno = EINVAL;
    try {
        auto next_strings = input;
        std::vector<C*> next_pointers;
        next_pointers.reserve(next_strings.size() + 1);
        for (auto& argument : next_strings) next_pointers.push_back(argument.data());
        next_pointers.push_back(nullptr);
        strings.swap(next_strings);
        pointers.swap(next_pointers);
        published = pointers.data();
        g_argc = static_cast<int>(strings.size());
        return 0;
    } catch (const std::bad_alloc&) {
        return errno = ENOMEM;
    }
}

int WINAPI game_configure_narrow_argv(int mode) { return configure_argv(mode, g_input_args, g_args, g_argv, g_argv_ptr); }
int WINAPI game_configure_wide_argv(int mode) { return configure_argv(mode, g_input_wargs, g_wargs, g_wargv, g_wargv_ptr); }

char* WINAPI game_get_command_line_a() { return g_cmdline.data(); }
char16_t* WINAPI game_get_command_line_w() { return g_wcmdline.data(); }
char** WINAPI game_p_acmdln_storage() {
    static char* p = nullptr;
    p = g_cmdline.data();
    return &p;
}
char16_t** WINAPI game_p_wcmdln_storage() {
    static char16_t* p = nullptr;
    p = g_wcmdline.data();
    return &p;
}

// What _get_narrow/_get_wide_winmain_command_line return: the command line after the program name and the blanks that
// follow it. The program name ends at the first character up to ' ' outside quotes.
template <typename C>
C* after_program_name(C* p) {
    using U = std::make_unsigned_t<C>;
    bool quoted = false;
    while (U(*p) > U(' ') || (*p && quoted)) {
        if (*p == C('"')) quoted = !quoted;
        ++p;
    }
    while (*p && U(*p) <= U(' ')) ++p;
    return p;
}
char* WINAPI game_get_narrow_winmain_command_line() { return after_program_name(g_cmdline.data()); }
char16_t* WINAPI game_get_wide_winmain_command_line() { return after_program_name(g_wcmdline.data()); }

struct EntryCall {
    uint64_t entry;
};

void game_thread(void* p) {
    auto* call = static_cast<EntryCall*>(p);
    uint64_t rax = 0;
    gta6rt_isa_run_entry(call->entry, 0, 0, 0, &rax);
    end_run(static_cast<int>(static_cast<uint32_t>(rax)));
}

}  // namespace

bool register_atexit_address(uint64_t guest_fn) {
    std::lock_guard<std::mutex> lock(g_atexit_lock);
    try {
        g_atexit.push_back(guest_fn);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

[[noreturn]] void abort_run(int code) { end_run(code); }

void set_command_line(const std::u16string& program, const std::vector<std::u16string>& args) {
    g_input_wargs.clear();
    g_input_wargs.push_back(program);
    g_input_wargs.insert(g_input_wargs.end(), args.begin(), args.end());
    g_input_args.clear();
    g_wcmdline.clear();
    for (const auto& a : g_input_wargs) {
        g_input_args.push_back(narrow(a));
        if (!g_wcmdline.empty()) g_wcmdline += u' ';
        g_wcmdline += quote(a);
    }
    g_cmdline = narrow(g_wcmdline);
    g_args.clear();
    g_wargs.clear();
    g_argv.clear();
    g_wargv.clear();
    g_argc = 0;
    g_argv_ptr = nullptr;
    g_wargv_ptr = nullptr;
}

int run_entry(uint64_t entry, size_t stack_bytes) {
    {
        std::lock_guard<std::mutex> lock(g_atexit_lock);
        g_atexit.clear();
    }
    g_exiting_thread = 0;
    g_exited = false;
    g_exit_code = 0;
    if (gta6rt_event_create(1, 0, &g_done) != GTA6RT_OK) return -1;
    g_running = true;
    EntryCall call{entry};
    gta6rt_thread_desc desc{};
    desc.fn = &game_thread;
    desc.arg = &call;
    desc.stack_size = stack_bytes;
    desc.name = "game main";
    desc.guest = 1;
    gta6rt_thread* thread = nullptr;
    if (gta6rt_thread_create(&desc, &thread) != GTA6RT_OK) {
        g_running = false;
        gta6rt_waitable_release(g_done);
        g_done = nullptr;
        return -1;
    }
    gta6rt_waitable* waits[] = {g_done, gta6rt_thread_waitable(thread)};
    uint32_t index = 0;
    gta6rt_wait_any(waits, 2, GTA6RT_WAIT_INFINITE, &index);
    bool expected = false;
    if (!g_exited.compare_exchange_strong(expected, true)) {
        gta6rt_wait_any(waits, 1, GTA6RT_WAIT_INFINITE, &index);
    }
    gta6rt_thread_release(thread);
    gta6rt_waitable_release(g_done);
    g_done = nullptr;
    g_running = false;
    return g_exit_code.load();
}

}  // namespace rt::process

namespace {
using namespace rt::process;
#define EXIT_PROCESS(dll) SHIM(dll, "ExitProcess", game_exit_process)
}  // namespace

SHIM_TABLE(process,
    SHIM("ucrtbase.dll", "exit", game_exit),
    SHIM("ucrtbase.dll", "_exit", game_quick_exit),
    SHIM("ucrtbase.dll", "_Exit", game_quick_exit),
    SHIM("ucrtbase.dll", "quick_exit", game_quick_exit),
    SHIM("ucrtbase.dll", "_crt_atexit", game_crt_atexit),
    SHIM("ucrtbase.dll", "_cexit", game_cexit),
    SHIM("ucrtbase.dll", "_c_exit", game_c_exit),
    SHIM("ucrtbase.dll", "abort", game_abort),
    SHIM("ucrtbase.dll", "terminate", game_terminate),
    SHIM("ucrtbase.dll", "__p___argc", game_p_argc),
    SHIM("ucrtbase.dll", "__p___argv", game_p_argv),
    SHIM("ucrtbase.dll", "__p___wargv", game_p_wargv),
    SHIM("ucrtbase.dll", "_configure_narrow_argv", game_configure_narrow_argv),
    SHIM("ucrtbase.dll", "_configure_wide_argv", game_configure_wide_argv),
    SHIM("ucrtbase.dll", "__p__acmdln", game_p_acmdln_storage),
    SHIM("ucrtbase.dll", "__p__wcmdln", game_p_wcmdln_storage),
    SHIM("ucrtbase.dll", "_get_narrow_winmain_command_line", game_get_narrow_winmain_command_line),
    SHIM("ucrtbase.dll", "_get_wide_winmain_command_line", game_get_wide_winmain_command_line),
    EXIT_PROCESS("kernel32.dll"),
    SHIM("kernel32.dll", "SetThreadAffinityMask", game_set_thread_affinity_mask),
    SHIM("kernel32.dll", "SetThreadGroupAffinity", game_set_thread_group_affinity),
    SHIM("kernel32.dll", "SetThreadpoolAffinityMask", game_set_threadpool_affinity_mask),
    SHIM("kernel32.dll", "GetCommandLineA", game_get_command_line_a),
    SHIM("kernel32.dll", "GetCommandLineW", game_get_command_line_w),
);
