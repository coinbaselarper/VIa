# port/d3d12_metal/cache: pipeline cache for the native Metal path (status)

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
- **Past the landing prompt** (movie, first-run pages, story load; two runs ended 68-73 s in at a Metal texture-view
  assertion of the adapter, BC5 viewed as BC7, reported to the adapter worker; one ran its 120 s into the story
  load): 11-17 pipelines in all. With the bootstrap from the Vulkan path's data alone (no earlier Metal run) 7 of 11
  first uses were handed over ready; the second run 9 of 12; pcm-big-a 12 of 17 handed over, and the 5 new ones were
  story-load compute pipelines that compiled at use in 77 ms-12.4 s (`library/lib_cs_01076`: 12.4 s, the one the
  Vulkan census also reports "still building").
- **World phase pipelines are the expensive ones**, measured offline because Metal runs do not reach it yet: the 777
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
| `cache/include/pcache.h`, `cache/src/codec.cpp` | pipeline key and text codec (`encode`/`decode`/`key_of`), manifest (load, merge, atomic save), `archive_version`, `open_archive`, `flush_archive` |
| `cache/src/pcache.cpp` | in-process recorder, bootstrap matching, background prebuild, hand-over at first use, stall watch, governor reclaimer, report |
| `cache/src/phase.cpp` | the title's phase from its state words (RVAs of `host/src/title_watch.cpp`), process age |
| `cache/cache.cmake` | adds the three sources to `gta6rt_d3d12_metal_objs` (one `include` line in `d3d12_metal.cmake`) |
| `cache/CMakeLists.txt` | standalone tree: `prepare_shaders`, tests |
| `tools/prepare_shaders.cpp` | offline archive build from manifests and spec lists, progress and ETA, at most 3 compile threads |
| `cache/tools/bootstrap.py` | `pcache-bootstrap.txt` from the Vulkan boot manifests and the S3 census logs |
| `cache/tools/boot_set.py`, `pcache_text.py` | the boot PSO set as specs from the pipeline-library records and the PSO census logs |
| `cache/tools/tiers.py` | per-phase spec lists (tier files) |
| `cache/tools/pcache_run.sh`, `pcache_series.sh`, `pcache_summary.py` | the title runs behind this file and their summary |
| `cache/tests/codec_test.cpp` | 2,000 random descriptions round-trip, key stability, manifest merge rules, atomic save, torn-file refusal |
| `cache/tests/runtime_test.cpp` | prebuild + hand-over + demand-created + bootstrap match + manifest merge on a real device and store metallibs |
| `cache/tests/archive_test.cpp` | archive keying and invalidation (translator version, key of another GPU/OS, torn file) |

Tests: `ctest --test-dir port/build/pcache-x86_64` and `-arm64`: 3/3 pass on both arches.

### Key, manifest, phases

- **Pipeline key** = FNV-1a 64 of a canonical text of everything the backend pipeline is made from: kind, shader names
  with the store's content key (16 hex), input layout, render-target count and formats, all 8 blend states, depth
  format, sample count, raster and depth-stencil state (the backend pipeline object carries them), topology type (the
  rect-list variant is its own entry: topology 19), rect mode, D24 mode; mesh: threads, payload, precheck; compute:
  threadgroup size. Labels, compile mode, priority and the root-layout pointer are not in it. The text is what the
  manifest stores, so an entry can be rebuilt with no title running.
- **Manifest** `pcache-manifest.txt` beside the archive: `key, phase, t, runs, text`, ordered by phase then time.
  Merge across runs: union; an entry keeps its earliest phase (and that run's time); `runs` counts the runs that used
  it. Written every 10 s, at every phase change, at exit and in the adapter's `end_run`, as tmp + fsync + rename; a
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
- Held (kept for hand-over) are entries up to two phases ahead, at most 4,096 (`GTA6RT_METAL_PCACHE_HOLD`); later
  phases are built into the archive and released, and queued again (archive hit, about 1 ms) when the title gets
  within two phases. Governor level 2+ (`GTA6RT_GOV_COMPILE_SHRINK`, `include/gta6rt/governor.h`) pauses the prebuild
  and releases held pipelines (seen once in pcm-far-b: 4 released).
- `GTA6RT_METAL_PCACHE=0`: no recorder, no prebuild; the hooks call the backend exactly as before.

### Archive

- The backend's `MTLBinaryArchive` (port/metal, unchanged). Key = this module's `archive_version` (translator line of
  `shaderc_msl/warmup/translator_version.txt`: recipe, spirv-cross, Metal toolchain and translator fingerprints; the
  catalog format `v3`; `pcache 1`) + the backend's GPU name, OS version and build, process arch. A different key is a
  different file; a stale key file under the same name is ignored and overwritten.
- `open_archive`: a file Metal cannot read (torn, foreign) is moved to `.unreadable` and a new archive started.
  Before, the adapter then ran with no archive at all (Metal: "has an invalid format").
- Written by the adapter's flusher every 20 s while it grows, and at exit (serialize to `.tmp`, rename). Runs end on
  the kill timer, so the last 20 s may be lost, never the file. `flush_archive`: after one failed serialize (MDB_MAP_FULL)
  no further flush in that run, because Metal's in-memory archive is damaged then and would overwrite the good file
  with a fraction.

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
| pcm-noprebuild-a (`GTA6RT_METAL_PCACHE=0`, archive loaded) | 17.1 / 16.7 | 0.3 MiB | 9.6 s | 13.1 s | - | - | - | - | - | 5,390 MiB |

Past the landing prompt (100 s runs, presses at 40/55/70/85 s, `pcache_series.sh far|prepared`): every run reached
first-run pages (41 s) and story load (56 s) and ended at the adapter's BC5-as-BC7 texture-view assertion.

| run | load | cache directory before the run | first uses by phase (boot/landing/movie/first-run/story-load) | handed over ready | compiled at use (waits) | end |
|---|---|---|---|---|---|---|
| pcm-far-a | 10.4 / 15.9 | bootstrap only | 3/2/4/2/- | 7 (bootstrap) | 4 (56, 56, 98, 98 ms) | abort 68 s |
| pcm-far-b | 15.9 / 15.5 | far-a's archive + manifest + bootstrap | 3/2/4/2/1 | 9 | 3: 2 from the archive (12.5 ms each, the first-run entries were outside the then hold window of one phase ahead, now two), 1 new (63.5 ms) | abort 73 s |
PREPARED_ROWS

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
| tier world (part) | 205 of 777 | 204 | 11 min | 102 MB (about 500 KiB each) | median 1.6 s, p90 65 s, max 158 s in flight; rest: `tiers/tier-world-2.specs` |
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
| world first frame | 777 specs for the 687 "still building" names | about 40-45 min (205 in 11 min) | about 385 MB at 500 KiB each: over one archive's limit, needs 2 files | `tier-world.specs` (approximate state) until a Metal run records it |
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

- No Metal run reaches the world phase yet (adapter assertion at 68-73 s), so the world-phase proof on the title is
  offline only (costs above), not cold-vs-warm in the title.
- The world tier is built for 205 of 777 specs; the full set does not fit one archive.
- The backend serialises the whole archive under its archive mutex: 31.5 s for 180 MB offline; in the title every
  20 s while it grows. With a large prepared archive and new pipelines added in the world phase that would block
  compile threads for seconds (measured offline only). See next steps.
- Tier specs carry guessed state; only recorded entries are exact.

## Next steps (ranked)

1. port/metal: several archives at once (`binaryArchives = @[read-only shards..., writable]`, each under ~150 MB) and
   serialise outside the archive mutex, only the writable one. Without it the world tier does not fit and a large
   archive's flush stalls compiles.
2. The adapter's BC5-as-BC7 view assertion (reported), then Metal runs into the story load and the world phase:
   record the world's exact pipelines, prepare them, and measure cold vs warm world first frame on the title.
3. Finish the world tier (`prepare_shaders --specs build/pcache/tiers/tier-world-2.specs`, about 30 min) into a
   second archive file once 1 lands.
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
