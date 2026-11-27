# port/d3d12_metal/upscale — the title's upscaler served by the host (round 7)

Date 2026-11-26, 22:40-00:00. Host: Apple M4 Pro (24 GB), macOS 27.0, x86_64 title process (Rosetta). Tree
`build/up7-x86_64` (the adapter's super-build; logs `build/up7-x86_64-build{1,2,3}.log`), offline harness
`build/up7-offline`. Title runs `run/logs/up7-*.log` through `tools/up_run.sh` (private run directory reset from
`build/up7-x86_64/rundir-template` before every run, private pipeline archive; the shared `run/saves` and
`run/overlay` were never written). Load average 6-14 during the runs, 3-5 other workers alternating in the gta6rt
slot. Measured = printed by a log, a tool or the harness; inferred = reasoning on top.

## Result

- **FSR4 is replaced on the Metal path, by default.** At D3D12 device creation the five
  `amd_fidelityfx_loader_xbox.dll` exports are patched to host functions; the FSR4 DLL is never loaded, no `fspx/` PSO
  is created or dispatched. Each frame's `ffxDispatch` becomes a host stage in the title's command stream:
  MetalFX temporal (default), MetalFX spatial or bilinear (`GTA6RT_UPSCALER=fsr4|metalfx-temporal|metalfx-spatial|
  bilinear`; `fsr4` is the old path, unchanged).
- **In the title** (6 world-phase runs, up7-a..f): 0 GPU timeouts; three runs stayed 120 s in the world and ended on
  the footprint cap or the watchdog (round 6: GPU timeout 13-25 s into the world with the FSR4 PSOs live), two ended
  on a title fault that also ends runs with FSR4 (exe+0x4e9b54a, section 3), one went on to the autosave page; every frame the title dispatched was upscaled (up7-c: 251 of 253,
  the 2 others still in flight at exit; 0 failed); MetalFX temporal 0.92-1.14 ms GPU a frame (p50), bilinear
  0.07 ms, spatial see table.
- **The picture is still black, and not because of the upscaler**: the 1280x720 HDR colour the title hands to the
  upscaler is **exactly zero** in every frame dumped (up7-c frames 20-260; up7-b frame 40), as r8-world measured on
  the Vulkan path. The scene is not drawn into it yet on Metal (the world's pipelines are still compiling: up7-a
  t=184 s, 146.7 of 154.2 draws a frame and 827.9 of 2,597 indirect mesh draws skipped as compiling). The upscaled
  output is therefore zero too, and the presented frame is the title's UI over black.
- **New screen on the Metal path**: the opening cutscene's credits and subtitles in the world phase
  (`/Users/a/Desktop/Projects/GTA-VI/port/run/present/up7-b/present_001981.png`, +152 s: "Character Art Direction",
  "Really? We gotta run these plates."). Round 6 never got past a black frame with the spinner in the world. Wrong in
  it: the 3D scene under the UI is black (the colour input above is zero). And the title's **COULDN'T AUTOSAVE** page,
  first time on Metal (`/Users/a/Desktop/Projects/GTA-VI/port/run/present/up7-f/present_002701.png`; the body text is
  missing most glyphs, the heading and buttons are fine; the save went to the private run directory's provider).
- **Conventions proved offline** (synthetic moving-camera sequence in the title's exact conventions): MetalFX
  `jitterOffset = -(FFX dispatch jitter)` on both axes, motion vectors unchanged (scale 1, no y flip). Best PSNR in all
  4 camera motions, by 1.7-5 dB over the next sign choice (section 4).

## 1. The interface (measured: disassembly of the packaged DLLs and the exe's call sites, read-only)

`amd_fidelityfx_loader_xbox.dll` (13,312 B) exports, RVAs: `ffxConfigure` 0x1310, `ffxCreateContext` 0x1170,
`ffxDestroyContext` 0x1290, `ffxDispatch` 0x1520, `ffxQuery` 0x1420. The exe imports four of them (no
`ffxConfigure`; IAT at RVA 0x6793fc8, jump thunks at exe+0x5f3ff90/ffa0/ffb0/ffc0) and calls them from six sites:

| site | call | what the title passes (and reads back) |
|---|---|---|
| exe+0x28ef2cc | `ffxCreateContext(&ctx at obj+0x128, desc, NULL)` | chain `0x10000` (upscale: flags, maxRender 1280x720, maxUpscale 1920x1080 from obj+0xf8..0x104, fpMessage NULL) -> `0x1000002` (backend: the device, global 0x148759d00) -> `0x1000b` (version 0x1001001 = API 4.1.1). Flags = obj+0x145 HDR, +0x147 << 3 depth inverted, +0x148 << 4 depth infinite, +0x146 << 5 auto exposure; measured in the title: **0x29** (HDR, inverted depth, auto exposure). Return value ignored. |
| exe+0x28ef30f | `ffxQuery` `0x10004` jitter phase count (render width, display width) -> obj+0x10c | 18 for 1280 -> 1920 (8 x 1.5^2). The title divides by it every frame (exe+0x28f03e8): it must be answered. |
| exe+0x28ef625 | `ffxQuery` `0x10005` jitter offset (index obj+0x110[i], phase count) -> x, y | stored, then the projection gets (2x, -2y) (constant -2.0 at 0x14614f804; per render pixel), i.e. FSR's convention. An alternative sequence (R2, constants 0.7549/0.5698) is used when obj+0x149 == 1. |
| exe+0x28f03db | `ffxDispatch` `0x10001` (desc at rsp+0x100, 432 B) | below |
| exe+0x28f0651 | `ffxQuery` `0x10008` GPU memory usage -> adds `.totalUsageInBytes` to a counter | answered 32 MiB |
| exe+0x28ef512 | `ffxDestroyContext(&ctx, NULL)` | |

Dispatch description (`ffxDispatchDescUpscale`, offsets checked by `static_assert` in `src/ffx_types.h` against the
call site): `commandList` +0x10 = the title's context list (obj[0x127c8]); `FfxApiResource` (48 B each: the
`ID3D12Resource*`, a description from `GetDesc`, a state) for colour +0x18, depth +0x48, motion +0x78, exposure +0xa8
(none: auto exposure), reactive +0xd8 and transparency +0x108 (both none in every frame seen), output +0x138;
`jitterOffset` +0x168 = **-(the queried jitter)** (xor with -0.0 at exe+0x28f0114); `motionVectorScale` +0x170 =
(1.0, 1.0) (pixel units, render resolution); `renderSize`/`upscaleSize` +0x178/+0x180 from the colour and output
textures; `enableSharpening` +0x188 = sharpness > 0 (0 in every frame seen); `frameTimeDelta` +0x190; `preExposure`
1.0; `reset` +0x198; `cameraNear/Far` +0x19c/+0x1a0 swapped when depth is inverted (seen: 10000 / 0.1-0.2);
`fovY` +0x1a4 (0.36-0.66 rad seen); `viewSpaceToMeters` 0; `flags` +0x1ac = 0.

Resource states (FFX's contract: a resource is left in the state it came in): colour, depth, motion, exposure
`0xc` (PIXEL_COMPUTE_READ); output, reactive, transparency `0x2` (UNORDERED_ACCESS). Measured in the title:
colour 1280x720 `MTLPixelFormatRGBA16Float`, depth `Depth32Float_Stencil8` (the layer's D32S8 storage), motion
`RG16Float`, output 1920x1080 `MTLPixelFormatRGB9E5Float` (93) with usage read|write|render target|format view.
Failure: FfxApiReturnCodes (0 ok, 2 unknown desc type, 4 no provider, 6 parameter); the title checks none of them,
so a failed create or query leaves its outputs as they were (the phase count then divides by garbage).

## 2. What is implemented (files)

- `src/ffx_types.h`: the FFX structs used, with layout asserts; FSR's Halton(2,3) jitter and phase count.
- `src/ffx_exports.cpp` (backend-neutral): the five exports as `GTA6RT_GUEST_ABI` host functions behind
  `gta6rt_isa_make_thunk` thunks, patched over the loader's export entries (`mov rax, thunk; jmp rax`,
  `gta6rt_isa_patch_code`) at device creation, before the title can call them. Contexts are host objects; queries
  0x10002/3/4/5/8 answered, others refused with 2 (logged); `ffxDispatch` of another type refused and logged.
  The upscale dispatch stores the frame (ResourceIds via `rt::d3d12::resource_id`, sizes, jitter, flags) in a
  256-entry ring and records `SetMarker(1, "gta6rt-upscale#<frame>")` on the real list behind the title's list proxy.
- `src/metal_upscaler.{h,mm}`: MetalFX temporal (auto exposure, reversed depth, jitter and motion mapped as measured;
  depth that MetalFX does not take, here D32S8, is copied to R32Float first), MetalFX spatial (HDR colour mode) and a
  bilinear compute kernel; an intermediate output plus a compute copy when the title's output lacks the usage MetalFX
  wants (not needed in the title: the output has render-target and write usage). PNG readback for the dumps.
- `src/metal_stage.mm`: the stage in the command stream (below), the per-frame time budget, statistics, dumps.
- Hooks (announced in `run/requests/d3d12_metal.md` 22:50, applied 23:14): `d3d12_metal.cmake` one include;
  `Device::Device` one `gta6rt::upscale::install(*state_.gpu)`; `metal_replay.cpp` `record(const DebugMarker&)` set
  case + `Replay::upscale_stage` (6 lines).
- `tests/upscale_offline.mm`: the offline proof and cost harness (section 4). `tools/up_run.sh`: the run launcher.

### Stage in the command stream (GPU hazards and releases)

At the marker the replay records, into the title's own `gpu::CommandList`: a UAV barrier (all), `write_value(gate[n %
512], n)`, `wait_value(done[n % 512] >= n)` (port/metal DESIGN 3.3 and 5.2: the barrier joins every earlier encoder,
the write is a blit after it, the wait splits the command buffer and gates the rest on a CPU-released event). The
stage's thread sees the gate value in shared memory (every earlier encoder of that queue has finished), encodes the
upscale into a command buffer of its own `MTLCommandQueue` with the `id<MTLTexture>`s taken at replay (retained by
the job and by Metal), and stores `done = n` from that command buffer's completed handler, failed or not. The title's
following work starts only after that (the encoder after a wait joins). So no title work runs concurrently with the
upscale on either side, no state transition is needed (Metal has none; the FFX states are left as they were), and
no GPU wait depends on another GPU wait. Releases: the post-split command buffer is committed and waiting while the
upscale runs, so port/metal v5 holds any release of those textures until it completed; the stage's own textures
are released by ARC after its command buffer.
Safety: per-frame GPU budget `GTA6RT_UPSCALE_BUDGET_MS` (8 ms) logs `over budget: <command buffer label> ran X ms`;
a stage command buffer not completed after 2 s logs `CULPRIT: command buffer gta6rt-upscale n ... has run ...`; a
gate not reached in 30 s is released without upscaling (logged); a mode MetalFX refuses falls back to bilinear for
the rest of the run (logged). Cost of the split: `commit->done` 0.25-0.4 ms (bilinear) and 2.4-2.6 ms (temporal)
per frame, measured from commit to the CPU store.

## 3. Measurements in the title

| run | mode | world at | in the world | end | GPU ms/frame p50 (p95, max) | presented fps in the world | footprint peak |
|---|---|---:|---:|---|---|---:|---:|
| up7-a | metalfx-temporal | +69.8 s | 122 s | footprint cap (exit 4) +192 s | 1.03-1.13 (1.2-2.4, first frame 13.1) | 1.2-1.6 | 16,341 MiB |
| up7-b | metalfx-temporal | +69.8 s | 120 s | footprint cap (exit 4) | 0.92-1.14 (1.1-7.4) | 0.9-1.8 | 15,722 MiB |
| up7-c | bilinear | +69.9 s | 120 s | watchdog (rc 124) | 0.069-0.093 (0.08-0.14) | 1.6-3.2 | 13,603 MiB |
| up7-d | metalfx-spatial | +70 s | 15 s | title fault exe+0x4e9b54a reading 0xc (not the stage: the same fault ended d3m-s7-a, which ran FSR4) | (too few frames) | | 10,562 MiB |
| up7-e | metalfx-temporal | +69.9 s | 3 s | title fault exe+0x4e9b54a at +72 s (newer live sources, build3) | (no frame) | | 10,616 MiB |
| up7-f | metalfx-spatial | +69.3 s | 11 FFX frames, then the title's COULDN'T AUTOSAVE page (no more FFX dispatches, 30 fps of UI) | killed by me at +735 s: the process stood still from +115 s to +709 s (no submission; machine load 43) and run.sh's 205 s timer did not end it | 0.266 (1.18) | 13.6 (page) | 14,734 MiB |

GPU timeouts: 0 in every run. FSR4's cost was not measured (section 5). The temporal runs presented fewer frames
and reached a higher footprint than the bilinear one; the stage's own GPU time (1 ms) and memory (113 MiB, offline)
do not explain 0.6-1 fps or 2-3 GiB; the replay -> gate latency (the title's GPU work before the stage) was 0.8-1.4 s
in the temporal runs and 0.8-1 ms in the bilinear one, so the title's queue was behind in those runs (inferred: page-in
under 6-7 GiB compressed memory; the runs had different builds of other workers' code and different machine load).
The up7-f stall is not the stage's: at +115 s every gate had been released (dispatches 11, ran 11, 0 pending) and
the title was on a menu page; its cause (the whole process, run.sh's timer included, not running for 594 s at load
43) is unknown.

Memory (offline, `upscale_offline --mode memory`, device allocation after the first frame, RG11B10F output): bilinear
+0 MiB, MetalFX spatial +12.6 MiB, MetalFX temporal +113 MiB; plus the stage's depth copy (R32Float 1280x720,
3.5 MiB) for temporal, `scratch 3840 KiB` in the logs.

Before/after pair (render-resolution input vs upscaled output, as the stage read them back, tone-mapped x/(1+x)):
- in the title (black, see Result): `/Users/a/Desktop/Projects/GTA-VI/port/build/up7-x86_64/dumps/up7-b/upscale-000040-input.png`
  and `/Users/a/Desktop/Projects/GTA-VI/port/build/up7-x86_64/dumps/up7-b/upscale-000040-output.png`
  (up7-c logs the statistics: 0.0000 of the texels non-zero, max 0, in every dump).
- offline, frame 63 of the moving sequence: `/Users/a/Desktop/Projects/GTA-VI/port/build/up7-offline/images/offline-input.png`
  -> `/Users/a/Desktop/Projects/GTA-VI/port/build/up7-offline/images/offline-temporal_j_m1_m1__mv_p1_p1_.png`
  (truth `offline-truth.png`, bilinear `offline-bilinear.png`, spatial `offline-metalfxmspatial.png` beside them).

## 4. Offline proof of the conventions and image quality (measured)

`tests/upscale_offline.mm` renders what the title renders, in its conventions: 1280x720 RGBA16F HDR colour (a
3-pixel checker, rings and HDR grid lines of 3.5, plus a spoked disk moving on its own path), inverted depth (R32F),
motion vectors = previous minus current position in render pixels, unjittered; jitter from the stage's own `ffxQuery`
(Halton 2,3, 18 phases), the projection offset by +j and the dispatch given -j, exactly as exe+0x28f0114 does. Truth:
the same scene at 1920x1080, 4x4 supersampled per output pixel. 64 frames; PSNR over the last 16 (tone-mapped
x/(1+x), 8-pixel border excluded); GPU time from the command buffers (`build/up7-offline/sweep-*.txt`).

| camera px/frame | MetalFX jitter = -dispatch, mv x1 | +dispatch | x only flipped | y only flipped | mv negated | mv y flipped |
|---|---:|---:|---:|---:|---:|---:|
| (2.3, -1.1) | **25.04** | 21.76 | 23.32 | 23.20 | 17.82 | 16.39 |
| (0, 0) (disk moving) | **30.27** | 25.22 | 27.12 | 27.05 | 30.19 | 30.17 |
| (5.7, 3.1) | **24.53** | 21.69 | 22.85 | 23.14 | 18.46 | 17.22 |
| (-1.3, 0.4) | **25.29** | 21.70 | 23.30 | 23.48 | 17.35 | 16.66 |

Same sequence, other modes: bilinear 20.87 dB, 0.11 ms; MetalFX spatial 17.71 dB, 0.30 ms (it sharpens, which
PSNR against a box-filtered truth punishes); MetalFX temporal 25.04 dB, 0.82 ms. MetalFX temporal in this x86_64
process prints an `appleneuralengine ... Bad argument` error at creation and then works (not investigated).

## 5. The FSR4 timeout (diagnosis, partial)

Not run again (rule: no repeat of a run that timed out until something addressed it; with the replacement there is
no reason to). What the round-6 logs show (measured):
- On Metal the chain never had all its passes: `fspx/amd_fidelityfx_upscaler_xbox_03` and `_18` (PASS_4, PASS_24)
  are not in the Metal shader store, and the on-demand build failed ("no SPIR-V at
  spike/s2-make-game/work/shaders/spirv/fspx/amd_fidelityfx_upscaler_xbox_03.spv", d3m-s4-h l.2374): spike/r6-fsr4's two new modules went into
  the Vulkan corpus only. So 25 of 27 network passes ran, the two missing ones leaving their outputs as whatever the
  buffers held: the network then reads garbage (a translation-side gap, not a slow GPU).
- No command buffer ran 250 ms or more before the timeouts, and the failed command buffers report "GPU time 0 ms";
  the chain ran 8-12 s of world frames before the first timeout. So one FSR4 frame cost well under the watchdog
  (inferred bound: < 250 ms a submission), i.e. not "simply too slow"; the timeouts came with the layer's list-value
  gates timing out ("the releaser has been inside one gate Signal ... the work the layer held behind the gate has not
  run"), which also happened in the FSR-refused run d3m-s4-j without a timeout.
- Not done: per-pass timing on a captured frame offline. The network passes take every input through 64-bit
  addresses in the push block (no descriptor tables: `plan.textures` empty, `table_loads`), so a faithful replay needs
  the frame's arena and console heap contents captured; a zero-input replay would time arithmetic only, with unknown
  grid sizes (the DLL computes them). Next step if FSR4 is wanted on Metal: install 03/18 into the Metal store
  (`shaderc_msl.py` on spike/r6-fsr4/corpus/fspx/*.spv), then capture one frame's FSR inputs at the marker point
  (the stage already sits there) and time the 29 passes offline.
- FSR4 stays selectable (`GTA6RT_UPSCALER=fsr4`); it is not the default, and with 03/18 missing it cannot be right.

## 6. Vulkan path, port/metal

- `run/requests/d3d12_vulkan-upscale.md`: what port/d3d12_vulkan would need for the bilinear/spatial stage
  (build `ffx_exports.cpp`, one `install` at device creation, a `vkCmdBlitImage` or FSR1 EASU at the marker in
  `vulkan_replay.h`). Not edited. On that path the input is zero as well (r8-world), so it would show nothing yet.
- `run/requests/metal-upscale.md`: the stage relies on `MetalTexture::mtl()` (it includes `port/metal/src/
  mtl_internal.h` read-only); requested `native_handle()` and `CommandList::encode_native` so the upscale can run in
  the title's own command buffer without the CPU round trip (2.5 ms a frame for temporal today).

## Next

1. The scene colour is zero before the upscaler: find which pass should write the 1280x720 RGBA16F target the title
   hands to FFX (colour ResourceId logged by `[upscale] ffxDispatch`) and why it does not (pipelines compiling, the
   S4 worker's scene probe). Nothing more to do in the upscaler for a picture.
2. The temporal runs' lower frame rate and higher footprint: one more A/B on the same build at the same load.
3. Sharpening (FFX `enableSharpening`, RCAS) is ignored: the title never asked for it in the frames seen.

## Switches

`GTA6RT_UPSCALER` (above); `GTA6RT_UPSCALE_BUDGET_MS` (8); `GTA6RT_UPSCALE_DUMP=DIR,FIRST[,EVERY]` (input/output PNG
pairs of the stage's frame FIRST and every EVERY after, with statistics of colour, output, depth and motion in the log);
`GTA6RT_UPSCALE_JITTER_SX/SY`, `GTA6RT_UPSCALE_MOTION_SX/SY` (sign overrides, for the sweep only).
Rebuild the harness: `clang++ -std=c++20 -O2 -arch x86_64 -fobjc-arc tests/upscale_offline.mm src/metal_upscaler.mm
-framework Metal -framework MetalFX -framework Foundation -framework ImageIO -framework CoreGraphics -o
../../build/up7-offline/upscale_offline`; `--mode all|bilinear|metalfx-spatial|metalfx-temporal|memory`,
`--speed DX DY`, `--frames N`, `--out DIR`.
