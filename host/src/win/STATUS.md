# Window layer status (host/src/window_shim.cpp + host/src/win/, 2026-11-26)

User32 surface of the title and its AppKit side. Owned here: `host/src/window_shim.{cpp,h}`, `host/src/win/*`. Not touched: `host/CMakeLists.txt`,
`host/src/main.cpp`, `d3d12_vulkan/` (read only: `platform/mac_window.h`, `platform/metal_window.mm`).

## What the title does with its window (read from the exe, `exe/game_scarlett_master.exe`)

- Only seven window-related imports: `RegisterClassExA`, `CreateWindowExA`, `DefWindowProcA`, `ShowWindow`, `PeekMessageA`,
  `TranslateMessage`, `DispatchMessageA` (no `GetClientRect`, `SetWindowPos`, `GetFocus`, `ClipCursor`, `ShowCursor`, ...).
- `RegisterClassExA("sgaWindowClass")` registers wndproc `0x14247a660`. That address is the exe's import thunk
  `jmp [DefWindowProcA]` (IAT 0x146793c50, resolved from the import table). **The title's window procedure is DefWindowProcA itself;
  it switches on no message.**
- Message loop (exe+0x24847a8..0x24847e8): `PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)`; while non-zero `TranslateMessage`,
  `DispatchMessageA`. It never reads `msg.message`, never tests for WM_QUIT.
- `CreateWindowExA("Americas", style 0xcf0000 = WS_OVERLAPPEDWINDOW, 3840x2160)` then `ShowWindow(hwnd, 1)`. (The size follows the console
  profile; with Series S it asked for 1920x1080 in one run.)
- Consequence: every message is answered by `DefWindowProcA`, so what matters is (a) DefWindowProcA's answers and (b) the close request,
  which has to end the title through the console's suspend rather than `DestroyWindow`. Input never goes through the window.

## What is implemented

| area | behaviour |
|---|---|
| creation | `WM_GETMINMAXINFO` (MINMAXINFO filled: screen size, min track 320x180), `WM_NCCREATE`, `WM_NCCALCSIZE`, `WM_CREATE` sent synchronously; `CREATESTRUCTA` carries the title's size; FALSE from NCCREATE or -1 from CREATE destroys the window |
| size on the Mac | `host_win_plan_size`: the title's 3840x2160 client area keeps its size when it fits the main screen's visible frame, else the same aspect shrunk (never up). 1800x1056 pt Retina here: 3600x2026 px (AppKit rounding), 16:9. The title renders at its own size; the layer's drawable (the view's pixels) is what the presenter scales to. Aspect locked with `contentAspectRatio`, min content 320x180 |
| backing scale | the plan works in pixels with the screen's `backingScaleFactor`; `NSWindowDidChangeBackingProperties` -> `WM_DPICHANGED` (dpi = 96 x scale, suggested rect behind lParam); the view's own drawable-size update is d3d12_vulkan's `Gta6rtMetalView` |
| activation/focus | `NSWindowDidBecomeKey/ResignKey` -> `WM_NCACTIVATE`, `WM_ACTIVATE` (WA_ACTIVE/INACTIVE), `WM_SETFOCUS/KILLFOCUS`; app active/resign -> `WM_ACTIVATEAPP` |
| size/move | `NSWindowDidResize` -> `WM_SIZE` (restored / minimised / maximised-or-full-screen, client pixels in lParam); `DidMove` -> `WM_MOVE` (client origin, screen pixels, top-left); consecutive unread WM_SIZE/WM_MOVE/WM_DISPLAYCHANGE of a window coalesce into the newest |
| display change | `NSWindowDidChangeScreen`, `NSApplicationDidChangeScreenParameters` -> `WM_DISPLAYCHANGE` (32 bpp, screen pixels) |
| WM_PAINT / erase | never generated (the layer is presented by Vulkan). `DefWindowProcA`: WM_ERASEBKGND = 1 (handled, nothing to erase), WM_PAINT/WM_NCPAINT = 0. The NSWindow background is black |
| close | the red button (`gta6rt_mac_window_close_requested`, polled in PeekMessageA) and the menu's Quit (Cmd+Q) queue `WM_SYSCOMMAND(SC_CLOSE)`; DefWindowProcA sends `WM_CLOSE` to the window's procedure; `DefWindowProcA(WM_CLOSE)` calls `gta6rt_shims_request_suspend()` (the title's suspend callbacks, then exit) instead of DestroyWindow. A custom wndproc that swallows WM_CLOSE therefore prevents the exit, as on Windows |
| DefWindowProcA | NCCREATE TRUE; CREATE/NCCALCSIZE/ACTIVATE/ACTIVATEAPP/FOCUS/SIZE/MOVE/SHOWWINDOW/GETMINMAXINFO/DISPLAYCHANGE/DPICHANGED/DESTROY/NCDESTROY 0; NCHITTEST HTCLIENT; MOUSEACTIVATE MA_ACTIVATE; NCACTIVATE TRUE; QUERYOPEN/QUERYENDSESSION TRUE; ENDSESSION 0; DEVICECHANGE TRUE; GETTEXT/GETTEXTLENGTH/SETTEXT on the NSWindow title; SYSCOMMAND SC_CLOSE/MINIMIZE/MAXIMIZE/RESTORE act; **any other message returns 0 and is logged once** (`DefWindowProcA: message 0x... has no specific handling`) |
| PeekMessageA | window and message-range filters, PM_REMOVE/PM_NOREMOVE, bad HWND -> ERROR_INVALID_WINDOW_HANDLE (1400), NULL MSG -> 87; FIFO, 512 messages max (oldest dropped and counted) |
| ShowWindow | SW_HIDE (orderOut), SHOW/SHOWNORMAL/SHOWDEFAULT/SHOWNA/SHOWNOACTIVATE (order front, deminiaturize), MINIMIZE/SHOWMINIMIZED/SHOWMINNOACTIVE (miniaturize), MAXIMIZE (zoom), RESTORE; returns the previous visibility (FALSE the first time); queues `WM_SHOWWINDOW`; unknown command -> logged, FALSE |
| full screen | `host::window::set_fullscreen/toggle_fullscreen/fullscreen`; the menu has Toggle Full Screen (Ctrl+Cmd+F) and Toggle Full Screen (F11); `GTA6RT_FULLSCREEN=1` enters it at creation. Native macOS full screen ignores the aspect lock, so the view is fitted to the title's aspect inside the window and the rest stays black (measured: 3600x2028 client on a 3600x2338 screen). Window collection behaviour is FullScreenPrimary |
| keys in the window | a local key monitor first offers key-downs to the menu (Cmd+Q, Ctrl+Cmd+F, F11, Cmd+M), then swallows key events of the title's windows (no system beep; the game reads the keyboard through GameInput). Cmd-modified keys that are not menu items pass to the system |
| hidden mode | `--hidden-window`: layer-only window as before, plus `set_hidden(true)` sets `LEONIDA_PRESENT_WINDOW=hidden` (unless set), so PresentX uses its own *hidden* window instead of showing a visible fallback. Measured: `PresentX ready (window ...: the runtime's window, hidden, client area 1920x1080)`, no window on screen. ShowWindow on a layer-only window synthesises SHOWWINDOW, ACTIVATEAPP, NCACTIVATE, ACTIVATE, SETFOCUS, SIZE, MOVE |
| main thread | AppKit work runs on the main thread (main.cpp's loop already calls `gta6rt_mac_window_pump_events` / `CFRunLoopRunInMode`); calls that need an answer wait up to 5 s |

## Stubs and gaps, by name

- **Not exported (the title does not import them, so nothing can call them):** `PostMessage*`, `PostQuitMessage`, `SendMessage*`, `GetMessage*`, `DestroyWindow`,
  `GetClientRect`, `SetWindowPos`, `AdjustWindowRect`, `GetFocus`, `GetForegroundWindow`, cursor and clip APIs, `MonitorFromWindow`. A different executable would hit the
  host's unimplemented-import path.
- **Not generated:** `WM_PAINT`, `WM_ERASEBKGND`, `WM_NCPAINT`, `WM_WINDOWPOSCHANGING/CHANGED`, `WM_SIZING/MOVING`, `WM_ENTERSIZEMOVE`, mouse and keyboard messages, `WM_DEVICECHANGE`, `WM_QUIT`.
  `TranslateMessage` translates nothing and returns FALSE.
- **DefWindowProcA:** any message outside the table above returns 0 (logged once).
- **`ShowWindow(SW_MAXIMIZE)`** is `zoom:` (not full screen); it is only undone by a later SW_RESTORE/SHOWNORMAL that this layer's own zoom made.
- **Hidden mode still creates a second window object:** PresentX builds its own hidden layer-only window because the title's layer-only window is not in the visible registry
  that `find_title_window` reads (d3d12_vulkan, not edited). Nothing is on screen. A single-window hidden mode needs `gta6rt_mac_window_find_largest` to accept layer-only windows.
- **Presenter resize:** the layer's drawable changes on resize, full screen and backing-scale change. d3d12_vulkan lists "window resize and swapchain recreation" as untested. With the title
  nothing is drawn yet, so it could not be judged; the real title survived 20 s of `GTA6RT_FULLSCREEN=1` and created its swapchain at the full-screen size (3600x2028), but a resize after PresentX is up was not exercised.
- **Focus from a terminal:** a host started from a terminal is not granted activation (macOS 14+); the window opens behind/unfocused and gets key (and `WM_ACTIVATE`, and `GCKeyboard` events) when clicked; full screen activates it
  (measured: focus and app-active events arrive right after entering full screen).
- Close was exercised only by the unit/GUI tests (Cmd+Q through `[NSApp sendEvent:]` and `request_close` stubbed); **no run of the real title was closed**, so the path DefWindowProcA(WM_CLOSE) -> `gta6rt_shims_request_suspend()` -> the title's suspend is unverified end to end.

## Tests (own build tree `build/win-<arch>`, no host needed)

```sh
cmake -S port/host/src/win/tests -B port/build/win-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C port/build/win-x86_64 -j3
port/build/win-x86_64/win_core_test     # queue, DefWindowProcA table, size fit
port/build/win-x86_64/win_shim_test     # the user32 shim on a layer-only window, native "title" wndproc forwarding to DefWindowProcA
port/build/win-x86_64/win_gui_test      # opens one real window for a few seconds (needs a display session; exits 0 when there is none)
```

Measured (x86_64 under Rosetta and arm64 builds; both pass): `win_core_test` 0 failures; `win_shim_test` 0 failures (creation order, queue and coalescing, focus/size/move/display/DPI
messages, DefWindowProcA answers, SC_CLOSE -> WM_CLOSE -> one suspend request, hidden-mode environment); `win_gui_test` 0 failures on the 1800x1056 pt Retina display: fitted 3600x2026 client,
16:9, WM_SIZE for full screen (maximised), restore, minimise, restore, Ctrl+Cmd+F and F11 through `[NSApp sendEvent:]`, Cmd+Q -> `WM_SYSCOMMAND(SC_CLOSE)` without terminating.
`PeekMessageA` on an empty queue: 30 ns per call (1,000,000 calls).

Run against the real title (host built privately in `build/win-host-x86_64` with `host_overlay.cmake`; MoltenVK x86_64; `LEONIDA_XMEM_32BIT=anywhere LEONIDA_CONSOLE_PROFILE=series-x`; logs in `port/run/logs/win-input-*.log`):
- hidden: 40 s watchdog, `ShowWindow` then the title's loop dispatches the synthesised messages through `DefWindowProcA` with no "no specific handling" warning, `PresentX` on the runtime's hidden window.
- visible: `CreateWindowExA 3840x2160 -> client 3600x2026`; the window stays 3600x2026 up to PresentX (an earlier build shrank it to 3528x1984: `isZoomed` is true for a window that fills the screen, and ShowWindow undid a "zoom" that was never made; fixed).
- full screen (`GTA6RT_FULLSCREEN=1`): entered, `WM_SIZE(2, 3600, 2028)`, display/focus/app-active events, PresentX swapchain 3600x2028, ran to the watchdog.

## Integrator notes

1. **`host/CMakeLists.txt`**: after `add_executable(gta6rt ...)` add `include(src/win/win.cmake)` (adds `win/win_core.cpp`, `win/appkit_window.mm` compiled as Objective-C++ through the C++ rule, and `-framework AppKit QuartzCore Foundation`). `window_shim.cpp` is already in the list. The directory needs no `enable_language(OBJCXX)`. Without this, the host does not link. To try it without editing: `cmake ... -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=$PWD/port/host/src/win/host_overlay.cmake` (used for `build/win-host-x86_64`).
2. **`host/src/main.cpp`**: no change is required. The loop already pumps AppKit when a visible window exists. Optional: expose `host::window::toggle_fullscreen()` on a CLI flag; `GTA6RT_FULLSCREEN=1` already enters full screen at creation.
3. The window logs one line per AppKit event (`[I window] event size (...)`); a long interactive resize produces many lines.
4. `--hidden-window` semantics changed slightly: it also exports `LEONIDA_PRESENT_WINDOW=hidden` (if unset). `verify.sh s1` (`--stop-at-winmain --hidden-window`) never creates the window and is unaffected.
5. Everything else the title needs from the window is described above; nothing in `shims_core`'s lifecycle was changed: `gta6rt_shims_request_suspend()` is still the close path.
6. **Other workers' state at the time of the runs:** the current tree needs `LEONIDA_XMEM_32BIT=anywhere` (xmem refuses a 32-bit reservation without `-pagezero_size 0x10000`; otherwise the title hits its `int3` at exe+0x19f4f58) and defaults to the Series S profile (window request 1920x1080, `LEONIDA_CONSOLE_PROFILE=series-x` restores 3840x2160).

## Integrated (round 3 integrator, 2026-11-26)

`host/CMakeLists.txt` now ends with `include(src/win/win.cmake)`; `host_overlay.cmake` adds nothing when the target
already has the sources. The integrated host (`build/macos/gta6rt`, x86_64; `build/macos-arm64/gta6rt`) links with it.
Visible-window title runs of the integrated host: `port/run/logs/s2-firstrun.log` and `s2r3-*.log` (port/STATUS.md).

## Input round (2026-11-26): cursor capture, click to key, mouse events

`appkit_window.mm` (own build trees `build/input-x86_64`, `build/win-input-x86_64`; tests CMake now compiles the hud sources too, the integrated window layer calls the hud).

- **Mouse into the pad.** A local mouse monitor forwards the key host window's mouse-moved/dragged deltas (`NSEvent.deltaX`, `-deltaY` so up is +) and left/right button state to
  `gta6rt_gameinput_mouse_*` (shims_gdk, "Input round"). `acceptsMouseMovedEvents` is set on the title window. Ignored while another window or app has key, and while Option is held.
- **Capture.** When a host window has been key for 0.5 s, the app is active, and the mouse pad is active (`gta6rt_gameinput_mouse_active()`: keyboard pad connected, `GTA6RT_MOUSE` not off):
  `CGAssociateMouseAndMouseCursorPosition(false)` + `[NSCursor hide]`; the cursor is first moved to the middle of the view only when it is not already well inside it (so a click on the title
  bar or close button cannot land after capture). Released on: window loses key (focus lost, Cmd+Tab, minimise), the pad turns off (a controller plugged in), detach, process exit (`atexit`;
  macOS also restores association when a process dies), and while **Option** is held (a free cursor without leaving the window). A 0.5 s main-thread timer reconciles; it is invalidated when the last
  window detaches. **Escape is never involved**: it stays the pad's Menu button; nothing in the cursor logic reads it. `GTA6RT_MOUSE=off` disables forwarding and capture. Log lines:
  `[rt] host_win: mouse captured (...)` / `mouse released`; in `run/logs/input-r4b.log` the window got key at launch, captured, and released when focus moved on (lines 289, 291).
  Not verified: that mouse movement then drives the stick in the title, a click-and-drag of the title bar while captured (Option releases), full screen capture (same code path), Spaces.
- **Focus from a terminal.** The window is a regular-policy app (d3d12_vulkan sets it) and `activateIgnoringOtherApps` does not take focus from the terminal on this macOS; the window opens unfocused.
  A mouse-down in it now explicitly activates the app (`[NSApp activate]`) and `makeKeyAndOrderFront`s the window, so one click gives key (and `GCKeyboard` events, `WM_ACTIVATE`).
  Not verified: no click could be delivered here (Accessibility is off); the earlier measurement ("gets key when clicked") stands, this adds the explicit activation for the case where the first click was swallowed.
- Tests: `win_core_test` 0 failures, `win_shim_test` 0 failures (x86_64); `win_gui_test` not re-run (it opens a real window and enters full screen).
