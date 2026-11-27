#include <gta6rt/isa.h>
#include <gta6rt/loader.h>
#include <gta6rt/media.h>
#include <gta6rt/nt_layout.h>
#include <gta6rt/os.h>
#include <gta6rt/seh.h>
#include <gta6rt/shims.h>
#include <gta6rt_hud/hud.h>

#include "guest_address.h"
#include "host_cpu.h"
#include "shims.h"
#include "trace.h"
#include "mac_window.h"
#include "register.h"
#include "window_shim.h"
#include "title_watch.h"
#include "governor/governor.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef GTA6RT_SOURCE_ROOT
#define GTA6RT_SOURCE_ROOT "."
#endif

extern "C" GTA6RT_GUEST_ABI void* GetProcessHeap(void);

namespace host {
void enable_d3d12_trace();
}

namespace {

constexpr uint32_t kDllProcessAttach = 1, kDllThreadAttach = 2, kDllThreadDetach = 3;
constexpr uint32_t kScnMemExecute = 0x20000000u;
constexpr uint64_t kWinMainRva = 0x5a91bd0;

struct Args {
    std::string exe;
    std::string package;
    std::string run_dir;
    host::trace::Options trace;
    bool marker = false;
    uint32_t marker_rva = 0;
    std::vector<uint32_t> breaks;
    unsigned watchdog_s = 0;
    bool force_reloc = false;
    bool map_only = false;
    bool reexec = true;
    bool hidden_window = false;
    std::string hud;
    bool no_zen_fixup = false;
    std::string vk_driver;
    std::string console_profile;
    int seh = -1;
    std::string peek;
    unsigned peek_s = 10;
    bool stage_watch = true;
    bool title_watchdog = true;
    bool null_pso_patch = false;
    host::governor::Options governor;
    std::vector<std::string> title_args;
};

Args g_args;
gta6rt_image* g_exe = nullptr;
gta6rt_image_info g_exe_info{};
void* g_peb = nullptr;
std::atomic<bool> g_images_initialised{false};
std::mutex g_fixup_mu;
std::set<gta6rt_gva> g_fixed_images;

void* g_guard_base = nullptr;
size_t g_guard_size = 0;

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[host] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

// Taken before main and before any host allocation can land there; released just before the exe and the package
// DLLs are mapped at their preferred bases.
__attribute__((constructor(101))) void reserve_guest_range() {
    const uint64_t lo = 0x140000000ull;
    for (uint64_t hi : {0x200000000ull, 0x180000000ull, 0x148000000ull}) {
        void* p = mmap(reinterpret_cast<void*>(lo), hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == reinterpret_cast<void*>(lo)) {
            g_guard_base = p;
            g_guard_size = hi - lo;
            return;
        }
        if (p != MAP_FAILED) munmap(p, hi - lo);
    }
}

void release_guest_range() {
    if (g_guard_base) munmap(g_guard_base, g_guard_size);
    g_guard_base = nullptr;
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

bool parse_u64(const char* s, uint64_t* out) {
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(s, &end, 0);
    if (errno || !end || *end) return false;
    *out = v;
    return true;
}

void usage() {
    std::fprintf(stderr,
                 "usage: gta6rt [options] [-- title args]\n"
                 "  --exe PATH          title executable (default <root>/../exe/game_scarlett_master.exe)\n"
                 "  --package DIR       content package, read-only (default <root>/../xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK)\n"
                 "  --run-dir DIR       writable state: overlay, registry, saves (default <root>/run)\n"
                 "  --trace[=N]         count every import call, print the first N with arguments (default 400)\n"
                 "  --trace-match S,..  also print calls whose name contains any of the comma-separated strings\n"
                 "  --stop-at RVA       report and exit when the exe reaches RVA (patched jump)\n"
                 "  --stop-at-winmain   same, at the WinMain call target (RVA 0x5a91bd0)\n"
                 "  --break-at RVA      int3 at an exe RVA: seh reports registers and a stack walk there (repeatable)\n"
                 "  --watchdog SECONDS  dump the trace and exit 124 after SECONDS\n"
                 "  --seh POLICY        log_and_abort (default) or dispatch\n"
                 "  --force-reloc       map the exe away from its preferred base\n"
                 "  --map-only          map, bind and initialise nothing; print the bind report\n"
                 "  --no-zen-fixup      leave CLWB/CLZERO/MONITORX/MWAITX to the SIGILL emulator\n"
                 "  --trace-d3d12       log every D3D12 call the layer records, with its result\n"
                 "  --hidden-window     the title's window is a layer only, nothing on screen\n"
                 "  --hud MODE          performance overlay: off, min (default with a visible window), full or title (GTA6RT_HUD; F1 cycles, F2 on/off)\n"
                 "  --vk-driver X       moltenvk, kosmickrisp or a dylib path (GTA6RT_VK_DRIVER); x86_64 default: the\n"
                 "                      spike/s1-vulkan-x86 MoltenVK 1.4.2 build when GTA6RT_VK_DRIVER is unset\n"
                 "  --console-profile P series-s, series-x or auto (LEONIDA_CONSOLE_PROFILE; default auto, series-x\n"
                 "                      under --stop-at/--stop-at-winmain)\n"
                 "  --input-script S    scripted pad presses, e.g. a@40,start@60 (LEONIDA_INPUT_SCRIPT)\n"
                 "  --present-dump D[,N] write every N-th presented frame as PNG into D (LEONIDA_PRESENT_DUMP)\n"
                 "  --memory-report S   log resident/compressed memory every S seconds (GTA6RT_MEMORY_REPORT_S)\n"
                 "  --memory-budget B   process memory budget: auto (default: physical memory less an OS reserve, less what\n"
                 "                      other processes hold now) or a size such as 14G; the console profile follows it\n"
                 "  --governor M        memory governor: on (default), observe (accounting only) or off\n"
                 "  --governor-actions L  governor actions to allow, letters a-e or all/none (default all)\n"
                 "  --peek SPEC         print 8-byte values at exe RVAs every --peek-seconds (LEONIDA_PEEK format:\n"
                 "                      RVA[:n] or RVA>OFF[:n], comma-separated, hex)\n"
                 "  --peek-seconds N    period of --peek (default 10)\n"
                 "  --no-stage-watch    do not log the title's stage changes (game state, session, loading byte)\n"
                 "  --title-watchdog on|off  off keeps the title's 30 s hang watchdog kicked (LEONIDA_TITLE_WATCHDOG=0)\n"
                 "  --null-pso-patch    patch exe+0x248c306 and amd_fidelityfx_upscaler_xbox.dll+0x65ff to skip a null PSO\n"
                 "                      (off by default: the D3D12 layer hands refused PSOs out as placeholders with S_OK;\n"
                 "                      --no-null-pso-patch is accepted and changes nothing)\n"
                 "  --no-reexec         do not re-execute for ROSETTA_ADVERTISE_AVX\n");
}

bool parse_args(int argc, char** argv) {
    const std::string root = GTA6RT_SOURCE_ROOT;
    g_args.exe = root + "/../exe/game_scarlett_master.exe";
    g_args.package = root + "/../xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK";
    g_args.run_dir = root + "/run";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--") {
            for (++i; i < argc; ++i) g_args.title_args.push_back(argv[i]);
            break;
        } else if (a == "--exe") {
            const char* v = next("--exe");
            if (!v) return false;
            g_args.exe = v;
        } else if (a == "--package") {
            const char* v = next("--package");
            if (!v) return false;
            g_args.package = v;
        } else if (a == "--run-dir") {
            const char* v = next("--run-dir");
            if (!v) return false;
            g_args.run_dir = v;
        } else if (a == "--trace") {
            g_args.trace.enabled = true;
            g_args.trace.log_first = 400;
        } else if (a.rfind("--trace=", 0) == 0) {
            g_args.trace.enabled = true;
            if (!parse_u64(a.c_str() + 8, &g_args.trace.log_first)) return false;
        } else if (a == "--trace-match") {
            const char* v = next("--trace-match");
            if (!v) return false;
            g_args.trace.enabled = true;
            g_args.trace.match = v;
        } else if (a == "--stop-at") {
            const char* v = next("--stop-at");
            uint64_t rva = 0;
            if (!v || !parse_u64(v, &rva) || rva > 0xffffffffu) return false;
            g_args.marker = true;
            g_args.marker_rva = static_cast<uint32_t>(rva);
        } else if (a == "--break-at") {
            const char* v = next("--break-at");
            uint64_t rva = 0;
            if (!v || !parse_u64(v, &rva) || rva > 0xffffffffu) return false;
            g_args.breaks.push_back(static_cast<uint32_t>(rva));
        } else if (a == "--stop-at-winmain") {
            g_args.marker = true;
            g_args.marker_rva = kWinMainRva;
        } else if (a == "--watchdog") {
            const char* v = next("--watchdog");
            uint64_t s = 0;
            if (!v || !parse_u64(v, &s)) return false;
            g_args.watchdog_s = static_cast<unsigned>(s);
        } else if (a == "--seh") {
            const char* v = next("--seh");
            if (!v) return false;
            if (!std::strcmp(v, "dispatch")) g_args.seh = GTA6RT_SEH_DISPATCH;
            else if (!std::strcmp(v, "log_and_abort")) g_args.seh = GTA6RT_SEH_LOG_AND_ABORT;
            else return false;
        } else if (a == "--force-reloc") {
            g_args.force_reloc = true;
        } else if (a == "--map-only") {
            g_args.map_only = true;
        } else if (a == "--no-reexec") {
            g_args.reexec = false;
        } else if (a == "--vk-driver") {
            const char* v = next("--vk-driver");
            if (!v) return false;
            g_args.vk_driver = v;
        } else if (a == "--no-zen-fixup") {
            g_args.no_zen_fixup = true;
        } else if (a == "--console-profile") {
            const char* v = next("--console-profile");
            if (!v) return false;
            g_args.console_profile = v;
        } else if (a == "--input-script") {
            const char* v = next("--input-script");
            if (!v) return false;
            setenv("LEONIDA_INPUT_SCRIPT", v, 1);
        } else if (a == "--present-dump") {
            const char* v = next("--present-dump");
            if (!v) return false;
            setenv("LEONIDA_PRESENT_DUMP", v, 1);
        } else if (a == "--memory-report") {
            const char* v = next("--memory-report");
            uint64_t s = 0;
            if (!v || !parse_u64(v, &s)) return false;
            setenv("GTA6RT_MEMORY_REPORT_S", v, 1);
            g_args.governor.report_s = static_cast<unsigned>(s);
        } else if (a == "--memory-budget") {
            const char* v = next("--memory-budget");
            uint64_t b = 0;
            if (!v || (std::strcmp(v, "auto") && !host::governor::parse_size(v, &b))) return false;
            g_args.governor.budget = v;
        } else if (a == "--governor") {
            if (!host::governor::parse_mode(next("--governor"), &g_args.governor.mode)) return false;
        } else if (a == "--governor-actions") {
            if (!host::governor::parse_actions(next("--governor-actions"), &g_args.governor.actions)) return false;
        } else if (a == "--peek") {
            const char* v = next("--peek");
            if (!v) return false;
            g_args.peek = v;
        } else if (a == "--peek-seconds") {
            const char* v = next("--peek-seconds");
            uint64_t s = 0;
            if (!v || !parse_u64(v, &s) || !s) return false;
            g_args.peek_s = static_cast<unsigned>(s);
        } else if (a == "--no-stage-watch") {
            g_args.stage_watch = false;
        } else if (a == "--title-watchdog") {
            const char* v = next("--title-watchdog");
            if (!v || (std::strcmp(v, "on") && std::strcmp(v, "off"))) return false;
            g_args.title_watchdog = !std::strcmp(v, "on");
        } else if (a == "--no-null-pso-patch") {
            g_args.null_pso_patch = false;
        } else if (a == "--null-pso-patch") {
            g_args.null_pso_patch = true;
        } else if (a == "--trace-d3d12") {
            host::enable_d3d12_trace();
        } else if (a == "--hidden-window") {
            g_args.hidden_window = true;
        } else if (a == "--hud") {
            const char* v = next("--hud");
            gta6rt_hud_mode hud_mode;
            if (!v || !gta6rt_hud_parse_mode(v, &hud_mode)) {
                std::fprintf(stderr, "--hud needs off, min, full or title\n");
                return false;
            }
            g_args.hud = v;
        } else if (a == "-h" || a == "--help") {
            usage();
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

void make_dirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty()) mkdir(cur.c_str(), 0755);
        }
        if (i < path.size()) cur.push_back(path[i]);
    }
}

std::string absolute(const std::string& p) {
    char buf[PATH_MAX];
    if (realpath(p.c_str(), buf)) return buf;
    return p;
}

#if defined(__x86_64__)
// x86_64 builds of the two Apple Vulkan drivers (spike/s1-vulkan-x86). MoltenVK is the default since S1 suspended
// KosmicKrisp as primary (NOTES.md, S1 result).
std::string x86_driver(const std::string& name) {
    const std::string d = std::string(GTA6RT_SOURCE_ROOT) + "/../spike/s1-vulkan-x86/drivers/";
    if (name == "moltenvk") return absolute(d + "moltenvk-1.4.2/libMoltenVK.x86_64.dylib");
    if (name == "kosmickrisp") return absolute(d + "kosmickrisp-26.3.1-x86_64/lib/libvulkan_kosmickrisp.dylib");
    return {};
}
#endif

// Driver, translated shader corpus and pipeline cache directory; each environment variable already set wins.
void configure_vulkan(const std::string& run_dir) {
    std::string drv = g_args.vk_driver;
#if defined(__x86_64__)
    if (drv.empty() && !getenv("GTA6RT_VK_DRIVER")) drv = "moltenvk";
    if (const std::string p = x86_driver(lower(drv)); !p.empty()) drv = p;
#endif
    if (!drv.empty()) {
        if (drv.find('/') != std::string::npos && access(drv.c_str(), R_OK) != 0) logf("vulkan driver %s: not readable", drv.c_str());
        setenv("GTA6RT_VK_DRIVER", drv.c_str(), 1);
    }
    const char* d = getenv("GTA6RT_VK_DRIVER");
    logf("vulkan driver: %s", d ? d : "(default search)");
    const std::string corpus = std::string(GTA6RT_SOURCE_ROOT) + "/build/shaders_vk/apple/spirv";
    if (!getenv("LEONIDA_SHADERS") && access((corpus + "/native_hashes.txt").c_str(), R_OK) == 0)
        setenv("LEONIDA_SHADERS", absolute(corpus).c_str(), 1);
    logf("shaders: %s", getenv("LEONIDA_SHADERS") ? getenv("LEONIDA_SHADERS") : "(none: every PSO with a shader fails)");
    if (!getenv("LEONIDA_PIPELINE_CACHE_DIR")) {
        std::string base = d ? d : "default";
        if (const size_t s = base.rfind('/'); s != std::string::npos) base = base.substr(s + 1);
        if (const size_t e = base.rfind(".dylib"); e != std::string::npos) base = base.substr(0, e);
        const std::string dir = run_dir + "/pipeline_cache/" + base;
        make_dirs(dir);
        setenv("LEONIDA_PIPELINE_CACHE_DIR", dir.c_str(), 1);
    }
    logf("pipeline cache: %s", getenv("LEONIDA_PIPELINE_CACHE_DIR"));
    const char* real = getenv("GTA6RT_VK_DRIVER");
    const char* guard_env = getenv("GTA6RT_VK_GUARD");
    char exe[PATH_MAX];
    uint32_t exe_size = sizeof exe;
    if (real && std::strchr(real, '/') && !(guard_env && !std::strcmp(guard_env, "0")) &&
        _NSGetExecutablePath(exe, &exe_size) == 0) {
        std::string guard = absolute(exe);
        guard = guard.substr(0, guard.rfind('/') + 1) + "libgta6rt_vkguard.dylib";
        if (access(guard.c_str(), R_OK) == 0) {
            setenv("GTA6RT_VK_GUARD_DRIVER", real, 1);
            setenv("GTA6RT_VK_DRIVER", guard.c_str(), 1);
            logf("vulkan driver guard: %s in front of the driver (reports image views Metal aborts on; GTA6RT_VK_GUARD=0 "
                 "turns it off)", guard.c_str());
        } else {
            logf("vulkan driver guard: %s not built; image views go to the driver unchecked", guard.c_str());
        }
    }
}

void report_unimplemented_and_trace(const char* why) {
    std::fflush(stdout);
    host::trace::dump(stderr, why);
    gta6rt_isa_vshift_check_report();
    std::fprintf(stderr, "=== shim report: imports called but not served ===\n");
    const size_t n = gta6rt_shims_report_unimplemented();
    std::fprintf(stderr, "=== %zu call(s) reached unimplemented imports; %llu isa traps (SSE4a, CLWB/CLZERO/MONITORX, cpuid) ===\n", n,
                 static_cast<unsigned long long>(gta6rt_isa_trap_count()));
    std::fflush(stderr);
}

// ---- fatal signals that neither isa nor seh took ----------------------------------------------------------

struct sigaction g_prev[32];
std::atomic<int> g_in_fatal{0};

void fatal_handler(int sig, siginfo_t* si, void* uc) {
    if (g_in_fatal.exchange(1) == 0) {
        char buf[256];
        uint64_t pc = 0;
#if defined(__x86_64__)
        pc = static_cast<ucontext_t*>(uc)->uc_mcontext->__ss.__rip;
#elif defined(__aarch64__)
        pc = static_cast<ucontext_t*>(uc)->uc_mcontext->__ss.__pc;
#endif
        const int len = std::snprintf(buf, sizeof buf, "[host] fatal signal %d (%s) addr=%p pc=%#" PRIx64 " exe+%#" PRIx64 "\n",
                                      sig, strsignal(sig), si ? si->si_addr : nullptr, pc, pc - g_exe_info.base);
        write(2, buf, static_cast<size_t>(len));
        host::trace::dump_fatal("fatal signal");
        host::governor::summary();
        const size_t n = gta6rt_shims_report_unimplemented();
        std::fprintf(stderr, "=== %zu call(s) reached unimplemented imports; %llu isa traps (SSE4a, CLWB/CLZERO/MONITORX, cpuid); "
                     "null_pso_skips %llu, fsr_skips %llu ===\n", n, static_cast<unsigned long long>(gta6rt_isa_trap_count()),
                     static_cast<unsigned long long>(host::title::null_pso_skips()),
                     static_cast<unsigned long long>(host::title::fsr_skips()));
        std::fflush(stderr);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_fatal_handlers() {
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT}) {
        struct sigaction sa{};
        sa.sa_sigaction = fatal_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, &g_prev[sig]);
    }
}

void install_alt_stack() {
    stack_t ss{};
    ss.ss_size = 256 * 1024;
    ss.ss_sp = mmap(nullptr, ss.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ss.ss_sp != MAP_FAILED) sigaltstack(&ss, nullptr);
}

// ---- thread hooks -------------------------------------------------------------------------------------------

gta6rt_status on_thread_start(gta6rt_thread* self, void*) {
    gta6rt_teb_desc d{};
    gta6rt_thread_stack(self, &d.stack_base, &d.stack_limit);
    d.tls_pointer = nullptr;
    d.peb = g_peb;
    d.process_id = static_cast<uint32_t>(getpid());
    d.thread_id = gta6rt_thread_id(self);
    void* teb = nullptr;
    if (const gta6rt_status st = gta6rt_isa_thread_attach(&d, &teb); st != GTA6RT_OK) return st;
    gta6rt_thread_set_user_slot(self, 0, teb);
    if (g_images_initialised.load()) gta6rt_loader_notify_thread(kDllThreadAttach);
    return GTA6RT_OK;
}

void on_thread_exit(gta6rt_thread*, void*) {
    if (g_images_initialised.load()) gta6rt_loader_notify_thread(kDllThreadDetach);
    gta6rt_shims_thread_detach();
    gta6rt_isa_thread_detach();
}

// ---- PEB ----------------------------------------------------------------------------------------------------

void put_ustr(uint8_t* at, uint8_t*& pool, const char* ascii) {
    const size_t n = std::strlen(ascii);
    auto* w = reinterpret_cast<char16_t*>(pool);
    for (size_t i = 0; i < n; ++i) w[i] = static_cast<char16_t>(ascii[i]);
    w[n] = 0;
    const uint16_t bytes = static_cast<uint16_t>(n * 2);
    std::memcpy(at + 0, &bytes, 2);
    const uint16_t max = static_cast<uint16_t>(bytes + 2);
    std::memcpy(at + 2, &max, 2);
    const uint64_t p = reinterpret_cast<uint64_t>(w);
    std::memcpy(at + 8, &p, 8);
    pool += (n + 1) * 2 + 15;
    pool = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(pool)) & ~uintptr_t(15));
}

gta6rt_status build_peb(gta6rt_gva image_base) {
    void* mem = nullptr;
    if (gta6rt_status st = gta6rt_vm_reserve(nullptr, 0x10000, 0, &mem); st != GTA6RT_OK) return st;
    if (gta6rt_status st = gta6rt_vm_commit(mem, 0x10000, GTA6RT_PROT_READ | GTA6RT_PROT_WRITE); st != GTA6RT_OK) return st;
    auto* peb = static_cast<uint8_t*>(mem);
    auto* params = peb + 0x1000;
    uint8_t* pool = peb + 0x2000;
    const uint64_t heap = reinterpret_cast<uint64_t>(GetProcessHeap());
    const uint64_t pp = reinterpret_cast<uint64_t>(params);
    const uint32_t ncpu = static_cast<uint32_t>(sysconf(_SC_NPROCESSORS_ONLN));
    std::memcpy(peb + GTA6RT_PEB_IMAGE_BASE, &image_base, 8);
    std::memcpy(peb + GTA6RT_PEB_PROCESS_PARAMETERS, &pp, 8);
    std::memcpy(peb + GTA6RT_PEB_PROCESS_HEAP, &heap, 8);
    std::memcpy(peb + GTA6RT_PEB_NUMBER_OF_PROCESSORS, &ncpu, 4);
    const uint32_t len = 0x400;
    std::memcpy(params + 0, &len, 4);
    std::memcpy(params + 4, &len, 4);
    put_ustr(params + 0x38, pool, "G:\\");
    put_ustr(params + 0x60, pool, "G:\\game_scarlett_master.exe");
    put_ustr(params + 0x70, pool, "game_scarlett_master.exe");
    g_peb = peb;
    return gta6rt_isa_set_peb(peb);
}

// ---- import resolution -------------------------------------------------------------------------------------

const char* const kSehExports[] = {
    "RtlCaptureContext", "RtlLookupFunctionEntry", "RtlVirtualUnwind", "RtlUnwindEx", "RtlUnwind", "RtlPcToFileHeader",
    "RtlAddFunctionTable", "RtlDeleteFunctionTable", "RtlInstallFunctionTableCallback", "RtlRestoreContext",
    "RtlRaiseException", "RtlCaptureStackBackTrace", "RaiseException", "AddVectoredExceptionHandler",
    "RemoveVectoredExceptionHandler", "AddVectoredContinueHandler", "RemoveVectoredContinueHandler",
    "SetUnhandledExceptionFilter", "UnhandledExceptionFilter",
};

#if !defined(__x86_64__)
// arm64: seh/ has no Win64-ABI exports (gta6rt_seh_native_export returns 0), so the ones the title calls before
// WinMain are served here as frame handlers over seh/'s public API.
void seh_pc_to_file_header(gta6rt_guest_frame* f, void*) {
    const uint64_t base = gta6rt_seh_image_base_of(f->rcx);
    if (f->rdx) std::memcpy(reinterpret_cast<void*>(f->rdx), &base, 8);
    f->rax = base;
}

void seh_set_unhandled_filter(gta6rt_guest_frame* f, void*) { f->rax = gta6rt_seh_set_unhandled_filter(f->rcx); }

void seh_lookup_function_entry(gta6rt_guest_frame* f, void*) {
    uint64_t base = 0;
    const gta6rt_runtime_function* fn = gta6rt_seh_lookup(f->rcx, &base);
    if (f->rdx) std::memcpy(reinterpret_cast<void*>(f->rdx), &base, 8);
    f->rax = reinterpret_cast<uint64_t>(fn);
}

unsigned register_seh_frame_exports() {
    const struct {
        const char* name;
        gta6rt_frame_handler fn;
    } rows[] = {{"RtlPcToFileHeader", &seh_pc_to_file_header},
                {"SetUnhandledExceptionFilter", &seh_set_unhandled_filter},
                {"RtlLookupFunctionEntry", &seh_lookup_function_entry}};
    unsigned n = 0;
    for (const auto& r : rows) {
        rt::shims::Found f;
        f.frame = r.fn;
        rt::shims::register_entry("kernel32.dll", r.name, f);
        rt::shims::register_entry("ntdll.dll", r.name, f);
        ++n;
    }
    return n;
}

void d3d12_configure_virtual_memory(gta6rt_gva fn, gta6rt_guest_frame* f) {
    f->rax = static_cast<uint64_t>(static_cast<int64_t>(reinterpret_cast<int32_t (*)(void*)>(fn)(reinterpret_cast<void*>(f->rcx))));
}

void d3d12_cvm_frame(gta6rt_guest_frame* f, void*) {
    static void* fn = rt::d3d12_vulkan::find_export("d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX");
    d3d12_configure_virtual_memory(reinterpret_cast<gta6rt_gva>(fn), f);
}
#endif

unsigned register_seh_exports() {
#if !defined(__x86_64__)
    return register_seh_frame_exports();
#endif
    unsigned n = 0;
    for (const char* name : kSehExports) {
        const uint64_t a = gta6rt_seh_native_export(name);
        if (!a) continue;
        rt::shims::Found f;
        f.fn = reinterpret_cast<void*>(a);
        rt::shims::register_entry("kernel32.dll", name, f);
        rt::shims::register_entry("ntdll.dll", name, f);
        ++n;
    }
    return n;
}

bool address_is_code(gta6rt_gva a) {
    if (const gta6rt_image* img = gta6rt_loader_find_image(a)) {
        gta6rt_image_info info{};
        if (gta6rt_loader_info(img, &info) != GTA6RT_OK) return false;
        return gta6rt_loader_rva_section_flags(img, static_cast<uint32_t>(a - info.base)) & kScnMemExecute;
    }
    mach_vm_address_t addr = a;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info),
                       &count, &obj) != KERN_SUCCESS)
        return false;
    return addr <= a && (info.protection & VM_PROT_EXECUTE);
}

std::string import_key(const gta6rt_import_ref* ref) {
    return lower(ref->dll) + "!" + (ref->name ? std::string(ref->name) : "#" + std::to_string(ref->ordinal));
}

std::atomic<uint32_t> g_unimpl_bindings{0};
std::mutex g_unimpl_mu;
std::vector<std::string> g_unimpl_list;

gta6rt_status host_resolver(void*, const gta6rt_import_ref* ref, gta6rt_import_binding* out) {
    if (!ref || !ref->dll || !out) return GTA6RT_E_INVALID_ARG;
    const std::string dll = lower(ref->dll);
    const std::string name = ref->name ? std::string(ref->name) : "#" + std::to_string(ref->ordinal);
    const std::string key = dll + "!" + name;
    const rt::shims::Found f = rt::shims::find_entry(dll, name);
    if (ref->kind_guess == GTA6RT_IMPORT_DATA || (f && f.data)) return gta6rt_shims_import_resolver(nullptr, ref, out);
    gta6rt_gva a = 0;
    if (rt::shims::guest_address(dll, name, &a, false) == GTA6RT_OK) {
        out->address = host::trace::wrap(a, key.c_str(), false);
        return GTA6RT_OK;
    }
    bool stub = false;
    if (const gta6rt_status st = rt::shims::guest_address(dll, name, &a, true, &stub); st != GTA6RT_OK) return st;
    g_unimpl_bindings.fetch_add(1);
    {
        std::lock_guard lock(g_unimpl_mu);
        g_unimpl_list.push_back(key);
    }
    host::trace::note_unimplemented_binding(key.c_str());
    out->address = host::trace::wrap(a, key.c_str(), true);
    return GTA6RT_OK;
}

gta6rt_status exe_resolver(void* user, const gta6rt_import_ref* ref, gta6rt_import_binding* out) {
    const gta6rt_status st = gta6rt_loader_package_import_resolver(user, ref, out);
    if (st == GTA6RT_OK && host::trace::enabled() && ref->kind_guess == GTA6RT_IMPORT_CODE && address_is_code(out->address))
        out->address = host::trace::wrap(out->address, import_key(ref).c_str(), false);
    return st;
}

using GetProcAddressFn = GTA6RT_GUEST_ABI uint64_t (*)(uint64_t, const char*);

#if !defined(__x86_64__)
gta6rt_frame_handler g_get_proc_address_frame = nullptr;

void host_get_proc_address_frame(gta6rt_guest_frame* f, void* user) {
    g_get_proc_address_frame(f, user);
    const uint64_t a = f->rax;
    const auto* name = reinterpret_cast<const char*>(f->rdx);
    const std::string nm = reinterpret_cast<uintptr_t>(name) < 0x10000 ? "#" + std::to_string(reinterpret_cast<uintptr_t>(name))
                                                                        : std::string(name ? name : "(null)");
    if (!a) {
        char mod[64];
        std::snprintf(mod, sizeof mod, "%#" PRIx64, f->rcx);
        if (const gta6rt_module* m = gta6rt_module_find_by_base(f->rcx)) std::snprintf(mod, sizeof mod, "%s", gta6rt_module_info(m)->name);
        host::trace::note_missing_proc(mod, nm.c_str());
        return;
    }
    if (host::trace::enabled() && address_is_code(a)) f->rax = host::trace::wrap(a, ("GetProcAddress:" + nm).c_str(), false);
}
#endif

#if defined(__x86_64__)
GetProcAddressFn g_get_proc_address = nullptr;

GTA6RT_GUEST_ABI uint64_t host_get_proc_address(uint64_t module, const char* name) {
    const uint64_t a = g_get_proc_address(module, name);
    const std::string nm = reinterpret_cast<uintptr_t>(name) < 0x10000
                               ? "#" + std::to_string(reinterpret_cast<uintptr_t>(name))
                               : std::string(name ? name : "(null)");
    if (!a) {
        char mod[64];
        std::snprintf(mod, sizeof mod, "%#" PRIx64, module);
        if (const gta6rt_module* m = gta6rt_module_find_by_base(module)) std::snprintf(mod, sizeof mod, "%s", gta6rt_module_info(m)->name);
        host::trace::note_missing_proc(mod, nm.c_str());
        return 0;
    }
    if (host::trace::enabled() && address_is_code(a)) return host::trace::wrap(a, ("GetProcAddress:" + nm).c_str(), false);
    return a;
}
#endif

void hook_get_proc_address() {
#if !defined(__x86_64__)
    const rt::shims::Found found = rt::shims::find_entry("kernel32.dll", "GetProcAddress");
    if (!found.frame) return;
    g_get_proc_address_frame = found.frame;
    rt::shims::Found f;
    f.frame = &host_get_proc_address_frame;
    rt::shims::register_entry("kernel32.dll", "GetProcAddress", f);
    return;
#endif
    auto* orig = reinterpret_cast<GetProcAddressFn>(rt::shims::find("kernel32.dll", "GetProcAddress"));
    if (!orig) return;
#if defined(__x86_64__)
    g_get_proc_address = orig;
    rt::shims::Found f;
    f.fn = reinterpret_cast<void*>(&host_get_proc_address);
    rt::shims::register_entry("kernel32.dll", "GetProcAddress", f);
#endif
}

// ---- package DLLs -------------------------------------------------------------------------------------------

void fix_new_images() {
    gta6rt_image* imgs[64];
    const uint32_t n = gta6rt_loader_package_images(imgs, 64);
    std::lock_guard lock(g_fixup_mu);
    for (uint32_t i = 0; i < n && i < 64; ++i) {
        gta6rt_image_info info{};
        if (gta6rt_loader_info(imgs[i], &info) != GTA6RT_OK || !g_fixed_images.insert(info.base).second) continue;
        gta6rt_isa_fixup_report rep{};
        const gta6rt_status st =
            gta6rt_isa_apply_image_fixups(info.base, info.size, GTA6RT_ISA_FIXUP_CHKSTK | GTA6RT_ISA_FIXUP_ZEN, &rep);
        gta6rt_isa_register_guest_code(info.base, info.size);
        const char* leaf = info.path_utf8 ? std::strrchr(info.path_utf8, '/') : nullptr;
        if (g_args.null_pso_patch && leaf && !std::strcmp(leaf + 1, "amd_fidelityfx_upscaler_xbox.dll")) {
            uint8_t patch[13];
            uint32_t rva = 0;
            if (host::title::patch_fsr_null_pipeline(info.base, patch, &rva))
                logf("FSR null-pipeline patch at amd_fidelityfx_upscaler_xbox.dll+%#x: %s (a dispatch whose pipeline the "
                     "layer refused returns without recording; counted as fsr_skips)",
                     rva, gta6rt_status_name(gta6rt_isa_patch_code(info.base + rva, patch, sizeof patch)));
        }
        logf("dll %s at %#" PRIx64 "%s size %#" PRIx64 ": fixups %s (chkstk %u, clzero loops %u)",
             info.path_utf8 ? std::strrchr(info.path_utf8, '/') + 1 : "?", info.base,
             info.base != info.preferred_base ? " (relocated)" : "", info.size, gta6rt_status_name(st), rep.chkstk,
             rep.clzero_loops);
    }
}

void* bundled_find_loaded(void*, const std::string& dll) {
    gta6rt_image* img = gta6rt_loader_package_find(dll.c_str());
    if (!img) return nullptr;
    gta6rt_image_info info{};
    gta6rt_loader_info(img, &info);
    return reinterpret_cast<void*>(info.base);
}

void* bundled_load(void*, const std::string& dll, uint32_t, bool& handled) {
    handled = false;
    if (!gta6rt_loader_package_exists(dll.c_str())) return nullptr;
    handled = true;
    gta6rt_image* img = nullptr;
    const gta6rt_status st = gta6rt_loader_package_load(dll.c_str(), GTA6RT_PKG_LOAD_DYNAMIC, &img);
    logf("LoadLibrary(%s): package DLL, load %s", dll.c_str(), gta6rt_status_name(st));
    if (st != GTA6RT_OK) {
        gta6rt_last_error_set(126);
        return nullptr;
    }
    fix_new_images();
    if (g_images_initialised.load()) gta6rt_loader_package_initialize();
    gta6rt_image_info info{};
    gta6rt_loader_info(img, &info);
    return reinterpret_cast<void*>(info.base);
}

void* bundled_get_proc(void*, void*, const std::string&, bool& owns) {
    owns = false;
    return nullptr;
}

bool bundled_owns(void*, void*) { return false; }

std::string utf8_of(const std::u16string& w) {
    std::string s;
    for (char16_t c : w) s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return lower(s);
}

void* bundled_find_loaded_w(void* ctx, const std::u16string& base) { return bundled_find_loaded(ctx, utf8_of(base)); }
void* bundled_load_w(void* ctx, const std::u16string& base, uint32_t flags, bool& handled) {
    return bundled_load(ctx, utf8_of(base), flags, handled);
}

const rt::shims::BundledModuleOps kBundledOps{&bundled_find_loaded, &bundled_load, &bundled_get_proc, &bundled_owns,
                                             &bundled_find_loaded_w, &bundled_load_w};

bool lookup_module(void*, uint64_t address, std::string& module, uint32_t& rva) {
    const gta6rt_image* img = gta6rt_loader_find_image(address);
    if (!img) return false;
    gta6rt_image_info info{};
    if (gta6rt_loader_info(img, &info) != GTA6RT_OK) return false;
    const char* p = info.path_utf8 ? std::strrchr(info.path_utf8, '/') : nullptr;
    module = p ? p + 1 : (info.path_utf8 ? info.path_utf8 : "?");
    rva = static_cast<uint32_t>(address - info.base);
    return true;
}

gta6rt_gva exe_export(void* user, const char* name, uint32_t ordinal) {
    return gta6rt_loader_get_export(static_cast<const gta6rt_image*>(user), name, ordinal);
}

// ---- marker -------------------------------------------------------------------------------------------------

#if !defined(__x86_64__)
void marker_frame(gta6rt_guest_frame* f, void*) {
    gta6rt_thread* t = gta6rt_thread_current();
    std::fprintf(stderr, "=== REACHED MARKER: RVA %#x on thread %#x ===\n", g_args.marker_rva, t ? gta6rt_thread_id(t) : 0);
    std::fprintf(stderr, "args: rcx=%#" PRIx64 " rdx=%#" PRIx64 " r8=%#" PRIx64 " r9=%#" PRIx64 "\n", f->rcx, f->rdx, f->r8, f->r9);
    if (g_args.marker_rva == kWinMainRva && f->r8) std::fprintf(stderr, "WinMain lpCmdLine=\"%s\"\n", reinterpret_cast<const char*>(f->r8));
    report_unimplemented_and_trace("marker reached");
    _exit(0);
}
#endif

#if defined(__x86_64__)
GTA6RT_GUEST_ABI uint64_t marker_hit(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9) {
    gta6rt_thread* t = gta6rt_thread_current();
    std::fprintf(stderr, "=== REACHED MARKER: RVA %#x on thread %#x ===\n", g_args.marker_rva, t ? gta6rt_thread_id(t) : 0);
    std::fprintf(stderr, "args: rcx=%#" PRIx64 " rdx=%#" PRIx64 " r8=%#" PRIx64 " r9=%#" PRIx64 "\n", rcx, rdx, r8, r9);
    if (g_args.marker_rva == kWinMainRva && r8) std::fprintf(stderr, "WinMain lpCmdLine=\"%s\"\n", reinterpret_cast<const char*>(r8));
    report_unimplemented_and_trace("marker reached");
    _exit(0);
}
#endif

gta6rt_status patch_marker() {
#if defined(__x86_64__)
    gta6rt_thunk_desc d{};
    d.kind = GTA6RT_THUNK_NATIVE;
    d.fn = reinterpret_cast<const void*>(&marker_hit);
    d.debug_name = "host!marker";
    gta6rt_gva thunk = 0;
    if (gta6rt_status st = gta6rt_isa_make_thunk(&d, &thunk); st != GTA6RT_OK) return st;
    uint8_t code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    std::memcpy(code + 2, &thunk, 8);
    return gta6rt_loader_patch_rva(g_exe, g_args.marker_rva, code, sizeof code);
#else
    gta6rt_thunk_desc d{};
    d.kind = GTA6RT_THUNK_FRAME;
    d.fn = reinterpret_cast<const void*>(&marker_frame);
    d.debug_name = "host!marker";
    gta6rt_gva thunk = 0;
    if (gta6rt_status st = gta6rt_isa_make_thunk(&d, &thunk); st != GTA6RT_OK) return st;
    uint8_t code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    std::memcpy(code + 2, &thunk, 8);
    return gta6rt_loader_patch_rva(g_exe, g_args.marker_rva, code, sizeof code);
#endif
}

// Exit paths with the title past device creation: memory totals, then the D3D12 layer's compiler threads and session
// pipeline cache (gta6rt_d3d12_vulkan_at_exit waits up to 2 s for a running build).
void finish_run(const char* why) {
    if (g_args.governor.mode == host::governor::Mode::off) gta6rt_os_memory_report(why, 64ull << 20, 12);
    else host::governor::finish(why);
    const auto t0 = std::chrono::steady_clock::now();
    const gta6rt_status st = gta6rt_d3d12_vulkan_at_exit(2000);
    logf("d3d12_vulkan_at_exit: %s after %.0f ms", gta6rt_status_name(st),
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    std::fflush(stderr);
}

void start_watchdog(unsigned seconds) {
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        std::fprintf(stderr, "[host] watchdog: %u s elapsed, stopping the run\n", seconds);
        report_unimplemented_and_trace("watchdog");
        finish_run("watchdog");
        _exit(124);
    }).detach();
}

}  // namespace

int main(int argc, char** argv) {
#if defined(__x86_64__)
    const char* arch = "x86_64";
#else
    const char* arch = "arm64";
#endif
    if (!parse_args(argc, argv)) {
        usage();
        return 2;
    }
    if (!g_args.governor.report_s)
        if (const char* e = getenv("GTA6RT_MEMORY_REPORT_S")) g_args.governor.report_s = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    host::governor::prepare(g_args.governor);
    if (g_args.reexec) {
        const gta6rt_status st = gta6rt_isa_reexec_for_cpuid(argv);
        if (st != GTA6RT_OK) logf("reexec_for_cpuid: %s", gta6rt_status_name(st));
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    logf("gta6rt: arch=%s isa=%s pid=%d guest range reserved=%#zx", arch, gta6rt_isa_backend_name(), getpid(), g_guard_size);

    install_alt_stack();
    install_fatal_handlers();
    if (gta6rt_status st = gta6rt_os_install_fault_handler(); st != GTA6RT_OK) logf("os fault handler: %s", gta6rt_status_name(st));
    gta6rt_isa_config cfg{};
    if (gta6rt_status st = gta6rt_isa_init(&cfg); st != GTA6RT_OK) {
        logf("isa_init: %s", gta6rt_status_name(st));
        return 1;
    }
    const uint64_t advertised = gta6rt_isa_advertised_features();
    logf("cpuid: advertised=%#" PRIx64 " policy=%#llx enforced=%d host_cpu.check_skipped=%d", advertised,
         static_cast<unsigned long long>(GTA6RT_CPUID_DEFAULT), gta6rt_isa_cpuid_enforced(), rt::host_cpu::check_skipped() ? 1 : 0);
    const gta6rt_seh_policy policy = g_args.seh < 0 ? GTA6RT_SEH_LOG_AND_ABORT : static_cast<gta6rt_seh_policy>(g_args.seh);
    if (gta6rt_status st = gta6rt_seh_init(policy); st != GTA6RT_OK) {
        logf("seh_init: %s", gta6rt_status_name(st));
        return 1;
    }
    logf("seh policy: %s", gta6rt_seh_get_policy() == GTA6RT_SEH_DISPATCH ? "dispatch" : "log_and_abort");

    const char* profile_env = getenv("LEONIDA_CONSOLE_PROFILE");
    std::string profile = g_args.console_profile;
    if (profile.empty() && !(profile_env && *profile_env) && g_args.marker) {
        profile = "series-x";
        logf("console profile series-x for --stop-at: the exe's pre-WinMain import count (36,739) is the S1 baseline; "
             "Series S makes 26 more calls (36,765)");
    }
    if (profile.empty() && !(profile_env && *profile_env)) {
        std::string why;
        profile = host::governor::profile_for_budget(host::governor::compute_budget(0), &why);
        logf("console profile %s from the memory budget: %s", profile.c_str(), why.c_str());
        setenv("LEONIDA_CONSOLE_PROFILE", profile.c_str(), 1);
    }
    if (!profile.empty()) {
        if (gta6rt_status st = gta6rt_shims_set_console_profile(profile.c_str()); st != GTA6RT_OK) {
            logf("console profile %s: %s", profile.c_str(), gta6rt_status_name(st));
            return 2;
        }
    }
    if (gta6rt_status st = gta6rt_shims_init(); st != GTA6RT_OK) {
        logf("shims_init: %s", gta6rt_status_name(st));
        return 1;
    }
    const gta6rt_status gdk_st = gta6rt_shims_gdk_register(), d3d_st = gta6rt_d3d12_vulkan_register();
    logf("shims_gdk_register=%s d3d12_vulkan_register=%s media_register=%s", gta6rt_status_name(gdk_st),
         gta6rt_status_name(d3d_st), gta6rt_status_name(gta6rt_media_register()));
#if !defined(__x86_64__)
    if (d3d_st != GTA6RT_OK) {
        rt::shims::Found f;
        f.frame = &d3d12_cvm_frame;
        rt::shims::register_entry("d3d12_xs.dll", "D3D12ConfigureVirtualMemoryX", f);
        logf("arm64: d3d12_xs.dll!D3D12ConfigureVirtualMemoryX served by a host frame handler (d3d12_vulkan has no arm64 table)");
    }
#endif
    logf("seh exports bound through the shim table: %u", register_seh_exports());
    hook_get_proc_address();
    host::governor::install_title_hooks();
    host::window::set_hidden(g_args.hidden_window);
    if (!g_args.hud.empty()) setenv("GTA6RT_HUD", g_args.hud.c_str(), 1);
    if (!g_args.hidden_window) {
        setenv("GTA6RT_HUD", "min", 0);
        gta6rt_hud_init_from_env();
        if (const char* profile = std::getenv("LEONIDA_CONSOLE_PROFILE")) gta6rt_hud_set_profile(profile);
        if (!g_args.vk_driver.empty()) gta6rt_hud_set_driver(g_args.vk_driver.c_str());
        if (host::trace::enabled()) gta6rt_hud_set_import_counter([](void*) { return host::trace::total_calls(); }, nullptr);
    }
    rt::shims::add_module_lookup(&g_args, &lookup_module);
    rt::shims::add_bundled_loader(&g_args, &kBundledOps);

    g_args.package = absolute(g_args.package);
    g_args.exe = absolute(g_args.exe);
    make_dirs(g_args.run_dir);
    g_args.run_dir = absolute(g_args.run_dir);
    const std::string overlay = g_args.run_dir + "/overlay";
    const std::string saves = g_args.run_dir + "/saves";
    make_dirs(overlay);
    make_dirs(saves);
    make_dirs(g_args.run_dir + "/drives");
    setenv("LEONIDA_SAVE_DIR", saves.c_str(), 0);
    configure_vulkan(g_args.run_dir);
    gta6rt_shims_configure_paths(g_args.package.c_str(), overlay.c_str());
    gta6rt_path_set_root((g_args.run_dir + "/drives").c_str());
    gta6rt_path_map_prefix("GAME:", g_args.package.c_str());
    gta6rt_path_map_prefix("G:", g_args.package.c_str());
    gta6rt_path_map_prefix("T:", overlay.c_str());
    logf("exe %s", g_args.exe.c_str());
    logf("package (read-only) %s", g_args.package.c_str());
    logf("writable state %s", g_args.run_dir.c_str());

    std::vector<const char*> targs;
    for (const auto& s : g_args.title_args) targs.push_back(s.c_str());
    gta6rt_shims_set_command_line("game_scarlett_master.exe", targs.data(), targs.size());
    std::string cmdline = "game_scarlett_master.exe";
    for (const auto& s : g_args.title_args) cmdline += " " + s;
    gta6rt_process_set_command_line(cmdline.c_str());

    gta6rt_package_desc pkg{};
    pkg.size = sizeof pkg;
    pkg.dir_utf8 = g_args.package.c_str();
    pkg.resolver = &host_resolver;
    if (gta6rt_status st = gta6rt_loader_package_init(&pkg); st != GTA6RT_OK) {
        logf("package_init: %s", gta6rt_status_name(st));
        return 1;
    }

    release_guest_range();
    gta6rt_map_options mo{};
    mo.flags = GTA6RT_MAP_ALLOW_RELOCATE | (g_args.force_reloc ? GTA6RT_MAP_FORCE_RELOCATE : 0u);
    if (gta6rt_status st = gta6rt_loader_map_file(g_args.exe.c_str(), &mo, &g_exe); st != GTA6RT_OK) {
        logf("map %s: %s", g_args.exe.c_str(), gta6rt_status_name(st));
        return 1;
    }
    gta6rt_loader_info(g_exe, &g_exe_info);
    logf("exe mapped at %#" PRIx64 " (preferred %#" PRIx64 ") size %#" PRIx64 " entry %#" PRIx64 " stack reserve %#" PRIx64,
         g_exe_info.base, g_exe_info.preferred_base, g_exe_info.size, g_exe_info.entry, g_exe_info.stack_reserve);
    host::trace::configure(g_args.trace, g_exe_info.base, g_exe_info.size);
    host::governor::start(g_args.marker ? 0 : g_exe_info.base);
    if (gta6rt_status st = gta6rt_loader_relocate(g_exe); st != GTA6RT_OK) {
        logf("relocate: %s", gta6rt_status_name(st));
        return 1;
    }
    if (gta6rt_status st = build_peb(g_exe_info.base); st != GTA6RT_OK) {
        logf("peb: %s", gta6rt_status_name(st));
        return 1;
    }
    gta6rt_thread_hooks hooks{&on_thread_start, &on_thread_exit, nullptr};
    gta6rt_thread_set_hooks(&hooks);

    gta6rt_bind_stats bs{};
    if (gta6rt_status st = gta6rt_loader_bind_imports(g_exe, &exe_resolver, nullptr, &bs); st != GTA6RT_OK) {
        logf("bind: %s", gta6rt_status_name(st));
        return 1;
    }
    logf("exe imports: %u total, %u bound, %u trapped, %u data (%u zero storage), %u bound to unimplemented stubs", bs.total,
         bs.bound, bs.trapped, bs.data, bs.data_zero_storage, g_unimpl_bindings.load());
    fix_new_images();

#if defined(__x86_64__)
    const uint32_t fix_flags = (advertised == GTA6RT_CPUID_DEFAULT ? (GTA6RT_ISA_FIXUP_ALL & ~GTA6RT_ISA_FIXUP_CPUID) : GTA6RT_ISA_FIXUP_ALL) |
                               (g_args.no_zen_fixup ? 0u : GTA6RT_ISA_FIXUP_ZEN);
#else
    // FEX executes insertq, vpsllvq/vpsrlvq and cpuid itself (isa/fexcore/STATUS.md); MONITORX/MWAITX still need ZEN.
    const uint32_t fix_flags = GTA6RT_ISA_FIXUP_CHKSTK | (g_args.no_zen_fixup ? 0u : GTA6RT_ISA_FIXUP_ZEN);
#endif
    gta6rt_isa_fixup_report rep{};
    const gta6rt_status fst = gta6rt_isa_apply_image_fixups(g_exe_info.base, g_exe_info.size, fix_flags, &rep);
    logf("exe fixups (%#x): %s known=%u chkstk=%u sse4a=%u vshift=%u cpuid=%u xgetbv=%u mismatched=%u vshift_safe=%u zen_nop=%u "
         "clzero_loops=%u zen_wait=%u vshift_checked=%u", fix_flags, gta6rt_status_name(fst), rep.known_image, rep.chkstk, rep.sse4a, rep.vshift, rep.cpuid,
         rep.xgetbv, rep.mismatched, rep.vshift_safe, rep.zen_nop, rep.clzero_loops, rep.zen_wait, rep.vshift_checked);
    if (g_args.marker) logf("marker at RVA %#x: %s", g_args.marker_rva, gta6rt_status_name(patch_marker()));
    if (g_args.null_pso_patch && !g_args.marker) {
        uint8_t patch[12];
        uint32_t rva = 0;
        if (host::title::patch_null_pso_bind(g_exe_info.base, patch, &rva))
            logf("null-PSO bind patch at RVA %#x: %s (a refused PSO leaves the previous one bound; counted as null_pso_skips)",
                 rva, gta6rt_status_name(gta6rt_loader_patch_rva(g_exe, rva, patch, sizeof patch)));
    }
    for (uint32_t rva : g_args.breaks) {
        const uint8_t int3 = 0xCC;
        logf("breakpoint at RVA %#x: %s", rva, gta6rt_status_name(gta6rt_loader_patch_rva(g_exe, rva, &int3, 1)));
    }
    if (gta6rt_status st = gta6rt_loader_finalize_protections(g_exe); st != GTA6RT_OK) {
        logf("finalize_protections: %s", gta6rt_status_name(st));
        return 1;
    }
    uint32_t tls_index = 0;
    gta6rt_status tst = gta6rt_loader_tls_assign_index(g_exe, &tls_index);
    logf("exe tls index: %s %u", gta6rt_status_name(tst), tls_index);

    const std::string module_path = g_args.package + "/game_scarlett_master.exe";
    gta6rt_module_desc md{};
    md.name = "game_scarlett_master.exe";
    md.path_utf8 = module_path.c_str();
    md.kind = GTA6RT_MODULE_GUEST_IMAGE;
    md.base = g_exe_info.base;
    md.size = g_exe_info.size;
    md.get_export = &exe_export;
    md.user = g_exe;
    gta6rt_module* exe_module = nullptr;
    if (gta6rt_status st = gta6rt_module_register(&md, &exe_module); st != GTA6RT_OK) logf("module_register: %s", gta6rt_status_name(st));
    else gta6rt_module_set_main(exe_module);
    const void* table = nullptr;
    uint32_t count = 0;
    if (gta6rt_loader_exception_table(g_exe, &table, &count) == GTA6RT_OK) {
        const gta6rt_status st = gta6rt_seh_add_function_table(g_exe_info.base, static_cast<const gta6rt_runtime_function*>(table),
                                                                count, g_exe_info.size);
        logf("exe exception table: %u entries, %s", count, gta6rt_status_name(st));
    }
    gta6rt_isa_register_guest_code(g_exe_info.base, g_exe_info.size);
    if (const gta6rt_gva hooks_va = gta6rt_loader_get_export(g_exe, "__XMemAllocationHooks__", 0)) {
        rt::shims::add_xmem_allocation_hooks(g_exe, reinterpret_cast<const rt::shims::XMemAllocationHooks*>(hooks_va));
        logf("exe exports __XMemAllocationHooks__ at %#" PRIx64, hooks_va);
    }

    if (g_args.map_only) {
        std::lock_guard lock(g_unimpl_mu);
        logf("imports bound to unimplemented stubs (exe and package DLLs): %zu", g_unimpl_list.size());
        for (const auto& k : g_unimpl_list) logf("  unimplemented: %s", k.c_str());
        logf("--map-only: not running any image code");
        return 0;
    }

    gta6rt_thread* main_thread = nullptr;
    if (gta6rt_status st = gta6rt_thread_adopt_current(1, &main_thread); st != GTA6RT_OK) {
        logf("adopt main thread: %s", gta6rt_status_name(st));
        return 1;
    }
    logf("main thread %#x teb=%p", gta6rt_thread_id(main_thread), gta6rt_isa_current_teb());
    if (g_args.watchdog_s) start_watchdog(g_args.watchdog_s);
    if (const char* v = getenv("LEONIDA_TITLE_WATCHDOG"); v && !std::strcmp(v, "0")) g_args.title_watchdog = false;
    if (const char* v = getenv("LEONIDA_PEEK"); v && *v && g_args.peek.empty()) g_args.peek = v;
    if (!g_args.title_watchdog) host::title::start_watchdog_kick(g_exe_info.base);
    if (!g_args.marker && (g_args.stage_watch || !g_args.peek.empty()))
        host::title::start_peek(g_exe_info.base, g_args.peek, g_args.peek_s, g_args.stage_watch);

    const auto t0 = std::chrono::steady_clock::now();
    const gta6rt_status ist = gta6rt_loader_package_initialize();
    logf("package DllMain/TLS initialisation: %s (%" PRIu64 " import calls so far)", gta6rt_status_name(ist), host::trace::total_calls());
    const gta6rt_status cst = gta6rt_loader_run_tls_callbacks(g_exe, kDllProcessAttach);
    logf("exe TLS callbacks: %s", gta6rt_status_name(cst));
    g_images_initialised.store(true);

    const uint64_t stack = g_exe_info.stack_reserve > (8u << 20) ? g_exe_info.stack_reserve : (8u << 20);
    logf("running entry %#" PRIx64 " (exe+%#" PRIx64 ") on a %" PRIu64 " MiB guest thread", g_exe_info.entry,
         g_exe_info.entry - g_exe_info.base, stack >> 20);
    std::atomic<bool> done{false};
    int code = 0;
    std::thread runner([&] {
        code = gta6rt_shims_run_entry(g_exe_info.entry, stack);
        done.store(true);
        CFRunLoopWakeUp(CFRunLoopGetMain());
    });
    while (!done.load()) {
        if (host::window::windows_created() && !g_args.hidden_window) gta6rt_mac_window_pump_events(0.05);
        else CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }
    runner.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    logf("title exited with code %d (%#x) after %.2f s", code, static_cast<unsigned>(code), secs);
    report_unimplemented_and_trace("title exit");
    finish_run("title exit");
    gta6rt_hud_shutdown();
    _exit(code);
}
