#pragma once
#include <cstddef>
#include <cstdint>

namespace host::window {

// Windows the title creates are layer-only (no NSWindow on screen) when set. Also asks the presenter for its own hidden
// window (LEONIDA_PRESENT_WINDOW=hidden, unless the environment already sets it) so that no visible fallback appears.
void set_hidden(bool hidden);
int windows_created();

// Full screen of the title's window (the first one with an NSWindow). False when there is none (layer-only) or the
// main queue did not answer. GTA6RT_FULLSCREEN=1 enters it at creation; the window menu has Toggle Full Screen
// (Ctrl+Cmd+F, F11).
bool set_fullscreen(bool on);
bool toggle_fullscreen();
bool fullscreen();

namespace testing {
// Delivers an AppKit-side event as if the window had raised it (unit tests, which run without a window).
void inject_event(uint64_t hwnd, int kind, const int32_t (&v)[6]);
}  // namespace testing

struct Stats {
    size_t posted = 0, dropped = 0, queued = 0;
    uint64_t delivered = 0;
};
Stats stats();

}  // namespace host::window
