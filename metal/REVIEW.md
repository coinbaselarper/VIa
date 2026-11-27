# Review of port/metal (S5.1 first slice)

Date 2026-11-26. Reviewer: independent pass. Host: Apple M4 Pro, macOS 27.0.1, SDK 27.2 (Metal headers read from
`/Applications/Xcode.app/.../MacOSX.sdk/System/Library/Frameworks/Metal.framework/Headers`). Only this file was
written inside `port/metal`. Build trees: `port/build/review-metal-{arm64,x86_64}` (logs
`port/build/review-metal-{configure,build}-<arch>.log`) and `port/build/review-metal-subdir-arm64`. Probe sources and
binaries live in the session scratchpad (`.../scratchpad/probe.cpp`, `probe_arm64`, `probe_x86_64`), not in the tree.

Verdict: APPROVED_WITH_FIXES. Every claimed test result reproduces and the tests are valid for what they assert. Three
claimed design decisions fail when measured (all three on both arches where run); they do not invalidate a reported
number, but the D3D12 adapter must not be built on them as written.

## 1. Reproduction of the builder's results

| claim | measured here |
|---|---|
| clean build both arches, 25 steps, 0 warnings, 0 errors | reproduced: 25/25 steps, 0 `warning` lines in either build log (wall time not timed) |
| 5 tests pass on both arches | reproduced: `ctest --timeout 300`, 5/5 on arm64 and on x86_64 (Rosetta, one test at a time) |
| pass with `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` | reproduced for all 5 tests on arm64; on x86_64 device, present, triangle, rect_list, game_shaders all PASS |
| residency negative control | reproduced on both arches: `GTA6RT_METAL_NO_RESIDENCY=1` + validation gives 2132 wrong of 7008 checked pixels (FAIL); without validation 0 wrong (PASS) |
| arena: gpuAddress = arena + X | reproduced; under validation the tags differ (arm64 arena 0x4100..., placed 0x3100...; x86_64 arena 0x2100..., placed 0x4100...) and only bits 0-43 keep the relation, as stated |
| wait64 release about 0.7 ms | reproduced: 760 us (arm64) |
| 12 of 12 game-shader pipelines | reproduced on both arches, also under validation |
| base MSL: one `spvDescriptorSet0`, overlapping ids | confirmed by reading `route_a/base/PS_BlitMod_fad9fd10_Wrapped.metal`: `m_12 [[id(0)]]` pointer and four `// Overlapping binding ... unsized array hack` members at id 0; the four typed arrays are all built from `&spvDescriptorSet0.m_12`. The split output has `spvDescriptorSetBuffer1..3` |
| `add_subdirectory(metal)` works | reproduced: scratch top level (`project(... C CXX ASM)`, CXX standard 20, `gta6rt_build_options` stub) configures and builds `libgta6rt_metal.a` (9 steps). Note OBJCXX gets its language standard only through `target_compile_features(... cxx_std_20)`; that is enough here |
| 40 headless runs of `metal_test_triangle` | not claimed by the builder; I ran 40, 40 passed (see 3.1: the test's blit-then-sample ordering is not guaranteed, it just did not show) |

SDK header facts checked: `MTL4CommandQueue.h`, `MTL4CommandBuffer.h` `writeTimestampIntoHeap:` exist;
`memoryBarrierWithScope:afterStages:beforeStages:` exists on the render encoder; `presentDrawable:afterMinimumDuration:`
exists; `MTLGPUFamilyMetal4` is macOS 26+; `depth24Stencil8PixelFormatSupported` and `MTLPixelFormatDepth24Unorm_Stencil8` are
deprecated with "Never supported on Apple Silicon" / "Use MTLPixelFormatDepth32Float_Stencil8", which matches the D24S8
emulation choice; `useResidencySet:` is per command buffer (macOS 15). `newBufferWithBytesNoCopy` exists as DESIGN F08 says.

## 2. Blocking findings (design choice fails when measured)

### B1. "Committed resources are tracked, Metal orders them" is false for bindless reads (DESIGN 3.3, F19)

DESIGN 3.3 and F19 make a barrier on a committed resource a no-op. The backend never calls `useResource` / `useHeap`
(`grep` over `src/`: none); committed textures reach shaders only through the argument-buffer table plus the residency
set. The SDK says what that gives: `MTLRenderCommandEncoder.h` lines 948/958 (`useResource:usage:` / `useResources:`):
"does not protect against data hazards; these hazards must be addressed using an MTLFence", and lines 968/978
(`useResource:usage:stages:`): "For hazard tracked resources, this method protects against data hazards". Nothing in
`MTLResidencySet.h` gives hazard tracking.

Measured (probe modes A, A2, A3; backend interface only): a 2048x2048 committed render target `R` is written by a heavy
pass, then sampled through a tier-2 table by the next pass into a 64x64 target, 400 iterations.

- A (both passes in one list): 14-18 of 400 iterations read the previous iteration's colour (arm64, 5 runs: 17, 16, 16,
  14, 18; x86_64: 13).
- A2 (two separate `execute()` calls): 13 of 400 wrong.
- A3 (same, `R` a placed texture in an untracked heap, `barrier(R)` between the passes, so the backend's fence path): 0
  of 400 wrong. The fence path orders correctly; the no-op path does not.

The reported tests do not catch this: `test_triangle` uploads one 4x4 texture with a blit and samples it in the next
encoder, a race too small to show (40 of 40 headless runs passed), and `test_device` counts encoders for the fenced
barrier without checking data.

Fix: per encoder, call `useResource:usage:stages:` (read, fragment/vertex stages) for every tracked resource named in a
bound table, or treat everything reachable through a table as untracked and fence it at barriers. Add a test that
renders to a texture and samples it in the next pass, many iterations, and assert 0 wrong.

### B2. Memory reached through `heap_base` is never made resident (DESIGN F08, 3.5)

F08 says console VAs inside the arena "translate by one add in the shader (`heap_base` from the push block)". DESIGN
3.5 says directly bound resources need no residency and tables own the residency sets. The arena buffer is neither bound
nor in any table: `set_heap_base` only writes 8 bytes into the push block, and no code path adds the arena (or any heap
that console memory lives in) to a residency set or calls `useHeap`. No test dereferences a `heap_base` address in a
shader (`PushBlock.heap_lo/hi` are never read by the test shaders).

Measured (probe modes G and G2): arena heap 16 MiB shared with a float4 written at +0x1000; pixel shader reads
`*(device float4*)(heap_base + 0x1000)` with `set_heap_base(arena->host_gpu_address())`.

- G (arena not in any descriptor table): the shader reads zeros, 0 0 0 0, on arm64 and on x86_64, without validation.
- G2 (same draw, arena also written into a buffer-kind descriptor table that is bound): reads 64 128 191 255, the
  written value, on both arches.

So the F08 arena model works only if something else happens to put the arena in a residency set.

Fix: put every arena / placement heap that holds title memory in a queue-level residency set
(`MTLCommandQueue addResidencySet:`) or attach it to every command buffer with `useResidencySet:`; add the shader-side
read as a test.

### B3. Residency sets are attached only to the last command buffer of an `execute` (mtl_queue.mm 240-275, 397-412)

At a `wait_value` the encoder commits `cb_` and starts a new one. `heaps_` accumulates over the whole execute but
`useResidencySet:` is called once, after the loop, on the final `cb_`. Every draw in a command buffer closed by a value
wait runs without its residency set. DESIGN 5.2 (wait splits) and 3.5 (residency) are each claimed done; the
combination is not.

Measured (probe C): clear + heavy pass + bindless sample draw, then `wait_value` already satisfied, in one list.
Under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` the draw writes 0 0 0 (want 128 64 191); the same list without the
`wait_value` writes 128 64 191. Without validation it is correct, as with the builder's own residency control.

Fix: attach the residency sets (and the `kept_` completion handler) to every command buffer the encoder produces, or use
the queue-level set from B2.

## 3. Non-blocking findings

Measured:

- **Held queue blocks the submitter (DESIGN 5.2 says it never does).** Queues are created with
  `newCommandQueueWithMaxCommandBufferCount:256` (mtl_device.mm:203). Probe D: one list with a `wait_value` that is
  never met, then `signal()` in a loop on another thread: 255 of 600 calls returned in 2 s, the rest blocked until the
  value was stored (then all 600 returned). A title thread that is blocked this way cannot submit the write that releases
  the hold. The title does about 4,100 wait64 in 150 s (SCOPE F27).
- **Arena size versus the device limit.** `maxBufferLength` is 13,639 MiB (caps output); one buffer over the whole
  heap cannot cover the 13.5 GiB (13,824 MiB) Series X title memory. SCOPE/NOTES default to the 7.6 GiB profile, so this
  is a limit to record in DESIGN, not a defect today. Not tested at that size (no multi-GiB allocation was made).
- **F09 "16 KiB pages" is not what Metal reports.** `heapBufferSizeAndAlign` gives alignment 256 B for buffers of 256 B to
  1 MiB (private and shared); a 1024x1024 RGBA8 texture reports size 4,227,072 B, alignment 128 B (the builder's own test
  prints 128 for 256x256). 64 KiB offsets are still valid placements.
- **A barrier first in a fresh queue does not hang** (probe B: `barrier(placed)` as the first work, `wait_idle` ok), so
  `waitForFence` on a never-updated fence is tolerated here.
- Empty scissor (`{10,10,10,10}`) under API validation: fine (probe E).

Read from the code, not run:

- **Rectangle wrapper ignores per-instance inputs.** `WrapperInput` is `{location, format, slot, offset}`; the wrapper
  fetches every attribute at `vertex * stride[slot]`, so an input element with `per_instance` / `step_rate` is read per
  vertex. The plain path honours them (`mtl_pipeline.mm` 165-170). The rectangle tests use no instanced input.
- **Fence chain.** Every encoder updates one per-queue `MTLFence`, but only the encoder after a fenced barrier waits, so
  a barrier orders against the immediately preceding encoder only. Probe A4 (placed `R`, an unrelated encoder between the
  write and the barrier) gave 0 of 400 wrong, so this is not reproduced; it is a risk, not a finding.
- `discarded_` persists after a copy into the discarded texture, so discard, copy-in, draw would load DontCare.
- `StoreOp::resolve` maps to `MTLStoreActionStoreAndMultisampleResolve` with no resolve texture set (invalid in Metal);
  STATUS lists it as not exercised.
- `~MetalRenderPipeline` waits on `state_` but `finish()` calls `cv_.notify_all()` after releasing the mutex, so the
  destructor can return first for an async pipeline (narrow use-after-free window).
- The IR stores raw `MetalBuffer*`, `MetalTexture*`, `MetalRenderPipeline*`, `MetalDescriptorHeap*` (mtl_internal.h
  240-343). DESIGN 2.1 documents the caller-keeps-alive rule; D3D12 allows releasing a PSO after recording, so the D3D12
  layer has to pin them until the list is no longer executed.
- `ValuePump` polls every 100 us on one thread, as STATUS says; every wait is a pump entry plus a shared event.
- Stencil state is not built (`DepthStencilDesc::stencil` ignored), SRGB BC formats are absent from the 37-row format
  table, `TargetView::format` is ignored. STATUS lists the first; the others are not listed.

## 4. Test validity notes

- `triangle.metal` and `blit.metal` are hand-written in SPIRV-Cross's shape (ORIGIN.md says so), not SPIRV-Cross output.
  The game-shader test uses real Route A output, but builds the vertex layout from `index.json` input signatures
  (float32 formats by component count, packed offsets) and fixed RGBA8 targets, so "pipelines build" is the strongest
  claim; STATUS says that.
- The pairs were never matched against a title PSO stream; Metal's own VS/PS link check passed for all four (and failed
  for the discarded `732599dc` pair, as STATUS says).
- The rect tests compare backend output against a triangle draw built on the CPU with the same corner rule
  (`triangles_for`), a valid check of the expansion logic.
- Reported GPU timestamp frequency 1 GHz reproduces (device test prints 1,000,000,000 Hz).

## 5. What I did not do

No multi-GiB heap allocation, no run of `route_a_msl.py` against a different SPIR-V set, no Metal 4 call (the module
makes none), no title or leonida IR driven, no thread-race stress beyond probes A/A2/A3/A4/C/D/G/G2, x86_64 probes only for
A, G, G2 (plus the full test set), no check of leonida's `vulkan_backend.h` beyond what DESIGN 2.4 cites.

## 6. Ordered fixes before the adapter (step 1 of STATUS next steps)

1. B1: per-encoder `useResource:usage:stages:` for tracked table resources, or fence them; add the render-then-sample test.
2. B2 and B3 together: queue-level residency sets for arena and title heaps, attached to every command buffer; add the
   `heap_base` read test and a `wait_value` + bindless draw test, both under `MTL_SHADER_VALIDATION=1`.
3. Raise or avoid the 256 command-buffer bound for held queues (fewer command buffers per signal/execute, or queue pooling
   with a hold-aware submit thread) and record the measured 255-signal block in DESIGN 5.2.
4. Add per-instance fetch to the rectangle wrapper, or refuse the pipeline when any input is per-instance.
5. Record the arena size limit and correct the "16 KiB pages" line in DESIGN F09.
