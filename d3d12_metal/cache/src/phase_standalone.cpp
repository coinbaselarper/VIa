#include "phase.h"

#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

namespace gta6rt::pcache {

bool read_title_phase(Phase&) { return false; }

double process_age_s() {
    static const double start = [] {
        kinfo_proc info{};
        std::size_t size = sizeof info;
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, ::getpid()};
        if (sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && size)
            return double(info.kp_proc.p_starttime.tv_sec) + info.kp_proc.p_starttime.tv_usec * 1e-6;
        return 0.0;
    }();
    timeval now{};
    gettimeofday(&now, nullptr);
    return double(now.tv_sec) + now.tv_usec * 1e-6 - start;
}

}  // namespace gta6rt::pcache
