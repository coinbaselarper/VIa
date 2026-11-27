// lifecycle: console application lifecycle shims for mapped titles.
//
// Console titles never receive a normal desktop WM_CLOSE; quitting suspends the title (running its
// RegisterAppStateChangeNotification callbacks with quiesced = TRUE, giving it ~1 s to flush saves) and then ends the
// process. This shim provides:
//   - RegisterAppStateChangeNotification / UnregisterAppStateChangeNotification on the psm-appnotify API sets;
//   - request_suspend(), called by the host's window code when the window closes;
//   - fit_window_to_area(), the host's rule for the title's oversized first window.
// Not ported: CreateWindowExA/W subclassing (SC_CLOSE interception). The macOS window belongs to the host's windowing
// code, which calls request_suspend() from its close handler.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <gta6rt/isa.h>
#include <gta6rt/os.h>

#include "lifecycle.h"
#include "process_shim.h"
#include "shims.h"
#include "util.h"

namespace rt::lifecycle {
namespace {

struct Registration {
    std::uintptr_t id;
    uint64_t routine;
    uint64_t context;
};

constexpr auto kSuspendDeadline = std::chrono::seconds(1);

std::recursive_mutex& reg_mutex() {
    static auto* m = new std::recursive_mutex;
    return *m;
}

std::vector<Registration>& reg_list() {
    static auto* list = new std::vector<Registration>;
    return *list;
}

std::atomic<std::uintptr_t> g_next_reg_id{1};
std::atomic<bool> g_ending{false};

void run_suspend_callbacks() {
    std::lock_guard lock(reg_mutex());
    std::vector<std::uintptr_t> ids;
    for (const auto& r : reg_list()) ids.push_back(r.id);
    for (const auto id : ids) {
        uint64_t routine = 0, context = 0;
        for (const auto& r : reg_list()) {
            if (r.id == id) {
                routine = r.routine;
                context = r.context;
                break;
            }
        }
        if (routine) gta6rt_isa_call_guest4(routine, 1, context, 0, 0);
    }
}

uint32_t WINAPI register_app_state_change(uint64_t routine, uint64_t context, uint64_t* registration) {
    if (!routine || !registration) return 87;
    const std::uintptr_t id = g_next_reg_id.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(reg_mutex());
        reg_list().push_back({id, routine, context});
    }
    *registration = id;
    return 0;
}

void WINAPI unregister_app_state_change(uint64_t registration) {
    std::lock_guard lock(reg_mutex());
    auto& list = reg_list();
    list.erase(std::remove_if(list.begin(), list.end(), [registration](const Registration& r) { return r.id == registration; }),
               list.end());
}

}  // namespace

std::pair<int, int> fit_window_to_area(const Rect& area, int width, int height) {
    const int area_w = area.right - area.left;
    const int area_h = area.bottom - area.top;
    if (width <= 0 || height <= 0 || area_w <= 0 || area_h <= 0 || (width <= area_w && height <= area_h)) return {width, height};
    const double scale = std::min(static_cast<double>(area_w) / static_cast<double>(width), static_cast<double>(area_h) / static_cast<double>(height));
    return {static_cast<int>(static_cast<double>(width) * scale), static_cast<int>(static_cast<double>(height) * scale)};
}

void request_suspend() {
    if (g_ending.exchange(true)) return;
    RTLOG(INFO, "lifecycle: window closed; running %zu suspend routine(s)", registered_notification_count());
    std::thread([] {
        run_suspend_callbacks();
        RTLOG(INFO, "lifecycle: title suspended; exiting");
        process::abort_run(0);
    }).detach();
    std::thread([] {
        std::this_thread::sleep_for(kSuspendDeadline);
        RTLOG(WARN, "lifecycle: suspend deadline elapsed; exiting");
        process::abort_run(0);
    }).detach();
}

std::size_t registered_notification_count() {
    std::lock_guard lock(reg_mutex());
    return reg_list().size();
}

void invoke_suspend_callbacks_for_test() { run_suspend_callbacks(); }

}  // namespace rt::lifecycle

namespace {
using namespace rt::lifecycle;
#define APP_NOTIFY_API(dll)                                                                          \
    SHIM(dll, "RegisterAppStateChangeNotification", register_app_state_change),                      \
    SHIM(dll, "UnregisterAppStateChangeNotification", unregister_app_state_change)
}  // namespace

SHIM_TABLE(lifecycle,
    APP_NOTIFY_API("api-ms-win-core-psm-appnotify-l1-1-0.dll"),
    APP_NOTIFY_API("api-ms-win-core-psm-appnotify-l1-1-1.dll"),
    APP_NOTIFY_API("kernel32.dll"),
);
