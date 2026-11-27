# gta6rt_xma status (2026-11-26)

Module `port/xma` → static library `gta6rt_xma` (C API `include/gta6rt/xma.h`, API version 1) plus the Rust static
library `gta6rt_xma_decoder` (built by CMake with cargo for x86_64-apple-darwin or aarch64-apple-darwin). It decodes
the title's SHAPE XMA contexts on the host. Nothing outside `port/xma` and `port/build/xma-*` was changed; the
integration is three patches in `proposals/` (not applied).

## Verdict

- **XMA is needed, but only for in-game vehicle engine audio, and not on the boot/menu/first-run path.**
  - All 1,818 XMA2 streams of the package are mono, ~32 kHz, in 151 `audio.rpf` banks that hold nothing else.
    1,185 of them carry the granular grain tables (tags 0x5a/0xd9); 52 resolve by name hash to `engine_accel`,
    `engine_decel`, `engine_idle`, `exhaust_accel`, `exhaust_decel`, `exhaust_idle`. The other 633 are short one-shots
    in the same banks (5 resolve to `shutdown`, `shutdown_01`, `shutdown_02`).
  - XMA is 0.35 % of the package's audio bytes (102 MB of 29.6 GB) and 3.7 h of 1,568 h. Speech, music/radio,
    ambience, weapons, UI and the cutscene/movie tracks are Opus or Bink audio, never XMA.
  - The exe picks the decoder by the AWC codec byte; codec 5 always goes to the hardware XMA decoder class that owns
    the 350 contexts. Without XMA, every car engine, exhaust and engine shutdown sound is silent; everything else
    plays.
  - No run so far submitted an XMA command: in the instrumented runs t1-t3 (boot, Rockstar stinger, A presses, the
    first-run pages) AcpHal received only command 2. So the boot, menu and first-run path does not need XMA.
- **Built:** leonida's decoder route, through the same Rust crate (`xma`, pinned commit), vendored and built offline for
  both arches. On top of it is a frame-accurate SHAPE context engine with 4 worker threads, wired into acphal by
  `proposals/01`.
- **Proof:**
  - 244 real streams (a stratified sample) decode at 119.6-132.3 dB PSNR against ffmpeg's xma2 decoder, correlation
    1.000000000, 99.94 % of samples bit-equal, at most 1 LSB off.
  - The x86_64 and arm64 outputs are byte-identical on all 244.
  - Frame-accurate seeks and loops match ffmpeg's decode at 119.5 dB or better outside the first frame after each
    jump.
  - Cost is about 1 ms of CPU per second of audio per voice; 350 voices take about 0.35 cores, natively and under
    Rosetta.
- **Default unchanged:** with `GTA6RT_AUDIO` unset or `off` the shim keeps its old silent XMA path. `acphal_test`
  output is byte-identical to an unpatched build (x86_64, unset and `=off`).

## 1. Census of the package's audio (measured, read-only)

The tools are `tools/awc_census.py` (ADAT banks found by their raw header, with leonida's `rsc8_audio.py` header rules
and the `docs/audio.md` format and music layouts) and `tools/bink_audio_census.py` (Bink 2 headers, `docs/movies.md`).
I re-implemented leonida's rules rather than running leonida code. Outputs are in `port/build/xma-census/`
(`census.json`, `xma_streams.csv`, `bink.json`).

Every `.rpf` in the package was scanned (22 archives, 160 GB, 416 s). ADAT banks exist only in four of them:

| Archive | Banks | Streams | Bank bytes |
|---|---:|---:|---:|
| `audio.rpf` | 7,162 | 1,606,241 | 28,505,691,532 |
| `pack/story000/story000.rpf` | 63,861 | 234,458 | 9,423,830,212 |
| `pack/shared_minigame000/shared_minigame000.rpf` | 638 | 4,334 | 359,078,504 |
| `pack/story000_restofworld/story000_restofworld.rpf` | 15 | 76 | 8,908,216 |

The other 18 archives had 0 valid banks. Stray `ADAT` bytes (94 in `levels.rpf`, 20 in `core_bvh4.rpf`) are not bank
headers. These counts equal leonida's `docs/design.md` and `docs/rsc8_audio.md` figures (7,162 banks, 1,606,241
streams).

**By codec**, summed over the four archives. Bytes are the stream data tags; music rows count descriptor channels.

| Codec | Streams | Bytes | Share of bytes | Duration | Kinds |
|---|---:|---:|---:|---:|---|
| Opus (0x0B) | 1,839,262 | 29,441,101,824 | 99.46 % | ~1,565 h* | speech with lip-sync clips 641,428 (14.0 GB); streamed music/radio channels 5,824 (7.7 GB); banks with cue markers 17,962 (0.4 GB); everything else 1,174,048 (7.3 GB: SFX, ambience, weapons, vehicles other than engines, UI, unclipped dialogue) |
| **XMA2 (5)** | **1,818** | **102,309,888** | **0.35 %** | **3.70 h** | granular engine/exhaust 1,185 (95.2 MB, 3.47 h); engine-bank one-shots 633 (7.1 MB, 0.23 h) |
| PCM float32 (2) | 227 | 58,417,152 | 0.20 % | 0.08 h | `audio.rpf` other (values above ±1: impulse responses, per leonida) |
| PCM16 (0) | 19 | 283,686 | 0.00 % | 3 s | `audio.rpf` other |

\* The music hours count interleaved stereo descriptor samples as frames, so they overstate by up to 2×. The other
rows are exact from the format tags.

**XMA2 detail** (all 1,818 streams; packet statistics read from all of their 49,956 packets):

- Channels and rates:
  - 1,818 of 1,818 are mono;
  - 1,812 are at 32,000 Hz; 6 are at 32,007, 32,016, 32,035 or 32,040 Hz, which the title and ffmpeg both decode
    with the 44.1 kHz tables.
  - There is no stereo or multichannel XMA in the package.
- Placement: 151 banks; no bank mixes XMA with another codec. All 1,818 carry a seek table (tag 0xa3).
- Granular streams (1,185):
  - 0.88-34.8 s, median 12.0 s.
  - Names by hash (joaat & 0x1FFFFFFF): engine_accel 9, engine_decel 9, engine_idle 8, exhaust_accel 9,
    exhaust_decel 9, exhaust_idle 8.
  - leonida's `rsc8_audio.md` reads their 0x5a entries as (sample position, pitch 3.7-147 Hz) grain tables; its
    reference names them "granulargrains" and "granularloops".
  - The exe has `audGranularSound` and `driverSettings_NumGranularDecoders`.
- Other XMA streams (633): 0.08-5.84 s, median 1.12 s; 5 named `shutdown*`, the rest unnamed.
- 6 streams have a format-tag loop word ≠ -1 (all 0, a whole-stream loop).
- Packets:
  - packet skip 0 in all 49,956;
  - metadata 1 in all;
  - 2-63 frames per packet (most often 16);
  - 832,026 frames in total;
  - declared samples = frames × 512 in every stream.

**Bink 2 audio** (movies and cutscenes):

| Archive | Bink streams | With audio | Tracks | Codec |
|---|---:|---:|---:|---|
| `movies.rpf` | 515 | 196 | 229 | Bink Audio DCT, 48 kHz (flags 0x7000 ×227, 0x3000 ×2) |
| `story000.rpf` | 264 | 162 | 165 | Bink Audio DCT, 48 kHz (0x7000 ×164, 0x3000 ×1) |

These are decoded by Bink on the CPU and fed to the title's own Bink sound driver (port/audio_out STATUS.md section
1), so XMA plays no part. Track time from frames ÷ fps is 16,122 s and 5,614 s.

## 2. What the exe does with the 350 contexts

Disassembled read-only with capstone (`port/media/tools`). Image base 0x140000000; addresses are absolute.

**Setup** (exe+0x43aec40, the AcpHalAllocateShapeContexts site from `audio_hal_exe_calls.md`):

1. SHAPE_CONTEXTS lives at 0x149ae98d0, with numXma = 0x15e (350) and every other count 0. It then memsets the
   350 × 64-byte array.
2. A 350-entry record table goes at 0x149ae9938, 32 bytes per record: ring, work buffer, state, free flag.
3. One APU allocation of 0x222e00 bytes (flag 2, non-cached) is split per context into a **0x1700-byte output ring
   (23 blocks of 256 bytes)** and a **0x200-byte work buffer**.
4. AcpHalCreate; the IAcpHal is stored at 0x149ae98b8.
5. Connect(0x1000, 0x1000), then SubmitCommand(2, {0xc0}), which registers message mask 0xc0. Command-completed
   messages (0x10) are not registered.

**Decoder choice** (exe+0x4ee95f0): the AWC format tag's codec byte (+0x1c) picks the decoder object:

| Codec | Object | Notes |
|---|---|---|
| 11 (Opus) | 0x5b60 bytes | XApu |
| 5 (XMA2) | 0x2028 bytes | vtable 0x1465f4fc0 |
| 0-3 (PCM) | 0x570 bytes | |

No other path consumes codec 5.

The XMA decoder object's slot 4 is exe+0x785d0 (submit a buffer) and slot 5 is exe+0x78120 (update).

**Per buffer** (exe+0x785d0):

1. Takes a context round-robin from the free records (exe+0x55cbc80) and zeroes the 64 bytes. It also zeroes the work
   buffer.
2. Sets word 1:
   - stereo bit 29 (only for at most 2 channels);
   - rate bits 27-28 (rate up to 24,000 → 0, up to 32,000 → 1, up to 44,100 → 2, else 3);
   - bits 20-23 = 4;
   - output-valid bit 31.
3. Sets ring size 23 in word 0 (22-26) and the work buffer in word 8.
4. Start offset without a loop (exe+0x55cbd10): the buffer pointer advances to the packet that holds the start sample.
   With a start sample s′ = s − 128 when s ≥ 128:
   - the read offset (word 2) is the **bit offset of frame s′>>9**, found by walking the 15-bit frame lengths;
   - the start skip is ((s′>>7)&3) subframes, in word 1 bits 24-26.
5. With a loop (exe+0x43a7440):
   - loop start/end are frame bit offsets (words 3, 4);
   - loop subframe end is (end>>7)&3 (word 1 bits 12-13);
   - loop subframe skip is (start>>7)&3 (bits 17-19);
   - the loop count is 255;
   - an end past the data falls back to the last frame.
6. Input 0 = the buffer (packet count in word 0 bits 0-11, valid bit 20). Input 1 is left invalid.

**Enable:** SubmitCommand(5 ENABLE_XMA, id 0xf6f6f6f6, {index}) at exe+0x43a73e0.

**Update** (exe+0x78120):

- Available = write − read blocks; when they are equal and output-valid is clear, the whole ring.
- Copies s16 from the ring (ApuMapApuAddress of word 7), drops the samples before its start, and counts samples up to
  its end.
- Then SubmitCommand(18 ADVANCE_XMA_READ, id 0xf1f1f1f1, {index, blocks}).

**Next buffer:** it **memcpys the old context's 0x200 work buffer into the new context's** (exe+0x781c6), disables the
old one with SubmitCommand(7, id index+1) at exe+0x925e0, and enables the new one. The decoder state therefore travels
in the work buffer.

Commands the exe sends: 2, 5, 7, 18. These are all the rip-relative loads of the IAcpHal global; besides them only
Connect, Disconnect and Release are called. 6, 8, 1 and 3 have no site.

**Game systems:** the exe has no RTTI. Which sounds reach the decoder is decided by the data: codec 5 exists only in
the 151 vehicle-engine banks above.

**Runs so far.** Logs in `port/run/logs` and `port/build/audio_out-host/run/logs`:

- Only the audio_out runs t1, t2, t3 and their evidence logs carry XMA instrumentation
  (`audio_out/proposals/diag-apu-counters.patch`). They show `AllocateShapeContexts ... xma 350` and
  `submit cmd 2` (config 0xc0), and no other command: no `submit cmd 5/7/18` line and no 20,000-command summary.
- Those runs cover boot → Rockstar stinger → A presses → first-run pages (NAVIGATION EXPERIENCE ... FINISH SETUP?)
  over 85-100 s.
- The other logs have no acphal command logging (the shim does not log commands).

## 3. What was built

### Decoder

`third_party/xma` is the crate leonida's `runtime/xma_decoder` uses (omonkeyz/xbox-emulator@27122f7, a Rust port of
FFmpeg's wmaprodec, LGPL-2.1+), fetched once and vendored. I added three accessor methods for the inter-frame state;
see `third_party/xma/LOCAL_CHANGES.md`. `rust/` is a new C ABI over it:

- one decoder per context;
- one 2048-byte packet per call into a caller buffer;
- state export and import;
- `catch_unwind` on every entry.

**Build:**

- Toolchain: cargo/rustc 1.100.0; `rustup target add x86_64-apple-darwin` was installed into `~/.rustup` (aarch64 was
  already there).
- `CMakeLists.txt` runs `cargo build --release --offline --locked --target <arch>-apple-darwin` per build tree and
  imports the `.a`: 6.38 MB on arm64, 6.36 MB on x86_64.
- `-DGTA6RT_XMA_DECODER_LIB=` skips cargo; a missing cargo fails configure by name.
- The module builds standalone and under add_subdirectory, for both arches, with 0 warnings (`-Wall -Wextra`).
- Linked into the real x86_64 host binary: `build/xma-host/full-x86_64/gta6rt`, the full patched tree, links rc 0.

### Context engine (`src/engine.cpp`)

It works per frame, the way the console's XMA block and Xenia's new context do, not per packet like the current
shim. For each frame at the context's read offset:

1. Gather its bits across packets, packet skips and the switch to input 1.
2. Wrap them in a one-frame XMA2 packet and decode with the context's decoder (the MDCT state carries across loop
   jumps).
3. Queue the selected subframes:
   - start skip (word 1 bits 24-26, then cleared);
   - loop end frame: subframes 0..loop-subframe-end, then jump to the loop start frame with loop-subframe-skip;
     word 0's loop count goes down unless it is 255.
4. Write whole 256-byte blocks into the ring, keeping write and read indices and output-valid as the title expects.

Further behaviour:

- **Inputs:**
  - inputs retire as the console does: the valid bit is cleared after their last frame, and the current buffer
    switches to the other input if it is valid;
  - read offsets before a packet's first frame skip the split tail;
  - a voice that ran dry sets error 4 with its flag, and resumes when an input becomes valid again (the first applied
    fix).
- **Output blocks:** partial blocks only at the stream's final tail (the second fix). Output is subframe-granular, so
  every block is whole.
- **Work buffer:** the engine writes a tag {magic, context, generation, format} into the context's work buffer.
  - A context whose work buffer holds another context's tag imports that context's last decoder state (the title's
    exe+0x781c6 copy).
  - A zeroed work buffer gives a fresh decoder.
- **Threads:**
  - 4 workers by default (`GTA6RT_XMA_THREADS`, 1-8), started at the first ENABLE;
  - contexts are split by index;
  - a pass runs every 2 ms while voices are active, and also on ENABLE/ADVANCE;
  - the decoder is created on the worker, not in the title's SubmitCommand.
- **Locking:** ADVANCE takes only a per-context ring lock (one index update), so it never waits for a decode.
  ENABLE and DISABLE take that voice's lock and can wait for the decode pass in progress on it (at most the frames
  for 23 blocks, about 6 frames of 10-20 µs each).
- **Errors:** every failure path logs a named `[rt] xma:` line (at most 4 per voice) and outputs silence for the
  frame's 512 samples, so timing holds. The paths:
  - unmapped input or ring;
  - read offset not on a frame start;
  - invalid frame length;
  - decoder create or decode failure;
  - out of memory;
  - ENABLE or DISABLE outside the bound contexts.
- **Mode:** `gta6rt_xma_wanted()` is 1 for `GTA6RT_AUDIO=on|null` or `GTA6RT_XMA=1`, and 0 for unset/off or
  `GTA6RT_XMA=0`.

### Logging hook (off by default)

`GTA6RT_XMA_LOG=1` turns it on, with a summary every `GTA6RT_XMA_LOG_INTERVAL` seconds (default 10).

- **Decoding path** (`GTA6RT_AUDIO=on|null`):
  - one line at bind, always: `[rt] xma: decoding SHAPE XMA on the host: 350 contexts ...`;
  - with the log on: the first 64 ENABLE lines, then every 1000th. The first is marked `first ENABLE`, with context,
    rate, channels, packets, read bit, start skip, loop fields, ring and time since bind;
  - DISABLE lines with frames, samples, loops and ran-dry;
  - periodic summaries: enables, disables, advances, active (peak), frames, samples, loops, state imports, starved,
    errors and decode ms.
- **Silent path** (`GTA6RT_AUDIO` off): `gta6rt_xma_observe` logs the same ENABLE details, marked
  `(silent path, no decode)`, the first use of each command and periodic counts. A story-load run therefore shows the
  first real XMA use with only `GTA6RT_XMA_LOG=1` added.

## 4. Proof (measured)

### Module tests (`tests/xma_engine_test`)

15 of 15 pass on arm64 and on x86_64 under Rosetta; `tests/run_tests.sh <build> 90` runs them. They use synthetic
XMA2 streams whose frames spill across packets, driven through `tests/title_driver.h`. That driver re-implements
exe+0x785d0, 0x43a7440, 0x55cbd10, 0x78120 and 0x925e0 as listed in section 2.

| Check | Result |
|---|---|
| mono 24/32/44.1 kHz and stereo 32/48 kHz full streams | equal sample for sample to the in-order packet decode |
| frame-accurate starts (14 start samples, up to 150,001) | output starts at the title's position; equal after the first frame |
| loops (5 regions, 3 passes each) | length is [start frame + skip, end frame + subframe end]; equal outside the first frame after each jump |
| a frame spilling from input 0 into input 1 | decoded; both inputs retire |
| work-buffer copy between contexts (exe+0x781c6) | continuous state, all 110 frames equal |
| without the copy | only the first frame (512 samples) differs |
| ran dry, then refilled without ENABLE | resumes and continues the stream |
| ring | fills 23 blocks and clears output-valid; ADVANCE 23 refills it |
| 350 contexts at once | 350 of 350 outputs equal the reference |
| `gta6rt_xma_wanted` | env combinations |

### Shim tests

The three patches were applied to a copy of port/ (`build/xma-host/port`; applying them to the live files gives
byte-identical results). Both arches, serial (`build/xma-host/final-tests.log`):

| Test | Result |
|---|---|
| `acphal_test`, GTA6RT_AUDIO unset | 166 of 166. Output byte-identical to an unpatched baseline build (`build/xma-host/baseline`, x86_64), also with `GTA6RT_AUDIO=off` |
| `acphal_test`, `GTA6RT_AUDIO=null` | 160 of 166; the 6 failures are by design, listed below |
| new `acphal_xma_title_test`, `=off` | 9 of 9: the title's heap and context sizes, silence with the old counts |
| new `acphal_xma_title_test`, `=null` | 12 of 12: mono 32 kHz and stereo 48 kHz through SubmitCommand equal the reference; a start at sample 12,345 begins at the title's position; 4 ENABLE and 4 DISABLE reach port/xma |
| `acphal_xma_title_test` with `GTA6RT_XMA_LOG=1` | logs as described in section 3 |
| `xapu_chad_test` | 33 of 33 |
| `silent_audio_test` | 129 of 129 |
| `gdk_link_check` | OK |

The 6 `acphal_test` checks that fail under `=null` encode the legacy packet-granular model, which the title does not
use:

- "The fallback writes silence": the engine decodes real PCM.
- "One loop of packet 0 writes 12 blocks" and "Loop count is consumed by the single loop": the test's loop end, bit
  100, is not a frame start. The exe always passes frame starts.
- "Initial and loop subframe skips consume 3 blocks": the same loop setup.
- "Refill and repeated enable preserve all six frames" and "A double refill plays input 0 before input 1": both expect
  an input to retire as soon as its packet was decoded whole. The engine retires it after its last frame, as the
  console and Xenia do, and the exe never reuses a context's input.

### Real streams against ffmpeg

`tools/xma_extract.py` and `tools/xma_proof.py`. Output is in `port/build/xma-proof/`, where the extracted audio
stays.

**Sample:** 244 streams, chosen by duration within each kind:

- 141 granular and 103 other;
- 0.40-30.5 s, 1,627 s in total;
- all 6 with the loop word, all 6 at non-32 kHz rates.

**Reference:** Homebrew ffmpeg 9.0.2 (arm64, offline tool only) on a RIFF XMA2 wrapper. ffmpeg drops the first frame
and 64 samples, so every stream aligns at lag −576; the engine outputs all frames × 512 samples (= the declared
count) on 244 of 244.

| Run (engine on x86_64 under Rosetta) | PSNR min / p5 / median / max | Bit-equal samples, max diff |
|---|---|---|
| whole stream | 119.62 / 121.04 / 123.12 / 132.34 dB; correlation ≥ 0.999999999 | 51,879,741 of 51,912,003 (99.938 %), 1 LSB |
| start at a third + 37 samples, after the first frame | 119.49 / 120.81 / 123.60 / 999 (exact) dB | 1 LSB |
| loop of the middle half, twice, outside the first frame after each jump | 120.15 / 120.91 / 122.88 / 134.09 dB | 99.937 % |
| same loop, all samples | 23.2 / 33.0 / 45.3 / 134.0 dB | the first frame after a jump carries the loop-end frame's overlap |

- Granular and other streams separately: whole stream ≥ 120.33 and ≥ 119.62 dB.
- arm64 engine output is byte-identical to x86_64 on 244 of 244.
- The loop "all samples" row is what the console does, not an error: the encoder's natural predecessor of the loop
  start frame is not the loop-end frame.

### Cost

`tools/xma_bench`. Real 32 kHz mono engine streams (12 s granular, 30.5 s granular, 1.1 s one-shot). The machine was
under load average 24-67 from other workers.

| Measure | arm64 native | x86_64 under Rosetta |
|---|---|---|
| packet decoder, single thread, per 512-sample frame | 4.9-8.1 µs (single runs under the heaviest load: up to 21.8) | 6.4-8.8 µs (up to 19.9) |
| engine decode calls per voice per second of audio | 0.70-1.57 ms | 0.62-2.08 ms |
| whole process CPU per voice per second of audio (16-350 voices) | 0.78-1.17 ms | 0.74-1.23 ms |
| 350 voices, aggregate decode throughput | 1,090-2,191× real time | 774-1,999× real time |

- **Worst case for this title:** 350 contexts × 32 kHz mono, about 1 ms per voice-second, so about 0.35 of one core,
  natively or under Rosetta.
- **Real-time playback** (consumer tick every 5 ms, 10 s) with 4 workers:
  - 64 voices: 0 underruns;
  - 350 voices: 0-86 underrun ticks per 700,000 voice-ticks (0.012 % at most), at load 48-67.
- With 2 workers at 350 voices: 578-1,361 per 700,000. That is why the default is 4.
- A context needs about 62.5 frames/s (32 kHz) and has a 92 ms ring.
- **What the title realistically runs:** not measured, since no run reaches traffic.
  `driverSettings_NumGranularDecoders` exists, but its value is in data that was not decoded.

## 5. What could and could not be verified in the title

**Verified:**

- the boot/menu/first-run path submits no XMA work: logs of runs t1-t3;
- the patched tree builds and links into the real x86_64 host;
- the shim behaves as the title's call sequence expects: `acphal_xma_title_test`, which uses the title's heap sizes,
  350 contexts, 23-block rings, ids, context words, packet seek, frame offsets and work-buffer copy, as read from the
  disassembly;
- the decode matches a reference implementation on real game streams.

**Not verified, because no run reaches gameplay and I made no title run:**

- the title's real ENABLE/ADVANCE traffic and voice counts;
- that the engine's absolute alignment equals the console's. This includes the title's −128-sample back-off and its
  +0x180/+0x380 preroll in exe+0x77fc0. The engine follows the context fields and the hardware decode convention
  (first frame kept, no 64-sample skip), but a console capture would be needed to confirm it;
- audible results of engine sounds;
- the title's response to error status 4.

The logging hook (section 3) reports the first real use when a story-load run reaches it.

## 6. Integrator steps

From `port/`:

1. `rustup target add x86_64-apple-darwin` (aarch64 is installed), if a machine lacks it.
2. Dry-run, then apply in order (all dry-runs pass against the tree as of 16:50):
   ```sh
   for p in 01-acphal-xma-host 02-cmake-xma 03-shims_gdk-xma-title-test; do patch -p1 --dry-run < xma/proposals/$p.patch || break; done
   for p in 01-acphal-xma-host 02-cmake-xma 03-shims_gdk-xma-title-test; do patch -p1 < xma/proposals/$p.patch; done
   ```
   - 01 is `shims_gdk/src/acphal.cpp`; it compiles to the old code without `GTA6RT_HAVE_XMA`.
   - 02 adds `add_subdirectory(xma)` after `audio_out` and makes shims_gdk link `gta6rt_xma`.
   - 03 adds the shim test and two CTest entries (`GTA6RT_AUDIO=off` and `=null`).
3. Reconfigure and build both arches; cargo runs once per build tree, offline.
4. Run serially per arch:
   - `acphal_test` (166/166 with GTA6RT_AUDIO unset);
   - `acphal_xma_title_test` with `GTA6RT_AUDIO=off` (9/9) and `=null` (12/12);
   - `xapu_chad_test`, `silent_audio_test`, `gdk_link_check`.

   Standalone module: `cmake -S xma -B build/xma-<arch> -G Ninja -DCMAKE_OSX_ARCHITECTURES=<arch> && ninja -C
   build/xma-<arch> -j3 && xma/tests/run_tests.sh build/xma-<arch> 90`.
5. Runs:
   - Unattended runs stay as today: XMA voices are silent unless `GTA6RT_AUDIO=on|null` (or `GTA6RT_XMA=1`).
   - For the story-load driver (`host/tools/s3_storyload.sh`), add `GTA6RT_XMA_LOG=1` to see the first XMA use on
     the silent path, or `GTA6RT_AUDIO=null GTA6RT_XMA_LOG=1` to also decode it, inaudibly.
   - `GTA6RT_XMA_THREADS` sets the worker count (default 4).

## Files

- `CMakeLists.txt`, `include/gta6rt/xma.h`, `src/engine.cpp`, `rust/` (Cargo.toml, Cargo.lock, src/lib.rs).
- `third_party/xma/`: vendored crate, `LOCAL_CHANGES.md`, `UPSTREAM.sha256`.
- `ORIGIN.md`.
- `tests/`: `xma_engine_test.cpp`, `title_driver.h`, `synth_xma.h`, `run_tests.sh`.
- `tools/`:
  - `awc_census.py`, `bink_audio_census.py`;
  - `xma_extract.py`, `xma_proof.py`;
  - `xma_decode.cpp`, `xma_bench.cpp`.
- `proposals/`: README.md and 01-03.
- Build trees and data under `port/build/`:
  - `xma-arm64`, `xma-x86_64`: standalone builds and test logs;
  - `xma-census`;
  - `xma-proof`: extracted streams and decodes, game audio, kept local;
  - `xma-bench`;
  - `xma-host`: patched copy, builds, test logs, baseline, full host link;
  - `xma-deps`: the fetched crate source and the Xenia reference files.
