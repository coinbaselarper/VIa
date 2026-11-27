# gta6rt_media status (2026-11-26)

Module `port/media` → static library `gta6rt_media`. Serves `xaudio2_9.dll` (XAudio2 2.9, Bink2Scarlett's import
`#1`) and the title's Media Foundation imports (`mfplat.dll` x6, `mfreadwrite.dll` x1) as guest-facing COM with
`ms_abi` vtables. Entry point `gta6rt_media_register()` (`include/gta6rt/media.h`, API version 1). Origins: `ORIGIN.md`.

## 1. What the title and Bink actually do with these imports (disassembly, `tools/`)

Commands: `spike/s1-seh/venv/bin/python -I port/media/tools/{import_sites,disasm,xref,ptrref}.py <PE> ...`.

### Bink2Scarlett's XAudio2 driver: reachable only through `BinkOpenXAudio2`, which the title does not use

- `XAudio2Create` is called from exactly one site, Bink2Scarlett+0xb8c4, inside the export `BinkOpenXAudio2`
  (ordinal 35, +0xb810). Its argument selects: 0 or -2 → Bink creates its own engine, -1 → returns 0, any other value
  → the caller's `IXAudio2*` plus mastering voice are used.
- What Bink calls when it does open XAudio2 (all slots match the Windows 2.9 vtables):
  - `XAudio2Create(&x, 0x400, 1)`. 0x400 is not a Windows 2.9 flag (console flag); processor 1.
  - `IXAudio2::CreateMasteringVoice` (slot 7) with `(&m, 0, 0, 0, deviceId, NULL, 6)`. The device id is only passed
    for argument -2. 6 is AudioCategory_GameEffects.
  - On reopen: `m->DestroyVoice` (slot 18), then `x->Release` (slot 2).
  - Per audio track, +0xb190: `CreateSourceVoice` (slot 5) with a PCM 16-bit WAVEFORMATEX, Flags
    `XAUDIO2_VOICE_NOPITCH`, MaxFrequencyRatio 1.0, and no callback, sends or effects.
    - It allocates 16 ring buffers, each bytes/s ÷ 16 rounded up to 32 bytes (62.5 ms; 12,000 bytes at 48 kHz stereo).
    - Playback starts once bytes/s ÷ 2 is queued (500 ms).
  - Source-voice methods used:
    - `SubmitSourceBuffer` (21): Flags 0, LoopCount 0, pContext 0.
    - `GetState(&s, 0)` (25): Bink reads only `BuffersQueued`. 0 means underflow and resets the track; 16 means the ring is full.
    - `Start(0,0)` (19): started for all tracks of a file at once.
    - `Stop(0,0)` (20): used for pause.
    - `SetChannelVolumes(1|2, v, 0)` (14): volume × 1/32768, with pan.
    - `SetOutputMatrix(NULL, srcCh, dstCh, m, 0)` (16): only on the speaker-volume path. dstCh defaults to 8.
    - `DestroyVoice` (18).
  - **No `IXAudio2VoiceCallback` and no engine callback.** Bink syncs on `BuffersQueued` and polling only, so the
    timing that matters is that buffers are consumed at the output clock's rate.
  - `BinkFindXAudio2WinDevice` (ordinal 13, +0xb5c0) uses `CoCreateInstance(CLSID_MMDeviceEnumerator)`, then
    `EnumAudioEndpoints`, `GetId` and `PKEY_Device_FriendlyName`. shims_gdk's silent_audio serves those classes.
- **The title gives Bink its own sound system.**
  - The exe imports `BinkSetSoundSystem` from Bink2Scarlett.dll and calls `BinkSetSoundSystem(exe+0x5f172c0, 0)`
    at exe+0x442fcf9.
  - exe+0x5f172c0 returns exe+0x1a9b300, the title's own Bink sound driver: the same driver-table slots +0x170..+0x1b0
    as Bink's XAudio2 driver.
  - The exe neither imports nor names `BinkOpenXAudio2` (the string is absent).
  - So, with this exe, **`xaudio2_9.dll!#1` is unreachable**. Movie audio goes through the title's audio engine. That
    engine ends in shims_gdk's silent spatial stream (`[rt] audio: silent spatial stream ...` in `run/logs/s1-moltenvk.log`).
  - Measured: not called in any run, including mine below.

### Media Foundation in the exe: one reader thread, for media the package does not contain

All 8 call sites are in exe+0x33a3e00..0x33a5620. The thread is exe+0x33a45c0 (64 KiB stack), started from
exe+0x33a4490 ← exe+0x33a7650 ← exe+0xdc0ff4. That caller walks a queue of items with a path (array exe+0x8759f90,
count exe+0x8759fa0); I did not identify which game feature fills the queue.

What the thread does:
- `CoInitializeEx(NULL, 0)`, retried with 6 on RPC_E_CHANGED_MODE. Then `MFStartup(0x20070, MFSTARTUP_LITE)`.
  At the end, `MFShutdown` and `CoUninitialize`.
- `MFCreateAttributes(&a, 3)`, then `a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 1)` (slot 21).
- `MFCreateSourceResolver`, then `CreateObjectFromURL(path, MF_RESOLUTION_MEDIASOURCE|MF_RESOLUTION_READ, NULL, &type,
  &src)` (slot 3). It requires type == MF_OBJECT_MEDIASOURCE.
- `MFCreateSourceReaderFromMediaSource(src, a, &reader)`.
- Reader methods: `GetNativeMediaType` (5), `SetStreamSelection(i, FALSE)` for unused streams (4),
  `SetCurrentMediaType` (7), `SetCurrentPosition(GUID_NULL, VT_I8)` (8), `ReadSample` (9),
  `GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION / MF_PD_TOTAL_FILE_SIZE)` (12).
  On the sample it calls `GetSampleDuration` (IMFSample slot 37). `MFGetSystemTime` paces the loop.
- Media types:
  - Native types read: `GetMajorType` (33), `GetGUID(MF_MT_SUBTYPE)`, `GetUINT64(MF_MT_FRAME_SIZE / FRAME_RATE)`,
    `GetUINT32(MF_MT_AUDIO_NUM_CHANNELS / BITS_PER_SAMPLE / SAMPLES_PER_SECOND)`.
  - Video codecs accepted: H.264, WMV1/2/3 and WVC1.
  - Audio codecs accepted: AAC (0x1610) and WMA (0x161..0x164).
  - Audio-only mode also requires 16-bit, 48 kHz, ≤ 2 channels.
  - Output types the title sets on the reader: `MFCreateMediaType` {Video, NV12} and {Audio, PCM, 48000 Hz}
    (exe+0x33a4250, +0x33a42b0).

Containers would be MP4/MOV (H.264/AAC) or ASF (WMV/WMA). **The package contains none:**
- no loose media file (22 .rpf, 57 .ttpf, 16 .dll, 5 .png, ...);
- every movie is Bink 2 in movies.rpf (`leonida/docs/movies.md`: 515 streams, 99.99% of the archive).

A URL cannot name an RPF entry. So no AVFoundation, VideoToolbox or AudioToolbox decoder was built. If a real file
ever appears, H.264/AAC would be decodable with AVFoundation, and WMV/WMA have no Apple decoder.

## 2. Implemented objects and methods

### XAudio2 (`src/xaudio2_com.cpp` → `src/xaudio2_engine.cpp`)

| object | implemented | fails (named log line + code) |
|---|---|---|
| `XAudio2Create` | yes. Unknown flags are accepted and logged once (Bink's 0x400). `XAUDIO2_1024_QUANTUM` is honoured | NULL out → E_INVALIDARG |
| IXAudio2 (13 slots) | QI (IUnknown, IXAudio2, IXAudio2Extension), AddRef/Release, Register/UnregisterForCallbacks, CreateSourceVoice, CreateSubmixVoice, CreateMasteringVoice, StartEngine/StopEngine, CommitChanges (operation sets), GetPerformanceData, SetDebugConfiguration (logs only) | see the voice rows |
| IXAudio2Extension (5) | GetProcessingQuantum (quantum/rate), GetProcessor (1) | — |
| mastering voice (20) | default 2 ch / 48 kHz, quantum rate/100. GetChannelMask, volumes, details, DestroyVoice | second master → XAUDIO2_E_INVALID_CALL. The device id is ignored and logged |
| source voice (29) | PCM 8/16/24/32-bit, float32, WAVE_FORMAT_EXTENSIBLE (PCM/float). Linear-interpolation SRC with frequency ratio. Start/Stop with operation sets, SubmitSourceBuffer (play region, loops incl. infinite, END_OF_STREAM), FlushSourceBuffers, Discontinuity, ExitLoop, GetState (incl. NOSAMPLESPLAYED), SetFrequencyRatio (clamped; NOPITCH → INVALID_CALL), SetSourceSampleRate, sends to submix/master, SetOutputVoices, volume/channel volume/output matrix (default matrices: identity, mono→stereo 1/1, N→mono average, >2→stereo fold) | ADPCM, xWMA, XMA2 and other tags → E_NOTIMPL `CreateSourceVoice: format 0x%04x (%s) not implemented`. `XAUDIO2_VOICE_USEFILTER` → E_NOTIMPL. Effect chains → E_NOTIMPL. `XAUDIO2_BUFFER_WMA` → E_NOTIMPL. A queue of 64 or a bad buffer → XAUDIO2_E_INVALID_CALL |
| submix voice (19) | input rate == master rate, processing stages, sends | other input rates → E_NOTIMPL. `SEND_USEFILTER` → E_NOTIMPL |
| effects/filters on any voice | SetEffectChain(NULL or 0 effects) S_OK. GetEffectState writes FALSE; Get*FilterParameters write {LowPass,1,1} | SetEffectChain with effects → E_NOTIMPL. Enable/DisableEffect, Set/GetEffectParameters, SetFilterParameters, SetOutputFilterParameters → XAUDIO2_E_INVALID_CALL (no effect or filter exists) |
| callbacks | IXAudio2VoiceCallback: OnVoiceProcessingPassStart(BytesRequired), OnBufferStart, OnBufferEnd, OnLoopEnd, OnStreamEnd, OnVoiceProcessingPassEnd. IXAudio2EngineCallback: OnProcessingPassStart/End. Called on the engine thread (an os guest thread) through `gta6rt_isa_call_guest`, with no engine lock held, so OnBufferEnd may resubmit | OnVoiceError and OnCriticalError are never raised (no device-loss detection) |
| DestroyVoice | waits for a running pass. Refuses (logs) while another voice still sends to it | from inside a callback → refused with a log line (on Windows this deadlocks) |

Timing rules:
- OnBufferEnd, LoopEnd and StreamEnd fire in the pass that outputs the buffer's last frame, not when it is fetched
  (the resampler looks 2 frames ahead).
- `BuffersQueued` keeps counting a buffer until then.

Output (`src/audio_output_*.cpp`), chosen by `GTA6RT_XAUDIO2_OUTPUT`:
- `coreaudio` (default) uses the default-output AudioUnit with float32 interleaved data.
  - The engine thread keeps a ring filled to io buffer + one quantum (992 frames here), so consumption follows the device clock.
  - If the device cannot be opened, it logs and falls back to silent.
- `silent` paces passes on the host monotonic clock (absolute deadlines) and discards the output.

### Media Foundation (`src/mf.cpp`)

| export / object | implemented | fails |
|---|---|---|
| `MFStartup` | SDK 2, API ≤ 0x70 → S_OK, counted. Flags are accepted | other versions → MF_E_BAD_STARTUP_VERSION |
| `MFShutdown` | decrements the count, S_OK | unbalanced → logged once (still S_OK) |
| `MFGetSystemTime` | 100 ns monotonic | — |
| `MFCreateAttributes` / IMFAttributes (33) | all 30 methods. Types UINT32/UINT64/DOUBLE/GUID/STRING/BLOB/IUNKNOWN (guest IUnknowns AddRef/Release/QI through guest calls). Allocated strings/blobs/PROPVARIANT payloads use malloc, which matches shims_core's CoTaskMemFree | Compare/CopyAllItems with a store not made here → E_NOTIMPL |
| `MFCreateMediaType` / IMFMediaType (38) | the above plus GetMajorType, IsCompressedFormat (MF_MT_COMPRESSED, else MF_MT_ALL_SAMPLES_INDEPENDENT), IsEqual (all four flags) | GetRepresentation/FreeRepresentation → E_NOTIMPL. IsEqual with a foreign type → E_NOTIMPL |
| `MFCreateSourceResolver` / IMFSourceResolver (10) | QI/AddRef/Release. CreateObjectFromURL parses the URL (plain path, or `file:` with %-decoding) and checks the file with shims_core's GetFileAttributesW row | missing file or directory → 0x80070002. Existing file → MF_E_UNSUPPORTED_BYTESTREAM_TYPE (no media source exists). Other scheme → MF_E_UNSUPPORTED_SCHEME. Flags without MEDIASOURCE/BYTESTREAM → E_INVALIDARG. CreateObjectFromByteStream → MF_E_UNSUPPORTED_BYTESTREAM_TYPE. Begin/End*, CancelObjectCreation → E_NOTIMPL |
| `MFCreateSourceReaderFromMediaSource` | — | E_NOTIMPL (NULL source → E_INVALIDARG, NULL out → E_POINTER) |

On the title's path, `CreateObjectFromURL` fails and the reader thread takes its error exit (exe+0x33a4c35: state 5).
No source reader, media source, IMFSample or IMFMediaBuffer object exists.

### Stubs by name (every path that fails on purpose)

XAudio2:
- `CreateSourceVoice` for ADPCM, xWMA (WMAUDIO2/3), XMA2, or EXTENSIBLE with another subformat.
- `XAUDIO2_VOICE_USEFILTER`, `XAUDIO2_SEND_USEFILTER`.
- Effect chains in `Create*Voice` and `SetEffectChain`.
- `SubmitSourceBuffer` with `XAUDIO2_BUFFER_WMA`.
- `CreateSubmixVoice` at a rate other than the mastering rate.
- `DestroyVoice` from a callback.
- `OnVoiceError` and `OnCriticalError` are never raised.

MF:
- `IMFMediaType::GetRepresentation`, `FreeRepresentation`.
- `IMFSourceResolver::CreateObjectFromURL` (no media source), `CreateObjectFromByteStream`,
  `BeginCreateObjectFromURL`, `EndCreateObjectFromURL`, `BeginCreateObjectFromByteStream`,
  `EndCreateObjectFromByteStream`, `CancelObjectCreation`.
- `MFCreateSourceReaderFromMediaSource`.
- `Compare`, `CopyAllItems` and `IsEqual` with foreign objects.

Accepted but ignored:
- the mastering device id and the audio stream category;
- the `XAudio2Create` processor argument;
- `XAUDIO2_STOP_ENGINE_WHEN_IDLE`;
- `XAUDIO2_PLAY_TAILS` (no tails exist);
- `MFStartup` flags;
- CreateObjectFromURL's property store.

arm64: `gta6rt_media_register()` returns GTA6RT_E_UNSUPPORTED, because the tables and vtables are ms_abi, as in
shims_gdk. Frame adapters are not started.

## 3. Build and tests (both arches, run serially)

```sh
cd port
cmake -S media/dev -B build/media-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake -S media/dev -B build/media-arm64  -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64  -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build/media-<arch> -j3 gta6rt_media media_xaudio2_test media_mf_test media_xaudio2_timing_test \
      media_link_check media_xma_fix_test_patched media_xma_fix_test_unpatched
media/tests/run_tests.sh build/media-<arch> 3       # serial, 120 s kill timer per binary
```

`media/dev` configures port/ with `GTA6RT_MODULES=os;isa;seh;loader;shims_core;shims_gdk` and adds port/media, so
port/CMakeLists.txt is not edited. Both trees build rc 0 with no warnings in media sources
(`build/media-<arch>-build.log`).

| test | what | arm64 (native) | x86_64 (Rosetta) |
|---|---|---|---|
| `media_xaudio2_test` | Manual-mode engine driven by a host fake client through the vtables. Covers Bink's exact sequence (output bit-exact, BuffersQueued drops in the pass that outputs frame 2999, Stop keeps state, underflow); callback order and passes; BytesRequired; loops/ExitLoop/play regions; 24→48 kHz SRC; frequency ratio; flush; discontinuity; validation and E_NOTIMPL paths; operation sets; QI and Extension; submix; 8/24/32-bit, float and extensible formats | 240 checks PASS | 240 checks PASS |
| `media_mf_test` | the title's sequence (MFStartup 0x20070/LITE, attributes, resolver results for missing/present/https, reader E_NOTIMPL, the title's output types, IsEqual), all IMFAttributes methods, guest IUnknown refcounts, MFGetSystemTime | 115 PASS | 115 PASS |
| `media_xaudio2_timing_test` | threaded engine, silent and CoreAudio: Bink-pattern client plus a callback client (40 ms buffers refilled from OnBufferEnd) | 40 PASS (10 s each) | 40 PASS (10 s each) |
| `media_link_check` | real shims_core/isa/os. Registers the DLLs, resolves the 9 imports with the PE spellings (`XAudio2_9.dll` #1, `MFPlat.DLL`, `MFReadWrite.dll`) to the module's functions, and checks the resolver against shims_core's GetFileAttributesW on a scratch G: | 2 PASS (registration unsupported) | 19 PASS |
| `media_xma_fix_test_unpatched` / `_patched` | see section 5 | 14 / 15 PASS | 14 / 15 PASS |

Run logs: `build/media-arm64-tests.log`, `build/media-x86_64-tests.log`, `build/media-x86_64-timing.log`.
- The first x86_64 run (`media-x86_64-tests.log`) overlapped with another worker's x86_64 host (`d3d12_vulkan-host-x86_64/gta6rt`, pid 17816).
- Its correctness results stand. Its timing numbers are not used here.
- The x86_64 timing below is the re-run on an idle machine (`media-x86_64-timing.log`).

### Measured timing (10 s runs; CoreAudio writes silence to the default device)

| | arm64 silent | arm64 CoreAudio | x86_64 silent | x86_64 CoreAudio |
|---|---|---|---|---|
| OnBufferEnd interval (40 ms buffers) mean / sd | 40.018 / 1.79 ms | 40.011 / 4.62 ms | 40.029 / 2.30 ms | 40.016 / 4.74 ms |
| fitted period vs 40 ms | +36 ppm | -1 ppm | -61 ppm | +41 ppm |
| max lateness vs the fitted schedule | 2.96 ms | 4.22 ms | 6.10 ms | 9.88 ms |
| Bink-pattern client: polls with BuffersQueued==0 | 0 of 3,572 | 0 of 3,587 | 0 of 2,726 | 0 of 2,713 |
| GetState cost | 13.7 ns | 10.7 ns | 39.1 ns | 39.1 ns |
| processing pass (2 voices + callbacks) mean / max | 27.1 / 85.5 µs | 22.9 / 72.0 µs | 46.6 / 134.8 µs | 43.1 / 494 µs |
| output glitches (render callback short of data) | — | 0 | — | 0 |

Notes on the measurements:
- **CoreAudio device on this Mac:** 48 kHz, io buffer 512 frames, reported device latency 7,680 frames, safety offset 0.
  `GetPerformanceData.CurrentLatencyInSamples` is 9,664: ring target 992 + device 7,680 + io buffer 512 + quantum 480.
  So an OnBufferEnd precedes audibility by about 200 ms on this device. Most of that is the device's own latency.
- **CoreAudio jitter:** the larger sd comes from 480-frame passes being drawn by 512-frame device callbacks.
- **Polled consumption rate:** the Bink client's rate measured from polled SamplesPlayed (-109..-684 ppm) is limited by
  the 2 ms poll. The OnBufferEnd fit is the clock measurement.
- No optimisation was done: nothing here has been exercised by the title.

## 4. Against the real title (worker host, `build/media-host-x86_64/gta6rt`)

- Built from `media/dev` with `-DGTA6RT_MODULES=all -DGTA6RT_MEDIA_HOST_AUTOREGISTER=ON`.
  - That links gta6rt_media into the host and registers from a constructor (`dev/autoregister.cpp`).
  - host/src/win/win.cmake is added as well: the window layer's new sources are not in host/CMakeLists.txt yet, and the
    tree does not link without them (`host::win::MessageQueue::drop_window` undefined).
- Runs used `host/tools/run.sh` with `GTA6RT_BIN`, one at a time, after checking that no gta6rt was running.
- `media-maponly.log` (`--map-only --hidden-window`): `exe imports: 587 total, 587 bound, 0 trapped, ..., 4 bound to
  unimplemented stubs`. Before: 12. The 4 left are crypt32 `CertCloseStore`, `CertOpenSystemStoreW`,
  `CertFreeCertificateChain`, `CertFindChainInStore`.
- The registration line appears twice because the host re-executes itself for ROSETTA_ADVERTISE_AVX.
- `media-winmain.log` (`--trace=0 --stop-at-winmain --hidden-window`): rc 0, the run reaches WinMain.
  - Marker: `REACHED MARKER: RVA 0x5a91bd0`, `rcx=0x140000000`.
  - Counts: `calls: 37062 total, 36765 from the exe, 84 distinct imports called (0 unimplemented)`.
  - S1's figure was 36,739. This tree builds the current sources of every other module, so the +26 is not attributed
    to media. The 84-import count is the same.
- Not run against the title: anything that calls these imports. Neither path is reached (section 1).

## 5. XMA fixes for shims_gdk: patch proposal `patches/acphal-xma-fixes.patch` (shims_gdk not modified)

Against `port/shims_gdk/src/acphal.cpp`. These are the two NOTES/cool_fox items.
1. **A voice that runs dry stays ended for good.**
   - `decode_one_packet` returns at once while `voice.ended`, so a title that refills an input with the context still
     enabled gets no more blocks until ENABLE_XMA.
   - Patch: when ended and an input is valid again, clear `ended`, clear an INPUT_INVALID status, and reset the decoder.
   - This applies **with and without** the XMA2 decoder.
2. **Half-filled blocks click.**
   - `pump_voice` publishes a zero-padded block whenever fewer than 128 samples are queued.
   - Patch: publish only whole blocks unless the voice has ended (the final tail).
   - Relevant once `LEONIDA_HAVE_XMA2_DECODER` is defined. The fallback decoder only produces multiples of 128.
- Check: `media_xma_fix_test_{unpatched,patched}`. They are built from a copy of acphal.cpp (patched with `patch` at
  build time) plus a stand-in decoder (`tests/xma/`: 100 samples/channel per frame).
  - **Blocks case** (a packet, 17 zero-sample packets, two more packets):
    - unpatched: 84 zero samples published in block 2;
    - patched: 0.
  - **Revive case** (dry, then input 0 refilled without ENABLE):
    - unpatched: the write block stays at 1;
    - patched: the refill is decoded (block 1 = refill PCM).
  - Both binaries pass on both arches; each asserts its own expected behaviour.

## 6. silent_audio / xapu (shims_gdk, read only): no conflict

- Different DLL names: shims_gdk registers xgameruntime, gameinput, acphal, xapu, chad, dstorage and the COM class
  CLSID_MMDeviceEnumerator. Media registers xaudio2_9, mfplat, mfreadwrite. No COM classes.
- The title's audio: XApu/SHAPE through acphal, mixed into silent_audio's spatial stream (`serving the silent output
  endpoint`, `silent spatial stream` in `s1-moltenvk.log`). No host device is opened.
- XAudio2 here opens CoreAudio only when a mastering voice is created, which this title never does. If it did, or if
  silent_audio later gets a CoreAudio backend, two AudioUnits on the default device are mixed by CoreAudio.
- Bink's `BinkFindXAudio2WinDevice` would use silent_audio's enumerator; the resulting device id is ignored here.
- Env names do not overlap (`GTA6RT_XAUDIO2_OUTPUT` vs `LEONIDA_*`).

## 7. For the integrator

Required changes:
1. `port/CMakeLists.txt`: add `media` to `GTA6RT_ALL_MODULES` after `shims_gdk` (before `d3d12_vulkan`/`host`):
   `set(GTA6RT_ALL_MODULES os isa seh loader shims_core shims_gdk media d3d12_vulkan host)`.
   port/media/CMakeLists.txt works as a module. Its link check and XMA patch test are only added when the
   shims_core/shims_gdk targets exist.
2. `port/host/CMakeLists.txt`: `target_link_libraries(gta6rt PRIVATE gta6rt_media)`. No `-force_load` is needed; the
   registration is an explicit call. The library carries `-framework AudioToolbox AudioUnit CoreAudio CoreFoundation`.
3. `port/host/src/main.cpp`: `#include <gta6rt/media.h>`. Call `gta6rt_media_register()` right after
   `gta6rt_shims_gdk_register()` and before `package_init` and the exe binding (Bink2Scarlett's `xaudio2_9.dll!#1` is
   bound with the package DLLs). Log its status: `ok` on x86_64, `unsupported` on arm64.
4. Expected after that: `4 bound to unimplemented stubs` (crypt32 only) instead of 12.
5. Optional: `GTA6RT_XAUDIO2_OUTPUT=silent` for headless or CI runs. The default opens the default output device the
   first time a mastering voice is created, which this title does not do.

For other owners:
6. shims_gdk owner: apply `patches/acphal-xma-fixes.patch`. Fix 1 matters now; fix 2 matters once the XMA2 decoder lands.
7. shims_core owner (finding, not changed):
   - `com_shim.cpp` `PropVariantClear` does not free `VT_VECTOR|VT_UI1` elements (vt 0x1011 → `vt & ~0x1000` = 17).
   - It does not Release `VT_UNKNOWN`.
   - It matters if the title clears PROPVARIANTs from `IMFAttributes::GetItem` holding blobs or interfaces. The title's
     own clears are VT_UI8 (MF_PD_DURATION / TOTAL_FILE_SIZE) and are fine.

Shared-file edits made by this task: `port/include/gta6rt/media.h` (new header, owned by this module). Nothing else
outside `port/media/` was written except build trees `port/build/media-{x86_64,arm64,host-x86_64}`, their logs in
`port/build/`, and `port/run/logs/media-*.log`.
