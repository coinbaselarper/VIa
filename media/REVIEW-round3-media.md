# Review round 3: port/media (gta6rt_media)

Verdict: **APPROVED**. No blocking defects. Every load-bearing claim in the builder's report reproduced. Eight
non-blocking findings are listed in section 5; none can be reached by this title today.

Reviewer scope: read the module, rebuilt both arches in fresh trees, re-ran all tests serially, re-did the
disassembly checks, wrote two probes, and ran the real title with a host that has the module linked in. Nothing outside
`port/media/REVIEW-round3-media.md` was edited. Not run: any guest code that calls XAudio2 or Media Foundation (nothing
in the title reaches them, see 2).

## 1. Rebuilt and re-tested (serial, one binary at a time)

Trees: `port/build/review-media-x86_64` (clean rebuild: `ninja -t clean`, then 93 steps, 12.5 s), `port/build/review-media-arm64`
(fresh, 94 steps), `port/build/review-media-host-x86_64` (host, `GTA6RT_MODULES=all`, `GTA6RT_MEDIA_HOST_AUTOREGISTER=ON`,
127 steps, 46 s). Logs: `port/build/review-media-{x86_64,arm64}-{build,tests}.log`. No compiler warnings in media sources
(the only warnings are 3 per tree of `ld: ignoring duplicate libraries: libgta6rt_os.a`).
A `review-media-x86_64` tree and log already existed (built 05:36); I cleaned and rebuilt it, so that log is mine now.

| test | arm64 | x86_64 | builder's claim |
|---|---|---|---|
| `media_xaudio2_test` | 240 PASS | 240 PASS | 240 |
| `media_mf_test` | 115 PASS | 115 PASS | 115 |
| `media_xaudio2_timing_test` silent / coreaudio (3 s via run_tests.sh) | 20 + 20 PASS | 20 + 20 PASS | 40 |
| `media_link_check` | 2 PASS | 19 PASS | 2 / 19 |
| `media_xma_fix_test_unpatched` / `_patched` | 14 / 15 PASS | 14 / 15 PASS | 14 / 15 |

The patch applies to the current `shims_gdk/src/acphal.cpp` (`patch --dry-run -p1`, mtime unchanged since 00:51). The
unpatched binary prints "84 zero samples ... (the click the patch removes)" and "the voice stays ended until the next
ENABLE_XMA (the bug)"; the patched binary shows neither. The patched file is the one built by CMake, not an edit of
shims_gdk.

## 2. Claims against the binaries (all reproduced)

Disassembly (`spike/s1-seh/venv/bin/python -I port/media/tools/disasm.py`, capstone/pefile):
- The exe imports exactly 23 Bink functions, including `BinkSetSoundSystem` and not `BinkOpenXAudio2` or
  `BinkSetSoundSystem2`. The strings `BinkOpenXAudio2` and `XAudio2` are absent from the exe.
- exe+0x442fce0 is `xor ecx,ecx; call 0x145f17360; lea rcx,[0x145f172c0]; xor edx,edx; jmp BinkSetSoundSystem`.
  0x145f172c0 is `lea rax,[0x141a9b300]; ret`. So Bink gets the title's own driver table.
- Bink2Scarlett: `BinkSetSoundSystem` (+0x4dc0) just stores the function and calls `fn(param)`; there is no default.
  `BinkOpenXAudio2` is export 35 at +0xb810; `xref.py` finds no reference to it inside the DLL; `XAudio2_9.dll!#1` is
  called from exactly one site (+0xb8c4). Vtable displacements in +0xb000..+0xbb50 are 0x28 (CreateSourceVoice),
  0x38 (CreateMasteringVoice), 0x70 (SetChannelVolumes), 0x80 (SetOutputMatrix), 0x90 (DestroyVoice), 0x98 (Start),
  0xa8 (SubmitSourceBuffer): all agree with the 2.9 slots and the media vtables.
- MF thread exe+0x33a45c0: `CreateObjectFromURL` is `call [r10+0x18]` with (url, 0x10001, NULL, &type, &obj), `*obj` is
  zeroed first, a failing HRESULT goes to a release helper and then to 0x33a4c35, which stores state 5 and returns. The
  media resolver writes `*object = 0` and `*type = MF_OBJECT_INVALID` before every failure return, so that path is safe.

Vtable order and layouts (re-derived from the public headers by memory; no header on disk): IXAudio2 13 slots, voice 19,
source 29, mastering 20, IMFAttributes 33, IMFMediaType 38, IMFSourceResolver 10; voice-callback order 0..6; pack(1)
layouts and PROPVARIANT 24 bytes with `caub.pElems` at +16 are right. All exports and vtable entries are
`GTA6RT_GUEST_ABI` (ms_abi on x86_64); LLP64 is respected (pointers as `uint64_t`, `BOOL` as `int32_t`). One exception: the
IXAudio2Extension layout, finding 1.

Real title (host built from this tree; `run.sh`, one host at a time, `GTA6RT_XAUDIO2_OUTPUT=silent`):
- `--map-only --hidden-window` (`run/logs/review-media-maponly.log`): `587 total, 587 bound, 0 trapped ... 4 bound to
  unimplemented stubs`; the 4 are crypt32. Matches.
- `--trace=0 --stop-at-winmain --hidden-window` (`review-media-winmain.log`): rc 0, `calls: 37062 total, 36765 from the
  exe, 84 distinct imports called (0 unimplemented)`. Identical to the builder's figures to the call.
- 45 s frame loop on MoltenVK x86_64 (`review-media-mvk-45.log`, `--watchdog 45`, 9,485,409 calls, 300 distinct imports,
  1 unimplemented: `_putenv_s` from amd_fidelityfx_upscaler, not media). **No call to any `xaudio2_9`, `mfplat` or
  `mfreadwrite` import.** The title does call `BinkSetOSFileCallbacks`, `BinkSetMemory`, `BinkStartAsyncThread`,
  `BinkSetSoundSystem`, `BinkGetPlatformInfo`, `BinkOpenWithOptions`, `BinkGetFrameBuffersInfo`, `BinkClose`,
  `BinkSetVolume`, `BinkPause`, `BinkSetWillLoop`. So Bink is live and goes through the title's sound driver, as claimed;
  the claim "unreachable for this exe" holds for 45 s of frame loop, not for the whole game (the MF feeder was not
  identified by the builder either).
- Note for the integrator: the first 45 s attempt aborted after 1 s (`SEH ... EXCEPTION_BREAKPOINT at exe+0x19f4f58`) because
  `shims: xmem: 32-bit reservation ... refused: no free range below 4 GiB (an x86_64 host needs -pagezero_size 0x10000 ...)`.
  It is unrelated to media (this host has no `-pagezero_size`); `LEONIDA_XMEM_32BIT=anywhere` let the run continue.

## 3. Probes

- **Thread-safety stress under TSan** (`scratchpad/stress2.cpp`, arm64, engine sources compiled with
  `-fsanitize=thread`, silent output, real engine thread, direct invoker): one callback voice resubmitting from
  `OnBufferEnd`, thread A creating/submitting/starting/GetState/stopping/flushing/SetFrequencyRatio/destroying a source
  voice in a loop (124,209 cycles), thread B hammering `GetPerformanceData`, SetVolume, SetChannelVolumes, an operation
  set + CommitChanges and GetState (330,047 iterations), thread C toggling Start/Stop on the callback voice, 6 s. Result:
  no TSan report, no deadlock, 139 OnBufferEnd callbacks (the stop/start toggling costs a few of the ~150 a 40 ms
  buffer would give), clean teardown.
- **Timing, x86_64 10 s runs, machine under load** (load average about 8; another worker's x86_64 host,
  `review-input-window-x86_64/gta6rt`, was running during this): silent: OnBufferEnd interval mean 40.002 ms, fitted
  period +2 ppm, max residual 4.06 ms, 0 of 3,938 polls saw BuffersQueued==0, GetState 17.6 ns, pass mean 43.3 us /
  max 1.51 ms. CoreAudio: mean 40.000 ms, -30 ppm, max residual 6.79 ms, 0 of 4,086 polls empty, 0 glitches, GetState
  14.6 ns, pass mean 27.3 us. These are the same order as the builder's (period within 61 ppm, lateness 6.1 / 9.9 ms,
  GetState 39 ns, pass about 45 us), so the numbers are measured, not invented. I did not reproduce 39 ns (I get 15-18 ns).

## 4. XMA patch review

Read against `acphal.cpp` (`decode_one_packet`, `pump_voice`). Fix 1 (revive when `ended` and an input is valid again, clear
the INPUT_INVALID status, reset the decoder) and fix 2 (publish only whole blocks unless `ended`) are consistent with the
surrounding state machine. Residual: a genuine mid-stream starvation still publishes one zero-padded tail block (the
voice is `ended` then), so a click remains in that case; fix 1 only makes the voice recover afterwards. This matches what
the builder describes. silent_audio / xapu: shims_core registers no `xaudio2_9`, `mfplat` or `mfreadwrite` rows (the
`registry.cpp` hit is only a return-class rule for unimplemented stubs, and the media rows take precedence: link check
resolves `XAudio2_9.dll!#1` to the media function), so there is no conflict.

## 5. Non-blocking findings

1. **IXAudio2Extension layout is unverified and probably wrong.** The module gives it 5 slots (IUnknown, then
   GetProcessingQuantum, GetProcessor). My recollection of `xaudio2.h` is that `IXAudio2Extension` is declared with
   `DECLARE_INTERFACE` (no IUnknown base), i.e. only the two methods. There is no SDK header on this machine to check, and
   the unit test re-asserts the same layout, so it is self-confirming. Nothing in the exe or Bink calls it. Verify before
   anything uses it.
2. **"Device reports 7,680 frames of its own latency / about 200 ms before heard" is device-specific.** On this Mac now
   the default device reports latency 60, safety 48, io buffer 512, and `CurrentLatencyInSamples` is 2,092 (not 9,664).
   The statement is true of whatever device the builder had selected, not of the machine.
3. **`FlushSourceBuffers` keeps flushed buffers in `BuffersQueued` until the next pass** (`test_flush_and_discontinuity`
   asserts 3, then 1 after a pass). Real XAudio2 behaviour here is not established; Bink never calls Flush (no 0xb0
   displacement in its XAudio2 driver).
4. **Raw `Voice*` captured in operation-set closures and `lookup()` dereferences the guest pointer before validating it.**
   DestroyVoice between a deferred call and `CommitChanges` is a use-after-free. Bink uses operation set 0 only.
5. **Guest callouts under the attribute-store mutex** (`put`, `a_delete_item`, `a_delete_all_items`,
   `a_copy_all_items` call `IUnknown::Release`/`AddRef` while holding `Attributes::mu`). A Release that re-enters the same
   store would self-deadlock only if the mutex were not recursive; it is `std::recursive_mutex`, so this is fine, but a
   Release that blocks on another thread that wants the store would deadlock. Unreachable (no IUnknown attribute is set).
6. **Engine thread fallback.** If `gta6rt_thread_create` fails, the engine thread is a plain `std::thread` with no TEB and
   a guest callback would fault. Callbacks are only installed by a client; the title and Bink install none.
7. **arm64 tests cannot check ms_abi** (`GTA6RT_GUEST_ABI` is empty there); only the x86_64 run exercises the calling
   convention, and only through same-compiler function pointers, never through real guest code.
8. **`host/tools/run.sh` serialisation guard only matches `build/macos/gta6rt`.** A host started with `GTA6RT_BIN` from
   another tree is invisible to it. I used a loose `pgrep gta6rt` before my three host runs and printed only PIDs, so I
   cannot rule out that another worker's host overlapped one of my runs; all three results match the builder's earlier
   runs, and I saw another worker's host (pid 83613) during the timing runs.

## 6. Integrator changes (unchanged from the builder's list; all confirmed necessary)

1. `port/CMakeLists.txt`: `set(GTA6RT_ALL_MODULES os isa seh loader shims_core shims_gdk media d3d12_vulkan host)`.
2. `port/host/CMakeLists.txt`: `target_link_libraries(gta6rt PRIVATE gta6rt_media)` (no `-force_load`).
3. `port/host/src/main.cpp`: `#include <gta6rt/media.h>`; call `gta6rt_media_register()` right after
   `gta6rt_shims_gdk_register()`, before `package_init`; log `ok` (x86_64) or `unsupported` (arm64).
4. After that: `4 bound to unimplemented stubs` (crypt32 only), as reproduced.
5. Apply `port/media/patches/acphal-xma-fixes.patch` to shims_gdk (owner's call).
6. shims_core owner: `PropVariantClear` (`com_shim.cpp:99`) does not free `VT_VECTOR|VT_UI1` contents (vt 0x1011 masks to 17,
   default case) or release `VT_UNKNOWN`; confirmed by reading it. Not hit by the title.
7. Host link: x86_64 host needs `-pagezero_size 0x10000` (or `LEONIDA_XMEM_32BIT=anywhere`) or the title aborts about 1 s into
   the frame loop; independent of media.

Shared-file edits by this review: none. Files created outside `port/media/`: build trees and logs under `port/build/review-media-*`,
run logs `port/run/logs/review-media-{maponly,winmain,mvk-45}.log`, scratch probe in the session scratchpad.
