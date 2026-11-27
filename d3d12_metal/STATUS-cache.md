# port/d3d12_metal/cache: pipeline cache for the native Metal path (status)

## Round 8 (2026-11-27 02:10 - 04:30): world tier complete, lookups on by default, what a warm start costs in memory

Host as below. Load was 3-9 after the 03:33 kernel panic and reboot (14-50 before it). I rebuilt the tree
`build/pc7-x86_64` from the live shared sources at 03:57. Runs used `cache/tools/pc7_run.sh` with
`cache/tools/fault_stop.sh` beside it, which kills the run at the first `[metal]` command-buffer failure or FAULTED
encoder (lead's rule 3). Settings for both runs: 13 GiB footprint cap, `GTA6RT_METAL_INDIRECT_GUARD=window` (the
narrowed default produced page faults in gates-d), no audio, hidden window.

### 1. World tier (done; refreshed as new draw classes appear)

- Lists: `cache/tools/tier_refresh.sh 300` merges every recorder file under `build/` (d3m-s7-*, up7-*, pc7-*, gates-*,
  d3m-s8-*, pc8-*), aligned by time since each run's world start. It writes `build/pc7/lists/world-r8-300.{tsv,specs}`,
  the tier's `pcache-tier.txt`, and `world-new.specs` (pipelines no earlier list had). The build catalog is the store
  plus the mesh overlay plus the uint overlay (`cache/tools/catalog_merge.py` -> `build/pc7/catalog-r8.tsv`), so
  overlay shaders are built at the keys a first use asks for.
- Builds into `build/pc7/arch-world` (nice 10, `--jobs 3 --title-jobs 2`), in order:
  - the rest of the 60 s list;
  - the 2,121-pipeline 300 s list: 70 min, done at 03:32, one minute before the panic. 879 new; 14,438 s in flight;
    2,982 s waiting for a running gta6rt;
  - after the reboot: all 87 shards passed their size + content-hash check;
  - the 2,429-pipeline refresh over the merged catalog: 2,394 ok, 35 refused, 93 new in 7 min (load 5-7);
  - the uint worker's 7 `arch-int` shards, copied in as `uintint-*`;
  - the 327 pipelines the two pc8 runs recorded that no list had: 180 tiled-lighting rect-list pipelines (reachable
    since the uint fix), 30 `vs_classfied_fullScreen`, and cascade-shadow mesh variants. 255 were already in the
    archive, 69 built, 3 refused (8 min, `prep-world-g`).
- Now: **2,756 world pipelines in the list, about 2,718 in the tier, 38 refused; 95 shards, 2.65 GB.**
- The 38 refusals: 37 are mesh shaders over 32 KiB of threadgroup memory (34,008 / 36,056 bytes), including the
  overlay's `MS_VS_PedTransformPreSkinD_Packed_6a40bffd` (names in the requests file). 1 is a vertex input in DXGI
  format 17, which has no Metal vertex format.

### 2. Lookups on by default (port/metal `PipelineCacheDesc::read_only`, `open_pipeline_cache`; pcache)

A Metal run opens its own archive directory, also looks up `build/pc7/arch-world` read-only, and prebuilds the tier's
manifest. Nothing needs to be passed. Options: `GTA6RT_METAL_ARCHIVE_TIERS=off` or `=dirA:dirB`. A recorded pipeline
whose shader now has another content key (overlays) is re-keyed at start.

### 3. Memory: what a warm start costs (measured offline, x86_64, load 5-7)

| `prepare_shaders` over the 2,429 list, all archive hits | footprint after |
|---|---|
| pipelines released after build | 1,880 MiB |
| all 2,394 pipelines held (`--hold`) | 1,891 MiB |
| libraries only: 1,758 metallibs (689 MiB on disk) loaded, no pipelines (`cache/tools/lib_footprint.mm`) | +1,388 MiB |

- Held and released end at the same footprint. That is not because pipelines are free: releasing a Metal pipeline
  state returns no memory (the Metal memory worker measured it, 04:05 note; 0.47 MiB per archive-served compute
  pipeline).
- All the libraries plus all 2,394 pipelines cost about 1.75 GiB. Libraries alone, for 72% of the metallibs, cost
  1.39 GiB (an MTLLibrary is about twice its metallib's size), and the shader store keeps a library once it is loaded.
  So libraries are most of the cost. The exact split is not measured.
- Round 7's "+270-370 MiB of prebuilt pipelines" was the prebuilt set's libraries and pipelines, made earlier than a
  cold run makes them. A warm start costs extra only for what the run would not have used, or would have used later.
- So releasing an unused pipeline saves nothing; only not making it early does. The bound is now:
  - `GTA6RT_METAL_PCACHE_AHEAD_S=10` (default): a recorded pipeline is prebuilt at most 10 s before its recorded first
    use (for a later phase, counted from that phase's start). 0 = old behaviour.
  - `GTA6RT_METAL_PCACHE_RELEASE_S=30` (was 60).
- Also fixed: the prebuild thread spun on its condition variable while nothing in the queue could start; it now waits
  250 ms.
- Tests: `pcache_runtime_test` checks that a tier entry recorded 1,000 s into its phase stays queued: PASS. Codec and
  archive tests: PASS.
- Opening 2.5-2.6 GB of shards costs +66 MiB footprint and 7.7-8.9 s.

### 4. Warm vs cold title runs (13 GiB cap)

| run | tier | before the world | world+1 s: first uses / ready / not ready | world+5 s | longest first-use stall | end |
|---|---|---|---|---|---|---|
| pc8-warm-c (world at 75.5 s, load 5-7) | on (2,464 entries then) | 1,538 prebuilt (all archive hits, 142.6 s); footprint 11,561 MiB at loading-screen end | 23 / 18 / 0 | - | 13 ms | footprint cap, 14,505 MiB at world+2 s |
| pc8-cold-d (world at 73.8 s, load 5-6) | off | footprint 10,404 MiB | 285 / 0 / 258 | 1,441 / 0 / 1,394 | 35.5 s (at world+40 s, 1,337 still not ready) | watchdog, world+41 s |

- 0 new gpuEvent reports (9 before and after both runs). 0 command-buffer failures.
- Why the warm run hit the cap: at world+2 s Metal held **2,986 MiB of buffers in 552 allocations** (layer "other"
  3,564 MiB); the cold run at world+41 s held 385 MiB in 51. These are the blocks `external_window` sets aside, kept
  resident (Metal memory worker, 04:05-04:20 notes). The tier makes the world ask for its buffers sooner. The fix is in
  trees built from 04:05; mine was built at 03:57, so it did not have it.
- What the tier removes, shown by the cold run: at world+40 s, 1,337 of its 1,517 first-used pipelines were still
  compiling.
- The "backend NotReady skips" counter reads 0 in both runs, so it does not count draws skipped as compiling (the
  backend line is not wired up).
- **Not done:** world+10 and +60 s counts and the "zero still-compiling skips after the first second" check for a warm
  run. They need a run that stays under the cap.

## Round 7 (2026-11-26 22:35 - 11-27 02:00): shards, the world recorded, a half-built world tier, why pipelines are slow

Host as below; load average 6-50 during this round (other workers' titles and compiles; noted per number). Trees:
`build/pc7-x86_64` (super-build, title), `build/pc7-cache-x86_64` (prepare_shaders, pso_probe, tests),
`build/pc7-metal-x86_64` (backend tests). Runs: `cache/tools/pc7_run.sh` (imported save, private run dir
`build/pc7/rundir` reset from a template each run, no audio, hidden window, FSR4 compute PSOs refused for GPU safety),
logs `run/logs/pc7-*.log`, archive dirs `build/pc7/arch-rec` (cold run) and `build/pc7/arch-world` (the world tier).

### 1. Archive shards (port/metal, done, tested)

- The cache is a set of files per key, `gta6rt-<key>-<tag>-<n>.metalarchive` + `.key` (tag = the title phase at write
  time, or the phase prepare_shaders is building). All shards with the key are loaded at open; the old single file
  is read too if its `.key` has a content line.
- A flush seals the in-memory archive (swap under a short lock), serialises it with **no lock a compile needs**,
  re-opens the `.tmp` to verify, writes `.key` (key + size + content hash), then `link()`s it under a new name: a
  written shard is never overwritten (not on a failed write, a stale key, or a torn file). The verified file replaces
  the in-memory archive in the lookup list, so the added binaries leave memory.
- **Metal's reader crashes on a damaged archive** (SIGSEGV in `deserializeBinaryArchiveHeader` on a truncated file,
  measured), so a shard is handed to Metal only when size and content hash match its `.key`; otherwise it is moved to
  `.unreadable`. Hashing 809 MB of shards at open: 2.2 s.
- Size cap: writers flush before a shard's estimate passes 150 MB (`GTA6RT_METAL_PCACHE_SHARD_MB`); prepare_shaders
  also starts a new shard at each phase. Largest shard written: 71.6 MB; none failed (MDB_MAP_FULL was at ~180 MB).
- In the title: `pcache::flush_archive` no longer waits for an idle compile queue (old 120 s deferral meant runs
  under 120 s never wrote anything: the shared archive was last written at 18:49 although seven S4 runs compiled
  700-1,100 world pipelines each); the recorder also flushes when 64 pipelines are pending. pc7-rec-a wrote 14 shards
  in its 248 s, 0.12-0.47 s each, 0 failures, compiles running throughout.
- Tests: `pcache_archive_test` rewritten (cold, warm, a new shard beside an old one, other translator, failed write
  into a read-only directory, stale key, torn shard, 14 async compiles with a flush every 5 ms, then all 14 hit):
  PASS; `pcache_codec`, `pcache_runtime` PASS; port/metal `pipeline_cache_cold/warm/warm2/async` PASS (Metal's
  per-user cache off).

### 2. The world recorded (done)

The S4 worker's seven world runs (20:58-21:48) had already recorded 294-1,376 world first uses each into the shared
manifest (they reached 0.5-22 s of the world). My cold run pc7-rec-a (current live sources, recorder on, private
directory seeded with that manifest, no archive) ran **176 s in the world without a fault** (ended by the 16 GiB
footprint guard). `cache/tools/world_list.py` merges the per-run files by time since each run's world start:
**1,628 pipelines in the world's first 60 s**, first-use order in `build/pc7/lists/world-60.tsv` / `.specs`:
775 in the first second, 555 in 1-5 s, 138 in 5-10 s, 116 in 10-30 s, 44 in 30-60 s; 856 compute, 354 mesh, 418 render.
In 176 s the run first-used 1,918 distinct pipelines. The world now issues 4,000-6,500 commands a frame (round 6's
census: ~480).

### 3. The world tier offline, and warm runs (partly done)

prepare_shaders (resumable; `--title-jobs N` limits it while any gta6rt runs) on the 60 s list into
`build/pc7/arch-world`, plus the cold run's own 14 shards linked in: 40 shards, **812 MB**, covering about 770 of the
1,628 (the first ~1 s of the world, which is the list's order) when the round ended. Time: 556 real compiles,
2,098 s of build time summed (median 0.39 s, p90 6.5 s, longest 265 s: `library/lib_cs_02653` at load 40-50); the
second half stalled for ~1.5 h while the machine's load was 40-50 (1 pipeline in 106 min; the backend test run at the
same time took 11 min instead of 4 s, so Metal's compiler service was starved, not the tool). Resume:
`build/pc7-cache-x86_64/prepare_shaders --specs build/pc7/lists/world-60.specs --archive-dir build/pc7/arch-world --jobs 3 --title-jobs 2`.
Opening all 40 shards in a process: +35-39 MiB footprint, 2.2 s.

| run (world at) | archive | world+1 s: first uses / handed over ready / not ready | world+3-5 s | skipped as compiling | longest first-use stall | end |
|---|---|---|---|---|---|---|
| pc7-rec-a (+71.7 s) | none (manifest only) | 459 / 123 / 336 | +5 s: 1,204 / 123 / 1,079 queued | world+1..11 s: 3,047 a frame, 39,611 in all (13 presents) | 163 s (`lib_cs_00975`) | footprint guard, world+176 s |
| pc7-warm-a (+73.1 s) | 30 shards (tier ~45 %) | 493 / 387 / 107 | +3 s: 935 / 388 / 548 | world+2..12 s: 1,017 a frame, 7,121 in all (7 presents) | 104 s (`lib_cs_02731`) | title SIGSEGV exe+0xfec056d2bd, world+169 s |
| pc7-warm-b (+72.9 s) | 40 shards (tier ~47 %), large-build cap | 707 / **621** / 27 | +3 s: 1,188 / 621 / 507 | world+3 s window: 15.5 a frame, 1,953 | 2.2 s (`lib_cs_01694`) | title access violation exe+0x4e9b54a, world+6 s |

So with the tier built, the world's first second is served almost entirely from it (621 of 707 handed over ready, 27
not ready); everything after it that is not in the tier yet (the unfinished half) still compiles cold, at about 2
pipelines/s on 3 threads. The three runs are at different load (rec-a 7-11, warm-a 12-15, warm-b 11-21). Both
warm-run crashes are in title code and not in cache code as far as the logs show (requests file).

### 4. Why the "65-158 s" world pipelines (measured, `cache/tools/pso_probe.mm`, one process per variant)

- **They are the library compute kernels (`library/lib_cs_*`, 0.02-2.1 MB metallibs), not the 7 huge named kernels**
  (RTShadowTrace32*, ShadowRenderTiledCascades*, ScreenProbes_Activate*: 9-25 MB SPIR-V, 339-486 s, ray tracing; none
  of the 1,918 recorded world pipelines is one of them). The library kernels have no content-key twin among named
  shaders.
- **Most of the old 65-158 s was not compile time**: prepare_shaders timed "in flight" (created to ready) with 6 in
  flight on 3 threads, so small kernels queued behind big ones (`lib_cs_00964`, 45 KB: 153 s in flight, **0.16 s**
  to build); and with Metal's per-user cache off every miss was compiled **twice**: the archive add after a compile
  recompiles (cold: `lib_cs_01056` 20.1 s + add 20.2 s; `lib_cs_00962` 41.1 s + 42.3 s; `lib_cs_02865` 2.0 + 2.0 s).
  With the per-user cache on, the add costs 1-2 ms (`lib_cs_02200`, `lib_cs_01500`). The report now has the build
  time (`compile_seconds`) beside the in-flight time.
- Real cold build times: 0.16 s (45 KB) .. 2 s (280 KB) .. 20-24 s (1.1-1.3 MB) .. 41 s (2.1 MB): it scales with the
  AIR size. **No function constants** in any of them (0 per function), so no specialisation at pipeline time;
  **reflection costs nothing measurable** (`lib_cs_02539`: 24.3 s without, 22.4 s with BindingInfo|BufferTypeInfo).
  The time is Metal's AIR -> GPU code generation.
- `metal-tt -arch applegpu_g16s` on a pipeline script builds the same kernel's binary offline in 32 s wall (24 s CPU),
  i.e. the same compiler cost, off-GPU; its output missed at run time for our descriptor (key mismatch, not
  resolved). So the reductions available are: build them once into shards (this), and in the translator, smaller
  AIR (shaderc_msl's `noinline_hot_helpers` kind of pass) for the 1-2 MB ones; not pipeline options.

### 5. Priority and concurrency (port/metal compile queue; partly measured)

- Demanded before background (as before), now with a **background cap** (threads-1; 1 during story load and world)
  so a demanded build always finds a worker, a **large-build cap** (libraries >= 512 KiB: at most threads-1 at once,
  smaller builds of the same priority go first; `GTA6RT_METAL_COMPILE_LARGE`, `_LARGE_KB`), and **QoS per priority**
  (demand user-initiated, background utility; XPC carries it to MTLCompilerService). The recorder's prebuild puts every
  recorded pipeline before any bootstrap guess, and in the first 10 s of the world (its opening cutscene;
  `GTA6RT_METAL_PCACHE_QUIET_S`) starts only recorded pipelines of the current phase.
- Frame rate against compiler processes (pc7-warm-a, world+40..130 s, 15 s steps of cold `prepare_shaders --jobs 1`
  processes beside it, load 12-15; `cache/tools/compile_load.sh`, `load_fps.py`):

| extra compiler processes | 0 | 1 | 2 | 3 (utility QoS) | 3 (default QoS) | 0 |
|---|---|---|---|---|---|---|
| frames/s (two 5 s windows) | 1.29 | 1.28 | 1.02 | 1.16 | 1.49 | 1.46 |

  Within noise. The world runs at 1.0-1.6 frames/s here because of the layer's per-submission CPU work (~40
  submissions a frame, `heaps` 7.5-10.6 ms each, 57-70% of the wall time in submission), so this does not show what
  3 compilers do to a 20-25 frames/s world; compile threads stay at 3.
- Not done: a cutscene signal (only the world's first seconds are treated as one); a fast lane for archive hits
  queued behind cold builds.

### 6. Memory

- The cache's own cost: shards open +35-39 MiB (812 MB of files). Held prebuilt pipelines: the warm run's footprint
  was +340-370 MiB above the cold run's on the first-run pages and story load and +270 MiB at the world's start
  (10,775 vs 10,502 MiB); those pipelines are the world's own first-second set, which the cold run creates seconds
  later, so the world-phase peak is not raised by it (warm-a peak 15,899 MiB at +178 s; cold rec-a ended at the 16 GiB
  guard). Not proven pipeline by pipeline.
- The descriptor hash table (not my area, request in `run/requests/d3d12_metal.md`): **29 live generations, 1,162 MiB**
  in pc7-rec-a at slow frame rates (8 live, 320 MiB in warm-a). Proposed: a cap with a bounded wait, then exact
  per-queue release marks instead of the 100 ms settle.

### Files (this round)

`port/metal`: `include/gta6rt_metal/gpu.h` (additive fields), `metal_backend.h` (`set_compile_limits`,
`set_pipeline_cache_tag`), `src/mtl_pipeline.mm` (archive section, compile queue, pipeline cost), `src/mtl_internal.h`,
`src/mtl_device.mm` (library byte size). `cache/src/pcache.cpp`, `cache/include/pcache.h`, `cache/tests/archive_test.cpp`,
`cache/CMakeLists.txt`, `d3d12_metal/tools/prepare_shaders.cpp`, new `cache/tools/{pc7_run.sh, world_list.py,
compile_load.sh, load_fps.py, census_skips.py, pso_probe.mm}`. No adapter file (`shims/`, `src/`) touched.

### Next (ranked)

1. Finish the world tier (command above; about 860 cold builds, ~30-60 min on a quiet machine), then one warm run.
2. Hash-table generations (request) and the layer's per-submission `heaps` cost: they decide the world's footprint
   and frame rate far more than the cache.
3. Compaction (rebuild many small run shards into few) and a lane for archive hits behind cold builds.
4. Smaller AIR for the 1-2 MB library kernels (shaderc_msl), or resolve metal-tt's key mismatch.


Date 2026-11-26. Host: Apple M4 Pro (`applegpu_g16s`), macOS 27.0.1 (26A434), x86_64 title process under Rosetta.
Every number was measured on this machine today with other workers' title runs and builds going on (load average
10-45; the load is given beside each number). Title runs are this module's binary
(`port/build/pcache-title-x86_64/gta6rt`, the adapter's super-build plus `cache/`), `GTA6RT_GPU=metal`,
`--hidden-window`, run.sh's kill timer. Logs `port/run/logs/pcm-*.log`, first-lit frames `port/run/present/pcm-*/`,
cache directories `port/build/pcache/runs/*`, offline outputs `port/build/pcache/*`.

A cache does not raise the steady frame rate. What it changes: compile waits at first use (with `NotReady::skip` a
draw whose pipeline is still compiling is dropped, so a wait shows as missing geometry, not as a frozen frame) and the
compile work done while the title runs.

## Summary

- **Landing prompt (5 pipelines).** Cold (no archive, Metal's per-user cache off): 5 first-use compiles, 721 / 867 ms
  of waits, longest 443 / 502 ms (`library/lib_cs_01726`), 87 / 96 draws skipped. Warm (archive + manifest): 5 of 5
  handed over ready at first use, 0 waits, 0 draws skipped. First lit frame (= the landing prompt) 12.2 / 10.7 s cold,
  10.5 / 11.2 s warm at load 12-24: the landing prompt is not pipeline-bound; the difference is within run-to-run noise.
- **Into the world phase.** With the adapter worker's 19:37 sources the Metal runs reach "world (loading screen
  ended)" at 71-100 s (before that, two runs ended at 68-73 s at the adapter's BC5-as-BC7 texture-view assertion).
  The title uses 11-12 pipelines up to the story load and another 17-26 in the last seconds of the loading screen
  (the world's first frame: light-integration passes, decoration blends, 13 library compute shaders) and 1 more in
  the first world seconds before the runs end. Cold for those (pcm-prep-a/b): 23 and 17 compiled at first use, 12.5 s
  and 7.9 s of waits in all, longest 2.3 / 2.6 s; earlier, pcm-big-a's `library/lib_cs_01076` waited 12.4 s.
  Warm on the recorded directory (pcm-world-warm-a): 11 handed over ready, 12 served from the archive at first use
  in 47-103 ms each, 1 new (2.1 s); waits 2.6 s in all. The world's first seconds are where the cold runs drop
  work: the backend logs "command skipped: its pipeline is still compiling" once per pipeline, and the world phase
  has 918 / 1,009 such lines in pcm-prep-a/b and 742 with the cache off (pcm-world-off-a) within 4-7 s: hundreds of
  pipelines first used at once. (pcm-world-warm-a has 0, but its world began at 99.8 s, 5 s before its watchdog, and
  those pipelines were not in its manifest either, so that 0 is not the cache's doing.) Those world first uses were
  never recorded: every run that drew the world crashed inside the recorder's then 10 s write interval (now 2 s,
  below), so the warm-world pair is still to be run.
- **Crashes are not the cache's**: the control with `GTA6RT_METAL_PCACHE=0` (pcm-world-off-a) crashed in the world
  like pcm-prep-a/b (SIGSEGV reading 0x10 at 75 s); and the tree as of 20:33 aborts at 9 s in the title's
  `amd_fidelityfx_upscaler_xbox.dll+0x380d` with the cache on (pcm-world-rec-c, -warm-c, -warm-d) and off (pcm-ctl-e), which
  stopped the last world pair.
- **Bootstrap without any Metal run**: from the Vulkan path's boot manifests and census names alone, 7 of the first
  11 first uses were handed over ready (pcm-far-a).
- **World phase pipelines are the expensive ones**, measured offline (the Metal runs end seconds into the world): the 777
  world-phase pipeline specs (the 687 names the S3 MoltenVK runs reported "still building", with their boot-census
  states) compile at a median 1.6 s, mean 16.9 s, p90 65 s, longest 158 s each (in flight, 3 compile threads, Metal's
  cache off, load 16) and take about 500 KiB of archive each (205 built: 102 MB; one pipeline alone added 15.7 MB).
  Story-load pipelines: median 0.09 s, 21 KiB.
- **Full boot PSO set: not practical as one archive.** 28,504 distinct (shaders, logged state) of the 55,650 graphics
  PSOs the census logged at boot, built offline in 25.4 min (3 compile threads, cache off, load 12-45): 13,578 distinct
  Metal pipelines (13,807 specs were duplicates in Metal's terms), 1,119 refused (approximate state, see tiers). The
  archive serialized at 180 MB with 10,654 pipelines, then **Metal's serializer failed with `MDB_MAP_FULL:
  Environment mapsize limit reached`** and the next serialize wrote 7.7 MB over the 180 MB file: one MTLBinaryArchive
  holds roughly 200-350 MB. Tiers below; the full set needs several archive files (backend change, next steps).
- **Archive writes stall compiles.** The backend serialises the whole archive under the lock every compile needs:
  in the title with a 102 MB archive 3.7, 4.5 and 7.5 s per write (pcm-big-a, load 37-42); offline next to running
  compiles 70 s for 35 MB and 205 s for 58 MB (load 41). The adapter's 20 s writer now goes through
  `pcache::flush_archive(..., when_idle)`: it waits while compiles are queued or running, for at most 120 s.
- **Memory**: opening a 102 MB archive costs 20 MiB of footprint (0.36 s), 117 MB: 31 MiB; pipelines served from it
  cost what the same compiled pipelines cost without it (held or not: within 4 MiB for 205 world pipelines). Footprint
  at 45 s on the landing prompt 5,400-5,425 MiB with or without the cache. The compile service, outside the title's
  footprint, reached 1.3 GiB RSS during the boot-set build and 370 MiB for one world pipeline: a prepared archive keeps
  that out of the world phase.
- **Portability**: an archive written by an arm64 process gives 291 of 291 hits in an x86_64 (Rosetta) process and
  back (Metal's cache off), so process arch need not be in the key. A different translator version, a key file of
  another GPU/OS under the same name, and a torn file are each ignored and replaced (`pcache_archive_test`).

## What exists

| path | what |
|---|---|
| `cache/include/pcache.h`, `cache/src/codec.cpp` | pipeline key and text codec (`encode`/`decode`/`key_of`), manifest (load, merge, atomic save), `archive_version` |
| `cache/src/pcache.cpp` | in-process recorder, bootstrap matching, background prebuild, hand-over at first use, stall watch, governor reclaimer, report, `open_archive`, `flush_archive` |
| `cache/src/phase.cpp` | the title's phase from its state words (RVAs of `host/src/title_watch.cpp`), process age |
| `cache/cache.cmake` | adds the three sources to `gta6rt_d3d12_metal_objs` (one `include` line in `d3d12_metal.cmake`) |
| `cache/CMakeLists.txt` | standalone tree: `prepare_shaders`, tests |
| `tools/prepare_shaders.cpp` | offline archive build from manifests and spec lists, progress and ETA, at most 3 compile threads |
| `cache/tools/bootstrap.py` | `pcache-bootstrap.txt` from the Vulkan boot manifests and the S3 census logs |
| `cache/tools/boot_set.py`, `pcache_text.py` | the boot PSO set as specs from the pipeline-library records and the PSO census logs |
| `cache/tools/tiers.py` | per-phase spec lists (tier files) |
| `cache/tools/pcache_run.sh`, `pcache_series.sh`, `pcache_world.sh`, `pcache_summary.py` | the title runs behind this file and their summary |
| `cache/tests/codec_test.cpp` | 2,000 random descriptions round-trip, key stability, manifest merge rules, atomic save, torn-file refusal |
| `cache/tests/runtime_test.cpp` | prebuild + hand-over + demand-created + bootstrap match + manifest merge on a real device and store metallibs |
| `cache/tests/archive_test.cpp` | archive keying and invalidation (translator version, key of another GPU/OS, torn file) |

Tests: `ctest --test-dir port/build/pcache-x86_64` and `-arm64`: 3/3 pass on both arches; runtime and archive tests
also pass under `MTL_DEBUG_LAYER=1`. Under `MTL_SHADER_VALIDATION=1` the archive test fails as expected: Metal misses
every archive lookup with shader validation on (port/metal STATUS says the same).

### Hooks in the adapter (`shims/d3d12_metal_device.cpp`; announced and agreed in `port/run/requests/d3d12_metal.md`)

`#include "pcache.h"`; `~PipelineState`: `pcache::forget(this)`; `create_graphics_impl` / `create_compute_impl`: one
`pcache::created(...)` after the PSO is registered; `ensure_built` / `ensure_rect`: the four
`device_.gpu->create_*_pipeline` calls are `pcache::make_*` (probed mesh PSOs still call the backend); archive block:
`pcache::open_archive` with `pcache::archive_version(...)`, then `pcache::start(...)`; the 20 s writer and
`save_pipeline_caches_at_exit` write through `pcache::flush_archive`; `~Device` calls `pcache::stop()`;
`save_pipeline_caches_at_exit` and `end_run` call `pcache::flush(...)`. `d3d12_metal.cmake`: one
`include(.../cache/cache.cmake)` line.

### Key, manifest, phases

- **Pipeline key** = FNV-1a 64 of a canonical text of everything the backend pipeline is made from: kind, shader names
  with the store's content key (16 hex), input layout, render-target count and formats, all 8 blend states, depth
  format, sample count, raster and depth-stencil state (the backend pipeline object carries them), topology type (the
  rect-list variant is its own entry: topology 19), rect mode, D24 mode; mesh: threads, payload, precheck; compute:
  threadgroup size. Labels, compile mode, priority and the root-layout pointer are not in it. The text is what the
  manifest stores, so an entry can be rebuilt with no title running.
- **Manifest** `pcache-manifest.txt` beside the archive: `key, phase, t, runs, text`, ordered by phase then time.
  Merge across runs: union; an entry keeps its earliest phase (and that run's time); `runs` counts the runs that used
  it. Written every 2 s while it grows (10 s until 20:33), at every phase change, at exit and in the adapter's
  `end_run`, as tmp + fsync + rename; a
  torn or foreign file is moved to `.unreadable`, never half used. Per run, `pcache-runs/<time>-<pid>.tsv` lists each
  first use (`t, phase, key, adopted-ready | adopted-compiling | created, label`) and each wait (`ms, archive |
  compiled`).
- **Phases**: boot, landing, movie, first-run, story-load, world, read every 250 ms from the title's game state,
  session and loading byte (the same words as the host's stage lines). "movie" is landing after the first pipeline
  with a Bink shader is used.
- **Bootstrap** `pcache-bootstrap.txt` (`bootstrap.py`): 941 PSO keys from the Vulkan path's boot manifests (the
  layer's `pipeline_key`, which the Metal copy computes the same way) and 705 pipeline names from the "still building"
  census lines of 15 S3 logs (s3*, r4vk-s3-*, gov-s3-*), each with the phase of the stage line before it (687 world,
  16 story-load, 2 first-run). At PSO creation a matching key or name (at most 4 PSOs per name) queues that PSO's
  pipeline at once.

### Prebuild and hand-over

- At start-up (after the archive opens) the manifest's entries are queued in phase order. One prebuild thread (QoS
  utility) loads libraries through the adapter's shader store and creates the backend pipeline async at
  `Priority::background`; at most 2 are in flight (`GTA6RT_METAL_PCACHE_INFLIGHT`), so of the backend's 3 compile
  threads one is always free for the title's own first uses. Never a title thread.
- At first use (`ensure_built` / `ensure_rect`) the pipeline with the same key is handed over: ready (no wait), or
  still compiling, then moved to `Priority::demand` (`precompile`) so it jumps the queue. Otherwise created as before.
- Held (kept for hand-over): every prebuilt entry, at most 4,096 (`GTA6RT_METAL_PCACHE_HOLD`); past that they are
  built into the archive and released. Holding costs no measurable footprint (below). Until 20:10 the hold covered only
  entries up to one, then two phases ahead; pcm-world-warm-a shows why that was wrong: its story-load entries were
  built at boot, released, re-queued at the first-run pages, but the governor paused the prebuild at level 2 during the
  story load, so 12 of them were created at first use from the archive (47-103 ms each) instead of handed over.
- Governor (`include/gta6rt/governor.h`, action `GTA6RT_GOV_COMPILE_SHRINK`): level 1 pauses the prebuild, level 2
  also releases held pipelines (the archive keeps them), level 0 resumes. Seen in pcm-far-b, pcm-big-a and
  pcm-world-warm-a (story load, footprint 10-12 GiB).
- `GTA6RT_METAL_PCACHE=0`: no recorder, no prebuild; the hooks call the backend exactly as before.

### Archive

- The backend's `MTLBinaryArchive` (port/metal, unchanged). Key = this module's `archive_version` (translator line of
  `shaderc_msl/warmup/translator_version.txt`: recipe, spirv-cross, Metal toolchain and translator fingerprints; the
  catalog format `v3`; `pcache 1`) + the backend's GPU name, OS version and build, process arch. A different key is a
  different file; a stale key file under the same name is ignored and overwritten.
- `open_archive`: a file Metal cannot read (torn, foreign) is moved to `.unreadable` and a new archive started.
  Before, the adapter then ran with no archive at all (Metal: "has an invalid format").
- Written by the adapter's flusher every 20 s while it grows, and at exit (serialize to `.tmp`, rename), through
  `pcache::flush_archive`: (1) the periodic write waits while the backend's compile queue has work, at most 120 s
  since the last write, because a serialize holds the lock every compile needs (3.7-7.5 s per write of a 100 MB
  archive in pcm-big-a; 70 s for 35 MB and 205 s for 58 MB offline next to compiles); (2) after one failed serialize
  (MDB_MAP_FULL) no further write in that run, because Metal's in-memory archive is damaged then and a later serialize
  wrote 7.7 MB over the 180 MB file. Runs end on a kill timer or a crash, so what was compiled since the last write can
  be lost (the manifest, written every 2 s and at each phase change, still names it, so the next run's prebuild
  compiles it in the background), never the file.

## Measurements

### Title runs (Metal path)

Landing prompt runs: 45 s, no presses after the prompt (`pcache_series.sh landing`). "Cold" = new archive directory
and `MTL_SHADER_CACHE_SIZE=0` (Metal's per-user cache off for the title process only); "warm" = the same directory
again (archive + manifest from the cold run).

| run | load start/end | archive | landing stage | first lit frame (prompt) | first uses | compiled at use | handed over ready | waits (sum / longest) | draws skipped (not ready) | footprint at 45 s |
|---|---|---|---|---|---|---|---|---|---|---|
| pcm-cold-a | 24.2 / 17.6 | new | 9.1 s | 12.2 s | 5 | 5 | 0 | 721 / 443 ms | 87 | 5,409 MiB |
| pcm-warm-a | 17.6 / 14.1 | 0.3 MiB, 0.002 s | 8.1 s | 10.5 s | 5 | 0 | 5 | 0 | 0 | 5,425 MiB |
| pcm-cold-b | 14.1 / 12.8 | new | 8.1 s | 10.7 s | 5 | 5 | 0 | 867 / 502 ms | 96 | 5,411 MiB |
| pcm-warm-b | 12.8 / 14.1 | 0.3 MiB, 0.003 s | 8.6 s | 11.2 s | 5 | 0 | 5 | 0 | 0 | 5,404 MiB |
| pcm-sys-a (Metal's cache on, no archive) | 14.1 / 13.3 | new | 8.6 s | 11.4 s | 5 | 5 | 0 | 545 / 238 ms | 34 | 5,400 MiB |
| pcm-noprebuild-a (`GTA6RT_METAL_PCACHE=0`, archive loaded) | 17.1 / 16.7 | 0.3 MiB | 9.6 s | 13.1 s | 5 (backend count) | 0 (5 archive hits) | - | not measured | 4 pipelines skipped draws (backend log) | 5,390 MiB |

Past the landing prompt (100-120 s runs, presses at 40/55/70/85 s, `pcache_series.sh far|prepared|prepared2|world`,
`cache/tools/pcache_world.sh`): first-run pages at 41 s, story load at 56 s in every run that booted; the ends
are in the last column. Phases in the first-uses column are the recorder's (it reads the title's state every 250 ms,
so a first use in the first moment of a phase can carry the phase before).

| run | load | cache directory before the run | first uses by phase (boot/landing/movie/first-run/story-load) | handed over ready | compiled at use (waits) | end |
|---|---|---|---|---|---|---|
| pcm-far-a | 10.4 / 15.9 | bootstrap only | 3/2/4/2/- | 7 (bootstrap) | 4 (56, 56, 98, 98 ms) | abort 68 s |
| pcm-far-b | 15.9 / 15.5 | far-a's archive + manifest + bootstrap | 3/2/4/2/1 | 9 | 3: 2 from the archive (12.5 ms each, the first-run entries were outside the then hold window of one phase ahead, now two), 1 new (63.5 ms) | abort 73 s |
| pcm-big-a | 36.8 / 42.3 (world-tier build running next to it) | 102 MB world-tier archive (offline) + far's manifest | 3/2/4/-/8 | 12 | 5: 77 ms, 163 ms, 588 ms, 1.3 s, 12.4 s (`library/lib_cs_01076`) | watchdog 120 s, story load; archive writes 3.7 / 4.5 / 7.5 s |
| pcm-prep-a | 8.5 / 11.0 | prepared offline from far's manifest (12) | 3/2/4/2/24 | 12 | 23 (18 waits, 12.5 s in all, longest 2.3 s) | **world at 71.0 s**, SIGSEGV 78 s |
| pcm-story-b | 11.0 / 11.7 | big-a's directory + bootstrap | 5/2/2/-/26 | 11 | 24 (waits lost in the crash) | world at 71.5 s, abort (signal 6, no message) 75 s |
| pcm-prep-b | 10.0 / 10.4 | prepared offline from far's manifest (12) | 3/2/4/2/18 | 12 | 17 (13 waits, 7.9 s in all, longest 2.6 s) | world at 71.5 s, SIGSEGV 78 s |
| pcm-world-warm-a | 10.7 / 29.6 | prep-a's directory after its run (35 entries) | 3/2/2/2/14 + world 1 | 11 | 13: 12 from the archive (47-103 ms), 1 compiled (2.1 s); waits 2.6 s in all | world at 99.8 s, watchdog 105 s |
| pcm-world-off-a (control, `GTA6RT_METAL_PCACHE=0`, new directory) | 18.2 / 12.1 | none | not recorded | - | 12 Metal pipelines at 60 s; 742 skip lines in the world | world at 69.0 s, SIGSEGV 75 s |
| pcm-world-rec-c, -warm-c, -warm-d, pcm-ctl-e (cache off) | 9.7-10.7 | prep-b's directory / none | - | - | - | all four: abort at 7-9 s in `amd_fidelityfx_upscaler_xbox.dll` (tree as of 20:33) |

### Offline prepare (`prepare_shaders`)

`prepare_shaders --manifest M | --specs S ... --archive-dir D --jobs 3` builds into the same archive file the title
opens (same backend and key; build it x86_64, or copy the file: the content is arch-independent, below). Progress
every 10 s with rate, ETA, footprint, its own compile-service processes and their RSS, load; per-pipeline report with
`--report`. A rerun resumes: what the last flush wrote are archive hits (about 1-3 ms each).

| input | pipelines | distinct in Metal | wall | archive | notes (all `MTL_SHADER_CACHE_SIZE=0`, 3 compile threads) |
|---|---|---|---|---|---|
| far manifest (exact, what the title used) | 12 | 12 | 1 s | 0.8 MiB | prep-a / prep-b |
| census sample 300 (seed 1) | 300 | 290 | 9 s (cache on, first time) / 27 s (cache off) / 0.5 s (cache on, second time) | 18.1 MiB (64 KiB each) | archive hits: 291 in 0.35 s, also with the cache off |
| tier story-load | 238 | 122 | 9 s | 2.5 MiB (21 KiB each) | 3 shaders not in the store |
| tier world (part) | 205 + 78 of 777 | 204 + 78 | 11 min (load 16) + 13.6 min (load 41) | 102 MB + 58 MB in two files (about 500-750 KiB each) | median 1.6 s, p90 65 s, max 158 s in flight; writes of the second file took 70 s (35 MB) and 205 s (58 MB) next to the compiles; rest: `tiers/tier-world-2.specs` |
| census boot set | 28,504 | 13,578 | 25.4 min | serialize failed past 180 MB (MDB_MAP_FULL) | 1,119 refused: 540 integer RTs with a float PS, 324 dual-source blend, 254 depth format 118, 1 vertex format |

The per-user Metal cache matters: with it on, a second build of the same 300 took 0.5 s with no archive; with it off,
3x the first-time cost. It is shared by every app and process of the user and trims itself (796 MB at 18:20, 408 MB
at 19:10 with nothing deleted by me), so it is not a cache to plan the world phase on; the archive is.

### Tiers

| tier | pipelines | build (3 threads, cache off) | archive | source of truth |
|---|---|---|---|---|
| boot path (boot, landing, movie) | 9 | under 1 s | 0.6 MiB | recorder manifest (exact) |
| first-run | 2 (+5 specs) | under 1 s | 0.2 MiB | manifest + bootstrap names |
| story load | 1 exact; 238 specs for the 16 bootstrap names | 9 s | 2.5 MiB | manifest + `tier-story-load.specs` |
| world first frame | recorded on Metal: 17-24 at the end of the loading screen (exact) + the world's own (not recorded yet); offline: 777 specs for the 687 "still building" names | recorded set: seconds offline (in pcm-prep-a its 23 compiled at use with 12.5 s of waits); specs: about 1-1.5 h (283 in 25 min at load 16-41) | specs: about 400-550 MB, over one archive's limit (2-3 files) | manifest (exact) for what Metal runs reached; `tier-world.specs` (approximate state) for the rest |
| full boot PSO set | 28,504 specs / 13,578 Metal pipelines | 25.4 min (most of it the world tail) | over the limit; at least 2-3 files | `census.specs`, `records.specs` |

The boot-set specs are approximate: the census logged topology, target count, RT0/RT1/DSV formats, samples and RT0
blend; input formats, blend ops and alpha factors, targets 2-7 are guessed the same way for all
(`boot_set.py` header). They measure cost and size; they serve the title only where the guess equals the title's
description. The recorder's entries are exact, so the plan is: prepare the tiers from specs for the cost and the
compile-service warm-up, and from the manifest once Metal runs reach those phases.

### Memory and disk

- Archive on disk: landing 0.3 MiB, through story load 0.8-14 MiB (with bootstrap prebuilds), world tier 102 MB for
  205 of 777 pipelines. The adapter's old archive (6.7 MB) is not reused (new key).
- Archive in memory: open 102 MB: +20 MiB footprint, 0.36 s; 117 MB: +31 MiB, 0.25 s; 18 MB: +0.2 MiB, 0.04 s. Hits on
  205 world pipelines: footprint +81-89 MiB (181 metallibs loaded plus the pipelines), the same held or released, and
  what compiling them would cost. Adding to an archive keeps the new binaries in memory until exit (offline: +93 MiB
  for 938 added).
- Held pipelines: 960 held vs released: no difference above the 20 MiB noise of the measurement.
- Title footprint on the landing prompt at 45 s: 5,390-5,425 MiB in all six runs, cache on or off.
- Metal's per-user cache (`$(getconf DARWIN_USER_CACHE_DIR)com.apple.metal`): 408 MB (functions) + 20 MB (MSL
  libraries) now, shared and self-trimming. Not deleted by me.
- Compile service (outside the title's footprint): up to 1.3 GiB RSS during the boot-set build, 370 MiB for one world
  pipeline.

### Portability (item 6)

- Process arch: arm64-written archive in an x86_64 process: 291/291 hits; x86_64-written in arm64: 291/291 (Metal's
  cache off). The backend still keys on the arch, so the file must be renamed (or the key changed in port/metal).
- Invalidate on mismatch (`pcache_archive_test`, both arches): another translator version: new file, 0 hits; same
  file with another GPU/OS key: not loaded, 0 hits, replaced; torn file: Metal refuses it, moved aside, new archive;
  the run after: 4/4 hits.
- Another machine with the same GPU family and OS build: not testable here (one machine). Shipping per GPU family is
  the supported route of Apple's toolchain: `metal-tt` (present, Metal toolchain 27.2) translates AIR for a named
  `-arch` (this GPU: `applegpu_g16s`, `xcrun metal-arch`) or `-gpu-family` from pipeline scripts, so archives can be
  built per architecture without that hardware. A runtime-harvested archive like ours is tied to the GPU and the
  compiler build: ship it only keyed by GPU architecture + OS build, and treat any other as a miss (what the key does).

### What carries over to the MoltenVK path

MoltenVK has only Metal's per-user cache (the layer's session file is not read or written there). The archive does not
carry over: MoltenVK compiles its own MSL from SPIR-V, so its functions are different archive entries. What does:
the recorder and manifest (phases, merge, first-use order) generalise the Vulkan path's `boot_manifest.txt`, which
has no phases; the bootstrap matching at PSO creation is that manifest's mechanism; the prebuild rules (background
only, one compile thread left for demand, demand jumps the queue, held window, governor pause) match its warm-up
rules. A MoltenVK prepare step would have to run the pipelines through MoltenVK offline to fill the per-user cache,
which trims itself (above); measured cost not done.

## Not done / gaps

- The world phase on the title: reached (71-100 s) but every run ends 4-7 s in, so no warm run of the world's first
  frame with its pipelines recorded yet; the recorder now writes every 2 s while it grows (built and unit-tested, not
  yet in a title run because of the 20:33 boot crash).
- The world tier is built for 283 of 777 specs (two files, 102 + 58 MB); the full set does not fit one archive.
- The backend serialises the whole archive under its archive mutex (3.7-7.5 s per write of 100 MB in the title,
  31.5 s for 180 MB and 205 s for 58 MB offline). `flush_archive(when_idle)` keeps the periodic write away from
  running compiles (measured: deferrals logged in pcm-story-b), but a write that starts still blocks any compile that
  starts during it; only the backend can fix that (next steps 1).
- The archive's pipelines need the shader store's metallibs at the same content keys; a corpus rebuild that changes a
  shader changes its key, so that entry misses (by design) and is rebuilt.
- Tier specs carry guessed state; only recorded entries are exact.

## Next steps (ranked)

1. port/metal: several archives at once (`binaryArchives = @[read-only shards..., writable]`, each under ~150 MB) and
   serialise outside the archive mutex, only the writable one. Without it the world tier does not fit and a large
   archive's flush stalls compiles.
2. Once the tree boots again: `d3d12_metal/cache/tools/pcache_world.sh` (record the world's first uses on prep-b's directory,
   then two warm runs) for cold vs warm at the world's first frame; then `prepare_shaders` on that manifest.
3. Finish the world tier: rerun `prepare_shaders --specs build/pcache/tiers/tier-world-2.specs --archive-dir
   build/pcache/tiers/archive-world-2 --jobs 3` (resumes: the 78 written are hits; about 1 h more at load 16-40),
   split so no file passes ~150 MB, once 1 lands.
4. Drop the process arch from the backend's archive key (measured portable) and key on GPU architecture + OS build;
   then try `metal-tt` per-architecture archives for shipping.
5. Decode the graphics state blob of the pipeline-library records (leonida shader_library.md: not decoded) or record
   the full PSO census on Metal, so offline specs are exact.

## Reproduce

```
cmake -S port/d3d12_metal/cache -B port/build/pcache-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 && ninja -C port/build/pcache-x86_64 && ctest --test-dir port/build/pcache-x86_64
cmake -S port -B port/build/pcache-title-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=$PWD/port/d3d12_metal/superbuild/attach.cmake && ninja -C port/build/pcache-title-x86_64 gta6rt
python3 -I port/d3d12_metal/cache/tools/bootstrap.py --port $PWD/port --out port/build/pcache/bootstrap/pcache-bootstrap.txt
python3 -I port/d3d12_metal/cache/tools/boot_set.py --port $PWD/port --out port/build/pcache/bootset
python3 -I port/d3d12_metal/cache/tools/tiers.py --specs port/build/pcache/bootset/census.specs --specs port/build/pcache/bootset/records.specs \
        --bootstrap port/build/pcache/bootstrap/pcache-bootstrap.txt --out port/build/pcache/tiers
port/build/pcache-x86_64/prepare_shaders --manifest DIR/pcache-manifest.txt --archive-dir DIR --jobs 3
port/d3d12_metal/cache/tools/pcache_series.sh landing|far|prepared
python3 -I port/d3d12_metal/cache/tools/pcache_summary.py port/run/logs/pcm-*.log
```
