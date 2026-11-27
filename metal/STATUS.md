# gta6rt_metal status (S5.1, round 2)

## api_version 4 and GPU-error diagnostics (round 5, made by port/d3d12_metal)

Additive over v3 (`port/d3d12_metal/STATUS.md` round 5 has the title runs that use them):

- `CommandList::copy_texture(TextureCopy)`: one subresource region into another texture (blit
  `copyFromTexture:...toTexture:`; 3D slices through `layer`/`depth`). `CommandList::fill_buffer_u32(buffer, offset,
  size, value)`: a blit `fillBuffer` when the four bytes are equal, else the builtin kernel `gta6rt_fill_u32`.
- `create_texture_view` refuses, with an error string, views Metal would abort the process on (seen in the title:
  `MTLPixelFormatBC5_RGUnorm` viewed as `BC7_RGBAUnorm`, d3m-r5-a): a block-compressed texture views only as itself or
  its sRGB twin, and a non-depth view of another format needs the same texel size.
- `create_buffer_texture` passes `usage::pixel_format_view` through (the layer views title textures in place).
- Command buffers are made with `MTLCommandBufferErrorOptionEncoderExecutionStatus` and every encoder is labelled at
  `endEncoding` (`e<n> render|compute|blit x<work> <first pipeline> .. <last pipeline>`): a failed command buffer logs
  each encoder that did not complete (FAULTED / affected / pending, with its debug signposts) and the command buffer's
  encoder summary; a command buffer that ran at least `GTA6RT_METAL_SLOW_CB_MS` (default 1000) on the GPU is logged
  with its summary before it would time out. `GTA6RT_METAL_ENCODER_STATUS=0` turns both off.
- A GPU timeout, "submissions ignored" or device removal sets the device's fatal flag: every later `execute` answers
  `device_lost` at once instead of submitting work Metal discards.
- Test `metal_copy_fill` (`tests/test_copy_fill.cpp`, new): a 3x2 region copied from (2,1) to (4,5) of an 8x8 RGBA8
  texture reads back exactly (every other texel stays 0), and two fills (kernel 0x12345678 over 1 KiB, blit 0x07070707
  over 512 B) leave every word of a 4 KiB buffer right. PASS on x86_64 under `MTL_DEBUG_LAYER=1
  MTL_SHADER_VALIDATION=1` (`build/metal-x86_64/copy_fill-r5.log`); `metal_test_texture_view` (debug layer),
  `metal_test_device` and `metal_test_compute` (debug layer) PASS in the same tree after the v4 build (55 steps, 0
  errors). The rest of the ctest suite was not re-run (other workers' titles were running: its GPU-hang cases need an
  idle machine).

## api_version 3 (round 4, made by port/d3d12_metal)

Additive over v2, for the D3D12 layer on this backend (`port/d3d12_metal/STATUS.md`):

- `Device::create_texture_view(texture, TextureViewDesc, error)` (`newTextureViewWithPixelFormat:textureType:levels:
  slices:swizzle:`), `usage::pixel_format_view`, `Swizzle`, `TextureViewDesc`. A view's `desc()` has the mip-shifted
  size, its mip and layer counts. Writes through `write_texture`/`write_region` on a view land in its parent's storage
  (`MetalTexture::storage()`).
- 27 more DXGI formats in `mtl_formats.mm` (the SINT/SNORM/UINT variants, BC sRGB and signed, BC6H SF16,
  B5G5R5A1), each a native Metal format; multisampled `tex2d_array` textures are `MTLTextureType2DMultisampleArray`
  (the layer's set-0 fallbacks create one; before this Metal's descriptor validation aborted the process).
- Test `metal_texture_view` (`tests/test_texture_view.cpp`): a BGRA8 view of layer 2 / mip 1 of an RGBA8 array reads
  back the 16 texels of that subresource exactly, an array view of layers 1-2, an R32F view of RGBA8, a first mip past
  the texture refused. PASS on x86_64 under `MTL_DEBUG_LAYER=1` (`port/build/d3d12_metal-mtltest-x86_64/tv.log`);
  `metal_test_device` PASS in the same tree. The full ctest suite was not re-run for v3 (its GPU-hang cases need a
  machine without a title running, which this round did not get).

Date 2026-11-26. Host: Apple M4 Pro, macOS 27.0.1, SDK 27.2, Apple clang 21.0.0, CMake 4.5.1, Ninja.
Design: `DESIGN.md`. Plan for driving this from leonida's D3D12 layer: `ADAPTER.md`. Provenance: `ORIGIN.md`. Every
number below was measured on this machine by the tests in `tests/`. Round 2 was built and measured 03:40-05:36 and
re-verified from a fresh configure and clean build 11:10-12:00 (section "Re-verification"); where the two runs differ
both are given. The response to `REVIEW-round2.md` (13:00-15:05) is the last section, "Review response (round 2)";
its final logs are `port/build/metal-<arch>/ctest-r2review-final.log` and `ctest-<arch>-validation-r2review.log`. Logs: `port/build/metal-<arch>/ctest-v3.log` (full verbose ctest, 20 entries, re-verification),
`ctest-<arch>-validation-v3.log` (every test binary under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`, written by
`tools/run_validation.sh`), the earlier `ctest-v2.log` / `ctest-<arch>-validation-v2.log`, and
`port/build/metal-{configure,build}-<arch>.log`. Round 1 sections are kept below unchanged where still true and marked
where round 2 changed them.

## Build

```sh
cmake -S port/metal -B port/build/metal-<arch> -G Ninja -DCMAKE_OSX_ARCHITECTURES=<arch> -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C port/build/metal-<arch> -j3
ctest --test-dir port/build/metal-<arch> --timeout 300      # serial; x86_64 runs under Rosetta one test at a time
```

| arch | clean build steps | wall (-j3), first run / re-verification / review response | warnings | errors | `lipo -archs` (library and 13 tests) |
|---|---:|---:|---:|---:|---|
| arm64 | 52 | 16 s / 11.6 s / 13.6 s | 0 | 0 | arm64 |
| x86_64 | 52 | 14 s / 10.4 s / 15.6 s | 0 | 0 | x86_64 |

- Target `gta6rt_metal` (static library). Public headers: `gpu.h` (`gta6rt::gpu`, **api_version 2**, additive over
  v1), `metal_backend.h` (`create_device`, `create_window`, `d24_quantize_probe_msl`), `rect_list.h`.
- Sources: 13,269 lines after the review response (headers 1,099, `src/` 6,640, tests 4,478 + test shaders 632, tools
  273, CMake 147); round 2 had 12,648. `src/` includes HUD counters another worker added (see the review response).
- The build step `metal_route_a_msl` (tools/route_a_msl.py) now also writes split MSL for 4 corpus compute shaders and 8
  corpus mesh shaders into `build/metal-<arch>/route_a/{compute,mesh}/`, with the S5.2 shape verdict of each mesh
  shader read from `spike/s5.2-msl-corpus` (read only). Nothing derived from game data is in the source tree.
- `metal_test_hostmem` is linked with `-pagezero_size 0x10000` on x86_64 only (the 32-bit heap test); arm64 binaries
  with that flag are killed at launch (exit 137, measured with a probe).

**Integration line for the integrator** (port/CMakeLists.txt, after `add_subdirectory(portable)`), unchanged:

```cmake
add_subdirectory(metal)
```

Re-checked in round 2 and again in the re-verification: a scratch top level (`project(... C CXX ASM)`, a
`gta6rt_build_options` target, `add_subdirectory(<port/metal> metal)`) configured, built 14 steps and ran an executable
that printed the device, api 2, 32,768 B threadgroup memory and the 18,186 MiB working set (arm64; tree
`port/build/metal-subdir-arm64`). port/CMakeLists.txt and port/compat were not edited.

## Test results, round 2 (20 ctest entries; x86_64 under Rosetta, serial; 27 since the review response, below)

| ctest entry | arm64 | x86_64 | every binary under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` |
|---|---|---|---|
| `metal_device`, `metal_present`, `metal_triangle`, `metal_rect_list`, `metal_game_shaders` (round 1) | PASS | PASS | PASS both arches |
| `metal_hazards` (400 iterations), `metal_hazards_validation`, `metal_hazards_table_sets` (round 1) | PASS | PASS | (are validation runs) |
| `metal_hazards_epoch` (new: the case round 1 got wrong, see "Join model fix") | PASS | PASS | PASS both arches |
| `metal_compute` | PASS | PASS | PASS both arches |
| `metal_draw2` (indirect, queries, quads, stencil, blend, depth bias, D24, B4G4R4A4, A8) | PASS | PASS | PASS both arches (timestamp ratio reported, not checked) |
| `metal_mesh` | PASS | PASS | PASS both arches (Metal verdicts reported, not checked) |
| `metal_hostmem`, `metal_hostmem_validation` | PASS | PASS | (second is the validation run) |
| `metal_present2` (visible window) | PASS | PASS | PASS both arches |
| `metal_memory` | PASS | PASS | PASS both arches |
| `metal_pipeline_cache_cold`, `_warm`, `_warm2`, `metal_pipeline_async` (`MTL_SHADER_CACHE_SIZE=0`) | PASS | PASS | PASS both arches since the re-verification (archive hits reported, not checked) |

The table is the final re-verification run (`ctest-v3.log`, `ctest-<arch>-validation-v3.log`). Earlier runs of the
same pass failed three ways, each explained in "Re-verification": `metal_draw2`'s timestamp ratio (a test flaw, fixed),
the pipeline-cache warm phases under validation (archive lookups miss under validation, now reported), and
`metal_hazards` `same_list` (3 of 400 and 1 of 400 iterations read 0 0 0). **Corrected in the review response:** that
failure did recur (the reviewer's run at 12:22), and all three failures were command buffers Metal discarded while
recovering from another process's GPU hang (`kIOGPUCommandBufferCallbackErrorInnocentVictim`, unified log), not a
hazard or residency fault.

`MTL_SHADER_CACHE_SIZE=0` turns off Metal's system shader cache (measured with a probe: without it a second process
built the same pipeline in 0.0006 s instead of 0.08 s; with it set, 0.05-0.06 s every run). The cache tests use it so
"cold" is cold. The system cache lives in `$(getconf DARWIN_USER_CACHE_DIR)com.apple.metal` and is shared by every
process of the user, so it was not deleted.

## Round 2 measured facts (both arches unless one is named; numbers arm64 / x86_64)

### 1. Compute (`tests/test_compute.cpp`, `shaders/compute.metal`, 4 corpus CS)

- Caps: 1,024 threads per threadgroup, 32,768 B threadgroup memory, **no 65,535 cap**: Metal grids are 32-bit per
  dimension (`max_dispatch_groups` 4,294,967,295).
- Grids above 65,535 groups run every group exactly once (atomic mark per group + thread counter), direct:
  131,072 x 1 x 1, 1 x 70,000 x 1, 300 x 300 x 2, 200,000 x 1 x 1 (also 3,000,000 x 1 in a probe). Indirect with
  GPU-written records of 70,000, 5 and 999 groups: 70,005 groups with a GPU-written count of 2, 71,004 without.
  **No split is needed on Metal, and none is done**: a split would also need a group-base offset the translated
  shaders do not read (`SV_GroupID`), so it could not be transparent.
- Console `DispatchX` bit 0x20 (`dispatch_flag::thread_counts`) uses `dispatchThreads` (non-uniform last group):
  `dispatch(1000,1,1,0x20)` with 64-thread groups ran 1,000 threads in 16 groups; without the flag 64,000 threads in
  1,000 groups (the fork's "64 times too many threads"); (100,30,1) ran 3,000 threads in 60 groups; (65,1,1) 65 in 2.
- A chain of 64 dependent dispatches (each reads its neighbour's previous result) with UAV barriers: 0 of 262,144
  elements wrong, in **one** concurrent compute encoder with 63 in-encoder memory barriers. Without barriers
  (negative control) 56,835 / 59,236 wrong, so the encoder really runs dispatches concurrently.
- Threadgroup memory: a kernel with exactly 32 KiB builds and reports 32,768 B; 40 KiB is refused with
  `Status::limit_exceeded`, `limit().kind == threadgroup_memory`, value 40,960, limit 32,768 parsed from Metal's error.
  2,048-thread groups are refused as `threads_per_group` before compiling.
- Corpus compute shaders (split recipe, threads from index.json `local_size`): `.OceanWaveSim_CS_InverseFFT_Y`
  (lds 18,556) builds, reports 18,560 B, threads 1,256,1; `.WaterSimMgr_CS_ClearVelocity` builds. Their reflected
  tables are the index.json bindings minus those without an argument buffer in the MSL (binding 1 / bindings 0 and
  1), with 0 mismatches. `Ivfx_proj_decal_trail_CS_UpdateProjDecalTrailMeshIndices` and
  `Lvfx_proj_decal_hit_detection_CS_ComputeHitDetectionResults` are refused with the value **equal to index.json
  `lds_bytes`** (33,332 and 41,372) after 0.13-0.34 s of compile.
- SPIRV-Cross writes `gl_WorkGroupSize` into the MSL only when the shader reads it (none of the 4 corpus CS have it),
  so the caller must pass `ComputePipelineDesc::threads`; the MSL parse is a fallback.
- Encoding cost: 10,000 one-group dispatches in one list: `execute()` 3.76 ms / 5.66 ms (0.38 / 0.57 us a dispatch).

### 2. Indirect draws, queries, quads, state, D24 (`tests/test_draw2.cpp`, `shaders/draw2.metal`)

- ExecuteIndirect shapes (leonida IR `ExecuteIndirect` kinds draw, draw_indexed, dispatch, dispatch_mesh; strides above
  the packed size; optional count buffer): draw with stride 32, 3 records and a GPU-written count of 2 drew records 0
  and 1 (1,280 px each) and not 2; without the count all 3. The count pre-pass ran **before** the render pass (1
  hoisted, 0 render-pass splits). Indexed indirect, stride 20, per-record base vertex: 3,840 of 3,840 px.
- Occlusion: a 16x16 rect counted 256 samples, the same rect behind the depth buffer 0, a binary query 1. Metal resets
  the counters per render pass (probe: a second pass gave 2,048 again, not 4,096) and leaves untouched offsets
  untouched.
- Timestamps, 1 GHz: two draws with 2x the loop, a UAV barrier between them, after 3 warm-up runs: ratio 1.88-2.03
  in 11 single-run checks (arm64 5, arm64 under validation 3, x86_64 3; e.g. 1.395 / 2.701 ms); the test now takes the
  minima of 7 runs (final: 1.98 arm64, 1.96 x86_64), because GPU time shared with other processes only adds to a
  delta. Under shader validation the ratio is reported only (seen: 1.75 arm64, 1.23 and 1.96 x86_64). **Correction:**
  the first round-2 test had no barrier between the draws, and the earlier "cold run ratios 1.2-1.3: clock ramp-up"
  was not ramp-up: without a barrier D3D12 (and the backend, DESIGN 3.3) lets the two draws overlap, so `t2 - t1` is
  less than the second draw. The re-verification runs of that version failed the ratio check twice (1.33 x86_64,
  1.18 arm64 under validation) and passed once (1.83); the test now records the barrier. All timestamps lie between
  `clock_calibration()` samples taken before and after; `write_timestamp` (console write64 flag 1) lands after the last
  query. Counter samples arrive after their encoder's fence: a resolve in the same command buffer read 0 for the last
  2 of 3 samples, with or without a fence wait; in a following command buffer that waits for the previous one's
  completion, 8 of 8 probe runs were complete. The backend resolves that way (DESIGN 3.8). An empty encoder takes no
  sample, so a timestamp is a one-thread compute encoder (measured).
- Quad lists: non-indexed (shared pattern buffer) 512 of 512 px; indexed, 16-bit indices written by a GPU copy earlier
  in the same list, 10 indices (2 quads + 2 dropped), 2 instances: 1,024 of 1,024 px, expanded on the GPU in a hoisted
  pre-pass.
- Stencil (D24S8 as D32F_S8, and D32F_S8): replace ref 5, incr_sat, then EQUAL 6 over the screen: 256 of 256 px.
- Blend: dual source (one * src0 + src1_color * dst) 128 64 96; blend factor 128 64 255; write mask G-only 128 0 128,
  all exact on every pixel.
- Depth bias on D32F: DepthBias 1000 at z 0.5 gives 0.500059605 (0.5 + 1000 * 2^-24).
- **D24 fragment depth** (the item round 1 left open): with `D24Mode::fragment` the pixel stage writes
  `quantise(z + DepthBias * r + slope term)` (leonida `depth24_fragment.h` rule) and the rasterizer bias is off:
  biased depth, plain depth (0.3 -> 0.299999982), `SV_Depth` 0.7 and a depth-only pipeline all equal leonida's
  `depth24_clear_value` exactly. EQUAL against a cleared 0.3: **4,096 of 4,096** px pass in fragment mode, **0** in
  rasterizer mode (unquantised depth). The GPU quantiser equals leonida's CPU formula on all 16,777,216 codes, 1,048,576
  random values and 8 edge values (0 differ). The round-1 CPU clear quantiser used a double product; it now uses
  leonida's float product (`test_device` reference updated to match).
- B4G4R4A4: Metal's ABGR4Unorm holds R in bits 12-15. Sampling a DXGI texel 0x1234 now gives 34 51 68 17 through a
  swizzle view (exact); as a render target, colour (2,3,4,1)/15 is stored as 0x2341 instead of DXGI 0x1234 (stub).
- A8_UNORM render target at the title's 3840x2160 (re-verification; the format KosmicKrisp refused in S1): mapped to
  `MTLPixelFormatA8Unorm`, not emulated. Clear alpha 0.5 -> 128, a draw of alpha 0.6 -> 153, the same draw blended
  one / inv_src_alpha over 0.5 -> 204: 0 of 8,294,400 texels off by more than 1 in each case, on both arches and
  under validation. A raw-Metal probe (scratchpad, both arches) gave the same: the pipeline builds and the alpha is
  stored. No R8 stand-in is needed on Metal.

### 3. Mesh pipelines (`tests/test_mesh.cpp`, `shaders/mesh.metal`, 8 corpus MS)

- Mesh-only (`draw_mesh(8)`, one quad per group) 512 of 512 px; object + mesh through a payload (2 object groups x 4
  mesh groups) 512 of 512; indirect mesh draws with a count buffer 256 (count 1) / 512 (no count).
- Refusal path: `create_mesh_pipeline` returns `Status::limit_exceeded` with `limit()` = kind, value, limit and a
  reason. With `precheck` (default) the estimate from the MSL (`nv * 16 * ceil(vertex scalars / 4)`, unique scalars
  with the `user(clipN)` copies counted once) refuses in 0.0006-0.0017 s; without it Metal refuses in 0.29-0.47 s per
  corpus shader, so the precheck saves that compile for each of the 4,631 over-limit shaders.
- 8 corpus shaders across the S5.2 shapes (4 / 28 / 64 scalars OK; 38,912 / 63,488 B; 65,536 B + 126 / 125 and
  67,584 B + 129 unique scalars): estimator scalars = the S5.2 counter for 8 of 8, precheck verdict = S5.2 verdict for 8
  of 8, Metal verdict = S5.2 verdict for 8 of 8, Metal's reported sizes and unique-scalar counts match S5.2's.
- A synthetic 30-float4 + clip[1] shape: estimate 65,536 B, Metal 64,512 B (both over the limit; the estimate is
  conservative there).
- Under shader validation the at-limit shape (64 scalars, exactly 32,768 B) is refused for threadgroup memory
  (36,056 B), and 4 corpus shaders end with `XPC_ERROR_CONNECTION_INTERRUPTED` from the compiler service.

### 4. Title graphics memory in place (`tests/test_hostmem.cpp`, `shaders/hostmem.metal`)

- The title's 185 MB 32-bit graphics heap: mapped, then a host alias of the same pages made with `vm_remap`; the
  alias wrapped by `create_host_buffer` (bytesNoCopy, untracked, in the device residency set) and registered at the
  title VA 0x34310000. **x86_64: the heap sits at 0x34310000 below 4 GiB** (with `-pagezero_size 0x10000`) and Metal
  accepts it; arm64: placed anywhere. Wrapping 185 MiB took 0.048-0.090 ms. `resolve_va` maps title VAs into it.
- The constants ring (174 MB at +0xB00000): 60 frames of 600 draws x 28 KiB (16.4 MiB a frame) written by the CPU into
  the title view and read by the GPU through the alias window: 0 wrong words. Per frame: in place 0.246 / 0.363 ms
  execute-to-idle; the copy path (memcpy to an upload buffer + 600 GPU copies to device-local + the same read)
  0.931 / 1.880 ms. **Cost avoided a frame: memcpy 0.26 / 0.73 ms CPU, GPU copies 0.255 / 0.248 ms (timestamps), and
  32.8 MiB of upload + device buffers.** (x86_64 under Rosetta pays the memcpy about 3x.)
- GPU writes into host-owned pages, two dependent dispatches with a UAV barrier: 0 of 1,024 words wrong when read by
  the CPU after the fence.
- Residency and hazard rules of the round-1 review hold: under validation with `GTA6RT_METAL_NO_DEVICE_RESIDENCY=1`
  (negative control, arm64) all 144,000 in-place words and all 1,024 GPU-written words are wrong; with the device set,
  0. Windows are untracked; barriers order them.
- Unaligned windows are refused with a reason (16 KiB pages arm64, 4 KiB x86_64; a 4 KiB-offset pointer was accepted
  by Metal itself on arm64 in a probe, the backend requires page alignment anyway).
- **Ocean read-back** (R32F 256², RG32F 256², RG32F 64², RG16UI 128²; the title's CPU physics reads them): written by
  a compute kernel into linear textures over a host window (`create_buffer_texture`, 16 B row alignment) and read by
  the CPU in place after the fence: 0 wrong texels in 30 frames, 0 bytes copied, 0.152 / 0.157 ms a frame. The copy
  path (device textures + `copy_texture_to_buffer`): 884,736 B copied a frame, 0.168 / 0.217 ms.

### 5. Pipeline performance (`tests/test_pipeline_cache.cpp`; 4 real game pairs, split recipe; `MTL_SHADER_CACHE_SIZE=0`)

| 4 pairs, total | MSL libraries | pipelines | from archive |
|---|---:|---:|---|
| cold, no archive (arm64 / x86_64) | 0.457 / 0.491 s | 0.461 / 0.464 s | 0 of 4 (4 added, 419,344 B, serialized in 0.017 s) |
| warm archive, MSL compiled at run time | 0.579 / 0.437 s | 0.0020 / 0.0057 s | 4 of 4 |
| warm archive, offline metallibs (first time) | 0.0010 / 0.0011 s | 0.479 / 0.445 s | 0 of 4 (metallib functions are different archive entries) |
| warm archive, offline metallibs (second run) | 0.0013 / 0.0009 s | 0.0011 / 0.0012 s | 4 of 4 |

- Per pipeline: cold 0.093-0.149 s, from the archive 0.0001-0.0008 s. Archive load 0.0009-0.0012 s (783,248 B).
- The archive key is "translator version + GPU + OS build + process arch"; a different key starts a new archive.
- Under `MTL_SHADER_VALIDATION=1` (re-verification, arm64): the cold run adds 4 entries (613,297 B, larger than the
  419,344 B without validation) and every later lookup misses: 0 of 4 MSL and 0 of 4 metallib pipelines from the
  archive on both warm runs, pipelines 0.24-1.0 s each. The cache tests report hits instead of checking them under
  validation.
- Off-thread creation never blocks the submitter: deferred MSL libraries (8-12 us to create), an async pipeline
  (33-92 us), a draw recorded and executed while it compiled: `execute()` 0.5-4.0 ms, the draw skipped (1, counted
  in `draws_skipped_not_ready`), the build finished 0.25-0.32 s later on a compile thread, and the next execute drew it.
- "First scene first": 12 pipelines queued at background on a one-thread device, then 2 moved to `first_scene` with
  `precompile`: 0 background pipelines started before them (bound 1 for the snapshot race). Negative control without
  `precompile`: 10 did.

### 6. Present, pacing, read-back (`tests/test_present2.cpp`; visible window, 120 Hz display)

- Two planes: scene 320x180 scaled 2x (point) + a premultiplied UI plane 640x360: 0 of 230,400 drawable pixels off by
  more than 1 from `o.rgb + s.rgb * (1 - o.a)`; the UI plane placed at (320,180)-(640,360): 0 of 230,400. The caller
  decodes the 72-byte plane struct (ADAPTER.md section 3) and passes `PresentPlane`s.
- Pacing: sync interval 1, 120 presents: median on-screen interval 8.333 ms (min 8.333, max 8.334), 119 / 118 shown;
  sync interval 2 (`presentDrawable:afterMinimumDuration:`): 16.667 ms median, min = max = 16.667, 60 of 60 shown.
- Frame events (CADisplayLink from `NSScreen` on its own thread): median period 8.334 / 8.333 ms (re-verification
  arm64: 8.342 ms).
- First-present cost (re-verification, probe in the scratchpad linking the library, arm64, visible window): the first
  `present_planes` with a premultiplied or straight plane built that compose pipeline on the presenting thread,
  13.5 / 13.4 ms against 2.6-9 ms for later presents. Optimised: `create_swapchain` now builds all three compose
  pipelines (19-20 ms with `MTL_SHADER_CACHE_SIZE=0`, was 10.9 ms for one), and the first two-plane present then
  takes 8.9 / 7.6 ms, the same as later ones (7.6-8.9 ms). Those 7.6-8.9 ms are `nextDrawable` waiting for a free
  drawable (3 drawables, presents faster than 120 Hz), not encoding.
- GPU->CPU read-back: section 4 (ocean) and `copy_texture_to_buffer`.

### 7. Memory (`tests/test_memory.cpp`)

- Per-category ledger after allocating 2 buffers, 2 textures, a heap (with a placed buffer), a 65,536-entry table, a
  1,024-query heap and an 8 MiB host window: exactly 5,242,880 / 5,292,032 / 16,777,216 / 524,288 / 8,192 (16,384
  x86_64) / 8,388,608 B, object counts 2/2/1/1/1/1, and back to 0 after freeing. `currentAllocatedSize` grew by the
  GPU categories **plus the bytesNoCopy window** (difference 8,380,416 / 8,388,608 B), so Metal counts host windows.
- Pressure: handlers run on a backend dispatch queue with each level; a toy governor holding 64 MiB of textures dropped
  to 32.5 MiB on warning and 16.2 MiB on critical, visible in the ledger. The real `DISPATCH_SOURCE_TYPE_MEMORYPRESSURE`
  source is registered but did not fire during the tests (levels were simulated). `memory_budget()` costs 1.2 / 1.8 us.
- Budget: recommended working set 18,186 MiB, physical memory 24,576 MiB.

### Re-verification (2026-11-26 11:10-12:00)

Rebuilt from a fresh configure (`cmake --fresh`, `ninja -t clean`), ran the 20 ctest entries on both arches and every
binary under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` on both arches (`tools/run_validation.sh`, new; it also runs
the four pipeline-cache phases, which round 2 never ran under validation). From about 11:45 other workers' Metal
compile jobs (`metal`, `MTLCompilerService`, `spirv-cross`) and another x86_64 ctest shared the machine, so the
timings of the final runs are higher than round 2's (examples below). What this pass found and changed:

1. **DESIGN.md was missing the round-2 design it claimed.** Its header listed sections 3.6-3.10 and 8 and updated
   feature rows, and the code, STATUS and ADAPTER cite DESIGN 3.6, 3.8 and 7, but none of that text existed. Written
   now: 3.6 pre-passes, 3.7 compute, 3.8 queries and timestamps, 3.9 mesh and the refusal path, 3.10 pipeline
   creation and the archive, 3.12 GPU errors, section 8 title memory and memory governance, rows F06, F08, F13, F14,
   F17, F21, F22, F26 and F28/F31/F32/F33, section 5.3. Two code comments that pointed at "DESIGN.md 7" now say 8.
2. **Timestamp test flaw.** See item 2: the two draws had no barrier, so they could overlap; ratios 1.33 (x86_64) and
   1.18 (arm64, validation) failed. Now a UAV barrier between them, the minima of 7 runs, and under shader validation
   the ratio is reported only (x86_64 under validation gave 1.23 with the barrier: instrumented draws did not scale
   with the loop). Final runs: ratio of the minima 1.98 arm64, 1.96 x86_64.
3. **Pipeline cache under validation:** every lookup misses (item 5); the test reports hits under validation.
4. **First present compiled a compose pipeline** on the presenting thread (item 6). Now built at `create_swapchain`.
5. **Failed command buffers were never noticed.** No completed handler looked at `MTLCommandBuffer.status`, so a GPU
   error left a test reading whatever memory held. Now counted (`DeviceStats::command_buffer_errors`) and reported
   by `Queue::wait_idle` as `device_lost` (DESIGN 3.12). The error path itself was not provoked (a GPU fault would
   also hit the other workers' GPU work). (Review response: now provoked per process with
   `inject_command_buffer_failure`, which found that a failed command buffer skips its event signals; fixed.)
6. **Unexplained `same_list` failures.** Two full ctest runs read 0 0 0 at the centre of the 64x64 target in 3 of 400
   iterations (arm64: iterations 207, 208, 223) and 1 of 400 (x86_64: iteration 41). This pass wrote "not reproduced
   since" and left it open. **Corrected in the review response:** it did recur (the reviewer's arm64 run at
   12:21:42-12:22:36), and the unified log places a `kIOGPUCommandBufferCallbackErrorInnocentVictim` discard in each
   of the three runs at the failing moment, at the same millisecond as a GPU hang of another worker's
   `shaderc_msl_vs_lane_test`; see "Review response (round 2)".
7. **A8_UNORM** measured as a native render target (item 2), which ADAPTER needed (KosmicKrisp refused it in S1).
8. ADAPTER.md re-read against the current `port/d3d12_vulkan` (it had changed after round 2 wrote the plan): line
   numbers refreshed, and corrected where leonida had moved on (two PresentX planes with Flags 0 are composed;
   `GetClockCalibration` uses calibrated timestamps; the bindless slot-0 fallbacks; the presenter cut is
   `WindowPresenter`; the Vulkan-only indirect clamps; A8).
9. ORIGIN.md gained the round-2 ported logic (D24 fragment rule, quad expansion, the mesh-size rule, fork facts). The
   stub list gained vertex slots 8-31, the unmapped vertex formats, adjacency and patch topologies,
   `DescriptorHeap::clear`'s null handle, `nextDrawable` blocking, the ledger's uncounted buffers and the untested
   error path.

Final-run numbers (11:50-11:54, under the external load), arm64 / x86_64; round 2's quieter numbers are in the items
above: 10,000 dispatches encode in 7.08 / 6.60 ms (0.71 / 0.66 us each, was 0.38 / 0.57); constants ring in place
0.616 / 2.030 ms a frame against the copy path 2.354 / 5.312 ms (memcpy 0.65 / 1.10 ms and GPU copies 0.262 /
0.293 ms avoided); ocean read-back in place 0.163 / 0.333 ms against copy 0.243 / 0.334 ms; 4 pairs cold 0.577 /
0.520 s of pipelines, from the archive 0.0026 / 0.0063 s (MSL) and 0.0020 / 0.0023 s (metallib, second warm run);
`memory_budget()` 1.55 / 2.08 us; frame events 8.338 / 8.332 ms; timestamp ratio 1.98 / 1.96; corpus mesh verdicts 8
of 8; 0 command buffer errors. The x86_64 validation run of `metal_test_mesh` was once killed by the script's 600 s
watchdog while the other workers' compile jobs held `MTLCompilerService` (it passed in the final run).

### Join model fix (a round-1 hole found while testing)

Round 1's barrier model made only the encoder right after a barrier wait for the earlier work; later encoders of the
same epoch did not. `metal_test_hazards --only bindless` (a barrier, then two passes sampling the render target)
showed it: **the unmodified round-1 code (rebuilt from a copy) read a stale target in the second pass in 8, 9 and 10
of 10 lists**. It passed in round 1 only when other cases ran first. Fixed (DESIGN 3.3): every encoder waits for the
fences of the epoch before the last barrier; observe-mode encoders (timestamps, present) wait for everything without
starting an epoch. After the fix: 0 of 10 in 5 runs, `metal_hazards_epoch` passes on both arches, and the full
round-1 hazard suite still passes (0 of 400 stale in every case). A second bug of the same family found by the ocean
test: an in-encoder UAV barrier did not make the next encoder (a copy) join; fixed.

## Round 1 test results (both arches; x86_64 ran under Rosetta), kept as measured then

| test | arm64 | x86_64 | under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` |
|---|---|---|---|
| `metal_device` | PASS | PASS | PASS both arches |
| `metal_present` | PASS | PASS | PASS both arches |
| `metal_triangle` | PASS | PASS | PASS both arches |
| `metal_rect_list` | PASS | PASS | PASS both arches |
| `metal_game_shaders` | PASS | PASS | PASS both arches |
| `metal_hazards` (review fixes; 400 iterations) | PASS | PASS | full-length B1 cases also PASS under validation (arm64) |
| `metal_hazards_validation` (ctest sets the validation env; 40 iterations) | PASS | PASS | (is the validation run) |
| `metal_hazards_table_sets` (validation, `GTA6RT_METAL_NO_DEVICE_RESIDENCY=1`) | PASS | PASS | (is the validation run) |

Measured facts from those runs:

- **Device caps** (same on both arches): Apple M4 Pro; argument buffers tier 2; residency sets available; unified
  memory; families Apple7/8/9, Metal3 and Metal4 all 1; barycentrics 1; placement sparse 1; D24S8 0.
  `[[threads_per_simdgroup]]` = 32 in the probe kernel. maxBufferLength 13,639 MiB; threadgroup memory 32,768 B;
  recommended working set 18,186 MiB. GPU timestamps from `sampleTimestamps` advance 1.0e9 per wall-clock second
  (1 GHz) on both arches.
  - An earlier version converted the CPU half of `sampleTimestamps` with `mach_timebase_info` and got 24 MHz on
    arm64 and 1 GHz on x86_64. That method is wrong, and the code now measures against `steady_clock`.
- **Fences** (`test_device`):
  - Queue signal and CPU wait work, and a wait for an unsignalled value times out.
  - `on_completion` fired once.
  - The slot-12 mirror (`value_address`) reached the signalled value.
  - Cross-queue: a GPU wait on a fence held a later signal for 50 ms. A CPU signal then released it.
- **Buffers and VA:**
  - upload → device_local → readback round trip is byte-exact.
  - `resolve_va` maps a console VA inside a buffer back to (buffer, offset).
  - The console VA, the Metal gpuAddress and the CPU address of a buffer all differ (e.g. VA 0x100000000, gpuAddress
    0x10000080000, CPU 0x102834000 on arm64).
- **Arena** (placement heap of 64 MiB, `console_arena`):
  - A buffer placed at +0x300000 has gpuAddress = arena gpuAddress + 0x300000, on both arches.
  - CPU writes through the placed buffer are visible in the arena. A GPU copy out of the arena at that offset
    returns the placed bytes.
  - `resolve_va` returns the placed buffer inside its range and the arena elsewhere.
  - Under `MTL_SHADER_VALIDATION=1` the top bits of each gpuAddress carry a per-allocation tag (0x2100... and
    0x4100...). Only bits 0-43 keep the relation then, and the test checks those under validation.
  - A placed texture (RGBA8 256x256: size 264,192 B, alignment 128) was created in a device-local placement heap.
- **Barriers** (rule changed after the review, see the end):
  - Transitions on a placed buffer and on a committed buffer are both fenced (2 fenced), in one barrier call that
    split the blit encoder in two. A COPY_SOURCE to COPY_SOURCE|PIXEL_SHADER_RESOURCE transition was a no-op
    (1 no-op, no split).
  - Joins: 2, one at the execute boundary and one at the barrier. (Round 2: same count with the epoch model.)
- **write32/write64 in the stream:** 0x1122334455667788 and 0xdeadbeef landed by blit copy in queue order.
- **wait64/wait32:**
  - A copy behind `wait_value(>= 5)` had not run after 100 ms, and the queue was not idle.
  - A CPU store released it: the queue was idle 694 µs after the store (arm64) and 657 µs after it (x86_64).
  - A `wait_value(== 7)` on queue A was released by a `write_value` executed on queue B.
- **D24S8:**
  - The texture is created as Depth32Float_Stencil8 (MTLPixelFormat 260).
  - A depth clear of 0.3f reads back 0x3e99999b, which is D24 code 5,033,165. Unquantised 0.3f is 0x3e99999a.
    (Round 2: with leonida's float product the clear now reads 0x3e999999, code 5,033,164, both arches.)
- **Present** (`test_present`, 640x360, visible NSWindow with CAMetalLayer):
  - 6 cleared frames were presented. All 230,400 pixels of each drawable read back as the clear colour within
    ±1 per colour channel (alpha exact). Frame 0 pixel BGRA = 191 128 64 255, exactly the expected value.
  - Each clear became one clear-only render pass. Presenting is a compose render pass into the BGRA8 drawable.
- **Bindless triangle** (`test_triangle`, 256x256, visible window):
  - An indexed triangle (16-bit indices) samples a 4x4 RGBA8 texture. The texture's handle is in slot 37 of a
    1,024-entry texture table, and the sampler is in slot 5 of a 16-entry sampler table. Both are tier-2 argument
    buffers of 8-byte entries, indexed by user data from the 144-byte push block.
  - 7,008 pixels were checked against an analytic expectation, 2,132 of them inside the triangle: 0 wrong in the back
    buffer, and the presented drawable equals the back buffer at all checked pixels.
  - Binding plan from reflection:
    - VS: push block at buffer(0), vertex buffer at 30.
    - PS: table 0 at buffer(1), table 1 at buffer(2), push block at buffer(3).
    - 0 mismatches.
  - Lazy compilation: of 4 lazy pipelines with the same description, only the 1 drawn was compiled.
  - The clear was folded into the draw's render pass (load action Clear).
  - 10 more frames were presented with a moving offset.
  - Negative control: with `GTA6RT_METAL_NO_RESIDENCY=1` (now both residency levels off) under
    `MTL_SHADER_VALIDATION=1`, all 2,132 inside pixels are wrong, on both arches. With only the device set off
    (`GTA6RT_METAL_NO_DEVICE_RESIDENCY=1`), 0 are wrong (arm64). Without validation the omission is not visible in
    this test.
- **Rectangle lists** (`test_rect_list`, 64x64 RGBA8, point-sampled blit of a 64x64 source):
  - The CPU rule checks pass. They are VIctory's 6 right-angle cases plus `VS_Blit_6e5520d1`'s corner at the second
    vertex.
  - Full-screen blit as one rectangle with its corner at the second vertex:
    - vertex_expand: 0 pixels differ from the reference.
    - cpu_rewrite: 0 pixels differ.
    - Coverage is 4,096 / 4,096, and the reference equals the source texture.
  - Two indexed rectangles (corners at the first and third vertex, base vertex 2): 0 and 0 pixels differ, with
    4,096 / 4,096 covered.
  - Vertex shader not linear in its inputs (uv.x = u·v), compared with the hardware rule (fourth corner from the
    vertex-stage outputs):
    - vertex_expand: 0 pixels differ.
    - cpu_rewrite: 496 of 1,024 covered pixels differ, as designed.
  - The explicit render pass (load clear, store preserve) equals the implicit one.
  - A depth target discarded after its draw was not stored (1 store skipped). The colour and depth clears were both
    folded (2), and the colour result is unchanged.
- **Game shaders** (`test_game_shaders`, Route A MSL from real SPIR-V):
  - Every base MSL is byte-identical to the spike/s5.2 step 1 output for the same shader id.
  - Base and split variants are compiled from MSL source at run time; the split variant is also loaded as an offline
    metallib.

  | pair | VS | PS | PS contract bindings (index.json) |
  |---|---|---|---|
  | blit | `VS_Blit_fad9fd10_Wrapped` | `PS_BlitMod_fad9fd10_Wrapped` | 0 1 2 |
  | ui_text | `VInstancedText_27eac676_Wrapped` | `FTextMul_27eac676_Wrapped` | 0 1 2 |
  | ui_gradient | `_VInstancedPosition3dVertexCxformGradientMask_15356630_Wrapped` | `.FVertexCxformMulGradientMask_15356630_Wrapped` | 0 1 2 |
  | bink | `VS_Bink_e9aa19fd_Wrapped` | `PS_BinkNoAlpha_e9aa19fd_Wrapped` | 0 1 2 6 |

  - For blit, ui_text and ui_gradient, s5.2 step 2 compiled both shaders to metallibs.
  - The bink pair is the exception:
    - `VS_Bink_e9aa19fd` was not in the s5.2 step-2 sample. Its SPIR-V is byte-identical to `VS_Bink_0573489d`
      (same md5), which compiled there and built a pipeline in step 3.
    - `PS_BinkNoAlpha_e9aa19fd` was only cross-compiled in s5.2. It is compiled here.
  - **12 of 12 render pipelines built** (4 pairs x base / split / split-metallib) on each arch. The rectangle-list
    wrapper also built a pipeline for the real `VS_Blit_fad9fd10` (vertex_expand).
  - Reflection against the root mapping, base recipe: for all 4 PS, one `spvDescriptorSet0` argument buffer holds
    every set-0 binding with overlapping ids. Examples:
    - PS_BlitMod: only member `m_12@0`, the hash-table pointer. The image and sampler arrays alias it at offset 0.
    - PS_BinkNoAlpha: members `m_14@0` (pointer) and `m_15@8` (3D images).
    - The test reports this as a mismatch ("tables cannot be bound separately").
  - Reflection against the root mapping, split recipe:
    - Each binding is its own argument buffer `spvDescriptorSet{1+b}` at `buffer(1+b)` with one member at offset 0.
      For PS_BinkNoAlpha these are buffers 1, 2, 3 and 7.
    - The reflected tables equal the index.json bindings for all 4 PS, with 0 mismatches.
    - The push block is found in both stages. It is at buffer(0) in every VS. In the PS it is at buffer(1) with the
      base recipe, and at buffer(4) (bindings 0-2) or buffer(8) (PS_BinkNoAlpha) with the split recipe, so its index
      has to come from reflection.
  - Compile times in the final runs:
    - arm64: MSL compile 0.019-0.046 s per VS and 0.046-0.085 s per PS; pipeline creation 0.013-0.053 s.
    - x86_64: MSL compile 0.019-0.044 s per VS and 0.047-0.087 s per PS; pipeline creation 0.013-0.055 s.
    - The split VS compiles in about 3e-5 s because its MSL equals the base VS. Its sets are untouched, since the VS
      reads no set-0 binding.
    - One earlier arm64 run hit Metal's shader cache, with everything under 0.0004 s.
  - Not a pair: `VTexTGCxform_732599dc` + `CFTexTGBlipColorAndEffectCxformEAlphaDepthOccluded_732599dc` share a hash
    suffix but do not link. Metal reports fragment input `user(locn3)` as not written by the vertex function. It was
    replaced by the ui_gradient pair, whose interfaces were checked first.

Not run: no game shader was executed on data (pipelines and reflection only); no title, trace, D3D12 layer or
leonida IR was driven through this backend; Metal 4 (`MTL4*`) was never called; no performance measurement beyond
the compile and release times above.

## What is implemented (round 1 + round 2)

- **Device** (`src/mtl_device.mm`): caps probe (round 2 adds threads per group, dispatch grid, counter sampling points,
  page size, linear texture alignment, display refresh), stats, DXGI format table (39 formats, `src/mtl_formats.mm`;
  round 2 adds R16G16_UNORM/UINT), console VA space (`VaSpace`), builtin library (compose, plane compose, SIMD probe,
  timestamp no-op, indirect-count and quad-expand kernels), shared quad index pattern.
- **Queues and fences** (`src/mtl_queue.mm`): per-queue timeline event and fence pool with the epoch join model
  (DESIGN 3.3, round 2); `signal`, `wait`, `wait_idle`, `clock_calibration`; fences with CPU wait, completion callback
  and slot-12 mirror; failed command buffers counted and reported as `device_lost` by `wait_idle` (re-verification,
  DESIGN 3.12).
- **Resources** (`src/mtl_resource.mm`, `src/mtl_memory.mm`): committed and placed buffers and textures, placement heaps
  and the console arena, device residency set, allocation info, `write_region`; round 2: bytesNoCopy host windows
  (`create_host_buffer`), linear textures over buffers (`create_buffer_texture`), query heaps, B4G4R4A4 swizzle views,
  per-category ledger, memory budget and pressure handlers (dispatch memory-pressure source).
- **Descriptor tables:** 8-byte tier-2 entries, strong references, per-table residency sets.
- **Root layout:** leonida's 144-byte contract, materialised per draw, dispatch and mesh draw.
- **Pipelines** (`src/mtl_pipeline.mm`): render (round 1) plus compute and mesh (object/mesh/fragment) pipelines, one
  `PipelineCore` state machine: eager, lazy, async on compile threads with three priorities, `NotReady::wait|skip`,
  `precompile`; deferred MSL libraries; `MTLBinaryArchive` cache keyed by translator version (`open/flush_pipeline_cache`);
  `Status::limit_exceeded` with `PipelineLimit` parsed from Metal's errors (threadgroup memory, mesh size, unique scalars)
  or estimated from the MSL (mesh precheck); stencil state; D24 fragment-depth wrapper (`src/msl_rewrite.cpp`).
- **Command lists and encoding** (`src/mtl_command.mm`, `src/mtl_queue.mm`): round 1 commands plus dispatch (with the
  thread-count flag), mesh draws, `execute_indirect` (4 kinds, strides, count buffers via a hoisted pre-pass),
  quad lists (pattern buffer / GPU expansion), occlusion and binary occlusion queries, timestamp queries,
  `write_timestamp`, separate stencil refs, concurrent compute encoders with in-encoder UAV barriers.
- **Present** (`src/mtl_present.mm`): up to 8 planes (opaque / premultiplied / straight, src and dst rects, point or
  linear), sync intervals (`presentDrawable:afterMinimumDuration:` for 2+), presented-time statistics, frame events
  (CADisplayLink thread, timer fallback), `keep_last_frame`.
- **Window** (`src/mtl_window.mm`) and **rectangle lists** (`src/rect_list.cpp`, now sharing the MSL parser in
  `src/msl_rewrite.cpp`): unchanged from round 1.
- **Tool** (`tools/route_a_msl.py`): round 1 output plus split MSL for 4 corpus CS and 8 corpus MS with their index.json
  `lds_bytes`/`local_size`/bindings and S5.2 mesh verdicts.

## Stubs and gaps, by name (exact as of round 2, re-checked against the code in the re-verification)

Not in the interface (gpu.h v2):
- `ClearUnorderedAccessView*` (UAV clears).
- `ResolveSubresource` (and `StoreOp::resolve`, which stores, logs and returns `not_implemented`).
- `CopyTexture` / `CopyResource` between textures.
- Clears with rectangles (`ClearRenderTarget`/`ClearDepthStencil` rects).
- Predication.
- Reserved and tiled resources (F40), `UpdateTileMappings`, `CopyTileMappings`, `CopyTiles`.
- Acceleration structures (the XDXR compute builder is not ported).
- `AtomicCopyBufferUINT/UINT64`.
- More than one viewport or scissor per draw.
- Vertex input slots 8-31 (D3D12 and leonida's IR have 32): a pipeline with an input in slot 8 or above is refused
  ("input slot above 7"); `set_vertex_buffers` ignores views past slot 7. Vertex buffers sit at `buffer(30 - slot)`,
  next to the table indices 1-29.
- Topologies other than point/line/triangle list and strip, rectangle list and quad list (adjacency lists, patch
  lists, so tessellation): the draw is skipped with `not_implemented`; `RenderPipelineDesc` has no hull/domain stage.
- ExecuteIndirect with root-changing arguments (constants, VBV, IBV); not in leonida's IR either.
- Indirect dispatch with the thread-count flag (`dispatch_flag` applies to direct dispatches only).
- Quad and rectangle lists drawn indirectly (refused with `not_implemented`).
- HDR / colour-space compose (F22 HDR10), swapchain resize.
- Geometry stage, logic ops, conservative rasterisation, stream output, depth bounds.
- D24 footprint copies (a depth-plane copy returns the float encoding) and `rgb96` repacking.
- Metal 4 (`MTL4*`) command layer: never called.

Declared but ignored or partial:
- `SamplerDesc::mip_lod_bias`: ignored (no Metal sampler field; shader-side `bias()` not done).
- `Sampler::border_exact`: false when the border colour was rounded to one of Metal's 3.
- `TargetView::format`: ignored; views use the texture's format.
- SRGB BC formats: not in the format table ("not mapped").
- Vertex formats: 16 DXGI formats map to an `MTLVertexFormat` (R32G32B32A32 float/uint, R32G32B32 float, R32G32
  float, R32 float/uint, R16G16B16A16 float/unorm, R16G16 float, R8G8B8A8 unorm/uint, B8G8R8A8 unorm, R10G10B10A2
  unorm, R8G8 unorm, R16 float, R8 unorm); any other (SNORM, SINT, R16G16 UNORM/UINT, R32G32 UINT, R11G11B10, ...)
  refuses the pipeline with "has no vertex format".
- `DescriptorHeap::clear` writes a null handle; a shader that reads a cleared slot reads an invalid resource. The
  D3D12 layer's fallback objects (ADAPTER.md section 1) must fill cleared slots.
- `RasterDesc::depth_bias` on non-D24 formats: Metal's float bias (D32F: 1000 at z 0.5 gave 0.5 + 1000 * 2^-24).
- D24 fragment depth: falls back to the rasterizer bias (logged) for pixel shaders without MSL source (metallib only),
  with `[[early_fragment_tests]]`, or when the wrapper cannot parse the entry; multisampled D24 not covered.
- B4G4R4A4 as a render target: stores Metal's ABGR4 order (measured 0x2341 for DXGI 0x1234); needs an RGBA8 stand-in.
  Sampling is exact (swizzle view).
- `TextureDimension::cube` / `tex1d` / `tex3d` / `tex2d_array` and MSAA: created, not exercised.
- Pipeline statistics queries resolve to zeros (logged once).
- An occlusion query open across render passes counts only the last pass (logged once).
- A second occlusion query heap inside one explicit render pass: `not_implemented`.
- `write_timestamp` values land at the end of the execute (in a trailing command buffer), not before later commands of
  the same execute; timestamp-query resolves split the command buffer (DESIGN 3.8).
- `Queue::clock_calibration`: GPU ticks from `sampleTimestamps`, CPU time read right after (not an atomic pair).
- Held queues: a queue held by `wait_value` blocks the submitting thread after 255 outstanding command buffers (round-1
  review probe D, not re-run in round 2). ADAPTER.md keeps leonida's QueueHolds in the layer, which avoids it.
- Mesh estimate: conservative on a synthetic clip[1] shape (65,536 B vs Metal's 64,512 B); exact on the 8 corpus shapes.
- Under `MTL_SHADER_VALIDATION=1`: `staticThreadgroupMemoryLength` is reported doubled (65,536 for a 32 KiB kernel,
  37,116 for 18,556), mesh shaders gain threadgroup storage (an exactly-32 KiB shape is refused), and 4 of 8 corpus mesh
  shaders end with `XPC_ERROR_CONNECTION_INTERRUPTED`. Limit tests report rather than check under validation.
- Compute threadgroup size: the caller passes it (SPIRV-Cross omits `gl_WorkGroupSize` unless the shader reads it).
- `CompileQueue`: promote and steal search a deque (O(queued)); fine for tens, unmeasured at 54k queued.
- Pipeline cache: MSL libraries still compile at run time (0.44-0.59 s for 4 pairs without the system cache); offline
  metallibs and runtime-compiled MSL are separate archive entries; under `MTL_SHADER_VALIDATION=1` every archive
  lookup misses.
- `present_planes` / `present`: block in `nextDrawable` while all 3 drawables are in flight (7.6-8.9 ms at 120 Hz when
  presents outrun the display).
- Memory pressure: the dispatch source is registered but never fired in a test (levels simulated).
- Memory ledger: the per-execute value-write constants and timestamp sample buffers are not counted (`internal`
  holds query result buffers and the per-execute scratch).
- GPU errors (DESIGN 3.12): a failed command buffer is counted and `wait_idle` returns `device_lost`, once per
  failure; the event signals it skipped are done by the CPU from its completed handler. Provoked in tests only with
  `inject_command_buffer_failure` (Metal `InvalidResource`); a discard by GPU recovery happened only in the three
  historical runs, so `command_buffer_victims` (error code 5 or "InnocentVictim" in the description, both read from
  the log text) has not seen a live victim's `NSError`. No other call reports an error (`execute`, `signal` and
  fences do not). A CPU repair can briefly lower an event a later GPU signal already raised; the next completed
  handler raises it again.
- `inject_command_buffer_failure` (test hook): not under `MTL_DEBUG_LAYER` (aborts on the released buffer); a command
  buffer that samples GPU counters or copies a texture to a buffer completes without the error (measured 0 of 5 and
  0 of 6), so the timestamp-resolve split after a failed command buffer is untested (it waits on the timeline the
  same way `signal()` does, which is tested).
- Host windows: page-aligned pointer and size (16 KiB arm64, 4 KiB x86_64); below-4-GiB placement only in an x86_64
  process linked with a small `-pagezero_size` (arm64 is killed at launch with it).
- Frame events: CADisplayLink from `NSScreen.mainScreen` (macOS 14), timer at `display_refresh_hz` without a screen;
  presented times are 0 for layer-only windows. When the window is not on screen no present reports a time
  (seen in the review response while another worker's title ran: 0 of 60 interval-2 presents shown in 6 of 7
  `metal_present2` runs after 14:53, 60 of 60 in the seventh), and the test then reports pacing as not measured.
- `Fence::value_address`: updated by the event listener on the CPU, not in GPU queue order.
- `ValuePump`: polls every 100 us on one thread.
- Rectangle-list wrapper and `cpu_rewrite` limits: unchanged from round 1 (formats, per-instance inputs, slot 0 only).
- `VaSpace::resolve`: backward linear scan; VAs are never reused.
- `create_window`: visible only on the main thread; no resize, close or event forwarding.
- Diagnostics: `GTA6RT_METAL_NO_RESIDENCY`, `GTA6RT_METAL_NO_DEVICE_RESIDENCY`, `GTA6RT_METAL_NO_TARGET_JOIN`,
  `GTA6RT_METAL_NO_ERROR_REPAIR` (review response; negative control), `GTA6RT_METAL_HEADLESS`; test-only
  `GTA6RT_TEST_NO_PRECOMPILE` and `GTA6RT_TEST_NO_READBACK_FILL` (review response).

## Next steps, in order

1. **The adapter** (ADAPTER.md sections 6.1-6.6), with the d3d12_vulkan owner.
2. **Gaps the adapter meets first:** UAV clears (builtin compute), texture-to-texture copies, resolve, clears with
   rects, multi-viewport, vertex slots 8-31 and the missing vertex formats (which ones the title's input layouts use
   is not counted), D24 footprint copies, B4G4R4A4 render target stand-in, an IR `flags` word for DispatchX.
3. **XDXR builder kernels in MSL** (BVH-builder pipelines at P0b).
4. **Split recipe over the corpus (S5.2)** into a metallib store keyed by translator version; measure archive size and
   creation over the P0b storm (53,967 creations).
5. **Held-queue submit** (round-1 next step 0) if the adapter ever forwards waits that QueueHolds cannot resolve.
6. **Overlap cost of the join model** and **residency-set cost with ~38,000 images**, on replayed frames.
7. **Metal 4 probe** (`MTL4CommandQueue`, `MTL4ArgumentTable`, `MTL4Compiler`) in arm64 and x86_64 processes.

Not run in round 2 or its re-verification: no title, trace, D3D12 layer or leonida IR drove this backend; no game
shader ran on data (corpus CS/MS: pipelines and reflection only); review probe D (held queue) was not re-run; the
memory-pressure source never fired for real; no GPU error was provoked (review response: an own error now is, a
discard by recovery is not); nothing measured overlap between passes, the
cost of `max_count` indirect commands, or the compile queue and archive at the 53,967-creation scale. (The
pipeline-cache tests now also run under validation.)

## Round 1 review response

(Round 2 replaced the join model described under B1 with the epoch model; see "Join model fix" above and DESIGN 3.3.)

Response to `REVIEW.md` (2026-11-26), findings B1-B3. All three are fixed, and each fix has a test that asserts the
measured result. Design changes are in DESIGN.md 3.3, 3.5, F08, F09, F19 and 5.2.

**Before the fixes**, measured in this session: the reviewer's probe binary, linked against the unfixed library,
arm64.
- A: 15 of 400 stale.
- G: 0 0 0 0.
- C under validation: 0 0 0.

The review's numbers reproduce.

**After the fixes**:
- The probe source was rebuilt against the fixed library (`/private/tmp/claude-501/-Users-a-Desktop-Projects-GTA-VI/eeb510fb-badf-42ab-9e97-2a01126ed0b0/scratchpad/probe_new/probe_{arm64,x86_64}`, outside
  the tree) and re-run.
- New test: `tests/test_hazards.cpp` with `tests/shaders/hazards.metal`, run as three ctest entries.

### B1: table reads are not ordered by Metal tracking

**Fix** (`src/mtl_queue.mm`):
- Every barrier is fenced, whatever resource it names. The only exception is a transition between two read-only
  D3D12 states (`barrier_is_noop`).
- A fenced barrier ends the encoder, flushes pending clears and makes the next encoder join: it waits for every pool
  fence updated since the last join (`MetalQueue::open_encoder`, 9 fences, at most 8 live).
- Joins also happen at the start of every `execute` (D3D12 decay at ExecuteCommandLists boundaries), after a value
  wait, and in the present compose pass.
- A render pass whose attachment is an untracked texture already attached since the last join also joins.
- This replaces the single per-queue fence. Under the old fence, a barrier ordered only against the immediately
  preceding encoder (the review's fence-chain risk).

**Measured** (`metal_hazards`): render target R, 2048x2048, written by 6 heavy draws (4,000-step loop per pixel),
then sampled through a tier-2 table into a 64x64 target, read back every iteration. 400 iterations per case.

| case | arm64 | x86_64 | arm64 under validation (400 iterations) | both arches under validation (40 iterations, 400-step loop) |
|---|---|---|---|---|
| `same_list`: committed R, barrier RT to SRV | 0 / 400 | 0 / 400 | 0 / 400 | 0 / 40 |
| `two_executes`: committed R, write and read in two `execute` calls, no barrier | 0 / 400 | 0 / 400 | 0 / 400 | 0 / 40 |
| `placed`: placed R, barrier | 0 / 400 | 0 / 400 | 0 / 400 | 0 / 40 |
| `placed_gap`: placed R, another pass between the write and the barrier (probe A4 shape) | 0 / 400 | 0 / 400 | 0 / 400 | 0 / 40 |
| `placed_target_order`: placed R drawn again by a later pass without a barrier, 3 pixels checked | 0 / 400 | 0 / 400 | 0 / 400 | 0 / 40 |

Reviewer probe, re-run after the fix:

| mode | arm64 | x86_64 | review, before the fix |
|---|---|---|---|
| A2 (two `execute` calls) | 0 / 400 | 0 / 400 | 13 / 400 |
| A3 (placed, barrier) | 0 / 400 | 0 / 400 | 0 / 400 |
| A4 (placed, gap) | 0 / 400 | not run | 0 / 400 |
| A (no barrier call between the write and the read) | 17 / 400 | 66 / 400 | 14-18 (arm64), 13 (x86_64) |

- **Mode A stays racy by design.** D3D12 requires an RT to SRV barrier there, and the backend does not order a table
  read that no barrier precedes. Calling `useResource:usage:stages:` for every tracked resource of every bound table
  in every encoder would order it, but costs O(table size) per encoder (DESIGN 3.3).
- **Not shown to be needed:** the untracked render-target join. With `GTA6RT_METAL_NO_TARGET_JOIN=1`,
  `placed_target_order` gave 0 of 400 wrong (arm64). It is kept because Metal gives untracked attachments no
  ordering, and its cost is one join per reuse (400 in that run).
- `test_device` now asserts the new rule:
  - Committed and placed transitions: 2 fenced.
  - A read-only to read-only transition: 1 no-op, with no encoder split.
  - 2 joins: one at the execute boundary, one at the barrier.
- `test_triangle` already records a COPY_DEST to PIXEL_SHADER_RESOURCE barrier after its upload. That barrier is now
  fenced.

### B2: memory reached through `heap_base` is not resident

**Fix**: a device-wide `MTLResidencySet` (`src/mtl_device.mm` `make_resident` / `evict` / `commit_residency`).
- It holds every heap and every committed buffer and texture, from creation to destruction (constructors and
  destructors in `src/mtl_resource.mm`).
- It is attached to every queue with `addResidencySet:` and committed before each command buffer is created
  (`MetalQueue::begin_command_buffer`).
- This is D3D12's "resident from creation" rule. It also covers buffers outside the arena that a shader reaches by
  "device address - heap_base" (leonida `title_va_alias.h`).

**Measured** (`metal_hazards` `heap_base`): pixel shader reads `*(device float4*)(heap_base + offset)`. The memory
is in no table and never bound.

| memory | read (both arches; with and without validation) | `NO_DEVICE_RESIDENCY`, no validation | `NO_DEVICE_RESIDENCY`, validation |
|---|---|---|---|
| shared arena + 0x1000 | 64 128 191 255 (want 64 128 191 255) | 0 0 0 0 on both arches | 0 0 0 0 on both arches |
| device-local arena + 0x2000, written by a copy in an earlier execute | 191 64 128 255 | correct on both arches | 0 0 0 0 on both arches |
| committed buffer + 0x40, `heap_base` = its address | 32 64 96 128 | arm64 correct, x86_64 0 0 0 0 | 0 0 0 0 on both arches |

- Reviewer probe G, re-run: 64 128 191 255 on arm64 and x86_64 (the review measured 0 0 0 0). G2 still reads
  64 128 191 255.
- Cost seen: 3,267 device-set commits in the 45 s `metal_hazards` run. Every `read_texture` creates and destroys a
  readback buffer.

### B3: residency sets only on the last command buffer of an execute

**Fix**: `Encoder::close_command_buffer` (`src/mtl_queue.mm`) runs for every command buffer an `execute` produces,
including those a `wait_value` closes. For each one it:
- attaches the residency sets of the tables bound in that command buffer;
- adds a completion handler for that command buffer's own kept objects;
- pops its debug groups.

The device set (B2) covers every command buffer through the queue.

**Measured** (`metal_hazards` `wait_split`): a list with clear, heavy draw into R, barrier, bindless sample into T,
a satisfied `wait_value`, then a bindless sample into T2. `bindless` is the same list without the wait. 10 lists
each:

| configuration (all under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`) | arm64 | x86_64 |
|---|---|---|
| default (`metal_hazards_validation`) | 10 / 10 correct, both lists | 10 / 10 correct, both lists |
| table sets only, `GTA6RT_METAL_NO_DEVICE_RESIDENCY=1` (`metal_hazards_table_sets`) | 10 / 10 correct | 10 / 10 correct |
| no residency, `GTA6RT_METAL_NO_RESIDENCY=1` (negative control) | 10 / 10 wrong, both lists | 10 / 10 wrong, both lists |

- Reviewer probe C under validation, re-run: the draw before the wait writes 128 64 191 on arm64 and x86_64 (the
  review measured 0 0 0).
- The configuration with table sets only is the one that tests the per-command-buffer attachment alone. In the
  default configuration the device set also makes R resident.

### Non-blocking findings

Fixed (code), but no new test covers these:
- `discarded_` mark persisting past a copy-in or clear: fixed. A copy into the texture, a clear pass, and any pass
  that attaches the texture now remove the mark; implicit passes take it before folding a clear. The existing
  `metal_rect_list` discard checks still pass.
- `StoreOp::resolve`: no longer sets `MTLStoreActionStoreAndMultisampleResolve` without a resolve texture. It
  stores, logs, and the execute returns `not_implemented`.
- `~MetalRenderPipeline` race: `finish()` and the failure path of `wait()` now notify while holding the mutex.
- Rectangle wrapper and per-instance inputs: the wrapper is refused for a per-instance input (falls back to
  cpu_rewrite), and cpu_rewrite refuses such draws with `unsupported`.

Fixed (design or behaviour) and measured:
- Fence chain: replaced by the join over all live fences. `placed_gap` (the A4 shape): 0 / 400.
- F09 "16 KiB pages": corrected in DESIGN with probe F re-run on both arches: buffer alignment 256 B (256 B to
  1 MiB, private and shared); 1024x1024 RGBA8 texture 4,227,072 B, alignment 128 B.

Recorded only:
- Arena size versus `maxBufferLength`: recorded in DESIGN F08 (13,639 MiB < 13,824 MiB). No multi-GiB allocation was
  made.

Not fixed:
- Held queue blocks the submitter: probe D re-run on arm64 gives 255 of 600 `signal()` calls returned after 2 s, and
  all 600 after the release, as the review measured. DESIGN 5.2 no longer claims "never blocks"; next step 0.
- Raw pointers in the IR: unchanged. The caller keeps them alive (DESIGN 2.1), and the adapter must pin them.
- Stencil state, `TargetView::format` and SRGB BC formats: unchanged. The last two are now in the stub list.

### Builds and runs after the fixes

- Clean builds: 28 steps on each arch (2 more for `metal_test_hazards`), 0 warning lines, 0 error lines.
  `lipo -archs` reports the build arch for the library and all 6 test binaries.
- ctest results:
  - arm64: 8 of 8 passed (`port/build/metal-arm64/ctest.log`).
  - x86_64 under Rosetta: 8 of 8, run one test at a time (`port/build/metal-x86_64/ctest.log`).
- The 5 original tests also pass under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` on both arches
  (`ctest-<arch>-validation.log`).
- `add_subdirectory(metal)`: re-checked from a scratch top level (`project(... C CXX ASM)`, a
  `gta6rt_build_options` target). It configured, built in 11 steps and linked an executable that printed the device
  (arm64). The integration line is unchanged: `add_subdirectory(metal)` in port/CMakeLists.txt.

Not done:
- No title, trace or leonida IR was driven.
- Nothing measures what the join model costs in overlap.
- `GTA6RT_METAL_NO_TARGET_JOIN` was run on arm64 only.
- Probe D was re-run on arm64 only.

## Review response (round 2)

Response to `REVIEW-round2.md` (finding B1, blocking), 2026-11-26 13:00-15:05, on the shared machine (load 7-17,
other workers' Metal compiles and a title run). Everything below was run here; probe sources are in the session
scratchpad, every check that a claim rests on is a ctest entry.

### B1.1 The loop (`--output-on-failure` until it fails)

`tools/loop_hazards.sh <tree> [runs] [ctest -R regex] [dir]` (new) repeats ctest with `--output-on-failure`, keeps
each run's output and `LastTest.log` in `port/build/metal-<arch>/<dir>/`, and stops when `metal_hazards` fails. The
test was first made to say more on a failure (B1.4) so that a recurrence would answer the review's questions.

| loop, 13:06-14:45 | runs of full-length `metal_hazards` | failures |
|---|---:|---:|
| arm64, full suite (`metal-arm64/hazards-loop`) | 45 | 0 |
| arm64, `metal_hazards` alone (`metal-dev-arm64/hazards-loop-alone`) | 9 | 0 |
| arm64, full suite, readback buffers left untouched by the CPU (`GTA6RT_TEST_NO_READBACK_FILL=1`, `hazards-loop-nofill`) | 9 | 0 |
| arm64, the round-2 test source unchanged, readback untouched (`metal-probe-arm64/orig`) | 6 | 0 |
| x86_64, full suite, one test at a time (`metal-x86_64/hazards-loop`) | 18 | 0 |
| arm64, under `MTL_SHADER_VALIDATION_RESOURCE_USAGE=1` (B1.5) | 1 | 0 (0 reports) |

88 runs, no failure. The reason is in the system log: between 12:22:31 and 14:44 no process hung the GPU and no
command buffer anywhere was discarded (`log show` for `kIOGPUCommandBufferCallbackError`: only this response's own
`InvalidResource` probes). Up to three loops ran at once; that is what made `metal_draw2` (14 runs) and
`metal_present2` (7 runs) fail in them (B1.6).

### B1.2 The cause, from the unified log

`log show --predicate 'eventMessage CONTAINS "kIOGPUCommandBufferCallbackError"'` over the day: another worker's
`shaderc_msl_vs_lane_test` hung the GPU 48 times (`kIOGPUCommandBufferCallbackErrorHang`: 04:32-04:51, 11:26-11:30,
11:48-11:49, 12:19, 12:22) and collected 2,052 `SubmissionsIgnored`. Each hang made Metal recover the GPU and fail
other processes' in-flight command buffers with `kIOGPUCommandBufferCallbackErrorInnocentVictim` ("Discarded (victim
of GPU error/recovery)"): WindowServer 75, `gta6rt` 5, `rio` 1 and `metal_test_hazards` 9. The 9 are exactly the
three failing runs (matched by start time, run length and iteration timing; the integrator's CI ran its metal tests
later, 11:35-11:40), each at the same millisecond as one of those hangs:

| failing run | process, start | discards (log lines) | into the run | what the run reported |
|---|---|---|---:|---|
| builder re-verification, arm64 full ctest | pid 4351, 11:25:57.379 | 6, at 11:26:03.397, 03.449, 04.034 (hangs of pids 4525, 4568) | 6.0-6.7 s | `same_list` iterations 207, 208, 223 read 0 0 0; at 29 ms an iteration those times are iterations 196-217 |
| builder re-verification, x86_64 | pid 6376, 11:27:11.320 | 2, at 11:27:13.515 (hang of pid 6379, 11:27:13.511) | 2.2 s | `same_list` iteration 41 |
| reviewer, `review-metal-r2-ctest-arm64-rep1.log` (Failed, 53.50 s) | pid 8870, 12:21:42.765 | 1, at 12:22:31.439 (hang of pid 5379, same ms) | 48.7 s | case not kept; by the ctest-v3 case times (11.7 + 10.7 + 10.2 + 11.0 s before it) `placed_target_order` |

- **command_buffer_errors:** the builder's runs predate the counter; the reviewer's run had it, but its stderr
  (`[metal] command buffer failed: ...`) and counters were not kept. Metal logs a discard whether or not the process
  reads the error (probe that never reads `status` or `error`: the line is logged), so the log is a complete record
  of discards: 6, 2 and 1 lines.
- **Re-read value:** not recorded in any of the three (the re-read print did not exist yet, and `placed_target_order`
  had none).
- **Case:** `same_list` twice, and for the reviewer's run `placed_target_order` by time. That fits the code of that
  build: a `device_lost` from `read_texture` inside the render-target cases indexed an empty vector (ctest reports a
  crash), while `placed_target_order` checks `wait_idle == ok` and fails normally ("Failed").
- **Zeros:** a discarded copy never writes the new readback buffer, which Metal allocates zeroed.

So B1 is not a hazard, residency or `wait_idle` fault; it is lost work, which the backend reported (the reviewer's run)
and the test counted as a stale read.

### B1.3 The failure on demand, with the record the review asked for

No hang occurred during the response, and provoking one would discard other workers' work (the log shows it did, 75
times for WindowServer). The same loss is produced per process: `inject_command_buffer_failure(queue, as_victim)`
(new, `metal_backend.h`) makes the next command buffer a real Metal failure, a first copy from a released 64 MiB
buffer without retained references, which the kernel driver fails as `InvalidResource` for this process only (the
log showed no other process affected) and which skips everything recorded after it. Reliable only on an idle queue
with the command buffer waited for (measured 8 of 8 that way; most did not fail with work queued around them), so
the hook does both; never under `MTL_DEBUG_LAYER` (it aborts on the released buffer); a command buffer that samples
GPU counters (0 of 5) or copies a texture to a buffer (0 of 6) completes without the error.

`metal_hazards_own_error` (`--only same_list --iterations 12 --heavy 400 --inject 5`), final arm64 run:

```
same_list (committed R, barrier) iteration 5 (failure injected) at 14:58:32.685 (0.5 ms; previous 3.6 4.6 4.2 4.2 ms)
  command buffer errors: 0 before the iteration, 1 at the read (0 of them discarded by GPU recovery), 1 now;
  wait_idle after the read: ok
  T first read:  read failed (device_lost)
  T read again:  0 want, 4096 previous colour, 0 zero, 0 not written by the copy, 0 other
  R (2048 x 2048):   0 want, 4194304 previous colour, ...
  T sampled again from R (execute ok): 0 want, 4096 previous colour, ...
same_list: 1 of 12 iterations read a wrong target (1 reads failed; 0 repeated ...; command buffer errors 1)
```

The count is 1, the next `wait_idle` returns `device_lost` (the review's requirement) and the following one `ok`, and
the re-reads show that nothing of that command buffer ran (T and all of R keep the previous iteration's colour). The
same on x86_64.

### B1.4 What changed

1. **Backend bug found by the injection: a failed command buffer skips its event signals.** Signals encoded before
   the point of failure happen, the rest do not (probe: a later command buffer waiting for the skipped value was
   still blocked after 2 s; a skipped `MTLFence` update does not block later fence waits). Every command buffer ends
   with the queue's timeline signal and `signal()`'s also with the fence signal, so `wait_idle` waited out its
   timeout, every later `signal()` blocked (its command buffer waits on the timeline) and with it the D3D12 fence.
   Fix (`src/mtl_queue.mm` `end_command_buffer`, DESIGN 3.12): the signals are recorded; a failed command buffer's
   completed handler does them on the CPU where the event is still below the value, and after any failure every
   completed handler raises the timeline to its own serial. `metal_hazards_gpu_error` (arm64 / x86_64): the fence of
   a failed `signal()` arrives after 0.0 / 0.0 ms, a `signal()` after a failed execute after 0.0 / 0.1 ms, each
   failure is reported by the next `wait_idle` and only once, the queue reruns the iteration correctly, 3 errors,
   4 signals repaired, 0 counted as victims. Negative control `metal_hazards_gpu_error_no_repair`
   (`GTA6RT_METAL_NO_ERROR_REPAIR=1`): the fence wait times out after 2,000 / 2,001 ms and `wait_idle` after
   2,001 ms. Whether a discard by recovery also skips signals is not known; the reviewer's run took 53.5 s, so it did
   not wait out a 10 s `wait_idle`.
2. **Victims are told apart.** `DeviceStats::command_buffer_victims` counts failures with error code 5 or
   "InnocentVictim" in the description (both from the log text), `event_signals_repaired` the CPU signals; every
   error is logged with its serial, kind and code (first 16).
3. **The hazard test no longer mistakes a discard for a stale read, and still fails on everything else.** An
   iteration whose only errors are discards is run once more and counted ("repeated after Metal discarded them");
   any own error, any wrong read without an error, and a wrong read on the repeat fail the case. Applies to the five
   render-target cases, `heap_base`, `bindless` and `wait_split`. `metal_hazards_victim_retry` (injected failure
   counted as a victim): 0 of 12 wrong, 1 repeated; `metal_hazards_own_error`: 1 of 12 wrong, 0 repeated (both
   checked by output regex, both arches).
4. **The test says more on a failure:** wall-clock time and pid (to match the system log), the error counts before
   the iteration, at the read and now, the discard count, `wait_idle` after the read, a census of every texel of T
   (wanted colour, previous colour, zero, not written by the copy, other) for the first read, a re-read, all of R and
   a fresh sample of R, and the times of the last iterations; the check is now every texel of T instead of two.
   `read_texture` fills its readback buffer with 0xEE first (`GTA6RT_TEST_NO_READBACK_FILL=1` leaves it as Metal
   allocated it) and returns the failing call's status instead of indexing an empty vector. `readback_repeat`
   (`--only`): 20,000 fresh readback buffers, 0 wrong, 0.638 ms a read.
5. **Residency is checked by Metal, not only by zero reads.** `MTL_SHADER_VALIDATION_RESOURCE_USAGE=1` with
   `MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1` reports every access to a non-resident resource. With both residency
   levels off (`metal_hazards_residency_negative`, must report): 23 reports (20 "Texture is not resident", 3 "Invalid
   load in 'device'") on each arch. With residency on (`metal_hazards_residency_check`, every case at 40 iterations,
   fails on any "Invalid Metal usage"): 0 on each arch; the full-length suite under it: 0 reports, 0 wrong.
6. **What shader validation does with a bad access** (`metal_hazards_validation_fault`): a load 64 MiB past a 4 KiB
   buffer is reported on stderr and reads 0 0 0 0, and the command buffer completes without an error; a non-resident
   texture or buffer under plain shader validation also reads zero with 0 errors. So validation cannot drive the
   error path; `MTL_SHADER_VALIDATION_ABORT_ON_FAULT=1` aborts the process (exit 134) instead.
7. **Two other tests failed while loops shared the GPU, and both were test flaws.**
   - `metal_draw2` timestamps: ratios 0.80-1.39 (draw A's minimum 2.5-4.0 ms instead of 1.36-1.40) and twice 17 and
     31 (draw A 0.205 and 0.090 ms). The second kind is real behaviour: nothing ordered draw A after the first
     timestamp's encoder, so under GPU load the draw started before the sample (DESIGN 3.8; D3D12 does not order it
     either). The test now records a barrier after the first timestamp too, and when the ratio of 7 runs is out of
     range it takes up to 35. With a concurrent full-length hazard run on the GPU: 10 of 10 passed (two took 14 and 8
     runs), against 7 of 8 before the change (the failure: draw A 0.013 ms). Final runs: ratio 1.95 arm64, 1.99
     x86_64 in 7 runs.
   - `metal_present2`: when no present of a phase reached the screen it compared stale intervals of the previous
     phase (median 8.333 ms against 16.667). It now uses only this phase's intervals and reports "not measured" when
     there are none; that happened in 6 of 7 runs after 14:53 while another worker's title ran (0 of 60 interval-2
     presents shown); the seventh measured 60 of 60 at 16.667 ms.
8. **Cost.** `signal()` 6.8-8.8 us against 7.1-10.7 us before, `execute()` of a one-copy list 19.9-29.0 us against
   19.2-28.2 us (5,000 calls, two runs each, the round-2 library against this one, under load: no difference above
   the noise), and the same after a failure has made every completed handler read the timeline. Full-length
   `metal_hazards`: 65.7 s arm64, 67.6 s x86_64 at load 10-16 (55.3 / 53.5 s in ctest-v3).

### Results after the changes (final runs 14:57-15:00)

- Fresh configure and clean build on both arches: 52 steps, 0 warnings, 13.6 s arm64, 15.6 s x86_64 (-j3, load 9-16).
- ctest, 27 entries (7 new: `metal_hazards_residency_check`, `_residency_negative`, `_validation_fault`, `_gpu_error`,
  `_gpu_error_no_repair`, `_own_error`, `_victim_retry`): 27 of 27 on arm64 (99.4 s) and 27 of 27 on x86_64, one test
  at a time (103.0 s). The run before (x86_64, 14:53) failed `metal_present2` on the stale-interval flaw (7 above),
  fixed before the final runs.
- `tools/run_validation.sh` on both arches: 16 of 16 binaries exit 0 under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`
  (`gpu_error` skips itself under the debug layer).
- `add_subdirectory(metal)` from a scratch top level (`project(... C CXX ASM)`, a `gta6rt_build_options` target):
  14 steps, an executable printed "Apple M4 Pro api 2 errors 0 victims 0" (`port/build/metal-subdir-arm64`).
  Integrator line unchanged: `add_subdirectory(metal)` after `add_subdirectory(portable)`.

### Other workers' edits in this directory

At 14:39-14:44 another worker added HUD counters to this module: `#include <gta6rt_hud/stats.h>` and
`gta6rt_hud_*` calls in `src/mtl_device.mm`, `mtl_pipeline.mm`, `mtl_present.mm`, `mtl_queue.mm` (submission time,
draws, dispatches, GPU time in the completed handler, present latency), `publish_pending_locked` in `mtl_internal.h`,
and `target_include_directories(... PRIVATE ../hud/include)` in `CMakeLists.txt`, leaving `src/mtl_internal.h.orig`
and `src/mtl_queue.mm.orig` behind. They merged with this response's edits (both are present), the header is
header-only, and every build and test above includes them. Not reverted and not reviewed here; the module now needs
`port/hud/include` to configure.

### Not done

- No discard by GPU recovery happened during the response, so `command_buffer_victims` and the victim repeat ran only
  with the injected stand-in; that a live victim's `NSError` has code 5 is read from the log text, not observed.
- No GPU hang or fault was provoked; the timestamp-resolve split after a failed command buffer is untested (the
  injection does not fail command buffers that sample counters).
- The probes (scratchpad) ran on arm64 only; x86_64 ran the loop, the suite and the validation script. ADAPTER.md
  gained a removal-reason row and two risks; nothing else in it changed.
