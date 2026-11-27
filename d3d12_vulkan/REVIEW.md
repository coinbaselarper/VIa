# S1.6 d3d12_vulkan review

Reviewer: independent. Date: 2026-11-26. Verdict: **APPROVED_WITH_FIXES**. Nothing blocking: the report's claims hold when re-run, and I found no defect that crashes or corrupts the guest on the boot path. The one hard stop for the title (the root-signature serializer) is stated by the builder and is real.

## What I rebuilt and re-ran (measured)

Private trees `port/build/review-d3d12-vk-{arm64,x86_64}` (modules os;isa;shims_core;d3d12_vulkan) and `port/build/review-d3d12-vk-all-{arm64,x86_64}` (full tree, target `gta6rt`). All were configured from empty directories, `-j3`.

| check | result |
|---|---|
| module build, arm64 | 109 steps, 0 errors, 36 warnings (matches the report) |
| module build, x86_64 | 115 steps, 0 errors, 36 warnings (matches) |
| warnings by kind (x86_64) | 24 `%lu` vs 32-bit `DWORD`/`uint32_t`, 8 unhandled switch values, 2 `__stdcall` ignored in `shims.h` (see note 6), 1 `DXGI_FORMAT` case, 1 CoreGraphics enum mix. Matches "leonida code", plus platform/ noise |
| `copy_streaming` object code | `otool -tv`: x86_64 object has 4 `movntdqa`, arm64 object has 2 `ldnp`. Matches |
| `d3d12vk_present_clear_test`, arm64, KosmicKrisp (Homebrew) | rc 0, splash 230305/230400 px lit, 121 presented, readback format 44, every pixel = (51,153,230) +-1, DXGI 120 frames |
| same, arm64, MoltenVK (Homebrew) | rc 0, same output |
| same, arm64, driver unset | rc 0, picked KosmicKrisp |
| same, x86_64 Rosetta, `spike/s1-vulkan-x86/install/kk-x86_64` KosmicKrisp | rc 0, all phases PASS |
| same, x86_64 Rosetta, `spike/s1-vulkan-x86/vulkansdk` MoltenVK | rc 0, all phases PASS |
| same, x86_64, driver unset | rc 77, "incompatible architecture (have 'arm64', need 'x86_64')" logged, as reported |
| `d3d12vk_register_test` x86_64 / arm64 | PASS both; x86_64 resolves 6 named rows, upper-case DLL name, ordinal 7 |
| full tree `host/gta6rt`, both arches | links; rc 0; `d3d12_vulkan_register=ok` (x86_64), `unsupported` (arm64) |
| extra: layer-only (hidden) window variant of the test (my own copy in scratch, window `visible=0`), arm64 KosmicKrisp | rc 0, presenter and DXGI phases PASS. The builder never ran this path |

x86_64 processes were launched one at a time under a kill timer. No hang.

Other facts verified by reading and diffing:
- ORIGIN.md matches reality: exactly the 14 files it lists differ from `leonida/runtime`; the rest of `shims/` and `src/` (including `xbox_present.cpp`) are byte-identical; `gen_d3d12_stubs.py` and `report_paths.py` are identical.
- DXGI vtable order: I extracted the method order of every interface in `include/dxgi*.h` and compared with the real DXGI order (IDXGIObject through IDXGIFactory7, 40 interfaces). All match. This matters because the test uses the same header on both sides and could not catch an ordering mistake. Spot-checked 9 IIDs against known values: all correct.
- Struct-return (MSVC RetVal) form: 21 methods rewritten in `d3d12.h`; the 12 that have stub bases are all overridden by real classes, so the generated stub bodies (which return `{}` without writing `*RetVal`) are never reached for them.
- Ms_abi handling: guest-facing exports and vtable slots are `WINAPI`/`STDMETHODCALLTYPE`; the `unsigned long` returns in `d3d12_xs_device.cpp` are host helpers or return-only (the caller reads EAX). No `unsigned long`/`long` parameter in a guest-visible signature.
- QPC in `win32_host.cpp` is the same clock as os/ and shims_core (`mach_absolute_time` ns / 100, 10 MHz), so calibrated timestamps are consistent.

## Not re-run, or not provable by the tests

- No shader, PSO, draw, PresentX, guest thunk call, resize, validation layer. The builder lists all of these as not tested; that is accurate.
- The "readback" is the presenter's own capture copy of each swapchain image, not the screen. The report says so.
- The DXGI phase checks only that calls succeed; it does no pixel check.
- The register test resolves thunks but never calls through one.
- The IID check (`tools/check_dxgi_iids.py`) tests that the 16 GUID bytes occur anywhere in the reference binaries, so a name-to-GUID swap would pass. I spot-checked 9 and the vtable order by hand (above); the "40 match" claim is true as far as it goes, but is weaker than "match" suggests.

## Findings (non-blocking)

Ordered by how likely they are to matter.

1. **PIX event labels are decoded as UTF-32.** `shims/d3d12_com.cpp` `event_label`, metadata 0 (the title's UTF-16 string): it reads the data as `const wchar_t*` (4 bytes on macOS) and uses `size / sizeof(wchar_t)`. UTF-16 `"AB\0"` becomes code points above U+10FFFF and `append_utf8` emits invalid UTF-8; the scan also runs past the terminator inside `size`. Memory-safe (bounded by `size`), but every SetMarker/BeginEvent with metadata 0 gives a garbled label. ORIGIN.md does not list `d3d12_com.cpp` as changed for this. Fix: read `const char16_t*`/`WCHAR*` and convert with `rthost::to_utf8(const char16_t*)`. This is the only guest-supplied wide string I found left on `wchar_t` (SetName, pipeline library names and DXGI descriptions are correct).
2. **Root-signature entry points leave out-parameters untouched on `E_NOTIMPL`.** In `shims/d3d12_xs.cpp` the 4 functions return `E_NOTIMPL` from `host()` failing without setting `*blob`, `*error` or `*out`. The real API nulls them. A guest that does not pre-initialise them and reads `error` after a failure would read garbage. Cheap fix: null them before returning. The builder is right that the title cannot proceed past this stub anyway; a silent success is not possible here (the HRESULT is a failure and `host()` logs).
3. **Fallback window race leaves a stray window.** `metal_window.mm`: when a visible create from another thread times out (5 s), the queued block still runs on the main queue later and builds a real, visible NSWindow on the orphaned struct. The code comment says the block "must not touch this window", but it creates a second window that nothing tracks or destroys. Reproduced in a scratch program with a 300 ms wait: the registry window stays `visible=0`, and after the main queue is serviced `[NSApp windows].count` is 1. Fix: let the block check a cancelled flag, or make the struct shared.
4. **Possible use after free on window destroy.** `gta6rt_mac_window_destroy` deletes the struct, then closes the NSWindow asynchronously on the main queue. `Gta6rtMetalView.owner` and the delegate's `owner` are `assign` pointers to the deleted struct; `updateDrawableSize` writes through it. ASan on a 3x create/destroy loop did not trigger it, so this is latent, not reproduced. Also `alive()` then dereference is a time-of-check/time-of-use race against a concurrent destroy (for example the present thread vs `~PresentState`). Fix: clear the owner pointers on the main thread before deleting, or reference-count the window.
5. **STATUS "every one logs or returns a defined failure" is slightly too strong.** Silent no-ops/approximations not listed: `SetThreadPriority` returns TRUE; `SleepEx`/`WaitForSingleObjectEx` ignore `alertable`; `WaitForMultipleObjects` is a 1 ms poll and `wait_all` consumes each auto-reset object as it signals (the code comment says so, STATUS does not); `SetThreadpoolWait` ignores an absolute timeout; `CreateThread(CREATE_SUSPENDED)` starts at once (logged); `FindFirstFileA` always reports not found. None is on the measured boot path, but they belong in the list. `CoInitializeEx` is listed as a no-op already.
6. **`shims.h` is declared twice with different types (ODR).** `d3d12_vulkan/shims/shims.h` has `XMemAllocHookFn = void*(__stdcall*)(...)` (ms_abi in leonida TUs); `shims_core/src/shims.h` has a plain function pointer for the same `rt::shims::XMemAllocationHooks`. Layout is identical and only this module calls the hooks (with ms_abi, which is right for title code), so nothing breaks today. `platform/register.cpp` is compiled without the force-include and so ignores `__stdcall` (the 2 warnings), harmlessly. Worth reconciling when host/ registers the hooks.
7. **Closing the PresentX window no longer ends the run.** leonida's fallback window terminated the process on WM_CLOSE; here `close_requested` is set and nothing reads it. The code comment says the host decides. host/ must poll `gta6rt_mac_window_close_requested`.
8. **Device LUID is 0** (KosmicKrisp reports none); `GetAdapterLuid` returns 0 silently apart from one log line. Anything in the title that keys on a LUID (`EnumAdapterByLuid`) gets a zero LUID.

## Claims in the report / STATUS / ORIGIN

- All numeric and pass/fail claims checked above are true. "4 of 4 runs" is accurate; there is a 5th PASS (arm64 default order) and the x86_64 default skip, both disclosed.
- No false claim found. The wording points to tighten are finding 5 and the IID check (see "Not re-run").
- Stubs are listed honestly where they matter: root-signature (blocks the title), registration on arm64, vectored exception handlers, guarded copies, SDR white level, HUD VRAM, `LoadLibrary`, tokens.

## For the lead / integrator

- x86_64 default driver search finds only arm64 Homebrew dylibs and exits 77. The shipped x86_64 build needs an x86_64 driver next to the executable or in `../Frameworks`, or `GTA6RT_VK_DRIVER`. Both spike-built x86_64 drivers work by path.
- host/ needs the AppKit main thread (services the main dispatch queue, calls `gta6rt_mac_window_pump_events`, and decides what a close request does).
- Root-signature serializer (inverse of `parse_rts0`) still needs an owner; it gates every root signature the title builds.
- Private build trees `port/build/review-d3d12-vk-*` and a scratch hidden-window variant are left on disk (scratch is under the session scratchpad). Nothing outside this file was changed.
