# port/d3d12_metal status (round 7)

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
