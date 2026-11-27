#pragma once

// The window a GDKX title shows its frames in, for PresentX.
//
// On the console PresentX puts a frame on the display: it names no window. A GDKX title still makes its own top-level
// Win32 window (CreateWindowExW with its own class, a message loop on its own thread) before it presents, so on the PC
// that window is where the frames belong. Measured on a retail executable: by its first PresentX the title has one
// visible, unowned top-level window on a thread of its own; the other windows of the process at that point are hidden
// (the IME windows Windows adds, a driver's dummy window).
//
// macOS host: windows live in platform/mac_window.h's registry (whatever serves the title's CreateWindowExW makes
// them there); the title's window is the largest visible one. `process_id` is kept for the call sites: the registry
// only holds this process's windows.

#include <windows.h>

#include "mac_window.h"

namespace rt::d3d12 {

// The visible window of this process with the largest client area, `exclude` aside (the runtime's own fallback
// window); null when the process has none.
inline HWND find_title_window(DWORD process_id, HWND exclude = nullptr) {
    (void)process_id;
    return reinterpret_cast<HWND>(
        gta6rt_mac_window_find_largest(reinterpret_cast<const gta6rt_mac_window*>(exclude)));
}

}  // namespace rt::d3d12
