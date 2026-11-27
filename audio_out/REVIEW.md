# Review of port/audio_out (2026-11-26)

Verdict: **no blocking finding**. The four patches apply cleanly in order and silent mode is the default and unchanged
by the tests. Nine non-blocking items should be fixed or documented before or soon after integration; the two most
useful are the unaligned spatial object buffer (one line) and the load sensitivity of the device-mode tests.

What was done: rebuilt the module standalone for both arches, ran its tests, rebuilt shims_gdk from a fresh copy of
port/ with the four patches applied in order and ran the shim tests, analysed the shipped capture
`build/audio_out-host/run/t3-on.wav` independently, and read `audio_out.cpp`, `device_coreaudio.cpp`, `downmix.cpp`,
`opus.cpp` and patches 01 to 04. No title run was made (host busy with other workers at load 17 to 29).

Builds: `build/review-audio_out-{arm64,x86_64}` (module, Release, 0 warnings) and `build/review-audio_out-patched`
(fresh patched copy, shims_gdk plus tests, both arches).

## Blocking

None found. Checked against the definition (stall or crash the title, wrong timing reports that desync Bink, a patch
that does not apply):

- **Patches apply.** From `port/`, `patch -p1 --dry-run` succeeds for 01, 02, 03, 04, `diag-apu-counters` and
  `media/patches/acphal-xma-fixes.patch` against the current tree. Applying 01 to 04 in order to a copy gave no offset
  or fuzz messages. The result is byte-identical to the builder's `build/audio_out-verify` copy for `silent_audio.cpp`,
  `xapu_chad.cpp` and `shims_gdk/CMakeLists.txt`. Each of 01, 02 and 03 reverses cleanly. The XMA fixes patch and the
  diag patch also still apply on top of 01 to 04. Patch 02 adds `add_subdirectory(audio_out)` at root line 66, before
  the module loop (line 80), so `if(TARGET gta6rt_audio_out)` in `shims_gdk/CMakeLists.txt` sees the target.
- **The callback does not block, allocate or log** (details below).
- **The title never waits on the device.** Writes are lock-free and drop when full; the device is opened, started and
  rebuilt on its own thread; the host clock takes over after 20 ms without a device pull.
- **Bink timing.** The title never initialises IAudioClient and never queries position or padding (builder's trace,
  consistent with the log: no `stream:` line). No timing report from the endpoint reaches Bink on this path.

## Measured

### Unit tests (module, run serially)

| test | arm64 | x86_64 (Rosetta) |
|---|---|---|
| downmix (421 checks) | PASS | PASS |
| src (12) | PASS, SNR 114.7 / 126.2 / 132.4 dB | PASS, 114.4 / 123.1 / 125.3 dB |
| opus (10) | PASS, 1 ch 25.4 us, 2 ch 45.9 us per packet | PASS, 37.9 us and 65.4 us |
| stream (20, null clock, jittery producer, 8 s) | PASS, 0 underruns, 0 regressions | PASS, same |
| device (15, CoreAudio muted) | see below | see below |

**Device test is load-sensitive.** `audio_out_device_test` at load average 17 to 29 (other workers building and
running the title):

| run | x86_64 phase A underruns | arm64 phase A underruns |
|---|---|---|
| in `run_tests.sh` | 1 (32 frames), phase B 2 (91 frames): FAIL | 0: PASS |
| 1 | 2 (128 frames): FAIL | 1 (32 frames): FAIL |
| 2 | 0: PASS | 2 (128 frames): FAIL |
| 3 | 0: PASS | 0: PASS |

`audio_out_spatial_test --device` on arm64 also failed once (3 underruns, 156 frames); the x86_64 run passed. The
failures are 32-frame multiples (one device pull short), the same kind the producer jitter at a 2,880-frame target
would give when the producer thread is starved for about 50 ms. Position and callback timing were fine in every run
(position regressions 0, callback p99 17 to 30 us, rebuild stall 5.7 to 14.6 ms). STATUS.md reports these as passing
without a load caveat; at quiet load they probably do. The null-clock tests (stream, spatial) never failed.

### Shim tests (patched copy, both arches)

- `silent_audio_test` plain: 129 of 129 on both. With `GTA6RT_AUDIO=null`: 129 of 129. The unpatched baseline binary
  (built 02:19) has 128 checks; the PASS/FAIL lines differ only by one added check
  ("the module registered a CoCreateInstance factory for CLSID_MMDeviceEnumerator"), which is a test-file change, not
  a behaviour change.
- `xapu_chad_test` 33 of 33; `xapu_opus_test` 10 of 10; `audio_out_spatial_test` (null) all PASS (99.9 to 100.1
  passes per second, 0 underruns).
- `silent_audio.cpp` and `xapu_chad.cpp` also compile without `GTA6RT_HAVE_AUDIO_OUT` (only unused-parameter,
  unused-private-field and unused-const warnings).

### Capture `t3-on.wav` (independent analysis)

- Format: 32-bit float, 2 ch, 48 kHz, 88.77 s, header size equals file size. No NaN or inf. Peak L 0.2990, R 0.2965;
  0 samples at or above 0.98; DC about -4e-5. L/R correlation 0.89.
- Per-second RMS matches STATUS.md: -36.7, -23.1, -20.8, -24.0, -38.4, -40.3, -39.3, -26.6, -26.6, -26.8, -28.7, (none),
  (none), -22.7, -30.1, -50.3 dBFS for seconds 35 to 50. Seconds 36, 37, 43 and 49 have 0 to 2% exact-zero frames;
  seconds 39 to 41 have 87 to 95% zero frames (bursts). The three later UI sounds (seconds 58, 73, 88) each hold about
  0.4 s of signal at -46 dBFS in that second.
- Spectrum: 89% of the energy below 250 Hz over 35 to 52 s, and 96% in the continuous seconds 36 to 38 (harmonic peaks
  at 59, 94, 117, 146, 164, 188, 205 Hz; 0.1% above 4 kHz). Tonal and plausible for a bass drone; there is no reference
  decode of the movie's audio track, so correctness of content (pitch, rate, channel order) is not independently
  verified.
- Channels: both carry signal, L and R levels within 0.6 dB, so FL and FR are not swapped or empty.
- Gaps: 46 zero runs of at least 1 ms between 35 and 52 s. 38 of them have signal of at least 0.005 on the sample
  before the gap, so the cut is abrupt (clicks). Several are exactly 480 frames (one title pass of zeros) inside
  continuous signal with equal level before and after (36.060 s: 0.124 to 0.121; 42.42, 42.58, 42.74, 42.90 s: spaced
  exactly 160 ms apart). The backend counted 0 underruns and 0 drops, so these zeros were written into the bed. Whether
  the title wrote them or the shim zeroed them (item 1 below) is not established from the capture.
- Log cross-check (`logs/t3-on.log`): callbacks 7,925, 0 underruns, 0 drops, limiter 0 frames, callback p50 22 us,
  p99 46, p99.9 112, max 260; interval max 12.53 ms; callback total 190.1 ms, service CPU 367.2 ms, pacer 140.3 ms over
  about 84.5 s, matching the 2.25 / 4.3 / 1.75 ms per second in STATUS.md. The host clock consumed 22,611 frames while
  the device started. There is a second, short "no device pulling" and "device pulls again" pair mid-run with no
  frames clocked (a stall under 1 tick). 0 device open or start failures in t2 and t3. t2 opened 16 spatial streams
  (the documented defect fixed in t3).

## Real-time safety of the render callback

Read path: `Impl::render_cb` to `Output::device_render` to `mix` to `mix_add` and `Limiter::process`. Findings:

- No mutex, no `new` or `malloc`, no `fprintf`, no `std::function`. Scratch (`pull_`) and rings are preallocated and
  `mlock`ed. `mix_add` uses a stack 8 by 8 matrix. Histogram updates are relaxed atomics on a preallocated array.
- Exclusion is one CAS on `consumer_`; on failure the callback writes silence and returns. That happens only when the
  service thread is mid-chunk during a device/host-clock handover (about 10 us) or a layout change; no wait.
- `semaphore_signal` (a mach trap) runs in the callback only when the pacer is waiting. It does not block; acceptable
  but not strictly "syscall free".
- Closed streams are freed by the service thread after the callback epoch moved on (odd/even epoch check); correct for
  the device path, and the clock path runs on the same thread that frees.
- The position seqlock reader cannot go backwards: between the sequence stores and the `read_pos` store a reader gets
  `done` (the old `read_pos`), which is at least any earlier interpolated value.
- Tap: copies into a preallocated 4 s ring; drops and counts when full. `set_tap` swaps the ring after a 30 ms sleep, so
  a callback preempted for more than 30 ms during a tap change could touch freed memory. It is only used before the
  service thread starts (`GTA6RT_AUDIO_WAV`) and by tests, so it is not reachable in a title run.

## Device behaviour

Read, not exercised with a real unplug (no safe way to remove the default device on this machine while someone is
using it):

- Listeners on the default output device (system object), the device's alive state, stream configuration and nominal
  rate. A change rebuilds the unit only if id, channel count or rate differ (`device_changed`). The listener only
  signals a semaphore.
- No default device or zero output channels: `prepare` fails with a named log line, listeners stay registered, retry in
  5 s, host clock consumes meanwhile. Start failure takes the same path.
- Device death while running: callbacks stop, `last_device_ns_` goes stale, the host clock takes over after 20 ms.
- The rebuild (measured in the device test, 8 runs): longest stall in position 5.7 to 14.6 ms, host clock frames
  796 to 1,874, 0 underruns during the switch in 7 of 8 runs (once 2 underruns, 91 frames, on x86_64 under load).
- Rate conversion is left to the HAL output unit (client format 48 kHz float); tested standalone with AudioConverter
  (SNR above 114 dB) but not with a real non-48 kHz device here.
- Gap: `reconfigure()` waits up to 100 ms for `consumer_` and, if it never wins, goes on to rewrite the matrices and
  then `consumer_.store(0)` anyway. Only reachable if the clock thread holds the consumer for 100 ms.

## Guest-facing ABI

- The patches add no virtual methods and change no signatures. `ReleaseBuffer(UINT32, DWORD)` already had the flags
  parameter; the overrides compile with `override`, so method order and signatures stay those of the interface
  headers. New helpers are non-virtual. The XApu exports keep their `__stdcall` signatures.
- HRESULTs are unchanged except for `IAudioClient::Reset` (host path returns S_FALSE when nothing was queued or played,
  S_OK otherwise) and `SpatialObject::SetVolume` (still E_INVALIDARG outside 0..1; now also stores the value).
- `SpatialObject::samples_` is a member array: its offset in the object is 12 (checked with a probe compile), so the
  buffer `GetBuffer` returns is only 4-byte aligned (heap object base plus 12). The old `scratch_` vector was 16-byte
  aligned. The title ran fine with this, but any aligned 16-byte store (`movaps`) on that pointer would fault on real
  x86 and the layout shifts with any member change (item 2).
- `XApuDequeueResult` now copies `sizeof(ApuResult)` = 0x38 bytes into the caller's buffer, not 8. The title reads +0x34
  for type 3, so its buffer is at least 0x38; the extra bytes written are zeros ("the rest stays as the caller zeroed
  it" still holds when the caller zeroed it). This is also true with audio off.
- `XApuEnqueueCommand` reads 0x40 bytes (`ApuDecodeCommand`) only for type 3, with the pointer fields validated against
  the shared buffer before use; output size is bounded by `frames * channels * 4`.

## Silent mode

- `GTA6RT_AUDIO` unset, `off` or any unknown value gives `GTA6RT_AUDIO_OFF` (`gta6rt_audio_out_mode`), and the shims then
  take their old branches: `host_output()` is false, nothing in the library is opened (the output is lazily created on
  the first stream), no thread starts, the AudioToolbox Opus path is skipped (`decode_enabled` false unless
  `GTA6RT_XAPU_OPUS=1`).
- Not byte-identical to the old code, but with no behavioural difference seen in tests: `SpatialObject` now registers
  itself in the stream's object list (a lock and a push per object), `SetVolume` stores its value, the XApu result
  copy is 0x38 bytes (above), type 3 and unknown commands log one named line, the host binary links AudioToolbox,
  CoreAudio and CoreFoundation. The log text `silent spatial stream:` and `silent stream:` is preserved.

## Non-blocking findings

1. **`GetBuffer` zeroes on every call** (patch 01, `SpatialStream::buffer`: `memset(o->samples_)` each time). If the
   title ever calls `GetBuffer` twice on an object in one pass, the second call wipes what it wrote. The 480-frame
   zero holes at 160 ms spacing in the capture are consistent with either a title-side zero pass or this; not
   established. Fix: zero only when `!o->written_`. Cheap check: log the number of `GetBuffer` calls per object per pass
   in a title run.
2. **Unaligned spatial object buffer** (offset 12). Add `alignas(64)` to `samples_`.
3. **Device-mode tests fail at high load** (table above). Either allow a few underruns in the device tests, run them
   only at low load, or raise the harness target; document it in STATUS.md.
4. **Position is the consumed count, not the heard count.** It leads what is audible by the device latency (1,310 frames,
   27 ms, plus the io buffer 512 frames) and `GetCurrentPadding` excludes frames inside the device. The patch adds the
   latency to `GetStreamLatency` but not to `GetPosition`. Not used by the title, so harmless today.
5. **Opus decoder cold start.** In a fresh process the first `AudioConverterNew` for Opus took 142 ms (x86_64, load
   about 20) and the first decode 3 ms; later creates took 0.05 to 0.2 ms. In the title this runs synchronously inside
   `XApuEnqueueCommand` under the connection mutex on the first Opus voice. The t3 run showed no underrun around the
   first A press, so the cost was not visible there, but a warm-up decoder create at `XApuConnect`, off the title's
   thread, would remove the risk.
6. **Sync is only partly measured.** STATUS.md says so for the frame-exact offset. In addition the delay of 50.6 ms
   median from `BinkNextFrame` to the audio burst in the tap already contains the ring target (default 50 ms,
   `GTA6RT_AUDIO_TARGET_MS`) and the device adds 38 ms after the tap, so audio leaves the speaker about 90 ms after the
   decode event. The cause of the gaps (title decode rate at 15 to 20 frames/s under load 16 to 29) is plausible from
   t1 against t3 (15.3 against 20.2 frames/s, 6.57 against 9.63 s of non-zero audio) but has no control run (same
   load with a different backend) and no reference decode of the movie audio.
7. **Tap channel count is fixed at tap open.** `GTA6RT_AUDIO_WAV` opens the tap in `ensure_service` with the null
   channel count (2) before the device layout is known, so on a device with more than two channels the WAV holds only
   the first two device channels and no log line says so. Capture is correct on this Mac (2 channels).
8. **Logging under the stream lock.** `render_pass_locked` writes a stderr line every 1,000 passes while holding
   `lock_` on the title's thread; a blocked stderr pipe would stall that pass. Same pattern as the rest of the
   shims; mention only.
9. **Documentation.** STATUS.md says the device tests pass; add the load caveat. The XMA fixes are referenced, not
   ported, which is acceptable because `media/patches/acphal-xma-fixes.patch` applies, but nothing in this module tests
   them, and they are inert on the boot path (no XMA voice runs).

## Integrator steps (confirmed)

From `port/`:

1. Dry-run, then apply in order (all succeed today):
   `for p in 01-silent_audio-host-output 02-cmake-audio_out 03-xapu-opus-decode 04-shims_gdk-tests; do patch -p1 --dry-run < audio_out/proposals/$p.patch && patch -p1 < audio_out/proposals/$p.patch; done`
   (run the loop only after the dry-run of all four has passed).
2. Optional: `patch -p1 < media/patches/acphal-xma-fixes.patch`. Do not apply `diag-apu-counters.patch`.
3. Apply fix items 1 and 2 above to patch 01 first if wanted (two small edits in `SpatialObject`/`SpatialStream::buffer`).
4. Reconfigure and build both arches. Run serially: `silent_audio_test` (129, plain and `GTA6RT_AUDIO=null`),
   `xapu_chad_test` (33), `xapu_opus_test` (10), `audio_out_spatial_test` (null mode), and the module tests with
   `audio_out/tests/run_tests.sh build/audio_out-<arch> 90`; run the device-mode tests (`audio_out_device_test`,
   `audio_out_spatial_test --device`) when the host is quiet.
5. Unattended runs stay silent by default. For sound set `GTA6RT_AUDIO=on`; use `GTA6RT_AUDIO_GAIN=0.05` while testing.
