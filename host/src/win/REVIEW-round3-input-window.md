# Round 3 review: window messages and GameInput (2026-11-26)

Reviewer: independent. Scope: `host/src/window_shim.{cpp,h}`, `host/src/win/*`, and the GameInput sources in `shims_gdk/src`
(`gameinput_mac.cpp`, `gameinput_gc.mm`, `gameinput_pad.h`, `gameinput_backend.h`) with their tests. I wrote only this file,
my build trees `port/build/review-input-window-{x86_64,arm64}`, run logs `port/run/logs/review-r3iw-*.log`, and probes in my
scratch area. I edited no sources.

## Verdict: APPROVED WITH FIXES

The window layer and the GameInput hub do what the report says on every path I could run. The exe findings, the tests and
benchmarks, the scripted-input passthrough, hidden mode and the visible 3600x2026 window all reproduce. The close path, which the
builder never ran, ran end to end once in my visible run. There are two blocking items, both false claims with small fixes:

- **B1.** Some unserved slots are silent, although the STATUS says they are logged.
- **B2.** The "nothing else of IGameInput" call census cannot see 5 slots.

There is also one real defect off the boot path:

- **F1.** Rumble levels are read through a dangling reference.

## What I built

| tree | command | result |
|---|---|---|
| `build/review-input-window-x86_64` | all modules + `-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=host/src/win/host_overlay.cmake`, `-j3` | 230/230, rc 0; `gta6rt` Mach-O x86_64. 48 `warning:` lines, none in `window_shim`, `win/` or `gameinput*` |
| `build/review-input-window-arm64` | first `GTA6RT_MODULES=os;isa;seh;loader;shims_core;shims_gdk` (190/190, rc 0), then reconfigured `all` + overlay, target `gta6rt` | rc 0; `gta6rt` Mach-O arm64. **The arm64 host with `win.cmake` links**, which the builder did not try |
| `.../win-tests` (both arches) | `cmake -S host/src/win/tests` | 14/14, rc 0 on both |

`nm -u window_shim.cpp.o` lists `host_win_attach/detach/plan_size/screen_info/show/set_fullscreen/is_fullscreen`. This confirms
that the shared host does not link without `include(src/win/win.cmake)`. I also checked with a CMake probe that the relative
`include(src/win/win.cmake)` resolves from `host/CMakeLists.txt`.

## Tests re-run

| test | x86_64 (Rosetta, one at a time) | arm64 |
|---|---|---|
| `shims_gdk/tests/run_all.sh` | **42/42 PASS** (`review-gdk-run_all.log`) | not run in full |
| `gameinput_mac_test` (identity-only / `--input-script` / `--synthetic`) | 9 / 50 / 57 ok, 0 failures | 9 / 50 / 57 ok, 0 failures |
| `gameinput_pad_test` | 21 ok | 21 ok |
| `win_core_test` / `win_shim_test` | 32 / 45 ok, 0 failures | 32 / 45 ok, 0 failures |
| `win_gui_test` (opens a real window) | 23 ok: client 3600x2026, full screen 3600x2028, F11 toggles, 0 failures | not run |

These are the builder's numbers exactly. Benchmarks (printed by the tests, not asserted):

- **`GetCurrentReading+GetGamepadState+Release`:** 111 ns on x86_64 (builder: 107). On arm64 it was 101 ns at load average
  11, then 57/57/55 ns on re-runs (builder: 51).
- **`PeekMessageA` on an empty queue:** 30 ns on x86_64 (builder: 30) and 21 ns on arm64 (builder: 19).

The numbers are measured as claimed.

## Exe claims (probe `pe_probe.py`, read-only on `exe/game_scarlett_master.exe`)

- **Window procedure.** The wndproc 0x14247a660 is `48 FF 25 disp32`, a `jmp [0x146793c50]`. The IAT slot at 0x146793c50 is
  `ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll!DefWindowProcA`. Confirmed.
- **Window imports.** There are exactly 7, all on `ext-ms-win-rtcore-ntuser-*`, none on `user32.dll`: CreateWindowExA,
  DefWindowProcA, RegisterClassExA, ShowWindow, DispatchMessageA, PeekMessageA, TranslateMessage.
- **Message loop.** At exe+0x24847a8 the loop loads PeekMessageA, TranslateMessage and DispatchMessageA into r14/r15/r12. It
  calls `PeekMessageA(rbx, 0, 0, 0, PM_REMOVE=1)`, then on a non-zero result `TranslateMessage(rbx)` and `DispatchMessageA(rbx)`,
  and jumps back. It never reads `msg.message`. Confirmed.

## Real-title runs (x86_64, `host/tools/run.sh`, one host at a time, `GTA6RT_BIN=build/review-input-window-x86_64/gta6rt`)

All three runs used MoltenVK 1.4.2 x86_64, `LEONIDA_XMEM_32BIT=anywhere` and `LEONIDA_CONSOLE_PROFILE=series-x`.

| run | args | result |
|---|---|---|
| `review-r3iw-hidden` | `LEONIDA_INPUT_SCRIPT="a@10,start@20,a@30"`, `--trace=0 --watchdog 60 --hidden-window` | rc 124 at the watchdog. Log: `press a@10.0`, `press start@20.0`, `press a@30.0`, `first call of SetRumbleState` after the first press. Window: `layer only, client 3840x2160`; PresentX: `the runtime's window, hidden, client area 1920x1080`. 0 "no specific handling" lines. 16.7 M calls, 297 distinct, 0 unimplemented. **My CGWindowList probe at t=35 s found 0 windows for the pid** |
| `review-r3iw-visible` | no script (GameController backend), `--trace=0 --trace-match DefWindowProc --watchdog 35` | backend started, keyboard pad connected, `CreateWindowExA ... client 3600x2026`, PresentX on the title's window at 3600x2026. The traced `DefWindowProcA` messages were 0x24, 0x81, 0x83, 0x01 at creation; 0x18, 0x86, 0x06, 0x07, 0x1c after ShowWindow; and focus/activate pairs. 0 "no specific handling" lines. **At 22 s the menu's Quit fired.** The log reads: `event close`, `DefWindowProcA(0x112, 0xf060)`, `DefWindowProcA(0x10)`, `WM_CLOSE reached DefWindowProcA`, `lifecycle: running 2 suspend routine(s)`, `lifecycle: suspend deadline elapsed; exiting`, `title exited with code 0 after 22.16 s`. I did not send that Quit: the focus kept toggling during the run, so someone or something at the console pressed Cmd+Q or picked Quit from the menu |
| `review-r3iw-visible2` | `LEONIDA_INPUT_SCRIPT="a@8,start@14"`, `--trace=0 --watchdog 30` | rc 124. Presses logged at 8.0 s and 14.0 s. At t≈10 s and t≈18 s the probe found **one on-screen window, 1800x1045 pt** (3600 px wide; the 1013 pt content is 2026 px plus the title bar) and four off-screen 1800x39 pt AppKit windows. No second window on screen |

So the close path `SC_CLOSE -> WM_CLOSE -> gta6rt_shims_request_suspend()` works and the process exits with code 0. The
title's 2 suspend routines did not return within shims_core's 1 s deadline (`title suspended` was never printed). That is a
shims_core/title matter and is recorded here only.

## Blocking

**B1. "Not served ... E_NOTIMPL or E_INVALIDARG and logged once" (shims_gdk/STATUS.md) is false for several slots, and the
hard rule "every unimplemented path must fail loudly" is not met there.**

These paths return a failure with no log line at all:

- `GetTemporalReading` (E_NOTIMPL, gameinput_mac.cpp:888);
- `CreateForceFeedbackEffect` (E_INVALIDARG, :348);
- `SetHapticMotorState` (E_INVALIDARG, :354);
- `CreateRawDeviceReport`, `GetRawDeviceFeature`, `SetRawDeviceFeature`, `SendRawDeviceOutput`,
  `SendRawDeviceOutputWithResponse` and `ExecuteRawDeviceIoControl` (E_NOTIMPL, :359-367).

These are silent no-ops or silent refusals:

- `IsForceFeedbackMotorPoweredOn`, `SetForceFeedbackMotorGain`, `SetInputSynchronizationState`,
  `SendInputSynchronizationHint`, `PowerOff`, `AcquireExclusiveRawDeviceAccess` (false) and `ReleaseExclusiveRawDeviceAccess`.

`EnableOemDeviceSupport` returns S_OK. The title calls none of these, so nothing breaks today.

Fix: put `not_served<N>(name)` or a named log line on each, or correct the STATUS text.

**B2. "The title calls RegisterDeviceCallback, GetCurrentReading and SetRumbleState, and nothing else of IGameInput" is
stronger than the instrumentation can show.** `first_call()` is missing from 5 IGameInput slots: `GetTemporalReading`,
`StopCallback`, `UnregisterCallback`, `FindDeviceFromPlatformHandle` and `FindDeviceFromPlatformString`. A title call to any of
them would leave no trace. No `IGameInputDevice` slot except `SetRumbleState`, and no `IGameInputReading` slot, is logged
either.

Fix: add `first_call` to those 5 slots (cheap; it is already on the others), or restate the claim as "of the 14 logged
methods".

## Should fix (not blocking)

**F1. Rumble reads a dangling reference (gameinput_gc.mm:193-212).** `set_rumble(uint64_t, const mac::RumbleLevels& levels)`
captures `levels` in a `dispatch_async` block. Clang blocks capture a C++ reference as a reference, not as a copy, so the block
reads the caller's stack slot (`Device::SetRumbleState`'s local) after that frame has returned.

- **Probe** (`probes/blockref.mm`, arm64 -O2): the block reads the caller's address and prints `-6.4e24 0.0` instead of
  `0.25 0.75`.
- **Effect:** with a real haptic controller, every rumble level is stack garbage. It is read, not written, so the title is not
  corrupted, but rumble is wrong and can be NaN or huge.
- **When it happens:** on the first-run pages, as soon as a pad with haptics is attached (the title calls SetRumbleState right
  after the first press).
- **Why the tests miss it:** the synthetic tests use the rumble observer, which never reaches this code.
- **Fix:** `const mac::RumbleLevels copy = levels;` and capture `copy`.

**F2. CoreHaptics state races (gameinput_gc.mm:39-47).** The engine's `stoppedHandler`/`resetHandler` write `_player` and
`_engineRunning` on CoreHaptics' queue. `setLevel:` reads and writes the same ivars on `rumble_queue_`. A concurrent ARC store
to `_player` can over-release. Run the handlers' bodies with `dispatch_async(rumble_queue_, ...)`. This path is unexercised
(no haptic hardware).

**F3. Duplicate pad at startup (gameinput_gc.mm:276-324).** `start()` runs `attach_controller` for `GCController.controllers`
on the title's thread, while `GCControllerDidConnectNotification` can run it on the main thread for the same controller. The
"already attached" check and the `push_back` are in two separate critical sections with `sink_->connect` between them, so both
can pass. The pad then appears as two devices ("#0" and "#0+1"). Hold a "pending" marker under `mu_`, or run `start()`'s loop
on the main queue. Also: `keyboard_ok_` is written after the observers are registered and read on the main thread, an
unsynchronised plain bool. Unexercised (no controller).

**F4. `GCController.shouldMonitorBackgroundEvents` is off by default.** So controller input, not only the keyboard, stops
while the app is not frontmost. STATUS says this for `GCKeyboard` only. In `--hidden-window` runs without a script the app has
no window to be frontmost with, so no real input arrives unless `GTA6RT_INPUT_BACKGROUND=1`. Say so in the recipe notes.

**F5. `--hidden-window` now also turns off the package splash.** `package_splash::configure` returns early for
`LEONIDA_PRESENT_WINDOW=hidden`/`own`. That is harmless for unattended runs, but it should be in the integrator notes next to
the existing item 4.

**F6. Small window-layer points.**

- WM_DPICHANGED's suggested-rect pointer comes from a static ring of 8 (window_shim.cpp:205-213). A ninth unread DPI change
  overwrites the first rect. DefWindowProcA ignores it, so this is harmless here.
- WM_SIZE for a minimised window carries the view's size; Windows sends 0x0.
- The Dock's "Quit" goes to `-[NSApp terminate:]`, not the menu target, so it exits without the suspend.

None of these is on the title's path today.

## Checked and found correct

- **Guest ABI and layouts.**
  - Every user32 shim is `WINAPI` (ms_abi) through `SHIM()`, which static-asserts the ABI.
  - `WNDCLASSEXA` (80), `CREATESTRUCTA` (80), `MSG` (48, with `lPrivate`) and `MINMAXINFO` (40) are asserted and match x64
    LLP64. `CreateWindowExA` takes 12 args with the LONG fields as `int32_t`.
  - GameInput: `STDMETHODCALLTYPE`/`CALLBACK` are `GTA6RT_WINABI`. The IGameInput, Device, Reading and Dispatcher vtable order
    is leonida's unchanged `gameinput_abi.h`, with struct static_asserts. The callback typedefs are ms_abi.
- **Last error.** Set only on failure: 87 for a bad WNDCLASSEXA or a NULL MSG, 1407 for an unknown class, 1400 for a bad HWND,
  8 when window creation fails.
- **Guest-callback threads.** Device, reading and guide callbacks run on an `rt::gdk::Thread` (an os-created, guest-capable
  thread), on the caller's thread for blocking enumeration, or on the dispatcher's caller. They never run on GameController's
  queues.
- **Hub thread safety** (probe `gi_stress.cpp`; `gameinput_mac.cpp` built with TSan and ASan on arm64, linked against the
  review tree's libraries).
  - 6 threads ran for 4 s: state pushes, connect/disconnect churn, 300-step GetPreviousReading walks, dispatcher
    create/Dispatch/Release, reading-callback register/unregister with re-entrant GetCurrentReading, and SetRumbleState plus
    GetBatteryState.
  - **TSan: 0 reports** (2,853 device events, 2,071 reading events).
  - **ASan: 0 errors** (145,469 device events, 33,106 reading events, 17,664 TOO_OLD steps).
- **Deadlocks.** None found by reading the code.
  - `set_fullscreen` and `toggle_fullscreen` hold the shim mutex but only `dispatch_async` to main.
  - AppKit callbacks take only the queue mutex.
  - `create_window_ex_a` holds no lock across `run_on_main_wait`.
- **Scripted input.** With `LEONIDA_INPUT_SCRIPT` set and `GTA6RT_INPUT=auto`, the GameController framework is not started at
  all, so unattended runs do not depend on it.
- **History.** `kHistoryDepth = 256`; the synthetic test checks 255 steps back and then `REFERENCE_READING_TOO_OLD`.

## Not verified by me

- I attached no physical controller and pressed no key, so keyboard-pad events, controller buttons, sticks and triggers,
  CoreHaptics and battery remain synthetic-only, as the builder said.
- I did not re-run `GTA6RT_FULLSCREEN=1` against the real title, because someone was using the console. Full screen is covered
  only by `win_gui_test` (3600x2028).
- I did not judge presenter resize after PresentX.
- I did not run the arm64 `run_all.sh` in full or `win_gui_test` on arm64.
- I did not test whether the title still needs `LEONIDA_XMEM_32BIT=anywhere` (another worker's state); I used it as the builder
  did.
- The accidental Quit in `review-r3iw-visible` is one run with an unknown trigger. It shows the path works, not that it is
  reliable.

## For the integrator (unchanged from the builder, confirmed)

1. In `host/CMakeLists.txt`, after `add_executable(gta6rt ...)`, add `include(src/win/win.cmake)`. Without it,
   `build/macos` stops linking: the 7 `host_win_*` symbols are undefined. With it, both x86_64 and arm64 link (measured here).
2. `main.cpp` needs no change.
3. Title runs on the current tree: `LEONIDA_XMEM_32BIT=anywhere`, and `LEONIDA_CONSOLE_PROFILE=series-x` for the 3840x2160
   request. Recipe: `LEONIDA_INPUT_SCRIPT="a@40,a@60,start@90" host/tools/run.sh 150 first-run --watchdog 140 --hidden-window --vk-driver <moltenvk x86_64>`,
   then `grep 'gameinput:' run/logs/first-run.log`. Script times count from the first `GameInputCreate`.
4. `run.sh`'s guard `pgrep -f build/macos/gta6rt` does not see a `GTA6RT_BIN` elsewhere. Check with `pgrep -x gta6rt`:
   `pgrep -f gta6rt` also matches `ninja ... gta6rt` builds.

## Files

- Logs:
  - `port/run/logs/review-r3iw-hidden.log`, `review-r3iw-visible.log` (with the close), `review-r3iw-visible2.log`;
  - `port/build/review-input-window-x86_64/{review-configure,review-build,review-gdk-run_all,review-gameinput-tests,review-win-tests}.log`;
  - `port/build/review-input-window-arm64/{review-build,review-build-all,review-tests}.log`.
- Probes, in `/private/tmp/claude-501/-Users-a-Desktop-Projects-GTA-VI/eeb510fb-badf-42ab-9e97-2a01126ed0b0/scratchpad/probes/`:
  - `pe_probe.py`, `blockref.mm`, `winlist.mm`;
  - `gi_stress.cpp`, `build_stress.sh`, `stress_tsan2.log`, `stress_asan.log`.
