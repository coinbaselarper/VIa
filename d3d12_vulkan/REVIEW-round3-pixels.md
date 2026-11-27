# Review, round 3: first pixels on the Vulkan path (port/d3d12_vulkan, port/shaders_vk)

Reviewer run 2026-11-26. Verdict: **APPROVED WITH FIXES**. The headline result reproduces: both Apple Vulkan drivers
build the title's first pipelines, submit its draws and present its first UI screen. Three claims in the builder's
report/STATUS are wrong or unsupported (blocking as false claims, B1-B3); no crash or silent corruption of the boot
path was found.

Nothing outside this file was changed. Reviewer outputs: `port/build/review-pixels-x86_64{,-*.log}`,
`port/run/logs/review-pixels-*.log`, `port/run/present/review-pixels-*`, `port/run/pipeline_cache-review/`.
Probes and scans are in the reviewer's scratch area only.

## What was built and run

| item | result |
|---|---|
| Host, x86_64, all modules, `build/review-pixels-x86_64` (-j3) | builds, 0 errors. It only links with `-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=host/src/win/host_overlay.cmake`: the plain `host/CMakeLists.txt` currently lists `src/window_shim.cpp`, whose `host::win` sources are not in the host target (undefined `host_win_*`, `MessageQueue::*`). Not this module's fault, but STATUS.md's host build command (no overlay) does not link today. |
| `d3d12vk_register_test` x86_64 | PASS |
| `d3d12vk_present_clear_test` x86_64 under Rosetta, MoltenVK x86_64 and KosmicKrisp x86_64 | PASS, PASS (120 frames, clear colour read back). The builder only ran these on arm64. |
| arm64 module tree | **not built**: it pulls in the vendored FEX build (several minutes of 3-job compiles), which I stopped. Instead the four changed TUs (`register.cpp`, `title_memory_probe.cpp`, `d3d12_com.cpp`, `d3d12_vulkan_device.cpp`) were compiled `-fsyntax-only` for arm64 with the x86_64 tree's flags: 0 errors. |
| Real title, MoltenVK 1.4.2 x86_64, via `run_vk.sh`, 90 s | 2 runs (`review-pixels-mvk1`, `-mvk2`) + 1 traced 45 s run |
| Real title, KosmicKrisp 26.3.1 x86_64 | 2 runs (`review-pixels-kk1`, `-kk2`) |

One host at a time throughout (another worker's `spike/r4-pso-census` host was running twice; I waited for it each time).
Machine load average was 16-44 during my runs.

## Reproduced

- **Pixels.** `run/present/review-pixels-mvk1/first_lit_000251.png` and `review-pixels-kk1/first_lit_000251.png`: 1920x1080,
  2,071,502 lit texels, 338 / 337 colours, flat dark background and "ACCOUNT PICKER (Y) / SCREEN NARRATION (OFF) (X) /
  PLAYER (A)" bottom right (viewed). Same picture as the builder's.
  Note the screen is a flat clear colour plus text; "first non-black frame" counts any texel with non-zero RGB.
- **Counts.** PSO totals identical to the builder's in every run: graphics 55,684 ok / 16,335 failed, compute 2,675 ok /
  28 failed, float-controls refusals 0, failed submissions 0 (MVK 995 and 957 submissions, KK 615 and 576), graphics
  Vulkan pipelines 2, compute 159-285 built / 0 failed, `worker_builds` = all builds, longest submission 2.04-2.12 s,
  title watchdog never fired, no crash on either driver (KosmicKrisp with host-memory import off).
- **Corpus.** `apple/spirv`: 29,277 modules (26,503 + 12 + 27 + 52 + 2,683), all SPIR-V 1.5 and all declare only
  `SignedZeroInfNanPreserve` / `SignedZeroInfNanPreserve 32` (0 modules with DenormPreserve, 0 with SZINP 64, 5 with
  Float64, 0 with Float16). 250 random modules pass `spirv-val --target-env vulkan1.2`. Index `float_controls` is
  uniformly `[shaderSignedZeroInfNanPreserveFloat32]` in all five index files. 1,500 random non-VS modules: the
  make_game module with the three float-controls lines removed is byte-identical to the shipped one (0 differ).
  ~85% of a 914-module sample use `UnpackHalf2x16`, 0 use `OpQuantizeToF16` / `PackHalf2x16` / `OpFConvert`.
- **Float-controls probe** (`fc_vulkan_probe` on the Homebrew arm64 drivers): identical to the builder's text. none:
  MoltenVK 5/16 f32 checks wrong, KosmicKrisp 2/16; apple and full: 0 and 1; f16 subnormal results wrong only for
  `OpQuantizeToF16` (2,046) in all variants. So the "what is lost" statement for f16 denormals holds on these two
  drivers on this GPU. (arm64 drivers; the title uses the x86_64 builds of the same drivers.)
- **Vertex lane probe** (`vs_lane_probe`, Metal): 0 mismatches 1..65,537 vertices. This tests the algorithm in a
  hand-written Metal function, not the translated SPIR-V through SPIRV-Cross; the translated module is only exercised by
  the boot's `exe/container_00` draw. It checks lane identity, not that a "wave64" of vertices is the 64 the title meant
  (the shader is still inexact on a 32-wide simdgroup, as before).
- **A8 stand-in.** `a8_red::remap` extracted into a harness and run on 40 random real pixel shaders with a Location 0
  output: 40/40 rewritten (`OpVectorShuffle %v4float %x %x 3 1 2 3` before the store), 0 refused, 40/40 pass
  `spirv-val`. Blend mapping (alpha factors to colour factors, DST_ALPHA to DST_COLOR, write mask A to R) read and
  consistent. KosmicKrisp boots with it (`A8_UNORM render targets are R8_UNORM images here` logged).
- **Layer review.** `float_controls_check.h` mode/width numbering matches SPIR-V (4459-4463) and Vulkan property order.
  Session cache files are published by rename. Per-PSO builds serialize on `variants_mutex_`. No ABI change on the guest
  side; `gta6rt_d3d12_vulkan_at_exit` is `extern "C"`, called by nothing.

## Blocking (false or unsupported claims)

**B1. "The PSO refusals are 14,993 mesh-shader PSOs and 28 compute shaders" is false.** Counted graphics failures are
16,335, i.e. 1,342 more than the 14,993 mesh refusals, with no reason recorded for them. Far larger and not counted at
all: a traced MoltenVK run (`review-pixels-mvk-trace.log`, 45 s, `--trace-d3d12`) shows
`XboxID3D12Device::CreateNativeGraphicsPipelineState` (slot 57, DeserializeGraphicsPipelineStateX) **ok 13,388, failed
12,062** (47% of native creations), next to `CreatePipelineState` ok 42,464 / unsupported 14,993 / failed 6.
Logged reasons: "no translated graphics stage in fspx_gfx for kind-6 sha256 ..." (kind-6 programs missing from the
corpus: `fspx` holds 27 modules) and E_INVALIDARG with `progA 0 B progB 0 B` (programs in fields the path does not
read; probably native mesh PSOs). Those E_INVALIDARG returns are not in `pso_graphics_failed` nor in the `refused:` list.
Whatever draws with them is silently missing. Not a crash on the first screen, but the report's accounting, and
STATUS.md's "PSO refusals, by reason", must say so and the counters must include the slot-57 failures.

**B2. Provenance of the shipped corpus is misstated.** The report and `shaders_vk/STATUS.md` say 22,742 modules are real
re-translations and 6,535 stripped. The manifest/`assemble.log` of the shipped `apple/spirv` say main: 15,715 + 2,236 VS
re-translated, **8,552 stripped**; the 2,017 modules of the stopped last re-translation were compared against the
stripped ones (identical) but not merged. Shipped: 20,725 re-translated, 8,552 stripped. Function is unaffected (my
byte comparison of 1,500 non-VS modules shows stripped == re-translated), but the number and the manifest disagree.

**B3. Cold/warm pipeline-cache numbers are not reproduced and the cache's benefit is not shown.** Builder: KosmicKrisp
120 cold, 354 warm compute pipelines at 30 s; MoltenVK 102 cold, 871 warm. Mine (load average 16-44, so noisy, but the
same machine state the builder describes as shared):

| run | state | compute built at 30 s / 60 s / end | build seconds | cache hits |
|---|---|---|---:|---:|
| mvk1 | no session file | 24 / 101 / 159 (87 s) | 302 | 0 |
| mvk2 | Metal cache warm from mvk1 | 42 / 240 / 285 (81 s) | 265 | 0 |
| kk1 | no session file | - / 85 / 219 (81 s) | 262 | 6 |
| kk2 | 15 MB session file from kk1 | 26 / 126 / 215 (84 s) | 291 | 204 |

KosmicKrisp with 204 cache hits spent as long compiling as without (about 1.3 s per pipeline, hits or not) and ended
with the same count; 60 s counts are 1.5x. MoltenVK never reads the session file by default, so the file is write-only
there. "3x pipelines at 30 s once warm" is not supported by these runs; at best the cache effect on KosmicKrisp is
unproven, and the MoltenVK benefit is Metal's own cache. The 1-vs-4-thread comparison (+8%, one run each) was not
re-run and is within this noise.

## Non-blocking findings

1. **Watchdog bound is per submission, not per frame.** The 2 s wait budget is reset in each `ExecuteCommandLists`
   (`run_counters::begin_submission`) and the queue mutex is held while waiting. A frame with N submissions that each
   hit an unbuilt pipeline can stall N x 2 s against the title's 30 s watchdog; the NOTES figure of >1,200 new pipelines
   on the first world frame makes that plausible after the first screen. Measured only on the first screen (4 waits,
   3.7 s waited in a 90 s run). Consider a per-frame or total budget.
2. **Latent use-after-free with 4 compiler threads.** `PipelineState::last_release` uses `after_running(owner, ...)`,
   which attaches the deletion to the first running job of the owner. A compute PSO can have a warm-up job and a demand
   job (`queue_build` does not know about the warm-up) running at once on different workers (they serialize on
   `variants_mutex_`); the deletion can then run when the first returns while the second still uses the PSO. Impossible
   with leonida's single worker. Not seen in any run (the title released no PSO mid-build). Fix: key `after_running`
   to all running jobs of the owner.
3. **Session cache loader loses builds.** On KosmicKrisp the loaded cache replaces the live one; pipelines built into the
   old cache meanwhile are not merged, so the next written file lacks them. Minor.
4. **`draws skipped ... (N of them for a pipeline not built yet)` is mislabelled.** `draws_pipeline_pending` also counts
   a draw whose pipeline can never be built (the rect-list blit: `VS_Blit_6e5520d1,PS_BlitColour_6e5520d1 ... was not
   built on the compiler thread` is the only such line; `pipelines not ready at use` is 1-4). The builder's own
   STATUS text attributes them correctly; the counter text does not. Dropped draws: mvk 477-498, kk 286-306 in my runs.
5. **A8 render targets as R8: sampling side not checked.** The shader output, blend and clear are handled; I found no
   component swizzle for SRVs of an `a8_as_red` target (`a8_as_alpha` views use VK A8's native alpha). A shader that
   samples such a target as alpha may read 1.0. Also `SRC_ALPHA_SATURATE` colour factors are not remapped for R8 (reads
   destination alpha as 1). Only KosmicKrisp is affected; not exercised at boot.
6. **Title-memory probe fallback is broad.** A miss accepts any private read/write unshared Mach VM region and adds the
   whole region as title memory. 9 of the probe's misses logged "no guest allocation or writable private mapping";
   the layer also logs "title address ... got no GPU copy: the view is written with no records (loads 0, stores
   dropped)" at boot (the new title-memory path does not cover every V#).
7. **Comment overstates a cause.** The host-import comment says Metal "did not make [the MTLBuffer] in its residency
   set"; STATUS.md says the cause is unproven (it is). Two of my KosmicKrisp runs (import off) did not crash.
8. **Per-submission cost of the descriptor-heap copy.** The log reports 38,912 KiB copied per submission, 19-34 ms on
   average (`console descriptor heap ... (copy)`), most of the 87% "of the wall time" in submit phases. This explains the
   low frame rate more than pipeline creation does; not profiled by the builder.
9. `MoltenVK VK_ERROR_OUT_OF_DEVICE_MEMORY ... host-coherent ... 16384 bytes` appears twice per run, also in mine
   (the builder saw it "twice under load"); the message claims a 14.3 GB limit, so it is not a size limit. Not understood.

## Claims checked and found fair

- Dropping `DenormPreserve 16` loses nothing measurable on these drivers (no f16-typed instruction; `UnpackHalf2x16`
  exact; the only flusher is `OpQuantizeToF16`, used by 0 modules in the sample). `--apple-float-controls` vs plain
  `--no-float-controls` is justified by the MoltenVK NaN/inf failures (5/16 vs 0/16), reproduced.
- Boot-time vertex-subgroup refusal fixed: the title builds its VS pipelines (`exe/container_00`), 0 float-controls and
  0 failed Vulkan pipelines in every run. The 2 CullDistance VS remain, as stated.
- Pipeline builds run on compiler threads in every run; no title thread was held more than ~2.1 s.
- The cache file is written every 30 s (2 writes in each 80-90 s run, 18-30 ms each, 7-15 MB), which is what the
  host's `_exit` needs.
- Tests: x86_64 register and present tests pass.

## For the integrator

1. Build the host with the `host/src/win` overlay (or after the integrator merges `win.cmake`): the plain host CMake
   does not link today.
2. Fix STATUS.md "Per-run counts"/"PSO refusals" per B1, the corpus provenance per B2, and soften the cache claims per
   B3 before it is cited. Count slot-57 failures in `pso_graphics_failed` and log a reason per kind.
3. Nothing calls `gta6rt_d3d12_vulkan_at_exit(2000)`; the 30 s checkpoint is the only cache write on `_exit`/kill.
4. Vulkan runs need `LEONIDA_SHADERS=<port>/build/shaders_vk/apple/spirv`, `LEONIDA_PIPELINE_CACHE_DIR`, and
   `LEONIDA_XMEM_32BIT=anywhere` (`run_vk.sh` sets all three).

## Not done by this review

arm64 module build and arm64 tests; a KosmicKrisp run with host-memory import forced on; `LEONIDA_SESSION_CACHE_LOAD=1`
on MoltenVK (the 39 MB stall claim); the 1-thread vs 4-thread comparison; any run past the first screen (no input);
Bink/movie shaders; the 2 CullDistance VS; the two probes beyond re-running the builder's binaries.
