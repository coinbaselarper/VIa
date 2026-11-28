# port/d3d12_metal status (round 7)

## Round 16, stencil worker (2026-11-27 17:05-18:50): stencil T#s now read the stencil (X32_Stencil8), the dot-speckle of every surface is gone; the 1D / 2D-storage index ranges are wide by default; view sharing makes a noise band and is off; depth-stencil UAV planes are needed (316 + 1 asked) but not built

Details, tables and PNGs: the worker's spike folder (r14-stencil). Same-binary A/Bs, two or more runs a leg, alone in
the slot; every run ended on the 4 Hz wired stop or the 13,312 cap at world+30..50 s; 0 failed command buffers, 0 GPU
faults, no gpuEvent from these runs (the count went 10 -> 13 during the round: another worker's hang probes).
M = measured, I = inferred; load 3.2-12.8 at run starts.

- **Stencil view, default ON (`GTA6RT_METAL_STENCIL_VIEW=0` off).** Every stencil T# (GFX10 8_UINT on a texture with a
  stencil aspect: title-built and plane-1 SRVs) used to get a view in the texture's own `Depth32Float_Stencil8`
  format, which reads the DEPTH plane; shaders read its `.y` as the stencil class (M: offline probe and code). Now
  `make_view` gives it `X32_Stencil8` (stencil in R, measured offline) with the T#'s swizzle (byte in G), and a stencil
  view has its own view identity. One path covers the D16S8 / D24S8 / D32S8 families (all `Depth32Float_Stencil8` on
  Metal). A/B (M): isolated "2-G-buffer" pixels written by the G-buffer fixup kernel 14.6-22.8 k off vs 0.12-0.24 k on;
  composite dots, whole frame / roof box: 0.26-0.86% / 3.6-5.0% off vs 0.13-0.20% / 0.12-0.20% on; what stays class 3
  is whole objects (foliage, rails, stilts, glass). A house that turns black is black albedo in RT0 in both legs (the
  known streaming fault), not the fixup.
- **`GTA6RT_METAL_BINDLESS_WIDE`, default ON (`=0` off):** 0 refused binds (vs 16-42 k by world+45 s), the pier
  walkway / dock edges / deck lit at world present 101 where both-off runs have them black; no GPU or memory cost
  measurable (GPU 357-366 vs 341-375 ms a frame, Metal allocated ~8.0 GiB in both).
- **`GTA6RT_METAL_VIEW_SHARE`, back to OFF (it was default 17:55-18:41):** with it, a full-width band of noise ~20 rows
  tall at one screen row appears in single presents of 7 of 12 runs, never in 8 runs without it (M, by detector and by
  eye); with it alone (WIDE off) 3 of 6 dumped presents. Cause not found.
- **Depth-stencil UAV planes (census, M):** the title asks 312 UAVs of D16 shadow maps' depth (R16_UNORM views), 4 of
  screen-size depths (R32_FLOAT / R16_UNORM) and 1 of a D24S8 stencil plane (R8_UINT); all write nothing on Metal
  (E_NOTIMPL). Not built: it needs depth/stencil <-> colour-plane blits at the texture's UAV transitions (the Vulkan
  path's `copy_depth_uav`), i.e. the barrier path plus a gpu API, outside this task's files; memory: a same-size copy of
  every such texture, wired.
- **Translator rows (speckle worker), re-checked with the right stencil:** 64-bit image atomics (3 rows) and the
  early-fragment-test rows WITHOUT KILL_ENABLE (136) hold in 2 warm runs each (no harm, no measurable gain); the
  magenta band of the full early-depth set comes from the 14 KILL_ENABLE rows (I: early tests write depth/stencil
  before a discard can cancel them); implicit LOD in compute: no verdict (one of two runs odd); mesh clamp: no rows.
- Files: `shims/d3d12_metal_device.cpp` (`stencil_view_on`, `bind_image_views_locked`, `make_view`, a census line in
  `refresh_title_textures`, a counter and a bounded log in `CreateUnorderedAccessView`, the `view_share_on` default),
  `src/metal_bindless.cpp` (constructor: WIDE default), port/metal `mtl_formats.mm` (DXGI 22 -> `X32_Stencil8`),
  `mtl_device.mm` (`create_texture_view`: a stencil view only of a pixel-format-view `Depth32Float_Stencil8`).
- Next (ranked): (1) the depth-stencil UAV planes (blits at UAV transitions); (2) the SHARE band (which index is shared
  when it shows; per-index ownership log in one band run); (3) a second `lod` pair; the KILL early-depth rows bisected.

## Round 16, GPU-budget worker (2026-11-27 17:06-18:40): a steady world frame is 387 ms of GPU, almost all shader stages run one at a time; non-shader terms ~3%; the structured mesh rows take 22-27 ms off

M = measured, I = inferred; 11 world runs, 0 failed command buffers, 0 GPU faults, every run ended on the machine-wired
stop or the machine guard 5-70 s into the world.
- New opt-in timing (port/metal queue, under `GTA6RT_METAL_PASS_TIMING`): `_MODES=b|s|p` per window (b: every encoder's
  stage-boundary timestamps without waits, every command buffer of every queue with encode / gate-hold / kernel / GPU
  times and a counter-clock calibration; s: splits now also cut explicit render passes and compute encoders at kernel
  changes), `_TRIGGER=<file>` (windows counted from the world). Metal allows 32 counter sample buffers a process, so
  command buffers share sample slabs. `gpu_budget.py` (cache tools) turns the TSV into a budget whose rows sum to the
  window exactly.
- Steady frame (M, defaults, 13 frames): 387 ms = compute 180 (46.5%) + vertex/mesh 146 (37.7%) + fragment 47.5 (12.3%;
  tile load/store 4.5-7.5 of it, offline probe x census bytes) + GPU idle 9.2 (submission latency 6.2, commit-gate holds
  2.6, waiting for an encode 0.4) + pass structure and untimed ~4. Command buffers on the GPU 97.5%, equal to the encoder
  sum within 0.1%. Two stages run at once only 1-1.4% of the time: barriers join every earlier encoder.
- The round-15 "unattributed" time: the mesh census never split explicit render passes (the G-buffer passes), and the
  compute census covered selected kernels only.
- The 19 structured mesh rows of round 14: same binary, two runs a leg: 387 -> 365 ms a frame, geometry 146 -> 126,
  controls equal, presented frames with no difference beyond run-to-run spread; proposed as default (requests file).
- Prebuild "window" (I, 2 of 2 runs against 4 of 4 with "all"): 700+ pipelines not ready at world entry, then a broken
  world for the whole run; a candidate cause of the intermittent broken scene.
- Reachable (I): ~2.9 frames/s with every non-shader term removed, ~3-3.3 with more shader passes of the current kind.
  The next 2x needs concurrency (resource-precise barriers, GPU-side waits for committed signallers) and a wave64
  emulation without cross-SIMD rounds for the G-buffer geometry.

## Round 15, visible-faults worker (2026-11-27 14:10-17:05): the dark blocks / sawtooth / pale rectangle are NOT the hair copy (measured) but are already in the pre-upscale colour; the "T# view refused" class is the 2D storage index range (4,096) running full; minimap, water and albedo not fixed; no before/after from the hand-over view

Tree `build/vfx-x86_64` (kept: `gta6rt.b6` = live sources of 15:57 + the bindless line below, the binary of the A/B
pair; `gta6rt.b9` = b6 + two opt-in diagnostics since removed again), launcher `run_world2.sh` (the S4 settings, cap
14,848 / 13,312, CB abort 2000, fault_stop, 4 Hz wired stop 9,728, machine guard 15,360, tmem probe lock, a per-run
binary copy, waits for machine wired < 4,200 MiB before starting), `tpair.sh` (scene probe + draw trace), `tlite.sh`
(presents only). Runs vfx-t0..t21. **0 failed command buffers, 0 GPU page faults.** gpuEvent-gta6rt 10 before and
after every run (the 10th, `gpuEvent-gta6rt_wrounds_p-...-154134.ips`, is the wave-rounds probe's at 15:41).
**Incidents:** (1) t0 and t2 ran AT THE SAME TIME for 59 s before the world (my second queue did not wait, and both
launchers copied to one path so neither saw the other); both ended on the machine guard (wired 8,280 + compressor
7,094 MiB), no GPU error. Fixed in my launcher (per-run copy path). (2) t10 ended at world+6 s on SIGABRT `mutex lock
failed: Invalid argument` in port/metal `MetalDescriptorHeap::residency_for_submit` from `Encoder::close_command_buffer`
on the release thread (a descriptor heap used after it was destroyed; first time seen; log
`run/logs/vfx-t10-crash.log`); not reproduced in 11 later runs; not my code (b8 differed from b6 by log lines only).
**Every world run ended 12-75 s into the world on the 4 Hz machine-wired stop** (9,730-9,897 MiB; 2.8-3.4 GiB before
the game), i.e. before the hand-over (world+57-105 s): **no frame of the hand-over view on the deck exists from my
runs, so the requested before/after pair from that camera was not produced.** All frames below are the pre-hand-over
stilt-house view at world+10..50 s. M = measured, I = inferred; load 3-11 at run starts.

### 1. Dark block, sawtooth, pale rectangle (M)

- My predecessor's attribution to `PS_HairCopy` (14:58 note) is wrong. The hair tile list the copy and the blur draw
  (s8+0 V#, read back in vfx-t12) holds 12-19 tiles of 16 px at 1280x720 around the head ((43..47, 17..20) of the
  80x45 grid, `0x0011002b`...), the VS constants are 1280x720, so both draws cover small tiles only. With
  `GTA6RT_METAL_SKIP_DRAWS=PS_HairCopy` (t15) the blocks are unchanged against t16 (same binary, no skip):
  `/Users/a/Desktop/Projects/GTA-VI/port/run/present/vfx-t15-haircopy-skipped-blocks-remain.png`,
  `/Users/a/Desktop/Projects/GTA-VI/port/run/present/vfx-t16-haircopy-on-same-blocks.png` (same faults in both).
- **The block and the sawtooth are already in the 1280x720 scene colour before the upscaler** (t17 per-pass dumps,
  `/Users/a/Desktop/Projects/GTA-VI/port/run/present/vfx-t17-colour-before-upscale-block-and-sawtooth.png`, after
  `PS_DistantLights`; top-left block [0,512)x[168,336) mean 6.3 against 22.9 elsewhere, 50% zero). They appear between
  `PS_ResolveDeferred_*` (pass 1423, block 83% zero but so is most of the frame) and `PS_DistantLights` (1689); between
  them run only compute (scattering/volumetrics 019xx, sr_tracing 028xx, `CS_ApplyCombinedDeferred` lib_cs_01976, sky
  `CS_SkyMain` 02099) and moon/stars. Refusing `lib_cs_01976` (t20) leaves every surface below the horizon black, so the
  lit geometry comes from that compute composite; refusing the sr denoiser 02909 (t19) leaves the sawtooth.
- **`TiledLighting` writes nothing** (t17: after the 180 indirect rect-list draws of pass 1227 f10 is 0.0% non-zero,
  rt1 0.2%; `SSSResolve`, also indirect rect-list, then writes 11.3%). Lighting owner: worth a draw trace of its
  rect-list counts (my `GTA6RT_METAL_INDIRECT_ARGS_DUMP` does not see rect-list draws: they return before it).
- Inferred, not proven: the tile-shaped regions are tiles a tiled compute pass (ApplyCombinedDeferred or one of its
  inputs) does not write, so they keep another aliased target's bytes (content of another view, hard tile edges).
  Next: per-dispatch dumps (the scene probe dumps after draws only) of f10 around 01976/02099.
- Not caused by T# resource choice: an opt-in census of every T# and slot-70 view of at least 640x360 (t9, t10, t13)
  found 0 addresses with more than one placement of the same size, so no aliasing ambiguity there (removed again).

### 2. "T# view refused" / "no view for the T#'s format or range": the 2D storage index range is full (M)

- 35,473 refusals in vfx-snap1c are not format mismatches (T# format 20 on DXGI 42, 11 on 57, 5 on 62, 15 on 51,
  13 on 54, 56 on 28 are the same formats). They are storage-capable textures (render targets / UAVs) whose bindless
  index must lie below the storage count, 4,096: `a bindless image array is full; dimension 0` at world+41 s (t1),
  and the 1D array (4,096, sampled and storage) full before world entry. New line every 10 s (device shim, default):
  `bindless: N images live, M refused (an index array was full)`: t3 (off) 2,090 refused on the first line after the array
  filled, 109,752 by world+49 s (refusals are retries every refresh, not distinct T#s); t5 35,423.
- Same binary b6, `GTA6RT_METAL_BINDLESS_WIDE=1 GTA6RT_METAL_VIEW_SHARE=1 GTA6RT_METAL_TSHARP_TWINS=1` (my
  predecessor's switches): **0 refused** (t4/t6/t8: 34,881 images live, 5,596 T#s took a shared view), "no view"
  15,915 -> 202 a 10 s window (t7 -> t8). Format twins are a small class (10-11 made, 13-14 binds, t6/t8).
- Presented frames t7 (off) / t8 (on) are of different moments of the same view; both show the blocks, sawtooth and
  dark tiles; no visible difference attributable to the switches was established, so **nothing is default-on**.
  Proposal: make WIDE + SHARE default after one same-camera pair (I expect them to matter for post-process inputs read
  after world+40 s and for every 1D T#).
- `GTA6RT_METAL_TSHARP_PLACEMENT_FORMAT` (a format-preferring pick among same-address placements): 0 picks in t1, removed.

### 3. Minimap, water, albedo (not fixed)

- **Minimap (offline + logs):** drawn by mesh pipelines `(MSNC_VS_TransformUnlit_65e26d92` + `PS_MinimapDraw_65e26d92`
  and `(MSNC_VS_TransformUnlit_1c76317a` + `%PS_MinimapWaterDraw_1c76317a` (vector shapes, colours from constant data,
  no textures; 2 BGRA8 targets + D32S8), plus `!VS_TransformMap`/`PS_MapBGSheen` (made, in the tier). The mesh shaders
  are in the store (route hw) but in no tier (`build/pc7/arch-world/pcache-tier.txt` has none), so they compile at first
  use; the skip census of snap1c after the hand-over shows no skipped mesh draw except split64 TG-memory (5/frame) and
  conservative raster (1/frame). Whether the minimap draws run and what the HUD samples was not measured: no run of
  mine reached the HUD.
- **Water:** `PS_ResolveWaterSurface` (360 vertices = 60 tiles, indirect) leaves both targets 0.0% non-zero (t8 dump).
  Its 40+ T#s sit at s8+0x300..0x12e0 (my table trace covered 0x20-0xc0); not traced further.
- **Albedo:** the "format" T#-made skips (223 in snap1c) are not textures (GFX10 formats 0/34/47/496 on 1x1/4x1 T#s
  with words like `7f003e03 ff003400 00000000 87fc0001`): resolved as nothing to do. Black-albedo shares by run vary
  with streaming time (t3-t8: 5.7-57% of the frame at different moments), so no on/off albedo effect was measurable;
  WIDE/SHARE do not touch sampled-only 2D textures (that range never filled: ~40k of 65,536 live).

### Files (shared, this round)

- `shims/d3d12_metal_device.cpp`: one line every 10 s in the title-texture refresh census (`bindless: ... refused`).
  Everything else I added there was removed again (placement switch, two ambiguity censuses).
- Unchanged from my predecessor and still opt-in: `GTA6RT_METAL_TSHARP_TWINS`, `GTA6RT_METAL_VIEW_SHARE`,
  `GTA6RT_METAL_BINDLESS_WIDE` (`src/metal_bindless.cpp`), `GTA6RT_METAL_INDIRECT_ARGS_DUMP` (`src/metal_replay.cpp`),
  `GTA6RT_METAL_WORLD_PRESENT_DUMP`'s 4th field (`src/metal_present.cpp`; dumps only while a scene probe exists, e.g.
  `GTA6RT_METAL_SCENE_LOG=1`).

### Next (ranked)

1. Per-dispatch colour dumps around `CS_ApplyCombinedDeferred` / `CS_SkyMain` for the tile block; why `TiledLighting`
   writes nothing (its rect-list indirect counts).
2. A same-camera pair for WIDE + SHARE, then default-on.
3. Water: trace `PS_ResolveWaterSurface`'s T#s (s8+0x300..0x12e0) and its tile list; minimap: a run that reaches the HUD
   (needs the machine-wired stop not to end runs at world+50 s).

## Round 15, submission-CPU worker (2026-11-27 14:10-16:20): CPU-only ceiling 3.9 -> 4.65 fps at the new defaults (within run-to-run spread); the resolve skip made the shadow copies 7x slower and is now off; the zero-copy pool mirror cuts shadows to 0.5 ms but draws a black world; CreatePlacedView's title threads -40% CPU

Tree `build/subcpu-x86_64` (attach.cmake super-build), binaries kept `gta6rt.b5` (15:31 sources) and `gta6rt.b6` (16:05:
b5 with the two defaults below flipped). Launcher `host/tools/s4_metal_world.sh` (slot + tmem probe lock, cap 14,848 with no
heavy job, `GTA6RT_METAL_CB_ABORT_MS=2000`, first-GPU-error stop, 4 Hz wired stop 9,728, machine guard 15,360), with
`GTA6RT_METAL_SHADOW_CENSUS=1 GTA6RT_METAL_PRESENT_CHECK=1 GTA6RT_METAL_PCACHE_PREBUILD=all`, hidden, no audio, private run
dir; scripts `build/subcpu-x86_64/chain*.sh`, summaries `tools/subsum.py LOG FROM_S` (ms a submission by part) and
`tools/tcpu.py THREADS.tsv META` (per-thread CPU, r12-cpuprofile's `threadcpu`). **0 failed command buffers, 0 GPU faults
in every run; gpuEvent-gta6rt 10 before and 10 after each of f..j** (the 10th is the wave-rounds probe's, 15:41). Runs
end on the machine-wired stop at world+46..66 s (f: machine guard), so every window is world+15..65 s: the tips still view
and the first gameplay seconds, not steady gameplay. Load 4.7-10.6 at the starts (other workers' runs and builds).
M = measured, I = inferred.

### 1. ms a submission by part, CPU-only ceiling (M, `subsum.py`, world+20 s to the end)

| run | binary, settings | subs/s | frames/s | subs a frame | heap refresh | heap lock wait | shadows | textures | replay | serial | CPU-only ceiling |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| cp-c (r12, ref.) | live 12:51 + C | | 2.3-2.8 | 34-40 | 3.55-3.75 | 4.6-8.2 | 2.1-2.4 | 1.2-1.6 | 1.1-1.3 | 7.5-10 | 2.8-4.0 |
| subcpu-a2 | b4, all new switches off | 100.2 | 3.97 | 25.2 | 3.33 | 5.38 | 2.19 | 1.34 | 5.17 | 12.03 | 3.29 |
| **subcpu-g (before)** | **b5, all off** | 117.5 | 4.58 | 25.6 | 3.30 | 6.05 | 1.41 | 1.05 | 4.23 | **9.99** | **3.91** |
| subcpu-f | b5 defaults of 15:31 (skip, memo, placed, wake on) | 75.7 | 2.05 | 36.9 | 4.42 | 9.26 | **10.02** | 2.09 | 4.84 | 21.37 | 1.27 |
| subcpu-h | b5, skip off (memo, placed, wake on) | 98.2 | 2.72 | 36.2 | 3.51 | 7.48 | 2.07 | 1.20 | 3.98 | 10.77 | 2.57 |
| **subcpu-i (after)** | **b6 defaults (placed, wake on)** | 132.3 | 4.87 | 27.2 | 3.12 | 3.82 | 2.03 | 1.21 | 1.55 | **7.91** | **4.65** |
| subcpu-j | b6 + `GTA6RT_METAL_TITLE_POOL_MIRROR=1` | 189.3 | 6.73 | 28.1 | 3.02 | 3.66 | **0.50** | 0.85 | 1.03 | 5.40 | 6.58, **world black** |

Serial = heap refresh x refreshes/submissions + shadows + textures + replay (lock waits out), as r12-cpuprofile. The g -> i
change (3.91 -> 4.65 fps) is mostly replay (4.23 -> 1.55), which nothing of mine touches: scene and load spread, not a
gain I can claim. The 6+ target is not reached with a world that draws.

### 2. Shadows (item 1, M)

- **What is copied** (census, `GTA6RT_METAL_SHADOW_CENSUS=1`, 5 s lines; slot-133 shadow buffers are not the bytes, title
  windows are): i 127 MiB tracked in 198 trackers, 92 MiB copied a submission in 1.7-1.9 ms: no pool page 17 MiB (86
  trackers), one consecutive pool run 41 MiB (18), several pool runs or mixed 73 MiB (94); 64 MiB of it the scatter
  windows. So ~85% of the copied bytes are on pool pages (the lead's premise holds at these settings).
- **Copy bandwidth:** offline (x86_64 under Rosetta, 4 threads, `scratchpad/bench/cmpbench.cpp`) 64 MiB copy 1.0-1.2 ms; 6
  or 8 threads 0.96; compare-then-copy-if-dirty 1.0-1.4 ms at 0-100% pages changed. **Comparing before copying saves
  nothing** (both are bandwidth-bound); there is no cheap dirty bit (mprotect costs ms in the title, r12-heaptrack; mach
  dirty bits do not clear). Not built.
- **Zero-copy pool mirror** (`GTA6RT_METAL_TITLE_POOL_MIRROR=1`, `title_pool_mirror`: a host `mach_vm_remap` of the pool
  pages behind a window, wrapped by a bytesNoCopy buffer): j 197 mirrors / 98 MiB, copies 20 MiB in 0.45 ms, shadows 0.50
  ms a submission, **but every world present is black (193 of 193, plane 0; HUD drawn after the hand-over, 3D black:
  `/Users/a/Desktop/Projects/GTA-VI/port/run/present/subcpu-j/world/world_present_001917.png`)**, GPU 97 ms a frame against
  231 in i. Cause not found (c2, b4 with the skip on, drew with 180 mirrors / 50 MiB). Stays **off**.
- **The resolve skip (item 4) was what made f's shadows 10 ms:** with it, the V# halves over 32 MiB no longer made the
  16 MiB windows that serve many small V#s; those V#s then made ~340 small windows over pages that fault on read: 408,787
  guarded-read faults recovered in f (Rosetta's exception server 19% of a core) against 1,399-1,910 in g/h/i. Its verify
  mode compared only the returned address, not the windows the attempt leaves behind.

### 3. Heap refresh (item 4, M)

- Cross-refresh V# memo (`GTA6RT_METAL_VSHARP_XMEMO`): exact (subcpu-b verify, b2: 782,582 reuses, 0 differ), but reuses
  ~4% of lookups (backings are added all the time with 64 KiB chunks); refresh 3.30 (g, off) vs 3.51 (h, on) ms. **Default
  now off.**
- Span skip (`GTA6RT_METAL_RESOLVE_SKIP`): 186,332 skips verified with 0 differing answers (subcpu-b), but the side effect
  above. `resolve_span_cap` (the most one resolve can back) is 32 MiB: pool spans/unions 32 MiB, copy and scatter windows
  16 MiB each (`kTitleWindowMax`; `title_pool_scatter_window`'s 64 MiB is a total budget, not a per-window size), stream
  extents 1 MiB; 0 (no skip) with in-place windows or geometry copies. Skips were ~14 a refresh, not r12-heaptrack's 1,213
  (pool wake already leaves those halves alone). **Default now off.**
- Where the refresh goes now (c2 heap line): pages 0.68-0.80, fixing 1.28-1.66, retries 0.54 ms; 600 V# halves a refresh
  get no records (empty), almost all spans under 32 MiB.

### 4. CreatePlacedView (item 2, M)

`LEONIDA_PLACED_ARGS_FAST` (default on, `d3d12_xs_device.cpp`: `proxy_create_placed_view_x`, `local_write_through`): the two
argument structs and a one-page write-through are read with a fault-safe copy before any VirtualQuery; only a copy that
faults asks the old questions. 3.4-6.6 M fast copies a run, 0 faulted. The 5 placed-view title threads: **16.5-16.7% of a
core each (9.6-9.7 sys) in g against 9.0-13.3% (4.0-6.7 sys) in f/h** (-0.2 to -0.37 cores of kernel time in all).
Vulkan path tests (same tree): `d3d12vk_fault_guard_test` 36/36, `_pso_accounting_test`, `_register_test`,
`_rts0_deserializer_test` pass; `_present_clear_test` skipped (no x86_64 Vulkan driver here); no Vulkan world run.

### 5. MONITORX/MWAITX wake (item 3, M)

The store the title's wait loop (exe+0x4fb0ad0) watches is the fence slot-12 mirror (`refresh_mirror`,
`d3d12_xs_device.cpp`, `GTA6RT_MONITOR_WAKE`, default on); no store site of it is in shims_core. The pump's list-value
writes land in `d3d12_xs_list_values.h` / `title_page_watch.h` (read-only for me): no wake there. In the title
(`GTA6RT_MONITOR_WAKE=count`, f): 540-601 stores per 10 s, 241-284 found a waiter; the MWAITX lines read the same with and
without (2-3 k of ~35 k waits a 10 s end by a write, the rest at the 200 us budget: the fence has not moved, the GPU is
the wait). Wake latency (offline `isa_zen_wait_test`, load 5.7): **poll p50 21.5 / p90 28.1 us -> wake p50 4.5 / p90 8.8
us**; ~27 wakes a second in the title, so < 1 ms a second (I). The title-side latency is not measurable from my files.

### 6. Pipeline-cache manifest (item 5): not done

The manifest (`cache/src/codec.cpp` `Manifest::load`) and `pcache.cpp`'s lists are read with `std::getline` (buffered).
The `istreambuf_iterator` reads in r12's samples are `src/metal_shaders.cpp` `MetalShaders::load_locked` (line 79, each
metallib; also 92 and 252): outside the files I may write. Fix for its owner: `stat` the size, one `read` into a sized
vector (or map the file, as the long-run worker's library cache does).

### Switches and files

- `shims/d3d12_metal_device.cpp`: `resolve_skip_mode`, `vsharp_xmemo_mode` (defaults now off; `=1` / `=verify`). Earlier in
  this round (announced 14:15/14:40): `title_pool_mirror*` (off), `shadow_census` (off), `refresh_shadows` (census line),
  `vsharp_xmemo_*`, `resolve_skip*`, `resolve_span_cap`, `backings_added`, `ConsoleHeap` fields.
- `d3d12_vulkan/shims/d3d12_xs_device.cpp`: `placed_args_mode` (`LEONIDA_PLACED_ARGS_FAST`, on; `=verify`),
  `local_write_through`, `proxy_create_placed_view_x`, `mirror_wake_mode` / `wake_mirror_waiters` / `refresh_mirror`
  (`GTA6RT_MONITOR_WAKE`, on; `=count`).

### Next (ranked)

1. The pool mirror's black world: one run with `GTA6RT_METAL_TITLE_POOL_MIRROR=main` (main windows only) to split main
   vs scatter mirrors; then the scene probe on the first G-buffer pass. If fixed, shadows fall to ~0.5 ms a submission.
2. Replay (1.5-5.2 ms a submission across runs) and the heap lock wait (3.7-9.3) vary more than any term I changed; a
   steady-gameplay window needs runs that outlive the 9,728 MiB wired stop.
3. The ~600 empty V# halves a refresh under 32 MiB (fixing 1.3-1.7 ms): attribute by reason before any skip.

## Round 15, long-run worker (2026-11-27 14:00-15:10): libraries over file mappings (-0.8 GiB malloc), pipelines made at first use (no hand-over step), table capped at 160 MiB; the 10-minute run is stopped by machine wired (9,728 MiB) at world+102 s, footprint 1.3 GiB under the cap; fmix32 rebuild scripted (1 h 41 min measured on 50 libraries + 86 pipelines), not run

Tree `build/lrun-x86_64` (attach.cmake super-build, trimmed after the round to `gta6rt.b2` (14:20, sha256 aed2df56...,
the binary of d2/d3/r2) and `gta6rt.b5` (15:01, the shared sources as left: + the auto prebuild rule), scripts, logs,
`keep/rundir` (pcache files); b1 14:09 and b3 14:50 deleted), launcher `build/lrun-x86_64/run_world.sh` (the ten-minute worker's: slot + tmem probe lock, cap 14,848 /
13,312 with a heavy job, CB abort 2000, first-GPU-error stop, fault_stop, 4 Hz wired stop 9,728, machine guard 15,360,
1 Hz machine log) behind `when_quiet.sh` (starts only with no heavy job and no gta6rt), summary `tools/minutes.py`.
**0 failed command buffers, 0 GPU page faults; gpuEvent-gta6rt 9 before and 9 after every run.** M = measured,
I = inferred. Other workers' runs, builds and prepare_shaders ran all along (load 3.6-9.6 at my run starts).

| run | binary, settings | world | ended | what it answered |
|---|---|---:|---|---|
| lrun-d1 | b1, everything new off, `MallocStackLogging=lite`, heap census at world+5/+70 | +23 s | CB abort (rc 5) | `heap` stops the process ~18 s; on resume the 2 s CB abort fires: **no in-process census (heap / malloc_history) is possible under the 2 s rule**; the world+5 s size census is kept (`census-d1/w5.heap.txt`) |
| lrun-d2 | b2, everything new off | +16 s | footprint cap 13,312 (another worker's ninja at start) | per-build Metal allocated (opt-in log), cold set seeded |
| **lrun-d3** | b2, everything new **off** (`GTA6RT_METAL_LIBRARY_FILE_BACKED=0 GTA6RT_METAL_PCACHE_PREBUILD=all GTA6RT_METAL_TABLE_CLUSTER_CAP_MB=0`) | **+52 s** | **wired stop 9,799** | the same-binary "off" half |
| **lrun-r2** | b2, **defaults** (all three on), walk 4 s + turn 2 s every 15 s from 150 s | **+102 s** | **wired stop 9,734** (footprint 13,524, cap 14,848) | the "on" half; HUD and Donnie's text from world+57 s |
| lrun-f5 | b5, defaults, a fresh run dir, 5 s of world | +5 s | watchdog | the auto rule picks "all" there (`prebuild auto: all (no pcache-cold.txt in this directory yet)`) and writes the cold list at world entry |
| lrun-r3 | b3 = b2 + each phase's opening 10 s prebuilt in window mode (archive hits too), defaults otherwise | +48 s | wired stop 9,761 | that variant: world+0 present lit, but 2,341 prebuilt, footprint 13,546 at world+51 (r2: 12,923), so **reverted** (the shared source is b2's rule) |

### 1. Same binary, off (d3) against on (r2), at world+30 / +52 s (M, footprint trace 1 Hz, layer report 30 s)

| | footprint | malloc in use | Metal allocated | graphics ledger | layer "other" (pipelines, libraries) | pipelines created | world presents (non-black / dumped) |
|---|---:|---:|---:|---:|---:|---:|---|
| off, world+30 | 13,393 | - | 7,874 | - | 1,150 | 2,563 | 4 of 5 (the 5th = the title's fade, world+50) |
| on, world+30 | 12,342 | - | 7,104 | - | 890 | 1,822 | 8 of 11 (world+5: black, entry; +50/+55: the fade) |
| off, world+52 | 14,072 | 2,597 | 8,174 | 4,490 | - | - | |
| on, world+52 | 12,923 (-1,149) | 1,766 (-831) | 7,322 (-852) | 4,223 | - | - | |
| on, world+93..104 | 13,433-13,650 | 1,796-1,803 (flat from world+60) | 8,187-8,423 | 4,437-4,469 | 1,018 | 2,025 | |

- Presented frames: the same scene, HUD, text and known faults in both (speckle, black minimap, dark block behind the
  head, the pale rectangle); r2's last present (world+95 s): `port/run/present/lrun-r2/world/world_present_001871.png`
  (still wrong: everything the round-14 list names). **One new cost: r2's world+5 s present is black** (d3's world+3 s
  is lit): at world entry 1,452 pipelines are made at their first use from the archive through 3 compile threads,
  180-229 of them not ready at world+7..10 s, longest wait 4.6 s; lit again by world+15 s. 0 draws skipped. Prebuilding
  each phase's opening 10 s during the phase before (r3) removed it but made 2,341 pipelines ahead and cost +0.6 GiB
  footprint and the wired stop at world+48 s: reverted.
- Frames/s (HUD median) 3.0 in d3 and r2 (2.0 in r3, load 7.8-9.6 at its start), GPU 300-350 ms a frame; ten-final's
  2.0 was an older build (patch C since).

### 2. Pipelines: what the hand-over makes, and the prebuild (M)

- **Libraries are not "other" and pipelines are small:** per-build Metal allocated (opt-in
  `GTA6RT_METAL_PIPELINE_MEM_LOG=1`, port/metal `PipelineCore::run_build`): archive hits at boot 0.12-0.19 MiB mean
  (median 0, a 16 MiB slab now and then; d1, 722 builds). In r2 "other" is 1,009 MiB at 1,980 pipelines (0.5 MiB each
  with their libraries' GPU part).
- **The hand-over (r2, world+57 s): 121 first uses in 10 s**, all archive hits made at first use in 15-70 ms each:
  player skinning (`PedTransformPreSkinD_PositionOnly`, `TransformSkinD_NoDeform`), cascade-shadow variants
  (`CascadeShadows_draw*`, `_PedTransformPreSkinD`, `SmhCascadeShadow` hair), 13 compute kernels (`lib_cs_0213x`,
  `0220x`, `00936`, `00948/9`, `03406`), foliage-skin and `TransformD_c5fd36bb` variants. "other" +123 MiB from
  world+32 to +63 (155 pipelines). **No +742 MiB step**: in ten-g that step came with `PREBUILD=all` (2,597 prebuilt,
  635 released unused, which returns nothing); its exact split was not measured again (d3 died before its hand-over).
- **Key churn (M, prepare_shaders on 86 tier pipelines of the fmix32 sample):** 36 of 86 distinct pcache specs were
  archive hits of an earlier one in the same fresh archive: they differ only in state that is not in the Metal
  pipeline (depth-stencil, raster) and Metal keeps one pipeline for them. So backend duplicates already cost nothing.
- **The title's 109,114 `PipelineState` (213 MiB, ten-c census):** 56,088 `CreateDerivedPipelineState` + 25,450
  `DeserializeGraphicsPipelineStateX` + compute: all made before world entry (calls from 14.7 s to 82.9 s, ten-g), not
  re-created in the world. Each is a 2,048-byte public view over one shared compiled object (`shared_native_graphics`
  keeps a weak map by description already). Not my file (device shim): **proposal** for its owner: hand out the live
  view for an identical (key + record) description with an AddRef when the title set no private data / name on it,
  or make the view a slim class (COM identity + record pointer + owner): -200 MiB malloc at boot. Not measured.
- **Prebuild, new default (`d3d12_metal/cache/src/pcache.cpp`):** a pipeline an archive or tier lists is made at its
  first use; only pipelines compiled (not found in an archive) in earlier runs of the directory (new
  `pcache-cold.txt`, written beside the manifest) are prebuilt, and only once their recorded first use is in the
  current phase and at most `AHEAD_S` away. r2: 22 prebuilt (was 2,597), 1,969 made at first use from the archive, 6
  compiled. **Its premise is that recorded pipelines are archive hits.** The submission-CPU worker's runs on my 14:10
  version (fresh run dir: everything made at first use, ~1,200 of them compiling, 102 s waits, every world present
  black, subcpu-b/-c1) showed where it fails, so the default is now **auto** (15:10): the window only when the
  directory already has a `pcache-cold.txt` (a run with this code reached the world there) and neither
  `GTA6RT_METAL_SHADER_OVERLAY` nor `GTA6RT_METAL_ARCHIVE_TIERS` is set; otherwise everything recorded is prebuilt as in
  round 14 (and the run writes the cold list). One `[rt] pcache: prebuild auto: window|all (why)` line says which.
  `GTA6RT_METAL_PCACHE_PREBUILD=window|cold|all` forces one.

### 3. Hash table (M)

- Cap exercised in the title (r2, `GTA6RT_METAL_TABLE_CLUSTER_CAP_MB` now defaults to 160 with bare FNV-1a, 0 off):
  38,902 keys in 4,194,304 slots (160 MiB), **5 keys refused** (their T#s read the fallback; which surfaces was not
  seen in the dumps), no doubling to 320 MiB in 104 s of world (ten-final doubled at world+106 s).
- **fmix32 for the Metal path: a rebuild, scripted, not run.** `src/metal_bindless.cpp`: `GTA6RT_METAL_TABLE_HASH=
  fnv1a_fmix32` builds the table with fmix32 (default fnv1a; a bad value aborts). `shaderc_msl/tools/shaderc_msl.py`:
  pass `hash_fmix32` (`--hash-kind`, default `SHADERC_HASH_KIND` else fnv1a) wraps every 8-word FNV expression of the
  lookups in `spvDescriptorFmix32()` and raises on an FNV basis it does not recognise. One command:
  `d3d12_metal/tools/rehash_world.sh [WORK]` (default `build/rehash-fmix32`; resumable, nice 15, 3 jobs, no new
  library job while a gta6rt runs, prepare_shaders with 1 compile while one runs; `rehash_world.py`, `rehash_check.sh`).
  It resolves the tier's 2,674 shader names through the default overlay order (all resolved), 2,321 libraries: 1,764
  with lookups (620 from the overlays' kept MSL, 1,142 rebuilt from the corpus SPIR-V with their recorded options,
  each row's original key recomputed first, 2 `.msl` rows); the rest keep their metallib. Then the 2,595 tier pipelines
  that use a changed library go to `WORK/arch`.
- **Exactness (M):** the overlays' kept `.metal.gz` compile to their shipped metallibs byte for byte (6 of 6 checked),
  so a rehashed library differs only by the wrap; the wrap + helper equal `descriptor_table.h`'s
  `hash_descriptor_key(fnv1a_fmix32)` on 4 M random keys (0 differ, `rehash_check.sh`); 33 of 33 SPIR-V rows
  reproduced their recorded key before the pass was added.
- **Time (M, 50 libraries with lookups, serial, nice 19, one other world run going, load 5-7):** 17 MSL rows 34.9 s
  (2.06 s each), 33 SPIR-V rows 216.4 s (6.56 s each), 0 failed; tier: 86 pipelines of those rows 310.9 s with one
  compile at a time (50 compiled, 6.2 s each; 36 archive hits), 0 failed. **Estimate: libraries 620 x 2.06 + 1,142 x
  6.56 = 8,770 s serial = 49 min with 3 jobs; pipelines 2,595 x 50/86 x 6.2 s = 9,350 s serial = 52 min with 3 jobs;
  about 1 h 41 min on a machine with no world run** (I: linear in jobs; while world runs go on, library jobs pause and
  the tier compiles one at a time: ~3.5 h). Not started: it could not finish and be verified in a world run within
  this task. Run after it: `GTA6RT_METAL_TABLE_HASH=fnv1a_fmix32 SHADERC_HASH_KIND=fnv1a_fmix32
  GTA6RT_METAL_TABLE_CLUSTER_CAP_MB=0 GTA6RT_METAL_SHADER_OVERLAY=WORK/overlay.tsv:<default list>
  GTA6RT_METAL_ARCHIVE_TIERS=<port>/build/pc7/arch-world:WORK/arch` (I: 38.9 k keys in 2^16 slots, 2.5 MiB:
  -157 MiB wired, -315 MiB footprint against the capped table).

### 4. Malloc (M)

- Census: in-process tools stop the title (d1: `heap -s` 18 s), which the 2 s CB abort ends; MallocStackLogging lite
  starts under Rosetta ("pages will be included in process footprint") but `malloc_history` needs the same pause, so no
  call-site census. Size classes instead, d1 world+5 s (`heap -s`) against ten-c world+93 s: blocks 48 KiB-1 MiB 1,910
  (306 MiB) -> 2,982 (691 MiB), > 1 MiB +107 MiB (the table's CPU copy 160), <= 48 KiB +92 MiB.
- **Top layer-side term: every Metal library keeps a malloc copy of its metallib.** port/metal
  `create_library_metallib` gave Metal a `dispatch_data` with the DEFAULT destructor (a copy), and Metal keeps it
  (`d3d12_metal/tools/lib_data_probe.mm`, 800 world metallibs, 129 MiB on disk: footprint +145.5 MiB as a copy, +1.2
  MiB over a read-only file mapping, +5.4 via `newLibraryWithURL`). **Fixed (default on):** the bytes go to a
  content-addressed file (`GTA6RT_METAL_LIBRARY_CACHE`, default `$TMPDIR/gta6rt-metallib-<uid>`, 684 MiB / 1,984 files
  after these runs; written once, temporary name + rename, never modified, compared after mapping) and Metal gets a
  read-only `MAP_PRIVATE` mapping (`GTA6RT_METAL_LIBRARY_FILE_BACKED=0`: the copy). r2: malloc flat at 1.80 GiB from
  world+60 s (d3: 2.60 GiB at world+52; ten-a 2.61 at +71). A cleaner variant for the store owner: `newLibraryWithURL`
  on the store's own path (needs a path in `gpu::Device`'s API and `metal_shaders.cpp`), no second copy on disk.

### 5. The 10-minute run (r2, M)

| world minute | footprint (MiB) | malloc | Metal allocated | graphics ledger | layer "other" | machine wired peak | compressor peak | wired + compressor peak | frames/s (HUD median) | frame ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0-1 | 9,240-13,894 | 1,243-1,776 | 5,555-8,034 | 2,418-4,777 | 108-890 | 9,428 | 5,027 | 14,116 | 3.0 | 384 |
| 1-1.7 | 13,398-13,650 | 1,769-1,803 | 7,796-8,423 | 4,363-4,469 | 1,013-1,018 | 9,644 | 5,233 | 14,783 | 3.0 | 322 |

**Ended at world+102 s on the 4 Hz machine-wired stop (9,734 >= 9,728), not on the footprint cap** (13,524 of 14,848).
Machine wired was 2,900 MiB before the game; 9,4xx-9,6xx through the second minute (ten-final 9,039 / 9,612; d3
9,799 at world+52). So with footprint off the critical path, **machine wired is what ends runs now**, and none of my
changes moves it much (the table's GPU copy is 160 MiB, pipelines' code ~1 GiB in "other"; the rest is title pool
pages resident, title copies 1.2 GiB, render targets 1.05 GiB, driver scratch in the graphics ledger: not my files).
Walk/turn inputs started at world+77 s (process 150 s); the run ended 25 s later, so no walking minutes measured.
verify-s4: not updated (no run lasted).

### Switches and files (this round)

- Default on, same-binary off/on above: libraries over file mappings (`GTA6RT_METAL_LIBRARY_FILE_BACKED=0`), the
  prebuild window under the auto rule (`GTA6RT_METAL_PCACHE_PREBUILD=all|cold|window`; auto = r2's window in a
  directory with history and default shaders, d3's "all" otherwise), the table cap 160 MiB with bare FNV-1a
  (`GTA6RT_METAL_TABLE_CLUSTER_CAP_MB=0`). Off: `GTA6RT_METAL_PIPELINE_MEM_LOG`, `GTA6RT_METAL_TABLE_HASH` (fnv1a).
- port/metal `src/mtl_device.mm` (`create_library_metallib` + static helpers `library_file_backed`,
  `library_cache_dir`, `library_hash`, `map_library_file`, `file_backed_library_data`), `src/mtl_pipeline.mm`
  (`PipelineCore::run_build`); `cache/src/pcache.cpp` (`State`, `startable_locked`, `note_cold_locked`,
  `write_locked`, `poll_locked`, `start`), `cache/include/pcache.h` (comment); `src/metal_bindless.cpp`
  (`table_hash_kind`, constructor, `cluster_refused`); `shaderc_msl/tools/shaderc_msl.py` (`hash_fmix32`,
  `msl_fixups`, `prepare`, `main`); new `d3d12_metal/tools/lib_data_probe.mm`, `rehash_world.py`,
  `rehash_world.sh`, `rehash_check.sh`.

### Next (ranked)

1. Machine wired (the stop at 9,728 MiB): an attribution of the game's ~6.7 GiB wired (title pool pages resident,
   title copies, RTs, driver scratch, pipelines) with `footprint` snapshots outside the 2 s CB rule's reach, then the
   texture-copy use stamp (round 14, 4) and the tmem worker's scratch.
2. Run `rehash_world.sh` on a quiet machine, then a world run with fmix32 (-157 MiB wired, -315 MiB footprint).
3. World entry with the prebuild window: one black present at world+5 s while 1,452 pipelines are made from the
   archive. Prebuilding the phase's whole opening (r3) costs too much; next try: more compile threads for demand
   (archive hits are 15-70 ms each) during the world's first 15 s, or a recorded "used in >= 2 of the last runs" set.
4. The device-shim proposal for the 109 k public PSO views (-200 MiB malloc).

## Round 13, compute-speed worker, continued (2026-11-27 11:20-13:10): the "~100 ms of encoder overhead" is the per-dispatch timer's own serialization; real encoder boundaries cost <1 ms a frame; the lossless passes make the hot compute kernels slower or no faster in the world, so nothing ships default

Tree `build/cspeed-x86_64`, binary `gta6rt.b2` (live sources of 11:45 + my changes; the same binary in every A/B).
Runs cs-e..cs-k, each alone (slot + probe lock), hidden window, imported save, `GTA6RT_METAL_CB_ABORT_MS=2000`, cap 14,848
(13,312 in cs-e: a heavy job ran), 4 Hz machine-wired stop at 9,728 MiB, fault_stop. **0 failed command buffers, 0 GPU
page faults, gpuEvent-gta6rt 9 before / 9 after every run and every offline probe batch.** cs-f1 ended on our own 2 s
command-buffer abort at world+2 s in the tiled-lighting command buffer (180 rect draws; pfix-a hit the same, not my
code: no capture had fired yet); cs-i never left the title's video/menu (rc 124, discarded). Load 4.4-7.6 at the
starts; other workers' runs shared the slot, so run windows are world+10..+60 s. M = measured, I = inferred.

### 1. Encoders (item 2) (M)
- Compute encoders per world frame (pass timer, encoders serialized, windows of 2 frames): **cs-g 62.6 on average
  (29-109, 8 windows), cs-j 98.1 (5 windows), cs-e 76.5 (1 window)**, 92-157 dispatches/binds ("work") an encoder. What ends them (per frame, cs-g windows): end of command
  list 13-49, an indirect draw's pre-pass/render work 11.5-33, a draw 2-15.5, copy_buffer 1-6, barrier 0.5-4.5,
  dispatch 0.5-1. Barriers do **not** end a joined compute encoder (an in-encoder `memoryBarrier`), debug labels are
  signposts, resource declarations are per encoder only. So consecutive dispatches already share one encoder.
- **The 183 imposter dispatches (`lib_cs_00906`) already run in ONE compute encoder** (`lib_cs_01402 .. 00906`,
  215-252 dispatches, **0.04-0.9 ms** an encoder; gp-d, mspeed-e/-h TSVs). The 5.16 ms a frame of my 09:xx table
  (`build/cspeed-x86_64/notes/cs-b-table.md`) came from `GTA6RT_METAL_DISPATCH_TIMING`, which gives every timed dispatch
  its own encoder that waits for all before it; the "~100 ms of per-encoder overhead" was that artefact.
- What a boundary really costs (offline probe `shaderc_msl/tools/cspeed_encprobe.mm`, idle GPU, trivial 64-thread
  kernel, best of 7): 0.32-0.34 us a dispatch in one concurrent encoder, 3.9-4.2 us with a buffer+texture barrier
  between dispatches, **7.1-10.4 us with one encoder per dispatch and a fence chain** (our join). At ~63 compute
  encoders a frame that is **~0.5-0.7 ms a frame** (I). Merging encoders across command lists ("end of command list",
  the largest cause) is `Encoder::run` in port/metal `mtl_queue.mm`, outside my area; it can buy at most that.
- New report line (pass timer only, `PassTiming`): `[metal] pass timing by kind C|R|B, per frame: N encoders, ms,
  work, ended by ...`.
- Before/after: no encoder change shipped (nothing to win), so encoders a frame are unchanged.

### 2. Capture and offline A/B (item 1) (M)
- The encode-time capture (`GTA6RT_METAL_CS_CAPTURE`) works for kernels whose inputs the CPU writes, but the indirect
  ones (00977, 00974, 01267) read tile lists / records that earlier GPU work writes: offline they wrote **0 words**
  (no verdict, as for the tmem worker). New opt-in `GTA6RT_METAL_CS_CAPTURE_GPU=1` (`metal_replay.cpp`
  `cs_capture`): every captured range that is a layer buffer (154-225 of 274-761 ranges) and the indirect record are
  copied on the GPU right before the dispatch (two transition barriers around the copies, so the encoder splits
  there) and written 3 s later; indirect group counts now come from the GPU (00977: 1,101 groups, 01267: 17,168).
  Textures are still synthetic (random, 3 classes) in the A/B.
- `shaderc_msl/tools/cspeed_ab.mm --shared-writes`: one title-sized write texture per writable binding at every slot
  (2048x2560 / 160^3 / 16,384) and an A-vs-A run first (0 words differ in every case: deterministic).
- Variants (`shaderc_msl/tools/cspeed_overlay.py`, store recipe + passes; `build/cspeed-x86_64/var/*.jsonl`): "all" =
  merge_typed_reads (01905/01921: 50 ops, 01903/01919: 41, 00977: 200, 00974: 150, 01267: 26, 01315: 23) + fp_inline
  + hash_memo (+ simd_votes on 00977/00974: 120/140 votes); chain_segments / round_counts_simd / barrier_tg_only do
  not apply (these kernels write device memory/textures, or have no PC switch). "merge" and "fph" (fp_inline +
  hash_memo) alone.
- Identity (store row vs variant, captured inputs, `build/cspeed-x86_64/ab-res*.tsv`): **0 words differ everywhere**;
  verdicts only where A wrote something: 01921 (1,216-1,280 words), 01905 (1,216-1,280), 01903 (11,712-138,576), 01919
  (1,280), 01315 (5,962-8,455 incl. heap) = verified for "all". **00977, 00974, 01267: still 0 words written** with
  GPU-time buffers and title-sized targets, so not verified (I: their early-out reads a texture, which is synthetic).
  So 00977/00974 are **not** added to tmem.
- Offline timing on these inputs says nothing useful: 0.8-6.7 ms a dispatch with A and B within noise both ways
  (the title runs 01921 at 14.7 ms a dispatch; synthetic textures change the loop trip counts).

### 3. World A/B, same binary b2 (item 3) (M, `GTA6RT_METAL_DISPATCH_TIMING` for the 8 kernels every 13 s + pass timer)

| kernel | off cs-g: ms a dispatch (n) | off cs-j | "all" cs-h |
|---|---:|---:|---:|
| 01905 volumetric gather 2 | 17.84 (20) | 17.93 (16) | **26.36** (15) |
| 01921 gather 18 | 14.66 (20) | 13.56 (16) | 15.27 (15) |
| 00977 cascade shadow tiles alpha | 13.49 (19) | 16.32 (17) | **21.00** (16) |
| 00974 cascade shadow tiles | 7.91 (19) | 9.92 (17) | 6.70 (16) |
| 01903 gather 0 | 4.35 (20) | 4.30 (16) | **6.51** (15) |
| 01919 gather 16 | 3.90 (20) | 3.60 (16) | 3.88 (15) |
| 01315 radiance spatial filter | 3.62 (19) | 4.21 (17) | 3.49 (16) |
| 01267 final gather | 2.64 (19) | 1.97 (17) | 2.03 (15) |

Groups a dispatch matched (01905: 284 in all three). Off-to-off spread: 01905/01903 within 1%, the others 8-25%.
**Result: "all" makes 01905 +48%, 01903 +50% and 00977 +29..56% slower** (outside the off-to-off spread; I: the
noinline helper calls of merge_typed_reads in the hot loops); 01921/01919/01315/00974/01267 are inside the spread
(no gain shown). Whole compute a frame (pass timer, encoders serialized, 5-8 windows of 2 frames): **cs-g 106.0 ms
(24.6-165.0), cs-j 146.4 (91.0-244.6), cs-h 62.7 (0.1-115.1)**: the scene differs window to window (cs-h's windows
included loading/lighter views), so whole-frame compute is no comparison here; the per-dispatch table is. The "fph"
world A/B (fp_inline + hash_memo only on the gathers, merge only on 00977/00974; `overlay-i.tsv`, archive built) was
not obtained: cs-k1 ended on the same world-entry tiled-lighting 2 s abort, cs-i/cs-k never left the title's video /
menu (load 6-8, cap 13,312 with a heavy job).

### Delivered (none default)
- `build/cspeed-x86_64/overlay.tsv`: 3 rows verified on captured inputs and not slower in the world (01921, 01919,
  01315, "all"); no measured gain either, so **not proposed as default**. Pipelines for all variants built (0 refused)
  in `build/cspeed-x86_64/arch` (2 shards); runs with it need
  `GTA6RT_METAL_ARCHIVE_TIERS=<port>/build/pc7/arch-world:<port>/build/cspeed-x86_64/arch`. Other row sets:
  `overlay-all.tsv` (8 rows, cs-h), `overlay-i.tsv` (fph + merge, unmeasured).
- tmem: 00977/00974 **not** added (unverified: 0 words written on captured inputs; also "all" was slower in the world).
- Switches: `GTA6RT_METAL_CS_CAPTURE_GPU=1` (with `GTA6RT_METAL_CS_CAPTURE`), pass-timer line by kind. Captures of
  this run: `build/cspeed-x86_64/cap2/*.cap` (GPU-time, 00977/00974/01267/01921/01315); configs:
  `shaderc_msl/tools/cspeed_capcfg.py MSL CAP OUT`; A/B: `build/cspeed-x86_64/ab_batch.sh` (CFGDIR, ABX=--shared-writes).
- Tools: `shaderc_msl/tools/cspeed_overlay.py` (build / rows), `cspeed_ab.mm` (+ `--shared-writes`),
  `cspeed_encprobe.mm`; `build/cspeed-x86_64/{ab_world.sh,cmp_runs.py,kern_runs.py}`.

### Next (ranked)
1. 01905/01921 are latency-bound, not throughput-bound: 01921 runs 36-38 groups of 32 threads (1,216 threads on a
   20-core GPU) for 13.6-15.3 ms, ~400 us a group; 01905 284 groups, 62 us a group. A per-thread chain of ~6 loops with
   texture samples and a bindless hash per access. Passes that keep code size do not move it; it needs the translator
   (fewer emulated registers live, resolved descriptor indices instead of a hash per access) or more parallelism.
2. The fph A/B in the world (one run with `overlay-i.tsv` first, the per-dispatch table above as the baseline).
3. The world-entry tiled-lighting command buffer can exceed 2 s (cs-f1, cs-k1, pfix-a): with `CB_ABORT_MS=2000` it
   ends ~1 in 4 of my runs at world+2 s. Cache/lighting owners: split that command buffer or raise the abort only for
   the first world seconds.
4. Merging compute encoders across command lists (port/metal `Encoder::run`): <= ~0.7 ms a frame (I).


## Round 14, textures / UI-text worker (2026-11-27 11:16-12:55): black albedo halved by decoding 2D title T#s without their word-4 data; T# textures on a worker, ready at world entry; menu text whole; 300 frames not shown

Tree `build/txt-x86_64` (attach.cmake super-build, `-build1..8.log`; build outputs deleted at the end, kept: the A/B
binaries `gta6rt.b6`, `gta6rt.b7` and `gta6rt.play1241` = the canonical `build_play.sh` binary of 12:41 with the new
defaults; pass one as `S4_BIN`), launcher `build/txt-x86_64/run_world.sh` (pfix's with
this tree's run dir, the tmem probe lock, cap 14,848 with no heavy job else 13,312, `GTA6RT_METAL_CB_ABORT_MS=2000`, 4 Hz
machine-wired stop at 9,728 MiB, gpuEvent count; new: `TXT_SCRIPT` input script, `TXT_TPL` run-dir template
(`rundir-fresh`: an empty profile), `TXT_RO_SAVES=1` read-only private save dir), `build/txt-x86_64/summ.sh RUN` (the
lines read below). Runs txt-m1..m3 (menus only), txt-a..g, h1, p1 (world), each alone (slot + lock), hidden window, no
audio, imported save unless said. **0 failed command buffers, 0 GPU page faults; gpuEvent-gta6rt 9 before and 9 after
every run.** The world runs ended 20-70 s into the world on the footprint cap (13,312 whenever another worker built,
14,848 otherwise) or, h1, on the new machine guard (wired + compressor 15.4 GiB). M = printed by a log, a readback or a
tool; I = inferred. Load 3-7 at run starts.

### 0. New measuring tools (opt-in, diagnostics only)

- `GTA6RT_METAL_SCENE_ALBEDO=<n>` (`SceneProbe::albedo_copy/albedo_report`, `src/metal_context.*`): every n-th present
  the G-buffer's albedo (DXGI 29) and normal targets are copied at the end of each G-buffer pass (the last copy of the
  frame stays) and `scene albedo: ... albedo non-zero, geometry, geometry with black albedo` is logged; with
  `GTA6RT_METAL_SCENE_ALBEDO_PNG=<dir>` every 6th sample is a PNG with black-albedo geometry in magenta.
- Census (`GTA6RT_METAL_TEXTURE_CENSUS=1`): the "a texture is registered there now" skip is split by what is there
  (slot-133 texture without an image at its address / of another shape; a T#-made one of this shape with or without its
  image), and T#s met inside a registered texture's range are classified (a level of it / behind its used levels /
  where its own levels lie), the first examples logged.
- `refresh_title_textures`: a line when the title memory map changes over an in-place texture (none seen: the glyph
  question below).

### 1. Black albedo (M)

**What the T#s inside registered slot-133 ranges are.** Runtime census (txt-a, c, e): "a level of it" 17-217, every one
a false match at level 0 (a T# of the registered texture's own shape at its level-0 offset = another texture packed
there); "another texture where its own levels lie" 24.9k-48.6k (the registered texture holds only its small levels;
no title T# ever named a base level for it, so its whole layout counted as used); "behind its used levels" 0-236.
**No T# is a mip of the registered texture, so no mip-offset view applies** (also what pfix's offline dump said). The
fix for this class is pfix's `GTA6RT_METAL_TSHARP_INSIDE` (made textures for them), which made every world present
black once the textures came fast (txt-b: exposure non-finite at every latch): textures made over bytes the title had
not streamed yet are zero, and a zero BC5 normal map decodes to a non-unit normal (I: NaN in lighting, one non-finite
texel and the title's exposure is black). **New `GTA6RT_METAL_TSHARP_ZERO_CHECK`**: no texture for a T# whose largest
level is still all zero bytes (the T# comes back at its next miss). With it: txt-d 92 non-black world presents then the
title's fade, exposure non-finite 0/86; 1,048 textures made inside registered ranges.

**The large class: title 2D T#s whose word 4 holds data.** The heap line's "unbound T#s by reason" was led by "T# names
layers the texture lacks" (1.6-3.4k a 10 s window, txt-d). Examples: `7849eea0 cb500000 003fc07f 90060fac 00000002 ...`
(type 9 = 2D, BC7), `014bde65 c4700000 003fc03f 90000fac 47521000 ...` (2D, word 4 a float). `gfx10::decode` reads word
4's DEPTH/BASE_ARRAY as a layer range for every type; a 2D (or 1D) sample carries no slice, so the hardware never reads
them, and the title keeps other data there. **New `decode_title_tsharp`** (`GTA6RT_METAL_TSHARP_2D_LAYERS`): such T#s are
decoded with those fields cleared (the words stay the hash key); used by `bind_title_image` and `make_orphan_textures`.
863k T# decodes took the cleared path in txt-f's 25 s of world; "names layers" fell to 22 a window.

| run (binary) | switches | world presents (black) | albedo non-zero | geometry with black albedo (of frame / of geometry) |
|---|---|---|---:|---|
| txt-e (b6) | none of mine | 52 (0), cap at world+30 s | 26.0-31.9% (frames 27-33) | 42.9-46.9% / 57-64% |
| txt-d (b6) | worker, inside, zero check | 92 (0) + the title's fade | 28.0% (15) -> 43.1% (79) | 47.3% -> 32.3% / 63% -> 43% |
| txt-g (b7) | worker, inside, zero check | 101 (0) + the fade | 28.2-33.1% (35-60) | 43.7-47.7% / 57-63% |
| **txt-f (b7)** | **+ 2D layers** | 18 (1, world entry), cap 13,312 at world+20 s | **50.3-50.4% (10-13)** | **25.2% / 33.4%** |

Same-binary pairs: e/d (b6) and g/f (b7). Images (same camera, the stilt-house lot, magenta = geometry with black
albedo): before `/Users/a/Desktop/Projects/GTA-VI/port/run/present/txt-g-albedo-before-2d-layers.png` (frame 35, 43.7%),
after `/Users/a/Desktop/Projects/GTA-VI/port/run/present/txt-f-albedo-after-2d-layers.png` (frame 10, 25.2%): the stilt
house's siding, decks, stairs and railings, the dock, seawall and much of the ground are textured. **Still wrong in it:**
see the table below. f's run is short (another worker's build held the cap at 13,312), so its present count is 18.

**What is still black, by pixel coverage** (txt-f frame 10, magenta pixels in disjoint boxes,
`scratchpad/txt/tools/magenta.py`; the same boxes in g / d frame 79 / e for comparison):

| region | f (all on) | g | d (79) | e | reason (evidence) |
|---|---:|---:|---:|---:|---|
| palm trunks (right + left) | 5.1% | 2.0% | 3.7% | 6.5% | not streamed by the title at that moment: textured in g and d's frame 79, black in e/f's early frames (I: streaming order) |
| beach | 4.8% | 10.4% | 1.5% | 6.0% | not streamed yet: textured by d's frame 79 (M, same run) |
| left house walls + the bare tree in front | 6.2% | 7.2% | 7.5% | 11.2% | ours, cause not found: black at every frame of every run |
| yard ground + dock | 5.9% | 15.8% | 10.3% | 12.0% | mostly fixed by 2D layers; the rest ours, not found |
| stilt house walls + roof | 2.2% | 3.9% | 4.6% | 5.4% | the roof and some siding: ours, not found |
| under the house + deck | 1.0% | 4.4% | 4.4% | 5.6% | mostly fixed by 2D layers |
| poles / sky line | 0.1% | 0.0% | 0.2% | 0.2% | |
| **all** | **25.2%** | 43.7% | 32.3% | 46.9% | |

Candidates for "ours, not found" (M counts, I link): T#s still unbound in txt-f: "no view for the T#'s format or range"
15-96 a window (BC textures viewed in another format: "a block-compressed texture views only as its own format or its
sRGB twin"), "T# type or samples do not fit" (1D T#s on 2D textures, 589-1,262 a window in d), T#-made skips "format"
179 (a format code `dxgi_of_gfx10` lacks), "description the XG model refuses" 447. Not attributed per pixel: needs the
draw trace on the walls' G-buffer draws (next).

### 2. Texture build time: the T# texture worker (M)

`GTA6RT_METAL_TSHARP_WORKER` (`TsharpBuilder`, `kick_tsharp_builder`, `run_tsharp_builder`, device shim): the refresh pass
kicks a worker that runs `make_orphan_textures` and the first fill of each T#-made texture (`tsharp_fill_queue_`, filled
from `take_tsharp_wanted`) in slices of `GTA6RT_METAL_TSHARP_SLICE_US` (4000) holding the registry lock, then releases
it for 1 ms; the pass leaves those textures alone. No queue work: their Metal textures are shared, a fill is the
worker's CPU write (`write_region`), so no blit was needed. `make_orphan_textures` now runs the checks that need no bytes
(registered there, budget, pool, overlaps) with SW_4KB_S's span before the bytes are scored, and again only if the score
picks another layout (scored 37.6k for 13.3k made before, pfix-l0; 19.6k for 21.6k made with the worker).

| run | T#-made textures at world entry | world+10..30 s | worker busy |
|---|---:|---:|---|
| txt-a (b2, off) | 7,247 | 14,061 (+25 s) | - |
| txt-e (b6, off) | 6,779 | 16,123 (+30 s) | - |
| txt-c (b4, worker) | 17,495 | 18.6k (+30 s) | 1.6-4.7 s of 10 s at its peak, 0.3-0.5 s later |
| txt-d (b6, worker+inside+zero) | 20,078 | 20.9k | 3.7-5.0 s of 10 s |

Most of these textures are made during the story load (its loading screen): without the worker ~7k are ready at world
entry and the backlog is worked off over the first ~30 s of the world, 1.5 ms a pass (the "made for a T# of this shape;
its image is there" skips, 40-53k, are that backlog: T#s queued before a twin made the texture); with it ~17.5-20k are
ready at world entry. World frame rate 1.6-2.4 presents/s with and without (machine load 4-7). **Time until albedo
coverage stops rising:** not reached in any run: it rose until each run's end (d: 28% at world+16 s to 43% at world+62 s)
through what the title streams (beach, seawall, dock, trunks), not our making (done by world entry with the worker).

Bug found on the way (fixed before d): with the worker, T#-made textures are created stale and the pass no longer
cleared that, so `take_tsharp_wanted` skipped them as "already stale" and nothing filled them (txt-b, "0 filled").

### 3. UI glyphs (M; cause not found)

- Menu pages that need no world (fresh profile `TXT_TPL=rundir-fresh`, txt-m2/m3): NAVIGATION EXPERIENCE, UI TEXT SIZE,
  SUBTITLES, BRIGHTNESS, FINISH SETUP?, LEGAL NOTICES: every glyph of headings and body text renders (OCR and by eye).
  PNG: `/Users/a/Desktop/Projects/GTA-VI/port/run/present/txt-m3-legal-notices-all-glyphs.png` (nothing wrong in it).
- The font caches are the two in-place A8 textures (2048x2048, 1024x1024, `make_in_place`) in my runs **and in the
  round-9 runs whose COULDN'T AUTOSAVE body lost letters** (gates-d 03:02, d3m-s7-j 02:05: same two "title texture in
  place" lines), so the loss was not our copy of the atlas going stale; no map change over an in-place texture was
  logged in any run.
- The pages that showed it are gone: COULDN'T AUTOSAVE no longer appears (the lead's 04:50 save fix) and the credits
  need the prologue. Provoking it with a read-only save dir: the menus (fresh profile, txt-m3) showed no alert; in the
  world (txt-h1) the run ended at world+28 s (machine guard) still on the tips screen, before any autosave.
- Not verified (I): the 2D-layers decode above also applies to the font T#s; a text draw whose title-built T# carried
  data in word 4 would have been unbound (all its glyphs missing, not some). No before/after pair exists.

### 4. 300 frames, the hand-over frame

- Canonical build (`d3d12_metal/tools/build_play.sh` at 12:41, the four switches on), default settings, present check
  only (txt-p1, cap 14,848, no heavy job): world at +83.6 s, **111 consecutive non-black world presents (0 black), then
  the title's fade** to the hand-over, then the footprint cap (14,982 MiB) ended the run at world+~54 s as the hand-over
  began (2.4-2.6 presents/s). So **300 is not shown** (longest this round 111; every run of mine ended on the cap at
  the hand-over or earlier) and **no frame after the hand-over exists**; the ten-minute worker's run of 12:23 ended the
  same way (rc 4, world+2.7 min). Cost of the new defaults there (I from the ledgers): title copies 1,208 MiB at the
  hand-over (txt-g 1,122, d 1,234 with them; e 842 at world+30 s without): ~+0.1 GiB of T#-made textures.
- Best presented world frame at the default settings (before the hand-over, plane 0 under the UI plane):
  `/Users/a/Desktop/Projects/GTA-VI/port/run/present/txt-p1-defaults-world-present-1925.png`. Stilt house siding, decks,
  stairs, railings, dock and seawall textured, palms and flowers, sky. **Wrong in it:** the lens-flare band is back at
  rows ~250-280 of 720 (pfix's band; not mine, reported), the left house's walls and the yard still dark (section 1),
  the dark rectangle behind the tip box, the water bottom left blown out, grainy foliage.

### Switches (device shim unless said)

- Default ON now (12:41; `=0` restores): `GTA6RT_METAL_TSHARP_2D_LAYERS`, `GTA6RT_METAL_TSHARP_WORKER`
  (`GTA6RT_METAL_TSHARP_SLICE_US`), `GTA6RT_METAL_TSHARP_INSIDE` (pfix's, was opt-in), `GTA6RT_METAL_TSHARP_ZERO_CHECK`.
- Opt-in diagnostics: `GTA6RT_METAL_SCENE_ALBEDO`, `GTA6RT_METAL_SCENE_ALBEDO_PNG` (`src/metal_context.*`).
- Functions: new `decode_title_tsharp`, `base_floor_bytes_zero`, `note_registered_there`, `note_inside_registered`,
  `TsharpBuilder`/`kick_tsharp_builder`/`run_tsharp_builder`; changed `make_orphan_textures` (order of checks, slice
  argument, census keys), `bind_title_image` (decode), `take_tsharp_wanted` (worker queue), `refresh_title_textures`
  (kick, visit skip, in-place remap line), `log_texture_census` (counters). No port/metal change.

### Next (ranked)

1. The walls/roofs/ground still black (~14% of the frame): trace one wall draw of the G-buffer pass
   (`GTA6RT_METAL_DRAW_TRACE`) to name its T#s and why each is unbound; the open classes are BC textures viewed in
   another format and 1D T#s on 2D textures (bind refusals), format codes `dxgi_of_gfx10` lacks.
2. Glyphs: needs a run that lives to an autosave (read-only save dir) or the prologue's credits.
3. 300 frames: see 4.

## Round 14, ten-minute worker (2026-11-27 11:16-12:35): 64 KiB pool chunks are the default; the dropped held submissions were the backend's console-VA space running out (fixed); a command-buffer leak and the extra hash-table generations fixed; no world run lasts 10 minutes yet (footprint cap at the hand-over, world+54..162 s)

Tree `build/ten-x86_64` (attach.cmake super-build, `build/ten-x86_64-build1..6.log`; removed after the round except the
scripts, `gta6rt.play1200`, the ten-c heap census and key dump; launcher `run_world.sh` = host/tools/s4_metal_world.sh's
settings (GTA6RT_GPU=metal, cap 14,848 / 13,312 with a heavy job, CB abort 2000, the backend's first-GPU-error stop,
`--trace=0 --hud min --memory-report 30`, hidden, no audio, saves copied fresh, slot + tmem probe lock, fault_stop.sh,
4 Hz 9,728 MiB wired stop) + the second guard + a 1 Hz machine log `run/logs/<run>.mach.tsv` (epoch, wired, compressor
occupied, RSS); `heap_at.sh` / `vmmap_at.sh` / `mach_log.sh`). The canonical binary of 12:00 (`build_play.sh`,
sha256 90435d67...) is copied as `build/ten-x86_64/gta6rt.play1200` so on/off pairs use one binary. Runs alone, no heavy
job at any start, load 2.6-7.6 at the starts. **0 failed command buffers, 0 GPU page faults, 0 "not resolved" / "failed
when released"; gpuEvent-gta6rt 9 before and 9 after every run.** M = printed by a log, tool or probe; I = inferred.

| run | binary, settings | world presents checked | black | wired peak | wired + compressor peak | ended |
|---|---|---:|---:|---:|---:|---|
| ten-a | b3, 64 KiB, cold run dir | 236 | 60 at entry (1,043 first uses not ready) + 1 (fade) | 9,734 | 14,024 | wired stop, world+122 s |
| ten-b | b3, 1 MiB, warm | 78 | 0 | 9,841 | 13,716 | wired stop, world+41 s |
| ten-c | b5, 64 KiB, warm | 215 | 1 (fade) | 9,790 | 14,269 | wired stop, world+158 s |
| ten-d | canonical, default (64 KiB), tombstones off | 113 | 1 (fade) | 9,559 | 15,209 | footprint cap 14,914, world+57 s |
| **ten-final** | canonical, defaults, scripted walk + turn | (no check; s4_evidence: lit 4 of 5, the 5th the fade) | - | 9,612 | 14,432 | **footprint cap 14,860, world+162 s** |
| ten-g | canonical, `GTA6RT_METAL_TABLE_CLUSTER_CAP_MB=160` | - | - | 9,306 | 14,438 | footprint cap 14,859, world+54 s |

### 1. 64 KiB pool chunks: default since 11:57 (M)

`title_pool_chunk_bytes` = 64 KiB (`shims/d3d12_metal_device.cpp`; `GTA6RT_METAL_TITLE_POOL_CHUNK_KB=1024` restores 1 MiB).
Same binary b3 with the 10:38 RT-alias rules, present check on every world present: 64 KiB (ten-a) had 0 black presents
except at world entry, where ten-a's cold run dir left the lighting pipelines compiling (`pcache: world+10s ... 1043 of
all first uses not ready`), and the title's fade (1 frame, check 109); 1 MiB (ten-b, warm) 0 of 78, but its world
lasted 41 s (wired 9,253 MiB at world+10 s vs 7,908). b5 64 KiB warm (ten-c): 1 black of 215 (the fade). So nothing
black that the chunk size explains, and 64 KiB lives 3-4x longer under the wired stop.

### 2. Footprint growth, per second, and the layer terms (M; ten-a: footprint trace 1 Hz, malloc zones 1 Hz, gpu-memory 10 s, [gov] 5 s, launcher 1 Hz)

| ten-a world+s | footprint | malloc | graphics ledger | Metal allocated | title copies | "other" (pipelines, libraries) | descriptors | RTs | title pool touched | machine wired / compressor |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| +1 | 10,350 | 1,474 | 2,847 | 6,587 | 764 | 167 | 464 | 1,017 | 5,883 | 7,447 / 1,473 |
| +31 | 12,176 | 2,230 | 3,295 | 7,096 | 863 | 332 | 544 | 1,054 | 5,940 | 8,118 / 3,444 |
| +71 | 14,440 | 2,611 | 4,538 | 8,522 | 1,092 | 1,211 | 544 | 1,057 | - | 9,568 / 4,363 |
| +121 | 14,510 | 2,676 | 4,589 | 9,030 | 1,118 | 1,228 | 544 | 1,060 | 5,990 | 9,734 / 3,899 |

- **+1..+71 s (the world arriving and the hand-over): +58 MiB/s**: pipelines + libraries +1,044 MiB (cold run dir;
  warm runs hold ~0.95 GiB of them at world entry), malloc +1,137, title copies +328, the hash table doubling +80, RTs
  +40, title pool +103 touched. **+71..+121 s: +1.4 MiB/s** (malloc +1.3/s, graphics +1.0/s; ten-c +0.9 MiB/s from
  world+88 to +158 s): the 12-40 MiB/s is the world arriving, not a term that keeps growing at that rate.
- Metal allocated keeps rising ~10 MiB/s in the plateau with every tagged category flat: title-pool union buffers over
  pages already counted (Metal counts overlapping buffers per buffer; not footprint).
- **What ends the runs now:** footprint at the hand-over (ten-d/final/g) or machine wired after it (ten-a/c). At the
  hand-over "other" jumps +0.36-0.74 GiB within a few seconds (ten-g: 1,147 -> 1,889 MiB; `prebuild: 2867 known, 2597
  built` against 1,936 first uses: ~660 prebuilt pipelines never used, ~0.3 GiB, I from 0.47 MiB a pipeline), and in
  ten-final the hash table doubled 160 -> 320 MiB a generation at world+106..136 s (+320 MiB of footprint with its CPU
  copy) and took it over the cap 26 s later. After the hand-over machine wired grows 1.5-3 MiB/s (title copies +0.5/s,
  pipelines +0.3/s, the rest pool pages named as the player moves, I).
- Malloc, heap census of the live game (ten-c, world+93 s, `heap -s`: 3.63 M blocks, 2.5 GiB; vmmap: Malloc Large 1.6
  GiB in 3,300 blocks): **36,999 live `AGXG16XFamilyCommandBuffer` of 57,372 ever made** (a leak, fixed below);
  **109,114 `PipelineState` objects, 213 MiB** (the title's 56,088 derived + 25,450 deserialized PSOs; not my area);
  31,086 `TitleTexture` (76 MiB); 62,973 AGX textures for 61,195 MetalTextures; large blocks: the hash table's CPU copy
  (160 MiB), 2 x 38 MiB console-heap copies, 2 x 55.5, 82, 128 MiB, and 671 MiB in ~3,000 blocks of 48 KiB-1 MiB.

| layer term | measured | change (default on unless said) |
|---|---|---|
| command buffers of `MetalQueue::signal` | 36,999 alive of 57,372 (ten-c), ~200 more a second, each with its handler block and captured strings | `@autoreleasepool` around it (port/metal `mtl_queue.mm`): the title's Signal thread has no pool, so every autoreleased command buffer lived for the process. Not re-censused after the fix (the census run ten-g ended before its census) |
| hash-table generations | 3-4 live (wired-g 240-320 MiB) | a retired generation the queues passed is freed on every flush (was: only on the next generation-path flush): **1 live** in ten-a/b/c/final, 2 for moments (ten-d: 2 = 320 MiB at its end) |
| flushes to a new generation | wired-g 69 in 195 s (42 rekeyed, 14 refills too soon); ten-d (tombstones off) 43 in 58 s of world (23 / 10) | **tombstones** (`GTA6RT_METAL_TABLE_TOMBSTONES`, `=0` off): an erased key's slot holds a CPU-only key that hashes to that slot until the GPU passed the flush that zeroed it, so no key lands in a slot emptied in flight. ten-a 17 in 210 s, ten-c 18, final 23 (6-11 rekeyed = relocations, 0 refills too soon, the rest doublings); 47-50 k tombstones made and released a run, 0 misplaced. Unit test, both hash kinds: `build/ten-x86_64/tools/t.cpp`, 0 bad of 13,334 tombstones. Presents: ten-d (off) 1 black = the fade, ten-a/c (on) the same |
| in-place journal | grew between generation-path flushes | cleared on each in-place flush while one generation lives |
| hash-table size | 31-34 k keys in 4,194,304 slots (160 MiB a generation + 160 MiB CPU copy); 8,388,608 (320 MiB) in ten-final | not changed by default, see 2b |
| console VA (port/metal) | 64 GiB bump allocator, never reused; GPU-VA-only holds took console VA | see 3 |
| retired Metal textures awaiting completion | `gpu-releases ... waiting now 0-69 (0 MiB)` | none needed |
| view / texture registries | image entries 3.1 k -> 33.6 k, title textures 1.5 k -> 29-31 k, replaced images 835 -> 1,022 over the world: they follow the title's resident set (new line `[rt] gpu-memory sizes`) | none needed |
| census / recorder buffers, logs in memory | census off by default; unbound-T# tracker capped at 262,144 slots; released-backing log at 65,536 | none needed |

#### 2b. Why the hash table is 160-320 MiB for 33 k keys (M, offline)

`GTA6RT_METAL_TABLE_DUMP=path` (new, off) writes the keys every 10 s. ten-c's 32,956 keys replayed into fixed-size
tables (the table's own insert, the shaders' 8-probe window; `build/ten-x86_64/tools/sim.cpp`): refused at 2^16 slots 4,086
(12.4%), 2^18 1,800 (5.5%), 2^20 (40 MiB) 588 (1.8%), 2^21 (80 MiB) 33 (0.1%), 2^22 0. The translated lookups hash with
**bare FNV-1a**, whose low bits ignore differences in high bits (format/type fields, addresses 512 MiB apart), so clusters
of 9+ keys on one home slot, not load (0.4-0.8%), double the table. `GTA6RT_METAL_TABLE_CLUSTER_CAP_MB=<n>` (new, off)
refuses such a key once the table is n MiB (it reads the fallback); ten-g ran it at 160 and ended (pipelines) before the
table wanted to grow: 0 refused, so not shown on screen. The fix is the `fnv1a_fmix32` kind in every translated module
(the runtime already supports it): these keys then fit 2^16 slots (2.5 MiB): -300 to -620 MiB of footprint, -150 to
-310 MiB wired. Translator + tier rebuild.

### 3. The dropped held submissions ("vertex buffer slot 0 VA 0xfeb0 not resolved") (M from logs)

- **Ordinary draws whose vertex/index buffer was made after port/metal's console-VA space ran out.** `VaSpace` is a
  64 GiB bump allocator that never reuses a range, and every GPU-VA-only hold of the title pool (shader-VA probes,
  spacers, hole fills) took console VA it never uses. Runs with the drops: `GPU VA only 117,770 MiB` (wired-b, 409
  dropped), 48,069 (pfix-d, 835); without: 3,366 (pfix-c), 785 (wired-g); 4 of 31 round-13 runs had them. Past that
  point every new buffer has console VA 0, so `gpu_va() + offset` is the offset alone (0xfeb0, 0xdd40, 0xffa0, ...: 10
  values in wired-b), the encoder cannot resolve it and fails the list, and the whole held ExecuteCommandLists is lost.
  Vertex and index data resolve through title-pool chunk buffers, so every draw of geometry whose chunk was made late.
- On screen (I from the counts): ~9 submissions a second at 2-3 frames/s, i.e. 3-4 of the ~39 submissions of each
  frame lost whole, with all their passes (geometry, shadows, post) for that frame.
- Fixed: `create_buffer(va_only)` takes no console VA (every user reads `host_gpu_address`); `MetalBuffer`/`MetalHeap`
  give their range back, reused only once the bump space is exhausted and 10 s after release; `[metal] console VA space
  exhausted` is logged; `gpu-memory detail` reports `console VA N of 65536 MiB handed out, released, reused, refused`
  (ten-a: 6,873 MiB in 210 s, 0 refused); `metal_replay.cpp` `bind_draw_state` skips just that draw (logged) when a
  vertex/index buffer has no console VA. 0 such lines in ten-a..g.

### 4. GPU use stamp for evicting texture copies: designed and measured, not implemented (over budget) (M, offline probe)

- Design: in each module's lookup (FNV-1a, 8 probes, `return index`) mark the index in a bitmap kept right after the
  slots in the table buffer itself (word `1 + (mask + 1) * 10 + index / 32`, or a byte per index): no new binding, every
  module that looks up a T# already has it. The CPU reads and clears it every second and keeps a last-seen time per
  bindless index. A title copy (not GPU-written) none of whose indices was seen for 30 s gets the 1x1 fallback written at
  its indices and its Metal texture released, keeping keys and indices, so a later use stamps the evicted index and the
  copy is made again from title memory (a few frames of fallback, nothing lost for good).
- Probe (`cspeed_ab`, captured `lib_cs_01315` dispatch: 3,680 x 64 threads, 75 texture ops; 40 dispatches a command
  buffer, best of 5, A/B alternated, 3 seeds each; outputs bit-identical in all 42 runs; through `tmem_slot.sh`; variants `build/ten-x86_64/tools/mkvar.py`, raw `probe*.out`):
  baseline 49.63-49.77 ms; **a plain byte store per lookup 50.27-50.34 (+1.2%)**; test, then atomic OR of a bit
  51.41-51.55 (+3.5%); the same on the first lane only (the lookup is uniform) 52.0-52.1 (+4.7%); an atomic OR every
  lookup 98.1-98.6 (+98%). `lib_cs_01267` (0.74 ms) is inside its noise.
- The cheapest variant is over the 1% budget on this kernel, and the change needs a translator pass over every module
  and a tier rebuild: **not implemented, no eviction**. Next: store only on a `hash_memo` miss (01315 has no memo; the
  mspeed rows memoize repeat lookups of an invocation), measured on a pixel and a mesh row too, folded into the same
  rebuild as fmix32 (2b).

### 5. The second guard and the 10-minute run

- **Guard** (port/metal `mtl_memory.mm` `start_machine_guard`, default on): 4 times a second machine wired + compressor
  occupied; over `GTA6RT_METAL_MACHINE_GUARD_MB` (default 15,360; 0 off) it logs `[metal] machine guard: ...` and exits 6.
  Also in `build/ten-x86_64/run_world.sh`. Never fired: worst 15,209 (ten-d, 9,559 + 5,650).
- **ten-final** (12:23-12:27): canonical build of 12:00 with every default (64 KiB chunks, tombstones, the guards), the
  launcher's settings, a warm private archive (a second play.sh start), A 40-70 s, then from 150 s `lsup` 4 s + `rsright`
  2 s every 15 s (44 holds delivered, the title read the pad on 4-8 frames each). Per minute of world:

| world minute | footprint (MiB) | machine wired peak | compressor peak | wired + compressor peak | frames/s (HUD median) | frame ms |
|---|---|---:|---:|---:|---:|---:|
| 0-1 | 11,510-13,271 | 9,039 | 5,441 | 14,432 | 2.0 | 549 |
| 1-2 | 13,749-14,371 | 9,612 | 5,275 | 14,289 | 2.0 | 515 |
| 2-2.7 | 14,344-14,793 | 9,565 | 4,824 | 14,307 | 2.0 | 488 |

  **Ended at world+162 s on the footprint cap** (14,860 > 14,848; exit 4) after the hash table's 160 -> 320 MiB doubling
  (above). Still view until world+40 s, the fade at world+69 s, HUD from world+105 s (s4_evidence OCR); the dumped
  presents ~27 s apart change heading (mean abs difference 81-94 of 255; the evidence script's 7 turn checks need denser
  dumps). Kept: `/Users/a/Desktop/Projects/GTA-VI/port/run/present/ten-final-handover-default-build.png` (present 2258,
  world+~160 s: the player on the deck, HUD $407/$2,480, Donnie's text, city, bridge and sea): wrong in it: dot-speckled
  surfaces, a dark block behind the head, the shirt's back shows a sawtooth of sky/water texture, the minimap black,
  the water a flat white-grey.
- ten-g (same binary, table cap 160 MiB): footprint cap at world+54 s when pipelines jumped +742 MiB at the hand-over.

### Switches and files (this round)

- Default on: 64 KiB pool chunks; hash-table tombstones (`GTA6RT_METAL_TABLE_TOMBSTONES=0`); settled generations freed
  each flush; journal trimmed; `va_only` buffers take no console VA and VAs are reused after exhaustion; the `signal`
  autorelease pool; the machine guard (`GTA6RT_METAL_MACHINE_GUARD_MB`); the replay's per-draw skip for a buffer with
  no console VA. Off: `GTA6RT_METAL_TABLE_CLUSTER_CAP_MB`, `GTA6RT_METAL_TABLE_DUMP`, `GTA6RT_METAL_MALLOC_TRACE`.
- `src/metal_bindless.h/.cpp` (`flush`, `erase_key`, `insert_key`, `report_in_place`, `stats`; new tombstone helpers,
  `free_settled_generations`, `cluster_refused`); `src/metal_replay.cpp` (`bind_draw_state`, `no_console_va`);
  `shims/d3d12_metal_device.cpp` (`title_pool_chunk_bytes`, `report_layer_sizes` + one call in `report_gpu_memory`);
  port/metal `mtl_internal.h` (`VaSpace`, `MetalBuffer::own_va`, `machine_guard_`), `mtl_resource.mm` (`VaSpace::*`,
  `~MetalBuffer`, `~MetalHeap`), `mtl_device.mm` (`create_buffer`, `~MetalDevice`, `memory_detail`), `mtl_memory.mm`
  (`create_host_buffer`, `start_machine_guard`, `malloc_zone_trace`), `mtl_queue.mm` (`MetalQueue::signal`).

### Next (ranked)

1. Footprint at the hand-over: the tier prebuilds ~660 pipelines no first use asks for (~0.3 GiB; cache worker:
   `GTA6RT_METAL_PCACHE_PREBUILD=cold` measured -0.3 GiB but 12 s first-use waits; a middle way, e.g. AHEAD_S smaller or
   only the recorded phase), and the hash table: `fnv1a_fmix32` in the translated modules (translator + tier rebuild):
   -0.3..-0.6 GiB.
2. Wired after the hand-over (+1.5-3 MiB/s at 9.5-9.7 GiB): title copies (needs the use stamp of 4, cheaper variant),
   driver scratch (translator), pipelines.
3. The title's PSO objects (109 k, 213 MiB) and the upscale stage's per-frame command buffer outside a pool (presented-
   frames / device owners).
4. A fresh `heap` census after the `signal` fix (expect ~0 live command buffers beyond those in flight).

## Round 13, presented-frames worker (2026-11-27 09:03-10:42): black presents and the 1080p band trace to RT aliasing wider than the console's (alias floor + share rule, now default); 300 consecutive world frames not reached (longest 115)

Tree `build/pfix-x86_64` (attach.cmake super-build, `-build1..15.log`, binaries `gta6rt.b8..b15`; b15 = the shipped defaults), launchers
`build/pfix-x86_64/run_world.sh` / `run_world2.sh` (expo's with this tree's run dir, the tmem probe lock (0.2 s poll in
`run_world2.sh`), cap 14,848 with no heavy job else 13,312, `GTA6RT_METAL_CB_ABORT_MS=2000`, a 4 Hz machine-wired watch
that ends the run at 9,728 MiB, `run/logs/<run>.wired.tsv`, env in `<run>.meta`). Run dir archive warmed from expo's and
gpass's shards (see 1). Runs pfix-a..e, h, h2, j..n0, q0, q1 (16 runs; f, g, i, n1 never started), each alone (slot + lock), hidden window, imported save, no audio. **0 failed
command buffers, 0 GPU page faults; gpuEvent-gta6rt 9 before and 9 after every run.** One run (pfix-a) ended on
`CB_ABORT_MS` (a 2 s command buffer at world+5 s while 953 pipelines compiled); the others on the wired watch or the
footprint cap (13,312 whenever another worker's build ran), 4 to 159 world frames each. M = printed by a log, a
readback or a tool; I = inferred. Load 3-7 at run starts.

### 0. How frames are checked (new, opt-in): `GTA6RT_METAL_PRESENT_CHECK=1[,dir]` (`src/metal_present.cpp`)

After each present, plane 0 (the title's game image, before the UI plane) is copied into a shared buffer on the
present queue and inspected when that copy's fence completes (`Fence::on_completion`): no CPU wait on the present
path. A present counts as a **world frame** when the host upscaler ran again since the previous present (the title's
own fades and menus are logged apart: `has no new world render`). Black = under 5% of texels with a channel >= 4; the
line per frame gives lit %, mean RGB, the run of non-black world frames and its longest, black/dark/missed counts;
black frames (first 8) and every 20th are written as PNGs. Also `GTA6RT_UPSCALE_PROBE=1` (`upscale/`): colour, motion
and output of the upscaler sampled on a 160x90 grid per frame (non-finite, zero, mean luma), and
`GTA6RT_METAL_SCENE_EXPOSURE_EVERY=1` (scene probe): the 1x1 exposure target read back at every upscaler latch.

### 1. Black presents: three causes, two of them ours (M)

| cause | evidence | fix |
|---|---|---|
| (a) 08:00-08:44 binaries carried `CLEARS_ACROSS_BARRIERS` default-on | expo-f/-g (binary 08:34) black/near-black dumps; expo-h/-i/-k (same tree, 08:46, without it) 32 dumps, 0 black | removed (gpass); no `CLEARS_ACROSS` in `metal/src/mtl_queue.mm` nor in any binary built after 08:44 (`strings`) |
| (b) cold archive for the 09:10 default overlays (rectvs, mop) | pfix-a (fresh run dir): 27/27 world presents black; tiled lighting "pipeline compiling" 241 of 274 rect draws a frame; play-build worker measured the same | run dir seeded from expo's + gpass's shards; **tier refresh asked of the cache worker / lead** |
| (c) **RT aliasing clobbers live targets** | below | `GTA6RT_METAL_RT_ALIAS_MIN_KB=256` + `GTA6RT_METAL_RT_ALIAS_SHARE_PCT=50` |

(c) in detail. The black frames carry CopyDithering's dither only (37.4% of texels at 1/255: the same signature as
expo-g's black dumps) or plain 0. Two paths:
- **The 1x1 RGBA32F exposure target** (`PS_ResetExposure` writes it, `PS_CompositeNMB`'s VS reads it) shares an alias
  extent with bigger targets. `fit()` places aliases at the same offset, so a target the title keeps on the *tail* of a
  big one's pages lands on that one's *first* bytes in the placement heap. In poison mode (pfix-d) the exposure texel is
  non-finite at all 96 latches and **117 of 117 world presents are black**; without poison it happens when the big
  target is written between the two passes (pfix-c: 4 of 116 black).
- **Water NaNs from aliased inputs:** the water resolve (`.VS_WaterResolveTiled_Variant_1cb4ed87` +
  `&PS_ResolveDeferred_0..3_1cb4ed87`) put 43-93k non-finite texels into the scene colour (expo-e/-f stage readbacks,
  pfix-d), in alternate frames (pfix-e probe: 0 / ~750 of 14,400 samples). They reach the title's luminance average
  (exposure non-finite in 5 of 61 frames, pfix-h) and MetalFX (its output all zero in 16 of 81 frames, pfix-h: auto
  exposure). The load hazards named: 960x540 RGBA16F <-> 512x512 R16F, 960x540 RGBA16F after 256x256 RGBA32F/R32.

| run (poison unless noted) | binary | MIN_KB | SHARE_PCT | world frames | black | exposure non-finite | MetalFX all-zero | colour non-finite samples/frame | loads after hand-over |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| pfix-d | b7 | 0 | - | 117 | **117** | 96/96 | 0 | ~700-1700 | 74 |
| pfix-e | b8 | 256 | - | 33 | 0 | 1/22 | 0 | 0 / ~750 alternating | 27 |
| pfix-h | b10 | 256 | - | 80 | 17 | 5/61 | 16 | ~700-1500 | - |
| pfix-h2 | b11 | 256 | 50 | 115 (then the title's fade) | **0** | 0/85 | 1 | 0-28 | 12 |
| pfix-q0, 1 MiB chunks (default) | b13 | 0 | - | 29 | 0 | | | | 2 (poisoned 35) |
| pfix-q1, 1 MiB chunks | b13 | 256 | 50 | 14 | 0 | | | | 0 (poisoned 1) |
| pfix-n0, 64 KiB chunks | b13 | 0 | - | 117 | 4 (3 at world entry, 1 the fade) | | | | 0 (poisoned 39) |
| pfix-k, 64 KiB, no poison | b12 | 0 | - | 10 | 0 | | | | 4 |
| pfix-j, 64 KiB, no poison | b12 | 256 | 50 | 29 | 0 | | | | 0 |
| pfix-l0, 64 KiB, no poison | b13 | 256 | 50 | 106 | 1 (the fade) | | | | |
| pfix-m, 64 KiB, no poison, glare skipped | b13 | 256 | 50 | 108 | 3 (2 world entry, 1 the fade) | | | | |

pfix-a..h2 used 256 KiB pool chunks (`GTA6RT_METAL_TITLE_POOL_CHUNK_KB=256`, opt-in, to live longer under the wired
watch), j..l1 64 KiB (+ `GTA6RT_METAL_PCACHE_PREBUILD=cold`, the wired-memory worker's), q0/q1 the default 1 MiB.
**Which targets alias depends on the pool's chunking** (VM-object runs): the exposure clobber is deterministic with
256 KiB chunks (pfix-d) and absent in q0/n0's frames; the hazards the switches remove are present in every chunking
(q0 35 poisoned hand-overs vs q1 1; k 4 loads after hand-over vs j 0). Same-binary pairs: q0/q1 (b13), k/j (b12);
cross-binary d (b7) vs h2 (b11) is the decisive one. **Now default** (`metal_rt_alias.h`, 10:38): `MIN_KB` 256,
`SHARE_PCT` 50 (`=0` each restores the old rule).

**Longest runs of non-black world frames:** 115 (pfix-h2, the whole tips still view, then the title's own fade to the
hand-over), 110 (pfix-n0), 103 (pfix-l0), 101 (pfix-m). **The 300 asked are not proven**: no run lived past world+80 s
under the wired watch / footprint cap (13,312 whenever another worker built), and the title presents 144-184 world
frames before the fade at ~2 frames/s. The black frames left in the alias-fixed runs are the first 1-3 at world entry
and the fade's first.

Render-target memory with both switches: 1,038-1,045 MiB (pfix-h2/-j) vs 1,011 MiB without (pfix-k): +~30 MiB.

### 2. The band at 1080p rows ~376-426: the lens-flare streak fed by clobbered / non-finite data (M)

- Not tile-row or pitch garbage: its rows follow the sun's height in the still view (expo-k/-i, gp-f, pfix-h2 all
  376-426 at that camera; pfix-c's other camera has none). Per-pass readbacks (pfix-c): the upscaler's input/output,
  HairCopy and CompositeNMB at frame 60 have no band.
- `GTA6RT_METAL_SKIP_DRAWS=MaskedGlareBlur,CalculateGhosts,FlareMask` (new opt-in, `src/metal_replay.cpp`): pfix-m's
  dumped presents (2140-2227) have no band (`run/present/pfix-m-check/`). So the band is the flare/glare chain:
  `VS_FlareMaskSingleStar_f6657ec3`/`PS_FlareMask` (an indirect draw the replay records with **no render target**,
  "[]") then `PS_MaskedGlareBlurPass_0_784d6491` .. `PS_CalculateGhosts_83c1062a` into an R11G11B10 480x270. Content:
  magenta/blue noise with aliasing as before (water NaNs into BrightPass), white/red in poison mode. With the alias
  switches on and nothing skipped (pfix-l0, the same still view and sun) the dumped presents have no band. Whether
  the mask draw's missing target is a UAV-only draw or a lost binding was not checked.

### 3. Black albedo: T#s inside registered slot-133 ranges (M from the dump; A/B in the world below)

- Offline (mtex-census T# dump, 512 bound + 512 naming none): the T#s that point inside a registered texture are other
  textures packed behind its resident levels (other BC formats 175/177/182 vs the registered BC1, 512x512/1024x512 vs
  1024x1024, 8 KiB apart from +80 KiB), not mips of it: a sub-range -> mip-offset mapping does not apply.
- `GTA6RT_METAL_TSHARP_INSIDE=1` (new, opt-in; `make_orphan_textures` overlap loop): a registered title-filled texture
  that merely contains the T#'s address no longer blocks the T#-made texture (the title's own T# there says the
  registered one's used levels end before it). Counter "inside a registered texture's range" in the census line.
- Census (pfix-c, off): T#-made 10,977; not made: "overlaps a registered texture" 23,652, **"a texture is registered
  there now" 38,276** (a T# at a registered texture's own address with another shape: memory the title reused),
  "title pool does not back all of it" 897. Misses: inside a registered texture 2,100 of 4,096 sampled.
- World A/B (same binary b13, 64 KiB chunks, alias switches on): pfix-l0 (off) vs pfix-l1 (`=1`): T#-made textures
  13,302 vs 24,397; albedo at upscaled frame 6 (G-buffer read at the latch): non-zero 23.7% vs 5.8%, geometry with black
  albedo 51.7% vs 67.0% of the frame (68.6% vs 92.0% of geometry); **8 black world frames mid-run in l1** (checks 41-64,
  dither only) vs 0 in l0. So not default: the extra textures are made later than the frame-6 snapshot (1.5 ms pass
  budget) and something they bring makes the composite black (I: BC6H/float formats decoded from bytes that are not that
  texture give non-finite texels). Later G-buffer readbacks at the latch read zero (aliased targets reused by then), so
  frame 6 is the only comparable one.
- Albedo baseline (pfix-c frame 90, G-buffer at the latch): albedo non-zero 33.3%, geometry 77.3%, **geometry with black
  albedo 44.0% of the frame (57.0% of geometry)**.

### 4. Glyphs (I, not run)

- The font caches are slot-133 A8 textures the title maps (2048x2048 and 1024x1024, the title's XG layout linear) and
  are **in place** (`make_in_place`: a buffer-backed Metal texture over the title's pages), so CPU-written glyphs are
  sampled with no copy or refresh. The evidence of missing glyphs is round 7 (`up7-b` credits "Ch r ct r Art Dir ctio",
  `up7-f` COULDN'T AUTOSAVE body), before in-place textures; the loading tips and subtitles of every run this round
  render every glyph. Not reproduced or fixed this round (needs a prologue run for the credits).

### Milestone image

`/Users/a/Desktop/Projects/GTA-VI/port/run/present/pfix-l0-presented-world-no-band.png` (pfix-l0 present 1985, plane 0
= the game image under the UI plane, alias switches on, nothing skipped): exposed sky and clouds, the white left house
textured, palms and flowers, the stilt house, **no band**. Wrong: the stilt house's siding and decks still black albedo,
the water bottom left saturated blue/white with a sawtooth edge (water resolve, values huge), a dark rectangle behind
where the tip box sits (the title's backdrop), grainy foliage.

### Switches added (all off by default unless said)

`GTA6RT_METAL_PRESENT_CHECK`, `GTA6RT_UPSCALE_PROBE`, `GTA6RT_UPSCALE_SANITIZE` (non-finite colour/motion -> 0 before
MetalFX; not needed once (c) is fixed, not measured), `GTA6RT_METAL_SCENE_EXPOSURE_EVERY`,
`GTA6RT_METAL_SCENE_GBUFFER_FORMAT`, `GTA6RT_METAL_SKIP_DRAWS`, `GTA6RT_METAL_TSHARP_INSIDE`,
`GTA6RT_METAL_RT_ALIAS_MIN_KB`, `GTA6RT_METAL_RT_ALIAS_SHARE_PCT`; the scene probe decodes DXGI 67 (RGB9E5); rt-alias logs
the first 16 loads after a hand-over.

### Not done / next (ranked)

1. 300 consecutive world frames: not reached in one run (longest 115, pfix-h2, then the title's fade; every run ended
   on the wired watch or the footprint cap at world+10..80 s, 13,312 whenever a build ran).
2. The flare mask draw recorded with no target (band).
3. Tier refresh for the rectvs/mop keys (cold archive = black world for 40+ s).
4. "A texture is registered there now" (38k): T#s at a registered texture's address with another shape.

## Round 13, wired-memory worker (2026-11-27 09:04-10:15): 64 KiB pool chunks (opt-in) take 0.65 GiB off machine wired and a pool-on run reaches the hand-over to the player; no world run lasts 10 minutes yet

Tree `build/wired-x86_64` (attach.cmake super-build, `-build1..11.log`, binaries `gta6rt.b2..b11`), launcher
`build/wired-x86_64/run_world.sh` (the pcpu/gpass launcher with this tree's run dir, tmem probe lock, cap 14,848 with no
heavy job else 13,312, `GTA6RT_METAL_CB_ABORT_MS=2000`, fault_stop.sh, 4 Hz machine-wired stop at 9,728 MiB, plus a
~1 Hz machine log `run/logs/<run>.mem.tsv`: wired, free, speculative, compressor occupied/stored,
`kern.memorystatus_vm_pressure_level`, swap, AGX "In use"/"Alloc system memory", process footprint; `footprint -w` at
world+10/+40 s, `footprint -v -w` at world+12 s). Tools `build/wired-x86_64/tools/{summary,fpv,pngstat}.py`, offline
probe `build/wired-x86_64/probe/residency3.mm`. Runs wired-a..j, each alone (slot + lock), hidden window, imported save.
**0 failed command buffers, 0 GPU page faults; gpuEvent-gta6rt 9 before and 9 after every run.** M = printed by a log,
tool or probe; I = inferred. Machine idle wired 2.4-2.6 GiB at every start, load 1.7-8 at the starts.

### Result (M)

| run | binary, settings (all: census on) | story load | machine wired, world+10 / +30 / +60 s (MiB) | pool chunks wired | end |
|---|---|---:|---|---:|---|
| wired-a | b3, defaults (archive half cold) | 37.5 s | 9,091 / - | 1,937 MiB | wired stop, world+35 s |
| wired-b | b4, `PCACHE_PREBUILD=cold` | 30.4 s | ~9,300 / 9,409 (+20) / - | 1,889 MiB | wired stop, world+45 s |
| wired-d | b6, defaults (warm) | 30.4 s | - | - | wired stop, world+8 s |
| wired-f | b8, cold | 28.3 s | 9,614 / - | - | wired stop, world+12 s |
| wired-c | b6, 64 KiB, no union (broken: black world) | 29.4 s | 8,782 / 8,868 / 9,370 (warm) | 1,008 MiB | footprint cap 14,848, world+67 s |
| wired-e | b8, 64 KiB + union, cold | 42.0 s | 9,015 / 9,025 / - | 1,176 MiB | footprint cap 13,312, world+37 s |
| **wired-g** | b11, 64 KiB + union/tail/fill, cold | 36.4 s | **8,709 / 8,808 / 9,013** | 1,077-1,316 MiB | wired stop, **world+95 s** |
| **wired-i** | b11, as g (the 10-minute attempt) | 31.9 s | **8,779 / 8,848 / 9,125** | 1,056-1,405 MiB | wired stop, **world+82 s** |
| wired-j | b11, 1 MiB, cold (g's same-binary control) | 30.3 s | 9,412 (+11) / 9,492 / - | 1,907 MiB | wired stop, world+46 s |
| wired-h | b8, 1 MiB, cold, `TITLE_POOL_SCAN_STRICT=1` (f's partner) | 33.5 s | 9,219 (+9) / - / 9,702 (+59) | - | wired stop, world+60 s |

- 64 KiB chunks hold the pool's wiring to ~1.1-1.4 GiB instead of 1.9 GiB and machine wired at 8.7-9.1 GiB through
  world+60 s, against 9.3-9.6 GiB at world+10-20 s with 1 MiB in the same prebuild mode. **wired-g reached the title's
  hand-over to the player** (HUD $407 / $2,480, health/armour, the player on the stilt house's deck, city and sea
  behind): `/Users/a/Desktop/Projects/GTA-VI/port/run/present/wired-g-first-pool-gameplay-frame.png` (present 2491,
  ~world+90 s). No pool-on run had got there (play worker: every pool-on run ended on the stop at world+28-57 s). Wrong
  in it: surfaces speckled with dot patterns, the interior and the character dark; and 2 of 7 world presents of g, 2 of
  4 of i were black (see 1b).
- Then the hand-over itself ends the run (g and i, world+71..+81 s): +0.45 GiB wired, +0.5-0.86 GiB footprint within
  10 s (title copies 926 -> 1,140 MiB, pipelines +140 MiB, a hash-table generation +80 MiB, pool +3 MiB, zero-size
  views 129 k -> 365 k). The world+60 s targets are not met: wired 9.0-9.1 GiB (target 8.5), footprint 13.3-13.5 GiB
  (target 12). 10 minutes: not reached.

### 1. Pool chunks (item 1): what wires them (M unless marked)

- **Residency is not the switch** (offline probe `probe/residency3.mm`, M4 Pro, macOS 27, through `tmem_slot.sh`): a 1 GiB
  bytesNoCopy buffer with 1 MiB touched wires +1,025 MiB once it is in a residency set (or at first GPU use); taking it
  out of the set and committing changes nothing; releasing the buffer unwires it ~1 s later (3,723 -> 2,787 MiB),
  unmapping the host range the rest. So dropping residency for idle chunks saves nothing; only releasing the buffer does,
  and that frees its GPU VA for the next buffer Metal makes.
- **Who names the chunks** (census, `GTA6RT_METAL_TITLE_POOL_CENSUS=1`, every 10 s: the console heaps' live buffer V#s,
  read from their GPU copies, and the title-pool aliases mapped onto the chunk buffers; 4-27 ms): at world entry (1 MiB)
  1,873-1,937 MiB of chunks, **1,690-1,742 MiB named by a live heap V#**, 26-45 MiB by an alias only, 97-157 MiB by
  neither; 0 chunk blocks hold pool pages the title does not back (xmem live map, new diagnostic). Releasing chunks no
  live descriptor names would free <= ~0.15 GiB and is not safe: descriptors the layer wrote are copied by the title into
  its own memory (the meshlet header tables, round 10), which nothing scans. Not done.
- **What the 1 MiB granularity costs:** the bytes the live heap V#s name, at 16 / 64 / 256 / 1,024 KiB granularity:
  **816 / 906 / 1,204 / 1,714 MiB** (wired-b, world+40 s; 795 / 880 / 1,161 / 1,649 at world entry). ~0.8 GiB of the
  1.9 GiB is the rest of each MiB around what descriptors name.

### 1b. Fine pool chunks (opt-in `GTA6RT_METAL_TITLE_POOL_CHUNK_KB=64`; the 1 MiB default path is unchanged)

- What chunks under 1 MiB needed (device shim; every change is gated on chunks < 1 MiB):
  - `title_pool_chunk`: no VA-only probe (its release waits for the queues, so the chunk after it got another address
    985 of 2,048 times even at 1 MiB); the buffer is made outside the residency set and joins it only when its shader VA
    is accepted, a refused one costs nothing (probe above). O(1) chunk count (the old count walked all chunk slots for
    every new chunk: 262,144 at 64 KiB). After a collision no spacer: `title_pool_fill_hole` fills the hole the chunk hit,
    up to the end of the title range it hit, with GPU-VA-only holds. wired-e's spacers had reached 393 GiB of VA and
    ~0.4 GiB more "Owned physical footprint (unmapped) (graphics)" (fp10: 2,382 vs 1,955-1,969 MiB); g/i: 689-785 MiB.
  - `title_pool_union`: the alias table holds one shader VA per title byte, so at 64 KiB a range running into the next
    chunk's alias was cut short (wired-c: 15,628 of 32,012 resolutions) and V#s met aliases shorter than their span
    (zero records): ~100 k more zero-size views and **a black world behind the loading tip** (wired-c, every world
    present). Now a range is mapped together with every title-pool alias it meets, as one alias over one span buffer,
    widened to the pool's 256 KiB blocks (`GTA6RT_METAL_TITLE_POOL_UNION_KB`; chunk making 16.6 s -> 1.8 s, e -> g), and
    the buffer reaches 64 KiB past the alias's end (root views and bound-table V#s take an existing alias for its start
    alone and read up to 64 KiB).
  - `resolve_title_memory`, `vsharp_address`, `rebase_title_vsharp`: a title-pool alias shorter than the bytes asked is
    resolved again (one condition each, announced 09:52/09:58).
  - `title_pool_va_floor`: GPU-VA-only holds below shader VA 4 GiB before the first chunk (0 were needed in these runs).
- Zero-size views with the union: 127-129 k at world+30..37 s (e, g, i) against 162 k at world+45 s (b, 1 MiB).
- Costs: story load 31.9-36.4 s (g, i) against 28.3-30.4 s with 1 MiB in the same mode (e, no widening: 42.0 s);
  6,700-9,100 chunk buffers instead of ~2,000; AGX "In use system memory" no longer follows machine wired (overlapping
  buffers counted per buffer: g 9.0 GiB in use at 8.7 GiB machine wired).
- **Same-binary on/off (b11, cold): wired-g / wired-i (64 KiB) vs wired-j (1 MiB):** machine wired 8,709-8,779 vs 9,412 MiB
  at world+10 s, 8,808-8,848 vs 9,492 at world+30 s (**-0.65 GiB**); pool chunks 1,056-1,316 vs 1,907 MiB; story load
  31.9-36.4 vs 30.3 s; in the world to the wired stop 82-95 vs 46 s.
- **Black presents (all (0,0,0)):** 64 KiB: g 2 of 7 world presents (2431, 2461), i 2 of 4 (2101, 2161), all from
  ~world+50 s on; 1 MiB: j 0 of 2, f 0 of 1, h 0 of 3, none of which lived past world+60 s. So either fine chunks or the
  later world time brings them; these runs cannot tell which (the presented-frames worker: 4 of 116 black at 1 MiB in
  pfix-c, RT aliasing clobbering the 1x1 exposure target; pfix-h 17 of 80, black from world+27 s on). **Not made
  default** for that reason.

### 2. Pipelines (item 2): strict first-use adoption saves ~0.3 GiB but the world's first uses then wait 12 s on average (M)

- Built (`d3d12_metal/cache/src/pcache.cpp`, opt-in): `GTA6RT_METAL_PCACHE_PREBUILD=cold`: a pipeline a manifest (tier or
  this directory's) lists under its current key is made at its first use; only re-keyed / unlisted ones are prebuilt.
  `GTA6RT_METAL_PCACHE_MIN_RUNS=N`: the same for entries fewer than N recorded runs used. Report field "at first use
  only". Tests (standalone tree `build/wired-pcache-x86_64`): `pcache_runtime_test <dir> cold` (new ctest
  `pcache_runtime_cold`) PASS, default case PASS.
- Memory: Metal's "outside every backend category" is 0.43-0.47 MiB a pipeline. Cold mode: 878 MiB at 1,854 pipelines,
  world+47 s (wired-b); warm default: 1,174 MiB at 2,624, world+48 s (wired-c) and 1,112 MiB at 2,555 (gp-i): **-0.3 GiB**.
  The category keeps growing in both (wired-c 1,174 -> 1,629 MiB from world+48 to +69 s for 118 more pipelines, I: the
  late mesh pipelines are large).
- **First use in the title is not the probe's 0.2 ms:** wired-b's 1,775 archive-served first uses waited **12.3 s on
  average, 29.6 s at most** (pcache-runs record, `wait ... archive`): 1,637 first uses in the world's first 40 s queue on
  the 3-thread compile queue (~70 ms each, I); draws are skipped meanwhile. Not default.
- Run counts predict use poorly (tier `runs` against the warm records gp-i and wired-b): entries 1-3 runs used: 288, of
  them 4-5 used here; >= 10 runs: 2,532, 62% used. `MIN_RUNS=4` would skip ~288 pipelines (~0.12 GiB); not run in the title.
- Side finding: with the 09:10 default overlays (rectvs + mop) the tier is stale; wired-a compiled 1,062 pipelines cold in
  the world (first-use waits up to 32 s, story load 37.5 s). Each run directory warms after a run or two.

### 3. Title texture copies (item 3): not evictable from what the layer sees (M, census)

- The census maps the bindless image entries onto the heaps' slots (GPU copy and title bytes): at world entry 19-21 k
  textures (3.7-3.8 GiB of views) have a T# in a heap slot, 2.9-3.0 k (396-430 MiB) have none; at world+40 s (g) 5,479
  (730 MiB) have none. Translated shaders find a texture by its T#'s words wherever the title keeps them (its per-draw
  tables too), and the layer only learns of a T# when a heap slot changes, so "not bound for M seconds" is not
  observable: an evicted texture whose T# lives on in title memory would sample the fallback until its slot is written
  again. Not built. The title copies are the largest term of the hand-over step (+214 MiB), so a GPU-side use stamp in
  the bindless lookup (translator + bindless owners) is what would make this lever usable.

### 4. Compression and footprint (item 4) (M, `footprint -w` at world+10/+40 s, `footprint -v` at world+12 s)

- Compressed pages at world+10 / +40 s: 2.3-4.2 / 4.3-4.4 GiB. By tag (wired-c, world+40): **untagged (the title's memory:
  pool and other committed memory) 2,867 MiB**, Malloc Large 818, Small 370, Tiny 124, Rosetta 54, graphics 80. The pool
  object itself (wired-a, world+12, host mapping counted once): 1,605 MiB dirty, 817 MiB compressed; governor: 5.9 GiB
  of the pool touched, 1.5-1.8 GiB of it compressed by world+20 s.
- So the compressor takes the title's CPU-side memory and malloc, not copies the pool made redundant: title shadows 0 MiB
  (ledger), title + scatter windows <= 2 x 64 MiB, console-heap host copies 2 x 38 MiB (dirty), one 80 MiB malloc block (a
  hash-table generation's size). Nothing freed.
- **Footprint is the stop after wired** (c: 14,848 cap at world+67 s, e: 13,312 at +37 s): 12.7-13.3 GiB at world+30 s,
  +12-40 MiB/s: graphics ledger +12 MiB/s (title copies, pipelines, hash-table steps), malloc in use +5 MiB/s (not
  attributed; a pool-off run, play-f, grows the same: 11.1 -> 14.3 GiB from world+0 to +138 s). Target not met.
- Descriptor hash table: 3-4 live generations of 80 MiB (240-320 MiB) at world+20..+50 s (b, g, gp-i): bindless owner.

### 5. The stop itself (item 5) (M, 1 Hz machine log in every run)

- In the world: machine wired 8.2-9.7 GiB; **free 95-390 MiB in every sample** (the kernel's free target from world entry
  on); compressor occupied 2.4-4.7 GiB (holding 4.4-7.0 GiB); swap 262 MiB, unchanged; **`kern.memorystatus_vm_pressure_level`
  1 (normal) in every sample, also at every stop.** With 1 MiB chunks AGX "In use system memory" follows wired at a
  1.1-1.5 GiB offset.
- The panic snapshot (10.7 GiB wired, 5.3 GiB compressed, 96 MiB free) has the free pages we always run at; it differs
  from our worst in wired (+1.0 GiB over the stop) and compressor (+0.6 GiB). A free-page or pressure-level guard would
  not have fired before it, so the data does not support replacing the wired stop. Proposal (a tightening): keep 9,728
  and also stop at wired + compressor occupied > 15 GiB (worst seen 9.37 + 4.73 = 14.1, wired-c world+60 s; panic 16.0).
- The 4 Hz stop reads single samples: wired-a's last second went 9,118 -> 9,456 -> 9,651 -> 9,734 MiB (2 s rolling
  median max 9,337); not loosened.

### 6. Strict heap-descriptor rule (item 6)

- Memory: chunks made by loose heap V#s are 42 MiB (pool-wake line, wired-a; canonical heap V#s 4, other paths 1,891),
  so strict mode moves wired by <= 42 MiB.
- Same-binary pair (b8, 1 MiB, cold): wired-f (off) vs wired-h (on): story load **28.3 s off, 33.5 s on**; strict refused 167
  chunk makings (loose-V# wiring 40 -> 0 MiB); the pool-wake filing grew 184 k -> 835 k entries, empty slots 190.9 k ->
  195.7 k; world presents of h 3 of 3 normal (f ended at world+12 s with one, normal); no scene dumps, so G-buffer
  coverage was not compared.
- Story load spread this round with the archive state alone: 28.3-37.5 s at 1 MiB; pcpu-c's 18.3 s sits outside it, the
  other pcpu runs inside. **Not made default**: no story-load gain in the pair, <= 40 MiB of wiring, and a few
  thousand more empty V# slots.

### Switches and files

- Off by default, the 1 MiB default path unchanged: `GTA6RT_METAL_TITLE_POOL_CENSUS=1`; `GTA6RT_METAL_TITLE_POOL_CHUNK_KB=64`
  with `GTA6RT_METAL_TITLE_POOL_UNION_KB` (256), `GTA6RT_METAL_TITLE_POOL_VA_FLOOR_GB` (4 with fine chunks),
  `GTA6RT_METAL_TITLE_POOL_PROBE=1`; `GTA6RT_METAL_PCACHE_PREBUILD=cold`, `GTA6RT_METAL_PCACHE_MIN_RUNS=N`.
- `shims/d3d12_metal_device.cpp`: `title_pool_census`, `title_pool_texture_census`, `title_pool_va_floor`,
  `title_pool_fill_hole`, `title_pool_fine`, `title_pool_short_alias`, `title_pool_union_grain`, `title_pool_union` (new);
  `title_pool_chunk`, `title_pool_window`, `title_pool_sync` (one call), `claim_shader_range` (records the refusing range's
  end), `resolve_title_memory`, `vsharp_address`, `rebase_title_vsharp` (one condition each); xmem declaration.
  `shims_core/src/xmem.cpp/.h`: `title_pool_live_map`. `d3d12_metal/cache/src/pcache.cpp`, `cache/tests/runtime_test.cpp`,
  `cache/CMakeLists.txt` (ctest). No port/metal change (the residency question was settled by the probe).

### Next (ranked)

1. Device-shim owner / lead: `GTA6RT_METAL_TITLE_POOL_CHUNK_KB=64` as the default once the black-present count is
   compared over the same world time (a 1 MiB run that lives to world+90 s needs the wired headroom first). Measured: -0.7..-0.8 GiB pool wiring, -0.6..-0.9 GiB machine wired at
   world+10..+30 s, runs to the hand-over; +2..+6 s story load against the same-binary control.
2. The hand-over step (+0.45 GiB wired at world+71..+81 s): title copies (+214 MiB) lead; 730 MiB of textures had no heap
   T# at world+40 s. A GPU use stamp in the bindless lookup (translator + bindless owners) would make evicting them safe.
3. Driver scratch ("Owned physical footprint (unmapped) (graphics)", wired 1.8-1.9 GiB at world+10 s): translator (render
   pool `%PS_DeferredTextured_4dbafe93~24` 520 MiB, compute `lib_cs_00977` 350 MiB).
4. Footprint, the stop after wired: +12-40 MiB/s in the world; malloc growth not attributed (a `heap`/malloc-stack run).
5. Bindless hash table: 3-4 live 80 MiB generations.
6. Tier refresh for the rectvs/mop rows.
7. Seen in passing: `a held ExecuteCommandLists failed when released (10)` after `[metal] draw: vertex buffer slot 0 VA
   0xfeb0 not resolved` (also index buffers): 409 in wired-b's 45 s of world, whole held submissions dropped ~9 a second
   (replay / presented-frames owners).

## Round 12, exposure / lighting worker (2026-11-27 07:23-09:05): tiled lighting never ran on Metal; with it, the presented world is plausibly exposed

Tree `build/expo-x86_64` (attach.cmake super-build, `-build1..9.log`), launcher `build/expo-x86_64/run_world.sh` (the
lit-frame launcher with this tree's run dir, the tmem probe lock, cap 14,848 with no heavy job at the start, else
13,312, `GTA6RT_METAL_CB_ABORT_MS=2000`, a 4 Hz machine-wired watch that ends the run at 9,728 MiB). Runs expo-a..k,
each alone (slot + lock), hidden window, imported save, load 3.3-7 at the starts. **0 failed command buffers, 0 GPU
page faults; gpuEvent-gta6rt 9 before and 9 after every run.** Ends: expo-a watchdog (world only at +140 s), expo-b
footprint cap 13,312 (a build was running), all others the wired watch (peaks 9,742-10,063 MiB, 11 s to 2.5 min into
the world). M = printed by a log, a readback or a pixel dump; I = inferred.

### 1. The cause: every indirect rect-list draw was dropped at encode (M)

- `GTA6RT_METAL_SCENE_SENTINEL=1` (expo-d) clears the scene colour (f10) to (4096, 0, 4096) before its first pass in a
  dump frame: after TiledLighting (180 draws, replay census "recorded 180") and SSSResolve (12) the target held the
  sentinel on every texel. Earlier stage readbacks (expo-b/-c) showed the target byte-identical before and after both.
- Why: port/metal `build_rect` wraps the vertex function of a rect-type pipeline in MSL, so it needs the library's
  source; libraries from the store are metallibs (`source()` empty), so every rect pipeline fell back to
  `cpu_rewrite` without a word, and `Encoder::indirect_rect` skips every indirect rect-list draw of such a pipeline
  ("indirect rect-list draw skipped: the pipeline has no vertex-stage rect expansion", logged once a process in expo-c,
  -d, lit-a, lit-c, pdef-c, fcpu-h). Dropped: TiledLighting (all 180 material classes), SSSResolve, the indirect-light
  composite `PS_CompositeIndirectDrawBlend` (TileGridDefaultPass), TiledWeathering (64), TiledHairBlur.
- So the "lit" scene colour of rounds 10-11 was not lighting: f10 held whatever its memory had (since 07:09 a placement
  heap shared with other targets: 18k-193k NaN/Inf texels in expo-b/c, mtex-a, pcpu-a/c; earlier runs 0). The black
  tile block, the white wash and the NaNs in those dumps are that junk.
- Fix (opt-in overlay + one loader change): `src/metal_shaders.cpp` `MetalShaders::load_locked`: a metallib with a
  sibling `.msl` is built from that text (deferred compile, MSL 3.1, fast math as the recipe's `metal`), so its library
  has a source. Overlay `build/expo-x86_64/rectvs/manifest.tsv` (`tools/rect_vs.py`): 142 vertex shaders of the
  corpus (TiledLighting, TileGrid, TiledWeathering, TiledHairBlur, TiledUpscale, screenTransform*, all 36 `VS_Blit_*`,
  the passthroughs seen drawing rect lists), the store's metallib copied with the recipe's MSL beside it (regenerated,
  the recorded MSL size checked: 142 of 142; the runs above used the first 107, before the `VS_Blit_*` rows). Load it first: `GTA6RT_METAL_SHADER_OVERLAY=<port>/build/expo-x86_64/rectvs/manifest.tsv:`
  + the default four. port/metal `build_rect` now logs the no-source fallback (16 lines, with the label): expo-f still
  named `VS_Blit_6e5520d1` and `VS_Blit_f8e5001d` (added since, not run).
- With it (expo-e/f/g/k): TiledLighting writes the target (expo-f frame 20: before 77,412 non-finite texels of junk,
  after TL 0; 34.7% non-zero, p99 22 pre-exposed), no "indirect rect-list draw skipped", direct rect lists recorded
  (no "vertex buffer unknown"), 0 GPU errors.

### 2. Exposure chain (M unless marked)

- `PS_ResetExposure` (every frame; `PS_CalculateExposure` = the same plus temporal adaptation): ev = log2(0.18 /
  max(L, 1e-6)) + a 1D curve term (cb 0x3f0: range -15.0..7.15, clamp -5.05..2.49), clamped to cb 0x420 = **[-12.9,
  5.98]**; output (2^ev, ev, ev, a). L = texel .g of a 1x1 RGBA32F texture the compute chain writes (lib_cs_00879..00905;
  .r ~min, .g = geometric mean, .b ~arithmetic mean). pdef-c's exposure 2^-12.9 is that clamp: L of the junk was huge.
- Pre-exposure: a 16-byte buffer (X, 1/X, 8, 1/8). Tiled lighting multiplies its colour by 1/X (s11), the composite
  (`PS_CompositeNMB`) by X (s24) and by the exposure the VS reads from the 1x1 target (2^ev). Pixel dumps: X 16-178,
  TL s11 = 0.0312 = 1/32.0 (expo-e). Consistent; nothing of ours in it.
- With tiled lighting (expo-k, mtex defaults off): ev -10.7..-10.9 (L ~330); composite output means 0.29-0.38 in both
  renders of each present; presents (alpha now 255) settle in 8 frames from white at world entry to RGB means 75-112.
- MetalFX: the title passes pre-exposure 1.0, no exposure texture, create flags 0x29 (HDR, inverted depth, auto
  exposure); output RGB9E5 1920x1080 (Metal 93). Auto exposure only steers MetalFX's history weighting (I); input and
  output stay in the colour's units.
- Bloom is energy-conserving (blurred copy of the luminance source, ~1% weight: cb 0x40 = 0.01); BrightPass feeds
  flares only. Not the white wash.
- **Present dumps were washed out by their alpha** (`src/metal_present.cpp`): the swapchain keeps the game image's
  alpha (mean 155-170 in lit-c / expo-c presents), the CAMetalLayer is opaque so the screen ignores it, but viewers blend
  the PNG over white. `lit-c-first-world-present.png`: RGB means 138/175/212, alpha 170. Dumps now write alpha 255.

### 3. G-buffer items (M)

- "Noisy red" normal target (gbuffer1, R8G8B8A8): G, B, A bytes hold a 12+12-bit octahedral normal (TL decodes it:
  24 bits, x = (v & 0xfff) * 2/4095 - 1, y = bits 12-23, fold); R is a separate value. Decoded offline from a raw dump
  (`tools/rawstat.py octa`): smooth per-surface normals. Correct; the probe's PNG drops alpha, hence the noise.
- gbuffer5 zero (alpha 1): the mesh-path G-buffer shader (`PS_DeferredTextured_c5fd36bb`) exports mrt0-mrt4 only; 191
  `%PS_DeferredTextured` variants do export mrt5. Not shown to be a fault. gbuffer4 is the constant 0.502 (a clear).
- The black block top left: with TL running it is the front of the left stilt house, whose albedo is black (missing
  texture), lit to 0 (expo-k frame 12 log-luminance map). A texture-streaming item, not a tile-grid one.

### 4. Still wrong (M)

- Presents in expo-g (mtex defaults on, no probe): mostly black (RGB mean 1.1), some dark (16-20); expo-f: good frames
  (RGB ~82-108) alternating with black ones; expo-k (mtex defaults off): stable. Each present has two renders and two
  plane-0 targets; in expo-f the second render's upscaled image was black (luminance downsamples 0), in expo-k the
  second render's f10 had 66k non-finite texels after the indirect-light composite while the composite output was
  clean. Not resolved.
- A band of garbage (magenta/blue noise) at 1080p rows ~380-410 appears after the scene colour (not in f10): post chain
  (motion blur / hair blur candidates), not traced.
- GPU frame ~570-636 ms (89-98% busy) in every world run of this tree, with or without the overlay (expo-c without:
  600; expo-f with: 614-636; expo-g: 570-592), so ~1.5 frames/s; the tiled-lighting cost is not separated (I: tens of
  ms). GPU-pass worker's numbers are the reference.
- Albedo is still missing on many walls, so the near world is dark under a plausible sky.

### Probes added (all off by default)

`GTA6RT_METAL_SCENE_STAGES=N`, `GTA6RT_METAL_SCENE_AFTER=part,...`, `GTA6RT_METAL_SCENE_RAW=1`,
`GTA6RT_METAL_SCENE_SENTINEL=1` (SceneProbe: readbacks at pass boundaries, ordered by UAV barriers; after named passes,
also after the upscaler latch; raw texels; luminance percentiles, top-left block stats; small targets printed per
texel); `GTA6RT_METAL_DRAW_TRACE` now also traces indirect draws, `GTA6RT_METAL_DRAW_TRACE_TABLE=[sN:]OFF[@START-END]`
(user-data register, T# decode, longer reads, title source), `GTA6RT_METAL_DRAW_TRACE_MAX`; GpuDebug prints pixel
register dumps from `build/expo-x86_64/tools/pix_dump.py` variants (`GTA6RT_METAL_SHADER_DEBUG_OVERLAY`). Offline tools
in `build/expo-x86_64/tools`: `rawstat.py` (stats, diff, octahedral decode, tiles), `logmap.py` (false-colour log
luminance), `nonfinite.py`, `regen_msl.py`.

### Milestone image

`/Users/a/Desktop/Projects/GTA-VI/port/run/present/expo-f-first-lit-presented.png` (expo-f present 1943, opaque
copy): tiled lighting on, sky with clouds exposed, backlit stilt houses, sunlit foliage and roof, the HUD tip. Wrong:
near walls and decks dark (black albedo), grainy foliage edges, and in that run every other sampled present was black.

### Next (ranked)

1. Make the rect-VS overlay default (`build/expo-x86_64/rectvs/manifest.tsv` first in the default overlay string, device
   shim) or ship MSL for every store vertex shader the same way; run once with the 142-row overlay.
2. The black presents (second render per present: black upscaled image / non-finite f10).
3. Time the TiledLighting pass (180 indirect draws, wrapper VS runs the translated VS for three corners).

## Round 12, GPU-pass worker (2026-11-27 07:20-09:05): the world's GPU frame is ~556 ms, 55% of it the G-buffer mesh shaders' geometry stage; our pass structure is <1%; private mesh outputs on 6 hot rows: 553-602 -> 487-495 ms a frame

Tree `build/gpass-x86_64` (`-build1..9.log`; frozen binaries `gta6rt.b8`, `gta6rt.b9`), launcher
`build/gpass-x86_64/run_fr.sh` + `run_world.sh` (the frame-cpu launcher with this tree's run dir, the tmem probe lock, cap
14,848 when no heavy job runs, `GTA6RT_METAL_CB_ABORT_MS=2000`, fault_stop.sh, machine wired polled 4x a second, stop at
9,728 MiB). Runs gp-a..i, each alone (slot + lock), hidden window, imported save, defaults of their build time (pool,
tier, rt-alias, tmem/mcc/overrides/uint overlays on). Load 3.9-7.7 at the starts; other workers' builds ran at times.
**0 failed command buffers, 0 GPU page faults; gpuEvent-gta6rt 9 before and 9 after every run (9 runs).** M = printed
by a log, a tool or a probe; I = inferred. Seven of nine runs ended on the 9,728 MiB wired stop (world+10..+84 s; the
machine's idle wired was 2.6 GiB instead of 2.0 before gp-g..i); gp-d and gp-f ran to the watchdog (world+59/+65 s).
So the numbers are at world+15..+65 s, not at +60 only.

### 1. The frame today (M)

- **GPU ~550-600 ms a frame, 1.4-1.8 frames/s**, any queue 92-100% busy (gp-d, gp-e, gp-f at world+20..+65 s; gp-f:
  553-602 ms, 1.37-1.80 frames/s). The 181-198 ms of the brief were pool-off runs (fcpu-h) with no G-buffer geometry;
  with the pool (default since 07:22) the G-buffer draws the world and the frame is GPU-bound.
- One complete frame, serialized (gp-d, `GTA6RT_METAL_PASS_TIMING=110,3`, direct-queue cbs 30024-30120, 286 encoders,
  37 untimed): direct queue **540 ms** timed = render geometry (vertex-stage samples) **306**, render fragment **89**,
  compute ~145; compute queue 16 ms. The G-buffer command buffer alone is 354 ms.

| # | encoder | attachments | first .. last pipeline | binds | indirect records (non-empty) / mesh groups | ms | geometry / fragment |
|---:|---|---|---|---:|---|---:|---|
| 1 | render | 1280x720 f29,f28,f34,D20 | `+MS_VS_PropFoliageDeferred_7cf79608~8` / `PS_PropFoliageDeferredTo_GBuffer_490a951c` .. `MS_VS_OctaSLODFire_5fd19871` | 970 | 970 (628) / 4,334 | **164.0** | 135.8 / 28.1 |
| 2 | render | 1280x720 G-buffer (6 RT + D20) | `_MS_VS_TransformD_c5fd36bb~23` / `PS_DeferredTextured_c5fd36bb` .. `MSR_VS_TransformSkinD_*_34f92402` | 1,317 | 1,317 (1,233) / 7,295 | 61.1 | 53.1 / 8.0 |
| 3 | compute | - | `lib_cs_02860` .. `lib_cs_02099` | 85 | - | 41.1 | - |
| 4 | render | G-buffer | `_MS_VS_TransformD_c5fd36bb~153` / `PS_DeferredTextured_942fcba4~30` | 313 | 313 (263) / 921 | 28.0 | 25.3 / 2.7 |
| 5 | compute | - | `lib_cs_02038` .. `lib_cs_01972` | 69 | - | 26.2 | - |
| 6 | render | 1280x720 f29,f28,f34,D20 | `2MS_VS_TransformTerrainNormalTint_e74ae427~8` / `PS_DeferredTexturedStdNC_48fb7423` | 174 | 174 (146) / 2,900 | 23.0 | 22.7 / 0.3 |
| 7 | render | G-buffer | `!MS_VS_Transform_4dbafe93~6` / `PS_DeferredTextured_4dbafe93~6` .. `c5fd36bb~72` | 194 | 194 (191) / 2,110 | 22.3 | 5.7 / 16.7 |
| 8 | render | G-buffer | `_MS_VS_TransformD_c5fd36bb` / `PS_DeferredTextured_942fcba4` .. | 1,305 | 1,305 (642) / 4,010 | 13.9 | 10.5 / 3.4 |
| 9 | compute | - | `lib_cs_01379` .. `lib_cs_01403` (irradiance cache) | 668 | - | 12.8 | - |
| 10 | render | 640x2560 D55 | `+MS_VS_CascadeShadows_draw_f6ce4799` .. `VS_OctaVsplitWindOrtho_e9795e82` | 1,716 | 1,713 (1,244) / 8,944 | 12.3 | 11.9 / 0.4 |
| 11 | render | G-buffer | `_MSR_VS_TransformSkinD_DetailMapsOff_NoDeform_34f92402` .. `MS_VS_TransformD_DetailMapsOff_7095f810` | 154 | 154 (150) / 733 | 11.1 | 10.7 / 0.5 |
| 12 | render | 1920x1080 f24 | `VS_PassthroughComposite_97148495` / `PS_CompositeNMB_97148495` | 1 | - | 9.8 | 0.0 / 9.8 |
| 13 | render | G-buffer | `_MS_VS_TransformD_062b2704~3` / `PS_TexturedDeferred_062b2704~3` .. | 313 | 313 (308) / 2,241 | 9.5 | 4.1 / 5.4 |
| 14 | compute | - | `lib_cs_01259` .. `lib_cs_01320` | 7 | - | 9.4 | - |
| 15 | compute (compute queue) | - | `lib_cs_02069` .. `lib_cs_02095` | 90 | - | 7.9 | - |
| 16 | compute | - | `lib_cs_01105` .. `lib_cs_01087` | 47 | - | 7.7 | - |
| 17 | render | G-buffer | `_MSR_VS_TransformSkinD_DetailMapsOff_NoDeform_34f92402` | 49 | 49 (44) / 453 | 7.5 | 7.0 / 0.5 |
| 18 | render | G-buffer | `_MS_VS_TransformD_c5fd36bb~23` / `PS_DeferredTextured_AlphaClip_c5fd36bb` .. | 383 | 383 (337) / 1,368 | 6.8 | 4.9 / 1.9 |
| 19 | compute | - | `lib_cs_01009` .. `lib_cs_01050` (graph ops) | 368 | - | 5.1 | - |
| 20 | compute | - | `lib_cs_01937` .. `lib_cs_01999` | 18 | - | 4.5 | - |

  "Binds" = pipeline binds in the encoder (one per ExecuteIndirect for the G-buffer); groups are read back from our
  indirect pre-pass records after the frame. Full per-encoder data: `run/logs/gp-d.passes.tsv`; the table:
  `build/gpass-x86_64/gp-d-frame.md`.
- **Per mesh group the G-buffer mesh shaders are 5-25x the shadow one (M):** foliage 31 us a group, `c5fd36bb~153` 27,
  terrain 7.8, `c5fd36bb~23` 7.3, cascade shadows `f6ce4799` 1.3. Draw count is not it: offline probe
  (`build/gpass-x86_64/probe/meshdraw.mm`, own kernels, idle GPU) 0.05-0.2 us of GPU per indirect mesh draw (empty or
  1 group), 10,000 draws ~1 ms. The foliage mesh row is 1.75 MB SPIR-V / 2.34 MB MSL (12.5 s to compile).

### 2. What is ours in the pass structure (M, census `GTA6RT_METAL_PASS_CENSUS=1`, per world frame)

- 166 render passes (47-53 clear-only), 113 compute and 32 blit encoders; attachments loaded 206-217, cleared 62-65,
  dont-care 19-23; stored 292-301, store dont-care 0; ~800 MiB loaded, ~1,070 MiB stored, ~225 MiB cleared.
- What ends a render pass: enhanced barriers (all become UA->UA on Metal) naming an attachment 44-50, naming another
  resource 28-30, the clear-only passes' own flush 33-35, end of command list 26-30, dispatch 10, draw 10, clear 6,
  set targets 4. Only 2-3 passes a frame reopen the previous pass's attachments with nothing between (+~16 with a
  compute encoder between, which cannot merge without moving work).
- Their GPU time (timed, gp-c window 0): clear-only passes ~0.4 ms a frame, blits 0.06, our indirect pre-passes 0.2; a
  split costs ~0.07 ms (median of 434 in-frame segments with <=2 mesh groups, gp-c window 1). The uploads queue is idle
  in the world (no per-frame title texture copies); residency is one set per heap per command buffer (no per-encoder
  useResource for the pool). **So merges and load/store changes can buy ~2-3 ms of ~556.** Not done for that reason.
- Tried and removed: keeping a pending clear across a barrier so the next pass folds it into its load action (-4.5
  clear-only passes a frame, gp-d). **It made the presented world black** (same binary b8, gp-e on: all 6 world dumps
  mean RGBA 1,1,1,0; gp-f off: 10 of 11 normal, ~145,152,167,155). Cause not found (the clear order looked preserved
  in every path I traced). It was in the shared sources ~08:00-08:44: pcpu/mtex/expo binaries of 08:02-08:34 carry it
  (`GTA6RT_METAL_CLEARS_ACROSS_BARRIERS`, default on then; requests file 08:45). Removed at 08:50.

### 3. The lever that worked: private mesh outputs on the hot rows (M)

- Hw-route mesh rows without `private_mesh_outputs` stage every output in 128-entry threadgroup arrays: 21,720 B a
  group for foliage and `c5fd36bb~153`, 17,624 B for terrain (Metal has 32 KiB a core, so about one group at a time).
  The mcc worker's `shaderc_msl/tools/mesh_out_private.py --force` rebuilt 6 store rows with the store recipe plus that
  pass: **21,720/17,624 -> 1,112 B** (`build/gpass-x86_64/mop/overlay.tsv`, `mesh_out_private.jsonl`):
  `+MS_VS_PropFoliageDeferred_7cf79608_Wrapped~8`, `~6`, `1MSNC_VS_PropFoliageSkinDeferred_4352f5e1_Wrapped~8`,
  `2MS_VS_TransformTerrainNormalTint_e74ae427_Wrapped~8`, `~11`, `_MS_VS_TransformD_c5fd36bb_Wrapped~153`.
- gp-i (b9, that overlay first, then the default four; timing window at world+15 s, 3 frames, 1 encoder untimed):
  foliage pass **164 -> 82.5 ms** (geometry 135.8 -> 57.4 for 4,334 -> 4,352 groups: 31 -> 13 us a group), terrain
  geometry 22.7 -> 16.5 ms (2,900 -> 2,992 groups), `c5fd36bb~153` 25.3 -> 17.6 ms (921 -> 950 groups); whole frame
  serialized 556 -> 441 ms. 10 s GPU lines, world+17..+47 s: **487-495 ms a frame, 1.79-2.00 frames/s** against gp-f
  (same defaults, no overlay, world+20..+65 s) 553-602 ms, 1.37-1.80 frames/s, and gp-d 557-585 ms, 1.69-1.80.
- Look: world presents of gp-i have the same scene, over-exposure and glitches as gp-f, palms drawn (means 125-149 /
  135-164 / 169-183 RGB against gp-f's 127-149 / 141-154 / 165-173). No new artifact seen. Example (not a milestone, still
  over-exposed with streaked geometry): `/Users/a/Desktop/Projects/GTA-VI/port/run/present/gp-i-world/world_present_001887.png`.
- **Not default** (the default overlay list is in `shims/d3d12_metal_device.cpp`, not my file): device-shim owner /
  mcc worker, put these 6 rows in front (or fold them into `build/mcc-x86_64/overlay.tsv`); cache worker, these are 6 new
  pipeline keys for the tier (the foliage one compiles in ~12 s). Gp-g/h/i all ended on the wired stop at
  world+10..+48 s; the graphics ledger with the overlay was the same as without (4.3-4.4 GiB at world+6..+11 s, gp-h vs
  gp-f), the machine's idle wired was 2.6 GiB in those runs.

### 4. What remains, and 30 frames/s (I from M)

- After the overlay, a frame is ~440-490 ms: G-buffer geometry ~215 ms (`c5fd36bb~23` 60, foliage 57, vehicles split64
  `34f92402` 21, `c5fd36bb~153` 18, cascade shadows 17, terrain 17), fragment ~90 (heaviest `PS_DeferredTextured_4dbafe93`
  16 ms, the 1080p composite 10, foliage PS 25), compute ~150 (`lib_cs_02860..` 28-41, `02038..` 19-26, irradiance 9-13).
- Next rows without private outputs: `_MS_VS_TransformD_c5fd36bb_Wrapped` (11 ms, 4,191 groups), `!MS_VS_Transform_4dbafe93~6`
  (5.7), `_MS_VS_TransformD_062b2704~3` (4.1). The split64 vehicle rows (`34f92402`, 15 us a group) are a different route.
- **30 frames/s needs <= 33 ms a frame: ~14x less than today's ~470.** It is not the title's own cost at this
  resolution: the title renders 1280x720 internally and composites 1920x1080; Series S (4 TFLOPS) does its whole frame
  in <= 33 ms, this M4 Pro GPU (20 cores, ~2x Series S FP32) should then need ~15-20 ms for the same shaders (I). The
  gap is the translated shaders' execution: geometry stages of emulated wave64 primitive shaders (2.3 MB of MSL for one),
  mesh-output staging, emulated VGPR/SGPR arrays in pixel and compute shaders. Encoder-side items (splits, load/store,
  blits, residency) total <1%.
- Not measured here: the tiled-lighting rect draws are currently dropped at encode (exposure worker, 08:30); when they
  run, the frame grows (180 rect draws at 1280x720).

### Switches added (port/metal `mtl_queue.mm`, all off by default)

`GTA6RT_METAL_PASS_CENSUS=1`; `GTA6RT_METAL_PASS_TIMING=<s>,<frames>[,<period s>]` with `_TSV=<file>`, `_SERIAL=0`,
`_SPLIT=1|2` (port/metal STATUS.md round 12). Nothing else changes default behaviour.

### Next (ranked)

1. Make the 6 private-output rows default (overlay list / mcc overlay) and re-prebuild their tier pipelines; then
   `mesh_out_private.py --scan --force` over every hw-route mesh row that stages outputs, A/B with the pass timer.
2. Translator: the geometry stage of `c5fd36bb~23` (7 us a group with private outputs, 60 ms a frame) and the foliage
   row (13 us a group after); the split64 vehicle rows (15 us a group).
3. Compute: `lib_cs_02860..02863` (28-41 ms a frame) and `02038..01972` (19-26 ms): look at their translated code.
4. Find why keeping clears across barriers blackened the frame before trying any pass merge again.

## Round 12, material-texture worker (2026-11-27 07:23-08:40): textures streamed in part get Metal textures of their resident levels; title-built T#s get textures with the right layout; both on by default

Tree `build/mtex-x86_64` (configured with `d3d12_metal/superbuild/attach.cmake`; `-build1..2.log`), launcher
`build/mtex-x86_64/run_world.sh` (the lit-frame launcher with this tree's run dir, the tmem probe lock, cap 14,848 with
no heavy job else 13,312, `GTA6RT_METAL_CB_ABORT_MS=2000`, a 4x/s machine-wired watch that ends the run at 9,728 MiB,
`run/logs/<run>.wired.tsv`). Runs mtex-census, -a, -b, -off, -c, -d, each alone (slot + lock), hidden window, imported
save. **0 failed command buffers, 0 GPU page faults; gpuEvent-gta6rt 9 before and 9 after every run.** M = printed by a
log, a probe or a tool; I = inferred.

### What is built (`shims/d3d12_metal_device.cpp` only)

- **Resident levels (default on, `GTA6RT_METAL_TEXTURE_PARTIAL=0` off).** `title_texture_readable`: a texture whose
  mapped prefix holds levels b..n-1 whole (addrlib's tail + small mips come first) is readable from b, floored at the
  lowest BASE_LEVEL any title-built T# named for it (`Resource::note_tsharp_base`; the pages past level b may hold
  another texture). Not for guessed layouts, arrays or 3D. `fill_title_texture` then asks the deferred texture for a
  Metal texture of levels b.. only (`Resource::want_mip_base` -> `make_deferred_texture` -> `levels_desc`), fills them
  (`upload_title_level`), and when the title later names or maps a lower level, makes a bigger one, fills it, swaps it
  in (`begin_rebase` / `finish_rebase`; the old one is released once the queues passed it) and re-makes every view of the
  texture over it (`DeviceState::rebind_image_views`, new indices; `ImageEntry` keeps what each view was made from).
  Views of such a texture are shifted (`DeviceState::make_view`, `mip_bases`): a view's level range maps onto the
  levels held, clamped to the first one (the same LOD-relative-to-base sampling the T#'s BASE_LEVEL gives on the
  console). Growth is driven by the title-memory map journal (`title_memory_map::changes_since`: a texture over a
  change is marked stale) and by a T# naming a level below the texture's first (`note_tsharp_wanted`); no polling.
- **Title-built T#s with no texture (default on, `GTA6RT_METAL_TSHARP_TEXTURES=0` off)** (`make_orphan_textures`):
  layout from the T#'s swizzle field when it names 4KB_S/64KB_S/64KB_D, else (always, measured below) SW_4KB_S unless
  the largest level the T# names reads clearly smoother as 64KB_D or linear (`layout_coherence` /
  `pick_tsharp_layout`: neighbour vs random block-endpoint difference, < 0.6 of 4KB_S's); levels BASE_LEVEL and up
  only (`TitleTexture::base_floor`), span = up to the end of level BASE_LEVEL (`make_title_texture`'s new
  `resident_span`); chain = MAX_MIP+1 when plausible, else LAST_LEVEL+1 / a BC chain to 4x4. An older T#-made texture
  over the same bytes is retired (the title reuses its pool); other registered textures are checked against the bytes
  they use (`used_span`). At most 1.5 ms a pass (`GTA6RT_METAL_TSHARP_TEXTURES_US`); refused T#s are queued again
  every 10 s.
- **Budget:** `GTA6RT_METAL_TITLE_BACK_MB` now caps the Metal textures of (resident-level + T#-made) textures, **512 MiB
  when unset**; a texture past it stays deferred (`over_budget`). Full-size deferred textures are unchanged (no limit
  unless the variable is set, as before). Only textures something samples (shader views) are made.
- `GTA6RT_METAL_FILL_TSHARP_TEXTURES` default on (a T#-named deferred texture is filled at the next pass).
- Report line every 10 s (default): `title textures streamed in part: N made, grown, levels held, Metal MiB of budget,
  refused; T#-made textures (by swizzle mode; BC layouts by content), not made by reason`. Opt-in census
  (`GTA6RT_METAL_TEXTURE_CENSUS=1`) adds `title-built T#s ...` (each sampled T# against the texture it bound to) and
  `GTA6RT_METAL_TSHARP_DUMP=<dir>` writes T# words + the bytes they name (for `tsharp_layout.cpp`, scratchpad tool).

### 2. The T# decode, checked against textures the API made (M; mtex-census, dump of 1,024 T#s, 256 MiB of bytes)

- **The same texture seen both ways:** 756 title-built T#s in one census window bound to slot-133 textures whose
  slot-70 view gave the layout. Extent equal 736/756, format code equal 717/756 (the rest UNORM/sRGB or BC5/BC7 views
  of reused memory), MAX_MIP equal 713, more 33, fewer 10 (words 4-7 carry floats: `bf7ffffc b605dde7`), **swizzle
  mode 0 in 756/756 although the views say 4KB_S (5) or 64KB_D (10)**: title-built T#s are the layer's own words copied
  and patched (address, BASE_LEVEL), so they cannot carry the layout. BASE_LEVEL > 0 in 496/756; MIN_LOD > 0 in 0.
- **Layout from the bytes:** decoding the largest whole level of each dumped BC texture under linear / 4KB_S / 64KB_S /
  64KB_D with the layer's XG model (`xg_xs_layout.h`) and scoring block-endpoint smoothness: textures whose view said
  4KB_S read best as 4KB_S 68 of 69 decided (1 as 64KB_D), 64KB_D as 64KB_D 21 of 24 (3 as 4KB_S): the model and the
  scorer agree with the API's layouts. T#s naming no texture (250 BC): **4KB_S 236, 64KB_D 4, linear 1**, 9 no answer.
  The old opt-in path took mode 0 = linear for all: that was the sand's magenta blotches. In the world (mtex-c) the
  runtime picker chose 4KB_S 41,660 times, 64KB_D 798, linear 457 (2,344 no answer -> 4KB_S).
- **Pool packing:** T#s with base 3 for two 1024x1024 BC1 textures sit 16 KiB apart (levels 3-8 = 12 KiB at 4KB_S): the
  title allocates only the resident levels, so a texture's T# address and BASE_LEVEL bound the bytes that are its own.

### 1. Zero copy and memory (M)

- **Metal refuses linear textures in BC formats** ("Linear textures do not support compressed pixel formats",
  `newTextureWithDescriptor:offset:bytesPerRow:` returns nil for BC1/BC7, M4 Pro, macOS 27; RGBA8 works), and the
  console's 4KB_S/64KB_D tiling is not Metal's anyway, so these are copies (shared storage, CPU detile, as before).
  Small BC textures cost what they hold (allocatedSize: BC1 16x16 3 levels 0.6 KiB, 64x64 5 levels 3.7 KiB, 256x256 7
  levels 48 KiB), so a texture of resident levels is cheap.
- Metal textures of resident levels + T#-made: **191 MiB** (mtex-b: 10,050 + 12,431 textures), **204 MiB** (mtex-c:
  5,988 + 8,814 at world+78 s), budget 512 never reached. Against the same binary with both off (mtex-off), the
  layer's ledger at t=134 s: 6,865 vs 6,481 MiB (+384), machine wired in the world 9.1-9.7 vs 8.7-9.3 GiB.
- **That puts runs at the wired limit:** mtex-b (census on, no pass budget) ended on the 9,728 MiB watch at world+41 s;
  mtex-c reached world+78 s and ended on the 14,848 MiB footprint cap (wired peak 9,722); mtex-d ended on the wired
  watch at world+82 s (9,796; 170 MiB of these textures, 7,561 + 12,371); mtex-off ran to the watchdog (wired peak
  9,428). All three `GTA6RT_METAL_PCACHE=0` (tier off, as pdef-c), load 3.8-6.1.

### 3. Coverage (M, albedo target `f29` 1280x720, the stilt-house lot right after the story load)

| run | binary | settings | frame 2 albedo non-zero | geometry with black albedo |
|---|---|---|---:|---:|
| mtex-off | 07:53 | both off, `PCACHE=0` | **12.9%** | |
| mtex-b | 07:53 (same) | both on, census on | **38.8%** | 35.8% of the frame (48.0% of geometry) |
| mtex-c | 08:12 | both on, 1.5 ms pass budget | 10.9% | |
| mtex-d | 08:22 | as mtex-c, + BC chain rule | 16.4% (frame 12: 21.4%, frame 42: 24.4%, camera moved by then) | |
| pdef-c (round 11) | | | 17.3% (frame 62: 21.9%) | 57.8% (72.6%) at frame 62 |

- Frame 2 is right after the load screen and depends on how far streaming got (mtex-c's frame 2 still lacks the dock
  and the house's deck geometry), so one frame is a noisy measure; the later scene dumps of these runs picked another
  target set (`f41`, the probe's choice). The same-binary A/B is mtex-off vs mtex-b (12.9% -> 38.8%, x3.0). With the
  1.5 ms pass budget (mtex-c/-d) the T#-made textures are made over the first world seconds instead of before frame 2,
  so frame 2 shows less (10.9 / 16.4%) and later frames more (mtex-d frame 42: 24.4%, the left house's siding, railings
  and stairs textured: `/Users/a/Desktop/Projects/GTA-VI/port/run/present/mtex-d-scene/scene_00042_gbuffer0.png`).
  Not measured: the same later frame with both off in the same position (mtex-off's later dumps also picked `f41`).
- **Frame rate:** mtex-b 0.6-1.0 presented frames/s in the world (T#-made textures scored and made with no time limit
  on the submitting thread, census on) against 1.6-1.8 for mtex-off; with the 1.5 ms pass budget (mtex-c) 1.6-1.8.
- **Largest remaining classes of black-albedo geometry** (mtex-b frame 2, by pixels of the frame; geometry = normal
  target non-zero; classes named by looking at the mask a magenta mask, rough rectangles, so the shares are approximate): 35.8% of the frame in all; the left house's walls
  (with the bare tree in front) ~8.3%, the stilt house's siding and roof ~6.8%, the beach ~5.9% (textured later in
  pdef-c's frame 62, so streaming time), the tall trunks / masts at the right ~5.4%, seawall tops and walkways ~3.8%,
  the yard ground ~3.2%, the canal strip ~0.5%, other ~1.8%. The census of the same run says why the walls and ground
  stay black: of the T#s still naming no texture, 58% point inside a registered slot-133 texture's range (item 2 of
  Next) and the rest are refused or not yet made.

### Images

- G-buffer albedo after: `/Users/a/Desktop/Projects/GTA-VI/port/run/present/mtex-b-gbuffer-albedo.png` (mtex-b, frame 2,
  38.8% non-zero): the stilt house's deck, railings and stairs, the seawall, the dock, rocks, plants, the tin roof of the
  left house, the apartment block. **Wrong:** house walls (siding) and the big tree trunks are still black, so are the
  yard ground, the beach and the canal surface; the poles/masts at the top right are black. Before (same binary, both
  off, same position): `/Users/a/Desktop/Projects/GTA-VI/port/run/present/mtex-off-gbuffer-albedo.png` (12.9%).
- No presented frame: the exposure fix had not landed (requests file, 08:25); mtex-c's world presents were black
  (post chain still compiling with `PCACHE=0`).

### Not done / next (ranked)

1. Memory: the +0.3-0.5 GiB of these textures needs room under the wired limit (the pool-CPU worker's graphics step);
   evicting textures no recent draw sampled needs a per-draw use mark (replay, not mine).
2. T#s inside registered slot-133 ranges whose readable prefix runs into packed neighbours: with no title T# naming
   a base, `used_span` is the whole layout and the T#s there stay unbound ("overlaps a registered texture" 10,781 in
   mtex-c); `T# names mips the texture lacks` 3,502 (two T#s of one packed texture disagreeing on the chain).
3. Writing the console swizzle mode into the layer's slot-70 T#s would let the title's copies carry it (hash keys
   change: descriptor owner).
4. Arrays/cubes/3D T#-made textures still need their whole layout readable.

## Round 12, pool-CPU / memory worker (2026-11-27 07:22-08:35): the world's graphics step is the driver's scratch pools; the retry list and the full live re-fixes are gone; story load and the memory targets are not met

Tree `build/pcpu-x86_64` (`-build1..8.log`; binaries kept as `gta6rt.base` = live sources of 07:37 before my device-shim
edits, `gta6rt.new` .. `gta6rt.new4`), launcher `build/pcpu-x86_64/run_world.sh` (pooldef's, own run dir, tmem probe
lock, cap 14,848 / 13,312 when a heavy job runs, `GTA6RT_METAL_CB_ABORT_MS=2000`, machine wired polled 4x a second and
the run ended at 9,728 MiB, gpuEvent count in `.meta`). Runs `pcpu-a..e`, each alone, hidden window, imported save,
tier on, pool on, rt-alias on, default overlays, scene dumps. **0 GPU errors, 0 failed command buffers; gpuEvent-gta6rt
9 before and 9 after every run.** M = printed by a log, a tool or a probe; I = inferred.

| run | binary, settings | load at start | story load | world entry: footprint / machine wired (MiB) | last sample | end |
|---|---|---|---:|---|---|---|
| pcpu-a | base (before) | 6.5 (ninja running: cap 13,312) | 26.8 s | 11,247 / 7,421 | world+21 s: 13,277 / 9,484 | footprint cap 13,312, world+~26 s |
| pcpu-b | new (pool wake + touched re-fix) | 5.7 | 27.3 s | 11,189 / 7,415 | **world+62 s: 13,547 / 9,263** | wired 9,899, world+~80 s |
| pcpu-c | new2 + `GTA6RT_METAL_TITLE_POOL_SCAN_STRICT=1` | 4.1 | 18.3 s | 11,247 / 7,479 | world+35 s: 13,899 / 9,549 | wired 9,925, world+~40 s |
| pcpu-d | new3 + `GTA6RT_METAL_TITLE_POOL_CHUNK_KB=256` | 5.0 | 38.5 s | 11,792 / 6,962 | world+61 s: 14,270 / 9,100 | footprint cap 14,881, world+~72 s |
| pcpu-e | new4 (final defaults), `sample` during the story load | 6.8 | 33.0 s (sampled: perturbed) | 11,508 / 7,238 | world+26 s: 13,861 / 9,402 | wired 9,925, world+~31 s |

No run reached world+150 s (B and D passed +60 s), so the asked +150 s column does not exist; the
targets (world+150 s under 11 GiB footprint and 8.5 GiB machine wired) are **not met**: every run is at 11.2-11.8 GiB
footprint and 7.0-7.5 GiB machine wired already at world entry (machine idle: 1.9-2.4 GiB wired).

### 1. The graphics step (M unless marked)

- **The overlay's 00968 is the one the title uses:** every round-11/12 run's pcache record (`rundir/pipeline-archive/
  pcache-runs/*.tsv`: lit, rtal, fcpu, pcpu) adopts pipeline `fd5f37fefb3fd105` for `library/lib_cs_00968`, which the
  tier manifest lists as `cs=library/lib_cs_00968@5bede920d7dcada4` = the tmem overlay's content key; the store's old key
  `138df4c4` (pipeline `23b30641`) is adopted by none of them.
- **What the step is now (pcpu-b, footprint snapshots + 1 s trace):** "Owned physical footprint (unmapped) (graphics)"
  1,365 MiB at world-2 s -> 1,896 MiB at world+3 s, then the graphics ledger +643 MiB in one second at world+6 s with
  Metal's allocated size +13 MiB. So ~1.1-1.2 GiB of driver-owned memory outside `currentAllocatedSize`, in two steps,
  as the first world frames run: the driver's per-thread scratch pools (compute and render are separate and add up,
  translator round 9). From the pipelines this run actually used (its pcache record) and the translator's per-shader
  probe values: compute pool = `lib_cs_00977` 349.6 MiB (00974 301.5 next; 00968 now 100.7), render pool =
  `%PS_DeferredTextured_4dbafe93_Wrapped~24` 520 MiB (`~19` 376; `~23`, the 580 MiB leader, was not used in 150 s),
  plus ~80 MiB per pool: ~1.0 GiB expected (I) against ~1.1-1.2 GiB seen. MetalFX is not it: its first dispatch comes
  before world entry. Pipelines are not it either with the tier on: they are made before the world (pdef-a: "outside
  every backend category" 745 MiB at t=43 s).
- **Libraries (probe, `build/pcpu-x86_64/probe/lib_release.mm`, compile only, no dispatch):** 600 store compute libraries cost
  +268 MiB footprint, 0 graphics, and releasing them returns **nothing** (also after `malloc_zone_pressure_relief`);
  40 small pipelines +0.5 MiB graphics. So libraries are footprint, not the graphics step, and dropping them saves nothing.
- **Not shipped:** 00977/00974 merged rows (-152 MiB of compute scratch at most, then 00974 decides): no way to capture a
  title dispatch's inputs from my files (the binding state is recorded in `metal_replay.cpp`), so they stay unverified
  and out of the overlay. The render leader's spill is not the typed-read pattern (translator: 520 -> 492 with
  merge_typed_reads); not changed.
- **Where wired goes at world+30 s (pcpu-b/e, M):** pool chunks 1.83-1.86 GiB (all from Create*View / tables / root
  views: heap-slot V#s made 1-46 MiB of it, new counter), driver scratch ~1.1 GiB, pipelines ("other") 1.06-1.18 GiB,
  render targets 1.0 GiB, title copies 0.86-0.92 GiB, arena/descriptors 0.44 GiB, plus ~2 GiB for the rest of the
  machine. Footprint also grows by compression: 0 -> 4.2 GiB compressed within 10 s of the world (machine pressure).
- **Pool chunk size (pcpu-d, opt-in `GTA6RT_METAL_TITLE_POOL_CHUNK_KB`):** 256 KiB chunks wire 1,426 MiB instead of
  ~1,886 (-0.45 GiB) and the run lived 71 s in the world (cap, not wired), but chunk making exploded (871 VA holds of
  142 GB, 1,922 probes missed, 5 chunks refused) and the story load took 38.5 s. Default stays 1 MiB.
- **Strict heap V#s (pcpu-c, opt-in `GTA6RT_METAL_TITLE_POOL_SCAN_STRICT=1`):** refused 36-52 chunks; no memory effect
  (heap slots are not who wires the pool). Its 18.3 s story load is not attributable to it (I: see 2).

### 2. Story load: not fixed

- **Built: pool wake (default on, `GTA6RT_METAL_POOL_WAKE=0` = the old queue-all rule).** xmem logs the title ranges that
  gain a pool view (`rt::xmem::title_pool_additions`, beside `title_pool_removals`). A slot left with an empty V# is filed
  under the 2 MiB title granules its empty half names; at each refresh the ranges added since the heap last looked wake
  only the slots filed there. A half no pool change can back is not filed (span over 32 MiB = more than a span buffer
  holds, base below 64 KiB or past 2^47). Measured (pcpu-e world): per 10 s 54-113k empty slots filed, 337-664k not
  filed, 0 woken (0-104 visits in 36-79 pool changes); **retry list 5-22k waiting against 0.85-1.55 M in pcpu-a**;
  "retrying" 0.51-0.55 ms a submission against 1.06-1.19 in pcpu-a.
- **It did not move the story load:** 26.8 s (a) vs 27.3 s (b), same machine load; the five runs spread 18.3-38.5 s.
  During the story load the GPU is 15-25% busy and the submit phases 11-39% of the wall time in every run; the 6 s
  `sample` of pcpu-e (`run/logs/pcpu-e.sample.txt`; Rosetta, no symbols) shows `game main` in a wait 87 of 96 samples
  and a few title worker threads in title code (I: the title waits on its own streaming, not on the layer). Next: a
  symbolized profile (Instruments on the arm64 side, or the os's own thread CPU counters per thread name).

### 3. Live V# re-fix after a release (default on; `GTA6RT_METAL_REFIX_ALL=1` = every live V#)

- `refix_plan` replaces `refix_ranges`/`refix_snapshot`: the alias snapshot is taken where the re-fix decides; a full
  re-fix "because an invalidation came while slots were fixed" is replaced by re-checking only the slots fixed since
  the last snapshot (`ConsoleHeap::refix_touched`), and of those only the ones whose rebased halves no longer match the
  alias table (`range_of` gives another shader VA or none) are re-fixed. An unchanged table (same size and invalidation
  count) empties the set. Full re-fixes remain only for a heap's first refresh and a release log that no longer reaches
  back.
- Measured (world): **full re-fixes 4 (141-358k slots each) in pcpu-a's 21 s, 0 in pcpu-b/-e**; re-fix 0.13-0.27 ms a
  refresh (pcpu-e; pcpu-a 0.37-0.79), longest single re-fix 5.8-12.8 ms; 696k touched slots kept by the table check in
  pcpu-e. The 100-180 ms `heaps` spikes are now the scan (pages, up to 69 ms) and the fixing of changed slots (up to
  135 ms: the title rewriting large spans), new line `pool wake ... longest refresh parts`.
- Not built: an index of live V#s by resolved range; the walk over 0.6 M live V#s per limited run is ~10 ns a slot
  (~6 ms) and is now most of the re-fix time.

### Refresh per submission (heap line, world, M)

| | pcpu-a (before) | pcpu-b | pcpu-e |
|---|---:|---:|---:|
| refresh avg | 4.68-5.74 ms | 4.20-5.91 | 4.51-5.65 |
| of it retrying | 1.06-1.19 | 0.51-0.55 | 0.51-0.53 |
| live V# re-fix per refresh | 0.37-0.79 | 0.12-0.50 | 0.13-0.27 |

### Files

`shims_core/src/xmem.cpp/.h` (`note_added(lo, hi)`, `g_additions`, `title_pool_additions`); `shims/d3d12_metal_device.cpp`
(xmem declaration; `RefixState`, `refix_plan`, `pool_wake_on`, `wake_range/file/note/queue/bucket`, `pool_wake`,
`wake_report`; `ConsoleHeap` fields; `fix_console_slot`, `fix_console_slots` one line; `refresh_console_heap`'s pool
block and live re-fix block; `title_pool_chunk` / `title_pool_window`: wiring attribution, `..._SCAN_STRICT`,
`..._CHUNK_KB`). shims_core tests not rerun (the xmem change is an additive log). No overlay row added.
New log line every 10 s per resource heap: `[rt] d3d12-metal: pool wake (last 10 s, ...)`.

### Next (ranked)

1. Wired: pool chunks (1.85 GiB) are the largest term and need release when no descriptor names them (hook where
   root views translate, `d3d12_com.cpp`/`metal_replay.cpp`), or cheaper small chunks (the 256 KiB try fails on VA probes).
2. Render scratch (520-580 MiB, PS_DeferredTextured_4dbafe93 variants): find the spill cause in the translator.
3. A dispatch capture in replay to verify 00977/00974 on title inputs (-152 MiB).
4. Story load: a symbolized profile; the layer's heap work is not it.

## Round 11, pool-default worker (2026-11-27 06:07-07:22): the title pool is the Metal default; pool wiring = what is named; boot with the tier fixed; spacers cost nothing

Tree `build/pooldef-x86_64` (`-build1..12.log`), launcher `build/pooldef-x86_64/run_world.sh` (s7_world_run.sh with this
tree's run dir, the tmem probe lock, cap 14,848 when no prepare_shaders/ninja/clang runs at the start, else 13,312,
`GTA6RT_METAL_CB_ABORT_MS=2000`, memory report every 5 s, and a 1 s vm_stat watch that ends the run past 10,240 MiB
machine wired: `run/logs/<run>.wired.tsv`). Runs pdef-a..f, pdef-boot2..5, pdef-off, pdef-vk, each alone (slot + lock),
hidden window, imported save, load 3-8 at the starts. **0 failed command buffers, 0 GPU page faults; gpuEvent-gta6rt 9
before and 9 after every run (13 runs).** M = printed by a log, a tool or a probe; I = inferred.

### What changed (defaults now; switches restore the old behaviour)

- **xmem** (`shims_core/src/xmem.cpp/.h`): the title pool is on when `GTA6RT_GPU=metal` (`GTA6RT_METAL_TITLE_POOL=0`
  off, `=1` forces it on any path); with it `GTA6RT_VM_BASE_HINT=0x2000000000` unless set (tpool-b: without it chunks
  collided with title memory). Free physical slots are a bitmap: a multi-page allocation takes a consecutive run where
  one is free (else fresh slots while <= `LEONIDA_XMEM_CONTIG_SLACK_MB`, 256, sits on the free list), any other the
  lowest free slot. **A range below 4 GiB is held from process start** (`LEONIDA_XMEM_LOW_HOLD_MB`, 256 on the Metal
  path, 0 elsewhere) and the title's 32-bit reservation is placed in it; once per run a line says what holds the low
  4 GiB when the title asks (`xmem: below 4 GiB when the title asks`). New tests in `test_xmem_pool.cpp` (consecutive
  slots after fragmentation; a 32-bit reservation lands in the held range): shims_core_test 101/101 with the pool off,
  on (`=1`) and Metal-default.
- **port/metal**: `BufferDesc::va_only`: a placement-sparse private buffer (macOS 26+) with no pages: GPU VA only.
  Probe (M): a 1 GiB one costs +0 MiB `currentAllocatedSize`, +1 MiB footprint, +2 MiB machine wired, and moves the next
  buffer's GPU address by 1 GiB; a bytesNoCopy "hold" costs +1,024 MiB allocated size, a private spacer +1,040.
  `metal_test_titlepool` gained `va_holds` (and Metal hands the buffer made right after a released probe the probe's
  address: 16 of 16 there, 64 of 64 in a probe with interleaved allocations): PASS; memory, hostmem: PASS.
- **Device shim** (`title_pool_*`, `claim_shader_range`, `refresh_console_heap`'s live re-fix, `backing`'s release
  branch, `title_window_in_place`, `external_window`'s spacer, the overlay string):
  - chunks of **1 MiB** (`GTA6RT_METAL_TITLE_POOL_CHUNK_MB`) in the **device residency set** instead of named in every
    encoder (`GTA6RT_METAL_TITLE_POOL_NAMING=encoder`: the old way; both wire a chunk whole until it goes, the set costs
    no CPU per encoder); span buffers up to 32 MiB.
  - a chunk's shader VA is probed with a VA-only buffer first; a colliding range keeps that VA-only buffer and a
    VA-only spacer skips the occupied range (the private spacer is only the fallback before macOS 26). Same for
    `external_window`'s spacers. Metal's own CPU mappings (VM tag 100) no longer count as title memory in
    `claim_shader_range` (pdef-b: they refused every try and, after 8 failures, the pool stopped making chunks); a
    refused range now refuses that chunk only, after 64 tries.
  - ranges on scattered pool pages get a copy window from their own budget (`GTA6RT_METAL_TITLE_POOL_SCATTER_MB`, 64).
  - the pool's empty-slot requeue runs at most every `GTA6RT_METAL_TITLE_POOL_RETRY_MS` (250).
  - **live re-fix limited** (`GTA6RT_METAL_REFIX_ALL=1`: every live V# as before): after a released backing or an
    alias invalidation only the slots whose title V# reaches an alias that is gone since the heap's last refresh (diff
    of the alias table against a snapshot taken as that refresh ended) or whose GPU copy names a released backing
    (`backing` logs released shader ranges). Every live V# when an invalidation came while slots were being fixed, at
    a heap's first refresh, or when the log no longer reaches back.
  - default shader overlays: tmem, mcc, msl-overrides, uint (lead 06:15).
  - `GTA6RT_METAL_TITLE_WINDOWS_IN_PLACE=alias` is removed (one line says so; the pool backs those pages).

### Measured (M)

| run | binary, settings | story load | world entry: footprint / machine wired (MiB) | world+60 s | pool | end |
|---|---|---:|---|---|---|---|
| tpool-h (round 10) | old pool, 8 MiB chunks per encoder, `PCACHE=0` | 24.3 s | 10,695 / 9,069 | - | 2,704 MiB wired for 1,524 named, 806 scattered, spacers 1,745 MiB | cap, world+9 s |
| pdef-a | 4 MiB per encoder, **tier on** | 29.3 s | 12,033 / 10,124 | - | 1,916 MiB wired at 512 chunks | wired watch 10,409 at world+~2 s |
| pdef-b | 1 MiB residency set, before the claim fix | 25.3 s | 10,973 / 6,666 | 14,500 peak / 8,490 | **chunk making stopped** after ~130 chunks (VA refusals) | watchdog, world+83 s |
| **pdef-c** | final pool, tier off (`PCACHE=0`) | 25.4 s | **10,783 / 8,126** | **14,002 / 9,934** | **1,674 MiB wired = 1,674 named** (8,192 ranges), 45 scattered, 0 refused, spacers with memory 0 | wired watch 10,490 at world+63 s |
| pdef-d | pdef-c + `REFIX_ALL=1` | 24.8 s | | | | watchdog, world+29 s |
| pdef-e | + shared sources of 07:08 (rt-alias on), pool | 23.3 s | ~6,800-7,300 (vm_stat) | world+20 s 8,600-9,000 | 1,738 MiB for 1,738 named, 145 scattered | watchdog |
| pdef-off | same binary as pdef-e, `GTA6RT_METAL_TITLE_POOL=0` | **14.7 s** | ~5,100-5,300 | world+28 s 6,560 | - | watchdog |
| pdef-f | + requeue limit | 23.9 s | | world+20 s 9,400 | | watchdog |
| pdef-vk | `GTA6RT_GPU=vulkan` (plain pool) | 34.4 s | world at +92.6 s, peak 11,126 | | - | watchdog 130 s, no error |

- **1. Wired.** Pool wiring now equals what descriptors name (1 MiB granularity): 1,674 MiB at world entry in pdef-c
  against 2,704 MiB in tpool-h (-1.0 GiB, with more ranges resolved), machine wired 8.1 vs 9.1 GiB at world entry.
  Spacers with memory 0 MiB (tpool-h 1,745 MiB in Metal's allocated size); VA-only holds 1-9 GiB of GPU VA, 0 MiB
  allocated. **Still over the limit later:** in every pool run the graphics ledger steps +1.4 GiB 6-9 s into the world
  (pdef-c +85 -> +91 s: graphics 3,740 -> 5,118 MiB, Metal allocated 6,416 -> 7,228, machine wired 8,071 -> 9,970), with
  the tmem overlay loaded (8 rows), and pdef-c reached 10,490 MiB at world+63 s. With the tier on (pdef-a) the world
  entry itself is 10.1-10.4 GiB. Not done: releasing chunks no live descriptor uses. Root views resolved in
  `d3d12_com.cpp` and `metal_replay.cpp` hit the alias table directly, so no count in the device shim sees every holder
  of a chunk's shader VA; releasing on such a count would leave recorded root views pointing at freed GPU VA (I). Nor
  per-pass residency: shaders index the heap bindlessly, so a pass reaches every live descriptor.
- **2. Story load: not fixed.** The live re-fix is not where it goes. In the story-load window pdef-c re-fixed 72,857
  slots in 28.6 ms (limited) and pdef-d 1.41 M slots in 515 ms (all), and both loads took 25 s. In the world the limit
  cuts the re-fix from 1.2-1.5 s to 0.03-0.32 s per 10 s (64 limited against 3-4 full re-fixes). The pool's whole cost
  is 23.3-23.9 s against 14.7 s (pdef-e/-f vs pdef-off, same binary): +2.5 s before session 0xa, +6.2 s in the second
  part. Measured parts: chunk making 1.04 s in all; fewer than 256 commits into virtual runs and fewer than 64
  multi-page physical allocations (no line), so xmem's views are not it; the requeue limit changed nothing (23.9 s).
  What differs in the heap line: the retry list holds 0.8-1.1 M slots waiting with the pool against ~20 k without,
  and a refresh costs 5.1-5.6 ms against 4.2 ms (I: more descriptors resolve, so more fixing and more textures; the
  world frame is 1.6-1.8 against 2.8-3.0 frames/s, `heaps` 10-12 ms a submission against 6). Next: a profile of the
  story load with the pool on.
- **3. Scattered ranges:** 806 (tpool-h) -> 45 (pdef-c) / 145 (pdef-e) at 8,192 ranges. The consecutive-run path for
  multi-page allocations ran fewer than 64 times; the drop is the lowest-first reuse of free slots. The rest get copy
  windows: at most 64 windows, 19 MiB, the 64 MiB budget never reached.
- **4. Boot with the tier on: 5 of 5** (pdef-a to the world, pdef-boot2..5 past the reservation to "landing"; fr10-c..g
  stopped there 5 of 5). What fills the low 4 GiB (pdef-a, M): **4,095 MiB mapped, 0 MiB free: Metal's binary-archive
  shards 3,277 MiB in 144 mappings**, the arena's VA reservation 382 MiB, the xmem hold 256 MiB, ICU data 148 MiB, a
  metallib 28 MiB; in the boot runs 13 MiB were left free besides the hold. The reservation is placed at 0xf4700000
  inside the hold (0xf0000000, 256 MiB).
- **5. Defaults.** Pool on for `GTA6RT_GPU=metal`; Vulkan unchanged: the plain pool (`xmem: physical pool 8 GiB`), no
  low hold, no base hint; shims_core_test 101/101; pdef-vk loaded the story (34.4 s; tpool-vk 40.0 s) and ran 38 s in
  the world to the watchdog with no error (that run still had the low hold; it is Metal-only since). The only change
  on the Vulkan path: free physical slots are reused lowest first instead of last in, first out (not observable).

### Milestone images (one each; the lit-frame worker reached all three earlier, 06:07-06:45)

1. G-buffer with albedo: `/Users/a/Desktop/Projects/GTA-VI/port/run/present/pdef-c-gbuffer-albedo.png` (pdef-c, upscaled
   frame 62, target 0: 21.9% of texels non-zero; frame 2: 17.3%, depth and target 1 79.9%; tpool-h 11.1 / 63.4%).
   Palms, a textured wooden dock with a lifebuoy, beach sand, a fence, an apartment block. Wrong: the stilt houses and
   most walls and roofs are black (no material texture), a sheet of stretched polygons crosses the top left, and a
   magenta line runs along a fence.
2. Lit scene colour: `/Users/a/Desktop/Projects/GTA-VI/port/run/present/pdef-b-lit-scene-colour.png` (pdef-b, upscaled
   frame 152; the first non-zero colour was frame 32): sky, clouds, open water, a far coast in haze. Wrong: almost no
   geometry (that run's pool stopped making chunks), a blown-out sky. pdef-c frame 62 (`pdef-c-scene/scene_00062_colour.png`)
   has the lit houses but is over-exposed white (mean 210) with black shadow streaks.
3. Presented window frame: `/Users/a/Desktop/Projects/GTA-VI/port/run/present/pdef-b-world-present.png` (pdef-b, present
   2101, 1920x1080): the HUD (health, armour, $407 / $2,480) over sky and the far coast. Wrong: the lower half is white,
   the clouds posterized, no near geometry.

### Next (ranked)

1. Story load with the pool (+8.6 s): profile it; the retry list (0.8-1.1 M waiting) and the per-refresh fixing are
   the first suspects.
2. The +1.4 GiB graphics step at world+6-9 s (tmem overlay loaded) and the tier's world-entry cost: with both the
   machine passes 10 GiB wired within a minute of the world.
3. Chunk release needs a hook where every alias hit happens (`TitleVaAliases`, read-only for me) or a translation
   table the shader reads (spike/ref-native-victory-macos finding 2).

## Round 11, frame-cpu worker (2026-11-27 06:07-07:22): the heap lock is no longer what sets the frame; the tier boots, ends the compute skips and with them the irradiance runaway

Tree `build/fcpu-x86_64` (`-build1..7.log`), launcher `build/fcpu-x86_64/run_fr.sh` + `run_world.sh` (gates' launcher
with this tree's private run dir, an own archive that starts empty, the tmem probe lock, `cache/tools/fault_stop.sh`
beside the run, and a machine-wired watch every 2 s that ends a run past 10 GiB). Runs `fcpu-a..h`, each alone (slot
polled), hidden window, imported save, **pipeline tier on** (the default), overlays tmem:mcc:msl-overrides:uintps (lead
06:15), `GTA6RT_METAL_CB_ABORT_MS=2000`, cap 14,848 MiB (no heavy job at any start but e2: 13,312), load 2.9-8.1.
**0 GPU page faults, 0 command-buffer failures; gpuEvent-gta6rt 9 before and 9 after every run.** M = measured, I = inferred.

| run | what | world at | end |
|---|---|---:|---|
| fcpu-a | combining on, verify 1/64 (biased, below), `DISPATCH_TIMING=lib_cs_013` 1 s every 20 s | 71.8 s | watchdog, world+143 s |
| fcpu-b | combining **off** (old code), nothing else | 71.5 s | footprint cap, world+46 s |
| fcpu-c | `LEONIDA_REFRESH_PER_FRAME=2` (ceiling probe) | 79.0 s | **CB abort** (lib_cs_01460..01468 > 1.5 s), world+6 s |
| fcpu-d/-e/-e2/-f | (title pool on by default in the live xmem from 06:33, unnoticed) | 78-84 s | my wired stop, 10.25-10.47 GiB, world+1-6 s |
| fcpu-g | pool off; combining + verify 1/16; `FORCE_SKIP` of 5 irradiance passes; timing | 70.8 s | watchdog, world+82 s |
| fcpu-h | pool off; rect fix (5) on, then opt-in; clean | 72.3 s | watchdog, world+157 s |
| fcpu-i | pool off; final build, all defaults (rect fix default on, direct draws only) | 74.8 s | watchdog, world+60 s |

### 1. The console-heap lock (`shims/d3d12_metal_device.cpp`, `refresh_console_heaps` and its lock)

- **Built: combining, default on (`GTA6RT_METAL_HEAP_COMBINE=0` = one refresh a submission).** Arrivals and refresh
  starts are ordered under a small mutex; a submission that, once it holds `console_heap_mutex`, finds a refresh
  completed that *started after it arrived* returns without its own. That refresh saw every title write made before
  the submission, which is all the per-submission contract promised (the GPU reads the arena copy in place, so a later
  refresh could always overtake). Exact by construction; nothing in `refresh_console_heap` changed.
- **Verify (`GTA6RT_METAL_HEAP_COMBINE_VERIFY=N`, in-title shadow compare):** every Nth arrival snapshots the title's
  heap bytes *before* it arrives; holding the lock afterwards, a slot whose snapshot still equals the title's bytes but
  not the bytes its arena slot was made from is stale. fcpu-g (M): **27 covered samples, 0 stale slots**; control
  (arrivals that refreshed themselves) ~480 samples, 3 stale slots in one window (a slot rewritten back to an older
  value after the scan: the detector's noise floor); 0.09-0.33 M slots compared a window. fcpu-a's 1/64 verify took its
  snapshot under the heap lock, which made every sampled arrival refresh itself (0 covered samples; fixed for fcpu-g).
- **What it buys (M): little.** Covered 5-9% of submissions (fcpu-a world+30: 1,116 submissions in 10 s, 1,031
  refreshes, 85 covered; world+120: 1,330 / 1,253 / 77). With two submitting threads the waiter behind a refresh always
  needs the next one, so combining pays only from three (I: the 5-9% are the moments a third arrived). Frames/s did not move (table in 4).
- **Lock at world+30 (fcpu-a, M):** refresh 5.2 ms inside the lock, wait 5.7 ms a submission (max 179 ms), lock busy
  ~54%; world+60..+120: refresh 3.2-3.4 ms, wait 2.8-3.4 ms, busy ~40%. Inside a refresh at world+30 (heap line):
  pages 0.60, fixing changed slots 3.9-4.9 (220-235 KiB, ~7,000 slots a refresh), retries 0.51 (the T# budget).
  Live V# re-fixes (new counters, fcpu-g/-h): see 4.
- **Options not taken, and why:**
  - *Refresh once per frame / per N ms + a check of 16 entries per bound table* (VIctory's design): our shaders read
    the arena copy bindlessly (`kHeapIndexReach` = 2^21 slots), so no bound-table set bounds what a submission reads;
    and stale descriptors are not harmless here: **fcpu-c (per-frame heaps) ended 6 s into the world on the CB abort**
    (lib_cs_01460..01468 / 02069.. command buffers past 1.5 s). The 16-entry check only works once shaders stop reading
    the arena copy raw (ref-native-victory-macos finding 2/3).
  - *Reader/writer split:* nothing reads under `console_heap_mutex` on the hot path (only the gpu-debug console
    source); the waits are refresh behind refresh, which combining already merges.
  - *Sharding per heap:* one resource heap (38 MiB) does all the work; the sampler heap is imported. Nothing to shard.
  - r9-heapcopy replay tool: not rerun; it checks `refresh_console_heap`'s scan/fix result per refresh, which this
    change does not touch (only which submissions call it). The in-title verify above is the check that applies.
- **Where the frame goes now (M, fcpu-a/-g, tier on):** frame 290-340 ms. GPU any-queue busy 54-56% at world+30
  (168-185 ms a frame, direct queue 152-165), 68-70% from world+60 (196-201 ms a frame, direct 177-182). Heap lock
  40-54% busy. Neither saturates; the GPU share is the larger and grows, so the next frame-rate step is GPU-side
  (direct-queue passes), not the heap.

### 2. The irradiance-cache lists

- **With the tier on nothing in that family is skipped, and the lists stay bounded (M):** fcpu-a, `lib_cs_01368`
  GatherCopy 37 groups a dispatch at world+3 s, 48-74 through world+130 s (0.05 ms); `lib_cs_01396` 490-980;
  `lib_cs_01377` 37-74. fr10-h (tier off, 177 compute passes skipped a frame): 56,307 -> 919,216 in 11 s. So the
  runaway needs a skipped pass, and a warm run makes it run.
- Bisect (fcpu-g, `GTA6RT_METAL_FORCE_SKIP=lib_cs_01331,01333,01335,01352,01365`, the family's 1-thread passes): the
  lists did **not** grow; `lib_cs_01368`'s records became empty (0 groups), so one of these builds its dispatch
  arguments and none is the reset. Which pass resets the counter is still not named (not among those five).
- **Skip report (new, default on, `GTA6RT_METAL_SKIP_REPORT=0` off):** every 10 s, `[rt] d3d12-metal compute skipped by
  pipeline`: each skipped pipeline, direct/indirect, why, and for "compiling" its compile state (queued / compiling /
  no pipeline object). A skipped pass whose pipeline waits in the compile queue is now moved up to demand priority
  (`MetalDevice::precompile`), as the backend's own NotReady path does for draws; replay never reached that path.
- Not done: a log line on the indirect group clamp. The clamp exists on the GPU (`groups_prepass`, port/metal,
  `GTA6RT_METAL_INDIRECT_CS_GROUPS` = 2^20) and is not in my files; a log line needs a readback there.

### 3. "Still compiling" skips with the tier on (M, fcpu-a)

- Compute passes skipped as compiling: 0.2 a frame at world+0..10, then **0 from world+21**; one burst of 60 pipelines
  at world+41..51 (an 83-present window, a scene change: all `queued`), then none. Draws: 12.4 a frame indirect mesh
  "compiling" in world+21..31 (a burst of 30 first uses at world+20), **0 at world+31..41**. pcache at world+30:
  "0 of all first uses not ready yet; compiles running 0, queued 0".
- **What was missing from the tier:** its prebuild list. 173 world pipelines in fcpu-a were "created at use" (161 from
  the archive, 15 compiled, 1 failed): the studio-shot PBR lighting set (`vs_classfied_fullScreen` x
  `PS_STUDIOSHOT_PBR_MATERIAL_*`, ~30), hair-strand G-buffer, blur/text/UI, trail-map grass, cascade-shadow mesh
  variants, `lib_cs_03152/01189/01206/01099`. Fixed with the cache worker's tools: `tier_refresh.sh 300` -> 3,172 world
  pipelines, 199 new (`build/pc7/lists/world-new.specs`), tier manifest 3,210 entries; `prepare_shaders` over a
  catalog with the tmem overlay folded in (`build/pc7/catalog-r11-tmem.tsv` = catalog-r9-mcc + tmem): **199 ok (183
  already in the archive, 16 built), 0 failed** (`build/pc7/prep-world-r11-a.*`). The translator's 7 thread-memory
  pipelines: 7 ok (`prep-world-tmem-a.*`).
- Not compile skips but still lost every frame (M, world+30): indirect mesh split64 "Metal refused the pipeline"
  (threadgroup memory 36,336 B) 20-24 a frame and (29,888 + 31,968 B) 4 a frame, although the mcc overlay was listed
  first (mcc / draw-mesh owner); DXGI format 17 vertex input 1 a frame; conservative raster 0.2-1 a frame.
- Boot with the tier on: every boot of this tree (10 of 10) passed the 32-bit reservation (0xe68e0000 in fcpu-a: close
  to the top). Its own archive started empty; fr10-c..g carried a 4 GB own archive. The pool-default worker holds a
  low range for it now.

### 4. Frames/s and the per-frame breakdown (M; pool off; per frame = per-submission values x submissions / frames)

| | fcpu-b +30 s (combining off) | fcpu-a +30 s | fcpu-h +30 s | fcpu-a +120 s | fcpu-h +120 s |
|---|---:|---:|---:|---:|---:|
| frames/s (5 s windows) | 2.8-3.0 | 3.0-3.2 | 3.0-3.2 | 3.4-3.6 | 3.2-3.6 |
| submissions/s | 103-107 | 108-114 | 111-115 | 128-136 | 126-131 |
| submit CPU, ms a frame (summed over threads): heaps / shadows / textures / replay | 267-306 / 68 / 51-52 / 44-45 | 354-394 / 58-59 / 34-39 / 38-40 | 237-291 / 57-64 / 37-41 / 38-41 | 235-237 / 52-57 / 34-36 / 33 | 196-238 / 52-60 / 33-37 / 31-35 |
| heap refresh inside the lock / lock wait, ms a submission | (not split) | 5.2 / 5.7 | 4.7 / 2.6 | 3.2 / 3.1 | 3.3 / 2.6 |
| of it, live V# re-fixes (all live V#s after a release/invalidation) | | | 0.73 ms a refresh (7 runs / 10 s, ~475k slots each) | | 0.76 ms (14 runs) |
| GPU any queue busy, ms a frame (%) / direct queue | | 168 (54%) / 153 | 181 (56%) / 163 | 198 (69%) / 178 | 192 (65%) / 172 |

- Load 3-8 at the starts, nothing else heavy running in these windows; pool off (`physical pool 8 GiB`), tier on.
  fcpu-a's `heaps` includes its verify snapshots (38 MiB copied under the heap lock every 64th arrival in that build)
  and 1 s dispatch-timing windows; fcpu-h is the clean run. fcpu-b has neither but its old code splits nothing.
  The live V# re-fixes are 0.73-1.19 ms of every refresh on average and every one of the ~100-180 ms "heaps (max)"
  spikes (pool-default worker owns that block: re-fixing only slots under an invalidated range is the next cut).
- fcpu-h ran with the rect fix (5): 0 dropped submissions (fcpu-a: 405), wired max 8.6 GiB (fcpu-a 8.9), frame rate
  unchanged.

### 5. Found on the way (M)

- **~1 held ExecuteCommandLists a frame was dropped whole** with the tier on: `rect cpu_rewrite: vertex buffer is not
  CPU visible` (port/metal) fails the encoder, the list does not close, the whole submission is lost (fcpu-a 405 in 130
  s of world; fcpu-b 119). `metal_replay.cpp` `bind_draw_state` now skips such a direct rect-list draw alone (fate
  "vertex buffer unknown"), default on, `GTA6RT_METAL_RECT_SKIP_ALONE=0` = old behaviour. fcpu-h and fcpu-i (final
  build): 0 dropped, 1 direct rect-list draw a frame skipped alone, 0 GPU errors, wired max 8.6 / 7.4 GiB. port/metal owner: the CPU expansion should read a GPU-only vertex buffer some other way, and a per-draw
  failure should not fail the encoder.
- My 06:44 note blamed the rect fix for the 10.25-10.47 GiB wired stops of fcpu-d/-e/-e2/-f; wrong (07:01
  correction): the title pool had become the default in the shared xmem; with `GTA6RT_METAL_TITLE_POOL=0` wired peaks
  at 8.4-8.6 GiB.
- Presented world frame from fcpu-a (pool off: no G-buffer geometry, so not a milestone of mine; the lit-frame worker
  holds the three milestone PNGs): `/Users/a/Desktop/Projects/GTA-VI/port/run/present/fcpu-a/present_002101.png`,
  world+~90 s: sky, clouds, sea and a flat coast under the HUD (money, minimap, a phone message), washed out to white
  like lit-c's frame; no buildings (the pool is off).

### Switches added

`GTA6RT_METAL_HEAP_COMBINE` (default on), `GTA6RT_METAL_HEAP_COMBINE_VERIFY=N` (off), `GTA6RT_METAL_SKIP_REPORT`
(default on), `GTA6RT_METAL_FORCE_SKIP=name,...` (debug, off), `GTA6RT_METAL_RECT_SKIP_ALONE` (default on). Log lines
every 10 s: `console heap refreshes (combined)`, `console heap live V# re-fixes`, `compute skipped by pipeline`.

### Next (ranked)

1. GPU: the direct queue is 153-182 ms of a 290-330 ms frame and grows; time the world's direct-queue passes
   (`GTA6RT_METAL_DISPATCH_TIMING=all` for compute; render passes have no timer yet).
2. Heap: re-fix only the live V#s an invalidated / released range reaches (0.7-1.2 ms a refresh plus the spikes); then
   the changed-slot fixes (~7,000 slots, 2-4 ms a refresh) are what is left; the 16-entry design needs shaders that
   stop reading the arena copy raw.
3. Name the irradiance counter reset (not the five 1-thread passes); with the tier on it runs, so this is only for
   cold starts.
4. split64 mesh rows still refused for threadgroup memory (20-24 draws a frame) with the mcc overlay first.


## Round 11, rt-alias worker (2026-11-27 06:07-07:15): slot-133 render targets share placement heaps — render targets 2,073 -> 1,007 MiB, machine wired at world entry -1.3 GiB; on by default

Tree `build/rtal-x86_64` (`-build1..7.log`), launcher `build/rtal-x86_64/run_world.sh` (s7_world_run.sh with this tree,
the tmem probe lock, a memory report every 10 s), wired watch `build/rtal-x86_64/wired_guard.sh` (kills the run past
10 GiB machine wired). Every run alone (slot + probe lock), hidden window, imported save, `GTA6RT_METAL_CB_ABORT_MS=2000`,
`GTA6RT_METAL_ARCHIVE_TIERS=off`, tmem+mcc+overrides+uint overlay (lead 06:15), cap 14,848 for world runs (no
prepare_shaders/build running at their start; load 5-8). **0 GPU errors, 0 failed command buffers, gpuEvent-gta6rt 9
before / 9 after every run (7 runs).** M = printed by a log or a tool, I = inferred.

### What is built (`src/metal_rt_alias.h`, device shim, replay)

- **Lifetimes from the title's own placement (the Vulkan path's patch 10, ported):** at registration
  (`register_title_texture` -> `Resource::note_title_pages`) a slot-133 GPU-written 2D target's title bytes are resolved
  to VM-object runs (`mach_vm_page_info` per 64 KiB). Two targets whose runs overlap by >= 64 KiB (or half the smaller)
  alias: the console cannot hold both, so the title never has both live. Candidates: slot-133, GPU-written (RT, depth
  or UAV-with-RT usage), 2D/2D array, not CPU-visible; colour, **depth (Metal places depth in heaps, unlike MoltenVK)**
  and MSAA alike.
- The Metal texture waits for its first use (replay `ensure`, a UAV view, or `image()`), as deferred title textures do
  (SRVs written meanwhile are bound then). The first use of a group lays out an extent from every registered unplaced
  target connected to it by aliasing, largest first, each at the lowest offset where it overlaps only its aliases;
  one `MTLHeapTypePlacement` heap (untracked; the backend's barriers already order placed resources) per extent, charged
  to "render targets". Later targets fit into their aliases' extent or seed their own. Freed members stay in the layout
  as tombstones, so a non-alias never lands on memory whose GPU work may still run. A target Metal refuses to place
  gets a private texture zeroed as before.
- **Load actions / hand-over check (replay, `note_alias_uses`):** every command's use of an aliased target is noted
  before it is recorded: draws, clears, copies, resolves (direct uses); transitions/enhanced barriers (to a read state
  = read, to UAV = write, **out of a write state = it was written**, which covers compute writes through descriptors;
  discard / from-undefined / aliasing barrier = fresh). A target is "not its own" after an overlapping alias wrote
  since its last write. A non-fresh use then (a draw that loads, a UAV pass, a read) is counted; at a target's first
  use it is zeroed (a committed target starts zeroed), otherwise nothing is copied (console semantics).
  `GTA6RT_METAL_RT_ALIAS_POISON=1` clears such a target to **magenta** (depth 0.5, stencil 0x5a) before the use instead,
  so a wrong assumption shows in the frame.
- Report line with every memory report: `[rt] gpu-memory aliases t=..`: candidates, used, depth/MSAA, never-read
  depth/MSAA, UNORM/sRGB twins, alias groups and the largest-first layout now, placed / extents / title bytes, uses,
  reads and loads after a hand-over or before any write, fresh hand-overs, poisoned, zeroed.
- Switches: **on by default** since 07:09; `GTA6RT_METAL_RT_ALIAS=0` private textures as before, `=census` only
  registers and counts; `GTA6RT_METAL_RT_ALIAS_POISON=1`.

### Runs (M)

| run | binary | settings | world at | end | render targets (ledger) | footprint world entry -> +151 s | machine wired world entry / max (gov.) |
|---|---|---|---:|---|---:|---|---|
| rtal-census | build3 | census, menus/movie 65 s | - | watchdog | 2,072 | - | - |
| rtal-on-a | build4 | on, menus/movie | - | watchdog | 1,005 | - | - |
| rtal-poison-a | build5 | on + poison, menus/movie | - | watchdog | - | - | - |
| rtal-world-a | build5 | on, first hand-over model | +86 s | watchdog, 65 s of world | 1,030 | 9,445 -> 11,860 | 5,140 / 6,130 (pool path older) |
| **rtal-world-off** | build6 | `GTA6RT_METAL_RT_ALIAS` unset (then off), pool on (new default) | +92 s | watchdog, 59 s of world | **2,073** | **10,466 -> 13,562** | **8,250 / 9,579** |
| **rtal-world-pz** | build6 (same) | on + poison | +91 s | watchdog, 60 s of world | **1,007** | **9,468 -> 12,730** | **6,932 / 8,817** (watch: 9,547 peak) |
| rtal-world-def | build7 | nothing set (default on) | +82 s | watchdog, 69 s of world | 1,017 | 9,574 -> 12,368 | 6,900 / 8,746 (watch: 9,001 peak) |

- **A/B, same binary (off vs pz):** render targets -1,066 MiB; footprint at world entry -998 MiB, at +151 s -832 MiB;
  graphics ledger at world entry 3,585 -> 2,498 MiB; machine wired at world entry -1,318 MiB. Layout (pz, t=125 s): 999
  targets (1,279 MiB of textures) in 389 extents of 781 MiB over 626 MiB of title memory; 0 refused, 0 "no room", 0
  pages moved. The design note's estimate was -0.8 to -1.1 GiB.
- No world run ended on the cap (13.6 GiB at most); all three world runs ran to the 150 s watchdog.
- rtal-world-a's binary predates the pool-default worker's changes (pool path imported 770 MiB vs 2,588), so it is not
  in the A/B.

### Proof that the title's use allows it (M)

- **Menus (frame hashes):** every present dump up to the movie (frames 1-1051, every 30th) is byte-identical between
  census (= today's textures) and alias on, and between census and **alias on + poison**. Movie frames differ by index
  in every run (timing), viewed: intact. Only 3 slot-133 targets are used before the world, so this is a weak test.
- **World (poison run, 60 s of world):** reads after a hand-over 0, loads after a hand-over 0, reads before any write 0;
  **3,518 hand-overs, every one started with a clear, discard or barrier from undefined**; 34 first uses were draws
  without a clear (poisoned there; zeroed in the default mode: 38 in rtal-world-def). No flat magenta in any G-buffer
  dump (`run/present/rtal-world-pz-scene/scene_*`; gbuffer1/gbuffer3's own purple normals/material colours are in the
  off run too) or presented frame. Default-mode run (def): the same 0/0/0, 4,460 fresh hand-overs.
- Not proven: shader reads through descriptors with no barrier at all are not seen; scene colour was 0 in these dumps
  (lighting pipelines still compiling, lit-frame worker's 06:40 note), so lit output under aliasing has not been looked
  at; the first model (rtal-world-a) counted 564 "loads after hand-over" because it took a transition *to* RT as a
  write and missed writes before a barrier; corrected before pz/def.

### The other two items

- **Memoryless depth/MSAA:** not built. Measured candidates: MSAA 1 target (0 MiB); depth/MSAA targets never read in
  60-69 s of world: 9-10 targets, 41-43 MiB (332 depth targets, 226 MiB in all). Memoryless needs proof at creation
  that a target never outlives a pass, which nothing gives us; at most ~0.04 GiB, and the depth targets now share heaps.
- **UNORM/sRGB pairs:** none on the Metal path: 0 slot-133 targets share pages with another of the same shape and a
  different format (census "twins 0" in every run), and other UNORM/sRGB views are Metal texture views
  (`pixel_format_view`), not second textures. Nothing to drop.

### Files

`d3d12_metal/src/metal_rt_alias.h` (new); `src/metal_context.h` (`MetalBinding::alias_watch`); `src/metal_replay.cpp`
(`Replay::run` one call, `note_alias_uses`, `clear_alias_target`); `shims/d3d12_metal_device.cpp` (`Resource::create_texture`
candidate branch, `image()`, `back_title_texture` (no in-place for candidates), `make_deferred_texture` ->
`make_alias_texture`, `note_title_pages`, destructor, members; `DeviceState::rt_alias`; one call in
`register_title_texture`; `report_rt_alias`). port/metal unchanged (its `create_heap` / `create_placed_texture` were
enough). Logs `run/logs/rtal-*.log` (+ `.wired`, `.wired.trace`), dumps `run/present/rtal-*`.

### Next

1. Look at a lit frame with aliasing on vs `GTA6RT_METAL_RT_ALIAS=0` once the lighting pipelines are warm (poison on).
2. Tighter packing: 781 MiB of extents for 626 MiB of title memory (Metal's textures are larger than the console's);
   the transient pool is the bulk.
3. The ~320 MiB of render targets that are committed (not slot-133) cannot alias by title pages; a frame-graph view
   of D3D12 placed heaps would be needed.

## Round 11, lit-frame worker (2026-11-27 06:07-06:45): albedo, a lit scene colour and the presented world frame; what stops each

Tree `build/lit-x86_64` (`-build1..12.log`; its `build.ninja` compiles `metal_replay.cpp` with `-include mach/mach.h`
because the live `metal_rt_alias.h` does not compile there, see the requests file 06:21), private launcher
`build/lit-x86_64/run_world.sh` (tpool's, plus the tmem probe lock, the cap picked by the lead's rule, and a machine-wired
watch that ends the run). Runs `lit-a..e`, each alone (slot polled), hidden window, imported save (the title loads it at
boot), `GTA6RT_METAL_TITLE_POOL=1 GTA6RT_VM_BASE_HINT=0x2000000000 GTA6RT_METAL_TITLE_POOL_CHUNK_MB=4`, the tmem + mcc +
msl-overrides + uint overlays, 14,848 MiB cap (no heavy job at any start), `GTA6RT_METAL_CB_ABORT_MS=2000`, load 3.6-6.6.
**0 command-buffer failures, 0 GPU page faults; gpuEvent-gta6rt in /Library/Logs/DiagnosticReports 9 before and 9 after
every run.** M = measured (log, dump), I = inferred.

**Every run was ended by my machine-wired watch, not by the footprint cap (M):** lit-a 10,350 MiB at world+~17 s, lit-b
10,261 at world+~9 s (pipeline prebuild on), lit-c 10,423 at world+~24 s, lit-d **11,240** at world+~10 s (the 1 s poll
saw it 1 GiB late; the watch now polls 4 times a second and stops at 9,728), lit-e 9,932 before the world
(13,312 cap, a build was running; fill budgets 320/128 MiB, pool cap 1,280 MiB: no dump). In each, wired is 7.9-9.0 GiB at world
entry and then the graphics ledger steps +1.4-1.9 GiB within a few seconds (lit-c: 3,781 -> 5,694 MiB, wired 7,942 ->
9,974), with the tmem overlay loaded (8 rows replace the store's). That step, plus the pool chunks (1.0-2.3 GiB wired),
is what limits every world run to its first ~15 s now.

### Milestone images (one per milestone, the first reached)

1. **G-buffer with albedo:** `/Users/a/Desktop/Projects/GTA-VI/port/run/present/lit-c-gbuffer-albedo.png` (lit-c,
   upscaled frame 6, target 0 `f29` R8G8B8A8_SRGB 1280x720): **18.2% of texels non-zero** (tpool-h: 11.1%). Textured:
   palms, bushes, the wooden dock, one wall's siding, a far apartment block, sand. **Wrong:** house walls, roofs,
   ground and water are still black (their textures are streamed in part and get no Metal texture within the budget,
   below); the sand has magenta blotches (one of the textures made from a title T#, layout not verified).
2. **Lit scene colour:** `/Users/a/Desktop/Projects/GTA-VI/port/run/present/lit-a-first-lit-scene-colour.png` (lit-a,
   upscaled frame 10, `f10` 1280x720): 91.8% non-zero: sky and clouds, the stilt houses, poles, decks, with cast
   shadows. **Wrong:** grossly over-exposed (channel means 500-1,000, max 65,300 = the fp16 limit; the probe's own tone
   map saturates), surfaces lit as near-white since most albedo is missing, a black rectangle top left (a tile block
   with no light). Same in lit-c frames 4-12 (94.8-95.9% non-zero, means 170-600).
3. **Presented window frame of the world:** `/Users/a/Desktop/Projects/GTA-VI/port/run/present/lit-c-first-world-present.png`
   (lit-c present 1845, 1920x1080, after MetalFX, the title's post chain and the UI): the same lot through the whole
   chain, **washed out to white/pale blue** (the over-exposure above survives tone mapping); 100% of pixels non-black.

### 1. Why the albedo is black (M, `GTA6RT_METAL_TEXTURE_CENSUS=1`, lit-b/c/d in the world)

- **The bulk is slot-133 textures streamed in part:** ~1,900-2,000 slot-133 textures, **3.8-4.0 GiB of console layout**,
  layout given by a slot-70 view, named by T#s, never filled: they "wait for readable memory" because only the first
  64-128 KiB of their range is mapped. Examples: 0x785fa10000 (1,441,792 B) mapped to +0x10000; 0x7800010000
  (5,636,096 B) to +0x20000; the rest is reserved (VirtualQuery `MEM_RESERVE`, and the title pool backs none of it).
  addrlib lays the mip tail and the small mips out first (offset 0) and mip 0 last, so the title has mapped the pages
  of the levels it has streamed (partial residency; I: the T#'s MIN_LOD keeps sampling on them, and the layer's views
  already honour MIN_LOD). This is not a backing problem: **the pool route found 0 textures to unlock** (VirtualQuery
  and `title_pool_view` agree everywhere; the GPU-only window is mapped read/write for the layer already).
- A few (4-8 a run) 8 MiB textures miss only their last page: our layout's span is 0x7F8000, the title mapped 127
  pages (I: our mip-tail size differs from the console's by 32 KiB).
- **T#s that name no texture in the title windows** (0x78-0x7a): 64,000-70,000 a run; of 4,096 sampled, ~2,800 name pool
  memory no texture was created or viewed over (textures the title knows only by T#s it writes itself) and ~1,300
  point inside a registered texture's range.
- Images made for slot-70 views: ~17,000 filled (~470 MiB) — these are what was textured before.
- Not the cause: null descriptors from slot 70 (tpool-h: 20,129 "no address" views of 94,866: the title passes address 0) are a minority;
  sampler and swizzle were not found at fault (not checked per draw: the material T#s come from a bindless table
  indexed by a material constant, `s_load_dwordx8 s[44:51], s[18:19], s41` in `PS_DeferredTextured_c5fd36bb`).

### 2. Changes (all opt-in; off = the previous paths) — `shims/d3d12_metal_device.cpp` registry, `src/metal_context.*`, `src/metal_present.cpp`

- `GTA6RT_METAL_TEXTURE_CENSUS=1`: every 10 s, the registry by state / layout source / 4 GiB region / T# views / why a
  waiting texture waits (VirtualQuery and pool at its first byte, a fresh probe, the first unreadable offset for 6),
  the sampled T# addresses with no texture, and the counters below (`log_texture_census`).
- `GTA6RT_METAL_TEXTURE_POOL_READABLE=1` (`title_texture_readable`): a texture the pool backs entirely is readable; one
  whose last page (< 64 KiB) is unmapped is read with zeros there; **one streamed in part fills the levels wholly in its
  mapped prefix** (`TitleTexture::resident_mip`, `short_tail`; the fill skips the levels above). Measured: 876 textures
  streamed in part made fillable in lit-d (0 through the pool route, 4-8 with a missing last page).
- `GTA6RT_METAL_FILL_TSHARP_TEXTURES=1` (+`_MB`): a deferred texture a title-written T# names is queued for a fill
  (`bind_title_image` -> `take_tsharp_wanted`): 167-249 queued until the budget, 3,400-4,000 more over it.
- `GTA6RT_METAL_TSHARP_TEXTURES=1` (+`_MB`, default 512): a T# in the title windows over pool memory with no texture
  gets a texture made from the T# (format, extent, mips, layers, word-3 swizzle mode) and filled (`make_orphan_textures`).
  1,362-1,454 made (83-85 MiB); 10,000+ refused as overlapping a registered texture. **All decoded swizzle mode 0
  (linear): not verified against the console; possibly the sand's blotches.**
- `GTA6RT_METAL_TITLE_BACK_MB`: cap on Metal textures made for deferred title textures at their fill (over: the texture
  stays deferred and is not visited again). lit-c 384 MiB used, 343 textures (508 MiB) refused; lit-d 512 used, 914
  (1,743 MiB) refused.
- Scene probe: `GTA6RT_METAL_SCENE_PASSES` (pass-log length, default 600 as before); the G-buffer is now the render-size
  target set with the most draws in the frame (it used to be the last set with the most targets, which picked a
  full-screen pass or an alpha prepass in lit-a); a 1x1 target drawn by a `*Exposure*` pass is read back with the dump
  (`exposure1x1`; lit-e, the only run with it, ended before the world). `GTA6RT_METAL_WORLD_PRESENT_DUMP=dir,every,count`: presented frames once the
  probe has seen an upscaled frame, with the share of non-black pixels.

### 3. One frame's passes, G-buffer to present (M, lit-c upscaled frames 2/6/12, 3,000-pass log)

- Order: G-buffer (~5,000 mesh draws into `f29|f28|f28|f28|f34|f28` + D32S8) -> cascade shadows (640x2560) -> ... ->
  **TiledLighting 180 rect-list draws (`TL_PS_PBR_*`) + SSSResolve 12** into the `f10` colour and an `f42` target ->
  AlbedoFreeLuminance downsample -> `PS_CompositeIndirectDrawBlend` (TileGridDefaultPass) -> luminance downsamples ->
  **`PS_ResetExposure` into a 1x1 `f2` target every frame** (`PS_CalculateExposure` ran once, frame 2) -> bilateral grid
  -> MetalFX (input = the `f10` colour, ffxDispatch log) -> `PS_CompositeNMB` into `f24` 1920x1080 -> CopyDithering.
- **Why the earlier dumps had scene colour 0:** the lighting pipelines were still compiling in the first world frames
  with `GTA6RT_METAL_PCACHE=0` (lit-c frame 2: TiledLighting 180 draws, 0 recorded; frame 6 on: 180 of 180; composite
  recorded from frame 6). With the prebuild on (lit-b) the run hit the wired watch at world entry, so not measured.
- Not resolved: the over-exposure (colour means in the hundreds: the exposure ResetExposure writes, or the light units;
  the new `exposure1x1` readback is the next look), `gbuffer5` all 0 with alpha 1, the red-dominant noisy normal target
  (`f28`, mean r 0.51 g 0.26 b 0.25: encoding not decoded), the black tile block top left.

### Next (ranked)

1. Memory before anything else: the +1.4-1.9 GiB graphics-ledger step at world+5-10 s (still there with the tmem
   overlay) and the pool chunks put every run at 10 GiB wired by world+15 s.
2. Streamed-in-part textures at their resident size: a Metal texture of the resident levels only (views offset by the
   dropped levels, re-made when the title maps more), or Metal sparse textures mapped per level; full-size textures for
   3.8 GiB of layout do not fit.
3. Verify the swizzle mode of title-built T#s (word 3 bits 20-24 read 0 for every one) before trusting
   `GTA6RT_METAL_TSHARP_TEXTURES`.
4. Exposure: read the 1x1 target (`exposure1x1`) and the ResetExposure constants.

## Round 10, title-pool worker (2026-11-27 05:00-06:10): zero-copy GPU backing of title memory (`GTA6RT_METAL_TITLE_POOL=1`) — the G-buffer and depth get the world's geometry with no copy and no 64 MiB budget

Tree `build/tpool-x86_64` (`-build1..10.log`), private launcher `build/tpool-x86_64/run_world.sh` (s7_world_run.sh with
this tree's run dir, memory report every 5 s), imported manual save (the title loads it at boot), runs `tpool-a..h`,
`tpool-off`, `tpool-vk`, each alone (slot polled; 4 collisions backed off), hidden window, 13,312 MiB cap (other heavy
jobs were running: prepare_shaders, builds; load 3.7-28 at run ends), `GTA6RT_METAL_CB_ABORT_MS=2000`, stop at the first
GPU error. The translator's loop-cap overlay had not landed. **0 GPU errors, 0 command-buffer aborts, gpuEvent-gta6rt
in /Library/Logs/DiagnosticReports 9 before and 9 after every run.** M = measured (log, tool), I = inferred.

### What is built (all behind `GTA6RT_METAL_TITLE_POOL=1`; off = today's paths)

- **xmem (`shims_core/src/xmem.cpp/.h`).** The pool is one Mach memory entry (`MAP_MEM_NAMED_CREATE`) mapped once over
  its reservation (64 KiB aligned, so 16 KiB aligned under Rosetta), never moved, protected or unmapped; made at the
  first XMem/VirtualAlloc call. Title pages are views of it: physical pages in windows as before (`gta6rt_vm_alias`),
  and now **committed XMemVirtualAlloc and MEM_GRAPHICS VirtualAlloc memory as runs of pool slots**
  (`GTA6RT_XMEM_POOL_VIRTUAL_GIB`, 8 GiB of slots above the 8 GiB of physical ones; `GTA6RT_XMEM_POOL_VIRTUALALLOC=all`
  takes every VirtualAlloc). Decommit inside a run makes the view no-access and keeps it (zeroed on recommit); freed
  slots are zeroed on reuse instead of decommitted (a decommit maps fresh memory over the host range). Table for the GPU
  side: `title_pool_view(va) -> (host byte, contiguous run)`, `title_pool_generation()` (lock-free),
  `title_pool_removals(since)` (title ranges unmapped/remapped/decommitted/released), `title_pool_range()`.
- **port/metal.** `HostBufferDesc::resident` (false: never in the device residency set) and
  `metal::set_encoder_resource(device, buffer, on)`: every compute and render encoder declares the named buffers with
  `useResources` when it opens (`Encoder::declare_named`).
- **Device shim.** `title_pool_window` (called first in `resolve_title_memory`): the pool bytes behind a title address
  are reached through a bytesNoCopy buffer over 8 MiB chunks of the pool's host range (`GTA6RT_METAL_TITLE_POOL_CHUNK_MB`;
  a range crossing a chunk end gets a span buffer over 2-8 chunks), made lazily, named per encoder, capped by
  `GTA6RT_METAL_TITLE_POOL_MB` (3,072 by default); an alias title VA -> shader VA is added for the contiguous stretch.
  A chunk's shader-VA range must hold no writable title memory (`claim_shader_range`; its holes are reserved no-access,
  the arena's own CPU mapping and read-only mappings are allowed); otherwise it is set aside and a spacer moves the
  next one, as `external_window` does. `title_pool_sync` (top of `refresh_console_heap`) forgets aliases over removed
  ranges (the alias invalidation makes the refresh re-fix its live V#s), and when the pool table changed the heap's
  **empty slots go on the retry list**. A descriptor with no backing keeps today's null view: base 0 = the arena's
  zeroed first 64 KiB (`kZeroPage`), 0 records, so reads give 0 and writes are dropped.

### Offline proof (M; `metal_test_titlepool`, x86_64 under Rosetta, own kernels; `shims_core_test`)

- Process page 4 KiB, kernel page 16 KiB, maxBufferLength 13,639 MiB. Memory entry of 2 GiB, bytesNoCopy over 256 MiB
  chunks of it: GPU writes through the chunk seen in two title views (0 of 49,152 words wrong); a 20 KiB view at a 4 KiB
  (not 16 KiB) offset: 0 wrong; 2,000 remaps of a title page: 0 wrong; **72,253 remap/protect rounds of title views
  during a 250 ms kernel on the same chunk: 0 wrong, no SIGBUS**; GPU writes while the view is no-access then recommit:
  0 wrong; a 1 MiB `vm_read_overwrite` into a view lands in the pool and the range still wraps; a chunk wrapped late
  is used together with chunk 0 in one kernel. PASS.
- **Wiring:** naming a 256 MiB chunk with 1 MiB touched adds 256 MiB of **machine wired** memory at first use (5 ms),
  footprint unchanged (the footprint cap cannot see it), and it stays wired until the buffer is released (per encoder
  or in the device residency set alike). useResources cost: 0.19 us per buffer per encoder (1,024 buffers: +0.19 ms a
  list), nothing measurable at 64.
- shims_core_test: 99/99 cases default, 99/99 with the switch; new `test_xmem_pool.cpp` (5 cases: view coherence,
  decommit/recommit zeroing, release, runs reused zeroed, 64 runs, page-by-page commits, MEM_GRAPHICS only, physical
  windows follow map/remap/unmap with removal records). metal_test_hostmem/device/memory/hazards: PASS.

### In the title (M)

| run | settings | story load (first load byte -> world) | footprint / machine wired at world entry (MiB) | zero-size views (story load / end) | pool | end |
|---|---|---:|---|---|---|---|
| tpool-a | pool, early allocs not yet pooled (bug), 32 MiB chunks | 16.2 s | 11,756 / **9,297** | 73,734 / 144,574 | cap 3,072 reached before world | cap (exit 4) |
| tpool-b | + early allocs pooled (2.9 GiB of runs) | 39.5 s | 11,868 / 8,423 | 1,643 / 438,745 | 123 chunks refused (shader VA collisions) | cap |
| tpool-c..e | + `GTA6RT_VM_BASE_HINT=0x2000000000`, 8 MiB chunks | 19.8 / 24.3 / 36.5 s | 11,864-12,136 / 6,314-6,391 | 22,868-80,863 / 154-208k | refused: the arena's own mapping, then read-only file views | cap |
| tpool-g | + arena and read-only mappings allowed | 30.4 s (load 28 at the end) | 12,059 / 9,223 | 1,437 / 106,381 | 8,192+ ranges, 1,558 MiB named, 2,792 MiB wired | cap |
| **tpool-h** | + span buffers, `GTA6RT_METAL_PCACHE=0`, scene dump | **24.3 s** | 10,695 / **9,069** | **2,752** / 115,082 | 8,192+ ranges, 1,524 MiB named, 2,704 MiB wired, 806 scattered, 814 invalidated, spacers 1,745 MiB | cap at world+~9 s |
| **tpool-off** | same binary and settings, switch off | **17.7 s** | 10,369 / **6,034** | **99,212** / 275,747 | - | cap at world+~11 s |
| tpool-vk | `GTA6RT_GPU=vulkan`, default path | 40.0 s (MoltenVK) | 11,171 / 6,881 | - | plain pool (`xmem: physical pool 8 GiB`) | watchdog 200 s, 100 s in the world, no error |

- **Geometry (M, A/B tpool-h vs tpool-off, scene dump of the 2nd upscaled world frame):** pool on: gbuffer1 63.4% of
  texels non-zero, gbuffer3 60.3%, depth 63.4% (0..0.0063, reversed Z), gbuffer0 (albedo) 11.1%, gbuffer2 6.7%;
  pool off: **every G-buffer target and depth 0%** in all three dumps (gbuffer4 the constant 0.502). Kept:
  `/Users/a/Desktop/Projects/GTA-VI/port/run/present/tpool-h-scene/scene_00002_gbuffer1.png` (stilt houses, palms, a
  dock: the same lot the meshlet-header worker's alias stopgap showed at 05:50, here through the pool). **What is wrong
  in it:** albedo is black on most surfaces (only vegetation, a dock, a far building are textured), scene colour is 0%
  in that frame (lighting/composite give nothing yet), gbuffer5 is 0, and the run ended on the footprint cap ~9 s into
  the world, so no later frame was seen.
- **Previously empty descriptors (M):** zero-size views at the story-load report 99,212 -> 2,752 (-97%); at the end
  (world) 275,747 -> 115,082. What is left is mostly title data read as V#s (bases like 0x2800000025, sizes of 50 GB:
  "no readable page"), 806 ranges on scattered pool pages (window pages whose slots are not consecutive: no single
  buffer can hold them; those still take the 64 MiB copy budget), and addresses outside the pool (0x54b054b0 3.7 MB,
  0x1000119a1: allocators not identified).
- **Story load:** 24.3 s with the pool against 17.7 s without in the A/B pair (+6.6 s); the other pool runs 16-40 s,
  the round-9 runs without it 15.7-20.3 s. So the pool costs some seconds here, nowhere near the 34 s of per-submission
  copies (I: candidates are the full live-V# re-fix after each alias invalidation, 814 in tpool-h, and a mach_vm_remap
  per commit into a virtual run; not separated).
- **Wired memory (M):** +3.0 GiB machine wired at world entry (9,069 vs 6,034 MiB), all of it the named chunks (2.7 GiB
  for 1.5 GiB of pool actually named in 1 MiB blocks), invisible to the footprint cap and kept until the device goes.
  The cap (`GTA6RT_METAL_TITLE_POOL_MB`, 3,072) bounds it; at 03:33 the panic came at 10.7 GiB wired.
- **Shader-VA spacers (M):** 1.7-2.3 GiB of private spacers were needed to keep chunk shader VAs off title memory; they
  count in Metal's allocated size (governor, DXGI budget).

### Not done / next (ranked)

1. The world phase is still ~10 s under 13,312 MiB (pool on or off); a 14,848 run on a quiet machine with the pool is
   the next look at later frames and the frame rate.
2. Wired cost: release chunks that no live V# names (needs the V# -> chunk reference counts), or make chunks 4 MiB;
   scattered window pages need per-page translation (VIctory's page table in the shader), not a buffer.
3. Story-load cost: re-fix only the V#s whose range an invalidated alias covered (heap-copy worker's note) instead of
   all live V#s.
4. Run the pool with the translator's loop cap once it lands (compute passes now read real title bytes).

Switches added: `GTA6RT_METAL_TITLE_POOL=1` (xmem + device), `GTA6RT_METAL_TITLE_POOL_MB`, `GTA6RT_METAL_TITLE_POOL_CHUNK_MB`,
`GTA6RT_XMEM_POOL_VIRTUAL_GIB`, `GTA6RT_XMEM_POOL_VIRTUALALLOC=all`; recommended with `GTA6RT_VM_BASE_HINT=0x2000000000`
(keeps title memory away from the shader VAs Metal hands out; without it chunks collided, tpool-b).

## Round 10, frame-rate worker (2026-11-27 05:00-06:10): the releaser no longer blocks, the hash table is written in place, and the heavy GPU term is now the irradiance cache's runaway list

Tree `build/gates-x86_64` (`-build5..7.log`). Launcher `build/gates-x86_64/run_fr.sh`: it waits for the slot, then picks
the footprint cap by the lead's rule (14,848 MiB with no other heavy job running, else 13,312). Runs fr10-a..h, each alone,
hidden window, imported saves. At boot the title read the imported saves (`Player`, `SARC0000`, `SARC0015`, M). The
story load came at +56 s and the world at +73-75 s, the same timing as before. Whether the world was the save's place
or the prologue was not checked. Load 3-15 at the starts.
**0 GPU timeouts, 0 page faults, 0 failed command buffers. gpuEvent-gta6rt: 9 before, 9 after every run.**
M = printed by a log or a tool. I = inferred.

| run | settings (besides defaults) | cap | world | end |
|---|---|---:|---:|---|
| fr10-a | tier off, dispatch timing (60 s) | 13,312 (prepare_shaders running) | +73 s | cap, world+10 s |
| fr10-b | same | 14,848 | +75 s | cap, world+50 s |
| fr10-c..g | tier **on** (default), with and without the worker, `LEONIDA_ARENA_MB=256` | | - | boot stop at 16 s: 32-bit reservation refused (below) |
| fr10-h | tier off, `GTA6RT_METAL_DISPATCH_TIMING=all` 2 s every 12 s, `CB_ABORT_MS=2500` | 14,848 | +73 s | cap, world+47 s |

### 1. Held-submission replay on a per-queue worker; in-place hash table (M)

- **Worker** (`metal_context.h/.cpp`, `MetalQueue`, `GTA6RT_METAL_RELEASE_WORKER=0` turns it off):
  - `release_ready` now only moves the ready operations to the queue's `ready_` list and wakes its worker. The worker
    replays and submits them in order.
  - Two locks. `exec_mutex_` covers replay and submission. `state_mutex_` covers the queues and the waits, and is never
    held while replaying.
  - An operation handed to the worker still counts as held (QueueHolds' count, `oldest_held_seq`) until it has run, so
    `after_held` (the presenter) keeps its order. After each operation the worker pumps, because a Signal it submitted
    can release other queues.
  - Waits travel with their operation (`Waits`) instead of being swapped through members. Waits an operation never
    submitted carry over to the next one, as before.
  - Worker stacks: 8 MiB, mapped above 4 GiB.
- **Gates (list values)**, world windows, gates-b against fr10-b/-h:

| | gates-b +30 s | gates-b +120 / +240 s | fr10-b +10 / +20 / +30 s | fr10-h +30 / +45 s |
|---|---:|---:|---:|---:|
| releaser time per gate Signal | 32.8 ms | 36.4 / 23.1 ms | 0.02 ms | 0.02 ms |
| longest Signal | 2,016 ms | 2,016 / 2,391 ms | 0.3-1.0 ms | 0.2-0.3 ms |
| gates timed out | 90% | 82% / 39% | 23 / 30 / 37% | 39 / 66% |

  The "stalls" counter (5-21) and its log line `inside one gate Signal ... for 18446744073709551615 ms` are an
  unsigned underflow in `watch_releaser` (`d3d12_vulkan/shims/d3d12_xs_list_values.h`, read-only for me). They are not
  stalls: the `since` stamp is set a tick after the pump reads the clock.
  Timeouts are still 23-66%. So most remaining timeouts wait for memory that no write lands in time, not for the
  releaser (I).
- **In-place hash table** (`metal_bindless.h/.cpp`, default on, `GTA6RT_METAL_TABLE_IN_PLACE=0` turns it off): this is
  the memory worker's patch, changed after a stress test.
  - **The patch as written gives wrong indices.** Offline (`build/gates-x86_64/tests/test_bindless_inplace.mm`): a GPU
    reader probes exactly as the translated shaders do (8 key words compared, index loaded, no pad check). Over 6 s it
    read **3,277,138 lookups with another key's index** in patch mode and 2,458,228 in naive mode. Cause: a slot that
    goes from one key to another in a flush (a relocation, or an erase and an insert into the same slot). A reader can
    pair the old key with the new index.
  - **The guarded version** (`bindless::write_in_place`) writes in place only these cases: same key with a new index,
    a key into an empty slot, and a key removed. Two cases go to the old generation path: a key replaced by another,
    and a refill of a slot that was emptied in place while GPU work from then is still in flight. Result: **0 torn in
    3.77e11 lookups (60 s)**, and 0 in 9.75e10 with the refill rule off (that rule is kept as a margin).
  - Measured in the game: 1,004-1,178 flushes in place against 46-58 to a generation, 0 flush waits (gates-b: 128
    waits, 38 s in all). 2 live generations (40 MiB) in fr10-h, after a fix that frees spare free generations.
    Report line every 10 s: `descriptor hash table: N flushes in place ...`.

### 2. Per-dispatch GPU time (M, fr10-h, world+15/+25/+36 s; `GTA6RT_METAL_DISPATCH_TIMING`, new, port/metal)

Each timed dispatch runs in its own encoder, with timestamps at the encoder's start and end. It waits for the encoders
before it, so these are serialized times.

- **compute_graph_ops** (the gates-b family `lib_cs_010xx`: a neural-network graph of MatMul, VecMatMul, Reduce,
  Gather, Clamp and Zero ops) is **legitimately light here**. VecMatMulAligned `lib_cs_01061`: 420-490 dispatches per
  2 s, 28 groups x 256 threads, 0.06 ms each. `lib_cs_01063`: 74 groups, 0.13 ms each. In all, ~50 ms per 2 s.
  `lib_cs_0172x` (LUT bakes): 0.04-0.06 ms each. No translator request for these.
- **The term that grows is the irradiance cache** (`indirect_light_probes`, indirect dispatches with group counts read
  back from their pre-pass records):

  | pipeline | +15 s | +25 s | +36 s (groups a dispatch, GPU ms per dispatch) |
  |---|---:|---:|---:|
  | `lib_cs_01368` IrradianceCache_GatherCopy (64 threads) | - | 56,307 / 5.5 | **919,216 / 87** |
  | `lib_cs_01396` GatherAlphaVBuffer_WithFar (32 threads) | 16,384 / 4.6 | 53,760 / 15.6 | 91,136 / 26.6 |
  | `lib_cs_01377` RadianceCopy (8x8) | - | 55,728 / 1.9 | 163,125 / 5.4 |
  | `lib_cs_01378` / `01374` IntegrateIrradianceSH / Shade | - | 6,206 / 2.5, 1.7 | 17,206 / 7.4, 4.4 |

  - `lib_cs_01368` has no thread-count check: each group reads `list[group id]` and all its lanes do their work (ISA).
    So the group count is the length of a GPU-built list, and that list grew 16x in 11 s. The pre-pass clamp
    (2^20 groups) is near.
  - Direct-queue GPU busy rose with it: 16% (+10 s) -> 35% (+30 s) -> 51% (+45 s). This matches the start of gates-b's
    GPU-bound phase (I).
  - Inferred cause: the list's counter is not reset every frame. In the same windows, 177 direct dispatches a frame
    were skipped as "pipeline compiling" (cold: the tier cannot be used, below), and a counter-reset pass among them
    would give exactly this growth. Not proven. Request posted to the draw/mesh / indirect owner.
- Not done: an offline replay of a captured command buffer. There is no headless `.gputrace` replay on this machine,
  only the Xcode GUI, and a capture of a 1 s command buffer over title memory would be several GB.

### 3. Frame time (M; +120/+240 s could not be reached)

At 14,848 MiB every world run ended 47-50 s in (footprint 13.7 GiB at +15 s, 14.2 GiB at +45 s, then the cap). gates-b
reached +240 s only under its old 16 GiB cap (15.6 GiB). So +30 s is the only comparable point. Both are tier-off runs
without a warm archive.

| | gates-b +30 s | fr10-b +30 s | fr10-h +30 s | fr10-h +45 s |
|---|---:|---:|---:|---:|
| frames/s | 1.1 | 3.3 | 3.1 | 2.8 |
| direct queue GPU busy | 80% (729 ms a frame) | 29% (87) | 35% (112) | 51% (184) |
| compute queue GPU busy | 0.1% | 0.7% | 5.0% | 4.7% |
| submission CPU, ms a frame: heaps / shadows / textures / replay | 316 / 31 / 29 / 113 | 380 / 62 / 43 / 33 | 592 / 65 / 39 / 53 | 340 / 58 / 42 / 37 |
| `heaps` inside the console-heap lock, ms a submission | 7.1-8.6 | | 5.3-6.0 | |
| gates timed out | 90% | 37% | 39% | 66% |
| footprint | 13.9 GiB | 13.7 GiB | 13.7 GiB | 14.2+ GiB |

- At +30 s the frame is CPU-bound on the console heap refresh. The `heaps` phase is timed around
  `refresh_console_heaps`, including the wait for `console_heap_mutex`. Now that several threads submit at once
  (submit phases add up to 125-188% of the wall time), its 10-15 ms a submission includes lock waits. Inside the lock
  the refresh takes 5.3-6.0 ms. At 100-120 submissions a second that lock is busy ~60% of the time (I).
- The 7.40 -> 2.52 ms of the heap-copy A/B was not seen in the world here: 5.3-6.0 ms inside the lock. That owner
  should look (device shim; not mine).

### 4. Found on the way (M)

- **Boot stops when the pipeline tier is on**: 5 of 5 runs (fr10-c..g) stopped at `xmem: 32-bit reservation of
  193986560 bytes ... refused: no free range below 4 GiB` (int3 exe+0x19f4f58). With `GTA6RT_METAL_ARCHIVE_TIERS=off`,
  2 of 2 runs booted. The only earlier difference in the logs is the tier being opened (3,012 pipelines). gta6rt has
  no 4 GiB page zero, so large anywhere mappings land in the low 4 GiB (a probe showed pthread stacks do too, hence the
  worker stacks above 4 GiB). Posted to the cache and title-pool workers (05:58 note, which corrects my 05:50 one).

### Switches added this round

`GTA6RT_METAL_RELEASE_WORKER` (default on), `GTA6RT_METAL_TABLE_IN_PLACE` (default **on**), and
`GTA6RT_METAL_DISPATCH_TIMING=<label substrings|all>` with `_S` (period, 30) and `_WINDOW_S` (2), off by default.

### Next (ranked)

1. Find who resets the irradiance-cache list counter (`lib_cs_01368`'s indirect records). Log the producer and reset
   passes skipped as "pipeline compiling" next to the record count, and run warm once the tier no longer breaks boot.
2. The console-heap refresh is now the CPU term (5.5 ms a submission, serialized): shim owner.
3. Tier archive mappings above 4 GiB (cache worker), so warm runs boot again.
4. Footprint: no run lives past +50 s at 14,848, so +120/+240 measurements need the memory work first.

## Round 10, meshlet-header worker (2026-11-27 05:00-05:50): the null meshlet header was the layer's; first 3D geometry in the G-buffer and depth

Tree `build/mhdr-x86_64` (`-build1..6.log`), launcher `build/mhdr-x86_64/run_world.sh` (s7_world_run.sh with this
tree's private run dir; it also holds the translator's probe lock so a probe cannot start inside a run). Runs
`mhdr-a..c`, each alone, hidden window, imported save set (`saves/manual-save-2026-11-26`, story load from the save:
world at +72.4 s in a and c), 13,312 MiB cap (offline translator probes were running), `GTA6RT_METAL_PCACHE=0`,
`GTA6RT_METAL_CB_ABORT_MS=2000`, load 4-10. **0 GPU errors, 0 failed command buffers; gpuEvent-gta6rt reports 9
before, 9 after every run.** Measured = printed by a log; inferred = reasoning.

### 1. Who writes the null header and why (measured)

- The null V# `00000000 00000000 00000000 30014fac` is the **layer's own encoding** (format 0x14, OOB raw, bit 24
  clear; the title's compiled V#s carry 0x11014fac): a buffer view `write_buffer_srv` / `write_constant_buffer_view` /
  `write_buffer_uav_at` wrote with zero records because `vsharp_address` found no backing. The title then copies that
  32-byte record into its per-draw tables with its own stores, which CopyDescriptors provenance cannot follow.
- Proof (mhdr-a, `GTA6RT_METAL_UNBACKED_TAG=1`: a zero-record view keeps the title address in its base, still no
  records): at record time 1,309 header slots (user data 9 + 0x80) had records, 87 were null, and every null one was a
  tagged copy, e.g. slot 0xee2d55c0 = `5dc96900 00000008 00000000 30014fac` = title **0x85dc96900, 336 bytes**;
  another 0x9116fab80, 480 bytes.
- Those addresses are in the title's **GPU-only stream window [0x700010000, 0xb00010000)** (xmem window, protect
  0x801, physical pool pages mapped per request). Why no backing: nothing in port/ calls
  `title_gpu_views::note_reservation`, so the window is not known as a pool pair and `resolve_pool_memory` never runs;
  as plain title memory it needs a copy window, and the 64 MiB `LEONIDA_TITLE_WINDOWS_MB` budget is full from boot
  (65,535 of 65,536 KiB); Metal has no geometry copies. 262,144+ views of title memory were written with no records
  by world+20 s ("title memory (no window range in the budget)", new reason labels on that line).
- So step 1's differential was not needed to answer it: the title never chose null; the difference is what our
  layer returned to `Create*View` (inferred for Vulkan: its geometry copies back some of these, round 5 measured
  4,096 zero-size views on MoltenVK by 100 s against 65,536+ on Metal; it drops every mesh draw anyway).

### 2. Fix (stopgap, opt-in) and the result (measured, mhdr-c)

- `GTA6RT_METAL_TITLE_WINDOWS_IN_PLACE=alias` (`title_window_in_place`): a title address in [4 GiB, 0x7f00'0000'0000)
  that a buffer SRV or CBV names gets a 16 KiB-aligned `mach_vm_remap` (copy=FALSE) alias of the title's committed
  pages around it (request + up to 1 MiB), wrapped once by a bytesNoCopy buffer and entered in the alias table; the
  title's own mapping is never wrapped. UAVs (and UAV counters) into such a window keep zero records unless
  `GTA6RT_METAL_IN_PLACE_ALIAS_UAV=1`. Cap `GTA6RT_METAL_IN_PLACE_ALIAS_MB` (default 1024): the windows are wired.
- **mhdr-c** (alias, 1 GiB cap reached at 3,556 windows before the world; imported title pages 766 -> 1,789 MiB):
  header slots at record time **9,101 with records, 13 null** (mhdr-a: 1,309 / 87); the instrumented
  `MS_VS_TransformD_c5fd36bb` variants (base, ~1, ~2, ~9, ~16, ~17, ~22; counters only, no `--regs`:
  `build/mhdr-x86_64/msl-count`) **1,098 groups, 81,968 primitives, 102,004 vertices** (round 9: ~1 primitive a
  group), paired `PS_DeferredTextured_c5fd36bb` variants **754,296 pixel invocations** (round 9: 0).
- Scene dump, upscaled frame 5 (world +~5 s): depth 66.6% non-zero, G-buffer normals (gbuffer1) 66.6%, gbuffer3
  66.6%, albedo (gbuffer0) 4.6%, gbuffer2 8.6%; frame 9 the same. **Kept:**
  `/Users/a/Desktop/Projects/GTA-VI/port/run/present/mhdr-c-first-geometry-gbuffer1.png` (G-buffer target 1, the
  normal/material target, 1280x720): a stilt house on a waterfront lot with decks, stairs, fences, power poles and
  low buildings behind it, all with correct depth (`mhdr-c-scene/scene_00005_depth.png`). What is wrong in it:
  albedo is almost all black (only distant props have colour: most material textures are still null or deferred,
  "deferred title textures 4,629 MiB in 2,778, no Metal texture yet"); the normal target is noisy red (decode or dither
  not checked); scene colour is 0 in every dumped frame (no lighting or composite ran in these first seconds); the run
  ended on the 13,312 cap at world+14 s (13,561 MiB; mhdr-a without windows reached world+24 s).
- mhdr-b (alias with UAVs and every other kind allowed, 1 GiB cap): the title faulted in the story load (+65 s,
  `EXCEPTION_ACCESS_VIOLATION` at exe+0x27c64b0 reading 0x7ff7ffe23888), not seen in any other log. One run, cause
  not proven (inferred: GPU writes landing in title memory through the new windows); hence UAVs stay out by default.

### 3. Item 3 (the "1.0f count" variants), measured only in part

- The valid +0x80 slots of mhdr-a/-c are 128-byte raw views (stride 0, 0x80 records) whose shader VA (e.g.
  0x18d08c90) is the slot-133 window of title 0xf2208c90 in the 185 MiB 32-bit region; the bytes there are rotation
  rows (`3f7fffde 3b02d5b7 39c51ede 00000000 ...`), and title VA and shader VA agree through the alias table, so the
  base is not shifted by our translation. Whether those draws read that slot as a meshlet header (dword 3 = 0) was not
  separated per variant: the counters above are summed over 7 variants. Not resolved.

### Limits of the stopgap (fail-loud lines exist for the cap and a refused alias)

- An alias shares the pages mapped at the time it is made: when the title remaps a stream-window page or decommits
  a range, the GPU keeps reading the old pages (stale, no fault). The windows are kept for the device's life and are
  wired (1 GiB at the cap; the footprint cap does not see wired memory).
- The title-pool worker's `GTA6RT_METAL_TITLE_POOL` (xmem pool as one memory entry + `title_pool_window`) is the real
  fix for the same addresses; my 05:32 note in run/requests/d3d12_metal.md gives them this case.

### Switches added

`GTA6RT_METAL_UNBACKED_TAG=1` (debug), `GTA6RT_METAL_TITLE_WINDOWS_IN_PLACE=alias`, `GTA6RT_METAL_IN_PLACE_ALIAS_MB`,
`GTA6RT_METAL_IN_PLACE_ALIAS_UAV=1`. Under `GTA6RT_METAL_GPU_DEBUG=1`: the first 48 distinct header-slot words at
record time with an explanation line for tagged ones (`explain_title_address`).

### Next (ranked)

1. Back title memory without the stopgap: the pool (`GTA6RT_METAL_TITLE_POOL`) for the GPU-only stream window and
   committed XMem memory; or register the stream pool pair (`note_reservation`) so `resolve_pool_memory` serves it.
2. Albedo: why most `PS_DeferredTextured` texture T#s are null/deferred in the first world seconds.
3. Run past world+14 s (cap 14,848 when no heavy job runs) for lit frames; then item 3 per variant.

## Round 9, draw/mesh worker (2026-11-27 03:45-04:55): why the G-buffer mesh draws draw nothing, named for one draw; no geometry yet

Tree `build/d3m-s4-x86_64` (`-build47..55.log`), runs `d3m-s9-a..k` with `tools/s7_world_run.sh` (private run dir),
each alone on the machine (slot polled, 2 collisions backed off), load 2.3-8, hidden window, B from +100 s, the lead's
13 GiB footprint cap, stop at the first GPU error. **0 GPU timeouts, 0 GPU page faults, 0 failed command buffers in 11
runs; gpuEvent-gta6rt reports in /Library/Logs/DiagnosticReports: 9 before, 9 after (0 new).** Measured = printed by a
log, the ISA or a tool; inferred = reasoning.

### 1. One draw, named (measured): `_MS_VS_TransformD_c5fd36bb_Wrapped` (G-buffer, with `PS_DeferredTextured_*`)

- **What the shader does** (RDNA2 ISA, `spike/s2-make-game/work/shaders/dxbc/disasm/_MS_VS_TransformD_c5fd36bb_Wrapped.s`
  lines 21-46, 171-190): `s_load_dwordx4 s[32:35], s[44:45], 0x80` with s44 = s17 = user data 9 (a descriptor table)
  loads the **meshlet-header V#**; each meshlet's 16-byte record gives vertex count = `(dword3 & 0x7f) + 1` and
  primitive count = `((dword3 >> 7) & 0x7f) + 1` (`s_and_b32 0x7f`, `s_bfe_u32 0x70007`, `+1`; per lane
  `buffer_load_dwordx2 ... offset:12`). A header that reads 0 therefore gives 1 vertex and 1 primitive.
- **On the GPU** (debug variant, registers at the output-count message, d3m-s9-b/-d/-i): m0 = 0x1001 (1 vertex,
  1 primitive), s[32:35] = `00000000 00000000 00000000 30014fac` (a raw-buffer V# with base 0 and 0 records), the
  primitive's three indices 0,0,0 (a point-sized degenerate triangle), positions finite and inside the clip volume
  (e.g. `(7.207 16.43 0.1496 29.29)`); the paired instrumented `PS_DeferredTextured_*` variants count **0 pixel
  invocations**. Over all instrumented variants of the family: 11,071 groups, 11,622 primitives (d3m-s9-d), i.e. ~1
  primitive per meshlet. Culling, depth state, viewport and pass are not the cause: the backend's mesh pipeline line
  for that draw reads `rts 6 (f29..), depth f20 test 1 write 1 func 7, cull 3 front_ccw 1 depth_clip 1, viewport
  0,0 1280x720 [0,1], scissor 0,0-1280,720, pass 1280x720, object 1 mesh 128x1x1`; a degenerate triangle covers no
  sample whatever the state.
- **Where the null header comes from** (record-time copies, new, `GpuDebug::note_table_slot`): when replay records
  each indirect mesh draw of the base variant, the 16 bytes at user data 9 + 0x80 in the arena copy *and* in the
  title's own descriptor heap: **354 of 354 null** (`00000000 00000000 00000000 30014fac` in both; d3m-s9-i). So the
  GPU reads exactly what the title's heap holds when the draw is submitted — not a stale arena copy, not a ring reuse,
  not a layer rewrite. With every `Create*View` that wrote no-records recorded and followed through `CopyDescriptors`/
  `CopyDescriptorsSimple`, **none** of these slots traces back to a layer view of unbacked memory (0 provenance
  hits), so the title writes that null raw-buffer descriptor into the slot with its own code (or copies it there with
  its own memcpy; inferred: the meshlet-header buffer it would name does not exist yet for those draws; what the title
  waits for is not identified).
- **The other variants** (85% of the family's records have a non-null header V# at record time, d3m-s9-d/-e): the V#
  is a slot-70 "SRV of placed memory" of the title's 185 MiB 32-bit region (e.g. title 0xef006300, stride 16, 22
  records) rewritten to its slot-133 window's shader VA (0x1b1f6300; the same holds for the stride-96 buffer in
  s[36:39]: the title asks for 0xf6b8af00 = 1400 x 96 B, the V# carries 0x2208af00). The CPU reads at that title
  address `00000000 00000000 3f800000 3f800000 | bf800000 x4`: dword 3 = 1.0f, whose low 14 bits are 0, so also
  1 vertex / 1 primitive per meshlet (inferred from the CPU view; the GPU-side read of those variants was not captured:
  the world phase ended at the 13 GiB cap 6-15 s in, before they ran, d3m-s9-j/-k).
- So: **the G-buffer mesh draws rasterise nothing because every meshlet they emit is one degenerate triangle: the
  meshlet header they read is a null V# the title wrote (base variant) or 16-byte records whose packed counts are 0
  (other variants).** Indirect arguments are fine (no count buffer, no record past the caps; the records with groups carry
  1-18 x 1 x 1, the rest are zero, as GPU culling leaves them), the producer guard records them, the render pass and targets are the G-buffer's.

### 2. Fixed on the way (measured)

- **Arena keep-out by our own VA reservation.** The shared title-memory probe took each 1 MiB chunk the module
  reserves at registration for a guest allocation (`gta6rt_vm_query` answers RESERVE for a no-access mapping:
  `title memory probe: 0x66c0000 is in a guest allocation [0x6600000, 0x6700000)`), so the arena kept out of 1 MiB
  "title" ranges (`41.35 MiB free (title 21.00 MiB ... longest gap 1.00 MiB)`) and from world entry refused every
  buffer over 1 MiB (19 in d3m-s8-m). `platform/register_metal.cpp` now installs a Metal probe after the shared one
  that answers "no title memory" for a reserved chunk (`... is the arena's VA reservation, no title memory`, 65,536+
  answers a run); `GTA6RT_METAL_ARENA_PROBE=shared` restores the old one. Every keep-out line since: `title 0.00 MiB`.
- **Arena 384 MiB by default** (`create_arena` + the registration reservation; `LEONIDA_ARENA_MB` wins). With 256 MiB
  the world filled it for real (252.8 MiB: buffers 129, descriptor heaps 102, shaders 22) and 35 world buffers were
  refused in 10 s, each after 16 blocks outside the arena whose shader VA overlapped the title's memory, all kept:
  560 set-aside blocks, Metal "other" 91 -> 2,710 MiB (d3m-s9-a; the memory worker's 04:20 note owns that path). With
  512: 0 refusals, 0 set-asides, peak 327 MiB (d3m-s9-b), but one boot found no room below 4 GiB for the title's
  185 MiB 32-bit reservation (`xmem: 32-bit reservation ... refused`, int3 at exe+0x19f4f58). 384 (d3m-s9-c..k):
  peak 325 MiB, 0 refused, 0 set aside.
- **Stop at the first GPU error by default** (lead's rule 3; port/metal `mtl_queue.mm` completion handler): a command
  buffer failing with its own error (page fault included) makes the device lost, so the layer reports and exits 3.
  `GTA6RT_METAL_GPU_ERRORS=continue` goes on after a page fault as before.
- **`GTA6RT_METAL_CB_ABORT_MS`** (opt-in, `InFlight::watch`): a command buffer running that long ends the process
  (`_Exit(5)`) with its encoders named, before Metal's watchdog. It ended d3m-s9-f at 2,007 ms (below).
- `tools/s7_world_run.sh`: `GTA6RT_METAL_FOOTPRINT_CAP_MB=13312` unless the caller sets it.

### 3. Task items

- **Producer guard narrowed** (round 8, default `GTA6RT_METAL_INDIRECT_GUARD=producers`): with warm pipelines
  (d3m-s8-m, tier prebuild) indirect mesh 4,433 a frame, 4,380 recorded; indirect dispatch 841, 841 recorded; the
  guard skipped 1-192 compute passes a frame. With the prebuild off (my 13 GiB runs) the census is dominated by
  "pipeline compiling" (d3m-s9-b +92 s: indirect mesh 7,551 / recorded 1,862 / compiling 3,999 / guard 1,633;
  indirect dispatch 1,115 / recorded 14 / compiling 1,021).
- **Warm archive**: the cache worker's tier is looked up by default; its bounded prebuild costs ~0.7-1 GiB in the first
  world seconds (d3m-s9-c: Metal "other" 1,017 MiB, cap 3 s into the world), so my runs used `GTA6RT_METAL_PCACHE=0`
  (lookups only).
- **Threadgroup override manifest**: on by default since round 8 (`build/d3d12_metal-msl-overrides`, 118 rows; the
  uint overlay loads after it). Measured this round: threadgroup refusals 5.4 a frame (one 36,056-byte shader left);
  the largest refusal class is now `XPC_ERROR_CONNECTION_INTERRUPTED` (MTLCompilerService dying: 47-59 indirect mesh
  draws a frame).

### 4. Tried and stopped (measured)

- d3m-s9-f, `GTA6RT_METAL_VSHARP_TITLE_FIRST=soft` limited to the title's first low allocation
  (`GTA6RT_METAL_VSHARP_SOFT_RANGE=18000000-1f000000`, in-place windows `=soft`): 32+ ambiguous V#s (104-byte constant
  ranges) rebased to in-place windows, then a direct-queue command buffer (`lib_cs_01208`, `lib_cs_03138` x5,
  `lib_cs_01467` x2, `lib_cs_01449`, `lib_cs_01144 .. 01123`) ran past 2 s and the abort switch ended the run 12 s into
  the world, before any timeout. Same family of spins as d3m-s8-e/-g: compute passes fed partly corrected inputs run
  unbounded. Not repeated.
- The world phase under the 13 GiB cap lasts 3-28 s (footprint 10.1-11.8 GiB at world entry, +2.2-2.8 GiB in the next
  10 s, mostly the title pool becoming resident), too short for most instrumented variants to run.

### Next (ranked)

1. Why the title writes a null meshlet-header V# (base variant) / why the header records carry no counts (others):
   trace the title's own descriptor writes into the user-data-9 tables (a write watch on one table page of the console
   heap, naming the title code address) and the producer of the 16-byte meshlet records at the slot-70 addresses.
2. Backing title memory without copies or ambiguity (lead's 03:58/04:15 notes: pool-backed XMemVirtualAlloc, one
   bytesNoCopy buffer per pool, translation at the consumer) — the soft rebase shows partial fixes make compute spin.
3. A loop cap in translated compute shaders (shaderc_msl), so experiments that make data more correct cannot spin.

### Switches added this round

`GTA6RT_METAL_ARENA_PROBE=shared`, `GTA6RT_METAL_GPU_ERRORS=continue`, `GTA6RT_METAL_CB_ABORT_MS`; debug only:
`GpuDebug` record-time header snapshots and copy-tracked unbacked slots (with `GTA6RT_METAL_GPU_DEBUG=1`),
`tools/msl_instrument.py --regs-if N` (dump only groups whose sN is non-zero; the dump now reads the first 4 words at
the bases of s[32:35], s[36:39], s[44:47], s[48:51] when their records are non-zero).

## Round 8, gates worker (2026-11-27 02:15-03:10): no command buffer waits on the GPU any more — 0 GPU timeouts in 313 s of world; the pool-node fault did not recur; frame time is GPU-bound

Tree `build/gates-x86_64` (new; attach.cmake super-build, `-build1..4.log`), launcher `build/gates-x86_64/run_world.sh`
(s7_world_run.sh with this tree's private run directory). Runs `gates-a..d`, each alone on the machine (slot polled),
load 11-18, swap 3.2-4.5 of 4-5 GiB. Measured = printed by a log or tool; inferred = reasoning.

| run | settings (besides defaults) | in the world | end | GPU timeouts |
|---|---|---:|---|---:|
| gates-a | tail watch; pipeline tier on (cache worker's new default) | 23 s | footprint cap (exit 4) | 0 |
| gates-b | tail watch, `GTA6RT_METAL_ARCHIVE_TIERS=off` | 257 s | footprint cap (exit 4): hash table 6 x 160 MiB | 0 (0 failed command buffers) |
| gates-c | as b + scene probe (present + dumps) | 125 s | footprint cap (exit 4) | 0 |
| gates-d | as c + `GTA6RT_METAL_TABLE_GENERATIONS=4`; sources now record indirect mesh draws (draw/mesh worker) | **313 s** | watchdog, rc 124 | **0** (9 command buffers failed with GPU *page faults*, below) |

### 1. GPU timeout: removed by design (measured)

- **Cause** (d3m-s7-k, read from its log): the command buffer Metal killed had GPU time 0: it was waiting at an
  `encodeWaitForEvent` for another queue's progress. The direct queue was 1.5 s+ behind (command buffer 36383 with 225
  behind it), and four compute queues' command buffers waited on it on the GPU for longer than the watchdog. Every
  cross-queue wait the adapter encodes (`MetalQueue::consume_waits`: D3D12 fence waits whose Signal is submitted but
  not done), the presenter's wait for every queue, and `wait_value` (the upscale stage's CPU-released gate) were GPU
  waits of unbounded length.
- **Fix** (port/metal `mtl_queue.mm`, announced in run/requests): a **commit gate**. A command buffer whose event waits
  are not met when it is closed is `enqueue`d but not committed; a listener of its own commits it once every event has
  reached its value. Metal keeps the queue's later command buffers on the CPU behind it (measured offline,
  `build/gates-x86_64/tests/enq.mm`: a later command buffer stays "committed", not scheduled, for 1.5 s until the
  earlier one is committed). So no command buffer waits on the GPU for anything but its own queue. `wait_value` uses
  the same path (its CPU-released event is a commit condition). `GTA6RT_METAL_COMMIT_GATE=0` restores GPU waits.
  Offline test `build/gates-x86_64/tests/test_commit_gate.cpp` (links the tree's `libgta6rt_metal.a`): cross-queue,
  CPU-signalled, wait_value and a 40-deep cross-queue chain: PASS.
- **Result**: 0 GPU timeouts and 0 waiting command buffers in gates-b (257 s in the world, default settings) and gates-d
  (313 s, probe on); before: d3m-s7-h and -k timed out 153 / 115 s in. Command buffers held on the CPU: 100-240 per
  10 s in the world, held p50 45-700 ms, longest 3.3 s (gates-b) — waits that were GPU waits before; 1-2 a run were
  held past 1.5 s (the upscale gate, a present).
- **Gate latency before / after** (list-value gates, `[rt] d3d12_xs: list values so far`): d3m-s7-k: 1,789 gates, 95%
  timed out, releaser 20.6 ms a signal on average, longest 1,401 ms; gates-b: 5,189 gates, 75% timed out, 38.2 ms,
  longest 2,391 ms; gates-d: 7,145 gates, 91% timed out, 29.0 ms, longest 2,015 ms. The GPU part of every wait is now
  0 by construction; the releaser is not faster, because its Signal runs the held submissions' replay on its own thread
  (including `MetalBindless::flush` waiting up to 2 s for a free hash-table generation). Next: run released batches on a
  per-queue worker so the releaser's Signal returns at once (needs a re-pump rule when `release_ready` cannot take the
  queue lock; not done).
- New 10 s lines (port/metal): `[metal] GPU time last 10 s, N presents: ...` (busy time per queue, union of
  GPUStartTime..GPUEndTime, and per presented frame; `GTA6RT_METAL_GPU_REPORT_S`), `[metal] commit gate: ...`,
  `[metal] command buffer N (queue) has been held on the CPU for X ms ...` (past `GTA6RT_METAL_CB_BUDGET_MS`).
- New failures in gates-d, not timeouts: 9 command buffers failed with `kIOGPUCommandBufferCallbackErrorPageFault`
  (GPU time 12-107 ms; faulted encoders `lib_cs_02860..02099` x4, `lib_cs_03010` x2, `lib_cs_01449`, `lib_cs_02038`,
  `lib_cs_00897`), from when the draw/mesh worker's sources recorded GPU-driven work (2,600 indirect mesh draws a
  frame); the device was not lost and the run went on. Reported to that worker (inferred: wild addresses in newly run
  GPU-driven work, not the commit gate: none of b/c had one).

### 2. The pool-node overwrite at exe+0x4e9b54a: not reproduced; explicit GPU writes ruled out (measured)

- **What the title does** (disassembly, exe+0x4e9b480): it takes a 64 KiB chunk from its allocator and writes the
  chunk's node into its last 0x18 bytes: `{next 0, object, count}` (`mov [r15+rax-0x18], 0 / rbx / count`), then links
  it at `[allocator+0x48]`; a later walk (exe+0x4e9b4b2) reads `object` and `count` of each node and calls exe+0x4e9b530,
  which faults on object -1 (`[rcx+0xc]` = 0xb) or 0. So the node is overwritten after the title wrote it.
- **Not reproduced**: 0 faults in gates-a..d (718 s in the world, 438 s of it with the scene probe that correlated 4 of
  6 times in round 7). With so few runs this is not evidence that it is gone (inferred: 0 of 2 probe runs against 4 of
  6 before, p ~0.1).
- **Ruled out (measured)**: every GPU write command the backend encodes into title memory (fills / UAV clears,
  `write_value`, buffer copies, query resolves, texture-to-buffer copies whose destination is a bytesNoCopy title
  buffer, `TitleWrites` in mtl_queue.mm): 9,070-26,257 a run, all `copy_buffer` into slot-133 windows at
  0xb8xxxxxxx; **none** in the pool region [0x30000000, 0x70000000) and none over a 64 KiB tail there. So no adapter
  clear, readback, resolve or copy writes those nodes.
- **Left**: shader UAV writes through title-written V#s into the placed GPU buffers the title keeps in that region
  (bounded by the V#s, so a write there is to a range the title handed the GPU, landing after the title reused it:
  the GPU runs 0.5-3 s behind the CPU in the world), and CPU writers (the list-values pump lands only 4/8-byte values).
  Diagnostic in place for the next occurrence: `GTA6RT_METAL_TAIL_WATCH=30000000,70000000` (reads every 64 KiB tail
  there every 20 ms; logs a valid node `{0|title, title object, count < 2^20}` that turns into a clear value, with the
  command buffers completed since the previous scan, encoders named). In gates-a/d it logged 2 / 38 such changes, all
  legitimate-looking title reuse (`{0, 0xb0438xxxx, N}` -> `{N+1, 0, 0}` or float data `3f000000 3f733333 3f800000`),
  none followed by a fault. Faulting nodes of round 7 for comparison: 0x38eaffe8 .. 0x4396ffe8, pc7-warm-b 0x5ca1ffe8.
- **pc7-warm-a SIGSEGV** (169 s in the world, `addr=0x12d5e8000` page aligned, `pc=0x1000056d2bd`): the gta6rt image
  is at 0x10000000000 (low page zero), so the pc is image offset 0x56d2bd: `leonida::MetalBindless::flush() + 685` in
  both the 01:51 d3m-s4 and the 02:18 pc7 binaries (the 23:38 binary that crashed is gone; inferred). `flush` reads
  `bytes = table_.size_bytes()` once, then (at the generation cap) unlocks and sleeps; a table that doubles meanwhile
  is written by `write_to_buffer` into a generation picked or made for the old size, running off the end of that
  shared buffer onto an unmapped page. pc7-warm-a was at the cap (32 flush waits) with the table already doubled once.
  Request posted to the bindless owner (refresh `bytes` after re-locking; check the size before writing).

### 3. Frame rate: GPU-bound (measured, per presented frame)

| window | frames/s | direct-queue GPU busy | GPU ms a frame | submission CPU ms a frame (heaps / replay) | compute queues |
|---|---:|---:|---:|---:|---:|
| gates-b, 14-170 s in the world | 0.6-1.8 | 75-99% | 550-910 | 490-1,690 (240-920 / 50-730) | 1-3 ms a frame |
| gates-b, 210-257 s in the world | 2.6-3.9 | 23-28% | 70-92 | 240-380 (165-250 / 17-40) | |
| gates-d (indirect mesh recorded), 60-313 s | 2.0-3.4 | 72-97% | 250-390 | 220-550 (130-400 / 22-210) | |

- In the slow phase the GPU sets the rate: the direct queue is busy nearly all the time and command buffers of
  0.6-1.1 s all carry `compute x366-1002 library/lib_cs_01035|01064 .. lib_cs_01050` and `compute x78-137
  lib_cs_01609 .. lib_cs_01728`; the submission CPU numbers in that phase include waiting for the GPU (hash-table
  generation cap). Run e's 7-19 frames/s was 22-32 s into the world, before those compute passes ran (inferred from
  gates-b's own curve: 14-18 frames/s at +73 s, then 1 frame/s from +84 s as the pipelines become ready).
- When those passes stop (gates-b +270 s) the submission CPU sets 3-4 frames/s and `heaps` (the console descriptor heap
  copy, ~6 ms a submission x 26-45 submissions) is two thirds of it.
- Gate waits are not the limiting term: held command buffers sit in the shadow of the direct queue's backlog.
- The largest terms are not mine (title compute as translated; `heaps` in the device shim): requests posted in
  run/requests/d3d12_metal.md (per-encoder GPU timing of those families; a cheaper console-heap copy). No frame-rate
  fix of mine this round.

### Scene targets

No 3D geometry yet. gates-d frame 605's alpha-prepass target set (3 targets by `PS_AlphaPrepass`) was the first
non-empty render-size target I saw: `run/present/gates-d-scene/scene_00605_gbuffer2.png` (R8, 93% non-zero: a vertical
gradient with dither noise and one black square — a screen-space term, not geometry); the scene colour was NaN/Inf in
every texel, the G-buffer proper and depth empty (depth 1.0 only in the 48 letterbox rows).

### Switches added this round

port/metal: `GTA6RT_METAL_COMMIT_GATE` (default on), `GTA6RT_METAL_GPU_REPORT_S` (10), `GTA6RT_METAL_TITLE_WRITES`
(default on), `GTA6RT_METAL_TAIL_WATCH=<lo>,<hi>` (off).

## Round 7 (2026-11-26 22:35 - 11-27 02:10): stability and the first world frame on Metal — the heap damage is gone, a title-side fault and an empty G-buffer remain; no world frame yet

Tree `build/d3m-s4-x86_64` (`-build19..29.log`). Launcher `tools/s7_world_run.sh` (new): s4_world_run.sh with a private
run directory (`build/d3m-s4-x86_64/rundir`: saves, overlay, registry, Metal pipeline archive) reset from
`rundir-template` (the imported user set) before every run; `run/saves` / `run/overlay` were never written;
`S7_B_AFTER=T` presses B instead of A from T s (B dismisses the title's COULDN'T AUTOSAVE page). Every run alone on the
machine (slot polled, collisions backed off). Evidence: `run/logs/s4-worldframe-metal.log` (rewritten,
`tools/s7_evidence.py`), proposed check `tools/s4_check.py`. Measured = printed by a log or tool; inferred = reasoning.

### Runs

| run | settings | world at | end | in the world |
|---|---|---:|---|---:|
| d3m-s7-a | FSR4 refused, scene probe (present + dumps) | +70.8 s | title fault exe+0x4e9b54a | 6 s |
| d3m-s7-b | probe, packed mesh shaders (manifest override); the upscale worker's MetalFX stage from here on | +70.4 s | footprint cap (exit 4): 48 hash-table generations, 1.9 GiB | 121 s |
| d3m-s7-c | as b, + table back-pressure, labelled encoders | +70.3 s | title fault exe+0x4e9b54a | 19 s |
| d3m-s7-d | as c, producer guard off | +70.4 s | title fault exe+0x4e9b54a (3 threads) | 10 s |
| d3m-s7-e | **nothing of mine switched on** (no probe, no override, guard on) | +70.9 s | watchdog, rc 124: **S4a PASS** | 202 s |
| d3m-s7-f | (no probe) | - | invalid: macOS maintenance at load 44-54, 1,043 submissions in 29 min; killed | - |
| d3m-s7-g | probe (present + dumps), override, guard off | +70.0 s | title fault exe+0x4e9b54a | 8 s |
| d3m-s7-h | probe (dumps only, cheaper), override, guard off | +69.5 s | **GPU timeout** (named ahead, below), device lost exit 3 | 153 s |
| d3m-s7-i | negative control: hooks removed (`GTA6RT_METAL_BVH_HOOKS=0`), heap writes dropped (`GTA6RT_METAL_DROP_HEAP_WRITES=1`) | +69.3 s | footprint cap (exit 4, 16.6 GiB) | 172 s |
| d3m-s7-j | nothing of mine switched on, B from +100 s (the world drawn from +106 s to the end, 1.5 frames/s) | +69.8 s | watchdog, rc 124: **S4a PASS** | 203 s |
| d3m-s7-k | as j | +69.4 s | **GPU timeout** (a waiting compute-queue command buffer, GPU time 0), device lost exit 3 | 115 s |

Load average 3-17 during the runs (54 during f), swap 2.1 of 3 GiB; other workers' titles between mine, never beside.

### 1. The world-phase heap damage: cause removed (measured)

- **The hooks.** `rt::xbox_bvh::set_hooks` is installed by `create_device_proxy` in `d3d12_xs_device.cpp`, which this
  module compiles from `port/d3d12_vulkan` by path, and `LEONIDA_VULKAN_DEVICE` is defined here too
  (`gta6rt_d3d12_vulkan_headers`): so spike/r8-world's patch 01, applied to the live tree at 22:32, installs the Metal
  copy's `device_proxy_real_device` / `real_object_behind_proxy` / `rt::d3d12::hold_until_reset` once the module is
  rebuilt (the menus worker added `xbox_bvh::` to `check_prelink.sh`'s allowed namespaces). Evidence: the shim's
  "RehydrateRaytracingAccelerationStructureBatched2X: the list cannot hold its scratch" line is in every round-6 log
  (d3m-s4-e/-f/-j) and in none of a..h.
- **Round 6's guard never fired**: `land_outside_arena` treated any `gta6rt_vm_query` hit as a guest allocation, and
  that call falls back to `mach_vm_region` for any host mapping, the malloc heap included ("dropped" was printed 0
  times in round 6). Now a VA in a malloc-tagged region (`mach_vm_region_recurse` user tags 2/3/4/7/8/9/11/12, checked
  under Rosetta with a probe) is counted and named; `GTA6RT_METAL_DROP_HEAP_WRITES=1` drops it (default: stored, since
  the title's HeapAlloc is host malloc).
- **Confirmation**: with the hooks, 0 list value writes landed in a malloc region in 8 runs (a-e, g, h: 0 lines);
  with the hooks taken out again after `create_device_proxy` (`GTA6RT_METAL_BVH_HOOKS=0`, new switch in
  `set_device_outer`), d3m-s7-i: "no proxy hooks are installed", the scratch line, and **2,048+ writes of 0 into malloc
  blocks at 48-byte steps** (`0x7fd002690360`, `...390`, `...3c0`: the 48-byte
  `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC` elements spike/r8-world named), all dropped by the guard; that
  run then lived 172 s in the world. The `GraphicsCommandList::Reset` SIGSEGV at 0x10 (round 6: 2 of 2 plain-tree
  runs, 5-8 s into the world) did not recur in any run.
- **A second collision, fixed**: a buffer block outside the full arena has console VA = its Metal gpuAddress minus the
  arena's, numbers where the title allocates (0x3e740000 in d3m-s4-*, 0x61518000 / 0x11a310000 in d3m-s7-a) — replay
  then resolved title addresses there to the block and list writes to the block's VA went to title memory.
  `external_window` now reserves each block's VA range in the process (no access, no memory) and sets a block aside
  when its range is not free (4-32 set aside a run); `land_outside_arena` tries the blocks first.
  `GTA6RT_METAL_EXTERNAL_RESERVE_VA=0` turns it off.

### 2. Survival: target not met (measured)

- Three consecutive 3-minute world runs: **not reached**. Passing S4a (world + 180 s, no fatal, no GPU error, no
  device loss): d3m-s7-e (202 s), | d3m-s7-j | nothing of mine switched on, B from +100 s (the world drawn from +106 s to the end, 1.5 frames/s) | +69.8 s | watchdog, rc 124: **S4a PASS** | 203 s |
| d3m-s7-k | as j | +69.4 s | **GPU timeout** (a waiting compute-queue command buffer, GPU time 0), device lost exit 3 | 115 s |K_SUMMARY.
- **Open: a title fault, `exe+0x4e9b54a` reading 0xb / 0xc** (a, c, d, g; 6-19 s into the world). The title walks its
  pool allocator's chunk list (`exe+0x4e9b480`) and a node's object pointer and count are -1 / -1 (a, d, g) or 0 / 0
  (c); the nodes are at `0x3a5cffe8`, `0x3a62ffe8`, `0x3a64ffe8` (d, three threads at once), `0x38eaffe8` (c): each the
  last 0x18 bytes of a 64 KiB chunk of the title's VirtualAlloc region `0x372b0000` (185 MiB), the same region where
  the title places small GPU buffers (`SRV/UAV of placed memory 0x38ef6850`, `0x3a8ab100` in the same logs). So whole
  chunks of title memory are overwritten with a clear value (0 or 0xFFFFFFFF), by a GPU or layer write landing in
  memory the title had reused (inferred; the writer is not found). It came in 4 of the 6 runs with my scene probe on
  and in none of 8 without it (e, i, j, k here; the upscale worker's up7-a/-b/-c and the cache worker's pc7-rec-a ran
  120-180 s in the world with the same sources). The probe records no GPU command until its first latch, and run c faulted
  without any latch, so the probe is a trigger (timing, malloc churn), not the writer (inferred). d3m-s7-h, the probe
  made cheaper (no per-draw string), ran 153 s without the fault.
- **GPU timeout, named ahead** (d3m-s7-h, producer guard off): world graphics command buffers grew from 1.0 to 2.2 s on
  the GPU in 60 s, each ~400 compute dispatches (encoders `library/lib_cs_01330 .. lib_cs_01087` x46,
  `lib_cs_02039 .. lib_cs_01370` x136, `lib_cs_01379 .. lib_cs_01403` x151, `lib_cs_03000 .. lib_cs_01365` x18); then
  Metal timed out a waiting compute-queue command buffer and the device was lost. With the guard on (default) no world
  command buffer reached 1 s in e or i. Not repeated. The FSR4 timeout is moot on the shared tree: the upscale
  worker's host upscaler (MetalFX) is on by default since b and the FSR4 DLL is not loaded.
- **Footprint cap** (16 GiB, exit 4) ended b and i: b by 48 live descriptor-hash-table generations (1.9 GiB, command
  buffers of 1.1 s let none retire); fixed: `MetalBindless::flush` waits up to 2 s for a free generation at 8 live
  (`GTA6RT_METAL_TABLE_GENERATIONS`); since then 8 live (160-320 MiB). i still reached 16.6 GiB at 172 s in the world
  (Metal 6.4 GiB: title copies 1.65, render targets 2.10, descriptors/arena 0.58, buffers 0.65, other 0.60; title pool
  4.7 GiB; malloc 1.9 GiB).
- **Budget monitor** (port/metal `mtl_queue.mm`, `GTA6RT_METAL_CB_BUDGET_MS`, default 1500): a command buffer still
  running that long after it could start is printed with its encoders while it runs, and again when it completes; the
  encoder labels now carry the pipeline's description label (`PipelineCore::label_text`) and the builtin kernels' names,
  and the summary the indirect pre-pass scratch and record counts. Indirect grids are capped at 2^18 mesh / 2^20
  compute groups per record (`GTA6RT_METAL_INDIRECT_MESH_GROUPS` / `_CS_GROUPS`).

### 3. First world frame: diagnostic path built, the scene it shows is empty (measured)

- **Scene probe** (`SceneProbe`, metal_context.h; replay hooks in `record(Draw)`, `record(IndirectRasterDraw)`,
  `record(Dispatch)`, `record(ExecuteIndirect)` and the command loop): per frame the HDR colour target written last
  at render size (1280x720, `GTA6RT_METAL_SCENE_SIZE`) before the upscaler (an `fspx/` dispatch, or the upscale
  worker's `gta6rt-upscale` marker), the widest multi-target set (G-buffer) and its depth.
  `GTA6RT_METAL_PRESENT_SCENE=1|2`: that colour, copied at the latch, tone-mapped (1 - exp(-c * exposure), exposure from
  a readback every 30 frames, sRGB) and scaled to the window, under the title's UI plane or alone (port/metal
  `PresentPlane::tone_exposure`, a tone-mapped compose pipeline). `GTA6RT_METAL_SCENE_DUMP=<dir>,first,every,count`:
  colour, every G-buffer target and depth as PNGs with value statistics, and the frame's render-target passes in the log.
- **What it found** (d3m-s7-h, upscaled frames 1/3/5, guard off): colour `f10` (RGBA16F) 1280x720 written by
  `VS_TileGridDefaultPass + PS_CompositeIndirectDrawBlend`; G-buffer 6 targets (`f29` sRGB albedo, 4 x `f28`, `f34`
  RG16F motion) + depth `f20` (D32S8), drawn by `MS_VS_TransformD_c5fd36bb`, `MSNC_VS_TransformSkinD`,
  `MS_VS_PropFoliageDeferred`, `MS_VS_TransformTerrainNorm...` with `PS_DeferredTextured*`: **1,100+ indirect mesh draws
  recorded into the G-buffer pass of frame 3**, shadow maps (`D f55` 256-768) 600+ more. Yet at the latch **every
  G-buffer target is 0 except target 4 (a constant 0.502 grey: a cleared normal), depth is 0 everywhere (with inverted
  depth: the far plane, nothing rasterised), and the colour is 0**. So geometry is recorded and draws nothing:
  culling output (the mesh grids come from GPU culling compute passes that are skipped while compiling or guarded),
  transforms, or the mesh stage's output counts. Not separated yet. Kept:
  `run/present/d3m-s7-h-scene/scene_00005_{colour,depth,gbuffer0}.png` (black; the evidence that the G-buffer is empty).
- **No 3D-scene frame exists** on this path yet; no world-phase present was kept (they are black or the autosave page).

### 4. Draw classes in the world (census, d3m-s7-j t=227 s: world drawn since +106 s, guard on, 14 presents in 10 s, per frame)

| class | per frame | recorded | skipped, why |
|---|---:|---:|---|
| indirect mesh (G-buffer, shadows, depth) | 1,368 | **0** | a compute pass before it not ready 1,055.7 (the producer guard), pipeline compiling 302.3, Metal refused 10 |
| indirect mesh split64 | 17 | 0 | compiling 17 |
| dispatch | 1,067 | 982 | compiling 84, PSO refused 1 |
| indirect dispatch | 833 | **0** | guard 492.6, compiling 340.4 |
| indirect rect-list (tiled lighting) | 257 | 0 | **Metal refused 170: "output of type float4 is not compatible with a MTLPixelFormatR32Uint color attachment"**, compiling 74, guard 13 |
| draw | 107 | 37 | compiling 43, Metal refused 27 (float4 output to an RG32Uint attachment) |
| draw indexed / rect-list / indirect draw / indirect indexed | 33 / 8 / 4 / 19 | 23 / 4 / 0 / 0 | compiling, guard, PSO refused 1 |

So with the guard on no GPU-driven work is drawn at all, 2.5 minutes into the world: some compute pipeline is
always still compiling (340 indirect dispatches a frame), which keeps the 2 s guard window open. With it off (h) the
G-buffer pass records 1,100+ mesh draws and still rasterises nothing (section 3), and the heavy compute command
buffers end in a GPU timeout. The rect-list class is the deferred lighting: 170 a frame refused for a float output to a
uint target (a pixel variant with a bit-cast output is needed, a translation change; not done).

- **Threadgroup memory** (the 26 refused a frame): SPIRV-Cross stages every mesh output in a 128-entry threadgroup
  array, and an MSL float3/uint3 takes 16 bytes there. `tools/mesh_tg_pack.py` rebuilds a store shader with its own
  recorded passes plus `packed_float3` staging and `uchar4` triangle indices (lossless; Metal's indices are uchar):
  `MS_VS_Transform_4dbafe93~13` 36,056 -> 29,912 B, `~16` 35,544 -> 29,912 B (112.8 + 13.3 draws a frame refused in
  e). The two at 34,008 B (`MS_VS_Transform_0c91b99b`, `MSNC_VS_TransformSkin_58f6b052`, 15 float4 outputs) still
  pass Metal's threadgroup + mesh-output limit (32,472 + 30,720 > 61,440): rebuilt through the store's meshlet split
  (route split64, 7 chunks of 64). All four create pipelines with their paired `PS_DeferredTextured_*` (offline probe)
  and, in the title, no 36,056/35,544 refusal appeared with the override (b, c, d, g, h). A scan of all 9,232 mesh
  shaders: 110 packed into the override, 2,044 still over by the tool's estimate (mostly float4 staging; the estimate
  of the mesh-output part is conservative). Override: `GTA6RT_METAL_SHADER_MANIFEST=build/d3m-s4-x86_64/msl-overrides/manifest.tsv`
  (the store's manifest with those rows pointing at `msl-overrides/cas`; not on by default).
- **Not addressed**: float outputs to R32Uint / RG32Uint targets (170 + 27 a frame, above); the split64 shader at
  36,336 B; the compiling and guarded classes (compile throughput is the cache worker's).

### 5. Frames/s, ms per submission, footprint (measured)

| | frames/s | ms per submission: heaps / shadows / textures / replay (max) | footprint | Metal (layer) by category, MiB |
|---|---:|---|---:|---|
| d3m-s7-e, world drawn (+92..+102 s, load 14-17) | 7.4-19.1 | 4.9-9.1 / 1.4-2.1 / 1.2-2.7 / 0.8-2.0 (64-197) | 13,232 (+94 s) | 4,837: title copies 948, render targets 2,090, descriptors/arena 416, buffers 41, other 542 |
| d3m-s7-e, autosave page (+107..+268 s) | 30.0 | 1.6-2.1 / 1.4-1.6 / 2.0 / 0.3 | 14,702 (+245 s) | 5,077 |
| d3m-s7-j, world drawn (+200..+210 s, load 4-7) | 1.45-1.50 | 3.9-4.3 / 1.0 / 0.9-1.0 / 0.5 (91-95) | 14,431 (+247 s) | 5,283: title copies 1,121, render targets 2,087, descriptors/arena 576, buffers 137, other 563 |
| d3m-s7-i, world drawn, hooks off | 1.1-3.0 | | 16,562 (+240 s, cap) | 6,385: title copies 1,655, render targets 2,104, descriptors/arena 576, buffers 647, other 595 |

Besides the Metal allocations: the title's pool 4,679 MiB committed (4,412 touched), imported title pages 777-791 MiB
read in place, deferred title textures 3.2-4.3 GiB without a Metal texture (title memory), malloc 1.9 GiB, 8
descriptor-table generations (160-320 MiB) since the back-pressure. About 30-40 submissions a second in the world;
at 1.5 frames/s that is ~26 submissions a frame, so the GPU (the guard leaves 980 dispatches a frame) and the gates,
not the CPU replay (0.5 ms), set the rate.

### Switches added this round

`GTA6RT_METAL_BVH_HOOKS=0`, `GTA6RT_METAL_DROP_HEAP_WRITES=1`, `GTA6RT_METAL_EXTERNAL_RESERVE_VA=0`,
`GTA6RT_METAL_TABLE_GENERATIONS`, `GTA6RT_METAL_PRESENT_SCENE`, `GTA6RT_METAL_SCENE_DUMP`, `GTA6RT_METAL_SCENE_LOG`,
`GTA6RT_METAL_SCENE_SIZE`, `GTA6RT_METAL_SCENE_EXPOSURE`; backend: `GTA6RT_METAL_CB_BUDGET_MS`,
`GTA6RT_METAL_INDIRECT_MESH_GROUPS`, `GTA6RT_METAL_INDIRECT_CS_GROUPS`; launcher `S7_B_AFTER`.

### Next (ranked)

1. GPU safety at the world phase: the queues' waits on the list-values gates (CPU-signalled events) must not outlive
   Metal's watchdog: hold such work on the CPU (QueueHolds) instead of encoding an event wait when the gate is not met at
   submission, or cap a gate's GPU wait below ~1 s (k). Then the heavy world compute (h).
2. The empty G-buffer: with the guard on nothing GPU-driven is drawn (j); narrow the guard to the consumers of the
   dispatches that were actually skipped (or to argument buffers written by them), then dump one indirect mesh draw's
   argument record and the mesh stage's output counts and check the camera constants (h: 1,100+ mesh draws recorded,
   depth untouched).
3. Float outputs to R32Uint / RG32Uint targets (the tiled lighting: 170 + 27 draws a frame): a pixel variant with a
   bit-cast output (shaderc_msl request).
4. The `exe+0x4e9b54a` fault: protect the tail page of the title's 64 KiB chunks in region `0x372b0000` to name the
   writer (late list value writes or clears into reused placed memory are the candidates).
5. Footprint: 14.4-16.6 GiB at 3 minutes; render-target aliasing (-1.1 GiB) and use-based eviction of title copies.

---

## Round 6, menus (2026-11-26, 20:05-23:30): launch movie fixed; BRIGHTNESS grey is the title's; other pages match

Tree `build/d3m-ui-x86_64`; every run is `host/tools/s3_storyload.sh` with `S3_SAVES=empty` (new-game flow: landing,
movie, the five setup pages, legal pages, terms), hidden window, load 4-60 (other workers' titles before and after).

- **Launch movie: shown.** Cause: the movie's draw had no render target on Metal. The title draws it through an
  `R8G8B8A8_UNORM` RTV of an `R8G8B8A8_UNORM_SRGB` target with no castable list; the Metal copy of `CreateRenderTargetView`
  wrote a null view ("No typed-resource format casting") and replay skipped the null slot, so the movie drew into
  nothing and `ps_srgb_write` composed that target's unchanged content (uniform 17/17/17). Upstream fixed the same thing
  at texture creation (`console_format_stand_ins` twin, 16:05+, not in this copy). Fix: `CreateRenderTargetView`
  accepts the UNORM/sRGB twin of a typed RGBA8/BGRA8 render target without a list (`console_twin`; every Metal
  texture already has pixel-format-view usage). Evidence: `run/logs/ui-s3m10.log` ("null RTV for a known resource
  ... DXGI 29 3840x2160 ... view DXGI 28 dimension 4"), `run/logs/ui-s3m11.log` (no null RTV);
  `run/present/ui-rt11/rt_5_VS_Bink_e9aa19fd_Wrapped.png` (the movie's target right after the Bink draw: the opening
  stars); presented `run/present/ui-s3m11/present_001381.png` against MoltenVK `run/present/ui-s3v1/present_001201.png`
  (same shot one frame apart: means 106.1/74.7/55.8 vs 105.9/74.7/55.9, |d| 13.4 from the animation),
  `present_001141.png` vs `ui-s3v1/present_001301.png` |d| 2.6. The shaders, vertex layout, constant buffers and
  planes were fine (checked with the table trace below).
- **BRIGHTNESS background grey 63 is what the title draws.** A `VS_Blit_6e5520d1/PS_BlitColour` rect fills the
  1920x1080 calibration target every frame with its CB colour (0.247059, 0.247059, 0.247059, 1) = 63/255
  (`run/logs/ui-s3m16.log`, CB read through the alias of the V#); the target is 63 before (`ui-s3m12`, 7 dumps) and
  after the calibration draw (`run/present/ui-rt15/rt_0_VS_Blit_f8e5001d_Wrapped.png`). On MoltenVK the same draws read their tables wrong
  (`LEONIDA_LOCAL_DIAGNOSTICS=1` fsdraw lines of `run/logs/ui-s3v2.log`: the GPU copy of the calibration table
  "DIFFER"s from the CPU slot; the BlitColour table reads zeros), so MoltenVK's black is the Vulkan path's defect, not
  the reference. Calibration CB: sRGB constants 12.92/0.4167/1.055/0.055, 0.247059, 0.0196 (5/255 step), 2.4/2.4.
  The symbol is still missing on Metal (its rect stays 63): the two T#s at table +0 / +0x20 (base 0x743f80000 and
  0x743f40000) are not proven bound; not the Vulkan worker's 256x1 T# class (those are at 0x2d8000/0x2ef600).
- **Other pages** (Metal `ui-s3m11` vs MoltenVK `ui-s3v1`/`ui-s3v3`, same binary): account picker |d| 0.04,
  navigation 0.46 (vs `s2-firstrun/present_001441.png`), UI text size 0.49, subtitles 0.45, FINISH SETUP 0.38 and
  LEGAL NOTICES 0.33 (vs `ui-s3v3/present_002101.png`, `_002401.png`), LEGAL TERMS 0.42 (`ui-s3m11/present_004501.png`
  vs `ui-s3v3/present_003701.png`): noise. The light beam MoltenVK showed on FINISH SETUP / LEGAL NOTICES in `ui-s3v1`
  (|d| 4.3 / 12.1) is an animation passing at that moment: `ui-s3v3` has none in its frames either.
- **Diagnostics added** (all off by default): `GTA6RT_METAL_DRAW_TRACE` takes a comma list; `_TRACE_EVERY`,
  `_TRACE_AFTER_S`, `_TRACE_TABLE=<hex offsets>` (the s8 table as the shader reads it, each V# and its memory, through
  the alias inverse when replay has no buffer), `GTA6RT_METAL_DRAW_DUMP_RT=<dir>` (+`_BEFORE=1`: the draw's first
  target as PNG, halved), `GTA6RT_METAL_CLEAR_TRACE=1`; `CreateRenderTargetView` logs the first 16 null RTVs.
  `vsharp_address` counts and logs a V# base that is title memory and also a backed shader VA ("taken as already
  rewritten"); `GTA6RT_METAL_VSHARP_TITLE_FIRST=1` takes such a base as the title's (off: no run showed it wrong).
- `tools/check_prelink.sh`: `rt::xbox_bvh::` (shims_core, upstream 22:32) allowed.

## Round 6 (2026-11-26, 20:05-21:55): the world phase on Metal (S4) — two crashes fixed; a heap-damage SIGSEGV and an FSR4 GPU timeout open; no world frame yet

Host as round 5. Trees: `build/d3m-s4-x86_64` (`-build1..17.log`), ASAN tree `build/d3m-s4-asan-x86_64`
(`-DGTA6RT_D3D12_METAL_ASAN=ON`: this module and port/metal under AddressSanitizer), backend tests
`build/d3m-s4-metal-x86_64`. Launcher (imported saves, A every 5 s, world at +70 s):
`d3d12_metal/tools/s4_world_run.sh` = `host/tools/s3_storyload.sh`'s imported flow with the import done before
the slot is polled and a collision back-off (kills its own title when another started in the same instant). Evidence summary: `run/logs/s4-worldframe-metal.log`.

### What ended the world phase, in the order found

| run | world at | end | cause | fix |
|---|---:|---|---|---|
| d3m-s4-a, -b | - | SIGABRT at 9 s | `[seh] FATAL ... amd_fidelityfx_upscaler_xbox.dll+0x380d` reading 0 after "GPU address arena full; shader-visible heap 5248 KiB": the title's 32-bit reservations (first fit from 8 MiB, 8-439 MiB here; 215-1239 MiB with a 1 GiB arena) sit on the arena's console VAs, which are no mapping of the process, so the arena keeps out of them until it is full | `register_metal.cpp` `reserve_arena_vas`: the arena's VAs [1 MiB, LEONIDA_ARENA_MB) are reserved (no access, no memory) at backend registration, before the title runs (d3m-s4-d: "254 MiB of 1-256 MiB"); `GTA6RT_METAL_ARENA_RESERVE_VA=0` off |
| d3m-s3-a (round 5), d3m-s4-c, -d | +70 s | SIGABRT 5-8 s into the world, no message | `malloc: Incorrect checksum for freed object 0x7f9fd5d96a50: probably modified after being freed` (MallocDebugReport=stderr, d3m-s4-d); the victim allocation in `note_layer_vsharp` (backtrace by the new handler, d3m-s4-c). The writer: list value writes landed by a host store at the VA's number (`list_values::Pump::land` -> `kernel_writes::store`), which reaches the malloc heap: r8-asan-a's ASAN report (WRITE of 8 bytes into a heap block from `Pump::land`); the "title memory" test behind it accepts any private host mapping (`title_memory_probe.cpp`) | `write_arena_value` -> `land_outside_arena`: past the arena, a live guest allocation (`gta6rt_vm_query`) -> as before; an external block's VA -> its memory; else dropped and logged. No SIGABRT since (d3m-s4-e..-i) |
| d3m-s4-e, -f | +70 s | SIGSEGV at 0x10, 5-8 s into the world | in `GraphicsCommandList::Reset` destroying an `IndirectRasterDraw` (backtrace d3m-s4-f): a freed control block; the same heap damage class (the ASAN tree, whose allocator quarantines frees, never hit it: d3m-s4-g/-h/-i ran past it) | open (writer not instrumented: title, shims or os) |
| d3m-s4-g, -h, -i (ASAN tree) | +73-75 s | GPU timeout 13-25 s into the world (`kIOGPUCommandBufferCallbackErrorTimeout`, "GPU time 0 ms", no command buffer ran 250 ms or more before it), then the layer's device-lost exit (code 3) and the title's device-removed int3 | not garbage indirect grids: -i had the record pre-pass and the producer guard (indirect mesh recorded 7 a frame instead of 300) and timed out as -g/-h did. **The FSR4 pass chain**: with every `fspx/` compute PSO refused (`GTA6RT_METAL_REFUSE_CS=fspx/`, d3m-s4-j) the world ran 27 s with no timeout, until the ASAN tree's footprint passed the 16 GiB guard (exit 4). FSR's programs 03/18 were off in -h/-i/-j (`GTA6RT_METAL_SPIRV_FALLBACK=off`), so one of the 25 others (or their sum in one command buffer) runs past the watchdog | open; kept (they bound damage, cost one tiny pre-pass per indirect call): the record pre-pass (`GTA6RT_METAL_INDIRECT_LIMITS`, mesh grids past D3D12's 65,535 per dimension / 2^22 groups and compute grids past 2^26 zeroed) and the producer guard (`GTA6RT_METAL_INDIRECT_GUARD_MS`, default 2000: GPU-driven indirect work skipped while a dispatch was skipped as not ready in the last 2 s) |

All three GPU timeouts were my process's (each run started only with no other gta6rt up); Metal discarded other
processes' work only if they were on the GPU at that moment. No world frame was reached: the frames dumped in the
world phase are black with the loading spinner (`run/present/d3m-s4-j/present_001921.png`, +94 s, FSR refused, so the
upscaled scene is not written either). The story-load screen now shows the loading artwork on Metal (round 5: a dark
screen with a spinner): `run/present/d3m-s4-c/present_006301.png`.

### Release safety (the device-loss lesson)

`port/metal` api v5 (its STATUS): every GPU object's release waits until every command buffer committed or being
encoded on any queue at the release has completed, including a descriptor slot's previous object and its residency
removal; `metal_test_deferred_release` PASS under the debug layer. In the title: "364-4,489 held until the command
buffers before them completed, up to 3,379 waiting at once, 0 command buffers failed" (gpu-releases lines, d3m-s4-c/-e).
The adapter's own time-based release (retired title textures, 3 s) is keyed on queue progress now.

### World draws (census per presented frame, `[rt] d3d12-metal work` every 10 s, new; d3m-s4-h t=84 s, ASAN tree)

| class | per frame | recorded | skipped, why |
|---|---:|---:|---|
| ExecuteIndirect DISPATCH_MESH (new: recorded as indirect mesh draws) | 334 | 300 | Metal refused 26 (threadgroup memory 36,056 / 35,544 / 34,008 B > 32 KiB), PSO refused 3.5 (slot 55), library pending 1.5, compiling 2.7 |
| indirect mesh, split64 route (new) | 1.1 | 0 | compiling 0.6, Metal refused 0.6 |
| direct dispatch / indirect dispatch | 51 / 63 | 28 / 13 | compiling 23 / 51 |
| indirect rect-list (new: backend pre-pass) | 19 | 0.6 | compiling 18 |
| indirect draw indexed / draw | 5.3 / 0.4 | 2.5 / 0.1 | slot-55 refused 1.6, library pending 0.7 |
| draw / draw indexed / draw rect-list | 3.7 / 5.5 / 0.9 | 1.3 / 4.7 / 0.7 | compiling |

Before the world (landing, menus, loading): 10 draws, 1 rect-list draw, 1 dispatch a frame, all recorded. Mesh PSOs at
creation: hw 24,693, split64 1,174 (all created now; were refused), 413 refused with no reason (slot 55), 15
conservative rasterization. Not seen in any world frame: acceleration-structure builds (ray tracing off), the
"two formats" texture copy (now done through a staging buffer when the block size matches).

### Memory (d3m-s4-e +63 s, story load / d3m-s4-g +95 s, world)

| | round 5 world (d3m-s3-a +213 s) | this round, world (d3m-s4-g +95 s, ASAN tree) |
|---|---:|---:|
| footprint | 13,368 MiB | 14,480 MiB (ASAN tree; the plain tree died before a world report) |
| Metal (layer) | 6,429 | 4,287 |
| title copies | 2,834 (33,535) | 793 (19,794): stale copies retired (merged `retire_overlapped_title_textures`: 4,409 images, 191 MiB by +63 s) |
| render targets | 2,135 (1,182) | 2,135 (1,182): unchanged, see the plan |
| descriptors/arena + hash table | 464 + 80 | 347 (arena 256 + 8 live table generations); d3m-s4-j +94 s: 537, of it **14 live hash-table generations, 281 MiB** (73 made, 854,022 slot writes) |

The three memory-map findings on this path: (1) render targets 2,135-2,140 MiB in 1,182-1,184 textures, unchanged
(each a private MTLTexture; aliasing needs the title's page -> pool slot map, see Next); (2) stale copies: retired now
(the plain tree's title copies at +63 s 222-235 MiB in 13,535; the ASAN tree's world 793-1,002 MiB against round 5's
2,834); (3) the bindless table: set 0's 28 argument buffers are fixed (65,536 2D slots = 512 KiB, the rest 2-32 KiB),
but the descriptor hash table (binding 2) is a full copy per generation, about 20 MiB each at the world phase and up
to 14 alive while submissions are slow (a generation is freed only when the queues passed it and 100 ms later); with
fnv1a it grows as on the Vulkan path (fmix32 retranslation, spike/r6-memory-map). World footprint: 14,985 MiB at
+94 s in the ASAN tree (ASAN adds roughly 1-1.5 GiB); the plain tree's world report did not come before its crash.

### Switches added this round

`GTA6RT_METAL_CRASH_BACKTRACE` (SIGABRT/terminate/near-null host faults print a stack), `GTA6RT_METAL_CRASH_MEMGRAPH`,
`GTA6RT_METAL_STACK_LOG_AT_S`, `GTA6RT_METAL_ARENA_RESERVE_VA`, `GTA6RT_METAL_MESH_SPLIT`, `GTA6RT_METAL_DUAL_SOURCE`,
`GTA6RT_METAL_SPIRV_FALLBACK`, `GTA6RT_METAL_WORK_REPORT_S`, `GTA6RT_METAL_INDIRECT_LIMITS`,
`GTA6RT_METAL_INDIRECT_GUARD_MS`, `GTA6RT_METAL_REFUSE_CS`, `GTA6RT_METAL_RELEASE_AT_ONCE` (backend),
`LEONIDA_TITLE_OVERLAP_RETIRE`; CMake `-DGTA6RT_D3D12_METAL_ASAN=ON`.

### Next (ranked)

1. The FSR4 timeout: find the pass (bisect `GTA6RT_METAL_REFUSE_CS` over the 25 `fspx/` programs, one run per step,
   alone on the machine) and whether it is one pass or the chain in one command buffer (split the command buffer
   after each FSR dispatch); then either fix the translation or replace the upscaler on Metal (spike/r6-fsr4 routes b
   and MetalFX). With FSR refused the world runs but the scene the title composes from FSR's output stays black.
2. The heap damage behind the Reset SIGSEGV (plain tree only): `MallocScribble=1` + `GTA6RT_METAL_STACK_LOG_AT_S` +
   the crash hooks here (malloc_history of the reported block hung in d3m-s4-d; a memgraph is the fallback).
3. Mesh shaders over 32 KiB of threadgroup memory (26 records a frame refused): shaderc_msl's x_buf/shrink transform
   for mesh stages.
4. Render-target aliasing (2.1 GiB for ~0.65 GiB of title pages): needs xmem's page -> pool slot (request).

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
