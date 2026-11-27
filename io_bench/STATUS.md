# io_bench status (S3.4 prep: "DStorage -> macOS file IO backend")

Storage throughput for story load. Standalone module `port/io_bench`. **Status after the integration round (2026-11-26, later): both proposals are applied to the live tree** (`port/shims_gdk` dstorage sources and `port/shims_core/src/file_system.cpp`); sections 1-10 describe the measurements and the design as reviewed, section 11 is what the integration changed, re-measured and found (applied patch, tests, A/B re-run, the BCPack profile, the first real title request shapes, request tracing, pool tuning). Every number below was measured on this Mac
on 2026-11-26 and is in `results/*.txt` (one `RESULT` line per run; `scripts/summarize.py` takes medians over the trials). Where a sentence gives a cause it says "likely" unless a measurement isolates it.

## 1. Result

- **Today's DStorage backend** (`port/shims_gdk/src/dstorage.cpp`): one worker thread per queue plus one helper that can overlap one adjacent pair, a 64 MiB shared staging window,
  `pread` through the kernel32 rows into the page cache, `leonida::zlib_inflate`. Replaying real zlib request shapes from the real archives through the shim itself (3 queues, section 8):
  **307 MiB/s out** (levels.rpf streams), **1,277** (core_bvh4.rpf), **403** (TTPF texture chunks), i.e. 147 / 74 / 155 MiB/s
  of stored bytes read, 1-2% of the SSD's 6,274 MiB/s sequential ceiling. It scales with the number of queues (levels 115 / 307 / 480 MiB/s at 1 / 3 / 6)
  and roughly doubles with a faster inflate (section 7); by construction each queue does read, then inflate, one request at a time.
- **Recommended backend** (`proposals/dstorage_io_backend.patch`, **applied; see section 11**): `F_NOCACHE` reads on the native descriptor, a shared prepare pool of 1.5 x cores threads (21 here) fed by a 32-request window per queue with in-order
  publishing, a 256 MiB staging window, system zlib for the inflate (the leonida inflater stays as the error path). Same replay through the patched shim, 3 queues:
  **1,605 / 6,626 / 2,488 MiB/s = 5.2x / 5.2x / 6.2x**
  (Rosetta: 4.2x / 3.7x / 5.6x), CPU per GiB out 8.4 -> 3.8 s (levels).
  All 489 `dstorage_test` checks and a new ordering/integrity stress test pass with the patch on arm64 and x86_64 (Rosetta).
- **The page cache is the hazard on a 24 GiB machine.** Streaming 68 GiB of levels.rpf with ordinary cached reads grew the file cache by 5,422 MiB, pushed the compressor from 2,872 to 7,221 MiB
  with 1,666,181 decompressions, and a 2 GiB resident anonymous working set took a median 372 ms per touch sweep (baseline 1.0 ms). With `F_NOCACHE` the same stream: cache +311 MiB,
  sweep 1.3 ms, 5,323 MiB/s instead of 3,934, 2.0 s of CPU instead of 14.9 s.
- **Decompression, not the SSD, is the ceiling on the stored data.** Per thread, arm64 / Rosetta (interleaved A/B on this loaded machine): system zlib 552 / 421 MiB/s out, leonida inflater 220 / 196,
  leonida BCPack BC1 decoder 48 / 37. BCPack is 20% of the texture bytes (`textures.md`); with all 14 cores it decodes 418 (arm64) / 173 (Rosetta) MiB/s out against 6,274 for the SSD.
- **What the title requests**: the S1 run logs under `port/run/logs` hold 20 real title requests (only failures are traced): all BCPack BC7 (`options=0x20000e`) into physical page lists, one REALTIME queue, from TTPF chunks of `ttdb_graphics.ttpf`, each matching the TTPF chunk table byte for byte (section 3).
  Formats: **zlib (RFC 1950) and BCPack BC1-BC7 (+ optional swizzle); no GDeflate** anywhere in leonida or port. Decompressors in leonida: `leonida::zlib_inflate`, `leonida::bcpack::decode`, `dstorage_swizzle.h`. None runs on the GPU.

## 2. Deliverables and integration

| path | what |
|---|---|
| `CMakeLists.txt`, `src/*.cpp` | `iobench`: shapes, read replay (pread / mmap / dispatch_io), codec benchmarks, a model of the shim's queue behaviour, page-cache pressure study |
| `scripts/run_suite.sh` | the reusable run: `scripts/run_suite.sh $PWD/../build/io_bench-<arch>/iobench out/ quick` (about 10 minutes here) or `full` (roughly 40 minutes, summed from the individual experiments); x86_64 runs go through `with_timeout.sh` |
| `scripts/e*.sh`, `scripts/summarize.py` | the individual experiments behind the tables, and the median/pivot tool |
| `shapes/*.iobs` | request shapes measured from the archives (binary; `iobench info shapes/x.iobs`; regenerate with `iobench shapes`) |
| `tests/dstorage_pool_stress_test.cpp`, `tests/dstorage_replay_bench.cpp`, `tests/native_fd_row.cpp` | programs against the real shim API: ordering/integrity stress, the real-shim A/B replay, and a stand-in for the `FileNativeFd` row |
| `scripts/build_shim_ab.py`, `scripts/make_requests.sh`, `scripts/e10_shim_ab.sh`, `scripts/e12_raw_ab.sh` | build the programs above against the shim as it is and with the patch applied to a copy, write the request lists, run the A/B (section 8) |
| `proposals/dstorage_io_backend.patch` | patch for `port/shims_gdk` (`patch -p1` from `port/shims_gdk`), **applied** (kept as the record; the live source has moved on, e.g. request tracing) |
| `proposals/shims_core_file_native_fd.patch` | the 6-line hook the patch wants in `port/shims_core/src/file_system.cpp`, **applied** |
| `baseline/dstorage.cpp`, `baseline/dstorage_staging.h` | the backend as it was before the patch (md5 `c36b7004...`, `edc22362...`): the "baseline" side of every A/B from section 11 on |
| `scripts/ensure_bcpack_history.sh`, `scripts/e14_bcpack_profile.sh` | write the BCPack predictor profile when missing; BC6H/BC7 decode throughput and failures by cause |
| `scripts/capture_title_shapes.sh`, `scripts/dstorage_shapes.py`, `scripts/title_threads.py` | one title run with request tracing on and a CPU census; summary and replay-list writer for the trace; census reader |
| `scripts/e13_ab_confirm.sh`, `e15_title_shape_ab.sh`, `e16_pool_vs_title.sh`, `e17_read_chunk.sh`, `ab_table.py`, `pool_table.py`, `tests/nocache_probe.c` | the A/B re-run, the A/B on the title's own request list, the pool sweep against busy threads, the Rosetta read cap, the F_NOCACHE residency probe |
| `results/` | every measurement; `shapes_info.txt` is the shape census |

Build (standalone, `-j2`): `cmake -S port/io_bench -B port/build/io_bench-arm64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64`, then `ninja -C port/build/io_bench-arm64 -j2`; the same with `x86_64` (Rosetta; run it under `scripts/with_timeout.sh SECONDS`).
Optional `-DIOB_LIBDEFLATE_DIR=<libdeflate 1.23 source>` adds the libdeflate comparison (MIT; https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.23.tar.gz; not vendored). `-DIOB_PORTABLE_DIR=` (default `../portable`) supplies leonida's
`zlib_inflate` and BCPack decoder; it is read only, and the generated BCPack tables go to the io_bench build tree.

Section 8 is reproduced with: `python3 -I port/io_bench/scripts/build_shim_ab.py arm64 b64:64 b16:16` (needs the existing `port/build/shims_gdk-arm64` tree with `dstorage_test` built; writes to `port/build/io_bench-shim-arm64`, also runs `test_*` = the 489 checks and `stress_*`),
`port/io_bench/scripts/make_requests.sh <iobench> <dir>`, then `port/io_bench/scripts/e10_shim_ab.sh port/build/io_bench-shim-arm64 out.txt <dir>` (x86_64: pass a `with_timeout.sh` prefix as the fourth argument).

**For the integrator**
- `add_subdirectory(io_bench)` in `port/CMakeLists.txt` (after `portable`; it adds only the `iobench` target; configuring it under a parent project was checked). The standalone configure above needs nothing else.
- (Done; kept for the record.) To adopt the backend, apply both patches. The dstorage patch also adds `find_package(ZLIB)` and `ZLIB::ZLIB` to `shims_gdk/CMakeLists.txt` (macOS SDK libz; the x86_64 slice is used under Rosetta).
  Without the shims_core row `gta6rt.internal!FileNativeFd` the dstorage patch still works (descriptor -1, reads go through the kernel32 rows) but loses `F_NOCACHE`; the pool, the window and the zlib change still apply.
- Optional: add `tests/dstorage_pool_stress_test.cpp` as a `gdk_add_test` (it needs the same `registry_min` / `fs_stub` support as `dstorage_test`).

## 3. What the title asks of DirectStorage (request shapes)

**ABI facts** (from `leonida/docs/directstorage.md`, `port/shims_gdk/src/dstorage_xs_abi.h` and `dstorage.cpp`; not re-derived): queue capacity 0x80-0x2000 requests; four priorities (a memory-source queue must be REALTIME);
`SetStagingBufferSize` accepts only 0/16/20/24/28/32 MiB (console values, the shim only stores it); a zlib or BCPack file request needs a 16-byte aligned offset and `DestinationSize > SourceSize`, a raw request `DestinationSize == SourceSize`;
ordinary buffer destinations are at most 32 MiB (physical page lists go above that); at most 1 GiB per request. The host staging window is `StagingPool::limit` = 64 MiB shared by all queues (`dstorage_staging.h`); a request above it runs alone.

**Compression formats and decompressors**
- zlib (`ZlibDecompress`, RFC 1950): 918,223 of 1,026,833 TTPF chunks (47,179 MiB -> 128,529 MiB, `leonida/docs/textures.md`) and the zlib streams that make up 93-99% of most RPF archives (`rpf.md`). In leonida: `leonida::zlib_inflate` (`port/portable/src/shader_library.h`), used by `dstorage.cpp`.
- BCPack BC1..BC7 (+ optional macro-unshuffle, modes 1-4): in the 46 pools present here 100,020 BC1, 5,120 BC7, 325 BC5, 268 BC6H, 77 BC4 and 35 BC3 chunks (no BC2). In leonida: `leonida::bcpack::decode` (`port/portable/src/bcpack.cpp`, interleaved rANS; BC6H/BC7 need a locally generated predictor profile `history.bcph`) and `dstorage_swizzle.h`.
- Raw: 5 TTPF chunks (16 KiB each). `audio.rpf` (28.5 GB) is 99.9% raw AWC and `movies.rpf` is Bink 2; whether they are read through DirectStorage is not known.
- **GDeflate and any other codec: none.** `grep -ri gdeflate` over `leonida/docs`, `leonida/runtime`, `port/shims_gdk`, `port/include` and `port/portable` finds nothing; the console API has `ZlibDecompress` and BCPack only.

**Real title requests** (`port/run/logs/r3fix-hidden.log`, `review-r3iw-hidden.log`, `win-control-old.log`, `win-input-hidden.log`; the same 20 requests in each). The S1 runs trace only failing requests; all 20 are `options=0x20000e` (BCPack BC7, no zlib, `DestinationIsPhysicalPages`),
`priority=2` (REALTIME), one queue, file `G:\.\data\ttdb\graphics\ttdb_graphics.ttpf`, `dst` 65,536 (11 requests, `src` 49,034-64,865) or 262,144 (9, `src` 196,291-257,714), offsets 256-byte aligned (first 9,288,960). Each failed with `0x89240030`
(`missing-profile`: no BC7 predictor profile in those runs). The first four offsets were checked against that file's chunk table: chunk physical offset = request offset, chunk size minus padding = `src` (51,012 / 49,034 / 64,555 / 57,481), codec 7, virtual delta = `dst` 65,536.
So one TTPF chunk is one DirectStorage request, which is how `iobench shapes ttpf` builds its shapes.

**Not measured:** the title's queue count, capacities and in-flight depth (only the `CreateQueue1` / `OpenFile` / `CreateStatusArray` calls are logged, without arguments); successful requests; any RPF-stream request (none appears in any log).
The RPF shapes assume a request is one zlib stream at its stored offset (the table of contents is encrypted; streams are found by walking, `rpf.md`). Queue count is therefore swept (1-6).

**Shapes measured from the archives** (`results/shapes_info.txt`; `src` = stored bytes = `SourceSize`, `dst` = inflated; the two size-share columns of the TTPF row cover all codecs):

| shape | requests | stored MiB | inflated MiB | ratio | src p50 | src p90 | src p99 | src max | requests <= 16 KiB | bytes in requests >= 256 KiB |
|---|---|---|---|---|---|---|---|---|---|---|
| levels.rpf, 6 x 128 MiB windows | 16,385 | 754 | 1,597 | 2.12 | 4,305 | 80,507 | 821,350 | 6,022,485 | 70% | 62% |
| story000.rpf, 4 windows (zlib part only) | 653 | 127 | 181 | 1.42 | 25,218 | 630,769 | 2,259,677 | 4,779,449 | 39% | 85% |
| anim.rpf, 4 windows | 11,947 | 496 | 857 | 1.73 | 26,276 | 89,921 | 320,341 | 1,755,428 | 33% | 15% |
| packs.rpf, 4 windows | 17,778 | 496 | 984 | 1.99 | 3,436 | 54,585 | 380,196 | 4,038,466 | 73% | 46% |
| core_bvh4.rpf, 4 windows | 25,640 | 487 | 8,205 | 16.84 | 104 | 57,090 | 297,054 | 1,064,807 | 80% | 26% |
| all 46 TTPF pools, zlib chunks | 908,603 | 46,343 | 126,710 | 2.73 | 11,105 | 136,773 | 628,077 | 4,158,986 | 56% | 48% |
| TTPF bcpack1 chunks | 100,020 | 10,334 | 14,220 | 1.38 | 25,951 | 377,696 | 455,675 | 3,795,681 | | |
| TTPF bcpack7 chunks | 5,120 | 1,091 | 1,214 | 1.11 | 60,807 | 935,697 | 1,021,906 | 4,077,986 | | |

core_bvh4.rpf's ratio of 16.8 means the decompressor, not the SSD, sets the pace there; TTPF zlib chunks inflate 2.7x, levels.rpf 2.1x. About half of the requests in levels.rpf are 4 KiB or smaller (24% in the TTPF pools) while 80% of the bytes are in requests of 64 KiB or more
(levels.rpf 83%, TTPF 81%): an IOPS-bound head and a bandwidth-bound tail.

## 4. Method and caveats

- Machine: Apple M4 Pro, 10 P + 4 E cores, 24 GiB, internal APFS SSD (`diskutil`: Apple Fabric, solid state), macOS 27.0.1, 16 KiB kernel pages. The archives are on that disk.
- **The machine was heavily loaded throughout** (seven other workers; load average 8-21, free memory 50 MiB to 4.5 GiB, swap activity from other processes, memory pressure level 1 = normal at all times). Absolute numbers are lower and noisier than an idle machine's.
  Comparisons are within the same session, medians of 2-3 trials (`trials` column); treat differences under about 15% as noise.
- Every timed run starts cold: the requested ranges are evicted from the page cache with `msync(MS_INVALIDATE)` on a mapping (checked: `File-backed pages` drops by the amount read; `mincore` residency 0%, `resident_before_pct` is in every result line). Destinations are pre-faulted anonymous memory.
  The load generator is closed loop (`qd` = outstanding requests: threads for `pread`/`mmap`, blocks in flight for `dispatch_io`). Latency is the service time of one request under that queue depth. CPU is process user+sys (`getrusage`) per GiB moved.
- No sudo, no `purge`; x86_64 processes ran one at a time under a timeout; nothing was written outside `port/io_bench` and `port/build/io_bench-*`; archives were opened read-only.
- The title itself was not run (the host slot belongs to someone else). Shapes come from the archives and match the 20 real requests above; `iobench model` and the real-shim replay add the queue, staging and inflate behaviour.

## 5. Storage measurements (arm64 native, `pread` unless stated)

### 5.1 Device: io size x queue depth

Sequential, 512 MiB per run from levels.rpf (MiB/s). A cached reader at qd 1 rides the kernel readahead (about 6,100 MiB/s whatever the io size); a second reader on the file defeats it (16 KiB: 6,142 at qd 1, 686 at qd 2), so cached small reads scale only with a deep queue. `F_NOCACHE` disables readahead.

cached:

| io | qd=1 | qd=2 | qd=4 | qd=8 | qd=16 |
|---|---|---|---|---|---|
| 16384 | 6142 | 686 | 890 | 1072 | 1128 |
| 65536 | 6149 | 743 | 1414 | 2642 | 4012 |
| 262144 | 6138 | 2182 | 3627 | 5176 | 5993 |
| 1048576 | 6227 | 4471 | 5902 | 5964 | 6004 |
| 4194304 | 5985 | 5961 | 5960 | 6225 | 6199 |
| 16777216 | 5325 | 6044 | 5987 | 6097 | 6061 |

`F_NOCACHE`:

| io | qd=1 | qd=2 | qd=4 | qd=8 | qd=16 |
|---|---|---|---|---|---|
| 16384 | 451 | 687 | 922 | 1080 | 1204 |
| 65536 | 422 | 747 | 1378 | 2525 | 3993 |
| 262144 | 1283 | 2389 | 3794 | 5390 | 6026 |
| 1048576 | 2768 | 4579 | 6114 | 6165 | 6024 |
| 4194304 | 4532 | 6236 | 6274 | 6250 | 4769 |
| 16777216 | 5401 | 6152 | 5869 | 6071 | 6100 |

Random reads over an 8 GiB span, 256 MiB per run (MiB/s), `F_NOCACHE`:

| io | qd=1 | qd=2 | qd=4 | qd=8 | qd=16 | qd=32 |
|---|---|---|---|---|---|---|
| 4096 | 44 | 81 | 169 | 322 | 604 | 743 |
| 16384 | 134 | 262 | 464 | 841 | 1558 | 2190 |
| 65536 | 336 | 662 | 1290 | 1924 | 3182 | 4074 |
| 262144 | 1134 | 1932 | 2977 | 4412 | 5487 | 5881 |
| 1048576 | 2455 | 4365 | 5883 | 5816 | 6242 | 6264 |

IOPS, same runs:

| io | qd=1 | qd=2 | qd=4 | qd=8 | qd=16 | qd=32 |
|---|---|---|---|---|---|---|
| 4096 | 11159 | 20683 | 43235 | 82391 | 154734 | 190083 |
| 16384 | 8562 | 16793 | 29714 | 53818 | 99697 | 140133 |
| 65536 | 5382 | 10597 | 20638 | 30783 | 50913 | 65182 |
| 262144 | 4537 | 7727 | 11910 | 17646 | 21949 | 23524 |
| 1048576 | 2455 | 4365 | 5883 | 5816 | 6242 | 6264 |

Cached random reads are within about 20% of these (4 KiB at qd 32: 648 vs 743 MiB/s; 16 KiB at qd 32: 2,560 vs 2,190).

- Ceiling: **6,274 MiB/s** sequential (4 MiB reads, qd 2-8); 1 MiB reads reach it at qd 4. Random reads at qd 32: 256 KiB 5,881, 64 KiB 4,074, 16 KiB 2,190 MiB/s.
- A 4 KiB random read takes p50 0.085 ms / p99 0.155 ms at qd 1 (44 MiB/s, 11,159 IOPS) and
  190,083 IOPS (743 MiB/s) at qd 32 (p50 0.146 ms, p99 0.346 ms). The backend today keeps one or two reads in flight per queue, so a stream of small requests runs at the left edge of these tables.
- CPU per GiB, 1 MiB reads at qd 8: 0.101 s cached, 0.025 s with `F_NOCACHE`; 16 KiB reads cost about 1.0 s/GiB either way (syscall bound).
- Latency of a 1 MiB `F_NOCACHE` read at qd 4 / 16: p50 0.64 / 2.68 ms, p99 0.87 / 4.39 ms.
- Re-reading data that is already cached (`results/e11_warm_vs_cold_arm64.txt`, 512 MiB sequential, 4 MiB reads): 11,102 MiB/s at qd 1 and 30,890 at qd 4, against 6,274 from the device. That is what `F_NOCACHE` gives up.

### 5.2 Request shapes through `pread` (`qd` threads, one outstanding read each)

384 MiB per run of the shapes of section 3, MiB/s. `runs` = sequential runs of mean length 16 at random places (spatial streaming), `rand` = uniform, `seq` = archive order.

`F_NOCACHE` (no readahead, no cache):

| shape | qd=1 | qd=2 | qd=4 | qd=8 | qd=16 | qd=32 |
|---|---|---|---|---|---|---|
| levels.rpf, runs | 455 | 580 | 874 | 1,159 | 1,716 | 2,282 |
| anim.rpf, runs | 350 | 446 | 703 | 1,114 | 1,593 | 2,280 |
| TTPF chunks, rand | 348 | 658 | 1,189 | 1,921 | 2,799 | 3,040 |
| core_bvh4.rpf, seq | 312 | 392 | 533 | 614 | 833 | 1,084 |

Cached, default readahead, no hint:

| shape | qd=1 | qd=2 | qd=4 | qd=8 | qd=16 | qd=32 |
|---|---|---|---|---|---|---|
| levels.rpf, runs | 1,007 | 888 | 1,176 | 1,529 | 1,859 | 2,731 |
| anim.rpf, runs | 884 | 624 | 801 | 1,269 | 1,932 | 2,582 |
| TTPF chunks, rand | 355 | 657 | 1,116 | 1,852 | 2,759 | 3,422 |
| core_bvh4.rpf, seq | 1,983 | 737 | 820 | 1,030 | 1,181 | 1,675 |

Cached, `F_RDAHEAD` off, no hint:

| shape | qd=1 | qd=2 | qd=4 | qd=8 | qd=16 | qd=32 |
|---|---|---|---|---|---|---|
| levels.rpf, runs | 717 | 830 | 1,118 | 1,511 | 1,939 | 2,816 |
| anim.rpf, runs | 459 | 560 | 835 | 1,305 | 1,942 | 2,726 |
| TTPF chunks, rand | 345 | 697 | 1,102 | 1,789 | 2,922 | 3,452 |
| core_bvh4.rpf, seq | 520 | 592 | 775 | 938 | 1,311 | 1,650 |

Cached with `F_RDADVISE` 16 requests ahead of the one being read:

| shape | qd=1 | qd=2 | qd=4 | qd=8 | qd=16 | qd=32 |
|---|---|---|---|---|---|---|
| levels.rpf, runs | 1,900 | 2,075 | 2,267 | 2,398 | 2,584 | 3,096 |
| anim.rpf, runs | 1,690 | 1,718 | 1,807 | 2,140 | 2,581 | 3,167 |
| TTPF chunks, rand | 2,185 | 2,529 | 2,972 | 2,972 | 3,596 | 3,581 |
| core_bvh4.rpf, seq | 1,507 | 1,588 | 1,664 | 1,555 | 1,810 | 2,053 |

Throughput (MiB/s) / request latency p50 (ms) / p99 (ms) / CPU (s per GiB) at qd 4 and 16:

| shape | cache | qd 4 | qd 16 |
|---|---|---|---|
| levels.rpf, runs | nocache | 874 / 0.15 / 0.82 / 0.53 | 1,716 / 0.36 / 1.49 / 0.86 |
| levels.rpf, runs | cache | 1,176 / 0.13 / 0.64 / 0.34 | 1,859 / 0.29 / 2.25 / 0.54 |
| anim.rpf, runs | nocache | 703 / 0.19 / 1.41 / 0.55 | 1,593 / 0.35 / 1.90 / 0.66 |
| anim.rpf, runs | cache | 801 / 0.16 / 1.63 / 0.40 | 1,932 / 0.31 / 0.89 / 0.54 |
| TTPF chunks, rand | nocache | 1,189 / 0.17 / 0.59 / 0.37 | 2,799 / 0.28 / 0.95 / 0.40 |
| TTPF chunks, rand | cache | 1,116 / 0.16 / 1.09 / 0.36 | 2,759 / 0.28 / 1.41 / 0.37 |
| core_bvh4.rpf, seq | nocache | 533 / 0.11 / 0.66 / 1.47 | 833 / 0.29 / 1.56 / 3.50 |
| core_bvh4.rpf, seq | cache | 820 / 0.04 / 0.51 / 0.83 | 1,181 / 0.18 / 1.84 / 1.97 |

- No shape reaches the device ceiling with any of these at a pool of up to 32: the best median is 3,743 MiB/s (ttpf_sample, cache=cache, advise 16, qd 32), 60% of 6,274. Small requests are bound by per-request latency (5.1).
- `F_RDAHEAD` default vs off matters for one sequential stream only: core_bvh4 at qd 1 1,983 MiB/s with the default, 520 off; from qd 4 up the two tables above differ by at most 10% in every one of their 16 cells.
- `F_RDADVISE` is the strongest single hint at low depth: at qd 1 it lifts TTPF from 355 to 2,185 MiB/s and levels from 1,007 to 1,900.
  It works under `F_NOCACHE` as well, but the advised pages then stay in the page cache after being read: in the 144 `F_NOCACHE` + advise runs without evict-behind, the cache grew by a median 102% of the bytes read (`file_pages_delta_mib` / `bytes_mib`; individual runs vary because other processes move the counter).

Distance sweep of `F_RDADVISE` (MiB/s at qd 1 / 4 / 8; `nocache` rows keep `F_NOCACHE` on):

| shape | cache | advise | qd=1 | qd=4 | qd=8 |
|---|---|---|---|---|---|
| levels.rpf runs | nocache | 0 | 493 | 577 | 909 |
| levels.rpf runs | nocache | 4 | 1,149 | 1,092 | 1,445 |
| levels.rpf runs | nocache | 16 | 1,698 | 2,078 | 1,922 |
| levels.rpf runs | nocache | 64 | 2,295 | 2,414 | 3,214 |
| levels.rpf runs | nocache | 256 | 1,817 | 3,872 | 4,082 |
| levels.rpf runs | cache | 4 | 626 | 1,146 | 1,680 |
| levels.rpf runs | cache | 16 | 1,270 | 2,081 | 2,243 |
| levels.rpf runs | cache | 64 | 1,903 | 2,811 | 3,304 |
| levels.rpf runs | cache | 256 | 2,857 | 3,808 | 3,866 |
| TTPF rand | nocache | 0 | 262 | 937 | 1,418 |
| TTPF rand | nocache | 4 | 1,182 | 1,651 | 1,998 |
| TTPF rand | nocache | 16 | 2,124 | 2,537 | 3,049 |
| TTPF rand | nocache | 64 | 2,345 | 3,140 | 3,196 |
| TTPF rand | nocache | 256 | 1,615 | 2,987 | 3,454 |
| TTPF rand | cache | 4 | 756 | 1,572 | 2,156 |
| TTPF rand | cache | 16 | 1,346 | 2,706 | 3,235 |
| TTPF rand | cache | 64 | 2,023 | 3,406 | 3,531 |
| TTPF rand | cache | 256 | 1,854 | 3,528 | 3,317 |

### 5.3 `mmap` and `dispatch_io`

Same shapes, MiB/s. `mmap`: one `MAP_SHARED` mapping per file, a `memcpy` per request. `dispatch_io`: one `DISPATCH_IO_RANDOM` channel per file, `dispatch_io_read` per request, copy out of the `dispatch_data`, qd = blocks in flight, single submitting thread.

| shape | backend | qd=1 | qd=4 | qd=16 | qd=32 |
|---|---|---|---|---|---|
| levels.rpf runs | pread, cached | 1,007 | 1,176 | 1,859 | 2,731 |
| levels.rpf runs | pread, F_NOCACHE | 455 | 874 | 1,716 | 2,282 |
| levels.rpf runs | mmap, no madvise | 618 | 1,135 | 1,656 | 686 |
| levels.rpf runs | mmap, MADV_RANDOM | 330 | 646 | 1,272 | 275 |
| levels.rpf runs | mmap, MADV_WILLNEED per request | 526 | 872 | 1,724 | 2,171 |
| levels.rpf runs | dispatch_io, cached | 710 | 1,063 | 1,387 | 1,265 |
| levels.rpf runs | dispatch_io, F_NOCACHE | 507 | 862 | 650 | 864 |
| TTPF rand | pread, cached | 355 | 1,116 | 2,759 | 3,422 |
| TTPF rand | pread, F_NOCACHE | 348 | 1,189 | 2,799 | 3,040 |
| TTPF rand | mmap, no madvise | 224 | 634 | 1,371 | 1,818 |
| TTPF rand | mmap, MADV_RANDOM | 139 | 377 | 1,349 | 2,246 |
| TTPF rand | mmap, MADV_WILLNEED per request | 335 | 1,054 | 2,259 | 2,686 |
| TTPF rand | dispatch_io, cached | 94 | 195 | 353 | 353 |
| TTPF rand | dispatch_io, F_NOCACHE | 76 | 135 | 329 | 298 |

Against cached `pread` at the same depth, `mmap` runs at 0.10-0.97x (the low end is qd 32 on the levels shape, 0.10x with `MADV_RANDOM` and 0.25x without a hint, likely page-fault contention) and never beats it. `dispatch_io` runs at 0.32-0.90x on levels and 0.09-0.26x on random TTPF chunks
(`set_high_water` of 64 KiB and 1 MiB did not change that: `results/e4_mmap_dio_arm64.txt`). Neither is worth using; `pread` on a pool is the backend.

### 5.4 Coalescing adjacent requests, and evicting behind

`--coalesce GAP` sorts each batch of 256 requests by offset and merges neighbours closer than GAP bytes (at most 4 MiB per read); throughput counts useful bytes. `evict-behind` drops each request's pages with `msync(MS_INVALIDATE)` right after the read.

| shape | variant | qd=1 | qd=4 | qd=8 | requests read |
|---|---|---|---|---|---|
| levels.rpf runs | F_NOCACHE | 493 | 577 | 909 | 8455 |
| levels.rpf runs | F_NOCACHE, merge gap 64 KiB | 1,830 | 4,144 | 4,841 | 572 |
| levels.rpf runs | F_NOCACHE + advise 16 | 1,698 | 2,078 | 1,922 |  |
| levels.rpf runs | F_NOCACHE + advise 16 + evict-behind | 406 | 1,197 | 1,668 |  |
| levels.rpf runs | cached + advise 16 + evict-behind | 394 | 1,063 | 1,856 |  |
| core_bvh4.rpf seq | F_NOCACHE | 190 | 489 | 738 | 19901 |
| core_bvh4.rpf seq | F_NOCACHE, merge gap 64 KiB | 2,939 | 4,353 | 5,020 | 151 |
| core_bvh4.rpf seq | F_NOCACHE + advise 16 | 1,142 | 1,523 | 1,779 |  |
| core_bvh4.rpf seq | F_NOCACHE + advise 16 + evict-behind | 308 | 527 | 769 |  |
| core_bvh4.rpf seq | cached + advise 16 + evict-behind | 412 | 854 | 936 |  |
| TTPF rand | F_NOCACHE | 262 | 937 | 1,418 | 6534 |
| TTPF rand | F_NOCACHE, merge gap 64 KiB | 272 | 984 | 1,772 | 6527 |
| TTPF rand | F_NOCACHE + advise 16 | 2,124 | 2,537 | 3,049 |  |
| TTPF rand | F_NOCACHE + advise 16 + evict-behind | 1,365 | 2,165 | 3,034 |  |
| TTPF rand | cached + advise 16 + evict-behind | 1,495 | 2,232 | 2,523 |  |

- Coalescing is large where streams are adjacent: levels.rpf runs 909 -> 4,841 MiB/s at qd 8 with 2% extra bytes read;
  core_bvh4 738 -> 5,020. It does nothing for random requests (TTPF). A gap of 0 merges nothing: streams are not byte-adjacent (padding between them).
  It needs the whole submit batch and a staging buffer shared by the merged requests, so it is **not in the patch**: the first follow-up (section 9).
- `evict-behind` keeps the cache empty while `F_RDADVISE` prefetches. Its price is a `msync` per request: at qd 1 levels 1,698 -> 406 MiB/s, TTPF 2,124 -> 1,365; at qd 8 levels 1,922 -> 1,668, TTPF 3,049 -> 3,034.

### 5.5 Page-cache pressure on a 24 GiB machine

`iobench pressure` streams a range of levels.rpf (70.9 GiB: the "76 GiB" file, 76,155,065,056 bytes) with 4 threads x 1 MiB `pread` while a victim holds 2 GiB of incompressible anonymous memory (a stand-in for title memory) and touches every page once per interval, timing the sweep
(`sweep_*_ms`). `evict-behind` here drops the consumed range in 64 MiB batches. It aborts if the kernel pressure level reaches a limit (never did: level 1 throughout).

| mode | streamed_gib | mibs | cpu_s | file_growth_mib | peak_file_mib | min_free_mib | compressor_base_mib | compressor_peak_mib | pageouts_delta | decompressions_delta | sweep_p50_ms | sweep_max_ms | trials |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| cache | 12.00 | 3776 | 2 | 4209 | 6198 | 47 | 3103 | 5569 | 745 | 238262 | 2 | 349 | 1 |
| cache | 68.00 | 3934 | 15 | 5422 | 8018 | 86 | 2872 | 7221 | 1167 | 1666181 | 372 | 449 | 1 |
| evict-behind | 12.00 | 4146 | 2 | 422 | 2514 | 793 | 2854 | 2854 | 0 | 8925 | 1 | 1 | 2 |
| nocache | 12.00 | 4742 | 0 | 148 | 1893 | 2093 | 3278 | 3278 | 0 | 16999 | 1 | 1 | 2 |
| nocache | 68.00 | 5323 | 2 | 311 | 1824 | 1356 | 4595 | 4595 | 0 | 33253 | 1 | 2 | 1 |

- Cached: the file cache grows until memory is tight (+5,422 MiB, free memory down to 86 MiB), the kernel compresses other anonymous memory to make room (compressor 2,872 -> 7,221 MiB,
  1,666,181 decompressions, 1,167 pageouts) and the victim's pages are likely among them (the sweep time is the evidence): median sweep 372 ms against 1.0 ms at rest, still 194 ms right after the stream ended.
  The first 12 GiB already cost +4,209 MiB of cache and a 349 ms worst sweep. The cache itself plateaus (peak 8,018 MiB over the whole 68 GiB), so the damage is churn: every further GiB read evicts or compresses about a GiB of something else.
- `F_NOCACHE`: no growth (311 MiB), no victim impact (max sweep 2 ms), 5,323 vs 3,934 MiB/s and 2.0 vs 14.9 s of CPU for the same 68 GiB.
- `evict-behind` also protects the victim (4,146 MiB/s, +422 MiB, 1.7 s of CPU per 12 GiB against 0.4 s for `F_NOCACHE`): the way to combine prefetch hints with a bounded cache, at the cost in 5.4.
- What `F_NOCACHE` gives up is re-reads of data that would have stayed cached (5.1: 11,102 MiB/s warm at qd 1). With 70+ GiB of streamed archives and 24 GiB of RAM that reuse is unlikely; small hot files (common, data, shaders: under 0.5 GiB each) were not benchmarked separately.

### 5.6 Rosetta (x86_64) vs native file IO

The same configurations from the x86_64 `iobench` (a Rosetta process; `getpagesize()` is 4,096 there; `mincore` and `msync` eviction work unchanged) and the arm64 one, run back to back (`results/e9_io_*.txt`, median MiB/s, arm64 / x86_64):

| pattern | cache | | | |
|---|---|---|---|---|
| seq 1 MiB | nocache | qd 1: 2,883 / 2,290 | qd 4: 6,160 / 6,036 | qd 16: 5,963 / 6,228 |
| seq 1 MiB | cache | qd 1: 6,178 / 6,194 | qd 4: 6,110 / 6,075 | qd 16: 6,272 / 6,241 |
| seq 16 KiB | nocache | qd 1: 412 / 427 | qd 4: 892 / 912 | qd 16: 1,152 / 1,127 |
| seq 16 KiB | cache | qd 1: 5,978 / 5,474 | qd 4: 790 / 847 | qd 16: 975 / 1,106 |
| random 4 KiB | nocache | qd 1: 47 / 36 | qd 8: 334 / 241 | qd 32: 615 / 529 |
| random 4 KiB | cache | qd 1: 38 / 37 | qd 8: 160 / 279 | qd 32: 614 / 764 |
| random 64 KiB | nocache | qd 1: 392 / 331 | qd 8: 2,221 / 2,061 | qd 32: 4,220 / 3,420 |
| random 64 KiB | cache | qd 1: 375 / 339 | qd 8: 2,212 / 1,985 | qd 32: 4,240 / 4,168 |
| levels.rpf runs | nocache | qd 4: 702 / 831 | qd 16: 1,490 / 1,470 | qd 32: 2,115 / 1,889 |
| levels.rpf runs | cache | qd 4: 1,129 / 895 | qd 16: 2,153 / 1,868 | qd 32: 2,760 / 2,190 |
| TTPF rand | nocache | qd 4: 1,160 / 1,150 | qd 16: 2,810 / 2,663 | qd 32: 3,410 / 3,036 |
| TTPF rand | cache | qd 4: 986 / 1,206 | qd 16: 2,250 / 2,428 | qd 32: 3,403 / 3,112 |

The file IO path itself shows no systematic Rosetta penalty: most cells are within 10%, a few small-random cells differ by 15-40% in both directions (cached 4 KiB at qd 8: 160 arm64 vs 279 x86_64), which is the size of the noise on this loaded machine.
What Rosetta does cost is CPU work on the data (section 6) and memory-copy bandwidth.

## 6. Decompression

Inputs are real streams (the bytes the shim would read) held in memory; outputs have the known inflated size. The digest of the first 4 KiB of every output is identical for libz, leonida and libdeflate on both architectures (`digest=` in the lines).
Work per run: 48-96 MiB of stored data. Implementations: **libz** = the macOS SDK system zlib 1.2.12 (the x86_64 slice runs under Rosetta); **leonida** = `leonida::zlib_inflate` with the bulk path the shim enables by default (`bytewise` = `LEONIDA_DSTORAGE_FAST_INFLATE=0`);
**libdeflate** 1.23 built from source per architecture (arm64 NEON; x86_64 with its runtime-dispatched code, whatever Rosetta advertises).

Output MiB/s, median of 3 runs, threads 1/2/4/8/14 (10 P + 4 E cores, other workers running: the 14-thread cells are not an idle-machine ceiling):

levels.rpf streams (ratio 2.1, median stream 4 KiB):

| arch | impl | threads=1 | threads=2 | threads=4 | threads=8 | threads=14 |
|---|---|---|---|---|---|---|
| arm64 | leonida | 274 | 513 | 904 | 1537 | 2181 |
| arm64 | leonida-bytewise | 139 |  |  |  |  |
| arm64 | libdeflate | 743 | 1348 | 2312 | 4913 | 5278 |
| arm64 | libz | 585 | 1115 | 2184 | 4112 | 5363 |
| x86_64 | leonida | 223 | 405 | 765 | 1381 | 1851 |
| x86_64 | leonida-bytewise | 75 |  |  |  |  |
| x86_64 | libdeflate | 617 | 1220 | 2189 | 3828 | 3975 |
| x86_64 | libz | 444 | 849 | 1689 | 2777 | 3767 |

core_bvh4.rpf streams (ratio 16.8):

| arch | impl | threads=1 | threads=2 | threads=4 | threads=8 | threads=14 |
|---|---|---|---|---|---|---|
| arm64 | leonida | 892 | 1764 | 3265 | 5946 | 7864 |
| arm64 | libdeflate | 2989 | 5837 | 10829 | 17068 | 23408 |
| arm64 | libz | 1676 | 3505 | 6847 | 12968 | 15931 |
| x86_64 | leonida | 694 | 1088 | 2173 | 3404 | 4096 |
| x86_64 | libdeflate | 1809 | 3513 | 5500 | 9074 | 10836 |
| x86_64 | libz | 1200 | 2197 | 3980 | 4333 | 7039 |

TTPF zlib chunks (ratio 2.7):

| arch | impl | threads=1 | threads=4 | threads=8 |
|---|---|---|---|---|
| arm64 | leonida | 302 | 1081 | 2099 |
| arm64 | libdeflate | 1036 | 3662 | 6932 |
| arm64 | libz | 886 | 3202 | 6361 |
| x86_64 | leonida | 157 | 521 | 657 |
| x86_64 | libdeflate | 407 | 1792 | 2219 |
| x86_64 | libz | 390 | 1520 | 2967 |

anim.rpf streams (ratio 1.7):

| arch | impl | threads=1 | threads=4 | threads=8 |
|---|---|---|---|---|
| arm64 | leonida | 222 | 842 | 1421 |
| arm64 | libdeflate | 653 | 2446 | 4007 |
| arm64 | libz | 582 | 2280 | 3431 |
| x86_64 | leonida | 143 | 516 | 876 |
| x86_64 | libdeflate | 340 | 1422 | 2769 |
| x86_64 | libz | 316 | 1221 | 2271 |

BCPack BC1 (TTPF chunks, `leonida::bcpack::decode`, fitted predictors allowed as the shim does; output MiB/s):

| arch | impl | threads=1 | threads=2 | threads=4 | threads=8 | threads=14 |
|---|---|---|---|---|---|---|
| arm64 | leonida | 53 | 103 | 197 | 359 | 418 |
| x86_64 | leonida | 26 | 48 | 82 | 118 | 173 |

Interleaved single-thread A/B (arm64, x86_64, arm64, ... three rounds, medians, `results/e6b_arch_ab.txt`), the more reliable architecture comparison here (output MiB/s):

| shape | codec | impl | arch=arm64 | arch=x86_64 |
|---|---|---|---|---|
| shapes/rpf_core_bvh4.iobs | zlib | leonida | 757 | 723 |
| shapes/rpf_core_bvh4.iobs | zlib | libdeflate | 2612 | 1848 |
| shapes/rpf_core_bvh4.iobs | zlib | libz | 1729 | 1007 |
| shapes/rpf_levels.iobs | zlib | leonida | 220 | 196 |
| shapes/rpf_levels.iobs | zlib | libdeflate | 688 | 590 |
| shapes/rpf_levels.iobs | zlib | libz | 552 | 421 |
| shapes/ttpf_sample.iobs | bcpack1 | leonida | 48 | 37 |

- System zlib against leonida's inflater, one thread: 2.5x (levels) and 2.3x (core_bvh4) on arm64, 2.1x / 1.4x under Rosetta. Leonida's inflater is scalar C++ appending to a `std::vector`;
  its non-default `bytewise` path is 2.0x (arm64) / 3.0x (Rosetta) slower again (levels). libdeflate is 1.04-1.78x libz across the four shapes and both architectures.
- Rosetta costs (arm64 / x86_64 per thread): 1.05-1.72x for the zlib implementations in the A/B and 1.30x for the BCPack decoder (an earlier non-interleaved run measured 53 vs 26 MiB/s, 2.0x: the spread on this machine is that large).
- Scaling at 8 threads over 1 thread: libz on arm64 7.0x (levels) / 7.7x (core_bvh4), under Rosetta 6.3x / 3.6x; BCPack BC1 arm64 6.8x, Rosetta 4.5x.
  Up to 14 threads gains little (4 of them are E cores and the machine is shared). The core_bvh4 cells under Rosetta are limited by memory bandwidth, likely: ratio 16.8 means 16 output bytes per stored byte (below).
- Against the SSD: x86_64 libz with 14 threads turns 3,767 MiB/s of output (levels) into about 1,373 MiB/s of stored data, 22% of the SSD's 6,274; BCPack BC1 at 173 MiB/s out consumes 115 MiB/s, 1.8%.
- The shim's per-request allocations: `data.resize(SourceSize)` zero-fills the read buffer and `inflate` allocates its output. Measured with fresh allocation per request (`fresh=1`, one thread, levels): libz 614 MiB/s against 585 reused, leonida 240 against 274 (arm64); the same run on x86_64 under Rosetta, the host's architecture (`results/e6_decode_x86_64.txt`): libz 257 fresh against 444 reused (-42%) and leonida 181 fresh, and the review's re-measurement of the Rosetta libz row gave 319 against 348 (-8%), so the 257 is most likely noise, but the Rosetta cost of fresh allocation is not pinned down; removing the zero fill in the model changed
  today's design by -8% / -20% / -21% (levels / core_bvh4 / TTPF; no gain: noise). Not worth touching.
- Memory copy bandwidth (`iobench membw`, 16 MiB blocks, best of 20, median of 3, one thread): arm64 39,702 MiB/s `memcpy` and 91,450 `memset`; Rosetta 20,616 and 25,149. Every output byte is written by the inflate, zero-filled before it and copied once more when published, so
  high-ratio data pays three or four passes at Rosetta's copy speed.

**Where decompression should run.** In the x86_64 host process, on the shared pool (what the patch does), with the system zlib.
- An arm64 helper process would give 1.0-1.7x per thread for zlib (the Rosetta cost above) and 1.3x for BCPack in the interleaved A/B, against an extra copy of every output byte through shared memory and a second process to build and supervise. In the shim the pool is the larger gain (section 8: 2.7-3.4x for the pool, 2.0-2.4x for the system zlib).
- libdeflate adds 1.04-1.78x over libz for one vendored MIT library. Not in the patch (it stays on the SDK's zlib); the next step if inflate is still the limit once the pool is in.
- BCPack is the actual problem: 418 MiB/s (arm64) and 173 (Rosetta) with all 14 cores, for the BC1 chunks that are 89.6% of the BCPack bytes. It is the one stage where native arm64, or a faster decoder (NEON, or a Metal compute rANS decoder; neither tried), pays for itself.
  BC6H and BC7 were **not benchmarked**: they need `history.bcph`, which these runs do not have (the real BC7 requests above all failed with `missing-profile`).

## 7. The shim as it is, and the proposed shape (`iobench model`)

`iobench model` re-implements the shim's queue behaviour around real work (real file reads, real inflate, a `memcpy` into a destination): one worker thread per queue (requests spread round-robin), a helper that overlaps one adjacent pair, at most two pairs at once, the shared staging window with ticket order, the 16 MiB per-request preparation limit
(`dstorage_staging.h`). Output MiB/s, arm64, cold cache, 128 MiB (48 MiB for bvh4) of stored data per run, medians of 2-3:

| shape | today, 1 queue | today, 3 queues | today, 6 queues | today, 3 queues, window 16 / 256 MiB | today + system zlib, 3 queues |
|---|---|---|---|---|---|
| levels | 120 | 296 | 436 | 244 / 256 | 615 |
| core_bvh4 | 580 | 1,719 | 2,257 | 1,428 / 1,400 | 3,180 |
| TTPF | 164 | 401 | 674 | 405 / 412 | 886 |

The window size changes nothing for today's design (peak staging use 8-38 MiB: the helper holds at most two requests of 16 MiB or less); a faster inflate roughly doubles it; the queue count scales it because each queue is one worker.

A shared pool of prepare threads (the patch's design: any queue's next requests are prepared in parallel, published in queue order), 3 queues, system zlib, window 256 MiB, MiB/s out:

| shape | cache | pool 8 | pool 12 | pool 16 | pool 24 | pool 32 |
|---|---|---|---|---|---|---|
| levels | nocache | 1,071 | 1,640 | 1,934 | 2,372 | 2,739 |
| levels | cache |  |  | 2,546 | 2,268 |  |
| core_bvh4 | nocache | 6,073 | 7,036 | 6,946 | 10,688 | 10,577 |
| core_bvh4 | cache |  |  | 10,431 | 10,649 |  |
| TTPF | nocache | 2,112 | 2,445 | 3,166 | 2,748 | 4,331 |
| TTPF | cache |  |  | 3,084 | 3,250 |  |

The smaller pools, page cache (from `iobench model`, `results/e7_model_arm64.txt`):

| shape | pool 2 | pool 4 | pool 8 | pool 12 |
|---|---|---|---|---|
| levels | 394 | 688 | 994 | 1,306 |
| core_bvh4 | 2,825 | 3,955 | 7,104 | 9,104 |
| TTPF | 566 | 1,152 | 1,783 | 2,072 |

Pools of 16-32 threads on 14 cores keep improving (levels 1,934 -> 2,739 MiB/s from 16 to 32): the threads likely spend much of their time waiting for small reads, so extra threads act as queue depth. Peak staging use with pool 24-32 was 25-161 MiB.
Window size with the pool (3 queues, pool 8, libz, page cache; MiB/s, levels / core_bvh4 / TTPF; 2-6 trials, noisy): 16 MiB 840 / 3,902 / 1,653; 64 MiB 1,216 / 5,995 / 1,986; 256 MiB 994 / 7,104 / 1,783; 512 MiB 1,243 / 7,317 / 1,384.

## 8. Through the real shim: baseline vs proposal (`tests/dstorage_replay_bench.cpp`)

The same request lists (`iobench export`: 128 MiB of levels.rpf runs = 2,634 requests, 48 MiB of core_bvh4 = 1,918, 128 MiB of TTPF chunks over 46 files = 2,234, after dropping requests the zlib rules refuse) go to the actual `dstorage.cpp` through `IDStorageQueueX2` (zlib requests, 3 queues unless stated, one status entry per queue at the end,
cold cache every run, 3 repetitions per program run), once for the shim as it is in `port/shims_gdk` and once for the patched copy. The patched copy is linked with a test-side `FileNativeFd` row (`GetFinalPathNameByHandleW` + `open`), because shims_core has no such row yet. MiB/s out; arm64 cells are the median of 9 trials (the experiment ran three times, below), x86_64 cells of 3; `proposal` = the patch's defaults
(F_NOCACHE, pool 21, depth 32, window 256 MiB, system zlib).

| arm64 native | levels.rpf runs | core_bvh4.rpf | TTPF chunks |
|---|---|---|---|
| 1 queue(s), baseline | 115 | 560 | 166 |
| 1 queue(s), **proposal** | **823** (7.2x) | **3,544** (6.3x) | **1,229** (7.4x) |
| 3 queue(s), baseline | 307 | 1,277 | 403 |
| 3 queue(s), **proposal** | **1,605** (5.2x) | **6,626** (5.2x) | **2,488** (6.2x) |
| 6 queue(s), baseline | 480 | 1,475 | 650 |
| 6 queue(s), **proposal** | **1,733** (3.6x) | **6,983** (4.7x) | **2,612** (4.0x) |

The same experiment in three separate runs (3 queues, MiB/s out; the table above pools all trials of the three runs). The machine's load changed between runs: the same configuration moved by up to 1.7x between runs, and the proposal stayed 3.5x or more above the baseline in every pairing of these three runs (a property of these runs, not of the backend: core_bvh4's proposal cells are CPU-bound, 16.8x inflate ratio and 21 pool threads, and in the review's re-run on a saturated machine core_bvh4 fell to 3.1x, 2.4-5.9x in single pairings; section 11.3 has the re-run):

| 3 queues | run 1 | run 2 | run 3 |
|---|---|---|---|
| baseline, levels | 231 | 325 | 321 |
| baseline, core_bvh4 | 1,277 | 870 | 1,444 |
| baseline, TTPF | 411 | 382 | 403 |
| proposal, levels | 1,540 | 1,460 | 1,613 |
| proposal, core_bvh4 | 7,044 | 5,070 | 6,626 |
| proposal, TTPF | 2,558 | 2,488 | 2,271 |

What each piece buys, 3 queues (each row changes one thing from `proposal`; MiB/s out):

| variant (3 queues) | levels | core_bvh4 | TTPF |
|---|---|---|---|
| proposal (all) | 1,605 | 6,626 | 2,488 |
| no native fd (kernel32 rows, page cache) | 827 | 3,410 | 2,048 |
| no pool (queue thread only; fd, F_NOCACHE, system zlib, 256 MiB) | 591 | 2,065 | 730 |
| no pool, leonida inflater (fd + F_NOCACHE + 256 MiB only) | 301 | 1,225 | 376 |
| pool, leonida inflater | 676 | 3,252 | 1,179 |
| page cache instead of F_NOCACHE | 1,563 | 5,980 | 2,589 |
| page cache + F_RDADVISE 16 | 1,582 | 6,026 | 2,597 |
| F_NOCACHE + F_RDADVISE 16 | 1,484 | 6,566 | 2,535 |
| pool 8 | 1,079 | 5,133 | 1,721 |
| pool 16 | 1,472 | 6,063 | 2,456 |
| pool 32 | 1,601 | 6,182 | 2,644 |
| window depth 4 | 829 | 3,206 | 1,421 |
| window depth 8 | 1,024 | 3,971 | 1,787 |
| window depth 64 | 1,776 | 6,624 | 2,805 |
| staging 16 MiB | 1,159 | 2,561 | 2,050 |
| staging 64 MiB | 1,662 | 5,890 | 2,465 |

- Largest single piece: the pool (2.7x / 3.2x / 3.4x over `pool0`), then the system zlib (2.4x / 2.0x / 2.1x over the leonida inflater),
  then the native descriptor (1.9x / 1.9x / 1.2x; TTPF is inside the noise).
- **Staging window.** 16 MiB costs 61% on core_bvh4 (outputs up to 17 MiB), 28% on levels and 18% on TTPF. 64 MiB is within 4% / 11% / 1% of 256 MiB (levels / bvh4 / TTPF): only core_bvh4 shows a difference
  (12%, about the noise level). The fork's "at least 256 MiB" is therefore not contradicted but not required by these shapes: they need more than 16 MiB and are content with 64 MiB. 256 MiB is a ceiling, not a cost: the model's peak use with pool 24-32 was 25-161 MiB.
- Window depth 32 is where the curve flattens at 3 queues (levels: depth 4 829, 8 1,024, 32 1,605, 64 1,776). `F_RDADVISE` on top of the pool did not help (`nocache_advise16` is 8% / 1% / -2% below `proposal`), so it is off.
- Page cache against `F_NOCACHE` has no consistent throughput winner in the pool design (levels 1,563 vs 1,605, TTPF 2,589 vs 2,488, core_bvh4 5,980 vs 6,626), so the memory evidence of 5.5 decides for `F_NOCACHE`.

Rosetta (the x86_64 build of the same test programs, `results/e10_shim_ab_x86_64.txt`), MiB/s out, 3 queues:

| x86_64 (Rosetta) | levels | core_bvh4 | TTPF |
|---|---|---|---|
| baseline | 278 (arm64 307) | 1,621 (arm64 1,277) | 418 (arm64 403) |
| proposal, no native fd | 644 (arm64 827) | 5,084 (arm64 3,410) | 2,370 (arm64 2,048) |
| proposal | 1,159 (arm64 1,605) | 6,008 (arm64 6,626) | 2,335 (arm64 2,488) |
| proposal without the pool | 486 (arm64 591) | 2,188 (arm64 2,065) | 768 (arm64 730) |
| proposal with the leonida inflater | 698 (arm64 676) | 4,460 (arm64 3,252) | 1,191 (arm64 1,179) |

Proposal over baseline under Rosetta: 4.2x / 3.7x / 5.6x. The system zlib is worth 1.7x / 1.3x / 2.0x over the leonida inflater end to end
(levels / bvh4 / TTPF) under Rosetta, against 2.4x / 2.0x / 2.1x on arm64.

Raw (non-transform) requests through the shim (`results/e12_raw_ab_arm64.txt`, arm64, stored MiB/s; `levels` = the 2,634 levels.rpf stream ranges read as raw, `64 KiB` / `1 MiB` = 256 MiB / 384 MiB of sequential raw reads in one file):

| raw requests | queues | baseline | proposal | proposal, `NOCACHE=0` |
|---|---|---|---|---|
| levels ranges (median 4 KiB) | 1 | 772 | 1,816 | 2,185 |
| levels ranges (median 4 KiB) | 3 | 635 | 2,316 | 2,979 |
| 64 KiB sequential | 1 | 5,462 | 3,703 | 4,129 |
| 64 KiB sequential | 3 | 372 | 3,700 | 4,204 |
| 1 MiB sequential | 1 | 6,132 | 6,272 | 5,809 |
| 1 MiB sequential | 3 | 2,480 | 5,978 | 6,128 |

Raw reads now ride the pool too (read into the prefetch buffer, one `memcpy` at publish). With several queues or small ranges that is a large gain (64 KiB, 3 queues: 372 -> 3,700; 1 MiB, 3 queues: 2,480 -> 5,978). **One queue reading one file sequentially in 64 KiB requests is
slower than today's cached readahead** (5,462 -> 3,703 MiB/s, 0.7 against 0.1 s of CPU per GiB); at 1 MiB it is on par (6,132 -> 6,272).
That is the price of not filling the page cache: the number is still far above what an audio or video stream needs, and `LEONIDA_DSTORAGE_NOCACHE=0` restores the cache (`proposal, NOCACHE=0` column: the pool, not the cache, is what limits the single-queue case).


## 9. Recommendation (with the numbers it rests on)

| setting | recommended | evidence |
|---|---|---|
| read API | `pread` on a native descriptor, from a shared pool | 5.3: `mmap` 0.10-0.97x and `dispatch_io` 0.09-0.90x of cached `pread` at equal depth |
| page cache | `F_NOCACHE` on every DirectStorage file | 5.5: 68 GiB stream, cache +5.3 GiB, compressor +4.2 GiB, victim sweeps 372 ms vs 1.0 ms; with `F_NOCACHE` +0.3 GiB, 2 ms, 1.4x the throughput, 7.3x less CPU |
| in-flight reads | pool of 1.5 x cores threads (8..32; 21 here), window of 32 per queue | 5.2, 7, 8: levels 1,934 (pool 16) -> 2,739 (32); window 4 -> 32: 1.9x |
| staging window | 256 MiB shared (64 MiB is enough for these shapes; 16 MiB is not) | 8: 16 MiB costs 18-61%; 256 vs 64 MiB +12% on core_bvh4, noise elsewhere. `SetStagingBufferSize` keeps its console values (only stored) |
| inflate | system zlib on the pool threads; leonida's inflater only to name errors | 6: 2.3x-2.5x per thread; 8: 2.4x / 2.0x / 2.1x end to end (arm64) |
| where to decode | x86_64 host process, same pool; no arm64 helper | 6: helper buys 1.0-1.7x per thread and costs a copy and a process; revisit for BCPack only |
| `F_RDAHEAD`, `F_RDADVISE` | default readahead, no `F_RDADVISE` | 5.2: help at qd 1-2 on a single stream only; 8: no gain on the pool; advised pages stay cached (5.2, 5.4) |
| adjacent-request coalescing | **not in the patch**; first follow-up | 5.4: 5.3x on adjacent RPF streams at qd 8 for 2% extra bytes; nothing for random TTPF |
| cache eviction | none | `evict-behind` is the fallback if prefetch hints are ever wanted (5.4, 5.5) |

**Not measured, or open**
- The title's real queue count, capacities and in-flight depth, and any RPF-stream request (section 3); results are swept over 1-6 queues.
- BCPack: decoder speed (section 6); BC6H and BC7 decode not benchmarked (no `history.bcph`); swizzle / macro-unshuffle and destination page-list publishing (`page_transfer`, which holds the xmem map lock) not measured on their own. The title's real requests use page lists and the patch leaves publishing serial per queue, on the queue thread.
- One queue reading one raw file sequentially in 64 KiB requests is slower with the patch than today (section 8, raw table).
- Coalescing (5.4) and libdeflate (6) are measured but not in the patch.
- CPU competition between the pool (21 threads, default QoS) and the game's own threads was not measured; the title was not run. The pool threads are created with `rt::gdk::Thread` like the helper they replace (so each gets the guest TEB/TLS setup; its cost was not measured).
- The `FileNativeFd` row (shims_core) and the `find_package(ZLIB)` line are the only edits outside `dstorage*`; both are in proposals, neither applied.

## 10. The patch (`proposals/dstorage_io_backend.patch`)

Against the sources as they are now (`dstorage.cpp` md5 `c36b700473097e0e43a8d62890ad3e4d`, `dstorage_staging.h` `edc2236221eb06bc563b880a5e6879ff`, `CMakeLists.txt` `6a11455ad6880c5e6004747e99facd95`); `patch -p1 --dry-run` from `port/shims_gdk` applies cleanly. `proposals/shims_core_file_native_fd.patch` is against `shims_core/src/file_system.cpp` md5 `5f1d503ce9c7d7085c78da7cf60b3237` (a `FileNativeFd` row returning the `FileObj` descriptor; syntax-checked with the existing arm64 and x86_64 compile commands, and its dry run applies).

- `File` takes a native descriptor from `gta6rt.internal!FileNativeFd` (absent row = -1 = the old path), sets `F_NOCACHE`, caches the file size, and reads with `pread` directly.
- `PreparePool` (shared threads) replaces the per-queue `PrepareHelper`. A queue keeps a window of `depth` slots. Each file request (raw or transform, prefetch buffers at most window/4) takes staging with a non-blocking `try_take`, **in queue order only** (an older slot without its share stops the younger ones; that is what rules out a cross-queue deadlock),
  and is posted to the pool. The queue thread waits for the front slot, runs `process()` (publish, status, fence, event) and pops it, so completion order, status ordering and `since_notify_` are unchanged. Memory sources and notifications stay on the queue thread in order.
  `CancelRequestsWithTag` also marks slots that have not started processing (a prefetched request that was cancelled is dropped at publish).
- `inflate()` tries the system zlib first and falls back to `leonida::zlib_inflate` on any failure, so every typed error (`E_DSTORAGE_ZLIB_*`) still comes from the same code as before.
- `StagingPool::limit` 64 -> 256 MiB; `Lease::try_take` added.
- Environment knobs (defaults): `LEONIDA_DSTORAGE_POOL` (1.5 x cores, 8..32; `0` or the old `LEONIDA_DSTORAGE_WORKERS=0/1` = serial), `_DEPTH` (32), `_NOCACHE` (1), `_RDAHEAD` (-1), `_ADVISE` (0), `_SYSTEM_ZLIB` (1).

Validation (arm64 and x86_64 under Rosetta; `scripts/build_shim_ab.py` compiles the unpatched and the patched `dstorage.cpp` from copies, swaps the object into a copy of the existing `libgta6rt_shims_gdk.a` and relinks the existing test objects; nothing in `port/shims_gdk` was touched):
- `dstorage_test`: 489 checks, 0 failures for the unpatched source, the patch without the native fd row, and the patch with a native fd row (16 native descriptors opened); repeated 6x with default knobs and 3x each with POOL=0, POOL=2 + DEPTH=2, NOCACHE=0, SYSTEM_ZLIB=0, ADVISE=16 + DEPTH=1, POOL=64, RDAHEAD=0 (arm64, on the version of the patch before raw requests were added to the pool; the final version passed all 489 checks with default knobs on both architectures and again with each of the eight knob sets, 3x each, on arm64).
- `tests/dstorage_pool_stress_test.cpp`: 1,500 streams of 1 KiB-3 MiB (every 4th raw) in one file, a status entry every 5 requests, each status polled and the destination bytes compared the moment it completes, then a full compare after `Close()`; 2 rounds, 0 failures for the baseline and the patch (arm64; x86_64 and knob variants smaller, same result).
- The first version of the patch notified a slot's condition variable after unlocking it and hung at depth 4 twice (once in the sweep, once in three repeat runs). After moving the notify under the lock, 120 consecutive soak runs (depth 4 / depth 2 / pool 3 / default, 30 each, 2 reps of the levels replay) did not hang, and the final version ran another 120 (the same four settings, 15 each, levels replay and raw 64 KiB replay) without a hang. The mechanism was not proven beyond the change removing the symptom.

## 11. Integration round (2026-11-26, after the review): applied, re-proved, extended

Machine for everything in this section: the load average was 10-38 the whole time (other workers building and running the title), so absolute numbers are lower and noisier than the idle-machine numbers above; each comparison below is interleaved on the same machine state.

### 11.1 Applied

- Both proposals were re-checked against the sources before applying: `dstorage.cpp`, `dstorage_staging.h`, `shims_gdk/CMakeLists.txt` and `shims_core/src/file_system.cpp` still had the md5s quoted in section 10 (nothing had touched them since the review), `patch --dry-run` was clean, then `patch -p1` was applied to `port/shims_gdk` and `port/shims_core`. The patch was read again against the current code: lock order (queue mutex, then staging mutex, never the reverse), the in-order `try_take` rule, notify under the slot mutex, lifetime (`Close()` waits for the window), cancellation of slots that have not started. No change was needed. One thing it does not do: `CancelRequestsWithTag` writes `entry.cancelled` of a slot while a pool thread may read the entry, a benign race on a bool (the request is dropped at publish either way); it was left.
- The unpatched backend is kept as `baseline/` (md5s in the table in section 2) so the A/B stays reproducible; `scripts/build_shim_ab.py` now builds `a` from `baseline/` and `b` from the live `port/shims_gdk/src`, against a tree `GDK_BUILD` (default `port/build/dstorage-ARCH`), into `port/build/dstorage-ab-ARCH`, and rejects variant names outside `[A-Za-z0-9_]+` (review item 3).
- The stress test moved to `port/shims_gdk/tests/dstorage_pool_stress_test.cpp` and the native-fd test row to `tests/dstorage_native_fd_row.cpp`; `shims_gdk/CMakeLists.txt` builds them as `dstorage_pool_stress_test`, `dstorage_pool_stress_native_fd_test` and `dstorage_native_fd_test` (the 489 checks with the row linked) and registers `gdk_dstorage_native_fd`, `gdk_dstorage_pool_stress`, `gdk_dstorage_pool_stress_native_fd` with ctest. The stress test gained a 4-queue phase and two cancel phases (cancel before Submit: every tagged request left alone; cancel after Submit: each cancelled request is whole or untouched).
- Own build trees: `port/build/dstorage-x86_64`, `dstorage-arm64` (`-DGTA6RT_MODULES="os;isa;seh;loader;shims_core;shims_gdk" -DGTA6RT_WITH_METAL=OFF`, `-j3`) and `port/build/dstorage-host-x86_64` (the whole host, to run the title; `gta6rt` linked once the other workers' half-edited files compiled).

### 11.2 Tests with the applied code (x86_64 under Rosetta and arm64 native, one process at a time)

| program | arm64 | x86_64 |
|---|---|---|
| `dstorage_test` (baseline `a`, live without the row `b`, live with the row `b_fd`) | 489 checks, 0 failures, all three | 489 checks, 0 failures, all three |
| `dstorage_native_fd_test` (the live tree's binary) | 489 / 0 (16 native descriptors) | 489 / 0 |
| `stress_a`, `stress_b`, `stress_b_fd` (600 streams x 2 rounds; 1 queue, 4 queues, cancel) | 0 failures each | 0 failures each |
| `dstorage_pool_stress_native_fd_test` (the live tree's binary) | 0 failures | 0 failures |
| `gdk_dstorage_real_core_test` (real shims_core: `FileNativeFd` row, zlib/raw/failing requests, page-cache residency after a 4 MiB read, plain `pread` control) | 21 checks, 0 failures | 21 checks, 0 failures (after the fix in 11.7b; 1 failure before) |
| 25 repeats of `stress_b_fd 300 1` | 0 failures | not repeated |

Not run: `tests/run_all.sh` over the other 38 shims_gdk programs (other workers have those trees); the 120-run hang soak of section 10 (the final code ran the 25 repeats above only).
`./verify.sh s1` passes with `GTA6RT_BIN=port/build/dstorage-host-x86_64/gta6rt` (the host with the applied backend; verify.sh itself checks `build/macos/gta6rt` for the Mach-O line).

### 11.3 The real-shim A/B again (3 queues, MiB/s out, median of 6 = 2 interleaved rounds x 3 reps; `scripts/e13_ab_confirm.sh`, `scripts/ab_table.py`)

| | levels | core_bvh4 | TTPF |
|---|---|---|---|
| arm64 baseline -> live | 536 -> 2,594 (4.8x) | 2,202 -> 7,945 (3.6x) | 599 -> 3,171 (5.3x) |
| arm64 worst single pairing | 4.1x | 2.9x | 4.0x |
| arm64 CPU per GiB out | 4.8 -> 2.4 s | 1.2 -> 0.9 s | 3.7 -> 1.9 s |
| x86_64 (Rosetta) baseline -> live | 474 -> 1,802 (3.8x) | 1,958 -> 5,952 (3.0x) | 576 -> 2,765 (4.8x) |
| x86_64 worst single pairing | 3.5x | 2.4x | 4.1x |
| x86_64 CPU per GiB out | 5.5 -> 3.3 s | 1.3 -> 1.2 s | 4.3 -> 2.4 s |

The gain holds in direction and size class but is smaller than the review's table (5.2x / 5.2x / 6.2x; Rosetta 4.2x / 3.7x / 5.6x): the baseline is faster in this session (levels 536 against 307 MiB/s) and the machine was busier. core_bvh4 is CPU-bound (inflate ratio 16.8) and is the cell most sensitive to load.
Results: `results/e13_ab_confirm_arm64.txt`, `e13_ab_confirm_x86_64.txt`. Repeated with the final code (after the read cap of 11.7b and the tracing, load average 15-22; `e13_ab_confirm_*_final.txt`): arm64 levels 503 -> 2,172 (4.3x), core_bvh4 2,165 -> 8,646 (4.0x), TTPF 675 -> 3,322 (4.9x); x86_64 474 -> 1,776 (3.7x), 1,944 -> 6,870 (3.5x), 593 -> 3,012 (5.1x); worst single pairings 3.2-3.8x (arm64) and 2.3-4.0x (x86_64).

On the title's own request list (section 11.5: 72,581 zlib and raw file requests, 1.75 GiB stored, 3.97 GiB out, one queue per title queue, 17 queues, the title's request order; `scripts/e15_title_shape_ab.sh`, 2 rounds x 3 reps):
x86_64 546 -> 2,104 MiB/s out (3.9x; the replay takes 7.4 s -> 1.9 s, CPU per GiB 4.9 -> 2.7 s), arm64 1,214 -> 2,652 (2.2x; the baseline already runs 17 worker threads there). `results/e15_title_shape_ab.txt`.

### 11.4 BCPack predictor profile: wired, decode throughput, residual failures

**Wiring.** The shim loads `<shader folder parent>/bcpack/history.bcph`; the host sets the shader folder to `port/build/shaders_vk/apple/spirv`, so the file is `port/build/shaders_vk/apple/bcpack/history.bcph`. It exists there (byte-identical to `spike/r4-bcpack-history/history.bcph`), and the title run logs `dstorage: loaded default BCPack profile; BC6H endpoint/index rows 210/256 249/256; BC7 endpoint/index fields 4228/6144 8160/8192` with no environment variable set (`run/logs/dstorage-shapes1.log` line 3518).
`scripts/ensure_bcpack_history.sh [--force] [--game DIR] [--out FILE]` writes it when missing (`leonida/tools/build_dstorage_history.py --if-needed`, python 3.10+ is picked from `/opt/homebrew/bin`; the system python 3.9 cannot run the tool); `ninja bcpack_history` runs it from the io_bench CMake project. Measured: a forced regeneration into a scratch folder took 126 s and produced a byte-identical file (`cmp`), stamp included. It is not run by any host script yet (`host/tools/*` belong to the integrator): add `sh port/io_bench/scripts/ensure_bcpack_history.sh` before the run. If the file is missing the shim now names the path and this script in its log line (and when no shader folder is known).

**Decode throughput with the profile** (`iobench decode ... --history`, leonida decoder, every BC7 / BC6H chunk of the 57 pools: 5,167 BC7 chunks, 1,097 MiB stored, 1,222 MiB out; 268 BC6H, 68 / 79 MiB; `scripts/e14_bcpack_profile.sh`, `results/e14_bcpack_profile_*.txt`; load average 18-38):

| | arm64 | x86_64 (Rosetta) |
|---|---|---|
| BC7, 1 thread (96 MiB subset) | 26-28 MiB/s out | 22-23 |
| BC7, 4 threads, all | 88-95 | 70 |
| BC7, 14 threads, all | 166-211 (6.5-7.4 s for the whole set) | 115-175 (7.2-10.6 s) |
| BC6H, 1 thread | 27-29 | 20-21 |
| BC6H, 14 threads | 161-165 | 123-131 |
| for comparison BC1 (section 6), 1 thread | 48 | 37 |

BC7 costs about 36-44 CPU seconds per GiB out (arm64), 45-56 (Rosetta). The output digests are identical on both architectures. The 1.2 GiB of BC7 art in all 46+11 pools would take 6-10 s of all 14 cores; the title requests 153 BC7 chunks in its first 110 s (11.5), so decode time is not the limit yet.
Without the profile every one of the 5,435 BPTC chunks (1,165 MiB stored) fails with `missing-profile` (0.0 s of decode work, which is how the earlier benchmark could not measure them).

**Residual failures with the profile, by cause** (leonida's own reason strings; `fitted` predictors allowed as the shim does; `results/e14_bcpack_profile_arm64.txt*.failures_*.tsv` list each failed chunk by file, offset, size and cause):

| codec | chunks | decoded | failed | cause | chunks | stored |
|---|---|---|---|---|---|---|
| BC7 | 5,167 | 5,107 (all `fitted`, none `confirmed`) | 60 (1.2%) | `missing-endpoint` (a block needs an endpoint field the profile does not cover, more often than the per-request tolerance max(2, blocks/1024)) | 53 | 9.6 MiB |
| | | | | `anchor-or-pbit` (consistency check against the reference tolerance fails) | 7 | 0.64 MiB |
| BC6H | 268 | 258 (all `fitted`) | 10 (3.7%) | `missing-endpoint` | 10 | 2.7 MiB |
| both, without the profile | 5,435 | 0 | 5,435 | `missing-profile` | 5,435 | 1,165 MiB |

The profile covers BC7 endpoint fields 4,228 of 6,144 (index fields 8,160 of 8,192) and BC6H endpoint rows 210 of 256 (index rows 249 of 256); the uncovered endpoint fields are what the 63 `missing-endpoint` failures hit. The 70 failing chunks are in 20 pools: `data/ttdb/ui/ttdb_ui.ttpf` 31 (the pool with by far the most), `ttdb_vehicles_loose_1_1` 6, `ttdb_props_3_3` 4, `ttdb_vfx` 3, `ttdb_terrain_5_7` 3, the rest 1-2 each. Failing BC7 chunks are 9 KiB to 976 KiB (median 104 KiB). In the title's first 110 s of boot, 1,749 BCPack requests were made (BC1 1,487, BC7 153, BC5 108, BC6H 1), none matched a failing chunk and none failed (11.5). Not measured: whether `fitted` blocks look wrong on screen (leonida's doc says fitted blocks can differ from console output); whether the UI pool's failures are the missing BRIGHTNESS calibration symbol (the symbol's texture was not identified).

### 11.5 What the title really asks of DirectStorage (first request trace)

Request tracing is in the shim now (off by default): `LEONIDA_DSTORAGE_SHAPES=<file>` appends one tab-separated record per queue creation, request (at `EnqueueRequest`, with the queue's outstanding count and the enqueuing thread), submit, completion (latency, read and decode time, pool or inline, window depth), cancel and queue close; `=1` writes the records to stderr. The format is documented in `shims_gdk/src/dstorage_shapes.h`; `scripts/dstorage_shapes.py` summarises a trace and writes a replay list; `scripts/capture_title_shapes.sh NAME [SECONDS]` runs the title hidden with tracing on and a per-thread CPU census (`GTA6RT_BIN` picks the binary; it waits for the slot).
The existing `LEONIDA_DSTORAGE_TRACE=1` (one verbose `ds-request` line with timings) is unchanged.

First capture: `run/logs/dstorage-shapes1.log`, `results/title_dstorage-shapes1_shapes.tsv`: the host from `port/build/dstorage-host-x86_64` (applied backend, tracing on), hidden window, the S2 input script, 119 s until the process aborted (11.6). It covers the boot, the logo/landing page and the first-run pages' loading. Facts, measured:
- **17 queues**, all created within 0.4 ms of each other by one thread (tid 38932361, also an enqueuing thread): `Name=fiDeviceDirectStorage`, capacity 4159, priority 2 (REALTIME), **auto-submit disabled** (the title submits explicitly: 1,486 submits, p50 of 1-11 requests each, up to 3,676 in one). All are file-source queues (no memory-source queue); 0 cancels and no request tags in the 74,330 requests (status, fence and event entries are not traced).
- **74,330 requests in 115 s of enqueueing (649/s on average, 2-3,000/s in bursts), 1,879 MiB stored** (16.4 MiB/s average): zlib 61,725 (83%), raw 10,856 (15%), BCPack 1,749 (BC1 1,487, BC7 153, BC5 108, BC6H 1; all BCPack requests go to physical page lists). **No GDeflate and no codec outside leonida's list.** 20,479 requests (28%) target physical page lists, the rest plain buffers.
- Sizes: stored p50 4,453 bytes, p90 42 KiB, p99 389 KiB, max 17.6 MiB (destination up to the 32 MiB limit; 527 requests have a destination over 1 MiB); 48% of the requests are 4 KiB or smaller and 75% 16 KiB or smaller; 45% of the bytes are in requests of 256 KiB or more and 17% in the 143 requests of 1 MiB or more.
- **Files**: RPF streams dominate: `levels.rpf` 28,808 requests / 758 MiB, `audio.rpf` 1,076 / 139 MiB (audio is read through DirectStorage, which was unknown), `anim.rpf` 3,013 / 135 MiB, `shaders.rpf` 4,199 / 131 MiB, `packs.rpf` 3,420 / 86 MiB, `textures.rpf`, `data.rpf`, `core_bvh4.rpf`, then TTPF pools (20,650 TTPF requests, `octa_imposters.ttpf` 1,935, `ttdb_props_*`, `ttdb_terrain_*`). Paths are `G:\<name>.rpf` and `G:\.\data\ttdb\...`. One queue (12,775 requests, 17%) carries only requests with a destination of exactly 64 KiB (stored p50 7.9 KiB) from `shaders.rpf`, `levels.rpf`, `core_bvh4.rpf`, `data.rpf`, `packs.rpf` and `common.rpf`.
- **Depth**: outstanding requests at enqueue p50 149, p90 994, p99 2,933, max 3,676 (each queue up to 2,277). Requests wait: enqueue-to-publish latency p50 25 ms, p90 120 ms, p99 269 ms with the pool (per-queue prefetch window full: window p50 = max = 32).
- **Enqueuing threads**: 4 busy threads account for 97% of the requests (36,758 / 24,076 / 8,050 / 3,008), so the title's own request thread count is about 4; a few more enqueue occasionally.
- **Title CPU** (`ps -M` census every 5 s, `results/title_dstorage-shapes1_threads.txt`): the process had 90-175 threads, used 175 CPU seconds in 115 s (1.5 cores on average, about 5 cores at the boot peak at +10..+15 s). Per-thread attribution is not possible (the thread order from `ps -M` changes). The DStorage requests' own read plus decode time, summed over requests, was 44.8 s of that 175 s (it includes IO waits).
- Same title run with the old-like configuration (`LEONIDA_DSTORAGE_POOL=0 _NOCACHE=0 _SYSTEM_ZLIB=0`; a serial worker per queue, cached reads, leonida inflate; not identical to the pre-patch backend, which paired two requests): 73,577 requests in the same 107 s, enqueue-to-publish p50 55 ms, p90 486 ms, p99 1,646 ms, mean 177 ms against 25 / 120 / 269 ms, mean 48 ms with the applied defaults (load average 20 against 10 when they ran; one run each). The title's progress did not improve visibly (cumulative MiB enqueued by +20/+40/+60/+80/+100 s: 346 / 572 / 573 / 932 / 1,647 with the pool, 346 / 572 / 699 / 1,478 / 1,871 old-like; the old-like run was ahead from +60 s on, which is boot-to-boot variation of the title that was not measured): in this part of the boot the title is not waiting on DStorage throughput. The backend's capacity on the same request list is 2.1 GiB/s out (11.3), 100x the title's 36 MiB/s out. What the shapes do say is that story load will be small-request, deep-queue, zlib-dominated, and spread across about 17 queues.

Run with the final code (read cap, QoS knob; `run/logs/dstorage-final.log`, `results/title_dstorage-final_shapes.tsv`, hidden window, 95 s watchdog, rc=124, no abort): 22,789 requests on the same 17 queues in 90 s (zlib 12,973, raw 9,664, BC5 100, BC1 32, BC7 20), 634 MiB stored, every completion S_OK, the profile loaded by default, per-queue enqueue-to-publish p50 5.6-61 ms and p99 17-127 ms. The boot is not deterministic (fewer and different requests than the first capture in the same time), so its counts are not comparable with the first run's; it only shows that the final code works in the title.

### 11.6 Found: the title aborts at about +110 s once BC7 data is real (not in this module)

Both traced runs (applied defaults and the old-like configuration) ended in rc=134 at 119 s and 107 s with Metal's `_mtlValidateArgumentsForTextureViewOnDevice:1877: failed assertion Texture Creation: source texture pixelFormat (MTLPixelFormatBC7_RGBAUnorm) not compatible with texture view pixelFormat (MTLPixelFormatBC1_RGBA_sRGB)` (the second run: `BC7_RGBAUnorm_sRGB` against `BC1_RGBA`), right after the `d3d12: slot-70 texture view` lines (`run/logs/dstorage-shapes1.log` line 7093, `dstorage-shapes2-oldlike.log` line 7028). The profile was loaded in both. Other workers' runs with the same profile (`s3r4-a`) were not seen to abort, so this depends on the binary or the timing; it is the same with the old serial backend, so it is not the pool, `F_NOCACHE` or the system zlib. It belongs to the d3d12_vulkan layer (a BC1 view of an image the layer made as BC7); the driver worker should look at it.

### 11.7 Pool size against the title's own threads

What a real title run showed (11.5): about 4 enqueuing threads, about 1.5 busy cores on average and 5 at the peak, 17 queue threads that mostly publish, 21 pool threads (default) on 14 cores. `tests/dstorage_replay_bench.cpp` replays the title's request list (`DSREPLAY_BURN=N` runs N busy threads at default QoS beside the replay and reports how much work they got relative to running alone: `burn_vs_solo`), `scripts/e16_pool_vs_title.sh`, `results/e16_pool_vs_title_x86_64.txt` (x86_64, 17 queues, 2 rounds x 2 reps, medians; the machine's load average was 17-36, so `burn_vs_solo` is also depressed by other processes):

| pool | no busy threads | 2 busy | 5 busy | busy threads kept (2 / 5) |
|---|---|---|---|---|
| 0 (a worker per queue, no pool) | 1,252 MiB/s | 1,228 | 1,263 | 0.77 / 0.90 |
| 4 | 822 | 698 | 762 | 0.96 / 0.86 |
| 8 | 1,399 | 1,212 | 1,237 | 0.77 / 0.78 |
| 12 | 1,873 | 1,636 | 1,604 | 0.72 / 0.61 |
| **21 (default, 1.5 x 14)** | 2,550 | 2,304 | 2,384 | 0.45 / 0.50 |
| 32 | 2,646 | 2,438 | 2,520 | 0.34 / 0.30 |
| 8, threads at `QOS_CLASS_UTILITY` | 1,176 | 937 | 583 | 0.98 / 0.96 |
| 21, `QOS_CLASS_UTILITY` | 1,074 | 827 | 272 | 0.98 / 0.93 |

Reading: throughput rises with the pool up to about 21 and flattens (+4% at 32); a pool of 4 is worse than none (17 queue threads already work in parallel) and 8 is no better than none; the busy threads lose more of their CPU the larger the pool. Utility QoS protects the busy threads but starves the pool whenever anything else on the machine runs (272 MiB/s at 5 busy threads, 20-30 CPU seconds per GiB), so it is not adopted; `LEONIDA_DSTORAGE_POOL_QOS=utility|initiated|background` exists for experiments only. The cost to the title's own threads is bounded by the work itself (3-5 CPU seconds per GiB): a larger pool ends a burst sooner, so the title loses a bigger share for a shorter time. With the title using about 1.5 cores on average, **the default stays at 1.5 x cores (21 here)**; it is not tuned further because the noise (load average 17-36) is larger than the differences between 12 and 21 for the busy threads, and because the title's boot did not wait on DStorage. Re-run `e16_pool_vs_title.sh` on an idle machine against story-load traces when the driver worker has them (`LEONIDA_DSTORAGE_POOL=12` is the first value to try if the title's frame times suffer during loads).

### 11.7b Found and fixed: `F_NOCACHE` does not hold for reads over 1 MiB under Rosetta

While adding a test that checks the page cache after a DirectStorage read through the real shims_core (`shims_gdk/tests/dstorage_real_core_test.cpp`), the x86_64 build left 75% of a 4 MiB raw read resident and the arm64 build 0%. `tests/nocache_probe.c` isolates it (plain C, one `pread` with `F_NOCACHE`, `mincore` afterwards; the preconditions evict with `msync(MS_INVALIDATE)` and the control without `F_NOCACHE` reads 100% resident):

| one pread of | 64 KiB | 1 MiB | 2 MiB | 4 MiB | 16 MiB | 32 MiB |
|---|---|---|---|---|---|---|
| arm64 resident after | 0% | 0% | 0% | 0% | 0% | 0% |
| x86_64 (Rosetta) resident after | 0% | 0% | 50% | 75% | 94% | 97% |

Only the first MiB of a single `pread` stays out of the cache under Rosetta (the same with 16 KiB-aligned buffers or offsets; many `pread`s of 1 MiB or less are all uncached). So the page-cache protection of section 5.5 (arm64 numbers) would have been lost for every request larger than 1 MiB in the real host: in the title's first 110 s those are 143 requests carrying 17% of the stored bytes (plus all large TTPF streams later).
Fix in `pread_file`: under x86_64 each `pread` is capped at 1 MiB while `F_NOCACHE` is on (`read_file_range` already loops over partial reads); `LEONIDA_DSTORAGE_READ_CHUNK_KIB` overrides (0 = no cap, arm64's default). Cost, x86_64 Rosetta, cap off against on, interleaved, 3 queues (17 for the title list), median of 6 (`scripts/e17_read_chunk.sh`, `results/e17_read_chunk_x86_64.txt`): levels 1,704 -> 1,901 MiB/s, core_bvh4 6,146 -> 6,854, TTPF 3,015 -> 2,866, title list 2,472 -> 2,468 (all within noise; CPU per GiB unchanged). After the fix the real-core test reads 4 MiB and finds 0.0% resident on both architectures. The system-wide page-cache pressure test (5.5) was not re-run under Rosetta.

### 11.8 Review findings addressed

1. STATUS section 8: the "3.5x or more in every pairing" sentence now says it describes those three runs, and that core_bvh4 is CPU-bound and fell to 3.1x on a saturated machine.
2. STATUS section 6: the Rosetta `fresh=1` row (libz 257 against 444 reused) is quoted next to the arm64 one, with the re-measurement.
3. `build_shim_ab.py` validates variant names before any `rmtree` (and the arch).
4. `run_suite.sh`: the cached pressure run is skipped unless `IOB_PRESSURE=1`, and the usage text says why.
5. Unit slips: STATUS already used MiB (5,422 MiB, 2,872 to 7,221 MiB); the "GiB" figures were only in the builder's report.
6. Provenance note (run 1 of `e10_shim_ab_arm64.txt` may predate the final patch): the e13 and e15 results above come from the applied code, so they supersede it for the headline.

### 11.9 Open, ranked

0. Nothing in the pressure study (5.5) was repeated under Rosetta after the read cap (11.7b); do that on an idle machine.
1. The d3d12_vulkan texture-view abort (11.6): it ends every title run at about 110 s with a real BC7 profile.
2. A story-load trace: `capture_title_shapes.sh` on a run that gets past the first-run pages, then `dstorage_shapes.py --replay-tsv` and `e15`/`e16` on it; the boot trace above is 83% zlib RPF streams of 4 KiB median and cannot say what a story load asks for.
3. Host scripts should run `ensure_bcpack_history.sh` (and the package should carry the profile) so a fresh checkout does not hit `missing-profile`.
4. Page-list publishing (`page_transfer`, 28% of the requests) is serial per queue and holds the xmem map lock; not measured on its own.
5. The 70 residual BPTC failures (31 in the UI pool) and whether `fitted` blocks are visibly wrong.
6. Adjacent-request coalescing (5.4) for the RPF streams: the real trace has many neighbouring `levels.rpf` streams at 4 KiB median; not in the backend.
