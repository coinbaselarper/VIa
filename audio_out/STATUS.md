# gta6rt_audio_out status (2026-11-26)

Module `port/audio_out` → static library `gta6rt_audio_out` (C API `include/gta6rt/audio_out.h`, API version 1). It is
the CoreAudio output behind shims_gdk's endpoint (`silent_audio.cpp`) plus an AudioToolbox Opus decoder for the
console APU's Opus commands (`xapu.dll`). Everything outside this directory is delivered as patches in `proposals/`
(not applied). System frameworks only: AudioToolbox, CoreAudio, CoreFoundation.

## Verdict

- With the patches and `GTA6RT_AUDIO=on`, the title's audio reaches the default output device. Measured on the S2 run
  (worker host from a patched copy of port/, x86_64 under Rosetta):
  - the intro movie's audio is captured at -20.8 to -40 dBFS per second while the movie plays;
  - the A-press UI sounds (Opus, through the new XApu decode) are captured too;
  - 0 underruns and 0 dropped frames in 85 s.
- **The size of the job is small for the boot path:**
  - The title mixes in guest code. SHAPE is used only for XMA (0 SRC, 0 EQ/comp, 0 filter/volume, 0 DMA, 0 PCM
    contexts allocated).
  - The final PCM is two mono float objects (FL, FR) of one `ISpatialAudioObjectRenderStream`, 480 frames per
    pass, event-driven.
  - Host-side decode is needed for Opus (`xapu.dll` type 3), which is implemented, and for XMA, which is not and is
    not used on this path.
- **Movie audio has gaps** wherever the title decodes the movie below 30 frames/s:
  - in the seconds where it decodes 30 frames, the audio is continuous;
  - over the whole movie, 9.63 s of non-zero audio played for 14.97 s of video decoded (t3).
  - This is the title's decode rate on this host, not the output (section 4).
- **Silent stays the default:** with `GTA6RT_AUDIO` unset or `off`, every patched path compiles to the old code path.
  - `silent_audio_test`: 129 of 129 pass, as before.
  - `xapu_chad_test`: 33 of 33 pass, as before.

## 1. The title's audio path (sources and traced runs)

Sources: `shims_gdk/src/silent_audio.cpp`, `acphal.cpp`, `xapu_chad.cpp`, and disassembly of the exe (capstone, read only).
Traced runs: `build/audio_out-host/run/logs/t1-on.log`, `t2-on.log`, `t3-on.log`. Those used
`proposals/diag-apu-counters.patch` and timestamped trace lines in the copy.

### Endpoint

1. The title calls `CoCreateInstance(CLSID_MMDeviceEnumerator)`, then `GetDefaultAudioEndpoint(eRender)`.
2. On the IMMDevice it calls `Activate(ISpatialAudioClient)` and then `ActivateSpatialAudioStream`:
   - static object mask **0x6 (FrontLeft | FrontRight)**;
   - dynamic objects 0..0;
   - object format mono float32 48 kHz (the only one offered);
   - an event handle.
3. `IAudioClient` is never initialised: there is no `stream:` line in any run, so `GetCurrentPadding`, `GetBufferSize`
   and `GetStreamLatency` are never called. Stream `GetService` is never called either.
4. The format is advertised as 7.1 float (mask 0x63F), but only the stereo pair carries audio: the title writes 480
   frames per object per pass.
5. The clock is the event. Per wake, the title calls `BeginUpdatingAudioObjects` (frames = 480), `GetBuffer` on both
   objects and `EndUpdatingAudioObjects`.
   - It has no position or padding query.
   - Its rate is whatever rate the event is set at: 100.0 passes/s measured (shim test), 8,000 passes in about 80 s
     (t3).
6. The title abandons a stream that consumes nothing:
   - t1: the first stream got 5 passes (2,400 frames), was stopped, and was replaced by a second stream.
   - t2: the device took over 5 s to start its callbacks, and its IO later stalled up to 4.2 s. The title opened 16
     streams.
   - t3, with the host clock covering the device start: 1 stream for the whole run.

### APU (`acphal.dll`, `xapu.dll`, `chad.dll`)

- `ApuCreateHeap`: cached 267,386,880 bytes, non-cached 2,256,384 bytes.
- `AcpHalAllocateShapeContexts`: src 0, eqcomp 0, filtvol 0, dma 0, **xma 350**, pcm 0.
  - So SHAPE does XMA decode only; it does no mixing.
  - `SubmitCommand` was called once in the whole run (cmd 2, register messages). There was no ENABLE_XMA, so no XMA
    voice runs on this path.
- **`xapu.dll` is the Opus decoder.**
  - It is used once per A press: type 1 (open slot), then type 3 per 20 ms packet, then type 2 (close).
  - `XApuGetResultCount`: 15,774 calls in 100 s.
- Type 3 command layout. It is built on the stack at exe+0x4f20ad0; the values are as dumped.

  | offset | field | value seen |
  |---|---|---|
  | +0x00 | type | 3 |
  | +0x04 | slot | — |
  | +0x1c | sample rate (float) | 48000.0 |
  | +0x20 | frames to skip in this packet | 312 on a stream's first packet, 0 after |
  | +0x24 | frames | 960 |
  | +0x28 | packet bytes | 160 or 80 |
  | +0x2c | input pointer | buffer + slot × 0x280 |
  | +0x34 | output bytes | channels × 0xF00 |
  | +0x38 | output pointer | buffer + 0x14000 + slot × 0x1E00 |

  The input bytes start with TOC 0xF8 (CELT, fullband, 20 ms, mono).
- The result loop (exe+0x4eaa840) needs a **type 3 result whose +0x34 holds the bytes of float PCM written**.
  - It stores that count at exe+0x4eaa8de.
  - exe+0x4e64b10 then converts that many bytes to 16-bit and requires a multiple of 32.
  - Without the type 3 result (current shims_gdk) the voice gets no PCM, and the UI sounds are silent (t1: 0 passes
    with signal after the movie).
- `chad.dll`: `XDspConnectWithMaximumStreamLimit` is called (watchdog import list). Nothing else of it was measured;
  the shim's zeroed status is unchanged.

### Bink (the title's own sound driver)

1. `BinkSetSoundSystem(exe+0x5f172c0, 0)` is called from exe+0x15e3d04.
2. exe+0x5f172c0 returns exe+0x1a9b300, the driver open function. It fills the Bink driver slots +0x170..+0x1b0
   (exe+0x20eeb40, +0x442e820, +0xb98f70, +0x4380 three times, +0x442e850, +0x442e860, +0x19031b0).
3. It allocates a PCM ring (exe+0x4ee9870 / +0x4ee9ae0) and registers {ring, Bink handle, channels, rate} in an
   8-entry table at exe+0x9dc6320 (exe+0x1a9b570).
4. The title's movie sound source (exe+0x9efd90) finds its entry by Bink handle (exe+0xa104b0) and reads up to 0x2EE00
   bytes (1 s of 48 kHz stereo 16-bit) into a streaming source at `this+0x2a8` (exe+0x43db2a0) of the title's mixer.
5. So movie audio is decoded by Bink on the CPU, mixed by the title on the CPU, and arrives as the FL/FR objects.
   Measured: object peak 0.2990 during the movie (t3).

## 2. The backend (`src/`)

- **One shared output, a ring per stream** (`audio_out.cpp`).
  - Each stream has a lock-free single-producer/single-consumer ring of interleaved float frames.
  - A write never waits: when the ring is full it takes the room and counts the rest as dropped.
  - The device callback (or the host clock) pulls from every running stream, applies the stream volume, channel
    volumes and mute, maps through the stream's matrix to the device layout, runs the limiter, feeds the tap, and
    applies the master gain.
  - The callback takes no lock and allocates nothing:
    - consumer ownership is one CAS;
    - closed streams are freed by the service thread only after the callback epoch has moved on;
    - scratch buffers and rings are preallocated and `mlock`ed (best effort).
- **Padding and position.**
  - Padding is written − consumed, exact to the frame.
  - Position is the consumed count, interpolated inside the last pull (`start + min(elapsed × 48000, pulled)`). It is
    monotonic by construction.
  - A seqlock carries the pull record; `Reset` rebases the position to 0.
- **Device** (`device_coreaudio.cpp`).
  - It uses the HAL output unit on the current default device, with a client format of 48 kHz float interleaved in
    the device's channel count, capped at 8.
  - The unit's converter does any rate conversion: the callback always works in 48 kHz frames.
  - The layout comes from `kAudioDevicePropertyPreferredChannelLayout`: labels, or the tag expanded through
    AudioFormat.
  - Listeners: the default output device, the device's alive state, its stream configuration and its nominal rate.
    A change rebuilds the unit only when the device id, channel count or rate actually differs.
- **Device work runs on its own thread.** That thread opens, starts and rebuilds the unit. The service thread keeps
  running meanwhile.
  - It consumes on the host clock whenever no device callback came for 20 ms (start-up, rebuild, stall, device gone),
    so the title's event keeps its rate.
  - It writes the tap WAV and frees retired streams.
  - The device-thread split came from t2 (section 1), where the device start and rebuild had blocked the clock.
- **Downmix** (`downmix.cpp`).
  - A speaker the device has passes at 1.0.
  - FC folds to FL and FR at -3 dB.
  - Side and back channels fold to the nearest surround present, or to the front pair at -3 dB.
  - LFE is dropped unless the device has one.
  - Mono takes 0.5 × (L + R).
  - Identity matrices take a copy path. On this Mac (2-channel "MacBook Pro Speakers") the title's FL/FR bed is passed
    through unchanged.
- **Limiter.** Instant attack, 50 ms release, threshold 0.98. Output never exceeds the threshold, and below it the
  samples are bit-exact. In the title runs it engaged on 0 frames.
- **Modes** (`GTA6RT_AUDIO`, read per call):
  - `off` (default; the shims keep their silent code);
  - `on` (CoreAudio; if the device fails it logs and consumes on the host clock);
  - `null` (host clock, no device).
- **Other environment variables:**

  | variable | meaning | default |
  |---|---|---|
  | `GTA6RT_AUDIO_GAIN` | master gain, 0..1 | 1 |
  | `GTA6RT_AUDIO_MUTE=1` | mute the output | off |
  | `GTA6RT_AUDIO_WAV=path` | 32-bit float tap of the device-layout mix before the master gain; its header is rewritten on every flush, so it survives a kill -9 | none |
  | `GTA6RT_AUDIO_STATS=s` | summary interval in seconds; 0 turns it off | 10 |
  | `GTA6RT_AUDIO_TARGET_MS` | queued audio the spatial and IAudioClient pacers keep | 50 |
  | `GTA6RT_AUDIO_NULL_CHANNELS` | channel count in null mode | 2 |
  | `GTA6RT_XAPU_OPUS=0\|1` | override the XApu decode | on when `GTA6RT_AUDIO` is on or null |
- **Every unimplemented path logs a named line and fails:**
  - a stream that is not 48 kHz, or has more than 8 channels or more than 16 streams;
  - an IAudioClient format that is not 48 kHz PCM16/24/32 or float32: it stays silent;
  - static object types with no bed position;
  - Opus with more than 2 channels or not at 48 kHz;
  - an XApu decode command outside the shared buffer;
  - XApu command types other than 1, 2 and 3;
  - a device open or start failure;
  - the XApu decode while off.

## 3. APU decode: what the boot path and movie need, and what remains

- **Intro movie:** no host decode is needed (Bink, CPU).
- **Boot-path UI sounds:** `xapu.dll` Opus. Implemented by `proposals/03-xapu-opus-decode.patch` with `gta6rt_opus_*`
  (AudioToolbox `kAudioFormatOpus`).
  - It decodes synchronously in `XApuEnqueueCommand` and queues the type 3 result with the byte count.
  - It applies the per-packet skip (AudioToolbox drops 120 frames of the first packet itself; the rest of the 312 is
    dropped here).
  - Check `xapu_opus_test`: an AudioToolbox-encoded tone comes back aligned to 1 sample (onset 4801 for 4800), with
    amplitude 0.4002 for 0.4 written, the first result 2,592 bytes, and all 51 results valid.
  - In the title (t3) each A press after the movie gives about 0.4 s of sound in the capture, at -42.8 to -46.3 dBFS
    for the second that holds it (object peak 0.1348).
- **XMA fixes:** the two from NOTES are already a ready patch from the media worker,
  `port/media/patches/acphal-xma-fixes.patch`, with tests `media_xma_fix_test_{unpatched,patched}`:
  1. a voice that ran dry stays ended for good;
  2. half-filled blocks click.

  It still applies to the current `acphal.cpp` (`patch -p1 --dry-run` from `port/`: ok). It is not duplicated here.
  On this path it changes nothing, because no XMA voice is enabled.
- **What remains for in-game audio:**
  1. **XMA2 decode.** There is no decoder on the macOS host: leonida's Rust crate needs a git dependency, and
     AudioToolbox has no XMA. The 350 contexts are allocated, and 1,818 XMA2 streams exist in audio.rpf (leonida
     `docs/audio.md`). Every enabled XMA voice will be silence until a decoder exists (`acphal.cpp` fallback).
  2. **Opus throughput.**
     - Decode cost under Rosetta: mono 36 µs and stereo 98 µs per 20 ms packet. Native arm64: 26.6 and 45.2 µs.
     - That is synchronous on the title's audio thread. Many concurrent game voices need a decode worker pool.
       Extrapolation, not measured: 64 voices would take about 16% of a core.
     - More than 2 channels is not implemented.
     - The title also handles a type 5 result (exe+0x4eaa91e). Its command is unknown and was never seen.
  3. **`chad.dll` (XDsp):** connected but not analysed; it still returns zeroed status.
  4. **Movie audio gaps:** they follow the title's movie decode rate (section 4). The output side needs nothing.
  5. **Other gaps:** device choice (default output only), outputs above 8 channels, and arm64 guest registration
     (shims_gdk returns `E_UNSUPPORTED` on arm64 module-wide; audio_out itself builds and passes on arm64).

## 4. Proof

### Unit tests (`tests/`, both arches, serial with kill timer: `tests/run_tests.sh BUILD 90`)

| test | what | arm64 | x86_64 (Rosetta) |
|---|---|---|---|
| `audio_out_downmix_test` | log sine sweep (20 Hz–20 kHz) per input channel through matrix + limiter, gains per third of the sweep, for 7.1→stereo, mono, 5.1 (L R C LFE Ls Rs), 7.1 device order; stereo bit-exact pass-through; 8 channels full scale in phase (3.121 before limiter, 0.9800 after); release to 1.0; NaN/inf frames zeroed | 421 PASS | 421 PASS |
| `audio_out_src_test` | AudioConverter (the unit's converter class) 48 kHz → 44.1/96/32 kHz, 1 kHz tone | SNR 114–132 dB | SNR 114.4 / 123.1 / 125.3 dB, unity gain |
| `audio_out_opus_test` | AudioToolbox Opus round trip, 1 and 2 ch | 10 PASS | 10 PASS |
| `audio_out_stream_test` | null clock, jittery producer (10% of passes 5–20 ms late), 8 s | 20 PASS | 20 PASS |
| `audio_out_device_test` | CoreAudio, master muted (silence on the speakers, signal in the tap), jittery producer 10 s, then a forced unit rebuild | 15 PASS | 15 PASS |

`audio_out_stream_test` results on x86_64:
- 0 underruns, 0 drops, 0 position regressions.
- Fitted rate 48,000.95 Hz; maximum deviation 278 frames.
- 384,074 frames compared in the tap: 0 mismatches.
- A 7.1 FL+SL tone lands in L at rms 0.4330 (expected 0.4330).

`audio_out_device_test` results on x86_64:
- 0 underruns; callback p50 14 µs, p99 27 µs.
- Position fit 47,999.90 Hz; maximum deviation 7.4 frames.
- During the rebuild: longest stall 12.7 ms, 1,173 host-clock frames, 0 underruns; the longest write call was 78 µs.
- The tap holds the 880 Hz tone at 0.2500.

Shim tests, from `proposals/04-shims_gdk-tests.patch`, in the patched copy:
- `audio_out_spatial_test` drives the title's sequence on the spatial stream (mask 0x6, FL 440 Hz at 0.3, FR 660 Hz at
  0.2, 5% of passes 5–25 ms late):
  - null clock: 99.9 passes/s;
  - CoreAudio muted: 100.1 passes/s;
  - in both, 0 underruns, tap L 0.3000 and R 0.2000, crosstalk 0.00000; 11 of 11 pass.
- `xapu_opus_test`: 10 of 10.
- `silent_audio_test`: 129 of 129 with `GTA6RT_AUDIO` unset, and 129 of 129 with `GTA6RT_AUDIO=null`.
- `xapu_chad_test`: 33 of 33.

### Title runs (S2 options: `tools/title_run.sh`)

- The binary is `build/audio_out-host/build/gta6rt`, built from a copy of port/ taken at 14:18 with patches 01–04 and
  the diagnostic patch.
- Options as in S2: MoltenVK x86_64, the Bink trace, A presses at 40/55/70/85/100 s, every 30th present dumped, a 100 s
  watchdog.
- The output is quiet: `GTA6RT_AUDIO=on GTA6RT_AUDIO_GAIN=0.05` (-26 dB).
- The capture is the tap before the gain.
- Analysis: `tools/title_audio_report.py`, `audio_out_wavstat`.

| run | load | spatial streams | underruns | callbacks / interval max | movie decode | non-zero movie audio |
|---|---|---|---|---|---|---|
| t1 (no XApu decode, device opened on the service thread) | 8 | 2 | 1 (64 frames, at start) | 7,473 / 12.8 ms | 15.3 frames/s | 6.57 s for 14.97 s of video |
| t2 (XApu decode, same device code) | 29 | 16 | several streams with 84–315 underruns each, at teardowns | device first callback after more than 5 s; interval max 4.22 s | — | lost after the stream replacements |
| **t3 (device thread + 20 ms stall clock)** | 16 | **1** | **0** (0 drops) | 7,925 / 12.5 ms | 20.2 frames/s | **9.63 s** for 14.97 s |

t3, measured:
- **Non-silent audio arrives during the movie.** Per-second RMS from the first decoded frames to the end of decode:
  -53.8, -36.7, -23.1, -20.8, -24.0, -38.4, -40.3, -39.3, -26.6, -26.6, -26.8, -28.7, (silence), (silence), -22.7,
  -30.1, -50.3, -67.0 dBFS.
  - File peak 0.2990 (-10.5 dBFS); limiter frames 0.
  - 89% of the energy is below 250 Hz.
  - CSV every 100 ms: `build/audio_out-host/run/t3-on.rms.csv`.
- **Audio/video.**
  - The movie decodes from WAV time 30.4 s to 52.6 s.
  - Presents with movie content (every 30th dumped, times from the logged host/epoch clock pair) run from 36.5 s to
    48.2 s.
  - In seconds with 30 decoded frames (36, 37, 43, 49), the zero-frame share is 0–2% and audio advances 1.00 s per
    1.00 s of video.
  - Where decode falls behind, the title's movie stream plays 2048-frame bursts between gaps, and some gaps last about
    1 s.
  - By the end of decode, 5.3 s fewer seconds of non-zero audio had played than seconds of video. Silence that belongs
    to the soundtrack is not separated from that figure, because the stinger's audio track was not decoded for
    reference.
  - Audio bursts start a median 50.6 ms after the latest `BinkNextFrame` (p10 8.3 ms). After being queued, audio
    reaches the speaker after the 50 ms target padding plus 1,822 frames (38 ms) of device latency and io buffer.
  - Not measured: the frame-exact offset between a presented frame and its audio, because presents are not
    timestamped per frame.
- **Underruns:** 0 in 4,082,400 frames.
  - The host clock consumed 22,611 frames (0.47 s) while the device started.
  - 2 short device stalls were covered without a stream replacement.

## 5. Performance (t3, x86_64 under Rosetta, 84.5 s of audio, title running)

| part | cost per second of audio |
|---|---|
| device callback (summed wall time; it never blocks) | 2.25 ms |
| service thread (host clock check every 2 ms, WAV tap writer) | 4.3 ms |
| pacer thread | 1.75 ms |
| mix + queue in `EndUpdatingAudioObjects` (title thread), mean 6.5–10 µs per pass at 100 passes/s | about 0.9 ms |
| **total host audio path, excluding Opus** | **about 9.2 ms (0.9% of one core)** |
| XApu Opus decode (only while a UI sound plays) | 1.8 ms mono / 4.9 ms stereo per voice |

Callback timings in t3:
- duration p50 22 µs, p99 46 µs, p99.9 112 µs, maximum 260 µs;
- interval p50 10,666 µs, p99 10,727 µs, maximum 12,530 µs.

The mix pass itself took up to 1.43 ms, measured on the title's own thread.

Unit-test comparison: callback p50 14 µs on x86_64 against 10 µs on arm64.

## 6. Integrator steps

From `port/`, after the dry-runs:
1. `for p in 01-silent_audio-host-output 02-cmake-audio_out 03-xapu-opus-decode 04-shims_gdk-tests; do patch -p1 --dry-run < audio_out/proposals/$p.patch; done`
   (all ok on 2026-11-26), then the same without `--dry-run`, in that order.
   - 02 adds `add_subdirectory(audio_out)` after `portable` in `CMakeLists.txt` (any module list). It also makes
     shims_gdk link `gta6rt_audio_out` and define `GTA6RT_HAVE_AUDIO_OUT` when that target exists.
   - Nothing changes in `host/` or `include/gta6rt/`. The host gets the library and the AudioToolbox, CoreAudio and
     CoreFoundation frameworks through shims_gdk.
2. Optional, independent: `patch -p1 < media/patches/acphal-xma-fixes.patch` (the XMA fixes; owner shims_gdk/media).
3. Do **not** apply `proposals/diag-apu-counters.patch`; it was for diagnosis only.
4. Reconfigure and build both arches as usual, then run serially:
   - `shims_gdk/silent_audio_test` (129 checks), once plain and once with `GTA6RT_AUDIO=null`;
   - `xapu_chad_test` (33);
   - `xapu_opus_test` (10);
   - `audio_out_spatial_test`, plain and with `--device` (11 each; the device run is muted).

   Checked on a fresh copy of port/ taken at 15:20 with the four patches applied (`build/audio_out-verify`, shims
   subset):
   - both arches build with rc 0;
   - every test above passes on x86_64 (Rosetta) and on arm64, as does `gdk_link_check` (`build/audio_out-verify/tests.log`).
5. Standalone module tests:
   `cmake -S audio_out -B build/audio_out-<arch> -G Ninja -DCMAKE_OSX_ARCHITECTURES=<arch> && ninja -C build/audio_out-<arch> -j2 && audio_out/tests/run_tests.sh build/audio_out-<arch> 90`.
   Under add_subdirectory the tests are off unless `-DGTA6RT_AUDIO_OUT_TESTS=ON`.
6. Unattended runs stay silent: the default is `off`. For sound, `GTA6RT_AUDIO=on`, optionally with
   `GTA6RT_AUDIO_GAIN`, `GTA6RT_AUDIO_WAV=<file>` and `GTA6RT_AUDIO_STATS=5`. For a capture without a device,
   `GTA6RT_AUDIO=null GTA6RT_AUDIO_WAV=<file>`.

## Files

- Module:
  - `CMakeLists.txt`, `include/gta6rt/audio_out.h`;
  - `src/{audio_out,device_coreaudio,downmix,opus,wav}.cpp`;
  - `tests/*.cpp`, `tests/run_tests.sh`;
  - `tools/wavstat.cpp` (`audio_out_wavstat`);
  - `tools/title_audio_report.py`;
  - `tools/title_run.sh` (the S2 run on the worker host).
- `proposals/`: `README.md`, `01`–`04` patches, `diag-apu-counters.patch`.
- Builds:
  - `port/build/audio_out-{arm64,x86_64}`, with test logs `port/build/audio_out-{arm64,x86_64}-tests.log`;
  - `port/build/audio_out-host` (patched copy, host binary, runs, captures);
  - `port/build/audio_out-verify` (fresh patched copy).
- Captures are game audio: `port/build/audio_out-host/run/t{1,2,3}-on.wav`, with logs in `run/logs/`. Keep them local.
