# d3d12_vulkan status

Library `gta6rt_d3d12_vulkan` (static; host/ links it with `-force_load`): leonida's D3D12-on-Vulkan layer ported to
macOS (S1.6), plus the Vulkan-path work of 2026-11-26 ("first pixels", "round 4", "round 5", newest first). Origins and every
change against leonida: `ORIGIN.md`. Shader corpus: `../shaders_vk/STATUS.md`. Mesh plan: `MESH_PLAN.md`.

## Round 5 (2026-11-26): GPU memory accounting, stale title copies, the arena, the world-phase device loss

All runs: `host/tools/s3_storyload.sh` (imported save, MoltenVK 1.4.2 x86_64, hidden window), binaries
`build/layer5-x86_64/gta6rt` and `build/layer5b-x86_64/gta6rt` (both all modules, this module's sources at the time),
one gta6rt at a time; the governor worker's `[gov]` lines are in the same logs (its governor is in host/ now).
Footprint by category from `tools/footprint_sampler.sh` (`run/logs/<run>.footprint.txt`, every 30 s).

**Result.** The wall at the world phase is **not GPU memory exhaustion**. The process now enters the world phase
with 1.6-1.8 GiB less layer memory and a peak footprint of 11.2-12.8 GiB (13.7 before), but the runs still end 5-18 s
into the world phase: 7 of 9 with Metal's `kIOGPUCommandBufferCallbackErrorInvalidResource` (code 9), the last two
without a device loss: `s3l5-final` with a host SIGSEGV (probably in a copy encode), `s3l5-macos` (with the
upload-ring wait) with a silent SIGABRT. The
`VK_ERROR_OUT_OF_DEVICE_MEMORY` line before it is a refused host-pointer import of 64 KiB of title pages (the layer
falls back to a shadow), and the loss happens without it (imports off) and without any free while the GPU is busy
(frees only when idle): see "What stops it now". **No world frame was presented**; S4 is not reached.

### 1. Accounting (`src/gpu_ledger.h`, `platform/metal_memory.mm`, `[rt] gpu-memory` lines)

Every `vkAllocateMemory` of the layer's function table is recorded under the category of the scope it ran in
(thread-local `gpu_ledger::Scope`: title copies, render targets, staging, title shadows, descriptors/arena, buffers,
textures, present, other); host-pointer imports are counted apart (title pages read in place); failures are counted
and logged per category. Every `GTA6RT_MEMORY_REPORT_S` seconds (the host's `--memory-report`; `LEONIDA_GPU_REPORT_S`
overrides) the layer prints:
- `[rt] gpu-memory t=..`: the categories, imports, failures, VK_EXT_memory_budget (usage/budget over all heaps), the
  MTLDevice's `currentAllocatedSize` / `recommendedMaxWorkingSetSize`, live pipelines, the title image pool (pages vs
  live images), the GPU address arena (size, in use, peak), deferred-deletion counters;
- `[rt] gpu-memory titles t=..`: the title-texture registry by class (what Metal could sample in place, what must stay
  copied), stale overlap, retirements, views starting below mip 0;
- `[rt] gpu-memory targets t=..`: GPU-written slot-133 textures by shape.
Metal's `currentAllocatedSize` equals the budget's usage in every line (4,324 MiB at +30 s in `s3l5-base`).

**Where the memory goes, `s3l5-base`** (round-4 behaviour plus the accounting; load 23-37, swap 4.2 -> 6.0 GiB of
5.1 -> 7.2, `memory_pressure` free 67% -> 21%):

| | landing (+29 s) | mid-load (+150 s) | world phase begins (+241 s) | failure (+246..259 s) |
|---|---:|---:|---:|---:|
| footprint (os report) | 6,055 | 7,031 | 13,609 | 13,704 peak |
| footprint: GPU private / shared / untagged (title) / malloc | 2,971 / 477 / 1,915 / 1,285 (+91 s) | 2,969 / 546 / 1,978 / 1,453 (+183 s) | 5,593 / 1,160 / 4,285 / 1,925 (+244 s) | - |
| layer VkDeviceMemory | 3,110 | 3,269 | 6,099 | - |
| title copies | 351 | 452 | 3,157 | - |
| render targets | 2,391 | 2,391 | 2,388 | - |
| descriptors/arena, staging, textures, present, other | 256, 15, 9, 24, 64 | 256, 32, 46, 24, 68 | 256, 32, 170, 24, 72 | - |
| imported title pages (no copy) | 743 | 750 | 755 | - |
| Metal allocated | 4,324 | 4,517 | 7,995 | 8,721 (gov) |

(MiB.) Title copies at the world phase: 16,256 made BC images holding 2,370 MiB whose title ranges summed 2,462 MiB
but covered **649 MiB distinct**: 1,813 MiB were images of textures whose memory the title had reused.

### 2. Title textures in place: what Metal permits, measured

A Metal texture can sample bytes in place only as a buffer-backed texture (`newTextureWithDescriptor:offset:
bytesPerRow:`): linear layout, one mip, 2D, uncompressed (block-compressed formats cannot be buffer-backed), row pitch
a multiple of the device's linear alignment. The title's textures are GFX10 swizzled (4kbS/64kbS/64kbD) or BC, which
Metal cannot sample at all without the layer's detile copy. Census at the world phase (`s3l5-r7`, +120 s; title bytes):

| class | made (slot-70) | slot-133 | in place possible |
|---|---:|---:|---|
| linear, uncompressed, 1 mip, 2D | 11,161 textures, 16 MiB | 4, 5 MiB | yes (21 MiB) |
| linear, other (mips, arrays) | 3,403, 3 MiB | 12, ~0 MiB | no (buffer-backed textures have one mip) |
| swizzled, uncompressed | 286, 42 MiB | 92, 85 MiB | no (GFX10 swizzle) |
| swizzled BC | 4,568, 578 MiB | 2,143, 4,380 MiB | no (BC and swizzle) |
| GPU-written (render targets) | - | 857, 1,825 MiB of images | nothing to share (title pages untouched) |

So **21 MiB of ~5.1 GiB of title texture bytes could be sampled in place; everything else must stay copied.** The
movie planes (linear R8, 3 sets of 3840x2160 + 2x1920x1080, 37 MiB, refilled every frame) are in the first class:
in place they would save their per-frame copies (CPU), not memory. Not implemented this round: on MoltenVK it needs
VK_EXT_metal_objects (`VkImportMetalTextureInfoEXT` of a texture made from the import's MTLBuffer with the console's
256-byte pitch, which a Vulkan linear image of MoltenVK's own pitch does not match), for 0.4 % of the bytes.
Copy once and free the title's side is not possible: the title rewrites, re-streams and remaps (CopyPageMappingsBatchX)
that memory, and the refresh pass needs its bytes to see it.

### 3. One copy per live texture

- **Stale copies retired** (`retire_overlapped_title_textures`): an image made for a slot-70 view is retired when a
  newer made image or a slot-133 placement covers part of its range at another address (the title reused the memory);
  and when a map change over a filled image leaves its pages unreadable (`gone_candidates_`). Its T#s leave the hash
  table at once, the image goes 3 s later. `LEONIDA_TITLE_OVERLAP_RETIRE=0` / `LEONIDA_TITLE_GONE_RETIRE=0`.
  Measured: 28,459 and 31,524 images (6.6 GiB each time) retired as overlapped by the world phase in `s3l5-r1` and
  `-r7`; 0 as unmapped in every run.
- **Release hook** (`gta6rt_gpu_memory_release`, below): retired copies go at once instead of after 3 s.
- **Title image pool pages 2 MiB** (were 8): on MoltenVK every page is an MTLHeap of its size while one slot lives
  (`MVK_CONFIG_USE_MTLHEAP` defaults to 1); at the world phase 227 pages held 1,055 MiB for 476 MiB of images
  (`s3l5-r7`). `LEONIDA_TITLE_POOL_PAGE_KB`.
- Staging: the title upload ring is 31.6 MiB, reused; staged copies free their buffer at once; no CPU image shadows
  are kept (the fill scratch is one texture). The `vkDeviceWaitIdle` per staged upload was removed in round 4.
- Not done: **mips on demand** (668 made images at the world phase have views starting below mip 0; the mips above
  hold 109 MiB of title bytes, the ceiling of that change) and **LRU eviction of live copies** (no use signal reaches
  the CPU: shaders look T#s up in a GPU hash table).

**After, same point (world phase begins):**

| run | layer | title copies | render targets | Metal | peak footprint | machine (load, swap) |
|---|---:|---:|---:|---:|---:|---|
| `s3l5-base` (round 4 + accounting) | 6,099 | 3,157 | 2,388 | 7,995 | 13,704 | 23-37, 6.0 / 7.2 GiB |
| `s3l5-r1` (+ retire, release) | 4,254 (+211 s) | 1,308 | 2,388 | 5,829 | 11,707 | 18-24, 4.7 / 5.1 |
| `s3l5-r7` (+ arena, two-phase) | 4,443 | 1,435 | 2,388 | 6,322 | 11,538 | 9-14, 5.0 / 6.1 |
| `s3l5-final` (+ 2 MiB pool pages) | 4,419 (+172 s, gov) | 1,412 | 2,388 | 5,977 | 11,160 | 17-22, 4.3 / 5.1 |

### 4. The GPU address arena (`src/arena_size.h`, `create_arena`)

Every S3 run of round 4 and the first of this round logged "GPU address arena full" right at the world phase; then
CreateCommittedResource of DEFAULT buffers fails (E_OUTOFMEMORY). Cause, measured (`s3l5-r4`, new first-refusal line):
the arena's host pages are mapped at 0xc8b4000 (201 MiB), inside the arena's own VA window [0, size); a garbage V#
base landing in them made the title-memory probe register the whole mapping as title memory, and arena allocations
keep out of title memory. So a 256 MiB arena had ~199 MiB usable, a 448 MiB one 201 MiB (250 MiB free, all of it
"title memory"). Fix: the arena's mapping is registered as the layer's own (`TitleVaAliases::add_own_memory`;
`add_title_memory` refuses ranges over it), and the default is 384 MiB (the whole arena is charged to the process:
IOAccelerator +192 MiB for +192 MiB of arena). Since: no "arena full" in `s3l5-r5`..`-r10` (in use 240-260 MiB at
the world phase, 381 with imports off). `LEONIDA_ARENA_MB=1024` breaks the title's 32-bit heap (title-placed buffers
below the window size are not imported; arena full at 27 s, `s3l5-r2`).

### 5. What stops it now: Invalid Resource 5-18 s into the world phase

| run | change | world phase at | run ended (rc) | in world |
|---|---|---:|---:|---:|
| `s3l5-base` | - | 240.6 s | 259 s (134) | 18 s |
| `s3l5-r1` | retire + release | 214.6 | 232 (134) | 17 |
| `s3l5-r3` | arena 448 (env), placeholders S_OK, host patches off | 147.8 | 153 (134) | 5 |
| `s3l5-r5` | arena fix, 384 MiB | 120.0 | 134 (134) | 14 |
| `s3l5-r6` | + `LEONIDA_HOST_MEMORY_IMPORT=0` | 138.3 | 150 (134) | 12 |
| `s3l5-r7` | + two-phase deferred deletion | 121.0 | 126 (134) | 5 |
| `s3l5-r10` | + `LEONIDA_DEFERRED_FREE_IDLE=1` | 151.1 | 156 (134) | 5 |
| `s3l5-final` | + 2 MiB pool pages, host defaults | 171.4 | 182 (139, SIGSEGV) | 11 |
| `s3l5-macos` | + upload ring idle before releases; `build/macos` | 189.2 | 201 (134, abort) | 12 |

rc 134: Invalid Resource, then the title's device-removed int3 at exe+0x2474606. `s3l5-final` ended differently: no
device loss, a host fault reading 0x10 at pc 0x100000db2be (not in gta6rt, which is mapped at 0x10000000000; if
MoltenVK was mapped at 0x100000000, which was not checked, that is `MVKCmdBufferImageCopy<16>::encode` + 0x2e: a
buffer/image copy encoded with a dead image). The governor had released 640 MiB of retired copies through the hook
before it. Since then the retired copies are released only after the title upload ring is idle
(`TitleImageUpload::wait_idle`): the ring submits on its own timeline, outside the D3D12 retire tickets the deferred
deletion queue waits for, so an image could go while its upload was still queued. Measured once (`s3l5-macos`): no
device loss either, but a SIGABRT 12 s into the world phase with no message on stderr (not a Metal assertion, no SEH
line); peak footprint 12,753 MiB (load 14-37, swap 3.5 of 4.0 GiB), Metal 6,334 MiB. Two runs without the device
loss are not enough to call it fixed; what aborts is not known.

**World frame (S4): not reached.** The last dumped frames of `s3l5-final` and `s3l5-macos` (+191 s, 2 s into the
world phase: `run/present/s3l5-macos/present_005581.png`) are the loading artwork fading out (the store-front signs
Vision reads, "CHECKS CASHED", "EFECTIVO RAPIDO", are in the illustration). No `s4-worldframe.log` was written.

- Not memory: Metal allocated 6.2-6.3 GiB of an 18.2 GiB working set at the loss in r6/r7/r10; no allocation failed
  in r6; the 64 KiB "Could not allocate a host-coherent or exportable VkDeviceMemory" (r1, r3, r5, r7, r10, final) is a host-pointer import
  MoltenVK refuses (`[rt] d3d12: the driver refused to import title pages 0xb48c00000 + 0x10000 ...`: the first 64 KiB
  of a 1,278 MiB title region, RW, private, with 3,605 imports live), and the loss also happens with imports off (r6).
- Not a free under a running command buffer: two-phase deletion (every due object but bindless slots and buffers is
  queued again behind the submissions made since: MoltenVK keeps every live allocation in one MTLResidencySet and
  every BDA buffer resident) and freeing only when no submission runs (r10: 6,229 drains postponed) changed nothing.
  Both are kept as switches (`LEONIDA_DEFERRED_TWO_PHASE`, on; `LEONIDA_DEFERRED_FREE_IDLE`, off).
- A Metal API validation run (`s3l5-r8val`, `MTL_DEBUG_LAYER=1`, errors logged not asserted) was too slow to reach
  the world phase in 600 s; up to the story load it reported 4,574 errors, all the known short-pitch `replaceRegion` of
  host image copies (`bytesPerRow(16) must be >= (64)` and the like), nothing else.
- Next to look at, in this order: the world phase's first new work (XDXR BVH rehydration "the list cannot hold its
  scratch", indirect draws with robust-index skips, queue waits that time out and run on), a validation run started
  at the story load with a longer watchdog, and MoltenVK's handling of a placed texture whose heap page goes.

### 6. Requests

- `run/requests/d3d12_vulkan-refused-pso-object.md`: done. Placeholders now return S_OK (FSR4 stops asserting);
  host patches off by default.
- `run/requests/d3d12_vulkan-view-format.md`: slot-70 views that reinterpret a slot-133 texture get their own image;
  title-written T#s naming another format still get the null view (0-3 a run); vk_guard reports instead of refusing.
- `run/requests/d3d12_vulkan-world-memory.md`: memory cut and accounted; the stop is not memory (above).
- `run/requests/governor.md`: the hooks below.

### 7. Hooks for the governor (`port/include/gta6rt/gpu_memory.h`, new)

`gta6rt_gpu_memory_report(gta6rt_gpu_bytes*)`: the ledger categories, imports, evictable, budget, Metal figures,
pipelines, failures. `gta6rt_gpu_memory_release(max_bytes)`: drops retired title copies now (measured 376 MiB in
152 ms, 276 in 83, 102 in 6). Both weak imports in the header, defined here; the governor calls them (host/src/governor).

### Builds, tests, files

- `build/layer5-x86_64`, `build/layer5b-x86_64` (mine, all modules, same options as `build/macos`): rc 0
  (`build/layer5{,b}-x86_64-build*.log`). `build/macos` rebuilt with the final sources (all workers' as of 18:42):
  rc 0, `build/macos-build-r5.log`. `./verify.sh s1`: PASS, both checks (`run/logs/verify-s1.log`).
- Unit tests not re-run (no test covers the changed paths; the S3 runs are the test).
- Files (mine): `src/gpu_ledger.h` (new), `platform/metal_memory.{h,mm}` (new), `tools/footprint_sampler.sh` (new),
  `shims/d3d12_vulkan_device.cpp`, `shims/d3d12_xs_device.cpp`, `shims/d3d12_title_image_upload.h`,
  `src/vulkan_backend.h`, `src/vulkan_bindless.h`, `src/vulkan_present.h`, `src/vulkan_present_compose.h`,
  `src/vulkan_title_image_pool.h`, `src/title_va_alias.h`, `src/arena_size.h`, `CMakeLists.txt`.
  Outside: `port/include/gta6rt/gpu_memory.h` (new, additive); `host/src/vk_guard.cpp` (refusal dropped);
  `host/src/main.cpp` (null-PSO/FSR patches off by default, `--null-pso-patch`; the guard's log line).
- For the lead applying perf12's patches (`spike/r5-perf-fixes`): they were made against the round-4
  `d3d12_vulkan_device.cpp`; this round's hunks are in the Resource texture/buffer creation, the title-texture registry
  (make/retire/refresh), the arena, the Device reporter and the file's end, not in PSO creation.

### Switches added this round

`LEONIDA_GPU_REPORT_S`, `LEONIDA_TITLE_OVERLAP_RETIRE`, `LEONIDA_TITLE_GONE_RETIRE`, `LEONIDA_TITLE_REINTERPRET`,
`LEONIDA_REFUSED_PSO_HR=keep`, `LEONIDA_TITLE_POOL_PAGE_KB`, `LEONIDA_DEFERRED_TWO_PHASE`, `LEONIDA_DEFERRED_FREE_IDLE`;
`LEONIDA_ARENA_MB` default 384.

### Next, ranked

1. The world-phase Invalid Resource (section 5).
2. Memory toward the lead's 9-10 GiB: floor at the world phase now = title pool ~4.3-4.7 GiB (the title's) + render
   targets 2.4 (the title's 857 slot-133 targets, 323 shapes, 307 MiB of them 4K: 3840x2160 BGRA8 x5, D32S8 x2, ...;
   none alias in title memory, so aliasing saves nothing) + live title copies 0.5-1.6 (up to the 4.4 GiB of slot-133
   BC textures deferred until first use) + MoltenVK/Metal 0.8-1.1 + host malloc 1.5-1.9 + arena 0.4. Not reachable without
   cutting the render targets the title asks for or host malloc (PSO records: perf12).
3. Title-written T#s in another format: make an image from the T# (section 6).
4. In-place movie planes (VK_EXT_metal_objects) for the per-frame copy.
5. Mips on demand (<= 109 MiB).

## Round 4 (2026-11-26): submission copies, device loss, rect lists, PSO census, compile scheduling

All runs: x86_64 under Rosetta, MoltenVK 1.4.2, hidden window unless noted, `host/tools/run.sh` through
`run/logs/<name>.log`, one gta6rt at a time; other workers' runs alternated with these, so the load average
(logged by run.sh at start/end) was 8–28, and 46–60 in the last two runs. Phase numbers are from the layer's `[rt] submit phases` lines
(`tools/phase_summary.py LOG NAME=FROM:TO`), ms per submission, submission-weighted.

### 1. Copies of the title's memory per submission

| window | run | frames/s | heaps | shadows | textures | replay | sum |
|---|---|---:|---:|---:|---:|---:|---:|
| landing (30–40 s / 32–37 s) | `r4vk-base-c` (before) | 45.9 | 2.67 | 2.22 | 0.42 | 5.36 | 10.66 |
| | `r4vk-step3-a` (after) | 52.6 | 1.22 | 1.38 | 0.48 | 5.68 | 8.76 |
| movie (50–55 s / 54–59 s) | `r4vk-base-c` | 39.8 | 3.03 | 2.76 | 3.75 | 2.32 | 11.87 |
| | `r4vk-step3-a` | 48.0 | 1.29 | 1.78 | 2.65 | 2.48 | 8.20 |
| first-run pages (76–131 s / 75–130 s) | `r4vk-base-c` | 35.8 | 2.56 | 2.49 | 0.85 | 6.23 | 12.14 |
| | `r4vk-step3-a` | 35.4 | 1.18 | 1.52 | 1.09 | 6.61 | 10.40 |

Load at start → end: base-c 15.0 → 11.4, step3-a 11.4 → 11.0. On the pages the submitting thread's share of the wall
time fell from 85–89 % to 72–76 %; the frame rate there is bound by `replay` (6.2–6.6 ms), which this round did not
touch (perf-scout items 1 and 2 are perf12's).

- **Console descriptor heap: compared in place, changed pages copied.** Before: the whole resource heap (38,912 KiB)
  copied every submission in 2.49–2.62 ms (`r4vk-base-c`, "console descriptor heap 0x23a90000 (copy)"). Now
  (`scan_console_heap`): 64 KiB chunks compared against the previous copy on the copy pool, changed 4 KiB pages copied:
  2.2–14.6 KiB copied per submission in 0.86–1.23 ms (`r4vk-step4-c`, "(copy, scanned)"), 0 write faults.
  `LEONIDA_CONSOLE_HEAP_SCAN=0` restores the full copy. **Not imported in place:** the resource heap's GPU copy is not
  the title's bytes (title V#s are rebased and T#s bound into it: 18–1,845 V#s rebased per interval in `r4vk-step4-c`),
  and the heap (0x27f20000 in that boot) overlaps slot-133 import #1 (0x28a20000–0x33820000) by 27 MiB.
- **Slot-133 shadows and title windows: copied on 4 threads, not write-watched.** 58.8–65.5 MiB per submission:
  2.48 ms on the submitting thread alone (`r4vk-base-c`) → 1.46 ms with `CopyPool` (`r4vk-step3-a`, caller + 3
  helpers, `LEONIDA_COPY_THREADS`). Write-watching was not switched on: the trackers' guard needs vectored exception
  handlers (now routed to `fault_guard`, `LEONIDA_HOST_VEH=1`), but on macOS xmem reports no aliases and the file
  shims do not pin kernel writes (`title_page_watch.h`), so a watched page written through an alias or by the kernel
  would go stale silently. As measured: the r4vk logs checked (base-c, step3-a, step4-c, s3-b, step7-movie) all say
  "0 KiB of pages write-watched".
- **Fault-safe copies.** `platform/fault_guard.{h,cpp}`: copy, compare and 64-byte-sample gather routines in assembly
  with known fault exits, and a SIGSEGV/SIGBUS handler chained in front of the runtime's that resumes them at their
  failure exit. Every read of title memory by the copies, hashes and scans goes through it (no VirtualQuery pre-check
  needed). `tests/fault_guard_test.cpp`: 36 checks pass on x86_64 and arm64 (4 faults recovered, across no-access and
  unmapped pages). 64 MiB best of 5: memcpy 1.89 ms vs guarded copy 2.01 ms (x86_64, Rosetta); 1.17 vs 1.35 ms (arm64)
  (`build/d3d12vk-r4-fault_guard_test-x86_64.log`).
- **Movie plane sets: found, refilled by the set index; the fill volume did not change.** The title's
  BINKFRAMEBUFFERS (movie object + 0x58; plane entries at +0x18 + set * 0x50 + plane * 0x10, read from its code at
  exe+0x442f760) is found among the guest-stack words saved at the planes' Map calls. Its entries name the planes
  through the title's CPU window (0x300010000) while the textures sit in its GPU window (0x700010000), 0x400000000
  apart (the layer fills the textures from the GPU window and gets the frames the decoder wrote through the CPU one),
  so the match is the one offset that takes all 9 entries to mapped R8 textures of the header's
  sizes: `r4vk-step7-movie` "movie frame buffers at 0xb1f0394b8: 3 sets of Y 3840x2160 and cR/cB 1920x1080 planes
  (entries +0xfffffffc00000000 from the textures)", then 381 set switches, 983 set visits, 0 indices out of range.
  Before that offset (`r4vk-step5-a`, `r4vk-step6-movie`) the search logged "not found ... (not a slot-133
  texture)". Fill volume: 6.75 MiB per submission in the movie before (`r4vk-step6-movie`, 45–55 s: 5,644 MiB in 1,380
  fills over 836 submissions) and 4.95 MiB after (`r4vk-step7-movie`, 65–75 s: 2,201 MiB in 506 fills over 445
  submissions), about 4.1–4.4 MiB per fill both times: the page hashes already limited fills to the planes the movie
  had rewritten, so the set index saves the checks of the other sets, not copies. The movie's `textures` phase
  (3.3–3.5 ms before, 3.95–5.32 ms after) is not comparable: the load average went from 22 to 46 during the second run.
  What would cut it is sampling the planes in place (next items).
- **Uploads no longer drain the device.** `write_staged_title_image` and `write_host_texture` wait on the D3D12 queues'
  timeline values instead of `vkDeviceWaitIdle` (`LEONIDA_TITLE_UPLOAD_IDLE_WAIT=1` restores).

### 2. MoltenVK "Invalid Resource" device loss

- **Mechanism, measured** (`tests/nocopy_unmap_probe.mm`, `build/d3d12vk-r4-probes/nocopy-unmap-x86_64.txt`): a
  `newBufferWithBytesNoCopy` buffer whose host pages are deallocated, remapped (xmem's alias) or made read-only under
  it still completes blits; releasing the buffer after commit, or after encoding and before commit, with unretained
  references fails the command buffer with code 9 `kIOGPUCommandBufferCallbackErrorInvalidResource`, the error of the
  device losses.
- **Fix:** host-imported memory (slot-133 imports, imported console heaps) is freed through the deferred deletion
  queue, after the submissions that may use it retire (`LEONIDA_HOST_IMPORT_FREE=now`: as before); `create_host_buffer`
  holds the context's submit mutex from `vkCreateBuffer` until the buffer is bound or declined.
- **Since:** 0 device losses in the 18 title runs of this round (every `run/logs/r4vk-*.log`; the task's figure
  before: 1 in 8). The unified log has no InnocentVictim or GPU-restart event in the 4 h of these runs.
- **Metal API validation** (`MTL_DEBUG_LAYER=1 LEONIDA_DEVICE_FAULT=1`): `r4vk-step4-mtlval` aborted at 25 s on
  `useResources: resource (at index: 221) should not be nil` (MoltenVK makes every device-address buffer resident),
  which led to the submit-mutex hold above; `r4vk-step6-mtlval` (with it) did not show that assertion and aborted at
  28 s on another: `replaceRegion: bytesPerRow(2176) must be >= (8448)` just before the landing page. The same upload
  logs `AGX: Texture read/write assertion failed: bytes_per_row >= used_bytes_per_row` 3,697 times in every run of
  this round, the baseline included: a pre-existing upload with a short row pitch, not yet found.

### 3. Rectangle lists without a geometry stage

`src/rect_list_vertex.h` rewrites the vertex shader (SPIR-V ≥ 1.4): its body runs once per corner of the rectangle on
Private copies of its inputs, the right angle is the corner opposite the longest side (ties: corner 1), the fourth
corner is a + b − right angle, and the draw is `count / 3` instances of 6 vertices with each vertex buffer bound three
times as per-instance bindings at `offset + corner * stride`, stride `3 * stride` (the approach of
`port/metal/src/rect_list.cpp`; the scout's `v1 + v2 − v0` is not used). Indexed, indirect, instanced and
non-multiple-of-3 rect-list draws are dropped by name ("rectangle-list draw dropped, ..." census). spirv-val passes on
`VS_Blit_6e5520d1` (`tests/rect_list_vertex_test.cpp`, both arches). Skipped draws: 4,840 (`r4vk-base-c`, all the
blit) → 5 (`r4vk-step3-a`, a pipeline still building) → 0 (`r4vk-step4-c`, 51,003 draws). No frame was compared
before/after for these draws; the evidence is the counters.

### 4. Slot-57 native PSOs

The factory now records why it returned (census "slot-57 factory: ...", same in every run): plan ok VS+PS built
13,364, VS built 24, **MS+PS device refused (E_NOTIMPL) 10,726, MS device refused 1,336**. So all 12,062 slot-57
failures are mesh PSOs the device cannot make; none is a planning failure. They are not drawn before FINISH SETUP?
(`MESH_PLAN.md`). For the story path, refused PSOs at slots 55/57/62/67/86 now hand the title a placeholder object
along with the failure HRESULT, in place of the null the host's null-PSO patch had to cover
(`LEONIDA_REFUSED_PSO_PLACEHOLDER=0`: null); draws and dispatches naming one are skipped and counted by name.
`r4vk-s3-b` (S3 with `GTA6RT_VK_GUARD=0 --no-null-pso-patch`) handed out 37,556 graphics and 28 compute placeholders
and reached the **world phase at +210.3 s** ("[host] stage t=210.3s: world"), then ended (below).

### 5. Pipeline compile scheduling

- One compiler thread is kept for demanded builds (speculative builds take at most n − 1); speculative warm-up starts
  at the first present with at least 4 of the title's draws (`LEONIDA_WARMUP_DRAWS`, `LEONIDA_WARMUP_START=now`) and
  runs on 1 thread (`LEONIDA_WARMUP_THREADS`). With 3 warm-up threads (`r4vk-step4-b`) the landing page ran at 31–35
  frames/s with replay 8.9–11.1 ms; with 1 (`r4vk-step4-c`) 46.8–49.2 frames/s, replay 6.2–6.4 ms.
- Cache hits counted per stage (MoltenVK sets the hit bit on stages only): `r4vk-step3-a` 33 hits / 768 misses (miss
  mean 460 ms); warm `r4vk-step3-warm` 34 / 871 (miss mean 194 ms). On MoltenVK the session file stays neither read
  nor written by design (round 3, B3); Metal's own shader cache is what persists (the miss mean fell 460 → 194 ms).
  Where it is used (other drivers, `LEONIDA_SESSION_CACHE_LOAD=1`), a session file that exists but was not loaded is
  never overwritten (`g_session_file_state`), and saves happen at checkpoints and exit (not exercised on MoltenVK this
  round).
- **Boot manifest** (`src/boot_manifest.h`, `boot_manifest.txt` beside the cache): every pipeline's first demanded use
  in order, written every 20 s and at exit (atomic rename; a file not ours is left alone); the next boot queues those
  builds when the title creates the PSO, ahead of warm-up, in that order. Boot to the pages uses 11 pipelines
  (`run/pipeline_cache-r4vk/r4vk-step5/boot_manifest.txt`).
- **Cold vs warm, same cache directory, same input script** (`r4vk-step5-a`, `r4vk-step5-warm`, load 23 → 28):
  landing stage 21.3 s vs 23.3 s, first lit frame (the account-picker prompt) 24.0 s vs 24.7 s; pipeline waits at
  draws 18 waits / 7.9 s vs 6 waits / 0.5 s. So the warm cache and the manifest remove the pipeline waits, but the
  time to the landing prompt is not bound by pipelines. (Round 3's pair: `r4vk-step3-a` 14 waits / 14.9 s vs
  `r4vk-step3-warm` 2 waits / 0.3 s.)

### 6. Package splash and the BRIGHTNESS symbol

- **Splash:** shown. One visible-window run (`r4vk-step6-vis`, 21 s): "[rt] d3d12_xs: package splash presented
  (1920x1080)" at the first queue, the package's `xbs_splash.png` in every dumped frame through frame 421
  (`run/present/r4vk-step6-vis/first_lit_000001.png`, `present_000421.png`). Hidden-window runs leave it off by
  design (logged). Each way it can fail is now logged ("package splash not shown: ...").
- **BRIGHTNESS symbol: still missing, and not because a draw is skipped.** `r4vk-step4-c` drew 51,003 draws with 0
  skipped, BRIGHTNESS shown at +73 s; the area right of the panel has 0 non-black pixels
  (`run/present/r4vk-step4-c/present_003121.png`), and the Metal path's S2 frame is the same
  (`run/present/s2-firstrun/present_002101.png`). `r4vk-step4-b` once skipped `VS_Blit_f8e5001d` +
  `$PS_BlitCalibration_f8e5001d` while it was building. Lead, not proven: the moment the page appeared the layer logged
  a title-written T# it could not bind, a 256x1 texture at 0x2ef600 ("unbound T# ... at +0x5de40 ... no texture at T#
  address", `r4vk-step4-c.log` line 6049); the calibration pixel shader samples two textures.

### 7. Mesh

Not built. `MESH_PLAN.md`.

### Where the runs stopped, in order

| run | binary | end |
|---|---|---|
| `r4vk-base-a/b/c` | round 3 | 120–133 s timer, first-run pages |
| `r4vk-step2-a` | heap scan, fault guard, rect list | abort at 103 s: Metal view BC5 → BC7_sRGB (fixed: views Metal cannot make are refused, census "view refused, ...") |
| `r4vk-step3-a`, `r4vk-step3-warm` | + copy pool, manifest | 132–133 s timer, pages |
| `r4vk-s3-a` | step 3 | abort at 182 s: BC7_sRGB → BC1_sRGB view through the SRV path (fixed: same check in `bind_image`) |
| `r4vk-step4-b`, `r4vk-step4-c` | + placeholders, Bink search, warm-up rules | 117–144 s timer, LEGAL NOTICES |
| `r4vk-s3-b` | step 4, no host guard, no null-PSO patch | world at +210.3 s, then MoltenVK `VK_ERROR_OUT_OF_DEVICE_MEMORY` (Metal would not make a 64 KiB host-coherent buffer; process footprint 13,339 MiB), FSR4 `DeserializeComputePipelineStateX` E_INVALIDARG for `FFX_FSR4_PASS_4`, assertion, access violation in `amd_fidelityfx_upscaler_xbox.dll+0x65ff` at 220 s |
| `r4vk-step4-mtlval`, `r4vk-step6-mtlval` | Metal validation | aborts at 25 s / 28 s (section 2) |
| `r4vk-step5-a`, `r4vk-step5-warm` | + manifest by demand, Bink miss report | 90–92 s timer, LEGAL NOTICES |
| `r4vk-step6-vis` | step 6, visible window | 21 s, splash |
| `r4vk-step6-movie` | + splash logs | 71 s timer, pages; Bink search logged its miss |
| `r4vk-step7-movie` | + Bink offset match | 76 s timer, pages (load 46 at the end); movie frame buffers found |
| `r4vk-s3-c` | step 7, as s3-b, `LEONIDA_MEM_PROBE=30` | world at +235.2 s (load 50–60), then the same `VK_ERROR_OUT_OF_DEVICE_MEMORY` (64 KiB, footprint 13,356 MiB) and the same access violation at `amd_fidelityfx_upscaler_xbox.dll+0x65ff`, 242 s |

`./verify.sh s1` passes with the step-4 binary and with the final one, step 7 (`run/logs/r4vk-verify-s1-step4.log`,
`run/logs/r4vk-verify-s1-step7.log`). Module tests, final code: `d3d12vk_fault_guard_test`, `_pso_accounting_test`,
`_register_test`, `_rect_list_vertex_test` (+ spirv-val) pass on x86_64 and arm64; `_present_clear_test` passes on
arm64 and skips on x86_64 (no x86_64 Vulkan driver in its search path).

### Switches added this round

`LEONIDA_CONSOLE_HEAP_SCAN`, `LEONIDA_COPY_THREADS`, `LEONIDA_SHADOW_KERNEL_COPY=1`, `LEONIDA_HOST_VEH=1`,
`LEONIDA_BINK_SETS=0`, `LEONIDA_HOST_IMPORT_FREE=now`, `LEONIDA_TITLE_UPLOAD_IDLE_WAIT=1`,
`LEONIDA_REFUSED_PSO_PLACEHOLDER=0`, `LEONIDA_BOOT_MANIFEST=0`, `LEONIDA_WARMUP_START=now`, `LEONIDA_WARMUP_DRAWS`,
`LEONIDA_WARMUP_THREADS`.

### Next, ranked by what stops the title or costs the most

1. World-phase GPU memory (request `run/requests/d3d12_vulkan-world-memory.md`). Both S3 runs ended when Metal would
   not make a 64 KiB host-coherent buffer, at process footprints of 13,339 and 13,356 MiB. Measured in `r4vk-s3-c` at
   t=210 s (25 s before the world phase; footprint 11,085 MiB, title memory 17,078 MiB committed): the layer's
   VkDeviceMemory is 4,450 MiB device-local in 1,612 allocations, 662 MiB host-coherent in 186, and 755 MiB imported in
   3,429; registered images are backed by 1,744 MiB (slot 70) + 1,998 MiB (slot 133), with 3,605 + 846 MiB more
   deferred (not yet backed). The made images are the layer's largest share: sampling title textures in place (also
   the movie planes, section 1) is the cut; then the FSR4 pass-4 PSO (E_INVALIDARG from the deserializer) the title
   asserts on.
2. Mesh pipelines for the world (`MESH_PLAN.md`): 25,479 slot-86 and 12,062 slot-57 refusals by the world phase.
3. Replay cost on the pages (6–13 ms per submission, now the largest phase) — after perf12's patches land.
4. The short-pitch upload (3,697 AGX assertions a run; Metal validation abort) and the BRIGHTNESS T# lead.
5. Shadow write-watching, once xmem reports aliases and kernel writes are pinned on macOS; then the 1.4–1.9 ms of
   shadow copies go.

## First pixels on the Vulkan path (2026-11-26)

### Result

**MoltenVK 1.4.2 x86_64:** the title builds its first pipelines, submits its draws and presents its first UI screen.
`run/present/vk-final-mvk-dumps/present_000601.png` (1920x1080, 2,071,502 of 2,073,600 texels lit, 338 colours): the
title's background with "ACCOUNT PICKER (Y)  SCREEN NARRATION (OFF) (X)  PLAYER (A)". The same picture (338 colours)
is the first non-black frame in 7 of 9 MoltenVK runs of this session (`run/present/vk-{f1,t1,t2,t3,t5,t6,t7}*/`,
frames 301–391, 25–43 s after start); in the other two the first non-black frame is the background clear alone
(1 colour) and the UI follows.

**KosmicKrisp 26.3.1 x86_64:** past its A8_UNORM stop, and the same screen:
`run/present/vk-final-kk2/first_lit_000191.png` (frame 191 at 48.7 s, 2,071,502 texels lit, 337 colours), in a run
that ended at the 90 s timer. Two of the three KosmicKrisp runs that imported host memory died in the driver (below);
host memory import is now off on KosmicKrisp.

Neither driver reaches past this screen: the title waits for the A button and the window shim delivers no input.
The title's 30 s hang watchdog did not fire in any run.

### Per-run counts (`[rt] vk-counters` lines; `tools/vk_run_summary.py LOG`)

Since review round 3 every PSO creation call the title makes is counted once, at the entry point it called (Xbox
device slots 55, 57, 62, 67, 86 and the PC methods), and every failed call has exactly one recorded reason; the log
checks it (`N failed PSO calls, N refusal reasons`). Before, slot 57's own failures were not counted and its inner
retries were counted twice (review B1, below). Final runs of the round-3 binary, 78–80 s, hidden window,
`tools/run_vk.sh`, log `run/logs/<name>.log`:

| | r3-mvk-final (MoltenVK) | r3-kk-warm2 (KosmicKrisp) |
|---|---:|---:|
| graphics PSO calls ok / failed | 55,852 / 14,998 | 55,852 / 14,998 |
| of them slot 57 (`DeserializeGraphicsPipelineStateX`) ok / failed | 13,388 / 12,062 | 13,388 / 12,062 |
| compute PSO calls ok / failed | 2,675 / 28 | 2,675 / 28 |
| failed calls / recorded reasons | 15,026 / 15,026 | 15,026 / 15,026 |
| float-controls refusals | 0 | 0 |
| Vulkan pipelines built: graphics / compute (failed) | 2 / 270 (0) | 2 / 490 (0) |
| submissions ok / failed | 1,901 / 0 | 772 / 0 |
| draws emitted / skipped | 12,450 / 949 | 2,766 / 384 |
| skipped draws: the rect-list blit pipeline (cannot be built) | 949 | 384 |
| first non-black frame | 261 at 32.9 s (UI, 338 colours) | 231 at 38.4 s (background only) |
| presented frames/s at the end | 15.96 | 3.78 |
| load average at start / end | 8.4 / 10.2 | 11.6 / 15.1 |

PSO refusals, by reason (the same counts on both drivers in all seven round-3 runs; the 6 conservative-rasterization
refusals were labelled by their entry point in `r3-mvk-acct` and by their source line in `r3-kk-cold`, before they got
a name; the entry split of those 6 is from `r3-mvk-acct`):

| reason (as logged) | calls | entry |
|---|---:|---|
| mesh PSO, no `VK_EXT_mesh_shader`; then the +0x28/+0x48 fallback finds no stage for the program in fspx_gfx | 10,726 | slot 57 |
| mesh PSO, no `VK_EXT_mesh_shader`; then the fallback has neither a VS nor an MS (blocks 0 and 1 empty) | 1,336 | slot 57 |
| mesh PSO, no `VK_EXT_mesh_shader` | 2,930 | slot 86 |
| conservative rasterization, no `VK_EXT_conservative_rasterization` (E_INVALIDARG) | 6 | slots 55 (3), 86 (3) |
| compute shader over 32 KiB of workgroup memory | 28 | slots 62 (14), 67 (14) |
| **total** | **15,026** | |

So 14,992 of the 14,998 failed graphics calls are mesh PSOs. Slot 57's 12,062 failures are all mesh PSOs: the
0x108-byte description resolves to a mesh pipeline through `native_hashes.txt`, the device refuses it (E_NOTIMPL),
and the slot then retries leonida's older reading of the description (bare programs at +0x28/+0x48, looked up in
`fspx_gfx`), which fails with E_INVALIDARG; the title sees E_INVALIDARG. That fallback built nothing in any round-3
run. Every skipped draw in these runs is `VS_Blit_6e5520d1` + `PS_BlitColour_6e5520d1` drawn as
a rectangle list: the layer draws rect lists through a generated geometry stage and neither driver has
`geometryShader` (logged: "a rectangle-list draw needs the generated geometry stage ... its draws are dropped").

### What it took

1. **Shaders** (`port/shaders_vk`): the corpus re-translated with `--apple-float-controls --vs-lane-from-ballot`.
   `LEONIDA_SHADERS=build/shaders_vk/apple/spirv`.
2. **Float controls in the layer** (`src/float_controls_check.h`): the device's `VkPhysicalDeviceFloatControlsProperties`
   are read and a module whose execution modes need one the device lacks is refused by name instead of being handed
   to the driver (Vulkan forbids it; MoltenVK silently drops the modes, KosmicKrisp refused 54,253 PSOs in
   `vk-kk1-a8` with the old corpus). `LEONIDA_FLOAT_CONTROLS=ignore` hands them over anyway. With the Apple corpus: 0
   refusals on both drivers.
3. **Vertex subgroup refusal:** fixed in translation (shaders_vk). At boot it hit `exe/container_00`, the
   executable's own VS for quad/UI draws (608 MoltenVK refusals in `vk-b0-baseline`); the Scaleform UI VS were not
   affected. All 2,236 VS: 1,252 refused before, 2 after (CullDistance, shaders_vk STATUS).
4. **A8_UNORM on KosmicKrisp** (`src/a8_red_fragment.h`): without an A8 colour attachment an A8_UNORM render target
   is an R8_UNORM image; the pixel shader's output at that location is rewritten to (a, g, b, a), alpha blend factors
   drive the red channel, clears write alpha to red. A shape the rewrite does not handle refuses the pipeline by name.
   `LEONIDA_A8_RED=0` refuses A8 targets as before. KosmicKrisp then boots to the same screen.
5. **Title memory the allocator never reported** (`platform/title_memory_probe.cpp`): shims_core's xmem does not
   report allocations to the layer as leonida's did, so a V# into title memory resolved to nothing (16 "unresolved
   resource" lines, black frames in `vk-r1`, `vk-r2`). A miss now asks the os/ region table (else the VM map) for the
   allocation and adds its range. This was what turned black frames into the UI.
6. **Native PSOs (slot 57)** need `native_hashes.txt` and `../dxbc/` beside the corpus; the earlier overlay corpus
   lacked both. With them, +1,251 PSOs are created, among them the rect-list blit above.
7. **Pipeline creation off the title's threads** (next section). Without it, `vk-t0-old-cold` (leonida's synchronous
   builds) refused 1,180 of 1,184 submissions: one rect-list draw with no pipeline makes the whole
   `ExecuteCommandLists` unsupported, so 0 draws were emitted and no frame was lit.

Evidence tooling: `LEONIDA_PRESENT_FIRST_LIT=dir[,N]` checks every N-th present (default 10) and writes the first
frame with a non-black texel as `first_lit_<frame>.png`; leonida's `LEONIDA_PRESENT_DUMP=dir,N` writes every N-th.
`run_vk.sh` sets the first to `run/present/<log name>`. Both read the presented swapchain image back (8 MB PNG each).

### Pipeline creation performance

**Where the time goes.** The title creates ~55,700 graphics and 2,703 compute PSOs in its first ~20 s. Graphics
pipelines are built at first draw (leonida's lazy default; 2 at boot), compute pipelines by a warm-up queue and at
first dispatch. In 70–85 s a boot builds 290–950 compute pipelines: 60–270 s of compile time on the compiler threads
(longest single build 1.3–15 s). MoltenVK's own pipeline creation time is dominated by Metal's compiler and its
per-user cache (`$(getconf DARWIN_USER_CACHE_DIR)/com.apple.metal`, shared by every process; not cleared here).

**Watchdog.** The title kills itself after 30 s without a main-loop tick (leonida `runtime/src/main.cpp`, trap at RVA
0x2215c35). Replay builds a missing pipeline on the submitting title thread, so a scene with many new pipelines can
hold that thread for minutes. Now:
- `LEONIDA_ASYNC_PIPELINES` is on by default (`=0`: leonida's synchronous builds). A draw or dispatch whose pipeline
  is not built hands it to the compiler threads and waits within a budget of **2,000 ms per ExecuteCommandLists**
  (`LEONIDA_PIPELINE_WAIT_MS`); past it the use is skipped and counted until the pipeline exists. A pipeline that
  cannot be built is skipped for good and named once.
- `LEONIDA_PIPELINE_COMPILE_THREADS` compiler threads (default 4; leonida 1).
- Measured: every compile in every async run ran on the compiler threads (`worker_builds` = all builds). Longest
  ExecuteCommandLists 0.35–2.06 s, at most 4 over 1 s in a run that built pipelines. When nothing could be built for a whole run
  (`vk-final`, `vk-final2`, the cache-load hang below) the title kept running: 29 submissions waited 2 s each, no
  watchdog.

**Compiler threads, 1 vs 4** (same session, Metal cache warm from the runs before, fresh empty session cache):

| run | threads | compute pipelines at 30 s / 60 s | compile seconds | longest submission |
|---|---:|---:|---:|---:|
| vk-t4-async-1thread | 1 | 806 / 844 | 60.2 | 0.35 s |
| vk-t5-async-4threads | 4 | 871 / 906 | 82.0 | 2.03 s |

One run each on a shared machine (load average 10–14): the +8 % is within run-to-run noise and is not a result
(review round 3). Not re-run.

**Pipeline cache under `port/run/`.** `run_vk.sh` sets `LEONIDA_PIPELINE_CACHE_DIR=run/pipeline_cache/<driver>`; the
session file is `pipeline_cache.session.bin` there.
- KosmicKrisp: written by a per-device thread every 30 s while pipelines are built
  (`LEONIDA_CACHE_CHECKPOINT_SECONDS`, 0 = off), 8–94 ms per write (7–57 MB in round 3), and read on a thread of its
  own (0.0 s for 17–21 MB), so builds never wait for it. The host ends a run with `_exit` or a kill, so a teardown
  write alone never happened.
- **MoltenVK: neither read nor written by default** (since round 3; before, it was written and never read).
  MoltenVK's cache holds MSL source and `vkCreatePipelineCache` compiles every library in it, one after another
  (`MVKPipelineCache::readData` → `MVKShaderLibrary::compileLibrary`, sampled in `vk-final2`). Read synchronously as
  leonida did, a 39 MB file held every build for the whole run (`vk-final`: 0 pipelines in 81 s, no frame;
  `vk-final2`: 0 in 44 s); on its own thread it had not finished after 75 s (`vk-t7-warm-bgload`).
  `LEONIDA_SESSION_CACHE_LOAD=1` reads and writes it; `LEONIDA_SESSION_CACHE_WRITE=1` only writes it. MoltenVK also
  fails `vkGetPipelineCacheData` (VK_INCOMPLETE, "Failed to write 4 bytes to output stream") while other threads add to
  the cache, so a write serializes a merged private copy. On MoltenVK the only cache that persists between runs is
  Metal's own per-user shader cache (outside `port/run/`).

**Cold vs warm boot.** Each build is now timed by its creation feedback (`pipeline builds by cache feedback: N hits
(mean X ms), M misses (mean Y ms)`). KosmicKrisp, round-3 binary, cold run with an empty session directory, then the
warm run on the file the cold run wrote; compute pipelines ready at the last report ≤ 30 s / ≤ 60 s / end:

| pair | run | session file | ≤ 30 s / ≤ 60 s / end | hits (mean) | misses (mean) | first lit | load avg |
|---|---|---|---:|---:|---:|---:|---:|
| 1 | r3-kk-cold | none | 91 / 221 / 306 | 7 (0 ms) | 301 (926 ms) | 41.5 s | 8.8→12.2 |
| 1 | r3-kk-warm (1 min later) | 21 MB | 374 / 482 / 543 | 275 (1 ms) | 270 (1,002 ms) | 33.6 s | 10.4→11.9 |
| 2 | r3-kk-cold2 | none | 83 / 185 / 271 | 8 (0 ms) | 265 (1,044 ms) | 46.8 s | 7.9→11.6 |
| 2 | r3-kk-warm2 (1 min later) | 17 MB | 337 / 423 / 490 | 244 (2 ms) | 248 (1,138 ms) | 38.4 s (background) | 11.6→15.1 |
| review | review-pixels-kk1 | none | – / 85 / 219 | 6 | – | 69.9 s | 16–44 |
| review | review-pixels-kk2 (~4 min later) | 15 MB | 26 / 126 / 215 | 204 | – (≈ 1.35 s per build overall) | 63.3 s | 16–44 |

In both round-3 pairs a hit cost 1–2 ms against ~1 s for a miss, and the warm boot had about 4x the compute
pipelines at 30 s (374 vs 91, 337 vs 83), 2.2–2.3x at 60 s, and its first lit frame 7.9 and 8.4 s earlier (warm2's
was the background without the UI). In the reviewer's pair the hits were not cheap:
204 hits among 215 builds and still ~1.35 s per build. Total "s building" is the same in every run (the four compiler
threads are busy the whole boot); what a cache changes is how many pipelines are ready by a given time.

Why a hit is not always cheap: KosmicKrisp 26.3.1's pipeline cache holds the MSL it generated, not a Metal binary; a
hit skips SPIR-V → NIR → MSL (x86_64 code under Rosetta here) but still compiles the MSL with Metal
(`kk_deserialize_shader` → `kk_compile_compute_pipeline` → `MTL4Compiler newLibraryWithDescriptor` +
`newComputePipelineStateWithDescriptor`, `spike/s1-vulkan-x86/dl/mesa/mesa-26.3.1/src/kosmickrisp/vulkan/kk_shader.c`).
That compile is cheap only while Metal's per-user shader cache holds it. `tests/metal_cache_probe.mm` (x86_64, the same
calls and options on 12 MSL sources taken from `r3-kk-cold`'s session file by `tools/kk_cache_msl.py`, results in
`build/d3d12_vulkan-probes/metal-cache-probe-x86_64*.txt`): a compile
no cache entry can match costs 180–212 ms mean (15–369 ms each); the same source again 0.4–0.5 ms; from a new process
a minute later 0.8 ms; 5 minutes after the first compile 33.9 ms. Metal's cache directory is shared by every process of this
user (499 MB, its main file rewritten continuously by other workers' Metal compiles), so what it holds changes
within minutes. That the reviewer's slow hits were Metal-cache misses fits but is not proven: those runs predate the
per-hit timing and I cannot clear or isolate Metal's cache without disturbing other workers.

**What is claimed now:** on KosmicKrisp the session cache gives the boot 2–4x the compute pipelines by 30–60 s
when its hits are served by Metal's cache (2 of 2 round-3 pairs, warm run 1 minute after the cold one), and gives no
measured benefit when they are not (the reviewer's pair). On MoltenVK the session file gives nothing and is off.
**Withdrawn** (not reproduced, review B3): "8.5x pipelines at 30 s once Metal's cache is warm" (vk-t1 vs vk-t5, one
uncontrolled run each), "3x pipelines at 30 s in k2", and the 7 % gain from a MoltenVK session file. The first lit frame
depends on the title's boot (two UI pipelines) and varied 25–63 s across all runs with machine load.

### KosmicKrisp crash (driver)

`vk-kk1-a8` (14 s) and `vk-final-kk` (49 s) died with SIGSEGV at address 0x14 in `-[IOGPUMetalResidencySet commit]`
← `kk_device_make_resources_resident` ← `kk_queue_submit` (crash reports `gta6rt-2026-11-26-052912.ips`,
`-122934.ips`). KosmicKrisp's host-pointer import (`kk_device_memory.c`) puts the `newBufferWithBytesNoCopy` result in
its residency set without checking it for nil. Runs without host-memory import (`vk-k2-kk-noimport`,
`vk-final-kk2`, 55 s and 84 s) did not crash; `vk-k1-kk` with import did not crash either. So the layer no longer
imports host memory on KosmicKrisp (`LEONIDA_HOST_MEMORY_IMPORT=1` forces it, `=0` turns it off everywhere); the title's
memory is copied instead. That a nil buffer is the cause is not proven: a Metal test here (`newBufferWithBytesNoCopy`,
x86_64 under Rosetta, 4 KiB-aligned anonymous memory) returned buffers for every offset, and 3 runs are few.

### Running it

```sh
d3d12_vulkan/tools/run_vk.sh 90 NAME --trace=0 --watchdog 85 --hidden-window     # MoltenVK
GTA6RT_VK_DRIVER=$PWD/../spike/s1-vulkan-x86/drivers/kosmickrisp-26.3.1-x86_64/lib/libvulkan_kosmickrisp.dylib \
  d3d12_vulkan/tools/run_vk.sh 90 NAME --trace=0 --watchdog 85 --hidden-window   # KosmicKrisp
python3 -I d3d12_vulkan/tools/vk_run_summary.py run/logs/NAME.log
python3 -I d3d12_vulkan/tools/png_stats.py run/present/NAME/*.png
```

`run_vk.sh` refuses while any gta6rt runs, uses `build/d3d12_vulkan-host-x86_64/gta6rt` (all modules, this module's
sources) unless `GTA6RT_BIN` is set, and sets `LEONIDA_XMEM_32BIT=anywhere`: on the x86_64 host shims_core finds no
free range below 4 GiB for the title's 185 MB 32-bit heap and refuses it otherwise (an integrator/shims_core item).

### For the integrator

1. Rebuild `build/macos` with these sources; nothing in host/ or the top-level CMake needs to change for them. The
   host tree only links with `-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=<port>/host/src/win/host_overlay.cmake` today (review
   round 3; the plain `host/CMakeLists.txt` lacks the `host::win` sources).
2. Before `_exit` (watchdog, title exit, fatal signal if safe) call `gta6rt_d3d12_vulkan_at_exit(2000)`
   (`include/gta6rt/shims.h`, API 4): stops the compiler threads and writes the session cache (KosmicKrisp; nothing is
   written on MoltenVK by default). Nothing calls it yet; the 30 s checkpoint is the only write a killed run gets.
3. Set in `run.sh` / the host for Vulkan runs: `LEONIDA_SHADERS=<port>/build/shaders_vk/apple/spirv`,
   `LEONIDA_PIPELINE_CACHE_DIR=<port>/run/pipeline_cache/<driver>`, and `LEONIDA_XMEM_32BIT=anywhere` until the host
   has a free range below 4 GiB (or shims_core places the 32-bit heap itself).
4. Optional: port leonida's `LEONIDA_TITLE_WATCHDOG=0` kicker (`runtime/src/main.cpp:36-67`) into host/; not needed in
   any run here.
5. Input: the title waits on its first screen for A; the window shim delivers no input.

### Not done / known gaps

- Rect-list draws on both drivers (no geometry stage): 317–1,289 draws dropped per run, all one blit pipeline so far.
  Needs a rect-list path without a geometry shader.
- Mesh PSOs (14,992 calls refused: 12,062 slot 57, 2,930 slot 86), 28 compute PSOs over 32 KiB, 6 PSOs with
  conservative rasterization, 2 VS with CullDistance (MoltenVK), barycentrics on KosmicKrisp: unchanged.
- Slot 57 retries a description the device refused (E_NOTIMPL) through leonida's older +0x28/+0x48 reading, which
  built nothing in any round-3 run and turns the mesh refusal into E_INVALIDARG. It would build a pipeline with
  guessed fixed-function state if a program ever matched `fspx_gfx`. Left as it was: the title's behaviour on
  E_NOTIMPL from slot 57 has not been run.
- MoltenVK `VK_ERROR_OUT_OF_DEVICE_MEMORY` for 1.2 MB host-coherent allocations appeared twice in `vk-final-mvk` and
  in the reviewer's MoltenVK runs (the message names a 14.3 GB limit, so not a size limit), not in `r3-mvk-acct` or
  `r3-mvk-final`; not investigated.
- KosmicKrisp: 4 boots in round 3 (host-memory import off), none crashed.
- No unit test exercises a pipeline build, the wait budget, the compiler threads or the checkpoint and loader threads;
  the title runs above are their only test. `gta6rt_d3d12_vulkan_at_exit` is called by no host yet: untested.
- Frame rate: 11–20 presented frames/s on MoltenVK with the boot's compiles running at load average ~10 (15.96 in
  `r3-mvk-final`; S1 measured ~42 without shaders); not profiled. The console descriptor-heap copy (38,912 KiB per
  submission, 15–25 ms on average) is the largest submit phase in the logs (review round 3, finding 8).

## Build

From `port/` (dependencies: os, isa, shims_core):

```sh
cmake -S . -B build/d3d12_vulkan-<arch> -G Ninja -DCMAKE_OSX_ARCHITECTURES=<arch> -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DGTA6RT_MODULES="os;isa;shims_core;d3d12_vulkan"
ninja -C build/d3d12_vulkan-<arch> -j3
cmake -S . -B build/d3d12_vulkan-host-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=$PWD/host/src/win/host_overlay.cmake   # the plain host CMake does not link today
ninja -C build/d3d12_vulkan-host-x86_64 -j3 gta6rt     # the host for title runs (all modules)
```

2026-11-26 after these changes: both module trees rc 0, 0 errors, 35 warnings each on a full module build (all leonida
`%lu`/`DWORD` and switch-coverage ones; `build/d3d12_vulkan-build-{arm64,x86_64}-v2.log`, incremental `-v3.log`); the
host tree rc 0 (`build/d3d12_vulkan-host-x86_64-build13.log`).

Review round 3: incremental module trees rc 0, 0 errors, 33 warnings each (`build/d3d12_vulkan-build-{arm64,x86_64}-r3.log`);
a fresh all-module host tree `build/d3d12_vulkan-host-x86_64-r3` (configured with the host overlay; 118 steps rc 0,
then incremental `-r3-build{2..6}.log`, rc 0). All round-3 title runs used `build/d3d12_vulkan-host-x86_64-r3/gta6rt`
except the last: `run_vk.sh`'s default tree `build/d3d12_vulkan-host-x86_64` was then rebuilt with every module's
sources as of 13:36 (`-build14.log`, `-build15.log`, linked while no host ran) and smoke-run once
(`r3-mvk-main-smoke`, 56 s, MoltenVK): the same PSO counts and reasons, UI frame at 32.7 s (338 colours).

Tests re-run:

| test | result | log (`build/`) |
|---|---|---|
| `d3d12vk_present_clear_test` arm64, MoltenVK (Homebrew) | PASS (splash, 121 presents, clear colour read back, DXGI 120) | `d3d12_vulkan-run-arm64-present-moltenvk-v3.log` |
| same, KosmicKrisp (Homebrew) | PASS | `d3d12_vulkan-run-arm64-present-kosmickrisp-v3.log` |
| `d3d12vk_register_test` x86_64 / arm64 | PASS / PASS | `d3d12_vulkan-run-x86_64-register-v3.log`, `-arm64-register-v2.log` |
| round 3: `d3d12vk_pso_accounting_test` (new) x86_64 / arm64 | PASS / PASS (18 checks) | `d3d12_vulkan-run-{x86_64,arm64}-pso-accounting-r3.log` |
| round 3: `d3d12vk_register_test` x86_64 / arm64 | PASS / PASS | `d3d12_vulkan-run-{x86_64,arm64}-register-r3.log` |

The x86_64 present test under Rosetta was not re-run; the title runs above are the x86_64 evidence.

## S1.6 port (unchanged unless noted)

**Ported leonida sources** (`ORIGIN.md`): `shims/` (`d3d12_com.cpp`, `d3d12_vulkan_device.cpp`, `d3d12_xs.cpp`,
`d3d12_xs_device.cpp`, `dxgi_com.cpp`, `xbox_present.cpp`, `package_splash.cpp` and their headers), `src/` (86
headers). Every kernel32/user32 call they make is renamed to an `rthost_*` function in the force-included
`platform/win32_host.{h,cpp}`; event, thread and VM calls go to os/.

**Generated headers:** `generated/d3d12.h` from DirectX-Headers v1.618.2 with all 21 struct-returning methods in the
MSVC RetVal form plus natural-return wrappers and `__CRT_UUID_DECL` for every interface; `generated/d3d12_stubs.h`
from leonida's `gen_d3d12_stubs.py` (14 interfaces; unimplemented methods return `E_NOTIMPL` or 0 with a logged path).

**DXGI** (`include/dxgi1_6.h`, own header; vtable order checked against real DXGI), **WSI** (`VK_EXT_metal_surface` on
the CAMetalLayer of a `gta6rt_mac_window`), **driver loading** (`platform/vk_driver.cpp`: `dlopen` of the driver
dylib, no Khronos loader; `GTA6RT_VK_DRIVER` = `kosmickrisp`, `moltenvk` or a path), **displays** (CoreGraphics),
**package splash** (ImageIO), **guest export table** (`platform/register.cpp`: d3d12_xs.dll, 7 rows, x86_64;
`GTA6RT_E_UNSUPPORTED` on arm64).

**Root signatures:** `D3D12SerializeVersionedRootSignature` / `D3D12SerializeRootSignature` write a DXBC container
with one RTS0 part (`platform/rts0_serializer.cpp`, S1 integration). The two deserializers still return `E_NOTIMPL`.

**Known gaps from S1.6** (each logs or returns a defined failure): `AddVectoredExceptionHandler` not wired;
`DuplicateHandle` of os/ handles, `ReadFileEx`, `ReadFileScatter`, tokens/privileges → `ERROR_NOT_SUPPORTED`;
`LoadLibrary*`/`GetProcAddress` → not found; `CoCreateInstance` → `REGDB_E_CLASSNOTREG`; guarded copies check pages
first (a page unmapped between check and read still faults); no SDR white level or EDR headroom; HUD without VRAM;
`rt::xmem::geometry_aliases` weak fallback. Silent approximations (review finding 5): `SetThreadPriority` returns TRUE,
`SleepEx`/`WaitForSingleObjectEx` ignore `alertable`, `WaitForMultipleObjects` polls at 1 ms, `CreateThread`
suspended starts at once (logged), `FindFirstFileA` reports not found. Open review findings 1–4, 6–8
(`REVIEW.md`) are unchanged.

## Review response (round 3)

Review: `REVIEW-round3-pixels.md` (B1–B3 blocking). Every fix was shown on the real title (six runs of the round-3
binary `build/d3d12_vulkan-host-x86_64-r3/gta6rt` and one of the rebuilt default tree, one at a time,
`run/logs/r3-*.log`, load average 7.9–15.1) or by a test; numbers above in "Per-run counts" and "Pipeline creation performance".

**B1: PSO refusal accounting was false. Fixed (code, test, title).**
- Every PSO creation call of the title is counted once, at the entry point it called (`PsoCall`,
  `src/vk_run_counters.h`; slots 55/57/62/67/86 in `shims/d3d12_xs_device.cpp`, the PC methods and
  `PipelineState::create_*` in `shims/d3d12_vulkan_device.cpp`), with exactly one reason per failure: the first one
  recorded below the entry, both reasons when slot 57 retries, `<entry> returned 0x... with no reason recorded` when
  none was, and the source line for each of the 47 E_INVALIDARG description checks that logged nothing. The log
  prints calls per entry point and `N failed PSO calls, N refusal reasons`.
- Test: `tests/pso_accounting_test.cpp`, 18 checks (nesting, retries, kinds, first reason wins, 4 threads x 1,000
  calls): PASS on x86_64 and arm64.
- Title: in all seven runs slot 57 is 13,388 ok / 12,062 failed, the reviewer's traced numbers
  (`review-pixels-mvk-trace.log`); graphics 55,852 ok / 14,998 failed, compute 2,675 ok / 28 failed; 15,026 failed
  calls and 15,026 reasons. 14,992 are mesh PSOs (12,062 slot 57, 2,930 slot 86), 6 need conservative rasterization,
  28 exceed 32 KiB of workgroup memory.
- What the reviewer's lines were: "no translated graphics stage in fspx_gfx for kind-6" (10,726 calls) and
  "E_INVALIDARG ... progA 0 B progB 0 B" (1,336) are both slot 57's fallback after the device refused a mesh PSO that
  the 0x108 description had resolved through `native_hashes.txt`; no slot-57 description failed to resolve, so no
  program is missing from the corpus. The 1,342 unexplained failures of the old counters were those 1,336 fallback
  calls counted a second time plus the 6 conservative-rasterization PSOs.
- Not done: whether the title skips or replaces the draws of a PSO it could not create was not traced. The old
  `vk-final-*` counts (55,684 / 16,335) used the old counting and are superseded in the table above.

**B2: corpus provenance was misstated. Fixed (STATUS, tool).** `../shaders_vk/STATUS.md` now gives the manifest's
numbers: 20,725 re-translated, 8,552 stripped. `../shaders_vk/tools/corpus_provenance.py` derives each module's
source from the files and checks the manifest: matches; 2,012 stripped modules have a byte-identical re-translation
that was not merged; 300 random stripped modules re-stripped from make_game's corpus: 0 differ
(`build/shaders_vk/provenance-r3.txt`). The corpus itself is unchanged.

**B3: cache benefit was not shown. Fixed (measurement, behaviour, claims withdrawn).**
- Each build's time is now split by its creation feedback (cache hit / miss / no feedback).
- KosmicKrisp, two cold/warm pairs: hits 1–2 ms against 0.9–1.1 s per miss; about 4x the compute pipelines at 30 s,
  2.2–2.3x at 60 s, first lit frame ~8 s earlier. The reviewer's pair showed no benefit (hits ~1.35 s per build).
  KosmicKrisp's cache holds MSL, so a hit still compiles it with Metal, and only Metal's shared, churning per-user cache
  makes that cheap (`tests/metal_cache_probe.mm`: 180–212 ms uncached, 0.4–0.8 ms cached, 33.9 ms 5 minutes later).
  The benefit is therefore conditional, and that is what STATUS now says.
- MoltenVK: the session file is no longer written by default (it was never read; `r3-mvk-*`: 0 writes, 0 bytes).
- Withdrawn: 871 vs 102 / "8.5x once Metal's cache is warm", 354 vs 120 / "3x", the MoltenVK session-file 7 %, and the
  1-vs-4-thread +8 % as a result. Not measured: a MoltenVK boot with Metal's cache cold under control.

**Non-blocking findings.** 4: the counter's text now reads "not built yet or never buildable". 8 (descriptor-heap
copy cost) and 9 (MoltenVK `VK_ERROR_OUT_OF_DEVICE_MEMORY` for a 1.2 MB host-coherent allocation) are noted under
known gaps. 1, 2, 3, 5, 6 and 7 are not changed in this round.
