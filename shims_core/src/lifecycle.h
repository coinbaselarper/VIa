// App lifecycle notifications (RegisterAppStateChangeNotification / UnregisterAppStateChangeNotification) and the
// suspend-on-close hook the host's window calls.
#pragma once

#include <cstddef>
#include <utility>

namespace rt::lifecycle {

struct Rect {
    int left, top, right, bottom;
};

// Scales down (width, height) proportionally to fit inside `area`, leaving fitting sizes and non-positive values
// unchanged. The host's window code applies it to the title's oversized (3840x2160) first window.
std::pair<int, int> fit_window_to_area(const Rect& area, int width, int height);

// The window was closed: runs the registered suspend routines with quiesced = TRUE (about one second to flush saves),
// then ends the run. Returns at once; a second call is ignored.
void request_suspend();

// For tests: number of registered callbacks, and a synchronous suspend invocation that does not end the run.
std::size_t registered_notification_count();
void invoke_suspend_callbacks_for_test();

}  // namespace rt::lifecycle
