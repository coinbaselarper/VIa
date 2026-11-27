# port/d3d12_metal status (round 5)

## Round 5 (2026-11-26, 17:55-19:45): runs for minutes; first-run pages, legal pages, story load and world phase on Metal

Same host and binary layout as round 4 (below). Build: `ninja -C build/d3d12_metal-x86_64 gta6rt` (logs
`build/d3d12_metal-x86_64-build2{0..9}.log`, `-build3{0,1}.log`; 0 errors). Every run had other workers' titles and
compiles beside it (load average 11-41, noted per run); one gta6rt at a time was not always kept by others.

### Result

`run/logs/d3m-s3-a.log` (`host/tools/s3_storyload.sh` with `GTA6RT_BIN=<this tree's copy> GTA6RT_GPU=metal
S3_SAVES=empty`, new-game flow, hidden window, load 11-17): on the native Metal backend, unattended:

| stage | Metal (d3m-s3-a) | MoltenVK, same flow (s3r4-g, round 4) |
|---|---:|---:|
| landing prompt | +9.1 s | |
| first-run pages | +41.3 s | |
| story load (loading byte 2) | +184.4 s | +184.6 s |
| world phase (loading byte 2 -> 0, registry 38,165 images) | +212.6 s | +217.6 s |
| end | SIGABRT at +221 s, no message (below); rerun `d3m-s3-c`: no crash in 290 s | device lost 25-35 s into the world phase |

Pages read from the dumps by `s3_evidence.py` (macOS Vision), one frame kept per screen:

| screen | frame | against the MoltenVK reference |
|---|---|---|
| ACCOUNT PICKER / PLAYER (A) | `run/present/d3m-s3-a/present_000601.png` | `s2-firstrun/present_000691.png`: mean abs d 0.04 of 255 |
| NAVIGATION EXPERIENCE | `run/present/d3m-s3-a/present_005851.png` | `s2-firstrun/present_001441.png`: 0.48 |
| UI TEXT SIZE | `run/present/d3m-s3-a/present_006301.png` | `s2-firstrun/present_001621.png`: 0.47 |
| SUBTITLES | `run/present/d3m-s3-a/present_006901.png` | (no reference frame) |
| BRIGHTNESS | `run/present/d3m-s3-a/present_007501.png` | `s2-firstrun/present_002101.png`: 58.4: the page's background is grey 65 on Metal, black on MoltenVK; the calibration symbol is missing on both |
| FINISH SETUP? | `run/present/d3m-s3-a/present_008101.png` | |
| LEGAL NOTICES | `run/present/d3m-s3-a/present_010651.png` | |
| ROCKSTAR GAMES LEGAL TERMS & POLICIES | `run/present/d3m-s3-a/present_015901.png` | |
| story load (dark screen, spinner) | `run/present/d3m-s3-a/present_018751.png` | MoltenVK shows the panning artwork here |
| world phase start (same screen) | `run/present/d3m-s3-a/present_019201.png` | |
| launch movie (still uniform grey 17/17/17) | `run/present/d3m-r5-b/present_002281.png` | MoltenVK shows the movie |
| loading artwork after the movie | `run/present/d3m-r5-b/present_002881.png` | |

Presented frames/s on the pages 97-119 (`[s3] presented frames/s`), story load 28.2 s (MoltenVK 33 s), 80,247
DirectStorage requests, 0 failed. No command buffer failed and none ran 1 s or more on the GPU in any run of this
round (all `d3m-r5-*`, `d3m-s3-a`; the 32 "ran ... ms" lines of `d3m-r5-a` were a reporting bug, a `GPUStartTime` of 0
on upload command buffers, fixed).

The SIGABRT at +221 s printed nothing (no Metal assertion text, no `[seh]` line, no crash report, nothing in the
unified log for the pid); the last lines are on-demand builds of library compute shaders (`lib_cs_02865`, 64 queued)
and the governor going from level 2 to 1. The rerun with the final binary (`d3m-s3-c`, load 17-35, machine swap
7.4 of 8 GiB used, compressor holding 15 GiB) took the same path (landing +10.6 s, first-run pages +41.3 s, all
eight pages read again, story load +184.4 s, registry past 30,000 images at +216 s) and ran its full 290 s without a
crash and without a GPU error, but the story load had not ended when the timer stopped it: the process was down to
705 MiB resident with 9.5 GiB compressed and presented 0.35-6 frames/s (`textures` 46-562 ms, `heaps` 18-127 ms a
submission). Footprint peak 12,608 MiB at +269 s.

### 1. GPU timeouts: cause, guard

- **What the two timeouts had in common** (round-4 logs): both came after the process footprint passed 17.7-18.8 GiB
  with 10-12 GiB of it compressed and under 1 GiB resident, while replay took 1-2 s per submission (`submit phases`
  of `d3m-s2-a` t=101-112 s and `d3m-m2-b` t=85-98 s). No shader was new at the time in `d3m-m2-b`; in `d3m-s2-a` four
  library compute shaders had just been built on demand (`lib_cs_01076`, `_00030`, `_00031`, `_02995`). The three
  vertex shaders known to hang on synthetic data and the seven huge kernels are not among the 13 pipelines those runs
  ever built (`Metal pipelines created 13`). Every resource is in one device residency set attached to every queue
  (`port/metal` DESIGN 3.5), so a command buffer needs all of them wired: with the machine swapping that is a page-in
  of gigabytes, longer than the GPU watchdog. Conclusion (inferred, not proven): memory, not a command. The fixes in
  section 4 took the same screens from 17.7-18.8 GiB to 10.1-10.4 GiB (`d3m-r5-b`, 83-96 s) and no run has hit a
  timeout or a 1 s command buffer since (6 runs, 52-221 s, one through the world phase).
- **Named next time**: command buffers carry `MTLCommandBufferErrorOptionEncoderExecutionStatus` and every encoder a
  label (`e<n> render|compute|blit x<draws> <first pipeline> .. <last pipeline>`); a failed command buffer logs each
  encoder that did not complete and the command buffer's encoder list, and one that runs 1 s or more on the GPU
  (`GTA6RT_METAL_SLOW_CB_MS`) is logged before the watchdog would kill it (`port/metal/STATUS.md`, api v4).
- **Guard**: after a timeout / "submissions ignored" the backend answers `device_lost` to every execute, the layer
  marks the device lost, prints its report and exits (code 3; `GTA6RT_METAL_ON_LOSS=continue` keeps it); a guard thread
  ends the run (code 4) when the footprint passes `GTA6RT_METAL_FOOTPRINT_CAP_MB` (default 16,384). `run.sh`'s timer
  still caps every run. No hung process was left by any run of this round.
- Not done: shader validation on a short reproduction (no timeout has reproduced since the memory fixes, so there was
  nothing to reproduce); the unified log keeps only the process's own line for the round-4 timeouts
  (`16:48:00.744 ... Caused GPU Timeout Error`), no kernel GPU-restart entry.

### 2. Launch movie

- **Merged** upstream's Bink frame-buffer scan (ORIGIN.md "Round 5 merges"): `movie frame buffers at 0xb1f0f7c98: 3 sets
  of Y 3840x2160 and cR/cB 1920x1080 planes (entries +0xfffffffc00000000 from the textures)`, 449 set switches, 0 sets
  out of range (`d3m-r5-c`).
- **The nine R8 planes are read in place** on Metal: each plane is a buffer-backed `MTLTexture` over the title's own
  pages (section 4), no copy and no refresh; the 3-4 views of each plane written before it had a texture were bound
  then ("4 of 4 views written meanwhile bound", `d3m-r5-c`). `GTA6RT_METAL_BINK_PROBE=1` samples the Y plane of the set the
  index names through its CPU address: mean 33, 1 distinct value before the first frame, then means 80-142 with 182-228
  distinct values over 40-55 s, so the decoded frames are in the textures the GPU samples.
- **Still grey.** The draw happens every frame (`GTA6RT_METAL_DRAW_CENSUS=1`: `VS_Bink_e9aa19fd + PS_BinkNoAlpha` x105-142
  per 2 s, followed by `default_vs + ps_srgb_write`, both in plane 1; plane 0 alone is black throughout,
  `LEONIDA_PRESENT_PLANE1=0`). Its vertices vary (`GTA6RT_METAL_DRAW_TRACE=VS_Bink`: a 4-vertex strip, stride 24,
  position (0,1,0) / (0,0,0) / (1,1,0)..., UV (0,1) / (0,0)...). A uniform 17/17/17 is what the shader chain gives for
  one texel of a black plane (Y ~17, chroma 128) through `ps_srgb_write`, i.e. constant UVs. `PS_BinkNoAlpha` multiplies
  the UVs by 4 scales from a constant buffer (`VS_Bink`: table offset 0xc0, cb +0x60).
  **Ruled out**: zero-size views of title memory (65,536+ on Metal by 60 s against 4,096 on MoltenVK by 100 s, `title
  memory ... got no GPU copy`; the Vulkan path backs them with geometry copies the Metal copy dropped). With
  `LEONIDA_TITLE_WINDOWS_MB=1024` they fall to 479 (570 windows, 148 MiB) and the movie is still uniform 17/17/17
  (`d3m-r5-f`, 60 s). Reading those pages in place instead (`GTA6RT_METAL_TITLE_WINDOWS_IN_PLACE`, off) is not safe:
  SIGBUS at 28 s with it on for every address (`d3m-s3-b`), at 30 s limited to 4 GiB-0x7f00'0000'0000 (`d3m-r5-e`).
  **Remaining lead**: the VS-to-PS interface of this pair in the Metal translation (`index.json` marks VS outputs
  26-29 as f16 halves; the PS reads `attr1` as 32-bit): compare the shaderc_msl MSL of `VS_Bink_e9aa19fd` /
  `PS_BinkNoAlpha_e9aa19fd` with MoltenVK's, or draw the pair on a synthetic plane in a `port/metal` test.

### 3. First-run pages and beyond

Reached and read (table above). Replay gaps closed: `CopyTextureRegion` between textures and `CopyResource` of
textures (one Metal blit per subresource, same DXGI format; 2 formats or MSAA counted), `ClearUnorderedAccessView`
of raw/structured/R32 buffer views (`fill_buffer_u32`) and of 2D colour UAV textures (they now carry render-target
usage and are cleared as one). A deferred title texture gets its Metal texture when a list copies to or renders into it.
`d3m-r5-b` had hit 10 texture-to-texture copies before the change; since, the only counted gap is "CopyTextureRegion
between textures of two formats" (10 in `d3m-s3-a` and in `d3m-r5-d`: a format-family copy Metal's blit cannot do;
next: a compute copy or a reinterpreting view), and no ClearUnorderedAccessView has been counted.

### 4. Memory

| | round 4 (d3m-s2-a / d3m-m2-b) | round 5 (d3m-r5-b, d3m-s3-a) |
|---|---|---|
| footprint past the loading artwork | 17,735 MiB at 85 s (m2-b), 18,809 MiB at 112-138 s (s2-a) | 10,148 MiB at 83 s, 10,391 MiB at 96 s (r5-b) |
| footprint on the first-run and legal pages | (not reached) | 5,759-6,460 MiB, +41 s to +183 s (s3-a) |
| footprint at the world phase | (not reached) | 13,368 MiB at +213 s (s3-a); MoltenVK s3r4-g 13,358 MiB; Vulkan path today (s3l5-r5) 11,181 MiB |
| Metal buffers (hash table generations) | 5,828 MiB in 263 | 10-320 MiB in 4-16 |
| Metal textures | 8,221 MiB in 32,754 | 4,969 MiB at the world phase (title copies 2,834 + render targets 2,135) |

- **Descriptor hash table**: every change made a new 20-80 MiB generation and none was ever freed (263 of them, the
  5.8 GiB "buffer" line). Now a generation is reused or freed once every queue's progress passed what it had submitted
  when the generation was left and 100 ms later, its size is the table's (not twice), and a reused generation gets only
  the slots changed since it was written (a change journal): s3-a, 213 s: 8 live (80 MiB), 92 made, 84 freed, 93 full
  writes, 255,424 slot writes.
- **Title textures deferred**: a texture the title keeps in its memory that the GPU does not write (slot 133's, or an
  image made for a view of memory not readable yet) gets its Metal texture at its first fill; views written before are
  bound then. At the world phase 5,757 MiB in 6,377 textures had none yet (s3-a). `GTA6RT_METAL_DEFER_TITLE_TEXTURES=0`
  makes them at creation.
- **In place** (buffer-backed `MTLTexture` over the title's pages, `make_in_place`): linear, uncompressed, one-mip 2D
  layouts the title's own XG computer or a slot-70 view gave (not a guessed one), 64 KiB or more, a row pitch and offset
  Metal accepts: the nine Bink planes (R8), the 2048x2048 and 1024x1024 A8 textures at 0xb5d7b0000 / 0xb5dbb0000. Cannot be aliased: BC formats
  (Metal's linear textures are uncompressed), swizzled layouts (the bytes are not rows), mip chains, arrays and 3D.
  At the world phase those are 99.8% of the title bytes (made swizzled BC 3,351 MiB, slot-133 swizzled BC 4,423 MiB;
  linear uncompressed 1-mip 25 MiB).
- **Dropped copies released**: upstream's retire-on-unmap merged (ORIGIN.md); 2,651 retired copies (73 MiB) awaiting
  release at 213 s.
- **Arena**: the refusals "with 45 MiB free" were not free space: from about 227 MiB on, the arena's VAs are the
  title's own memory (keep-out report, `d3m-r5-a`/`-b`: `0xe310000+156.94 MiB (title 156.93 MiB)`; 0.13-7 MiB truly
  free). So the default stays 256 MiB on Metal (upstream's became 384, which only added footprint here), the refusal
  line now prints the free space outside the title's memory, and buffers that miss the arena take ranges of blocks
  outside it that are given back when the buffer goes (first fit, reused; before: bump allocation, never reused).
- **Accounting**: `[rt] gpu-memory t=... (periodic)` and `[rt] gpu-memory titles` lines in the Vulkan path's categories
  (title copies, render targets, staging, title shadows, descriptors/arena, buffers, textures, present, other; imported
  title pages; deferred title textures), every `GTA6RT_METAL_MEMORY_REPORT_S` (30 s).
- **Not reached: 9-10 GiB at the world phase.** At +213 s the adapter holds 6.4 GiB of Metal memory; the rest of the
  13.4 GiB footprint is the title's own memory and the host. The largest remaining items: 2.5 GiB of BC copies made for
  slot-70 views (16,280 images), 2.1 GiB of render targets (1,182), 0.76 GiB of slot-133 windows (in place, but
  charged as wired pages).

### Switches added this round

`GTA6RT_METAL_ENCODER_STATUS`, `GTA6RT_METAL_SLOW_CB_MS`, `GTA6RT_METAL_ON_LOSS`, `GTA6RT_METAL_FOOTPRINT_CAP_MB`,
`GTA6RT_METAL_MEMORY_REPORT_S`, `GTA6RT_METAL_DEFER_TITLE_TEXTURES`, `GTA6RT_METAL_TITLE_IN_PLACE`,
`GTA6RT_METAL_IN_PLACE_MIN_KB`, `GTA6RT_METAL_TITLE_WINDOWS_IN_PLACE` (off), `GTA6RT_METAL_DRAW_CENSUS`,
`GTA6RT_METAL_CENSUS_S`, `GTA6RT_METAL_DRAW_TRACE`, `GTA6RT_METAL_BINK_PROBE`. `tools/metal_run.sh` now waits for the
machine's gta6rt slot and dumps every `D3M_DUMP_EVERY` (300) presents.

### Next steps (ranked)

1. The world phase for longer: rerun the S3 flow on a quieter machine (`d3m-s3-c` stalled in the story load at load
   35 with the machine's swap nearly full); if the silent SIGABRT of `d3m-s3-a` recurs, get the stack from the host's
   signal handler (no text reached the log). The `textures` phase (46-562 ms a submission under pressure) is the next
   CPU cost.
2. The movie: the VS_Bink/PS_BinkNoAlpha interface in the Metal translation (section 2); the story-load artwork (a
   dark screen with a spinner on Metal) may share the cause. Then back the zero-size title views with copies anyway (a
   Metal pool for the Vulkan path's geometry copies): 65,536+ shader loads read 0 today.
3. Memory toward 9-10 GiB: free made BC copies the GPU has not sampled for a while (they can be refilled from the
   title's bytes), and share one image between slot-70 views of the same bytes; measure render targets the title
   never binds after the boot.
4. BRIGHTNESS: grey background where MoltenVK is black (plane 1 composition or a clear), symbol missing on both.
5. Mesh `split64` route and the 546 refused slot-57 mesh PSOs (world-phase draws).

---

# Round 4

Date 2026-11-26. Host: Apple M4 Pro, macOS 27.0, x86_64 title process (Rosetta), Apple clang 21, CMake 4.5.1.
Every number below was measured on this machine in this round, with three to four other workers' title runs and
builds on the machine (load average 20-61 during the runs; noted per run). Logs: `port/run/logs/d3m-*.log`, frames:
`port/run/present/d3m-*/` (all dumps kept for d3m-m1-c, d3m-m3-b, d3m-s2-a, d3m-vk-a; the other runs keep their
first-lit frame only, to stay above 60 GiB free), builds: `port/build/d3d12_metal-x86_64*.log`.

leonida's D3D12 layer now runs the title on `gta6rt::gpu` (`port/metal`) when `GTA6RT_GPU=metal`: the Vulkan layer
is linked but not registered, and no MoltenVK line (`[mvk-`) appears in any Metal run's log.

## What exists

- **Module** `port/d3d12_metal` (`ORIGIN.md` for provenance): the layer's backend-neutral front end compiled from
  `port/d3d12_vulkan` by path, five Vulkan-bound headers replaced for this module by a clang VFS overlay, three
  mixed files copied and ported (`shims/d3d12_com.{h,cpp}`, `shims/dxgi_com.cpp`, `shims/d3d12_metal_device.cpp`),
  and the cut points ADAPTER.md names written for Metal:
  - `src/metal_replay.cpp`: every IR command (`command_list.h`, 29 kinds) onto `gpu::CommandList`. Draws (with the
    rect-list variant built at the first rect-list draw of a triangle PSO), indirect draws and dispatches, mesh
    draws, barriers (legacy and enhanced), clears, buffer copies, footprint copies (3D slices included), queries,
    labels, markers, immediate writes. Counted and dropped (never half recorded): texture-to-texture copies,
    ResolveSubresource, ClearUnorderedAccessView, predication (runs unconditionally), clears with rectangles,
    acceleration-structure builds. On the landing prompt none was hit (the "not implemented on Metal yet" line never
    printed in d3m-m1-c, d3m-m3-a, d3m-m3-b); d3m-s2-a hit one texture-to-texture copy past the loading artwork.
  - `src/metal_context.{h,cpp}`: resource / VA / pipeline / query registry, `MetalQueue` (the layer's `QueueHolds`
    cross-queue holds, pins, progress fence, label completions, submit-phase accounting).
  - `src/metal_present.cpp`: `WindowPresenter` over `gpu::Swapchain` on the window's `CAMetalLayer`; one- and
    two-plane PresentX (`present_planes`), present dumps and first-lit frames through `present_dump.h` (same PNG
    writer and `LEONIDA_PRESENT_DUMP` / `LEONIDA_PRESENT_FIRST_LIT` variables as the Vulkan path). The present queue
    waits on every queue's last submission on the GPU (encodeWait), no CPU wait.
  - `src/metal_shaders.{h,cpp}`: the shader store (`port/build/shaderc_msl/runs/main/manifest.tsv`, 20,619
    metallibs at run time) and on-demand builds of shaders the store lacks through the same recipe
    (`shaderc_msl.py`), on two builder threads of the layer (never a title thread), each `metal` compile under the
    recipe's own timeouts (300 s cross, 600 s metal, `shaderc_msl.py`). Until a build lands the draw or dispatch is skipped and counted, so no title thread waits.
  - `src/metal_bindless.{h,cpp}`: set 0 (29 bindings, one argument buffer per binding) and the descriptor hash table.
  - Pipelines: PSO creation keeps the description (the title creates ~80,000 PSOs by the movie); the Metal pipeline
    is created at the first draw/dispatch that names it, compiled async on the backend's threads
    (`NotReady::skip`), from the store's metallib, into a `MTLBinaryArchive` written every 20 s while it grows
    (`build/d3d12_metal-x86_64/pipeline-archive/`, `GTA6RT_METAL_ARCHIVE_DIR`, `off` for none).
- **`port/metal` api_version 3** (additive; `port/metal/STATUS.md` top section): texture views, 27 more DXGI formats,
  multisampled 2D arrays, 3D-texture slice writes/copies; test `metal_texture_view` PASS under `MTL_DEBUG_LAYER=1`.
- **Symbol isolation**: the module's objects are prelinked (`ld -r -exported_symbols_list`) so its copy of leonida's
  classes coexists with `gta6rt_d3d12_vulkan` in one binary; only `gta6rt_d3d12_metal_*` and the process-wide
  shared state (title-memory notes, XG layouts, value sinks, labels, DirectStorage, traces) are exported, weak.
  `tools/check_prelink.sh` runs in the build and fails it if a layer symbol is left to bind to the Vulkan copy.

## Build and selection

```
cd port
cmake -S . -B build/d3d12_metal-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=$PWD/d3d12_metal/superbuild/attach.cmake
ninja -C build/d3d12_metal-x86_64 gta6rt
```

`port/CMakeLists.txt` and every module are configured unchanged; `superbuild/attach.cmake` adds this module after the
top level and links the host with it. Run: `GTA6RT_GPU=metal` selects Metal; unset or `vulkan` keeps the Vulkan
path (verified: d3m-vk-a ran the same binary on MoltenVK to the launch movie). Launcher used for every run below:
`d3d12_metal/tools/metal_run.sh NAME SECONDS [metal|vulkan]` (the S2 script of `host/tools/s2_firstrun.sh` with
`--hidden-window`, dumps every 30th frame, scripted A presses at 40/55/70/85/100/115 s, through `host/tools/run.sh`;
it runs a copy of the binary so the tree can be rebuilt meanwhile).

**The host change for the integrator** (`host/src/main.cpp`; attach.cmake applies exactly this to a patched copy in
the private tree, and fails if the lines move):

```
-    const gta6rt_status gdk_st = gta6rt_shims_gdk_register(), d3d_st = gta6rt_d3d12_vulkan_register();
+    const gta6rt_status gdk_st = gta6rt_shims_gdk_register(), d3d_st = gta6rt_d3d12_backend_register();
-    const gta6rt_status st = gta6rt_d3d12_vulkan_at_exit(2000);
+    const gta6rt_status st = gta6rt_d3d12_backend_at_exit(2000);
```

plus `#include <gta6rt/d3d12_backend.h>` and, in `host/CMakeLists.txt`, `target_link_libraries(gta6rt PRIVATE
gta6rt_d3d12_select "$<LINK_LIBRARY:WHOLE_ARCHIVE,gta6rt_d3d12_metal>")` with `include(d3d12_metal/d3d12_metal.cmake)`
in `port/CMakeLists.txt` after `add_subdirectory(d3d12_vulkan)`.

## Milestones on the real title

Same S2 script, `--hidden-window`, x86_64. Reference frames: `port/run/present/s2-firstrun` (MoltenVK, the shared
`build/macos` binary, visible window) and `port/run/present/d3m-vk-a` (MoltenVK, this module's binary with
`GTA6RT_GPU=vulkan`, same hidden window and script as the Metal runs).

| run | backend | load avg | first non-black frame | end |
|---|---|---|---|---|
| d3m-m1-c | Metal, archive new | 23 | frame 581, t=12.5 s | title fault exe+0x51216aa at 66 s (arena full, see below) |
| d3m-m2-b | Metal, archive loaded, 1536 MiB arena | 20 | frame 751, t=17.1 s | watchdog 100 s |
| d3m-m3-a | Metal, archive loaded | 38 | frame 641, t=18.8 s | title fault exe+0x51216aa at 78 s |
| d3m-m3-b | Metal, archive loaded, buffers outside a full arena | 27-51 | frame 691, t=23.8 s | watchdog 130 s, no fault |
| d3m-cold-a | Metal, no archive (`GTA6RT_METAL_ARCHIVE_DIR=off`) | 51 | frame 761, t=23.0 s | watchdog 45 s |
| d3m-warm-a | Metal, archive loaded (7 hits, 0 misses) | 52-37 | frame 691, t=16.6 s | watchdog 45 s |
| d3m-vk-a | MoltenVK (same binary, `GTA6RT_GPU=vulkan`) | 20-49 | frame 1321, t=47.9 s | watchdog 100 s |
| s2-firstrun | MoltenVK (reference, earlier today) | 6-8 | frame 671, t=27.7 s | watchdog 150 s |

1. **Device, heaps, descriptor heaps, root signatures: done.** `D3D12XboxCreateDevice` returns the proxy over the
   Metal device in every run (`[rt] d3d12-metal: Apple M4 Pro (x86_64 process) ...`, arena 256 MiB, set 0, shader
   store 20,619 metallibs). No MoltenVK line (`[mvk-`) appears in any Metal log; the Vulkan layer is registered
   only when `GTA6RT_GPU` is not `metal`. The title gets past its startup (D3D12 calls through PSO creation, ~80,000
   PSOs by t=65 s: `vk-counters ... psos graphics 80175 ok`, d3m-m1-c) and presents.
2. **Landing prompt through Metal: done.** d3m-m1-c `present_001021.png` against d3m-vk-a `present_001441.png`
   (MoltenVK, same binary and window mode): mean |d| 0.02 of 255, 920 pixels differ by more than 16, all inside the
   prompt text box (x 1540-1879, y 1023-1050); against the older visible-window reference
   `s2-firstrun/present_000691.png`: mean |d| 0.50, background 32.3/32.3/44.3 vs 32.6/32.6/44.6 and the reference
   has one more prompt ("SCREEN NARRATION (OFF) X"), which the MoltenVK hidden-window run lacks as well (a title
   setting, not the backend). Every present is a two-plane PresentX composed by the Metal presenter (2,527 of 2,527
   presents in d3m-m3-a), the rect-list draw uses a rectangle variant of its triangle PSO built at first use
   (`rect-list pipeline variants 1`), the two library compute shaders the store lacked (`library/lib_cs_01725`,
   `library/lib_cs_01728`) were built on demand by the recipe in 4.5-29.5 s on the layer's builder threads while
   the title went on (their dispatches skipped meanwhile: `dispatches ... (2 skipped)`), 7 Metal pipelines drew the
   landing screen (`Metal pipelines created 7`), 0 draws skipped (d3m-m3-b), 0 commands of an unimplemented kind.
3. **Launch movie and first-run pages: not reached.** After the A press the Metal runs reach the next screens (grey
   loading frames, then the full-screen artwork in `d3m-m3-b/present_001771.png` .. `002191.png`, changing between
   dumps), 130 s without a fault in d3m-m3-b. Two faults of the title at the same place (exe+0x51216aa reading 0x10,
   d3m-m1-c at 66 s and d3m-m3-a at 78 s) came with a full arena: arena allocations of 40-64 KiB buffers refused
   while 45 MiB were free in 538 blocks (the allocator skips ranges that overlap the title's own low addresses,
   `title_overlap_end`); buffers and slot-133 shadows that do not fit now go to shared blocks outside the arena (as
   the Vulkan path's title windows do), and d3m-m3-b ran its 130 s. The Bink movie decodes but is not shown: d3m-s2-a
   (150 s, Bink traced) has `BinkOpenWithOptions` 48, `BinkRegisterFrameBuffers` 1, `BinkNextFrame` 449 (449 with
   the same count on the MoltenVK reference log), and `s2_evidence.py` on its dumps finds the landing prompt from frame 721 (+15 s), then
   uniform grey frames (17/17/17, one colour) from the A press at 40 s to +58 s where the movie plays on MoltenVK,
   then the title's loading artwork (+59 to +71 s, `present_004561.png` .. `004861.png`, counted as "video" by
   the script) and no first-run page. So the nine R8 planes Bink writes on the CPU are not reaching the movie's draw
   on Metal. The same binary on MoltenVK (d3m-vk-a, the current upstream device) does show the movie
   (`d3m-vk-a/present_001681.png` .. `001951.png`, closest reference frames at mean |d| 33-60, green-tinted); the
   copy here predates upstream's 16:05 Bink frame-buffer scan (`note_bink_candidates`, not merged, ORIGIN.md), the
   first candidate cause, before the Metal title-texture refresh. **GPU hazard**: two runs that went past the loading artwork hit GPU timeouts
   (`kIOGPUCommandBufferCallbackErrorTimeout`, command buffers 4887-4888 in d3m-s2-a at about +75 s, 4892 in
   d3m-m2-b), after which Metal ignores the process's submissions. No other title ran then; runs after that were
   kept to the landing prompt.
4. **Measurements against MoltenVK** (section below).
5. **Mesh pipelines: wired; creation measured, first draws not reached.** On MoltenVK every mesh PSO is refused
   (d3m-vk-a: 14,992 = 2,930 slot 86 + 1,336 + 10,726 slot 57, "need VK_EXT_mesh_shader"). On Metal the manifest's
   `hw` route builds a `gpu::MeshPipeline` and the others are refused by name: d3m-m3-b at exit, 25,507 mesh PSO
   calls: 23,647 created (`hw` route), 1,471 refused for the `split64` route (no Metal pipeline yet), 15 for
   conservative rasterization, 374 with no reason recorded (slot 55 CreateDerivedPipelineState returned
   E_OUTOFMEMORY for 382 PSOs in that run: the code-base copy did not fit the full arena). Slot 57 alone: 11,597 of 12,062 created (465 `split64`). No screen reached draws a mesh, so no mesh
   pipeline was built at a draw; `GTA6RT_METAL_MESH_PROBE=400` builds the first 400 at creation instead
   (d3m-mesh-a, 50 s, load 44-61, background priority): 167 ready, 124 refused by Metal (121 "Fragment shader does
   not write to render target color(0), index(1) that is required for blending": dual-source blend PSOs whose pixel
   shader has no second output in the translation; 3 over 32 KiB of threadgroup memory: 34,008 and 36,056 bytes),
   109 still compiling when the run ended.

## Measurements: Metal against MoltenVK

Per-submission numbers are the layer's own `submit phases` lines (both backends print them; 5 s windows), on the
landing prompt (Metal d3m-m1-c t=20-40 s, load 23; MoltenVK d3m-vk-a t=30-46 s, load 20-49).

| | Metal | MoltenVK |
|---|---|---|
| frames/s on the landing prompt | 110-120 | 64-72 |
| submissions/s | 220-240 | 128-145 |
| replay (prepare, record, submit), ms per submission | 0.26-0.35 | 0.75-1.07 |
| console descriptor heaps refresh, ms per submission | 1.23-1.37 | 2.38-2.81 |
| slot-133 shadows / title textures, ms per submission | 0.13-0.26 / 0.23-0.26 | 0.06-0.09 / 0.17-0.23 |
| process start to first submission | under 5 s (160-180 submissions/s in the t=5-10 s window) | 15-25 s (`t=25s: 3 submissions in 10.6 s`) |
| process start to first non-black frame | 12.5-23.8 s (load 23-51; cold 23.0 s at load 51, warm 16.6 s at load 52-37) | 47.9 s (load 20-49); 27.7 s in the reference (load 6-8) |
| pipelines built | 7 Metal pipelines for the landing prompt, 13 by 130 s; archive 1,001,552 bytes | 59 Vulkan pipelines by t=106 s, 132.9 s of compile in all, mean 2,253 ms each (`vk-counters t=106s`) |
| process footprint on the landing prompt / later | 5,632-5,835 MiB (d3m-m3-b t=25-55 s) / 7,862 MiB at 115 s, 9,569 MiB at 130 s | 6,062-6,283 MiB (d3m-vk-a t=45-60 s) / 6,837 MiB at 100 s |

Costs that disappeared: MoltenVK's SPIR-V -> MSL translation and Vulkan pipeline compiles at first use (59
pipelines by t=106 s at 2.25 s mean on MoltenVK; on Metal the store's metallibs load in 0.02-0.08 s for 10-22
libraries and the pipelines compile in the background, 7 for the landing prompt); the Vulkan command translation
(replay 0.26-0.35 against 0.75-1.07 ms per submission); 10-20 s of boot before the first submission. Cold against warm archive: 7 of 7 pipelines
hit the archive warm; the landing prompt needs so few pipelines that the archive does not move the first frame
measurably at this load (23.0 s cold vs 16.6 s warm at load 37-52; 12.5 s cold at load 23).

New costs: every title texture is a Metal texture in shared memory filled from the title's pages (Metal memory at
exit of d3m-m3-b: textures 3,488 MiB in 21,799 objects, host windows over slot-133 pages 755 MiB in 3,419 buffers,
buffers 1,538 MiB, arena 256 MiB; device allocated 6,130 MiB, footprint 9,569 MiB) where the Vulkan path keeps
device-local images; the footprint grows after the landing prompt (5,835 -> 9,569 MiB by 130 s) while MoltenVK's is
6,837 MiB at 100 s. The CPU refresh before each submission (console heaps, shadows, title textures: the same code on
both paths) is the largest cost on both: `before-execute 92.7 s` of the 130 s run on Metal under load 27-51.

## Gaps (each fails loudly: a counted `not implemented on Metal yet: ...` line, an E_NOTIMPL with a log line, or a
## refused PSO handed out as a placeholder)

- Mesh PSOs on the manifest's `split64` / mesh-as-compute routes are refused (no Metal pipeline for them yet);
  conservative rasterization (15 PSOs) is refused; 28 compute PSOs declare more than Metal's 32 KiB of threadgroup
  memory (refused on both paths).
- Replay: texture-to-texture copies, ResolveSubresource, ClearUnorderedAccessView, predication, clears with
  rectangles, more than one viewport, vertex slots 8-31, acceleration-structure builds (prebuild sizes are the XDXR
  layout the Vulkan path answers; the builds are dropped). Only a texture-to-texture copy was reached (once, d3m-s2-a).
- HDR10 planes are shown without PQ decoding; the PC DXGI swap chain is E_NOTIMPL (the title presents through
  PresentX); placed resources in an existing heap (OpenExistingHeapFromAddress) are E_NOTIMPL, as on the Vulkan path.
- Memory: title textures are shared-memory copies; external blocks for buffers that miss the arena are never reused.
  The arena allocator refuses requests with free space left when the arena's VA range runs into the title's own low
  addresses (a 1536 MiB arena made it worse: 59,306 refusals with 1,381 MiB free, d3m-m2-b).
- Metal refused one bytesNoCopy window over the title's pages per run (that buffer becomes a shadow copy).
- Only x86_64 was built and run; arm64 needs the layer's arm64 export table, which d3d12_vulkan does not have.

## Minimal refactor of port/d3d12_vulkan that would remove the copies (proposal, not applied)

The copies exist because five headers and three files mix the backend in. In order of value:

1. `src/vulkan_present.h`: move `PresentRect`, `PresentPlane`, `PresentPlanes` (lines of the struct block only) into a
   new `src/present_planes.h` that `vulkan_present.h` includes. Removes `metal_present_planes.h`'s copy;
   `xbox_present.cpp` then includes `present_planes.h` and a `WindowPresenter` interface instead of `VulkanPresenter`.
2. `shims/d3d12_pipeline_compile.h`: move `note_present_for_warmup` and `compile_us` to `shims/d3d12_warmup.h`, and
   have `d3d12_xs_device.cpp` include only that. Removes the overlay entry.
3. `shims/d3d12_com.h`: split into `d3d12_com_api.h` (everything `d3d12_xs*.cpp` and `xbox_present.cpp` use:
   `TargetView`, `RootArgumentLayout`, `XboxDepthStorage`, the free-function declarations, `WindowPresenter` holding a
   `std::unique_ptr<leonida::Presenter>` interface with `present`, `present_planes`, `present_blank`, `read_back`)
   and `d3d12_com_vulkan.h` (`create_command_queue(VulkanContext&, VkQueue, ...)`, `VulkanDeviceInfo`'s handles).
   `VulkanDeviceInfo` becomes `DeviceFeatures` (the bools) + `VulkanHandles`; `d3d12_xs_device.cpp` only reads
   `presentation`. Removes the `d3d12_com.h` copy (42 hunks).
4. `shims/d3d12_com.cpp`: `CommandAllocator` and `GraphicsCommandList` (recording into `leonida::CommandList`) are
   backend-neutral (the copy keeps 2,471 of the 2,880 upstream lines unchanged); `CommandQueue` and `Fence` call `VulkanQueue`/`VkSemaphore`. Move the queue
   and fence into `d3d12_queue_vulkan.cpp` behind a `leonida::Backend` interface (`execute(lists)`, `signal`, `wait`,
   `fence create/complete/on_completion`). Removes the `d3d12_com.cpp` copy (64 hunks).
5. `shims/dxgi_com.cpp`: the factory/adapter/output objects are neutral; the swap chain's surface, images and blit are
   Vulkan. Split `dxgi_swapchain_vulkan.cpp` out; the adapter description comes from a `DeviceFeatures`/name query.
   Removes the `dxgi_com.cpp` copy (21 hunks).
6. `shims/d3d12_vulkan_device.cpp` (19,437 lines; the copy keeps 11,878 unchanged, in 453 changed hunks): the neutral
   part (argument validation,
   console heaps, arena allocator, title memory, gfx10 encoding, root signatures, PSO parsing and shader lookup,
   CheckFeatureSupport answers) moves to `d3d12_device_core.cpp` over a `DeviceBackend` interface with the calls the
   copy replaced: `create_buffer/texture/view(desc)`, `bind_image(words, view)`, `create_pipeline(desc)`,
   `format_properties(format)`, `allocation_info(desc)`, `upload(texture, region, bytes)`, `external_block(bytes)`.
   This is the large one; the interface is exactly the set of functions `shims/d3d12_metal_device.cpp` rewrote
   (`origin/hunks.txt` lists them by line).
7. `src/vulkan_backend.h` is included by neutral TUs only for `ReplayResult`, `ResourceId` and `QueueHolds`: move those
   to `src/backend_types.h`. Removes the overlay entry for it.

After 1, 2 and 7 (small, mechanical) the module still needs its copies of 3-6; after all seven it needs none.

## Next steps (ranked)

1. The GPU timeouts past the loading artwork (d3m-s2-a, d3m-m2-b): find the command (the backend's per-encoder
   labels and `MTL_SHADER_VALIDATION=1` on a run that stops right after the loading artwork, with no other title
   running) before any longer Metal run; until then Metal runs should stop at the landing prompt.
2. The launch movie: merge upstream's 16:05 Bink frame-buffer scan into the copy, then trace the nine R8 planes
   through the title-texture refresh into the movie's draw; 449 frames are decoded and none is shown on Metal.
3. The arena: fix the refusals with free space at their cause (the `title_overlap_end` keep-out against the title's
   low ranges, and the descriptor heaps' 171 MiB of 256 MiB), reuse external blocks; then re-check the title fault at
   exe+0x51216aa (twice with a full arena, not seen since buffers go outside it).
4. Title textures: keep the GPU-only ones private and alias CPU-filled ones in place (`create_buffer_texture` over a
   host window, ADAPTER.md section 5) instead of shared copies: the footprint reached 19,691 MiB at 150 s in
   d3m-s2-a (textures 8,221 MiB in 32,754 objects, buffers 5,828 MiB) against MoltenVK's 6,837 MiB at 100 s.
5. Mesh: build the `split64` route, and run the hw route's Metal mesh pipelines at first draw on a screen that draws
   meshes (the screens reached draw none).
6. The replay gaps (ClearUnorderedAccessView and texture-to-texture copies first), then arm64.
