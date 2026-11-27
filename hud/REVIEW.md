# Review: port/hud (performance overlay), 2026-11-26

Independent review of the builder's module, test app, screenshots, STATUS.md and the three proposal patches. Nothing outside
`port/hud` was changed. I touched only this file, plus the mtimes of `src/*` and `tests/*` (an accidental `touch`; contents
unchanged). My build trees are `port/build/review-hud-arm64` and `port/build/review-hud-x86_64`. My own adversarial test
(`rv.mm`) and the patched scratch tree are in the session scratchpad, not in the repo. The host and the title were not run.

**Verdict: approved with fixes.** One blocking item, in `d3d12_vulkan.patch`. The overlay module itself showed no way to
take focus, eat input or reach the frame dumps.

## Blocking

1. **The `d3d12_vulkan.patch` GPU-time hunk (`src/vulkan_backend.h`) puts a lock, a heap allocation and a thread wake on
   the title's submit path while the HUD is on.**
   - With the HUD on, every successful direct-queue submission calls `context_.enqueue_fence_callback(...)`.
   - `FenceWaitDispatcher::enqueue_callback` (`vulkan_backend.h:755`) then does all of this on the submitting thread:
     - takes the dispatcher `mutex_`;
     - `push_back`s an `Entry` holding a `std::function`. Its captures are int + time_point + u64 + bool, which is past
       libc++'s inline buffer, so that is a heap allocation;
     - starts the dispatcher thread if it was not running;
     - calls `vkSignalSemaphore` on the wakeup semaphore (`signal_wakeup_locked`);
     - notifies the condition variable.
   - The dispatcher thread is the one that signals the title's D3D12 fence events (`SetEventOnCompletion`). Waking it
     once more per submission changes when those events are delivered.
   - This breaks the design constraint "producers never block and never allocate". It is a way the overlay can change
     the title's timing. The same path already exists behind `LEONIDA_GPU_LATENCY=1`, but that is a diagnostic.
   - **Fix (pick one):**
     - drop the hunk and leave GPU time unset on Vulkan (`gpu -`), which the brief allowed;
     - or gate it behind a separate explicit opt-in (for example `GTA6RT_HUD_GPU=1`) and document the cost;
     - or derive busy time where the queue already reads `progress_` to recycle command buffers (`slot.ticket`), with no
       new waits.
   - With the HUD off this hunk changes nothing: `hud_gpu` is false and the `gpu_latency` path behaves as before.

No other blocking findings. All three patches apply cleanly today.

## What I ran (both arches, one run at a time, with timeouts; load average 7 to 11)

| test | arm64 | x86_64 (Rosetta) |
|---|---|---|
| build of `port/hud` standalone (`-j2`) | ok; 2 warnings in `hud_test_app.mm` (CGBitmapInfo enum mix) | ok, same |
| `hud_unit_test` | 67 checks, 0 failed | 67 checks, 0 failed |
| `hud_test_app --auto --shots <scratch>` | 56 checks, 0 failed | 56 checks, 0 failed |
| 4 Hz rate over 10 s | 40 ticks in 10.04 s, interval max 251.2 ms | 40 in 10.01 s, max 251.6 ms |
| 10 Hz rate over 10 s | 100 in 10.04 s, max 101.1 ms | 100 in 10.00 s, max 101.6 ms |
| window-server hit test inside the overlay | returns the game window; with `ignoresMouseEvents` off it returns the overlay; restored, the game window again | same |
| screencapture | failed ("could not create image from window"), as the builder reported; the in-process composite was used | same |

The screenshots in `port/hud/screenshots/` match the claims. They show:
- top-left placement, 6 pt margin, small monospaced text on a translucent backing;
- amber only on the frame-time part (injected hitch) and the memory part (injected pressure level);
- the one-line title mode;
- the full-screen min line.

They are composed in-process, not window-server captures, as STATUS.md says.

### Adversarial test (`rv.mm`, linked against my rebuilt `libgta6rt_hud.a`)

**Focus**
- Inactive case: our app inactive, another app frontmost. I attached, cycled off/min/full/title/off/full and sent F1/F2
  through `gta6rt_hud_handle_key`. The overlay never activated our app and never became key or main.
  - Runs: 3 of 4 clean.
  - In the fourth, the frontmost app changed between two other processes during the run. Our app stayed inactive.
- Active case: our app active, game window key.
  - One run, apart from one artefact: attach, every mode change and F1/F2 left `NSApp.keyWindow`, `mainWindow` and
    the frontmost pid unchanged, windowed and in full screen. After leaving full screen the game window was key and
    main again.
  - The artefact: my first check counted AppKit's own `NSToolbarFullScreenWindow` child as "the panel". After I
    filtered by class, the HUD panel was never key or main.
  - Other active runs were spoiled by other processes taking focus, among them another worker's
    `port/build/macos/gta6rt` (pid 71464) and the T3 Code app. In every failing sample focus went to an outside
    process, never to the panel.

**Hidden and miniaturized parent** (8 checks, 0 failed, both arches). The overlay was off screen (no extra on-screen
`CGWindowList` entry) in each of these cases, and never ordered the parent in or deminiaturized it:
- attached to a never-shown parent;
- parent ordered out, including after a mode change;
- parent miniaturized, including after a mode change.

It reappeared on deminiaturize.

**Parent closed first**
- Closing first: 3 checks, 0 failed, both arches. The timer stopped and nothing was left on screen. Later mode changes
  and F1 were harmless.
- Found a robustness bug (non-blocking, see N1). If the parent NSWindow is **deallocated without being closed or
  detached**, the HUD keeps its 10 Hz timer, its old panel and the dead parent's observer tokens. Ticks return early, so
  the timer does nothing. A later `attach` to a new window then skips `observe_parent` because `tokens.count != 0`: the
  new window's moves and close are not observed, and closing it left the timer running (`timer 1`). Nothing was on
  screen.
- The host cannot reach this path: `gta6rt_mac_window_destroy` closes the window and `window_shim.cpp:292` calls
  `host_win_detach` first.

**Stats-block hammer**, HUD full at 30 Hz on a window, 15 s:
- Load: 4 producer threads calling every producer function, 2 threads rewriting `state`/`driver` with one-character
  strings of random lengths, 1 reader thread checking for torn strings.

| | arm64 | x86_64 |
|---|---|---|
| producer rounds | 19.15 M | 15.14 M |
| lost increments (presents, draws, submissions) | 0 | 0 |
| torn string reads | 0 of 96.3 M | 0 of 110.8 M |
| empty reads (writer busy) | 52 M | 113 M |
| overlay ticks | 465 | 463 |
| tick max | 4.9 ms | 25.1 ms |

- Under 4-thread contention a round of 10 calls costs about 3 to 4 µs. All fields share cache lines, but uncontended
  calls cost 2 ns active and 0.3 ns inactive, as in the unit test.

**Exit with the overlay live** (30 Hz, a producer thread running, `exit(0)` called):
- from a background thread: 15/15 arm64 and 8/8 x86_64 exited with rc 0;
- from the main thread: 15/15 rc 0.

The host's own exits are `_exit` (title exit, watchdog; the game's exit/ExitProcess end in `_exit` via
`shims_core/src/process.cpp`). So timers at exit are moot there. `gta6rt_hud_shutdown()` before `_exit` is harmless.

**Presentation effect** (arm64, 120 Hz display, CAMetalLayer clear loop, presented-handler gaps, 10 s per condition):

| condition | presented/s | gap p50 | gap p99 |
|---|---:|---:|---:|
| off | 117.3 | 8.33 ms | 16.67 ms |
| full 10 Hz | 116.6 | 8.33 ms | 16.67 ms |
| title 10 Hz | 115.5 | 8.33 ms | 16.67 ms |
| off (second run) | 114.3 | 8.33 ms | 20.83 ms |
| full 10 Hz (second run) | 116.0 | 8.33 ms | 16.67 ms |

No difference above the load noise.
- Not measured: in native full screen a visible overlay window keeps the game's layer from being shown direct to the
  display, so the compositor stays involved. This applies only while the HUD is on, and any visible overlay would do
  the same.

**Cost per update** (`hud_test_app --cpu 10`, arm64, main-thread ms of CPU per second):

| condition | main ms/s | tick mean | tick max |
|---|---:|---:|---:|
| control (two runs) | 1.66 / 0.99 | | |
| min 4 Hz | 2.56 | 264 µs | 0.49 ms |
| full 4 Hz | 2.49 | 279 µs | 0.62 ms |
| title 4 Hz | 5.19 | 379 µs | 0.69 ms |
| min 10 Hz | 3.78 | 244 µs | 0.44 ms |
| full 10 Hz | 8.77 | 759 µs | 2.32 ms |
| title 10 Hz | 26.88 | 1230 µs | 4.25 ms |

- `hud_title_bench --seconds 10`: `setTitle:` mean 0.51 ms (4 Hz) and 0.46 ms (10 Hz), max 0.82 ms; main thread 6.50
  and 13.36 ms/s against a 1.28 to 1.53 ms/s control.
- So a title update costs about 1.2 ms of main-thread CPU and an overlay update about 0.25 to 0.8 ms. This agrees with
  the builder's conclusion: the overlay is cheaper per update and is the only mode visible in full screen.
- The host's `run_on_main_wait` timeouts are 5 s, so these main-thread costs cannot reach them.

## Patches

- **Apply.** `patch -p1 --dry-run` from the repo root: host, d3d12_vulkan and metal all apply with no offsets or fuzz
  (re-checked during this review).
- **Compile check.** I applied all three to a scratch copy and ran `-fsyntax-only` with the real compile commands. All
  returned rc 0:
  - x86_64 host database `port/build/d3d12_vulkan-host-x86_64`: `host/src/main.cpp`, `host/src/win/appkit_window.mm`,
    `d3d12_vulkan/shims/d3d12_xs_device.cpp`, `d3d12_com.cpp`, `d3d12_vulkan_device.cpp`;
  - `ci-metal-arm64` and `ci-metal-x86_64`: `metal/src/mtl_queue.mm`, `mtl_present.mm`, `mtl_device.mm`.
- **Configure check.** A full CMake configure of the patched tree (x86_64, rosetta) succeeds, with `add_subdirectory`
  of `port/hud` from `host/CMakeLists.txt`.
- **Not done.** Nothing was linked into the host.

### Behaviour with the HUD off (default)

- **`host.patch`**
  - `gta6rt_hud_init_from_env()` runs whenever `--hidden-window` is not given. It allocates the leaked `Ui`, registers
    one `atexit` handler and runs `apply()` with mode off, which does nothing.
  - `host_win_attach` calls `gta6rt_hud_attach`. With mode off this stores a weak parent and creates no window, observer,
    timer or dispatch source. I verified this: `off: attach creates no child window`.
  - Detach and shutdown do nothing.
  - Strings are copied into the block.
  - The only user-visible change: **F1/F2 in the game window now turn the HUD on.** The local monitor already swallowed
    every non-Cmd key, F1/F2 included. The title takes keyboard input from GCKeyboard (`gameinput_gc.mm`), not from
    these NSEvents. `gameinput_pad.h` maps no F-key. So the title sees no input change.
  - `--hidden-window` gives `ns_window_of` == nil: `host_win_attach` returns before the HUD call, and HUD init is
    skipped.
- **`d3d12_vulkan.patch`**
  - Gated calls: one relaxed load and a branch per present, per compile-queue change and per texture registration.
  - Not gated: one new relaxed `fetch_add` per dispatch (`dispatches_emitted`), the same pattern as the existing
    `draws_emitted`.
  - The `Counters` field is added, and the vk-counters log line is unchanged.
  - The `vulkan_backend.h` lambda keeps the existing `LEONIDA_GPU_LATENCY` behaviour.
- **`metal.patch`**
  - On each present: one `CACurrentMediaTime()`, a `presentedTime` read in the existing presented handler, and the
    gated calls.
  - `execute()` is restructured around a `Status result`, with the same semantics: the `Encoder` is still destroyed
    inside the autorelease pool.
  - The driver string is always set.

### Behaviour with the HUD on

- Vulkan: see Blocking 1.
- Metal (non-blocking, the backend is not shipped): `mtl_present.mm` calls `device_->compile_queue_stats()` on every
  present. That takes `CompileQueue::mutex_` (`mtl_pipeline.mm:543`). Compiles run unlocked, so the hold time is
  short, but it is still a lock on the present path. Better to push the pending gauge from inside `CompileQueue`
  where it changes, as the Vulkan patch does in `d3d12_pipeline_compile.h`.

## Frame dumps

`present_dump.h` writes PNGs from the presenter's own image. Nothing in `port/host`, `d3d12_vulkan/platform` or
`shims_*` uses `CGWindowList*` or screencapture. The overlay is a separate NSWindow and can never appear in the PNG
dumps. This is by reasoning, which agrees with the builder; I did not run a dump.

## Non-blocking findings

- **N1. Parent released without close.** Teardown is incomplete, as described above. Fixes:
  - in `gta6rt_hud_attach`, call `teardown_runtime(u)` whenever runtime state exists (`tokens.count || panel || timer`),
    not only when `u->parent` is non-nil;
  - in `tick()`, call `teardown_window` when `u->parent` is nil instead of returning.
- **N2. C and C++ callers get different stats blocks.** At `-O2`, a C TU (weak `gta6rt_hud_stats_block` with its own
  static) and a C++ TU (inlined `inline` function) used different blocks in my test:

  ```
  c 0x104984188 cpp 0x104984000 same 0
  ```

  - The same test at `-O0` shared one block.
  - Effect: a plain-C producer is never activated and its data never reaches the overlay. This contradicts "callable
    from C". No patch has C producers today.
  - Fix: give the block one non-inline definition in `gta6rt_hud` (an `extern` object) instead of per-language inline
    or weak copies.
- **N3. Title mode overwrites the window title the title sets.** It writes `<title captured at mode entry> - <line>`
  each tick and restores that captured title on exit. A `SetWindowText` from the title is shown for at most one tick
  and is lost on restore.
  - The title itself is not affected: `GetWindowText` reads the shim's own record (`window_shim.cpp:136`).
  - It also fights `LEONIDA_TITLE_HUD=1` (`title_hud.h`) if both are on.
  - STATUS.md mentions the first point but not the restore.
- **N4. Readings jump when the HUD is turned on mid-run.**
  - `last_present_ns` and the published totals keep their old values while the HUD is off. The first window after
    turning it on (F1) can show a huge `max` frame time and a burst of draws per frame for about 1 s.
  - Fix: reset `last_present_ns` and the sampler baseline when the block goes active.
- **N5. Small accuracy points.**
  - The Vulkan pending gauge is not updated when queued jobs are dropped (cancel, stop).
  - Metal reports `psofail 0` as a real zero, not `-`.
  - Vulkan "tex" is cumulative, not live (STATUS says so).
- **N6. One comment in UI code.** `src/hud.mm:552` has `}  // namespace`, against the no-comments-in-UI-code rule.
- **N7. Measurement noise.** The builder's numbers are single runs on a loaded machine, and so are mine. Orders of
  magnitude agree. The builder's "title 10 Hz 11.34 ms/s" was 26.88 ms/s in my run.

## Integrator steps

1. Fix Blocking 1 in `port/hud/proposals/d3d12_vulkan.patch`: drop the `vulkan_backend.h` hunk or gate it as described.
   Then re-run the dry run.
2. Apply the patches:

   ```sh
   cd /Users/a/Desktop/Projects/GTA-VI
   for p in host d3d12_vulkan metal; do patch -p1 --dry-run < port/hud/proposals/$p.patch && patch -p1 < port/hud/proposals/$p.patch; done
   ```

3. Rebuild the host per `port/README.md`:

   ```sh
   cmake -S port -B port/build/host-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 ...
   ninja -C port/build/host-x86_64 -j2
   ```

4. Default runs need nothing: the HUD is off.
5. To use it: `GTA6RT_HUD=min|full|title` or `--hud MODE`. Rate: `GTA6RT_HUD_RATE` (0.5 to 30 Hz, default 4). Budget:
   `GTA6RT_HUD_BUDGET_MS` (default 33.4). In the window, Fn+F1 cycles modes and Fn+F2 toggles.
