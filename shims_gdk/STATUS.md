# shims_gdk (S1.5b) status

Targets: `gta6rt_shims_gdk` (static, link with `-force_load`), `gta6rt_xtaskqueue` (static), 17 unit-test binaries,
`gdk_link_check`. Entry point: `gta6rt_shims_gdk_register()` (include/gta6rt/shims.h).

## Implemented (ported from leonida/runtime/shims, tables unchanged)

| shim | source | table (dll: exports) | diff vs leonida |
|---|---|---|---|
| xgameruntime, _user, _gamesave, _store, _offline | `src/xgameruntime*.cpp` | xgameruntime.dll 42, gameplatformservices.dll 2 | store 2 lines, user 44, offline 39, gamesave 68, main 151 |
| dstorage | `src/dstorage.cpp` | dstorage.dll, dstorage_xs.dll, xcrdapi.dll (1 each) | 375 lines; the console COM ABI only |
| acphal | `src/acphal.cpp` | acphal.dll 9 | 0 lines (compiles unchanged) |
| xapu_chad (the linked one) | `src/xapu_chad.cpp` | xapu.dll 5, chad.dll 5 | 10 lines; xapu.cpp/chad.cpp not ported (dead) |
| silent_audio | `src/silent_audio.cpp` | COM objects only (IMMDeviceEnumerator, IMMDevice, IPropertyStore, IAudioClient3, render client); no table | 130 lines; silent path only, no WASAPI |
| gameinput (mac) | `src/gameinput_mac.cpp`, `src/gameinput_gc.mm`, `src/gameinput_pad.h`, `src/gameinput_backend.h` | gameinput.dll 2 (`GameInputCreate`, ordinal `#1`) | rewritten from leonida's `gameinput_no_sdl.cpp`; see "GameInput on GameController" |
| xtaskqueue | `xtaskqueue/` | n/a | see below |

Registration: `SHIM_TABLE` is kept; each shim's table text is unchanged. `shared/shims.h` (a copy of leonida's) redirects the
macro into `rt::gdk::TableRegistration`; `src/gdk_register.cpp` groups the rows by DLL and calls
`gta6rt_shims_register_dll` with `CODE_NATIVE` exports (names starting with `#` also get that ordinal).

Porting changes (all in the copies): `std::wstring` -> `std::u16string` (WCHAR is 2 bytes; DSTORAGE_REQUEST filenames and
the file objects use `char16_t`), `%lx` GUID/HRESULT formats for 32-bit LONG, `__try/__except` guarded copy ->
`mach_vm_read_overwrite` (`rt::gdk::guarded_copy`), `ReOpenFile` -> reopen by path, `std::thread` -> `rt::gdk::Thread`
(os-created guest-capable threads, `support/gdk_thread.h`), env via `gta6rt_env_*` and `getenv`, QPC from the os module,
display-sleep deferral via IOPMAssertion (`src/gdk_platform_darwin.cpp`), shader directory from `shader_path.txt` beside the
executable or `LEONIDA_SHADERS`.

dstorage backend: a shared prepare pool with an in-order window per queue, `F_NOCACHE` positional reads on the file's native descriptor (the `gta6rt.internal!FileNativeFd` row of shims_core; without it
reads go through the registered kernel32 `ReadFile` row with an OVERLAPPED offset and sizes through the `GetFileSizeEx` row, plain Win32 calls when no row is registered), system zlib with leonida's inflater as the error path, worker
threads are `rt::gdk::Thread`. `dstorage.cpp` includes `bcpack.h` and `shader_library.h` from `port/portable/src`. The "DirectStorage backend" section below has the applied patch, knobs, tests and numbers.

xtaskqueue (libHttpClient, MIT): `TaskQueue.cpp`, `AsyncLib.cpp` and the headers are leonida's copies with two edits:
`#ifdef _WIN32` -> `#ifdef GDK_TQ_HANDLE_WAITS` (defined in `pch.h`), and a `CALLBACK` on one lambda in `AsyncLib.cpp` (x86_64
ms_abi function-pointer conversion). `ThreadPool_win32.cpp` / `WaitTimer_win32.cpp` are replaced by new portable
`ThreadPool_posix.cpp` (on-demand worker threads, limit max(4, cores) plus long-running ones, 2 s idle exit, `Terminate`
drains pending and running work like `WaitForThreadpoolWorkCallbacks`) and `WaitTimer_posix.cpp` (one thread per armed timer,
generation counter, a callback may terminate its own timer).


## Stubs and gaps (by name)

- **arm64 registration**: `gta6rt_shims_gdk_register()` returns `GTA6RT_E_UNSUPPORTED` on arm64. The tables hold `ms_abi`
  function pointers, and the COM objects handed to the guest (IXGameImpl*, IDStorage*, IAudioClient3, IGameInput, ...) have
  `ms_abi` vtables. arm64/FEX needs `gta6rt_guest_frame` adapters (shims_core/src/frame_adapter.h has the template) for every
  table entry AND every vtable method. All code compiles and the unit tests pass natively on arm64 (they call the functions
  directly); only the guest-facing registration is missing. Not started.
- **gameinput**: see "GameInput on GameController" below. Not served: every one of the 62 slots of IGameInput, IGameInputDevice, IGameInputReading and IGameInputDispatcher logs its first call once, and an unserved one logs `first call of X: not served, returns R (logged once)` (the `--census` test proves it for all 34 unserved slots). Failure codes: `GetTemporalReading` E_NOTIMPL (as GameInput v0 itself), `CreateAggregateDevice` for any kind but gamepad E_NOTIMPL, `FindDeviceFromObject/PlatformHandle/PlatformString` DEVICE_NOT_FOUND, `CreateForceFeedbackEffect` and `SetHapticMotorState` E_INVALIDARG, the six raw-device slots E_NOTIMPL, `EnableOemDeviceSupport` E_NOTIMPL, `AcquireExclusiveRawDeviceAccess` false. Void slots that cannot fail (`SetForceFeedbackMotorGain`, `SetInputSynchronizationState`, `SendInputSynchronizationHint`, `PowerOff`, `ReleaseExclusiveRawDeviceAccess`, `SetFocusPolicy`) and getters that answer 0/false (switch, key, mouse, touch, motion, arcade, flight, wheel, raw report) are logged as "not served" with that answer; `RegisterKeyboardLayoutCallback` returns S_OK and the callback never fires (logged as such). The title uses none of the unserved slots (see Review response, round 3).
- **silent_audio**: output is silence by design; no WASAPI/CoreAudio.
- **title memory hooks** (`support/gdk_title_memory.h`): `set_title_memory_hooks()` is where d3d12_vulkan plugs in
  (`to_cpu`, `other_view`, `texel_write`, `write`, `commit`). Unset, title memory is plain memory and the write is
  `guarded_copy`. The header is private to this module; to let S1.6 call it, it must move to `include/gta6rt/` (not done).
- Network, store, user, gamesave and the other xgameruntime classes keep leonida's offline behaviour (unchanged).
- Not exercised: any call from real guest code. Everything below ran host-side only.

## Tests (leonida's, ported; list in CMakeLists.txt)

Builds: `build/shims_gdk-x86_64`, `build/shims_gdk-arm64` (RelWithDebInfo, Ninja, `-DGTA6RT_MODULES="os;isa;seh;loader;shims_core;shims_gdk"`,
`-j3`). Logs: `build/shims_gdk-build-<arch>.log`, `build/shims_gdk-run-<arch>.log`.
Run: `shims_gdk/tests/run_all.sh build/shims_gdk-<arch>/shims_gdk 120` (serial; each process killed after the timeout).

- arm64 native: 38 of 38 pass (api_ref_state, api_global_refs, 5 context_*, xgameruntime 174 checks, _user 46, _gamesave 76,
  _store 27, _offline 74, dstorage 489, acphal 166 + `--noncached-only` 14, xapu_chad 33, silent_audio 128,
  gameinput_no_sdl and `--input-script`, 19 wait_registration cases; the first-session figure, before the GameInput rewrite below). 8 extra repeats of the 11 threading/IO-heavy tests: 0 failures in 88 runs.
- x86_64 under Rosetta, one process at a time with a kill timeout: 38 of 38 pass (same set), run twice (before and after the last rebuild).
- `gdk_link_check` (links the real shims_core, isa, os, portable; no stand-ins): links with no unresolved symbols; on x86_64
  it registers all tables with the real registry and resolves 10 imports (including `gameinput.dll!#1` and
  `gameplatformservices.dll!QueryApiImpl`) to thunks, rc 0. On arm64 it reports the `E_UNSUPPORTED` above.
- `compat_abi_test` PASS on both arches, with new layout asserts for WAVEFORMATEX (18), WAVEFORMATEXTENSIBLE (40),
  PROPERTYKEY (20), PROPVARIANT (24), AudioClientProperties (16), SpatialAudioObjectRenderStreamActivationParams (40).
- The tests link the real `gta6rt_os` Win32 surface plus `tests/support` stand-ins for shims_core pieces (registry_min, fs_stub,
  com_stub, xmem_fake). `-DGTA6RT_GDK_TEST_WIN32_EMUL=ON` links `win32_emul.cpp` instead of os (was used before os landed; the
  last full runs use os).

Test-side fixes made this session: `GetTempPathW` returns a Windows spelling (`Z:\var\...`) from os, which the tests then used
as a host path (this had created a stray directory in `port/`, since removed) -> the prelude maps it to a host path;
`acphal_test` overrode `operator new` with a `GetCurrentThreadId()` call, which recursed into os thread adoption -> uses
`pthread_threadid_np`; `dstorage.cpp` maps `errno == EFAULT` after a failed ReadFile to `ERROR_NOACCESS` (shims_core's `read_file` folds EFAULT into
ERROR_GEN_FAILURE (31); mapping it to ERROR_NOACCESS there is the cleaner fix).

## Interface and top-level notes for the integrator

- Link: `gta6rt_shims_gdk` with `-force_load` (tables register from static constructors), after `gta6rt_shims_core`; it needs
  shims_core (registry, `rt::fs`, `rt::xmem`, com), os, `gta6rt_portable` (bcpack, shader_library); `gta6rt_xtaskqueue` is a PUBLIC
  dependency; frameworks IOKit and CoreFoundation are linked by the target. host/CMakeLists.txt already force-loads it.
- Call `gta6rt_shims_init()` then `gta6rt_shims_gdk_register()`; on arm64 expect `GTA6RT_E_UNSUPPORTED` until frame adapters exist.
- No change to the top-level CMakeLists or to include/gta6rt/ was needed. compat/ additions (audio, propsys and propidl headers) were
  made earlier in this task, with the layout asserts above in `compat/tests/abi_test.cpp`.

## Open issues

- arm64 guest-facing registration (above).
- Rosetta/x86_64 SDL3 and Vulkan availability gate the SDL3 GameInput path.
- Title memory hooks header placement (above).
- `shims_gdk` tests use os's Win32 file/event layer, so os changes (path spelling, EFAULT) can break them; the two cases above were found that way.

## Review response (REVIEW.md: APPROVED WITH FIXES)

- **B1 fixed.** `file_size_of` and `pread_at` in `src/dstorage.cpp` no longer look at os `gta6rt_file` handles. They call
  `rt::shims::find("kernel32.dll","GetFileSizeEx")` and `find("kernel32.dll","ReadFile")` (OVERLAPPED offset, positional),
  falling back to the plain Win32 calls when no row is registered (the unit tests). `ERROR_HANDLE_EOF` from the row maps to a
  0-byte read. shims_core's `read_file` reports a pread EFAULT as ERROR_GEN_FAILURE, so `pread_at` clears errno before the call
  and maps `errno == EFAULT` to `ERROR_NOACCESS`, which keeps the title-memory commit retry working.
  New test `gdk_real_core_test` (`tests/real_core_test.cpp`, target links the real shims_core, isa, os; no stand-ins):
  GAME: read of 4 KiB at offset 129 through shims_core's CreateFileW handle matches the file bytes, an overlay file is read at
  an offset, a read past the end reports `E_DSTORAGE_END_OF_FILE`, and a read stays right with 300 other descriptors open.
- **B2 fixed.** `rt::gdk::register_com_classes()` (`src/gdk_register.cpp`) registers `CLSID_MMDeviceEnumerator` with
  `rt::com_shim::register_class` through an adapter to `silent_audio::create_enumerator`. `register_all()` calls it, and the
  arm64 `gta6rt_shims_gdk_register()` calls it before returning `E_UNSUPPORTED`, so the COM class works on arm64 too (it is a plain
  C++ factory, not an ms_abi table row). `shared/com_shim.h` holds the declaration copied from shims_core's. `tests/support/com_stub.cpp`
  gained a `register_class`/`find_class` stand-in; `silent_audio_test` now checks the module registered the factory and drives it.
  `gdk_real_core_test` calls the real `ole32!CoCreateInstance(CLSID_MMDeviceEnumerator)` row and gets S_OK and a render
  endpoint; `gdk_link_check` does the same CoCreateInstance on both arches.
- **B3 corrected.** The claim was wrong: the link check has 10 probes. STATUS says 10; the check now prints
  "(10 imports resolved)".
- Also done from the non-blocking list: `device_audio_endpoint` uses `WCHAR*` / `u'\0'`; the stray `shims_gdk/build/` tree is
  deleted; the CMakeLists header comment no longer points at STATUS.md for the undefined references.
- Not done (open): shared/ header drift against shims_core (should-fix 2), `SHIM()` ms_abi compile-time check in the tables (3),
  `ThreadPool_posix::Submit` silent drop when no thread can be spawned (4), one parked thread per `WaitTimer_posix` (5),
  `env_get` precedence, gamesave fsync, `xapu_chad` mmap not in os's VM table (6).
- Results after the fixes (`tests/run_all.sh`, now 40 entries incl. `real_core` and `link_check`): arm64 native 40/40 PASS;
  x86_64 under Rosetta, one process at a time with a kill timeout, 40/40 PASS. Both trees rebuilt with 0 FAILED steps.
  x86_64 `gdk_link_check`: `gdk_register: 0`, CoCreateInstance S_OK, 10 imports resolved. arm64: `gdk_register: -6`
  (E_UNSUPPORTED, unchanged), CoCreateInstance S_OK. Not run: the old-code negative runs (the reviewer's probe measured the
  failures), the 45-run threading repeats, `compat_abi_test`.


## GameInput on GameController (this session, 2026-11-26)

Replaces `gameinput_no_sdl` (a scripted pad or an identity-only object) with a device hub over Apple's GameController
framework. System frameworks only (Foundation, AppKit, GameController, CoreHaptics), so it builds and runs for x86_64 under
Rosetta and for arm64 from the same source. The shim table, the ordinal alias `gameinput.dll!#1` and `GameInputCreate`
are unchanged. Origin of the COM layer: leonida `runtime/shims/gameinput_no_sdl.cpp` (GPL-3.0-or-later), generalised
from one pad to a device list; `gameinput_script.h` and `gameinput_abi.h` are leonida's, unchanged except one comment.

Files: `src/gameinput_mac.cpp` (hub, IGameInput/Device/Reading/Dispatcher, scripted pad), `src/gameinput_gc.mm`
(GameController backend: controllers, keyboard, CoreHaptics rumble, battery), `src/gameinput_pad.h` (pure mappings,
no windows.h), `src/gameinput_backend.h` (the Sink/Backend seam and test hooks). CMake: `enable_language(OBJCXX)`,
`-fobjc-arc` on the `.mm`, frameworks AppKit/GameController/CoreHaptics/Foundation (AppKit must be loaded or
`GCKeyboard.coalescedKeyboard` stays nil; measured with a bare tool).

What it serves:
- **Pads.** Every `GCController` with an extended gamepad profile: A/B/X/Y, shoulders, triggers (analog), both sticks (+Y
  up, no deadzone, the title applies its own), stick clicks, d-pad, Menu (`buttonMenu`), View (`buttonOptions`), guide
  (`buttonHome`, only through the guide callback and controller-button 5). Every pad is reported as family Xbox One; vendor
  and product id are guessed from `productCategory` (Xbox, DualSense, DualShock, Switch) or 0.
- **Connect/disconnect.** `GCControllerDidConnect/DisconnectNotification` (main run loop) become device callbacks with the
  status change (`Connected|InputEnabled|OutputEnabled`, from/to `NoStatus`). The same pad coming back (matched by
  vendor/category/twin number) gets its device object and id back; twins get distinct ids.
- **Keyboard as a gamepad** (`GCKeyboard.coalescedKeyboard`): arrows d-pad; Return/Space/keypad Enter A; Backspace B; X, Y;
  Q/E shoulders; Escape Menu; Tab View; W/A/S/D left stick; I/J/K/L right stick; F/G stick clicks; Z/C triggers.
  Opposite keys cancel, diagonals sit on the round gate (0.7071). `GTA6RT_KEYBOARD_PAD`: `auto` (default: connected only
  while no controller is), `on`, `off`.
- **Readings.** Taken when the backend reports a change; per device the last **256** are kept (`kHistoryDepth`), so
  `GetNextReading`/`GetPreviousReading` step through input the title has not read; a reading it holds stays valid; a step
  from a reading that left the history is `GAMEINPUT_E_REFERENCE_READING_TOO_OLD`; a disconnected device is
  `GAMEINPUT_E_DEVICE_DISCONNECTED`. With no device given, the newest reading across connected devices wins.
  Gamepad state, UI navigation (as leonida's table), controller axes (0..1, sticks 0.5 at rest) and buttons (SDL order,
  guide at 5, stick clicks 7/8).
- **Callbacks.** Device (blocking, async and dispatcher enumeration plus live status changes), reading (with analog
  threshold), guide button; `Dispatch`/`OpenWaitHandle`. Guest callbacks run on one long-lived shim thread that may enter the
  guest (or on the title's thread through a dispatcher), never on GameController's queues.
- **Aggregate device** for kind gamepad only.
- **Rumble** (`SetRumbleState`): CoreHaptics continuous events per motor (left/right handle or one combined handle motor, left
  and right trigger where the controller reports those localities), intensity updated as a dynamic parameter, restarted
  before the 30 s event limit; unchanged levels are not re-sent. `GTA6RT_RUMBLE=0` turns it off.
- **Scripted input**: `LEONIDA_INPUT_SCRIPT` unchanged (same parser/timeline/log lines, 300 ms holds, press log
  `LEONIDA_INPUT_LOG`). With the script set, `GTA6RT_INPUT=auto` serves the scripted pad alone (deterministic unattended
  runs); `GTA6RT_INPUT=gc` adds the real devices; the script's reading is taken when the title asks, as before.

Environment: `GTA6RT_INPUT` = `auto` (default) | `gc` | `none` (the identity-only object, every other slot E_NOTIMPL and logged)
| `test` (hub without a backend; unit tests drive `rt::gameinput_mac::sink()`); `GTA6RT_KEYBOARD_PAD`; `GTA6RT_RUMBLE`;
`GTA6RT_INPUT_BACKGROUND=1` (keep receiving controller and keyboard events while another app is in front; off by default);
`LEONIDA_INPUT_SCRIPT`, `LEONIDA_INPUT_LOG`. The first call of every slot of IGameInput, IGameInputDevice, IGameInputReading and
IGameInputDispatcher is logged once (`[rt] gameinput: first call of GetCurrentReading`, `... of IGameInputReading::GetGamepadState`).

Measured against the real title (host built in `build/win-host-x86_64`, MoltenVK x86_64, 20-40 s runs, `LEONIDA_XMEM_32BIT=anywhere
LEONIDA_CONSOLE_PROFILE=series-x` because another worker's xmem change refuses the 32-bit reservation otherwise):
- Superseded by the round 3 census below: the first-session wording "nothing else of IGameInput" only covered the 14 slots that were logged then.
- Scripted pad: `a@10,start@20,a@30` produced `press a@10.0`, `press start@20.0`, `press a@30.0` in the log at those times
  (`run/logs/win-input-hidden.log`), i.e. the title polls readings and the script passthrough works. The old shim read the
  same presses in `win-control-old.log`.
- GameController backend, no script (`run/logs/win-input-gc.log`): `GameController backend started (0 controller(s))`, then the keyboard
  pad connects from the main-thread notification and the title reads readings. **No physical controller was attached; no key was
  pressed** (this machine cannot send keys: `osascript` keystrokes are refused), so button/stick/trigger/rumble paths of the
  GameController backend are exercised only by the synthetic tests, and keyboard events, CoreHaptics rumble and battery have never run
  against real hardware or key presses.
- Cost: `GetCurrentReading + GetGamepadState + Release` 107 ns per call (x86_64 under Rosetta, 200,000 calls, one pad). Nothing was tuned.

Tests (`tests/gameinput_mac_test.cpp`, `tests/gameinput_pad_test.cpp`, probe `tests/gameinput_probe.cpp`): synthetic controller state
through the hub's Sink, a real `GameInputCreate` through the registered table. `gameinput_mac_test` (identity-only), `--input-script`
(the old script cases plus reading callbacks), `--synthetic` (57 checks: connect/disconnect callbacks, twin pads, keyboard stand-in policy,
reading callbacks with threshold, guide callback, 256-deep history and TOO_OLD, held readings, aggregate device, rumble clamping and
de-duplication, reconnect identity, dispatcher). `gameinput_pad_test` (button/stick/trigger/keyboard mapping). arm64 and x86_64 pass.
`gameinput_probe [seconds]` prints devices and readings from the real backend by hand.
Results: x86_64 under Rosetta `tests/run_all.sh` 42 of 42 PASS (`build/gameinput-run-x86_64.log`; one process at a time); arm64 tree builds with 0 failed steps and the four
GameInput tests pass natively (the other arm64 gdk tests were not re-run). Counts: identity-only 9 checks, script 50, synthetic 57, pad mapping 21.

Open: key and controller input verified only synthetically (above); `GCKeyboard` delivers events only while the app is key (or with
`GTA6RT_INPUT_BACKGROUND=1`); the mouse is a right stick since the input round (below); `GCController` motion, touchpad and DualSense adaptive triggers are not read;
arm64 guest registration is still `E_UNSUPPORTED` (module-wide).

## Input round (2026-11-26): mouse as right stick, script alongside live input, scripted-press delivery

Sources: `src/gameinput_gc.mm`, `src/gameinput_mac.cpp`, `src/gameinput_pad.h`, `src/gameinput_backend.h`; tests `tests/gameinput_*`.

- **Mouse as the keyboard pad's right stick and triggers.** Model from leonida (`docs/gameinput.md`, "Keyboard pad"): the stick follows movement, keeps 0.875 per 2 ms and adds
  counts; 2000 counts/s is full deflection (times `GTA6RT_MOUSE_SENSITIVITY`, default 1); a stopped mouse falls to exactly 0 in about 130 ms; up is +Y. The step is time-based
  (`mouse_stick_step`, `gameinput_pad.h`), run by a 5 ms timer on the backend queue only while the stick is non-zero. Left button = right trigger, right button = left trigger, added to
  the keyboard pad's own reading (so keys and mouse combine, clamped). Deltas come from the host window layer (`NSEvent` deltas of the key window, so trackpads work too and they are
  the system's accelerated deltas, not leonida's raw counts) through `gta6rt_gameinput_mouse_motion/buttons/release/active` (C entry points in `gameinput_gc.mm`). Ignored while the keyboard
  pad is not connected (a real controller took over) or with `GTA6RT_MOUSE=off`. Feel (sensitivity) was NOT tuned on a real mouse: no mouse input could be generated here.
- **Script alongside live devices.** `GTA6RT_INPUT=auto` (default) and `gc` now always start the GameController backend; with `LEONIDA_INPUT_SCRIPT` the scripted pad is one more device
  next to the keyboard pad/mouse and real controllers (`[rt] gameinput: scripted pad for LEONIDA_INPUT_SCRIPT (...), alongside the keyboard pad, the mouse and real controllers`).
  `GTA6RT_INPUT=script` restores the scripted pad alone. With no device named, `GetCurrentReading` now prefers a device holding something over idle ones (before: the newest reading, so a
  scripted release hid a held key). Run 1 log: `GetCurrentReading asked for Scripted pad` and `... for Keyboard pad`: the title polls each device by name (logged once per form).
- **Scripted press delivery (glitch 3).** Held 300 ms by the script, and now until the title has asked on 4 separate frames (calls >= 8 ms apart; at most 1.5 s), and a press whose 0.3 s hold
  passed with no ask at all (a long frame) is delivered late (up to 10 s), once, with a log line. `GTA6RT_SCRIPT_MIN_POLLS=0` restores the exact timeline. New log line per press:
  `scripted release at T s: held N ms, asked on F frame(s) (C call(s))`.
  Evidence (`run/logs/input-r4b.log`, cold pipeline cache, machine loaded, title at 0.2-2 fps with stalls up to 6.5 s): the press at 70 s was delivered at 73.9 s, 85 s at 86.9 s, 100 s at 109.7 s,
  115 s at 115.7 s; each was asked on 4 frames. In the earlier run `run/logs/input-r4.log` (4-frame hold, no late delivery) the presses at 70 and 85 s were never read at all (no `press` line): the title asked for no reading during their 0.3 s holds.
  The reported glitch (A at +100 s on UI TEXT SIZE read, page unchanged, `s2-firstrun.log`) was NOT reproduced: neither new run reached the first-run pages in time. What the old log shows: the press was read
  (logged) at 100.0 s at about 16 frames/s, so it was seen on about 5 frames; the shim did not drop it. The cause is not established. Hypothesis, unproven: the page was not taking input at that moment.
  If it recurs the new release line says how many frames saw the press.
- Tests (x86_64, own tree `build/input-x86_64`): `gameinput_pad_test` 0 failures (mouse step: full at 2000/s, half at half speed, clamp, stop in 40-80 steps of 2 ms, sign, sensitivity, trigger mapping),
  `gameinput_mac_test` `--identity`, `--input-script`, `--synthetic`, `--census`, `--sticky` (4-frame hold), `--late` (late delivery, once, dropped after 10 s), `--alongside` (live pad + script, non-idle wins): all 0 failures.
- Not verified: mouse movement and buttons reaching the title (no way to inject input here: no Accessibility), the backend with a physical controller, arm64.

### Scripted-input run recipe (unattended A/Start at timed points)

Verified on the real title in this session (`run/logs/win-input-hidden.log`: `a@10,start@20,a@30` produced `press a@10.0`, `press start@20.0`,
`press a@30.0`; the title polls `GetCurrentReading` from its frame loop).

```sh
cd port
LEONIDA_INPUT_SCRIPT="a@40,a@60,start@90" \
  host/tools/run.sh 150 first-run --watchdog 140 --hidden-window \
  --vk-driver $PWD/../spike/s1-vulkan-x86/drivers/moltenvk-1.4.2/libMoltenVK.x86_64.dylib
grep 'gameinput:' run/logs/first-run.log
```

- Seconds are counted from the first `GameInputCreate` (not from process start; it happens once the title is past WinMain, so the press appears later than the same number of seconds after launch).
- Entries are `button@seconds`, comma separated, case-insensitive: `a b x y start back lb rb up down left right`, and `rsup`/`rsdown` (right stick thrown fully up/down). Each press is held 300 ms
  (about 12 title frames at the measured 42 frames/s); for a longer hold repeat the entry at 0.3 s steps (`a@40,a@40.3,a@40.6`). Rejected entries are counted in the startup line.
- With the script set the scripted pad is the only device (`GTA6RT_INPUT=auto`); `GTA6RT_INPUT=gc` adds the keyboard pad and any controller. `LEONIDA_INPUT_LOG=0` silences the press lines.
- A press is logged when the title reads it: `scripted press of button <n> at <t> s` (n = SDL button number: a=0, start=6) then `press <name>@<t>`. A missing line means the title was not polling at that time.
- Under the current tree the title only runs with `LEONIDA_XMEM_32BIT=anywhere` (xmem, another worker) and `LEONIDA_CONSOLE_PROFILE=series-x` for the 3840x2160 window; the recipe itself needs neither.

## Review response (round 3)

Findings B1, B2 and F1 of `host/src/win/REVIEW-round3-input-window.md`; both blockers were correct.

**B1 (silent unserved slots) and B2 (census blind to 5 slots): fixed in code, not in wording.** `first_call()` is replaced by a
per-call-site macro (`GI_CALLED(name)` / `GI_UNSERVED(name, result)`, a relaxed atomic load on the hot path; `gameinput_mac.cpp`).
All 62 slots of the four interfaces now carry one (the review listed 5 IGameInput slots; the Device and Reading slots had none either).
A served slot logs `first call of X`; an unserved one logs `first call of X: not served, returns R (logged once)`.
Behaviour changes: `EnableOemDeviceSupport` returned S_OK for work it does not do and now returns E_NOTIMPL; `RegisterKeyboardLayoutCallback`
keeps S_OK but is now logged in the same format. Names: IGameInput slots unprefixed, the others `IGameInputDevice::`, `IGameInputReading::`,
`IGameInputDispatcher::` (`SetRumbleState` is now `IGameInputDevice::SetRumbleState`).

Tests: `gameinput_mac_test --census` (new; `gdk_gameinput_mac_census` in CMake, `run_all.sh`) captures fd 2, calls every slot of the four
interfaces on a connected synthetic pad, and requires each of the 62 names exactly once (34 as "not served", 28 as served), the exact
count of "not served" lines, the failure code or answer of each unserved slot, and nothing new logged by a second round.
Result: 39 checks, 0 failures, x86_64 (Rosetta) and arm64. `tests/run_all.sh` on x86_64: 43 of 43 PASS
(`build/gameinput-r3fix-run_all-x86_64.log`). Existing counts unchanged: `--synthetic`, `--input-script`, identity-only and `gameinput_pad_test` all 0 failures.

Real title (`run/logs/r3fix-hidden.log`; `build/win-host-x86_64` rebuilt with the fix; `LEONIDA_INPUT_SCRIPT="a@8,start@14,a@20"`, `--hidden-window`,
MoltenVK x86_64, killed at its 40 s watchdog): the complete census of the title's IGameInput use is the five slots
`RegisterDeviceCallback`, `IGameInputDevice::GetDeviceInfo`, `GetCurrentReading`, `IGameInputReading::GetGamepadState`,
`IGameInputDevice::SetRumbleState`; **no "not served" line appeared**. Presses logged at 8.0, 14.1 and 20.1 s. The earlier statement is corrected: the title calls
those five of 62 slots (GetDeviceInfo and GetGamepadState were invisible before). The run covers one title path (boot to the first-run page, 3 scripted presses);
a later page may call other slots and would now say so in the log.

Cost: `GetCurrentReading+GetGamepadState+Release` under load average about 16 (so absolute numbers are inflated), same binary pair A/B,
5 alternating runs each, x86_64: old source 212-262 ns, new source 202-215 ns (the old `first_call` took a mutex and a linear scan per call; the macro does not). arm64: 78 ns. No regression.

**F1 (rumble through a dangling reference): fixed** in `gameinput_gc.mm` (`set_rumble` copies the levels before the `dispatch_async` block). Evidence: a probe
(arm64, -O2, ARC) with the same shape printed `4.46301e-37 1.4013e-45` for the reference capture and `0.25 0.75` for the copy. The real haptic path is still
unexercised (no hardware), so this is shown on the probe, not on a controller.

Not changed (not blocking, recorded): F2 (CoreHaptics handler race) and F3 (duplicate pad at startup, `keyboard_ok_` unsynchronised) are still open and unexercised without a controller.
F4: `GCController.shouldMonitorBackgroundEvents` is off by default, so controller input as well as keyboard input stops while the app is not frontmost; in a `--hidden-window`
run without a script no real input arrives unless `GTA6RT_INPUT_BACKGROUND=1`. F5 (for the integrator, next to the hidden-window notes): `--hidden-window` also turns off the package splash.

## DirectStorage backend (2026-11-26: io backend applied, request tracing, BCPack profile wiring)

Source of the design and every measurement: `port/io_bench/STATUS.md` (sections 1-10 as reviewed, section 11 for this round).

**Applied.** `io_bench/proposals/dstorage_io_backend.patch` to `src/dstorage.cpp`, `src/dstorage_staging.h`, `CMakeLists.txt` (`find_package(ZLIB)`, `ZLIB::ZLIB`) and `shims_core/src/file_system.cpp` (the `gta6rt.internal!FileNativeFd` row, 6 lines).
The sources had not changed since the review (md5 checked), the patch applied clean. What it does: a prepare pool shared by all queues (1.5 x cores threads, 8..32) reads and inflates ahead; each queue keeps a window of 32 slots and publishes, signals and notifies in order on its own thread;
staging is taken in queue order without waiting (`StagingPool::Lease::try_take`), `StagingPool::limit` is 256 MiB (was 64); reads are `pread` on the native descriptor with `F_NOCACHE`; inflate tries the system zlib first and repeats a failure on `leonida::zlib_inflate`, so every typed error is unchanged;
`CancelRequestsWithTag` also marks window slots that have not started.

**Added in this round (same files): request tracing, a pool QoS knob, a clearer missing-profile message.**
- `src/dstorage_shapes.h`: `LEONIDA_DSTORAGE_SHAPES=<file>` (or `1` for stderr) logs, off by default, one tab-separated record per queue creation, request (shape, destination kind, enqueuing thread, outstanding count), submit, completion (latency, read and decode time, pool or inline), cancel and queue close. Record layout is in the header. `LEONIDA_DSTORAGE_TRACE=1` is unchanged. Summary and replay-list writer: `port/io_bench/scripts/dstorage_shapes.py`; a whole title run with tracing: `port/io_bench/scripts/capture_title_shapes.sh`.
- `LEONIDA_DSTORAGE_POOL_QOS=utility|initiated|background` sets the pool threads' QoS class (default: unset). Measured: utility starves the pool on a busy machine (io_bench 11.7); experiments only.
- The BCPack profile loader is unchanged (`<shader folder parent>/bcpack/history.bcph`, i.e. `port/build/shaders_vk/apple/bcpack/history.bcph` for the host's default corpus); when the file or the shader folder is missing the log line now names the path and `port/io_bench/scripts/ensure_bcpack_history.sh`.

**Knobs** (environment, defaults): `LEONIDA_DSTORAGE_POOL` (1.5 x cores clamped 8..32; `0`, or the old `LEONIDA_DSTORAGE_WORKERS=0/1`, makes every queue serial), `_DEPTH` (32, 1..128), `_NOCACHE` (1), `_RDAHEAD` (-1 = leave), `_ADVISE` (0), `_SYSTEM_ZLIB` (1), `_READ_CHUNK_KIB` (1024 under x86_64, 0 = no cap on arm64: under Rosetta `F_NOCACHE` holds only for the first MiB of one `pread`, so every native-fd read is split at 1 MiB; measured in io_bench 11.7b, no throughput cost), `_POOL_QOS`, `_SHAPES`, `_TRACE`, `_HISTORY`, `_FITTED`, `_FAST_INFLATE`, `_FAST_BCPACK`, `_EXTENDED`.

**Tests** (new, `CMakeLists.txt`): `dstorage_native_fd_test` (`tests/dstorage_test.cpp` with `tests/dstorage_native_fd_row.cpp`, so the native-descriptor path runs: 16 descriptors), `dstorage_pool_stress_test` and `dstorage_pool_stress_native_fd_test` (`tests/dstorage_pool_stress_test.cpp`: a status entry every 5 requests with the bytes compared the moment it completes, 4 queues on one file, cancel before and after Submit), ctest names `gdk_dstorage_native_fd`, `gdk_dstorage_pool_stress`, `gdk_dstorage_pool_stress_native_fd`; `gdk_dstorage_real_core_test` (`tests/dstorage_real_core_test.cpp`, links the real shims_core: the `FileNativeFd` row, zlib / raw / failing requests over its handles, and that a 4 MiB DirectStorage read leaves the range out of the page cache with a plain-`pread` control; 21 checks, 0 failures on both architectures; it caught the Rosetta `F_NOCACHE` limit above), ctest `gdk_dstorage_real_core`.
Results, own trees `build/dstorage-x86_64` and `build/dstorage-arm64` (modules `os;isa;seh;loader;shims_core;shims_gdk`), one process at a time, x86_64 under a kill timeout: `dstorage_test` 489 checks / 0 failures with the unpatched backend, the live backend without the row and with it, on both architectures; the stress programs 0 failures on both; 25 repeats of the stress test 0 failures (arm64). `./verify.sh s1` passes with the host built from the applied tree.
Not run: the other shims_gdk programs (`tests/run_all.sh`) against the applied tree.

**Measured** (real shim through `tests/dstorage_replay_bench.cpp` in io_bench, baseline = the unpatched backend kept in `io_bench/baseline/`; 3 queues, MiB/s out; load average 10-38):
arm64 levels 536 -> 2,594 (4.8x), core_bvh4 2,202 -> 7,945 (3.6x), TTPF 599 -> 3,171 (5.3x); x86_64 (Rosetta) 474 -> 1,802 (3.8x), 1,958 -> 5,952 (3.0x), 576 -> 2,765 (4.8x). On the 72,581 zlib and raw requests the title made in its first 110 s, 17 queues: x86_64 546 -> 2,104 MiB/s (3.9x), arm64 1,214 -> 2,652 (2.2x).
Real title (hidden window, 105-119 s before it aborted, see below), same title phase with and without the pool: enqueue-to-publish latency p50 25 ms / p99 269 ms with the applied defaults against 55 ms / 1,646 ms with `POOL=0 NOCACHE=0 SYSTEM_ZLIB=0`; the title's own progress was not visibly different (its boot is not waiting on DStorage yet).

**What the title asks for** (first trace, `port/io_bench/results/title_dstorage-shapes1_shapes.tsv`): 17 file-source queues of capacity 4159, REALTIME, auto-submit off; 74,330 requests in 115 s (zlib 83%, raw 15%, BCPack 2%, all BCPack to page lists), stored p50 4.4 KiB; `levels.rpf`, `audio.rpf`, `anim.rpf`, `shaders.rpf` dominate; depth up to 3,676 outstanding; 4 enqueuing threads. Details and caveats: io_bench 11.5.

**BCPack profile.** With `history.bcph` present every BC7 and BC6H request the title made in its first 110 s decoded (153 BC7, 1 BC6H, no failures); over all 5,435 BC7/BC6H chunks in the 57 pools 70 still fail (BC7 53 `missing-endpoint` + 7 `anchor-or-pbit` of 5,167; BC6H 10 `missing-endpoint` of 268), every success is `fitted`. Decode: BC7 26-28 MiB/s per thread arm64, 22-23 under Rosetta, 115-211 MiB/s on 14 threads. io_bench 11.4.

**Open / found.** (1) Both traced title runs aborted at 107-119 s with a Metal assertion (BC1 view of a BC7 texture) in the d3d12_vulkan layer; same with the old serial backend (io_bench 11.6). (2) No host script runs `ensure_bcpack_history.sh` yet. (3) Page-list publishing is serial per queue and was not measured alone. (4) `CancelRequestsWithTag` writes `entry.cancelled` of a prefetched slot while a pool thread may read the entry (benign bool race, request dropped at publish either way).

## Input script: sticks, triggers, keys and hold times (round 11, play worker, 2026-11-27)

`src/gameinput_script.h` + `Hub::refresh_script` (`gameinput_mac.cpp`): an entry is `name@seconds[:hold]` (hold 0.3 s when
none is given, at most 600 s). New names: `lsup lsdown lsleft lsright rsleft rsright` (with the old `rsup rsdown`) throw
a stick fully, `lt rt` pull a trigger, `key_<k>` holds a keyboard key through `pad_state_from_keys` (the keyboard pad's
own mapping; k = a letter, return, space, escape/esc, backspace, tab, arrowup/down/left/right). Old scripts parse and
play the same. Each stick/key entry logs `[rt] gameinput: scripted lsup@100.0 held from 100.1 s for 4.0 s` once.
Test: `gameinput_mac_test --axes` (ctest `gdk_gameinput_mac_axes`): 25 checks, 0 failed; the other seven modes and
`gameinput_pad_test` unchanged at 0 failures (tree `build/play-x86_64`). In-title results: `spike/r10-play/STATUS.md`.
