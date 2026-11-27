# io_bench review

Independent review of `port/io_bench` (S3.4 prep, DStorage to macOS file IO backend), done on 2026-11-26 on the same loaded machine. During my runs the load average was 11.8 to 19 and `top` showed 0.5% idle CPU with 261 MiB of memory unused. The only file I wrote in the project is this one. My builds, request lists and raw outputs went to the session scratchpad. I did not run the gta6rt host.

**Verdict: approved, with non-blocking fixes.** I found no wrong headline number and no unsafe script.

## 1. Headline numbers recomputed from `results/*.txt`

I recomputed these with my own parser, not `make_status.py`, and checked them against STATUS.md and the builder's report.

| claim | recomputed | match |
|---|---|---|
| Shim A/B, 3 queues, baseline (levels / bvh4 / TTPF) | 307 / 1,277 / 403 (median of 9, three files pooled) | yes |
| Shim A/B, 3 queues, proposal | 1,605 / 6,626 / 2,488 | yes |
| Speedups 5.2x / 5.2x / 6.2x | 5.23 / 5.19 / 6.17 | yes |
| Rosetta 4.2x / 3.7x / 5.6x | 1159/278 = 4.17, 6008/1621 = 3.71, 2335/418 = 5.59 | yes |
| 1 and 6 queue rows, `proposal_nofd`, all ablation rows in section 8 | identical | yes |
| CPU per GiB out, levels, 8.4 -> 3.8 s | 8.44 -> 3.79 | yes |
| Per-run table (231/325/321 ...), "proposal at least 3.5x baseline in every pairing" | the worst pairing is 5,070 / 1,444 = 3.51 | yes (from the files) |
| Staging: 16 MiB costs 28 / 61 / 18%; 64 MiB within 4 / 11 / 1%; bvh4 +12% | 0.72 / 0.39 / 0.82; 1662 vs 1605, 5890 vs 6626, 2465 vs 2488 | yes |
| Pressure, 68 GiB stream, cached vs `F_NOCACHE` | +5,422 / +311 MiB file cache; compressor 2,872 -> 7,221 MiB; sweep p50 372.2 / 1.3 ms; 3,934 / 5,323 MiB/s; 14.91 / 2.05 s CPU | yes |
| Device: 6,274 MiB/s ceiling; 4 KiB qd 1 at 11,159 IOPS, p50 0.085, p99 0.155; 190,083 IOPS at qd 32 | identical | yes |
| `mmap` 0.10-0.97x, `dispatch_io` 0.09-0.90x of cached `pread` | 275/2731 = 0.10, 1135/1176 = 0.97; 298/3422 = 0.09, 1063/1176 = 0.90 | yes |
| Decode, interleaved A/B (libz 552/421, leonida 220/196, BC1 48/37) and the e6 tables | identical, after keying on `codec` and `fresh` | yes |
| Raw A/B (64 KiB single queue 5,462 -> 3,703; 1 MiB 6,132 -> 6,272) | identical | yes |
| 20 real title requests, `options=20000e`, `reason=missing-profile`, 4 logs | 20 `ds-request` lines in each of the 4 logs (80 in total), all with `options=20000e`, all `missing-profile`, one queue per log | yes |
| No GDeflate in leonida/docs, leonida/runtime, port/shims_gdk, port/include, port/portable | `grep -ril gdeflate` finds nothing | yes |

Two unit slips in the builder's report (not in STATUS.md, which uses MiB):
- "+5.4 GiB" is 5,422 MiB, which is 5.3 GiB.
- "compressor 2.9 to 7.2 GiB" is 2,872 to 7,221 MiB, which is 2.8 to 7.1 GiB.

## 2. Re-runs (light, one process at a time)

**Build.**
- I configured standalone (`cmake -S port/io_bench -B <scratch> -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64`) and built with `ninja -j2`. It built clean, and leonida's codecs were picked up from `../portable`.
- I did not rebuild x86_64. I ran the existing `port/build/io_bench-x86_64/iobench` instead.

**Patch.**
- The md5s of `dstorage.cpp`, `dstorage_staging.h`, `shims_gdk/CMakeLists.txt` and `shims_core/src/file_system.cpp` match those quoted in STATUS section 10.
- Both patches apply with `patch -p1` to scratch copies.
- The patched `dstorage.cpp` and `dstorage_staging.h` are byte-identical to `port/build/io_bench-shim-{arm64,x86_64}/b/src/`, so the A/B binaries now on disk are the proposed patch.

**Tests** (existing binaries in `port/build/io_bench-shim-arm64`):
- `test_b_fd`: "489 checks, 0 failures", with 16 native descriptors opened.
- `stress_b_fd 600 1`: 600 streams, 0 failures.

**Real-shim A/B.**
- Setup: the request lists were regenerated with my build of `scripts/make_requests.sh`, and the counts are the same as the builder's (2,634 / 1,918 / 2,234). I ran 3 queues, `bench_a` against `bench_b_fd`, interleaved, 2 rounds of 3 repetitions each, then 3 more interleaved rounds of core_bvh4 alone.

  | MiB/s out (median of 6) | levels | core_bvh4 | TTPF |
  |---|---|---|---|
  | baseline | 309 | 1,135 | 389 |
  | proposal | 1,461 | 3,481 | 1,984 |
  | speedup | 4.7x | 3.1x | 5.1x |

  In the extra core_bvh4 rounds the ratios were 5.9x, 5.0x and 4.4x (907/5,365, 783/3,883, 700/3,107).
- The direction and size class are reproduced.
- core_bvh4 is the one shape whose throughput depends on load. Its proposal cells are CPU-bound (ratio 16.8, 21 pool threads), and on a saturated machine I measured 3.1-5.9x against the claimed 5.2x. In one pairing it was below the claimed "3.5x or more in every pairing". That claim holds for the builder's files but is not a property of the backend under arbitrary load.

**Device, sample** (`bulk`, `F_NOCACHE`, 64-512 MiB, 2 trials):

| case | measured here | STATUS |
|---|---|---|
| 4 MiB sequential at qd 4 | 5,874 MiB/s | 6,274 |
| 1 MiB sequential at qd 4 | 5,348 MiB/s | 6,114 |
| 4 KiB random at qd 1 | 6,979 IOPS, p50 0.096 ms, p99 1.175 ms | 11,159, 0.085, 0.155 |
| 4 KiB random at qd 32 | 141,831 IOPS | 190,083 |

`resident_before_pct` was 0 in every run, so eviction works. All four are lower under the heavier load and of the same shape.

**Decode, sample** (levels, one thread, 48 MiB, 2 interleaved rounds):

| arch | libz | leonida | ratio |
|---|---|---|---|
| arm64 | 390 | 170 | 2.3x |
| x86_64 (Rosetta) | 348 | 156 | 2.2x |

STATUS gives 2.5x and 2.1x. All 16 output digests were identical.

**Fresh allocation per request.**
- `results/e6_decode_x86_64.txt` holds a Rosetta `fresh=1` row (libz 257 against 444 reused, -42%) that STATUS leaves out. STATUS quotes only arm64 and concludes "not worth touching".
- Re-measured: Rosetta libz `fresh=1` 319 against 348 (-8%), arm64 477 against 390.
- So the 257 was likely noise and the conclusion stands. STATUS should still quote the x86_64 row, since the host is x86_64.

**Not re-run.** I did not re-run the cached page-cache pressure test, because it deliberately fills the file cache and compresses other processes' memory, and only 261 MiB was free. I checked its numbers from the raw output only (section 1).

## 3. Script and program safety

- **xbox/ is opened read-only everywhere.**
  - Every `open()` on archive data is `O_RDONLY`, and every `mmap` of a file is `PROT_READ`.
  - Eviction is `msync(MS_INVALIDATE)` on a read-only `MAP_SHARED` mapping, which drops clean pages and writes nothing.
  - The only writers are `shape_save` (to `--out`), the stress test and the scripts' `$OUT` redirects.
  - The default game path is the xbox folder, used only for reads.
- **Deletes.**
  - `build_shim_ab.py` calls `shutil.rmtree(port/build/io_bench-shim-<arch>/<variant>)`.
  - `make_status.py` removes only its own `results/_tmp_*`.
  - `tests/dstorage_pool_stress_test.cpp` calls `remove_all` on `<GetTempPathW>/leonida_ds_stress_<pid>`, a directory it creates in the temp folder, outside `port/`. This is the same pattern as the existing shim tests. It is self-cleaning and touches nothing else.
  - The scripts contain no `rm`.
- **Processes.** Nothing launches the gta6rt host. `with_timeout.sh` sends `kill -9` only to its own child pid. The A/B programs are shim test executables relinked from copies of the existing objects. `build_shim_ab.py` only reads `port/build/shims_gdk-<arch>` (`ninja -t commands`, copying the `.a`).
- **Nits** (not blocking):
  1. `build_shim_ab.py` does not sanitise the `VARIANT` argument. A name such as `../../shims_gdk` would make the `rmtree` delete outside `port/build`. Only a mistyped or malicious argument triggers this; reject names that are not `[A-Za-z0-9_]+`.
  2. `run_suite.sh quick` includes `pressure --mode cache --gib 12 --victim-gib 2`, which by design grew the file cache by about 4 GiB and the compressor by about 2.5 GiB in the builder's run. On this shared 24 GiB machine that slows other workers. It aborts only at pressure level 4 (critical). Say so in the usage text, or make the cached pressure run opt-in.

## 4. Patch review (`proposals/dstorage_io_backend.patch`)

- The design matches the description in STATUS:
  - A shared detached pool.
  - A per-queue window of `Slot`s.
  - In-order `try_take` of staging that stops at the first older slot without its share.
  - Publishing and notifying on the queue thread.
  - Cancellation applied to slots that have not started.
  - `set_pending(-1)` for a cancelled prefetched slot.
  - The system zlib tried first, with the leonida inflater kept as the fallback and the source of typed errors.
- The hang fix (notify under the slot mutex) also removes a real use-after-free. Before it, the worker could wake, `pop_front()` the slot and destroy its condition variable while the pool thread was still inside `notify_one()`. That is a sufficient explanation for the hang the builder saw, though like the builder I have not proven it was the cause.
- Lifetime: a queue's window empties only after each scheduled slot has been waited on, and `Close()` waits for `entries_` and `window_` to be empty. A pool job therefore cannot outlive its `Queue`.
- Open points the integrator should know about (all disclosed in STATUS):
  - The single-queue regression on 64 KiB sequential raw reads.
  - The CPU competition of the 21 pool threads with title threads, which was not measured.
  - BC6H and BC7 decode, which were not benchmarked.

## 5. Provenance note

`results/e10_shim_ab_arm64.txt` (run 1) was last written at 12:34:01, 77 s after `proposals/dstorage_io_backend.patch` was last written (12:32:44). Its RESULT lines alone sum to 84 s of wall time. So run 1 either used binaries built from the patched tree before the patch file was (re)written, or it predates the final patch content. I cannot tell which from the files.

Runs 2 and 3 (12:39-12:42) come after the current arm64 binaries (12:40:15), which match the patch byte for byte. My own re-run with those binaries (section 2) agrees within the noise. Run 1's cells agree with runs 2 and 3 in the same range.

## 6. Fixes requested (non-blocking)

1. In STATUS section 8, describe "3.5x or more in every pairing" as a property of these runs. Add that core_bvh4 is CPU-bound and fell to 3.1x in one rerun on a saturated machine.
2. In STATUS section 6, quote the Rosetta `fresh=1` row next to the arm64 one.
3. In `build_shim_ab.py`, validate variant names before the `rmtree`.
4. In `run_suite.sh`, flag that the quick mode's cached pressure run evicts other processes' memory.
5. In the report wording, convert MiB to GiB properly (5,422 MiB is 5.3 GiB; 2,872 to 7,221 MiB is 2.8 to 7.1 GiB).
