#import <AppKit/AppKit.h>

#include <mach/mach.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static mach_port_t g_main_thread;

static uint64_t now_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static double main_cpu_s() {
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(g_main_thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) != KERN_SUCCESS) return 0;
    return double(info.user_time.seconds + info.system_time.seconds) +
           double(info.user_time.microseconds + info.system_time.microseconds) * 1e-6;
}

static double ps_cpu_s(const char* name) {
    std::string cmd = std::string("ps -A -o cputime=,comm= | grep -E ' /?.*") + name + "$' | head -1";
    FILE* p = popen(cmd.c_str(), "r");
    char line[256] = {};
    if (!p || !std::fgets(line, sizeof line, p)) {
        if (p) pclose(p);
        return -1;
    }
    pclose(p);
    int h = 0, m = 0;
    double s = 0;
    if (std::sscanf(line, " %d:%d:%lf", &h, &m, &s) == 3) return h * 3600.0 + m * 60.0 + s;
    if (std::sscanf(line, " %d:%lf", &m, &s) == 2) return m * 60.0 + s;
    return -1;
}

static void pump(double seconds) {
    @autoreleasepool {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
        for (;;) {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!e) break;
            [NSApp sendEvent:e];
            until = [NSDate distantPast];
        }
        [NSApp updateWindows];
    }
}

static void spin(double seconds) {
    const uint64_t end = now_ns() + uint64_t(seconds * 1e9);
    while (now_ns() < end) pump(0.05);
}

struct Result {
    double main_ms_per_s, ws_ms_per_s, mean_us, p50_us, p99_us, max_us;
    size_t n;
};

static Result run(NSWindow* w, double hz, double seconds, bool vary) {
    std::vector<double> durations;
    __block uint64_t counter = 0;
    __block std::vector<double>* d = &durations;
    NSTimer* timer = nil;
    if (hz > 0) {
        timer = [NSTimer timerWithTimeInterval:1.0 / hz repeats:YES block:^(NSTimer*) {
            ++counter;
            char buf[160];
            std::snprintf(buf, sizeof buf, "Americas - %.0f fps %.1fms (max %.0f) | draws %u | gpu %.1fms | pso %u/%u pending | mem %.1f/24G",
                          30.0 + double(counter % 31), 16.0 + double(counter % 97) * 0.1, 20.0 + double(counter % 40),
                          unsigned(10 + counter % 50), 3.0 + double(counter % 20) * 0.1, unsigned(counter % 7), unsigned(counter % 3),
                          4.0 + double(counter % 100) * 0.02);
            NSString* s = [NSString stringWithUTF8String:buf];
            const uint64_t a = now_ns();
            w.title = s;
            d->push_back(double(now_ns() - a) / 1000.0);
        }];
        timer.tolerance = 0.002;
        [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    }
    spin(1.0);
    durations.clear();
    const double m0 = main_cpu_s(), w0 = ps_cpu_s("WindowServer");
    const uint64_t t0 = now_ns();
    spin(seconds);
    const double dt = double(now_ns() - t0) * 1e-9;
    const double dm = main_cpu_s() - m0, dw = ps_cpu_s("WindowServer") - w0;
    [timer invalidate];
    Result r{};
    r.main_ms_per_s = dm / dt * 1000.0;
    r.ws_ms_per_s = dw < 0 ? -1 : dw / dt * 1000.0;
    r.n = durations.size();
    if (!durations.empty()) {
        std::vector<double> sorted = durations;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0;
        for (double x : sorted) sum += x;
        r.mean_us = sum / double(sorted.size());
        r.p50_us = sorted[sorted.size() / 2];
        r.p99_us = sorted[std::min(sorted.size() - 1, size_t(double(sorted.size()) * 0.99))];
        r.max_us = sorted.back();
    }
    (void)vary;
    return r;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        double seconds = 20;
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        g_main_thread = mach_thread_self();
        if (CGMainDisplayID() == 0 || !CGDisplayIsActive(CGMainDisplayID())) {
            std::printf("no active display session\n");
            return 0;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
        [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical reason:@"bench"];
        NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 300, 720, 440)
                                                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        w.releasedWhenClosed = NO;
        w.title = @"Americas";
        w.backgroundColor = NSColor.blackColor;
        [w orderFrontRegardless];
        spin(1.5);
        std::printf("window occlusion visible %d, %.0f s per condition\n", (w.occlusionState & NSWindowOcclusionStateVisible) != 0, seconds);
        struct Cond {
            const char* name;
            double hz;
        };
        const Cond conds[] = {{"control (no updates)", 0}, {"title 4 Hz", 4}, {"title 10 Hz", 10}, {"control (no updates) again", 0}};
        std::printf("%-28s %6s %12s %12s %10s %10s %10s %10s\n", "condition", "n", "main ms/s", "WServer ms/s", "set mean", "set p50", "set p99", "set max");
        for (const Cond& c : conds) {
            Result r = run(w, c.hz, seconds, true);
            std::printf("%-28s %6zu %12.2f %12.1f %8.1fus %8.1fus %8.1fus %8.1fus\n", c.name, r.n, r.main_ms_per_s, r.ws_ms_per_s, r.mean_us,
                        r.p50_us, r.p99_us, r.max_us);
            std::fflush(stdout);
        }
        [w close];
    }
    return 0;
}
