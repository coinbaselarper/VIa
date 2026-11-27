// Test-only stand-ins for the os and shims_core functions window_shim.cpp calls, so the window layer is tested
// without the rest of the host. The "guest" window procedures are native functions.
#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>

#include <gta6rt/os.h>
#include <gta6rt/shims.h>

#include "shims.h"
#include "stubs.h"

namespace stubs {
int suspend_requests = 0;
uint32_t last_error = 0;
}  // namespace stubs

extern "C" {
void gta6rt_log(gta6rt_log_level level, const char* module, const char* fmt, ...) {
    if (!stubs::verbose && level > GTA6RT_LOG_WARN) return;
    std::fprintf(stderr, "[%s] ", module);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}
void gta6rt_last_error_set(uint32_t code) { stubs::last_error = code; }
uint64_t gta6rt_time_monotonic_ns(void) { return stubs::now_ns; }
uint64_t gta6rt_os_invoke(gta6rt_gva fn, const uint64_t* args, uint32_t count) {
    using Fn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);
    return count == 4 ? reinterpret_cast<Fn>(fn)(args[0], args[1], args[2], args[3]) : 0;
}
void gta6rt_shims_request_suspend(void) { ++stubs::suspend_requests; }
}

namespace stubs {
bool verbose = false;
uint64_t now_ns = 1'000'000'000;
}

namespace rt::shims {
namespace {
std::map<std::string, void*>& registry() {
    static std::map<std::string, void*> r;
    return r;
}
}  // namespace
Table::Table(const Entry* entries, size_t count) {
    for (size_t i = 0; i < count; ++i) registry()[std::string(entries[i].dll) + "!" + entries[i].name] = entries[i].fn;
}
void* find(const std::string& dll, const std::string& name) {
    auto it = registry().find(dll + "!" + name);
    return it == registry().end() ? nullptr : it->second;
}
}  // namespace rt::shims

extern "C" {
void gta6rt_gameinput_mouse_motion(double, double) {}
void gta6rt_gameinput_mouse_buttons(int, int) {}
void gta6rt_gameinput_mouse_release(void) {}
int gta6rt_gameinput_mouse_active(void) { return 0; }
}
