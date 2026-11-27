# port/hud: performance overlay (gta6rt_hud), 2026-11-26

A small transparent overlay (or the same line in the window title) fed by a lock-free stats block. Module
`gta6rt_hud` (static library, Objective-C++, AppKit/QuartzCore/Foundation only). Nothing here has been applied to
`port/host`, `port/d3d12_vulkan`, `port/metal` or `port/CMakeLists.txt`; the three patches are in `proposals/`.
The host and the title were never run; everything below was measured with `tests/hud_test_app.mm`, a window with a
CAMetalLayer that renders 19 draws at 60 Hz.

## Layout

| path | content |
|---|---|
| `include/gta6rt_hud/stats.h` | header-only C stats block and producer calls (any thread, ms_abi-safe) |
| `include/gta6rt_hud/hud.h` | control API: init, attach/detach, mode, rate, key handler, info |
| `src/hud_model.{h,cpp}` | pure C++: sampler (deltas, 1 s window, smoothing) and formatter (lines, colour spans); unit tested |
| `src/hud.mm` | AppKit: child panel, CATextLayer, timer, memory-pressure source, title mode (no comments, per the UI rule) |
| `tests/hud_unit_test.cpp` | 67 checks: atomics from 4 threads, seqlock strings with 3 writers, ms_abi caller, sampler, formatter, colours, keys, pad-map collision, no-window path |
| `tests/hud_test_app.mm` | window + Metal animation + 56 scripted checks, screenshots, `--cpu` measurement |
| `tests/hud_title_bench.mm` | cost of `setTitle:` at 4 and 10 Hz |
| `screenshots/` | six PNGs (see "Screenshots" for how they were made) |
| `proposals/` | `host.patch`, `d3d12_vulkan.patch`, `metal.patch`, `README.md` |

Build (standalone, one arch per tree; also works under `add_subdirectory(hud)`, checked with a parent project):

```sh
cmake -S port/hud -B port/build/hud-arm64  -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64
cmake -S port/hud -B port/build/hud-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64
ninja -C port/build/hud-<arch> -j2
port/build/hud-<arch>/hud_unit_test
port/build/hud-<arch>/hud_test_app --auto [--shots DIR]   # needs a display session; exits 0 without one
port/build/hud-<arch>/hud_test_app --cpu 15
port/build/hud-<arch>/hud_title_bench --seconds 20
```

## Design

- **Isolation.** The overlay is an `NSPanel` subclass (borderless, non-activating, `ignoresMouseEvents`, clear
  background, `canBecomeKey/Main` NO, `hidesOnDeactivate` NO, same level as the game window), added with
  `addChildWindow:ordered:NSWindowAbove`, so it moves with the parent and sits just above it in the window order. Text is
  a `CATextLayer` (monospaced system font 10 pt, `contentsScale` = the window's backing scale, re-set on
  `DidChangeBackingProperties`) on a 42 % black rounded layer. Resize, screen change, full screen, de-miniaturise
  and expose notifications re-anchor it to the top-left of `contentLayoutRect` (6 pt margin). It never touches the title's
  CAMetalLayer, swapchain or Vulkan queue.
- **Not in the PNG dumps.** `present_dump.h` writes PNGs from bytes it reads back from the presenter's image
  (`write_png(..., src)`); the overlay is a different NSWindow composited by the window server, so it cannot be in them. This is
  reasoning from the code, not a run.
- **Data.** `stats.h`: a POD struct of 64-bit integers touched with `__atomic` builtins (relaxed), one instance
  (`gta6rt_hud_stats_block()`, inline in C++, weak in C). Producers either add events (`note_present`, `add_draws`,
  `note_submission`, `note_gpu_ns`, `note_gpu_done`, `note_display_latency_ns`, `add_imports`) or store running totals
  they already keep (`publish_totals`, `publish_pipelines`, `set_pipelines_pending`, `publish_textures_waiting`). Strings
  (state, driver, profile; 63 bytes) use a seqlock whose writers try-lock and drop on contention: nothing blocks or allocates.
  `have` bits mark optional fields; an unset field prints `-`. Until the overlay switches the block active, every call is one
  relaxed load and a branch.
- **Sampling.** An `NSTimer` on the main run loop in `NSRunLoopCommonModes` (default 4 Hz, `GTA6RT_HUD_RATE` or
  `gta6rt_hud_set_rate`, clamped 0.5 to 30 Hz). Per tick: read the block (`frame_ns_max` is exchanged to 0), `task_info(TASK_VM_INFO)`
  for `phys_footprint`, resident and compressed bytes, `hw.memsize` for RAM, format, update the layer or title.
  FPS, frame time (avg, worst) and the per-frame figures are over the last 1 s of ticks; "avg" in the full view is an EMA with
  a 3 s time constant.
- **Fields.** fps; frame time = present-to-present interval, avg and max; draws, dispatches, submissions and ExecuteCommandLists
  CPU time per frame; GPU time per frame (producer-dependent, below); present-to-display latency (Metal only); pipelines built /
  pending / cache hits / PSO failures; textures waiting; resident, compressed and footprint against RAM; guest imports per
  second (host counter callback, only when `--trace` counts them); driver, console profile and a state string (default
  "compiling N pipelines" while the gauge is above 0).
- **Lines.** min: `60 fps 16.7ms (max 21) | draws 19 | gpu 4.1ms | pso 3/0 pending | mem 0.2/24G`. full adds
  `avg 59.0 | sub 1.0/f | disp - | exec 1.6ms/f | lat 25.0ms | imp - | tex -` and
  `cache 3 psofail 0 | rss 0.1G cmp 0.0G | <driver> <profile> | <state>`. Colour only on trouble: the frame-time part goes amber
  when avg is over the budget (`GTA6RT_HUD_BUDGET_MS`, default 33.4) or max over twice it, red over 1.5x; the memory part goes amber on
  a dispatch memory-pressure WARN or footprint over 85 % of RAM, red on CRITICAL or over 95 %.
- **Title mode.** Writes `<original title> - <min line>` at the tick rate and restores the original title when leaving the mode or
  at shutdown. If the title also calls `SetWindowText`, the later write wins until the next tick.
- **Modes and controls.** `GTA6RT_HUD=off|min|full|title` (default off), host option `--hud MODE` (sets the env var),
  `gta6rt_hud_set_mode` at run time. Keys, from the host's existing local key monitor: **F1 cycles off, min, full, title; F2 toggles off
  and the last mode shown.** `gameinput_pad.h` maps arrows, Return, Space, KeypadEnter, Backspace, X, Y, Q, E, Escape, Tab, W/A/S/D, I/J/K/L, F, G, Z, C; no F-key
  is used (F1/F2 are HID 0x3A/0x3B, checked in `hud_unit_test` against `kKeyboardPadKeys`, which is read from the real header). F11
  is the host's full-screen toggle. Keys with Shift/Ctrl/Opt/Cmd and auto-repeats are not acted on. On a Mac keyboard F1/F2 need Fn held
  unless "Use F1, F2, etc. keys as standard function keys" is on (same as F11 today).
- **Safety.** No window or timer exists before `gta6rt_hud_attach`; attach with NULL (layer-only or `--hidden-window`) logs
  `[rt] hud: overlay disabled: ...` once and turns the producers off. The parent is held weakly; `NSWindowWillClose` tears
  everything down; `detach` and `shutdown` invalidate the timer, cancel the pressure source, close the panel, restore the title. Overlay code runs under `@try`; an
  exception disables it with one line. Mode off holds only the weak window reference (no observers, timer, panel or dispatch
  source). All AppKit work is queued to the main thread, so nothing happens until the main run loop services it.

## Measured (this machine: 1800x1169 pt Retina, load average 8 to 10, Rosetta for x86_64)

Unit test, both arches: 67 checks, 0 failed. `hud_test_app --auto`: x86_64 56 checks 0 failed; arm64 50 checks 0 failed (the arm64 run had no `--shots`, so its 6 screenshot checks did not run). The scripted run covers, in one process: nothing runs before the window exists; attach; panel flags; top-left offset 6.0,6.0
after a move, a bottom-edge resize and a top-left-edge resize; full screen entered (panel stays a child, visible, 6,1079 in a 1800x1169
screen, on the active space, offset 6.0,6.0) and left; title mode and restore; F1/F2 delivered as NSEvents through a local monitor
identical to the host patch; window closed (detached, timer stopped, no ticks afterwards); re-attach to a new window; detach; shutdown (no timer,
no ticks, producers inactive).

**Rate** (x86_64): 4 Hz: 40 ticks in 10.05 s, interval mean 250.0 ms, max 251.9 ms. 10 Hz: 100 ticks in 10.02 s, mean 100.0 ms, max 100.1 ms
(arm64: 40 in 10.05 s, 250.0 / 250.8 ms and 101 in 10.10 s, 100.0 / 103.6 ms). Tick body (read, format, update): 0.2 to 0.9 ms mean.

**Click-through.** Flags verified (`ignoresMouseEvents`, non-activating, cannot become key or main). Window-server hit test
(`+[NSWindow windowNumberAtPoint:belowWindowWithWindowNumber:]`, which asks the window server): at a point inside the overlay the topmost window is the
game window; with `ignoresMouseEvents` switched off for a moment the same point returns the overlay; switched back it returns the game
window again. A synthetic click could not be posted: `AXIsProcessTrusted()` is 0 here, so `CGEventPost` is not allowed; no real click
was delivered to the test view. (An earlier version of the test posted a click guarded by a wrong condition; with the process untrusted it
had no effect on the test window and I cannot say where, if anywhere, it landed. The guard now requires the test window to be the topmost window at the point and the process to be trusted.)

**CPU of the overlay, main thread** (`hud_test_app --cpu 15`, window in front and visible, 15 s per condition; the main thread of the
app, ms of CPU per second, via `thread_info`; control = HUD off):

| | arm64 | x86_64 (Rosetta) |
|---|---:|---:|
| off (two controls) | 1.37 / 0.55 | 2.38 / 2.59 |
| min 4 Hz | 2.75 | 4.63 |
| full 4 Hz | 3.36 | 7.12 |
| title 4 Hz | 7.40 | 15.97 |
| min 10 Hz | 3.53 | 10.22 |
| full 10 Hz | 4.06 | 10.22 |
| title 10 Hz | 11.34 (timer delivered 4.86 ticks/s in this run, 10.05 in the x86_64 run: load, not explained) | 29.09 |

Roughly 0.3 to 0.6 ms of main-thread CPU per overlay update on arm64, 0.5 to 0.9 ms under Rosetta, i.e. 0.1 to 0.6 % of one core at 4 Hz. Noisy (loaded machine,
one run each).

**Window title** (`hud_title_bench`, arm64, plain window, no rendering, 20 s per condition, one run):

| | updates | main-thread CPU ms/s | `setTitle:` call mean / p50 / p99 / max |
|---|---:|---:|---|
| control (two runs) | 0 | 1.44 / 0.84 | |
| 4 Hz | 80 | 6.58 | 0.69 / 0.51 / 13.95 / 13.95 ms |
| 10 Hz | 200 | 13.73 | 0.53 / 0.47 / 0.78 / 8.96 ms |

So a title update costs about 1.3 ms of main-thread CPU (4 Hz: (6.58 - 1.14) / 4; 10 Hz: (13.73 - 1.14) / 10 = 1.26), the call itself
0.5 to 0.7 ms, with outliers of 9 to 14 ms. The overlay costs about 0.3 to 0.6 ms per update; its tick body also showed outliers on this loaded machine
(max 0.5 to 23 ms across the non-occluded runs, one 23 ms in an x86_64 10 Hz run), so outliers are not unique to the title. WindowServer CPU was sampled with `ps` but is not usable (control rows were higher than update rows). A 30 Hz row
delivered 193 of 600 updates and was removed from the benchmark without investigation.

**Producer cost** (`hud_unit_test`, single thread): inactive 0.20 to 0.29 ns per call; active add 1.8 ns, `note_submission` 1.9 ns, `note_present`
15 ns (arm64) / 21 ns (x86_64; includes a clock read and a CAS).

**Recommendation.** Use the overlay (min for a quick look, full for diagnosis): about a third to half the main-thread cost per update of title mode,
works in full screen, and keeps the title bar and `SetWindowText` alone. Title mode is the fallback for
windowed runs when a second window is unwanted; it is invisible in native full screen (the title bar is hidden; the test confirms the title still
changes). 4 Hz is enough to read; 10 Hz costs 2 to 2.5x as much.

## Screenshots

`screencapture -l <window id>` failed here ("could not create image from window"; `CGPreflightScreenCaptureAccess()` is 0 for this process chain), and
`CGWindowListCreateImageFromArray` returned null, so the PNGs are **composed in-process by the test app**, not window-server captures: the drawable's pixels (blit
to a buffer), the title bar via `cacheDisplayInRect:` of the frame view (omitted in full screen), and the overlay's own layer via `renderInContext:` at the panel's real position
and scale. They show the real overlay text, colours and placement, not the compositor's blending. Only the test app's windows are involved; no other window was captured.

`windowed_full.png` (3 lines, amber max-frame stutter from an injected 70 ms hitch), `windowed_min.png`, `windowed_min_pressure_warn.png`
(memory part amber, injected level 1), `windowed_title.png` (the line in the title bar), `fullscreen_full.png`, `fullscreen_min.png` (x86_64 run).

## Not done or not verified

- GPU time: only what each backend gives cheaply (see the proposals). No timestamp-query integration in the Vulkan layer.
- Memory pressure was never raised for real (`memory_pressure` needs privileges); the colour path is covered by the formatter test and the injected-level screenshot, the
  dispatch source is created and cancelled but its handler has not seen an event.
- Synthetic click delivery (needs Accessibility), a real `screencapture` (needs Screen Recording), Spaces other than the full-screen one,
  multiple displays, and Retina scale change between displays were not exercised.
- The patches were checked with `patch --dry-run`, then applied to scratch copies: `d3d12_vulkan_device.cpp`, `d3d12_xs_device.cpp` and `d3d12_com.cpp` compile
  (`-fsyntax-only`, arm64 flags), `main.cpp` and `appkit_window.mm` compile, `port/metal` builds (arm64). None was linked into the host or run with the title.
- Present-to-display latency exists only on the Metal backend; the Vulkan layer leaves it unset (`lat -`).

## Essentials round (2026-11-26): quiet test, live numbers against the title

Own trees `build/hud-input-x86_64` (tests) and `build/input-x86_64` (host with the overlay). Changes: `GTA6RT_HUD_LOG=<seconds>` logs the full three lines plus the raw cumulative counters
(`[rt] hud: ... || raw presents N draws N dispatches N submissions N pso built N pending N failed N`) from the overlay's own tick (needs a visible window); the min line shows
`stalled 3.4s` (amber, red over 2 s) instead of `idle` when frames were presented before but none in the last second (a title stall is not idleness); `hud_unit_test` 77 checks, 0 failed (x86_64).

**Quiet test** (`hud_test_app --quiet`, `run/logs/hud-quiet.log`, 28 checks, 0 failed, about 15 s): Accessory activation policy, a 360x203 window at the bottom-right corner of the main screen,
ordered front without making it key; the frontmost application stayed the user's (checked before and after), the app never became active. Checked: overlay ignores mouse events, cannot become
key or main, non-activating borderless, is not key; sits 6,6 from the content's top-left and follows a move, a bottom-edge resize and a top-left-edge resize; closing the window detaches it
(no timer, no overlay, 0 ticks in the next 1.2 s); after `gta6rt_hud_shutdown` no timer, no overlay, 0 ticks. Not done quietly: the window-server hit test (another window covered the whole
screen, so no point was conclusive; the flag check passes, and the earlier non-quiet `--auto` run had the hit test conclusive with its control) and full screen (it would take over the display; code
unchanged this round, the earlier `--auto` run covers it).
- **Min line width.** Worst case I could construct (144 fps, 123 ms, max 2010, 25,400 draws, gpu 123 ms, 56k built / 12k pending pipelines, 24/24 GB):
  `144 fps 123ms (max 2010) | draws 25400 | gpu 123ms | pso 56k/12k pending | mem 24/24G` = 85 characters, 538 pt with the overlay's font and padding; fits 1280 and even 640 pt. A real line from the
  title: `24 fps 68.7ms (max 1191) | draws 0.0 | gpu - | pso 89/1.7k pending | mem 5.8/24G`. The panel is also clamped to the window's width.
- **F1/F2 with and without Fn-lock** (synthetic key events through the same local-monitor path as the host): plain F1/F2 (Fn-lock on, no modifier flag) and Fn+F1/F2 (Fn-lock off: the event carries
  `NSEventModifierFlagFunction`, also with NumericPad) cycle/toggle correctly; Cmd+Fn+F1 is not taken. 6 of 6 consumed. Physical limit, not testable here: with Fn-lock **off** a Mac keyboard
  sends F1/F2 alone as brightness keys that the system consumes, so the overlay only sees them with Fn held; that is the macOS default behaviour for the F-row, same as the F11 full-screen item.

**Live numbers against the title** (visible run, `run/logs/input-r4.log`: window 1920x1080, `--hud` default min, `GTA6RT_HUD_LOG=5`, cold pipeline cache and a loaded machine, so the title was slow; the numbers still
check the plumbing). The HUD's raw counters against the run's own `vk-counters` / `submit phases` / os memory lines for the same seconds:

| field | overlay | log |
|---|---|---|
| frames | 89 fps (t~19 s), 65 fps (t~24 s) | `submit phases` 79.6 and 77.7 frames/s (5 s means) |
| submissions per frame | `sub 2.0/f` | 159.2 submissions/s over 79.6 frames/s = 2.0 |
| submissions total | raw 2038 at about t=42 s | vk-counters 1881 (t=39 s), 2137 (t=45 s) |
| draws total | raw 1380 at about t=42 s | vk-counters draws emitted 3 (t=39 s), 2076 (t=45 s) |
| PSO failures | 15026 | 14998 graphics + 28 compute = 15026 |
| pipelines built | 111 (t~42 s), 120 (t~46 s) | graphics 2 + compute 101 = 103 (t=39 s), 2 + 118 = 120 (t=45 s) |
| pipelines pending | 1673 | gauge of the compile queue (no log line to compare) |
| memory | 5.9-6.4 GiB | os memory report: footprint 6038 MiB, 6453 MiB, 6590 MiB (= 5.9, 6.3, 6.4 GiB) |
| exec time per frame | `exec 3165ms/f` in a 3 s stall | ExecuteCommandLists longest 3.13 s (t=51 s) |

All of these agree within the sampling offset. Fields that are blank by design on this path: `gpu -` (the Vulkan layer reports no GPU time: "calibrated timestamps lack the DEVICE or QueryPerformanceCounter domain",
and nothing feeds `note_gpu_ns` there, so the overlay shows `-`, never a made-up number); `lat -` (Metal backend only); `imp -` (the import counter exists only with `--trace`, the same run used `--trace=0`
so the count was off; not a wrong number); `disp` has no log counterpart (the `dispatches_emitted` counter is not printed anywhere), 1.0 per frame is what the counter says.
Nothing was found wrong in the min line. In the stalled seconds the line now reads `stalled N s`.

## Windowless log (round 11, play worker, 2026-11-27)

`GTA6RT_HUD_LOG=<s>` now also works with `--hidden-window`: the host initialises the overlay when the variable is set
(`host/src/main.cpp`), and with no window attached a dispatch timer at the overlay's rate samples the same stats block and
prints `[rt] hud (no window): <the full three lines> || raw ...` every `<s>` seconds. It stops sampling while a window is
attached (the panel's own tick logs then). Measured on the Metal path (`GTA6RT_GPU=metal`, hidden, world phase,
`run/logs/play-d.log`): `1 fps 781ms (max 781) | draws 1721 | gpu 1144ms | pso 2.6k/3 pending | mem 13/18G gpu 6.5G`;
menus `29 fps 34.5ms | draws 7.7 | gpu 6.2ms`. hud_unit_test 82 checks, 0 failed.
