# Review of port/metal round 2 (and its re-verification pass)

Date 2026-11-26 12:00-12:50. Host: Apple M4 Pro, macOS 27.0.1. `REVIEW.md` is the round-1 review, so this is `REVIEW-round2.md`.
Only this file was written inside `port/metal`. Build trees: `port/build/review-metal-{arm64,x86_64}` (logs
`port/build/review-metal-r2-*.log`), `port/build/review-metal-subdir-r2-arm64`. Probe sources and binaries are in the
session scratchpad, not in the tree. The machine carried load average 8-17 from other workers for the whole review, so
timings are comparable to the builder's "final runs under external load", not to round 2's quiet numbers.

Verdict: APPROVED_WITH_FIXES. Every count and test result the builder reports reproduces. The performance numbers are
measured by the tests, not assumed. One item is open and I could confirm it independently: `metal_hazards` fails
intermittently on arm64 for a cause nobody has found (finding B1).

## 1. Reproduction of the claimed counts

| claim | measured here |
|---|---|
| fresh configure + clean build, 52 steps, 0 warnings, both arches | reproduced: 52/52 steps both, 0 `warning` lines, 12.7 s (arm64) and 13.0 s (x86_64) at -j3 under load (claimed 11.6 / 10.4) |
| 20 ctest entries pass on arm64 and on x86_64 (one at a time under Rosetta) | reproduced: 20/20 on each. arm64 was then re-run 3 more times: 2 of 3 passed 20/20, 1 failed `metal_hazards` (B1) |
| every test binary passes under `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`, 16 of 16 | reproduced with `tools/run_validation.sh` on both arches: 16 exits of 0 each, "binaries failing under validation: 0" |
| tests are the claimed ones | `add_test` count is 20; no SKIP_RETURN_CODE skip appeared in any run |
| `add_subdirectory(metal)` works unchanged | reproduced from a scratch top level (`project(... C CXX ASM)`, a `gta6rt_build_options` target): 14 steps, an executable printed "Apple M4 Pro api 2 tg 32768". `port/CMakeLists.txt` has no `metal` line yet and `port/compat` was not touched (mtimes). Integrator line is `add_subdirectory(metal)` after `add_subdirectory(portable)` |
| sources 12,648 lines (1,089 / 6,509 / 4,054 / 632 / 243 / 121) | reproduced exactly with `wc -l` |
| timestamp ratio 1.98 arm64 / 1.96 x86_64 with the barrier | 1.95 x86_64 (ctest), 1.85 arm64 and 1.92 x86_64 under validation (reported only there) |
| 4 real pairs cold 0.461 / 0.464 s, warm from archive 0.0020 / 0.0057 s, metallib second warm 0.0011 / 0.0012 s | cold 0.476 / 0.433 s; warm MSL 0.0017 / 0.0048 s; metallib second warm 0.0007 / 0.0013 s (arm64 / x86_64). The libraries still take 0.47-0.50 s from MSL at run time and 0.0009-0.0010 s from metallib, as STATUS says |
| pipeline cache hits 0 of 4 under shader validation | reproduced on arm64 (0 of 4 MSL, 0 of 4 metallib on both warm runs; pipelines 1.3-2.5 s) |
| in place vs copy path (constants ring) | arm64 0.372 ms in place vs 1.328 ms copy frame (memcpy 0.32 ms, GPU copies 0.293 ms); x86_64 0.429 vs 1.616 ms (memcpy 0.48, GPU 0.265). 0 wrong words, both arches |
| dispatch encode cost | 10,000 one-group dispatches: 0.34 us each arm64, 0.51 us x86_64 |
| ledger exact | 5,242,880 / 5,292,032 / 16,777,216 / 524,288 / 8,192 / 8,388,608 B, `currentAllocatedSize` difference 8,380,416 B (arm64) |
| A8_UNORM 3840x2160 exact | 0 of 8,294,400 texels off by more than 1 in all three cases (x86_64 log; arm64 passes the same test) |
| mesh verdicts 8 of 8 | reproduced; precheck 0.0006-0.0007 s against Metal 0.0119 s on the synthetic shapes and 0.34-0.50 s on the corpus shapes |
| 1 GHz timestamps | `caps().timestamp_frequency` = 1,000,000,000 in 25 of 25 device creations with 12 extra CPU burners running (see N4) |
| ADAPTER consistent with `port/d3d12_vulkan` | method and symbol names, the 29-alternative `Command` variant, `binding_count = 29`, the Vulkan-only indirect clamps (`vulkan_indirect_clamp.h:60-64`), slot 80 not forwarded, the two-plane Flags 0 compose with `LEONIDA_PRESENT_PLANE1`, `PresentPlane` fields and the fallback slot 0 all match. Every API name ADAPTER uses exists in `gpu.h` |

## 2. Blocking

### B1. `metal_hazards` fails intermittently on arm64, cause unknown, and the failure is not explained by the builder's theory

Builder: 2 failed full ctest runs (3 of 400 and 1 of 400 `same_list` iterations read 0 0 0), not seen in 3,760 later
iterations. Here: 13 ctest runs of `metal_hazards` on arm64 (4 in the full suite, 9 more in a six-test subset),
**1 failed** (the first of my 3 repeats; ctest reported "metal_hazards (Failed)", every other entry passed). I ran that
loop without `--output-on-failure` and a later run overwrote `LastTest.log`, so I do not have which case failed or what it
read. After that: 0 failures in 14 runs of the five render-target cases, 80 runs of `heap_base,bindless,wait_split`, 8
parallel full runs (4 at a time, twice), 6 parallel `same_list,two_executes` runs (4,800 iterations), 6 runs with 14 CPU
burners, and the full x86_64 ctest. So the rate is about 1 in 13 arm64 ctest runs (about 1 in 25 counting every full-length run I made), on a machine shared with other
workers, and it is not specific to the moment "right after the outside load began".

It matters because the test is the evidence for the barrier model and the residency model (DESIGN 3.3, 3.5). A zero read
fits the builder's list (a failed command buffer, `wait_idle` returning early, a non-resident texture). It also fits what
macOS does to a long command buffer on a GPU shared with other processes (a killed or "innocent victim" command buffer):
the shipped test draws take about 100 ms per iteration. The command-buffer error counter added in the re-verification is
the right instrument, but nothing has yet caught a recurrence with it printed.

Required before the adapter relies on the model: run `metal_hazards` in a loop with `--output-on-failure` until one
fails, and record `command_buffer_errors`, the re-read value and the case. If the count is 0 and the re-read is also
wrong, it is a backend bug (residency commit or fence ordering). If the count is above 0 it is the GPU error path, which
has never been exercised, and `wait_idle` must be shown to return `device_lost` there. The report's wording "has not
recurred" should be corrected.

## 3. Non-blocking findings

N1. **ADAPTER line numbers were already stale at review time.** `d3d12_vulkan_device.cpp` grew from 18,962 to 19,080
lines after the builder's 11:30 read (modified 12:05): every cited line in it is now 112 too low (`CheckFeatureSupport`
:15819 is :15931, `CreateCommandQueue` :13525 is :13637, `OpenExistingHeapFromAddress` :15138 is :15250), and
`d3d12_com.cpp` from :2380 on is 9 low (`queue_.execute` :2386 is :2395). `vulkan_replay.h` is off by 1. ADAPTER says the
integrator is editing and names symbols first, so the symbols are what to trust. Two calls are not the real signatures:
`DescriptorHeap::write_texture(binding, index, texture)` and `write_buffer(2, 0, buffer, 0)` (the interface is
`write_texture(slot, texture)` and `write_buffer(slot, buffer, offset)`, one heap object per binding).

N2. **The in-place versus copy "frame" times are not like for like.** The in-place span starts after the list is
recorded; the copy span starts before the memcpy and includes recording 600 copy commands. The headline "avoided" figures
(memcpy and GPU-copy time, 16.4 MiB) are measured separately and are fair; do not quote the frame difference (0.96 ms
arm64, 1.19 ms x86_64) as the saving. The comparison also tests a compute kernel reading the ring, not draws.

N3. **Ocean read-back in place is not faster everywhere.** x86_64: 0.247 ms in place against 0.223 ms copy; arm64 0.164
against 0.285 ms. The saving that always holds is the 884,736 B copy.

N4. **The reported timestamp frequency is a runtime estimate**: `probe_caps` samples `sampleTimestamps` 50 ms apart
against `steady_clock` and rounds to MHz. It gave exactly 1,000,000,000 in 25 of 25 creations under 12 CPU burners, so it
is stable, but a loaded start-up could in principle report 999 or 1001 MHz to `GetTimestampFrequency`. Pinning 1e9 once
the hardware fact is trusted would remove the risk.

N5. **`NotReady::wait` is the default** for render, compute and mesh pipelines, so a caller that forgets `skip` compiles
or waits on the submitting thread. With `skip` a draw is dropped (counted) until the pipeline is ready: measured here
`execute()` 0.54 ms, 1 draw skipped, next execute drew it. ADAPTER already says to use skip; the D3D12 layer must accept
missing draws.

N6. **Thread-count dispatch (flag 0x20) uses `dispatchThreads` with a partial last group.** The cool_fox fork launches
whole groups and passes the thread count to the shader in a root word (`draw.rs` 3135-3160) so the extra lanes can be
masked. Both are defensible; which one the translated BVH-builder shaders need is untested. The backend is exact on its
own contract (probe: (7,5,3) threads with 64-wide groups gave 105 threads in 15 groups; (20,20,1) with 8x8 gave 400 in 9;
zero counts launch nothing).

N7. **Present pacing text is stronger than the test output.** STATUS gives min = max = 8.333 ms at interval 1; this run
printed max 16.667 ms, 118 of 120 shown, and 94.3 presents per second, which is below the 120 Hz refresh, so "presents
outrun the display" is not what that run shows. The attribution of the 7.6-8.9 ms present calls to `nextDrawable` has no
timer in the tree (the probe that measured it is not kept). The pacing medians (8.333 and 16.667 ms) reproduce.

N8. `open_pipeline_cache` accepts an empty translator-version string, which would key the archive on GPU, OS and arch
only. `barriers_fenced` counts the 63 in-encoder UAV barriers of the compute chain as "fenced" (STATUS calls them
in-encoder memory barriers); the test asserts both numbers.

N9. STATUS item 6 explains the 7.6-8.9 ms by `nextDrawable`, and the first-present gain (13.5 ms to none) rests on a
scratchpad probe, not a test. The code does build all three compose pipelines in `create_swapchain` (`mtl_device.mm:661`).

## 4. Own probes (scratchpad, linked against the review build, arm64 unless stated)

- `probe_compute`: `dispatch(0,1,1)`, `(1,0,1)` and the thread-count flag with 0: status ok, 0 groups, with and without
  validation. 4,000,000 groups ran (256,000,000 threads). Indirect dispatch with a GPU-written count of 0, 1 and 99 and
  max 3 gave 0, 70,000 and 71,004 groups.
- `probe_ind`: indirect dispatch with `max_count` 0 (count buffer or none), 1 with count 0, and 100,000 with count 2 over
  a 4-record argument buffer: all return ok, no command buffer error, group counts exact (0, 0, 0, 21, 46), also under
  `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`. The count kernel reads only `i < min(count, max)`.
- `probe_key`: a copy of the warm archive loads for the builder's translator string (783,248 B) and starts a different,
  empty archive for a changed string. The test suite does not cover this.
- `probe_d24`: `d24_fragment_wrapper` was applied to the first 3,000 files of the S5.2 MSL corpus: 1,235 fragment shaders,
  1,235 wrapped, 0 refused; 12 of the wrapped sources compiled with `xcrun metal -c -std=metal3.1`. The shipped test only
  uses hand-written shaders, so this is new evidence that the rewrite survives real SPIRV-Cross output. Pixel shaders
  that arrive as metallib-only still fall back to the rasterizer bias (STATUS lists it).
- `probe_freq`: see N4. `metal_test_hazards` stress: see B1.

## 5. What the builder got right (checked)

- STATUS stub list: every `not_implemented` / `unsupported` return in `src/` has an entry (resolve store, rectangle on a
  non-rectangle pipeline, topologies above strip, indirect quad/rect, second occlusion heap, timestamp without the counter
  set, cpu_rewrite limits). I found no unlisted stub.
- The timestamp-test correction (a barrier between the draws) is real: the test now asserts a 1.4-2.6 ratio of 7-run
  minima and the ratios reproduce.
- Residency, hazard and bytesNoCopy behaviour: `metal_hostmem_validation` and the `NO_DEVICE_RESIDENCY` negative control
  (`metal_hazards_table_sets`) pass on both arches; the x86_64 title heap really sits at 0x34310000 (host page 4 KiB) and is
  accepted by Metal.
- DESIGN section references in code comments (3.3, 3.5-3.8, 5.3, 8) all exist now.
- Mesh refusal path, thread-count flag, no 65,535 cap, queries, quad lists, stencil, blend, depth bias, D24 fragment
  depth (4,096 of 4,096 EQUAL passes against 0 in rasterizer mode; quantiser equal on all 16,777,216 codes), A8 at
  3840x2160, two-plane compose, ledger and pressure handler: all reproduce on both arches.

## 6. What I did not do

No title, trace or leonida IR drove the backend. The memory-pressure source was not fired (a system-wide simulation would
hit other workers); only the simulated path ran. No GPU error was provoked, so the `device_lost` path is still unproven.
No multi-GiB allocation, no 53,967-pipeline run, no measurement of overlap cost or of large `max_count` encode cost. Probes
ran on arm64 only except the x86_64 test suite and validation run. I did not re-time presents with a drawable-wait timer.
