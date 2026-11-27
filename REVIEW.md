# S1 integration review (2026-11-26)

Reviewer: independent. Scope: `port/STATUS.md` and the integrator's report. I rebuilt the x86_64 tree from scratch into a separate directory, ran the new binary against the real exe myself, and checked the three things the task named. Nothing outside `port/build/review-host-x86_64*`, `port/run/logs/review-*.log` and this file was written. (The runs also write under `port/run/overlay` as the title does; the package was not touched.)

## Verdict: APPROVED WITH FIXES

The headline claims reproduce: the Mach-O x86_64 host maps the exe at 0x140000000, reaches WinMain with 36,739 exe import calls, and with MoltenVK runs the title's frame loop to the 120 s watchdog with 0 unimplemented imports called. KosmicKrisp stops at the A8_UNORM render target as reported. Four things need fixing or restating before this is relied on. They are listed under "Fixes".

## What I built

```
cmake -S port -B port/build/review-host-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C port/build/review-host-x86_64 -j3          # whole tree, not only gta6rt
```

- Configure and build: rc 0, 217 of 217 steps (`port/build/review-host-x86_64-configure.log`, `-build.log`).
- `file build/review-host-x86_64/gta6rt` reports `Mach-O 64-bit executable x86_64`. Load commands: `__PAGEZERO` vmsize 0x100000000, `LC_MAIN` stacksize 8388608. Both match STATUS.md.
- `file build/macos-arm64/gta6rt` reports `Mach-O 64-bit executable arm64`. I did not rebuild arm64; that is a file(1) check on the integrator's existing binary only.
- The fresh binary is 8,579,472 bytes against 8,578,208 for `build/macos/gta6rt`. The bytes differ (build-path strings, presumably); I did not diff further.
- Warnings: 47 `warning:` lines. The integrator said the only warnings were `-no_pie`, duplicate libraries and `__stdcall`. That is incomplete. The build also has 24 `-Wformat` warnings and 8 `-Wswitch` warnings (`dispatch_mesh` and `invalid_predication` not handled in a switch, one DXGI_FORMAT case, one CoreGraphics enum). They are all in `d3d12_vulkan` sources and also appear in the older incremental log `build/macos-build.log`, so they are not new regressions. None is in `crt_extra`, `rts0_serializer`, `zen_insn` or `window_shim`. The `dispatch_mesh` switch warning is a reminder that DISPATCH_MESH is skipped, not handled.

## What I ran (x86_64, one process at a time, under `host/tools/run.sh` with `GTA6RT_BIN` set to the new binary)

| run | command (after `run.sh SECS NAME`) | result | log |
|---|---|---|---|
| WinMain | `--trace=0 --stop-at-winmain --hidden-window` | rc 0, ~1.2 s wall. `=== REACHED MARKER: RVA 0x5a91bd0`, `args: rcx=0x140000000 rdx=0 ... r9=0xa`, `calls: 37036 total, 36739 from the exe, 84 distinct imports called (0 unimplemented), 765 wrapped`. Exe mapped at its preferred base; `zen_nop=12 clzero_loops=10` | `review-winmain.log` |
| WinMain, full trace | `--trace --stop-at-winmain --hidden-window` | identical counts. I read the ordered list of the 84 distinct imports: none is unimplemented | `review-winmain-trace.log` |
| WinMain, relocated | `--trace=0 --stop-at-winmain --hidden-window --force-reloc` | exe at 0x300000000, rcx=0x300000000, same 36,739 / 84 / 0 | `review-winmain-reloc.log` |
| KosmicKrisp (default driver) | `--trace=0 --watchdog 120` | rc 134 after 3 s. `A8_UNORM render targets need VK_FORMAT_A8_UNORM_KHR`, `GetResourceAllocationInfo refused: dim 3 format 65 3840x2160`, then SEH FATAL `EXCEPTION_BREAKPOINT` at exe+0x344196. 149,606 calls (integrator: 149,495; the count varies a little between runs) | `review-kosmickrisp.log` |
| MoltenVK x86_64, run 1 | `--trace --watchdog 120 --hidden-window --vk-driver .../libMoltenVK.x86_64.dylib` | **did not reach the watchdog.** rc 134 after 89 s, 111,391,512 calls. See "Fixes" 1 | `review-moltenvk.log` |
| MoltenVK x86_64, run 2 (same command, after a concurrent job on this machine ended) | same | rc 124 at the 120 s watchdog. `calls: 136,410,606 total, 136,362,111 from the exe, 286 distinct imports called (0 unimplemented)`, `=== 0 call(s) reached unimplemented imports; 0 isa traps`. 35 to 42 submitted frames/s; 8,181 submissions in all | `review-moltenvk2.log` |

Furthest point reached, with evidence:
- **With KosmicKrisp:** window created, D3D12 device on Vulkan, root signatures and PSOs, then the title's own `int3` at exe+0x344196 after the layer refused to size the A8_UNORM target. 149.6 k import calls.
- **With MoltenVK:** the title's frame loop (PeekMessageA, QueryPerformanceCounter, WaitForSingleObject, ExecuteCommandLists, PresentX) for the full 120 s in run 2. All of the 286 imports are served.
- **Nothing drawn:** not checked by eye (I ran hidden). What the log shows is that every PSO creation fails: `d3d12_xs: slot 67 ... failed with 0xffffffff80004005` 2,689 times and slot 57 `0x80070057` 16 times, plus "no translated graphics stage in fspx_gfx" lines. This was run without `LEONIDA_SHADERS`, so that is expected. I did not re-run the S2.4 shader case (1,085 MoltenVK vertex-pipeline refusals).
- **Sleep spin confirmed:** `Sleep` is 124,522,889 of 136,410,606 calls in run 2 (91 percent), all from the exe.

## Fixes

1. **"MoltenVK: no crash until the 120 s watchdog" is not unconditional.** Run 1 ended at 89 s: `present failed (VkResult -4)`, `vkWaitSemaphores ... VK_ERROR_DEVICE_LOST`, MoltenVK `Lost VkDevice after MTLCommandBuffer ... Discarded (victim of GPU error/recovery) (kIOGPUCommandBufferCallbackErrorInnocentVictim)`, then the title's fail-fast `int3` at exe+0x2474606 (rsi=0x887a0005, DXGI_ERROR_DEVICE_REMOVED). "Innocent victim" means the Metal command buffer was discarded because of a GPU reset caused elsewhere. At that moment another worker's `shaderc_msl/bin/vs_lane_test` and `MTLCompilerService` were running on this machine, which fits but does not prove the cause. Run 2, started after that job exited, ran clean. So: one clean run of two in my review, and the integrator's own runs were clean. Restate the claim as "ran 120 s on an otherwise idle GPU; a device loss from outside kills the title with a fail-fast". The host has no device-loss recovery. That is expected, but record it.
2. **Unimplemented-import policy defaults to returning 0.** `shims_core/src/registry.cpp` `unimplemented_frame`: with the default `LEONIDA_UNIMPLEMENTED=log`, a call into an unserved import returns 0 (`rax=0`, `xmm0=0`) with one WARN line and a counter. For an HRESULT-class import 0 is S_OK, and out-parameters are left unwritten. The host does not set the variable. On the paths I ran this never triggered: 0 unimplemented called in all runs, and no `unimplemented:` or `STOP:` line in any log. The risk is the 12 imports bound to unimplemented stubs (`mfplat!MFStartup/MFCreateAttributes/...`, `mfreadwrite!MFCreateSourceReaderFromMediaSource`, four `crypt32` Cert*, `xaudio2_9!#1`). The title will call them when movies or audio start, and `MFStartup` and `MFCreateAttributes` would then report success with garbage out-pointers. Set the host's default to `fail` (E_NOTIMPL) or `stop`, and keep the "0 unimplemented" check in verify. The integrator's own report line that these were "never called" is correct for what I ran.
3. **`verify.sh` as it stands cannot pass on the real binary.** It greps `file` output for `Mach-O 64-bit x86_64`; the real text is `Mach-O 64-bit executable x86_64`, so even with the path fixed, line 44 would print FAIL. It also reads `build/macos/gta6rt` at the project root (the binary is under `port/`) and `spike/s1-trace.log` (nothing produces it). I did not edit verify.sh (read-only for me). The proposed check in STATUS.md has the correct `executable` pattern.
4. **The proposed `verify.sh s1` check distinguishes success from failure, but needs three hardening changes.**
   - **Tested:** I applied the three greps to 12 logs. They pass on `review-winmain`, `review-winmain-trace`, `s1-winmain` and `r04-winmain` (all WinMain runs). They fail on `review-kosmickrisp`, `review-moltenvk` (device-lost run), `s1-moltenvk` (ran past WinMain without the marker), `s1-maponly`, `r02-winmain` (stopped before WinMain), `r05-past`, `arm64-maponly`, and on the relocated run (`rcx=0x300000000`, as intended).
   - **Stale-log false pass:** `run.sh` returns 125 before it opens the log when any `build/macos/gta6rt` is running, so an old `verify-s1.log` would still satisfy the greps. The check must `rm -f` the log first and require `run.sh` rc 0.
   - **Exact count `36739`:** this is brittle but real: the same count came from 7 runs. The looser form `3673[0-9]` in STATUS.md is fine. The "0 unimplemented" part is the one that matters.
   - **Guard string:** `run.sh`'s `pgrep -f build/macos/gta6rt` only guards that exact path. A `GTA6RT_BIN` elsewhere (as I used) is not guarded. Harmless for verify, but a second parallel x86_64 launch would not be refused.

## No module silently returns success for unimplemented imports: findings

- **Imports:** the 84 imports on the WinMain path and the 286 on the frame loop path are all served. The trace and the shim report both read 0 unimplemented, and the log has no `unimplemented:` warning. Policy caveat in Fix 2.
- **Served stubs that return success, by design and logged** (not "unimplemented", but a reader should know): ETW `EventRegister`/`EventWriteTransfer` accepted with no session; `SetThreadAffinityMask`/`SetThreadGroupAffinity` (8 each) answered with the mask; xgameruntime offline user, privilege checks granted, installation monitor, speech "silent offline", HDR off; `custom sampler border colours without VK_EXT_custom_border_color; transparent black instead` (8 lines). Each prints a log line saying what it did. `LoadLibrary(dbghelp.dll)` and `LoadLibrary(rpcrt4.dll)` return not found, not a fake handle.
- **D3D12 layer:** unsupported work fails loudly. PSO creation returns E_FAIL (2,689 times) and `ExecuteCommandLists` in the shader run returned "unsupported; nothing was submitted" per the integrator's log. I did not find a success return on a path the logs show failing.
- **Gaps in the measure:** the "unimplemented" counter covers imports only. It does not cover COM vtable slots that forward or no-op (`device slot N (forwarded) first call`, `queue slot N (past the public chain) first call` appear in the log), and DISPATCH_MESH records are skipped silently by design. The fatal-signal dump prints the last 30 calls but not the unimplemented count, so a crash run cannot be used to show "0 unimplemented"; only the marker and watchdog paths print it.

## Not verified by me

- No rendered frame was inspected; "nothing drawn" is inferred from the failed PSO creations, not seen.
- I did not re-run the unit tests (`shims_core_test`, `isa_unit_test`, `isa_rosetta_test`, `d3d12vk_register_test`) or build arm64. They are the integrator's numbers.
- I did not code-review `crt_extra.cpp` (61 rows), `rts0_serializer.cpp`, `zen_insn.cpp`, `xmem.cpp` placeholder split/coalesce or `window_shim.cpp` beyond the unimplemented-import path. Only the title runs exercise them, and the integrator says so. `rts0_serializer` has no unit test.
- I did not run with `LEONIDA_SHADERS`, with a visible window, or with `LEONIDA_UNIMPLEMENTED=stop`.
- Call counts vary between runs (KosmicKrisp 149,495 vs 149,606; MoltenVK 156 M vs 136 M in 120 s). Treat them as order-of-magnitude, not as pass criteria. Only the WinMain counts (36,739 / 84 / 0) were identical in every run.

## For the lead to decide

- **Primary Vulkan driver:** S0.6 already picked KosmicKrisp (not relitigated here). On the evidence, KosmicKrisp stops at 3.5 s on A8_UNORM, while MoltenVK gets to the frame loop but cannot build vertex pipelines with the translated shaders. Both stops are real work items.
- **Unimplemented policy default** (Fix 2): `fail` or `stop`.
- **Driver location:** `spike/s1-vulkan-x86/drivers/...` is still a hard-coded default.
- **Ownership** of the 160 CRT imports (shims_core) and the user32 shim (`host/`): no objection from this review.
- **verify.sh s1:** adopt the STATUS.md check with the Fix 4 hardening, and fix the `file` pattern in `verify.sh` itself (the lead owns that file).

## Files

- `/Users/a/Desktop/Projects/GTA-VI/port/build/review-host-x86_64/gta6rt`, `port/build/review-host-x86_64-configure.log`, `port/build/review-host-x86_64-build.log`
- `/Users/a/Desktop/Projects/GTA-VI/port/run/logs/review-winmain.log`, `review-winmain-trace.log`, `review-winmain-reloc.log`, `review-kosmickrisp.log`, `review-moltenvk.log` (device-lost run), `review-moltenvk2.log` (clean 120 s run)
