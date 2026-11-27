#include "phase.h"

#include <gta6rt/os.h>

#include <cstdint>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

namespace gta6rt::pcache {

namespace {

constexpr std::uint32_t kGameStateRva = 0x7446510;
constexpr std::uint32_t kSessionRva = 0x98f828c;
constexpr std::uint32_t kLoadingRva = 0x7453ed0;

bool read_u64(std::uint64_t at, std::uint64_t* out) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), at, sizeof *out, reinterpret_cast<mach_vm_address_t>(out), &got) ==
               KERN_SUCCESS &&
           got == sizeof *out;
}

std::uint64_t exe_base() {
    static std::uint64_t base = 0;
    if (base) return base;
    for (std::size_t i = 0;; ++i) {
        gta6rt_module* m = gta6rt_module_at(i);
        if (!m) break;
        const gta6rt_module_desc* d = gta6rt_module_info(m);
        if (!d || d->kind != GTA6RT_MODULE_GUEST_IMAGE || !d->name) continue;
        const std::size_t n = std::strlen(d->name);
        if (n > 4 && (std::strcmp(d->name + n - 4, ".exe") == 0 || std::strcmp(d->name + n - 4, ".EXE") == 0) &&
            d->size > kSessionRva + 8) {
            base = d->base;
            break;
        }
    }
    return base;
}

}  // namespace

bool read_title_phase(Phase& out) {
    static bool loading_seen = false;
    const std::uint64_t base = exe_base();
    if (!base) return false;
    std::uint64_t game = 0, session = 0, load = 0;
    if (!read_u64(base + kGameStateRva, &game)) return false;
    const bool s = read_u64(base + kSessionRva, &session), l = read_u64(base + kLoadingRva, &load);
    if ((game & 0xffffffff) != 3 || !s) {
        out = Phase::boot;
        return true;
    }
    const std::uint32_t sess = std::uint32_t(session);
    if (l && (load & 0xff) == 2) loading_seen = true;
    switch (sess) {
    case 0: out = l && loading_seen && (load & 0xff) == 0 ? Phase::world : Phase::story_load; break;
    case 3: out = Phase::landing; break;
    case 0xd: out = Phase::first_run; break;
    default: out = Phase::boot; break;
    }
    return true;
}

double process_age_s() {
    static const double start = [] {
        kinfo_proc info{};
        std::size_t size = sizeof info;
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, ::getpid()};
        if (sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && size)
            return double(info.kp_proc.p_starttime.tv_sec) + info.kp_proc.p_starttime.tv_usec * 1e-6;
        timeval now{};
        gettimeofday(&now, nullptr);
        return double(now.tv_sec) + now.tv_usec * 1e-6;
    }();
    timeval now{};
    gettimeofday(&now, nullptr);
    return double(now.tv_sec) + now.tv_usec * 1e-6 - start;
}

}  // namespace gta6rt::pcache
