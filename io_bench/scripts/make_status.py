"""Regenerates STATUS.md from results/ (tables and the numbers quoted in the prose). usage: python3 -I scripts/make_status.py

The e10 A/B tables pool results/e10_shim_ab_arm64{,_r2,_r3}.txt; results/_tmp_* are scratch files removed at the end."""
import glob
import os
import re
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
R = ROOT + "/results/"


def parse(files):
    rows = []
    for f in files:
        for line in open(R + f):
            if line.startswith("RESULT "):
                rows.append(dict((m.group(1), m.group(2).strip('"')) for m in re.finditer(r'(\w+)=("[^"]*"|\S+)', line)))
    return rows


def rowsel(files, where):
    rows = parse(files if isinstance(files, list) else [files])
    for k, v in where.items():
        rows = [r for r in rows if r.get(k) == str(v)]
    return rows


def V(files, where, metric):
    vals = [float(r[metric]) for r in rowsel(files, where) if metric in r]
    if not vals:
        raise SystemExit(f"no rows: {files} {where} {metric}")
    return statistics.median(vals)


def T(files, rows, metric, cols=None, where="", fmt=0):
    args = ["python3", "-I", "scripts/summarize.py"] + [R + f for f in (files if isinstance(files, list) else [files])] + ["--rows", rows, "--metric", metric, "--fmt", str(fmt)]
    if cols:
        args += ["--cols", cols]
    if where:
        args += ["--where", where]
    return subprocess.run(args, capture_output=True, text=True, cwd=ROOT).stdout.strip()


def tag(src, dst, mode="w", **kv):
    with open(R + dst, mode) as out:
        for line in open(R + src):
            if line.startswith("RESULT "):
                out.write("RESULT " + " ".join(f"{k}={v}" for k, v in kv.items()) + " " + line[7:])


def f0(x):
    return f"{x:,.0f}"


def x(a, b):
    return f"{a / b:.1f}x"


tag("e6_decode_arm64.txt", "_tmp_dec_arm64.txt", arch="arm64")
tag("e6_decode_x86_64.txt", "_tmp_dec_x86_64.txt", arch="x86_64")
DEC = ["_tmp_dec_arm64.txt", "_tmp_dec_x86_64.txt"]
tag("e9_io_arm64.txt", "_tmp_io_arm64.txt", arch="arm64")
tag("e9_io_x86_64.txt", "_tmp_io_x86_64.txt", arch="x86_64")
IO9 = ["_tmp_io_arm64.txt", "_tmp_io_x86_64.txt"]
tag("e10_shim_ab_arm64.txt", "_tmp_ab_arm64.txt", arch="arm64")
tag("e10_shim_ab_arm64_r2.txt", "_tmp_ab_arm64.txt", "a", arch="arm64")
tag("e10_shim_ab_arm64_r3.txt", "_tmp_ab_arm64.txt", "a", arch="arm64")
for i, f in enumerate(("e10_shim_ab_arm64.txt", "e10_shim_ab_arm64_r2.txt", "e10_shim_ab_arm64_r3.txt"), 1):
    tag(f, f"_tmp_ab_run{i}.txt", arch="arm64")
tag("e10_shim_ab_x86_64.txt", "_tmp_ab_x86_64.txt", arch="x86_64")

E1, E2, E3, E4, E5 = "e1_bulk_seq_pread_arm64.txt", "e2_bulk_rand_pread_arm64.txt", "e3_replay_pread_arm64.txt", "e4_mmap_dio_arm64.txt", "e5_hints_arm64.txt"
E7, E7B, E8, E11, E12 = "e7_model_arm64.txt", "e7b_model_pool_arm64.txt", "e8_pressure_arm64.txt", "e11_warm_vs_cold_arm64.txt", "e12_raw_ab_arm64.txt"
MEMBW = "membw.txt"

LV, BV, TT = "req_levels.tsv", "req_bvh4.tsv", "req_ttpf.tsv"
ab = lambda arch, req, label, q, m="out_mibs": V("_tmp_ab_%s.txt" % arch, {"req": req, "label": label, "queues": q}, m)
ceil_seq = max(V(E1, {"io": 4194304, "cache": "nocache", "qd": q}, "mibs") for q in (2, 4, 8))
P = lambda mode, g, m: V(E8, {"mode": mode, "streamed_gib": f"{g:.2f}"}, m)
SHP = lambda s: f"shapes/{s}.iobs"
a64 = lambda impl, shape, codec="zlib": V("e6b_arch_ab.txt", {"arch": "arm64", "impl": impl, "shape": shape, "codec": codec}, "out_mibs_med")
x64 = lambda impl, shape, codec="zlib": V("e6b_arch_ab.txt", {"arch": "x86_64", "impl": impl, "shape": shape, "codec": codec}, "out_mibs_med")
LVS, BVS, TTS = SHP("rpf_levels"), SHP("rpf_core_bvh4"), SHP("ttpf_sample")
D1 = lambda arch, impl, shape, threads, m="out_mibs_med", codec="zlib": V(DEC, {"arch": arch, "impl": impl, "shape": shape, "threads": threads, "codec": codec, "fresh": 0}, m)

# real title requests
reqs = [dict(re.findall(r"(\w+)=(\S+)", l)) for l in open(ROOT + "/../run/logs/r3fix-hidden.log", errors="replace") if "[rt] ds-request" in l]
n64 = sum(1 for r in reqs if r["dst"] == "65536")
n256 = sum(1 for r in reqs if r["dst"] == "262144")
s64 = [int(r["src"]) for r in reqs if r["dst"] == "65536"]
s256 = [int(r["src"]) for r in reqs if r["dst"] == "262144"]
assert len(reqs) == 20 and n64 + n256 == 20

OUT = []
w = OUT.append

w(f"""# io_bench status (S3.4 prep: "DStorage -> macOS file IO backend")

Storage throughput for story load. Standalone module `port/io_bench`; nothing else under `port/` was edited. Every number below was measured on this Mac
on 2026-11-26 and is in `results/*.txt` (one `RESULT` line per run; `scripts/summarize.py` takes medians over the trials). Where a sentence gives a cause it says "likely" unless a measurement isolates it.

## 1. Result

- **Today's DStorage backend** (`port/shims_gdk/src/dstorage.cpp`): one worker thread per queue plus one helper that can overlap one adjacent pair, a 64 MiB shared staging window,
  `pread` through the kernel32 rows into the page cache, `leonida::zlib_inflate`. Replaying real zlib request shapes from the real archives through the shim itself (3 queues, section 8):
  **{f0(ab("arm64", LV, "baseline", 3))} MiB/s out** (levels.rpf streams), **{f0(ab("arm64", BV, "baseline", 3))}** (core_bvh4.rpf), **{f0(ab("arm64", TT, "baseline", 3))}** (TTPF texture chunks), i.e. {f0(ab("arm64", LV, "baseline", 3, "src_mibs"))} / {f0(ab("arm64", BV, "baseline", 3, "src_mibs"))} / {f0(ab("arm64", TT, "baseline", 3, "src_mibs"))} MiB/s
  of stored bytes read, {min(100 * ab("arm64", r, "baseline", 3, "src_mibs") / ceil_seq for r in (LV, BV, TT)):.0f}-{max(100 * ab("arm64", r, "baseline", 3, "src_mibs") / ceil_seq for r in (LV, BV, TT)):.0f}% of the SSD's {f0(ceil_seq)} MiB/s sequential ceiling. It scales with the number of queues (levels {f0(ab("arm64", LV, "baseline", 1))} / {f0(ab("arm64", LV, "baseline", 3))} / {f0(ab("arm64", LV, "baseline", 6))} MiB/s at 1 / 3 / 6)
  and roughly doubles with a faster inflate (section 7); by construction each queue does read, then inflate, one request at a time.
- **Recommended backend** (`proposals/dstorage_io_backend.patch`, not applied): `F_NOCACHE` reads on the native descriptor, a shared prepare pool of 1.5 x cores threads (21 here) fed by a 32-request window per queue with in-order
  publishing, a 256 MiB staging window, system zlib for the inflate (the leonida inflater stays as the error path). Same replay through the patched shim, 3 queues:
  **{f0(ab("arm64", LV, "proposal", 3))} / {f0(ab("arm64", BV, "proposal", 3))} / {f0(ab("arm64", TT, "proposal", 3))} MiB/s = {x(ab("arm64", LV, "proposal", 3), ab("arm64", LV, "baseline", 3))} / {x(ab("arm64", BV, "proposal", 3), ab("arm64", BV, "baseline", 3))} / {x(ab("arm64", TT, "proposal", 3), ab("arm64", TT, "baseline", 3))}**
  (Rosetta: {x(ab("x86_64", LV, "proposal", 3), ab("x86_64", LV, "baseline", 3))} / {x(ab("x86_64", BV, "proposal", 3), ab("x86_64", BV, "baseline", 3))} / {x(ab("x86_64", TT, "proposal", 3), ab("x86_64", TT, "baseline", 3))}), CPU per GiB out {ab("arm64", LV, "baseline", 3, "cpu_s_per_gib_out"):.1f} -> {ab("arm64", LV, "proposal", 3, "cpu_s_per_gib_out"):.1f} s (levels).
  All 489 `dstorage_test` checks and a new ordering/integrity stress test pass with the patch on arm64 and x86_64 (Rosetta).
- **The page cache is the hazard on a 24 GiB machine.** Streaming 68 GiB of levels.rpf with ordinary cached reads grew the file cache by {f0(P("cache", 68, "file_growth_mib"))} MiB, pushed the compressor from {f0(P("cache", 68, "compressor_base_mib"))} to {f0(P("cache", 68, "compressor_peak_mib"))} MiB
  with {f0(P("cache", 68, "decompressions_delta"))} decompressions, and a 2 GiB resident anonymous working set took a median {f0(P("cache", 68, "sweep_p50_ms"))} ms per touch sweep (baseline {P("cache", 68, "sweep_base_ms"):.1f} ms). With `F_NOCACHE` the same stream: cache +{f0(P("nocache", 68, "file_growth_mib"))} MiB,
  sweep {P("nocache", 68, "sweep_p50_ms"):.1f} ms, {f0(P("nocache", 68, "mibs"))} MiB/s instead of {f0(P("cache", 68, "mibs"))}, {P("nocache", 68, "cpu_s"):.1f} s of CPU instead of {P("cache", 68, "cpu_s"):.1f} s.
- **Decompression, not the SSD, is the ceiling on the stored data.** Per thread, arm64 / Rosetta (interleaved A/B on this loaded machine): system zlib {f0(a64("libz", LVS))} / {f0(x64("libz", LVS))} MiB/s out, leonida inflater {f0(a64("leonida", LVS))} / {f0(x64("leonida", LVS))},
  leonida BCPack BC1 decoder {f0(a64("leonida", TTS, "bcpack1"))} / {f0(x64("leonida", TTS, "bcpack1"))}. BCPack is 20% of the texture bytes (`textures.md`); with all 14 cores it decodes {f0(D1("arm64", "leonida", TTS, 14, codec="bcpack1"))} (arm64) / {f0(D1("x86_64", "leonida", TTS, 14, codec="bcpack1"))} (Rosetta) MiB/s out against {f0(ceil_seq)} for the SSD.
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
| `proposals/dstorage_io_backend.patch` | patch for `port/shims_gdk` (`patch -p1` from `port/shims_gdk`), **not applied** |
| `proposals/shims_core_file_native_fd.patch` | the 6-line hook the patch wants in `port/shims_core/src/file_system.cpp`, **not applied** |
| `results/` | every measurement; `shapes_info.txt` is the shape census |

Build (standalone, `-j2`): `cmake -S port/io_bench -B port/build/io_bench-arm64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64`, then `ninja -C port/build/io_bench-arm64 -j2`; the same with `x86_64` (Rosetta; run it under `scripts/with_timeout.sh SECONDS`).
Optional `-DIOB_LIBDEFLATE_DIR=<libdeflate 1.23 source>` adds the libdeflate comparison (MIT; https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.23.tar.gz; not vendored). `-DIOB_PORTABLE_DIR=` (default `../portable`) supplies leonida's
`zlib_inflate` and BCPack decoder; it is read only, and the generated BCPack tables go to the io_bench build tree.

Section 8 is reproduced with: `python3 -I port/io_bench/scripts/build_shim_ab.py arm64 b64:64 b16:16` (needs the existing `port/build/shims_gdk-arm64` tree with `dstorage_test` built; writes to `port/build/io_bench-shim-arm64`, also runs `test_*` = the 489 checks and `stress_*`),
`port/io_bench/scripts/make_requests.sh <iobench> <dir>`, then `port/io_bench/scripts/e10_shim_ab.sh port/build/io_bench-shim-arm64 out.txt <dir>` (x86_64: pass a `with_timeout.sh` prefix as the fourth argument).

**For the integrator**
- `add_subdirectory(io_bench)` in `port/CMakeLists.txt` (after `portable`; it adds only the `iobench` target; configuring it under a parent project was checked). The standalone configure above needs nothing else.
- To adopt the backend, apply both patches. The dstorage patch also adds `find_package(ZLIB)` and `ZLIB::ZLIB` to `shims_gdk/CMakeLists.txt` (macOS SDK libz; the x86_64 slice is used under Rosetta).
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
`priority=2` (REALTIME), one queue, file `G:\\.\\data\\ttdb\\graphics\\ttdb_graphics.ttpf`, `dst` 65,536 ({n64} requests, `src` {min(s64):,}-{max(s64):,}) or 262,144 ({n256}, `src` {min(s256):,}-{max(s256):,}), offsets 256-byte aligned (first 9,288,960). Each failed with `0x89240030`
(`missing-profile`: no BC7 predictor profile in those runs). The first four offsets were checked against that file's chunk table: chunk physical offset = request offset, chunk size minus padding = `src` (51,012 / 49,034 / 64,555 / 57,481), codec 7, virtual delta = `dst` 65,536.
So one TTPF chunk is one DirectStorage request, which is how `iobench shapes ttpf` builds its shapes.

**Not measured:** the title's queue count, capacities and in-flight depth (only the `CreateQueue1` / `OpenFile` / `CreateStatusArray` calls are logged, without arguments); successful requests; any RPF-stream request (none appears in any log).
The RPF shapes assume a request is one zlib stream at its stored offset (the table of contents is encrypted; streams are found by walking, `rpf.md`). Queue count is therefore swept (1-6).

**Shapes measured from the archives** (`results/shapes_info.txt`; `src` = stored bytes = `SourceSize`, `dst` = inflated; the two size-share columns of the TTPF row cover all codecs):

| shape | requests | stored MiB | inflated MiB | ratio | src p50 | src p90 | src p99 | src max | requests <= 16 KiB | bytes in requests >= 256 KiB |
|---|---|---|---|---|---|---|---|---|---|---|
""")


def shape_rows():
    text = open(R + "shapes_info.txt").read().split("\n")
    out, cur, blocks = [], None, {}
    for line in text:
        if line.startswith("shape "):
            cur = line.split()[1]
            blocks[cur] = []
        elif cur:
            blocks[cur].append(line)
    names = [("shapes/rpf_levels.iobs", "levels.rpf, 6 x 128 MiB windows"), ("shapes/rpf_story000.iobs", "story000.rpf, 4 windows (zlib part only)"), ("shapes/rpf_anim.iobs", "anim.rpf, 4 windows"),
             ("shapes/rpf_packs.iobs", "packs.rpf, 4 windows"), ("shapes/rpf_core_bvh4.iobs", "core_bvh4.rpf, 4 windows"), ("ttpf_all.iobs", "all 46 TTPF pools, zlib chunks")]
    for key, label in names:
        b = blocks.get(key)
        if not b:
            continue
        info = [dict(re.findall(r"(\w+)=(\S+)", l)) for l in b if l.startswith("INFO codec=")]
        zl = [d for d in info if d["codec"] == "zlib"][0]
        buckets = [dict(re.findall(r"(\w+)=(\S+)", l)) for l in b if l.startswith("INFO src_le=")]
        small = sum(float(d["pct_n"]) for d in buckets if int(d["src_le"]) <= 16384)
        big = sum(float(d["pct_bytes"]) for d in buckets if int(d["src_le"]) > 262144)
        out.append(f"| {label} | {int(zl['n']):,} | {float(zl['src_mib']):,.0f} | {float(zl['dst_mib']):,.0f} | {float(zl['ratio']):.2f} | {int(float(zl['src_p50'])):,} | {int(float(zl['src_p90'])):,} | {int(float(zl['src_p99'])):,} | {int(float(zl['src_max'])):,} | {small:.0f}% | {big:.0f}% |")
        if key == "ttpf_all.iobs":
            for d in info:
                if d["codec"] in ("bcpack1", "bcpack7"):
                    out.append(f"| TTPF {d['codec']} chunks | {int(d['n']):,} | {float(d['src_mib']):,.0f} | {float(d['dst_mib']):,.0f} | {float(d['ratio']):.2f} | {int(float(d['src_p50'])):,} | {int(float(d['src_p90'])):,} | {int(float(d['src_p99'])):,} | {int(float(d['src_max'])):,} | | |")
    return "\n".join(out)


w(shape_rows() + "\n")
w("""
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

""")
w(T(E1, "io", "mibs", "qd", "cache=cache") + "\n\n`F_NOCACHE`:\n\n" + T(E1, "io", "mibs", "qd", "cache=nocache") + "\n")
w("\nRandom reads over an 8 GiB span, 256 MiB per run (MiB/s), `F_NOCACHE`:\n\n")
w(T(E2, "io", "mibs", "qd", "cache=nocache") + "\n\nIOPS, same runs:\n\n" + T(E2, "io", "iops", "qd", "cache=nocache") + "\n")
cr = lambda io, qd, c: V(E2, {"io": io, "cache": c, "qd": qd}, "mibs")
w(f"""
Cached random reads are within about 20% of these (4 KiB at qd 32: {f0(cr(4096, 32, 'cache'))} vs {f0(cr(4096, 32, 'nocache'))} MiB/s; 16 KiB at qd 32: {f0(cr(16384, 32, 'cache'))} vs {f0(cr(16384, 32, 'nocache'))}).

- Ceiling: **{f0(ceil_seq)} MiB/s** sequential (4 MiB reads, qd 2-8); 1 MiB reads reach it at qd 4. Random reads at qd 32: 256 KiB {f0(cr(262144, 32, 'nocache'))}, 64 KiB {f0(cr(65536, 32, 'nocache'))}, 16 KiB {f0(cr(16384, 32, 'nocache'))} MiB/s.
- A 4 KiB random read takes p50 {V(E2, {'io': 4096, 'cache': 'nocache', 'qd': 1}, 'p50_ms'):.3f} ms / p99 {V(E2, {'io': 4096, 'cache': 'nocache', 'qd': 1}, 'p99_ms'):.3f} ms at qd 1 ({cr(4096, 1, 'nocache'):.0f} MiB/s, {f0(V(E2, {'io': 4096, 'cache': 'nocache', 'qd': 1}, 'iops'))} IOPS) and
  {f0(V(E2, {'io': 4096, 'cache': 'nocache', 'qd': 32}, 'iops'))} IOPS ({cr(4096, 32, 'nocache'):.0f} MiB/s) at qd 32 (p50 {V(E2, {'io': 4096, 'cache': 'nocache', 'qd': 32}, 'p50_ms'):.3f} ms, p99 {V(E2, {'io': 4096, 'cache': 'nocache', 'qd': 32}, 'p99_ms'):.3f} ms). The backend today keeps one or two reads in flight per queue, so a stream of small requests runs at the left edge of these tables.
- CPU per GiB, 1 MiB reads at qd 8: {V(E1, {'io': 1048576, 'cache': 'cache', 'qd': 8}, 'cpu_s_per_gib'):.3f} s cached, {V(E1, {'io': 1048576, 'cache': 'nocache', 'qd': 8}, 'cpu_s_per_gib'):.3f} s with `F_NOCACHE`; 16 KiB reads cost about {V(E1, {'io': 16384, 'cache': 'nocache', 'qd': 8}, 'cpu_s_per_gib'):.1f} s/GiB either way (syscall bound).
- Latency of a 1 MiB `F_NOCACHE` read at qd 4 / 16: p50 {V(E1, {'io': 1048576, 'cache': 'nocache', 'qd': 4}, 'p50_ms'):.2f} / {V(E1, {'io': 1048576, 'cache': 'nocache', 'qd': 16}, 'p50_ms'):.2f} ms, p99 {V(E1, {'io': 1048576, 'cache': 'nocache', 'qd': 4}, 'p99_ms'):.2f} / {V(E1, {'io': 1048576, 'cache': 'nocache', 'qd': 16}, 'p99_ms'):.2f} ms.
- Re-reading data that is already cached (`results/e11_warm_vs_cold_arm64.txt`, 512 MiB sequential, 4 MiB reads): {f0(V(E11, {'io': 4194304, 'qd': 1, 'resident_before_pct': '100.0'}, 'mibs'))} MiB/s at qd 1 and {f0(V(E11, {'io': 4194304, 'qd': 4, 'resident_before_pct': '100.0'}, 'mibs'))} at qd 4, against {f0(ceil_seq)} from the device. That is what `F_NOCACHE` gives up.

### 5.2 Request shapes through `pread` (`qd` threads, one outstanding read each)

384 MiB per run of the shapes of section 3, MiB/s. `runs` = sequential runs of mean length 16 at random places (spatial streaming), `rand` = uniform, `seq` = archive order.

`F_NOCACHE` (no readahead, no cache):

""")
SH = (("rpf_levels", "levels.rpf, runs"), ("rpf_anim", "anim.rpf, runs"), ("ttpf_sample", "TTPF chunks, rand"), ("rpf_core_bvh4", "core_bvh4.rpf, seq"))
QDS = (1, 2, 4, 8, 16, 32)


def shape_table(wh):
    rows = []
    for shape, label in SH:
        cells = [f0(V([E3], dict(wh, shape=SHP(shape), qd=q), "mibs")) for q in QDS]
        rows.append(f"| {label} | " + " | ".join(cells) + " |")
    return "| shape | qd=1 | qd=2 | qd=4 | qd=8 | qd=16 | qd=32 |\n|---|---|---|---|---|---|---|\n" + "\n".join(rows) + "\n"


w(shape_table({"cache": "nocache"}))
w("\nCached, default readahead, no hint:\n\n" + shape_table({"cache": "cache", "rdahead": -1, "advise": 0}))
w("\nCached, `F_RDAHEAD` off, no hint:\n\n" + shape_table({"cache": "cache", "rdahead": 0, "advise": 0}))
w("\nCached with `F_RDADVISE` 16 requests ahead of the one being read:\n\n" + shape_table({"cache": "cache", "rdahead": -1, "advise": 16}))
rows = []
for shape, label in SH:
    for cache, extra in (("nocache", {}), ("cache", {"rdahead": -1, "advise": 0})):
        cells = []
        for q in (4, 16):
            wh = dict({"shape": SHP(shape), "cache": cache, "qd": q}, **extra)
            cells.append(f"{f0(V([E3], wh, 'mibs'))} / {V([E3], wh, 'p50_ms'):.2f} / {V([E3], wh, 'p99_ms'):.2f} / {V([E3], wh, 'cpu_s_per_gib'):.2f}")
        rows.append(f"| {label} | {cache} | " + " | ".join(cells) + " |")
w("\nThroughput (MiB/s) / request latency p50 (ms) / p99 (ms) / CPU (s per GiB) at qd 4 and 16:\n\n| shape | cache | qd 4 | qd 16 |\n|---|---|---|---|\n" + "\n".join(rows) + "\n")
allcfg = {}
for r in parse([E3]):
    allcfg.setdefault((r["shape"], r["cache"], r["rdahead"], r["advise"], r["qd"]), []).append(float(r["mibs"]))
best = max((statistics.median(v), k) for k, v in allcfg.items())
adv_rows = [r for r in parse([E5]) if r["cache"] == "nocache" and int(r["advise"]) > 0 and r["eb"] == "0" and r["coalesce"] == "-1"]
grown = [float(r["file_pages_delta_mib"]) / float(r["bytes_mib"]) for r in adv_rows]
ra_diff = []
for shape, _ in SH:
    for q in (4, 8, 16, 32):
        a_ = V([E3], {"shape": SHP(shape), "cache": "cache", "rdahead": -1, "advise": 0, "qd": q}, "mibs")
        b_ = V([E3], {"shape": SHP(shape), "cache": "cache", "rdahead": 0, "advise": 0, "qd": q}, "mibs")
        ra_diff.append(abs(a_ - b_) / max(a_, b_))
w(f"""
- No shape reaches the device ceiling with any of these at a pool of up to 32: the best median is {f0(best[0])} MiB/s ({best[1][0].split('/')[-1].replace('.iobs', '')}, cache={best[1][1]}, advise {best[1][3]}, qd {best[1][4]}), {100 * best[0] / ceil_seq:.0f}% of {f0(ceil_seq)}. Small requests are bound by per-request latency (5.1).
- `F_RDAHEAD` default vs off matters for one sequential stream only: core_bvh4 at qd 1 {f0(V([E3], {'shape': BVS, 'cache': 'cache', 'rdahead': -1, 'advise': 0, 'qd': 1}, 'mibs'))} MiB/s with the default, {f0(V([E3], {'shape': BVS, 'cache': 'cache', 'rdahead': 0, 'advise': 0, 'qd': 1}, 'mibs'))} off; from qd 4 up the two tables above differ by at most {100 * max(ra_diff):.0f}% in every one of their {len(ra_diff)} cells.
- `F_RDADVISE` is the strongest single hint at low depth: at qd 1 it lifts TTPF from {f0(V([E3], {'shape': TTS, 'cache': 'cache', 'rdahead': -1, 'advise': 0, 'qd': 1}, 'mibs'))} to {f0(V([E3], {'shape': TTS, 'cache': 'cache', 'rdahead': -1, 'advise': 16, 'qd': 1}, 'mibs'))} MiB/s and levels from {f0(V([E3], {'shape': LVS, 'cache': 'cache', 'rdahead': -1, 'advise': 0, 'qd': 1}, 'mibs'))} to {f0(V([E3], {'shape': LVS, 'cache': 'cache', 'rdahead': -1, 'advise': 16, 'qd': 1}, 'mibs'))}.
  It works under `F_NOCACHE` as well, but the advised pages then stay in the page cache after being read: in the {len(adv_rows)} `F_NOCACHE` + advise runs without evict-behind, the cache grew by a median {100 * statistics.median(grown):.0f}% of the bytes read (`file_pages_delta_mib` / `bytes_mib`; individual runs vary because other processes move the counter).

Distance sweep of `F_RDADVISE` (MiB/s at qd 1 / 4 / 8; `nocache` rows keep `F_NOCACHE` on):

""")
rows = []
for shape, label in (("rpf_levels", "levels.rpf runs"), ("ttpf_sample", "TTPF rand")):
    for cache in ("nocache", "cache"):
        for adv in (0, 4, 16, 64, 256):
            if cache == "cache" and adv == 0:
                continue
            files = [E5, E3] if adv == 0 else [E5]
            cells = []
            for q in (1, 4, 8):
                wh = {"shape": SHP(shape), "cache": cache, "advise": adv, "qd": q, "eb": 0, "coalesce": -1}
                if adv == 0:
                    wh["rdahead"] = -1
                cells.append(f0(V(files, wh, "mibs")))
            rows.append(f"| {label} | {cache} | {adv} | " + " | ".join(cells) + " |")
w("| shape | cache | advise | qd=1 | qd=4 | qd=8 |\n|---|---|---|---|---|---|\n" + "\n".join(rows) + "\n")

w("""
### 5.3 `mmap` and `dispatch_io`

Same shapes, MiB/s. `mmap`: one `MAP_SHARED` mapping per file, a `memcpy` per request. `dispatch_io`: one `DISPATCH_IO_RANDOM` channel per file, `dispatch_io_read` per request, copy out of the `dispatch_data`, qd = blocks in flight, single submitting thread.

""")
rows = []
ratios = {"mmap": [], "dio": []}
for shape, label in (("rpf_levels", "levels.rpf runs"), ("ttpf_sample", "TTPF rand")):
    def row(name, files, wh, kind=None):
        cells = []
        for q in (1, 4, 16, 32):
            v = V(files, dict(wh, shape=SHP(shape), qd=q), "mibs")
            cells.append(f0(v))
            if kind:
                ratios[kind].append(v / V([E3], {"shape": SHP(shape), "cache": "cache", "rdahead": -1, "advise": 0, "qd": q}, "mibs"))
        return f"| {label} | {name} | " + " | ".join(cells) + " |"
    rows.append(row("pread, cached", [E3], {"cache": "cache", "rdahead": -1, "advise": 0}))
    rows.append(row("pread, F_NOCACHE", [E3], {"cache": "nocache"}))
    rows.append(row("mmap, no madvise", [E4], {"backend": "mmap", "madv": "none", "advise": 0}, "mmap"))
    rows.append(row("mmap, MADV_RANDOM", [E4], {"backend": "mmap", "madv": "random", "advise": 0}, "mmap"))
    rows.append(row("mmap, MADV_WILLNEED per request", [E4], {"backend": "mmap", "madv": "willneed", "advise": 0}, "mmap"))
    rows.append(row("dispatch_io, cached", [E4], {"backend": "dio", "cache": "cache", "advise": 0, "hw": 0}, "dio"))
    rows.append(row("dispatch_io, F_NOCACHE", [E4], {"backend": "dio", "cache": "nocache", "advise": 0, "hw": 0}, "dio"))
w("| shape | backend | qd=1 | qd=4 | qd=16 | qd=32 |\n|---|---|---|---|---|---|\n" + "\n".join(rows) + "\n")
dio_t = [V([E4], {"backend": "dio", "cache": c, "advise": 0, "hw": 0, "shape": TTS, "qd": q}, "mibs") / V([E3], {"shape": TTS, "cache": "cache", "rdahead": -1, "advise": 0, "qd": q}, "mibs") for c in ("cache", "nocache") for q in (1, 4, 16, 32)]
dio_l = [V([E4], {"backend": "dio", "cache": c, "advise": 0, "hw": 0, "shape": LVS, "qd": q}, "mibs") / V([E3], {"shape": LVS, "cache": "cache", "rdahead": -1, "advise": 0, "qd": q}, "mibs") for c in ("cache", "nocache") for q in (1, 4, 16, 32)]
w(f"""
Against cached `pread` at the same depth, `mmap` runs at {min(ratios['mmap']):.2f}-{max(ratios['mmap']):.2f}x (the low end is qd 32 on the levels shape, 0.10x with `MADV_RANDOM` and 0.25x without a hint, likely page-fault contention) and never beats it. `dispatch_io` runs at {min(dio_l):.2f}-{max(dio_l):.2f}x on levels and {min(dio_t):.2f}-{max(dio_t):.2f}x on random TTPF chunks
(`set_high_water` of 64 KiB and 1 MiB did not change that: `results/e4_mmap_dio_arm64.txt`). Neither is worth using; `pread` on a pool is the backend.

### 5.4 Coalescing adjacent requests, and evicting behind

`--coalesce GAP` sorts each batch of 256 requests by offset and merges neighbours closer than GAP bytes (at most 4 MiB per read); throughput counts useful bytes. `evict-behind` drops each request's pages with `msync(MS_INVALIDATE)` right after the read.

""")
rows = []
for shape, label in (("rpf_levels", "levels.rpf runs"), ("rpf_core_bvh4", "core_bvh4.rpf seq"), ("ttpf_sample", "TTPF rand")):
    def r4(name, files, wh, extra=""):
        cells = [f0(V(files, dict(wh, shape=SHP(shape), qd=q), "mibs")) for q in (1, 4, 8)]
        return f"| {label} | {name} | " + " | ".join(cells) + f" | {extra} |"
    base = {"cache": "nocache", "advise": 0, "eb": 0, "coalesce": -1, "rdahead": -1}
    rows.append(r4("F_NOCACHE", [E5, E3], base, f"{V([E5, E3], dict(base, shape=SHP(shape), qd=8), 'n'):.0f}"))
    rows.append(r4("F_NOCACHE, merge gap 64 KiB", [E5], {"cache": "nocache", "advise": 0, "eb": 0, "coalesce": 65536}, f"{V([E5], {'cache': 'nocache', 'advise': 0, 'eb': 0, 'coalesce': 65536, 'shape': SHP(shape), 'qd': 8}, 'n'):.0f}"))
    rows.append(r4("F_NOCACHE + advise 16", [E5], {"cache": "nocache", "advise": 16, "eb": 0, "coalesce": -1}))
    rows.append(r4("F_NOCACHE + advise 16 + evict-behind", [E5], {"cache": "nocache", "advise": 16, "eb": 1, "coalesce": -1}))
    rows.append(r4("cached + advise 16 + evict-behind", [E5], {"cache": "cache", "advise": 16, "eb": 1, "coalesce": -1}))
w("| shape | variant | qd=1 | qd=4 | qd=8 | requests read |\n|---|---|---|---|---|---|\n" + "\n".join(rows) + "\n")
cw = {"cache": "nocache", "advise": 0, "eb": 0, "coalesce": 65536, "qd": 8}
w(f"""
- Coalescing is large where streams are adjacent: levels.rpf runs {f0(V([E5, E3], {'shape': LVS, 'cache': 'nocache', 'advise': 0, 'eb': 0, 'coalesce': -1, 'rdahead': -1, 'qd': 8}, 'mibs'))} -> {f0(V([E5], dict(cw, shape=LVS), 'mibs'))} MiB/s at qd 8 with {100 * V([E5], dict(cw, shape=LVS), 'read_mib') / V([E5], dict(cw, shape=LVS), 'bytes_mib') - 100:.0f}% extra bytes read;
  core_bvh4 {f0(V([E5, E3], {'shape': BVS, 'cache': 'nocache', 'advise': 0, 'eb': 0, 'coalesce': -1, 'rdahead': -1, 'qd': 8}, 'mibs'))} -> {f0(V([E5], dict(cw, shape=BVS), 'mibs'))}. It does nothing for random requests (TTPF). A gap of 0 merges nothing: streams are not byte-adjacent (padding between them).
  It needs the whole submit batch and a staging buffer shared by the merged requests, so it is **not in the patch**: the first follow-up (section 9).
- `evict-behind` keeps the cache empty while `F_RDADVISE` prefetches. Its price is a `msync` per request: at qd 1 levels {f0(V([E5], {'shape': LVS, 'cache': 'nocache', 'advise': 16, 'eb': 0, 'coalesce': -1, 'qd': 1}, 'mibs'))} -> {f0(V([E5], {'shape': LVS, 'cache': 'nocache', 'advise': 16, 'eb': 1, 'coalesce': -1, 'qd': 1}, 'mibs'))} MiB/s, TTPF {f0(V([E5], {'shape': TTS, 'cache': 'nocache', 'advise': 16, 'eb': 0, 'coalesce': -1, 'qd': 1}, 'mibs'))} -> {f0(V([E5], {'shape': TTS, 'cache': 'nocache', 'advise': 16, 'eb': 1, 'coalesce': -1, 'qd': 1}, 'mibs'))}; at qd 8 levels {f0(V([E5], {'shape': LVS, 'cache': 'nocache', 'advise': 16, 'eb': 0, 'coalesce': -1, 'qd': 8}, 'mibs'))} -> {f0(V([E5], {'shape': LVS, 'cache': 'nocache', 'advise': 16, 'eb': 1, 'coalesce': -1, 'qd': 8}, 'mibs'))}, TTPF {f0(V([E5], {'shape': TTS, 'cache': 'nocache', 'advise': 16, 'eb': 0, 'coalesce': -1, 'qd': 8}, 'mibs'))} -> {f0(V([E5], {'shape': TTS, 'cache': 'nocache', 'advise': 16, 'eb': 1, 'coalesce': -1, 'qd': 8}, 'mibs'))}.

### 5.5 Page-cache pressure on a 24 GiB machine

`iobench pressure` streams a range of levels.rpf (70.9 GiB: the "76 GiB" file, 76,155,065,056 bytes) with 4 threads x 1 MiB `pread` while a victim holds 2 GiB of incompressible anonymous memory (a stand-in for title memory) and touches every page once per interval, timing the sweep
(`sweep_*_ms`). `evict-behind` here drops the consumed range in 64 MiB batches. It aborts if the kernel pressure level reaches a limit (never did: level 1 throughout).

""")
w(T(E8, "mode,streamed_gib", "mibs,cpu_s,file_growth_mib,peak_file_mib,min_free_mib,compressor_base_mib,compressor_peak_mib,pageouts_delta,decompressions_delta,sweep_p50_ms,sweep_max_ms", None, "", 0) + "\n")
w(f"""
- Cached: the file cache grows until memory is tight (+{f0(P('cache', 68, 'file_growth_mib'))} MiB, free memory down to {f0(P('cache', 68, 'min_free_mib'))} MiB), the kernel compresses other anonymous memory to make room (compressor {f0(P('cache', 68, 'compressor_base_mib'))} -> {f0(P('cache', 68, 'compressor_peak_mib'))} MiB,
  {f0(P('cache', 68, 'decompressions_delta'))} decompressions, {f0(P('cache', 68, 'pageouts_delta'))} pageouts) and the victim's pages are likely among them (the sweep time is the evidence): median sweep {f0(P('cache', 68, 'sweep_p50_ms'))} ms against {P('cache', 68, 'sweep_base_ms'):.1f} ms at rest, still {f0(P('cache', 68, 'sweep_after_ms'))} ms right after the stream ended.
  The first 12 GiB already cost +{f0(P('cache', 12, 'file_growth_mib'))} MiB of cache and a {f0(P('cache', 12, 'sweep_max_ms'))} ms worst sweep. The cache itself plateaus (peak {f0(P('cache', 68, 'peak_file_mib'))} MiB over the whole 68 GiB), so the damage is churn: every further GiB read evicts or compresses about a GiB of something else.
- `F_NOCACHE`: no growth ({f0(P('nocache', 68, 'file_growth_mib'))} MiB), no victim impact (max sweep {f0(P('nocache', 68, 'sweep_max_ms'))} ms), {f0(P('nocache', 68, 'mibs'))} vs {f0(P('cache', 68, 'mibs'))} MiB/s and {P('nocache', 68, 'cpu_s'):.1f} vs {P('cache', 68, 'cpu_s'):.1f} s of CPU for the same 68 GiB.
- `evict-behind` also protects the victim ({f0(P('evict-behind', 12, 'mibs'))} MiB/s, +{f0(P('evict-behind', 12, 'file_growth_mib'))} MiB, {P('evict-behind', 12, 'cpu_s'):.1f} s of CPU per 12 GiB against {P('nocache', 12, 'cpu_s'):.1f} s for `F_NOCACHE`): the way to combine prefetch hints with a bounded cache, at the cost in 5.4.
- What `F_NOCACHE` gives up is re-reads of data that would have stayed cached (5.1: {f0(V(E11, {'io': 4194304, 'qd': 1, 'resident_before_pct': '100.0'}, 'mibs'))} MiB/s warm at qd 1). With 70+ GiB of streamed archives and 24 GiB of RAM that reuse is unlikely; small hot files (common, data, shaders: under 0.5 GiB each) were not benchmarked separately.

### 5.6 Rosetta (x86_64) vs native file IO

The same configurations from the x86_64 `iobench` (a Rosetta process; `getpagesize()` is 4,096 there; `mincore` and `msync` eviction work unchanged) and the arm64 one, run back to back (`results/e9_io_*.txt`, median MiB/s, arm64 / x86_64):

""")
rows = []
cfgs = [("seq 1 MiB", "bulk", {"io": 1048576, "order": "seq"}, (1, 4, 16)), ("seq 16 KiB", "bulk", {"io": 16384, "order": "seq"}, (1, 4, 16)),
        ("random 4 KiB", "bulk", {"io": 4096, "order": "rand"}, (1, 8, 32)), ("random 64 KiB", "bulk", {"io": 65536, "order": "rand"}, (1, 8, 32))]
for label, bench, wh, qds in cfgs:
    for cache in ("nocache", "cache"):
        cells = []
        for q in qds:
            a = V(IO9[0], dict(wh, cache=cache, qd=q, bench=bench), "mibs")
            b = V(IO9[1], dict(wh, cache=cache, qd=q, bench=bench), "mibs")
            cells.append(f"qd {q}: {f0(a)} / {f0(b)}")
        rows.append(f"| {label} | {cache} | " + " | ".join(cells) + " |")
for shape, label in (("rpf_levels", "levels.rpf runs"), ("ttpf_sample", "TTPF rand")):
    for cache in ("nocache", "cache"):
        cells = []
        for q in (4, 16, 32):
            a = V(IO9[0], {"bench": "replay", "backend": "pread", "shape": SHP(shape), "cache": cache, "qd": q}, "mibs")
            b = V(IO9[1], {"bench": "replay", "backend": "pread", "shape": SHP(shape), "cache": cache, "qd": q}, "mibs")
            cells.append(f"qd {q}: {f0(a)} / {f0(b)}")
        rows.append(f"| {label} | {cache} | " + " | ".join(cells) + " |")
w("| pattern | cache | | | |\n|---|---|---|---|---|\n" + "\n".join(rows) + "\n")
w("""
The file IO path itself shows no systematic Rosetta penalty: most cells are within 10%, a few small-random cells differ by 15-40% in both directions (cached 4 KiB at qd 8: 160 arm64 vs 279 x86_64), which is the size of the noise on this loaded machine.
What Rosetta does cost is CPU work on the data (section 6) and memory-copy bandwidth.

## 6. Decompression

Inputs are real streams (the bytes the shim would read) held in memory; outputs have the known inflated size. The digest of the first 4 KiB of every output is identical for libz, leonida and libdeflate on both architectures (`digest=` in the lines).
Work per run: 48-96 MiB of stored data. Implementations: **libz** = the macOS SDK system zlib 1.2.12 (the x86_64 slice runs under Rosetta); **leonida** = `leonida::zlib_inflate` with the bulk path the shim enables by default (`bytewise` = `LEONIDA_DSTORAGE_FAST_INFLATE=0`);
**libdeflate** 1.23 built from source per architecture (arm64 NEON; x86_64 with its runtime-dispatched code, whatever Rosetta advertises).

Output MiB/s, median of 3 runs, threads 1/2/4/8/14 (10 P + 4 E cores, other workers running: the 14-thread cells are not an idle-machine ceiling):

""")
for shape, label in (("rpf_levels", "levels.rpf streams (ratio 2.1, median stream 4 KiB)"), ("rpf_core_bvh4", "core_bvh4.rpf streams (ratio 16.8)"), ("ttpf_sample", "TTPF zlib chunks (ratio 2.7)"), ("rpf_anim", "anim.rpf streams (ratio 1.7)")):
    w(f"{label}:\n\n")
    w(T(DEC, "arch,impl", "out_mibs_med", "threads", f"shape={SHP(shape)},codec=zlib,fresh=0") + "\n\n")
w("BCPack BC1 (TTPF chunks, `leonida::bcpack::decode`, fitted predictors allowed as the shim does; output MiB/s):\n\n" + T(DEC, "arch,impl", "out_mibs_med", "threads", "codec=bcpack1") + "\n")
w("\nInterleaved single-thread A/B (arm64, x86_64, arm64, ... three rounds, medians, `results/e6b_arch_ab.txt`), the more reliable architecture comparison here (output MiB/s):\n\n")
w(T("e6b_arch_ab.txt", "shape,codec,impl", "out_mibs_med", "arch") + "\n")
bytew_a = V(DEC, {"arch": "arm64", "impl": "leonida-bytewise", "threads": 1, "shape": LVS}, "out_mibs_med")
bytew_x = V(DEC, {"arch": "x86_64", "impl": "leonida-bytewise", "threads": 1, "shape": LVS}, "out_mibs_med")
sc = lambda arch, impl, shape: D1(arch, impl, shape, 8) / D1(arch, impl, shape, 1)
ld_over_libz = [D1("arm64", "libdeflate", s, 1) / D1("arm64", "libz", s, 1) for s in (LVS, BVS, TTS, SHP("rpf_anim"))] + [D1("x86_64", "libdeflate", s, 1) / D1("x86_64", "libz", s, 1) for s in (LVS, BVS, TTS, SHP("rpf_anim"))]
rosetta = [a64(i, LVS) / x64(i, LVS) for i in ("libz", "libdeflate", "leonida")] + [a64(i, BVS) / x64(i, BVS) for i in ("libz", "libdeflate", "leonida")]
in14 = D1("x86_64", "libz", LVS, 14, "in_mibs_med")
bc_in14 = V(DEC, {"arch": "x86_64", "codec": "bcpack1", "threads": 14}, "in_mibs_med")
w(f"""
- System zlib against leonida's inflater, one thread: {x(a64('libz', LVS), a64('leonida', LVS))} (levels) and {x(a64('libz', BVS), a64('leonida', BVS))} (core_bvh4) on arm64, {x(x64('libz', LVS), x64('leonida', LVS))} / {x(x64('libz', BVS), x64('leonida', BVS))} under Rosetta. Leonida's inflater is scalar C++ appending to a `std::vector`;
  its non-default `bytewise` path is {x(D1('arm64', 'leonida', LVS, 1), bytew_a)} (arm64) / {x(D1('x86_64', 'leonida', LVS, 1), bytew_x)} (Rosetta) slower again (levels). libdeflate is {min(ld_over_libz):.2f}-{max(ld_over_libz):.2f}x libz across the four shapes and both architectures.
- Rosetta costs (arm64 / x86_64 per thread): {min(rosetta):.2f}-{max(rosetta):.2f}x for the zlib implementations in the A/B and {a64('leonida', TTS, 'bcpack1') / x64('leonida', TTS, 'bcpack1'):.2f}x for the BCPack decoder (an earlier non-interleaved run measured 53 vs 26 MiB/s, 2.0x: the spread on this machine is that large).
- Scaling at 8 threads over 1 thread: libz on arm64 {sc('arm64', 'libz', LVS):.1f}x (levels) / {sc('arm64', 'libz', BVS):.1f}x (core_bvh4), under Rosetta {sc('x86_64', 'libz', LVS):.1f}x / {sc('x86_64', 'libz', BVS):.1f}x; BCPack BC1 arm64 {D1('arm64', 'leonida', TTS, 8, codec='bcpack1') / D1('arm64', 'leonida', TTS, 1, codec='bcpack1'):.1f}x, Rosetta {D1('x86_64', 'leonida', TTS, 8, codec='bcpack1') / D1('x86_64', 'leonida', TTS, 1, codec='bcpack1'):.1f}x.
  Up to 14 threads gains little (4 of them are E cores and the machine is shared). The core_bvh4 cells under Rosetta are limited by memory bandwidth, likely: ratio 16.8 means 16 output bytes per stored byte (below).
- Against the SSD: x86_64 libz with 14 threads turns {f0(D1('x86_64', 'libz', LVS, 14))} MiB/s of output (levels) into about {f0(in14)} MiB/s of stored data, {100 * in14 / ceil_seq:.0f}% of the SSD's {f0(ceil_seq)}; BCPack BC1 at {f0(D1('x86_64', 'leonida', TTS, 14, codec='bcpack1'))} MiB/s out consumes {f0(bc_in14)} MiB/s, {100 * bc_in14 / ceil_seq:.1f}%.
- The shim's per-request allocations: `data.resize(SourceSize)` zero-fills the read buffer and `inflate` allocates its output. Measured with fresh allocation per request (`fresh=1`, one thread, levels): libz {f0(V(DEC, {'arch': 'arm64', 'impl': 'libz', 'fresh': 1}, 'out_mibs_med'))} MiB/s against {f0(D1('arm64', 'libz', LVS, 1))} reused, leonida {f0(V(DEC, {'arch': 'arm64', 'impl': 'leonida', 'fresh': 1}, 'out_mibs_med'))} against {f0(D1('arm64', 'leonida', LVS, 1))}; removing the zero fill in the model changed
  today's design by {V([E7], {'shape': LVS, 'design': 'current', 'impl': 'leonida', 'queues': 3, 'window_mib': 64, 'zero_fill': 0}, 'out_mibs') / V([E7], {'shape': LVS, 'design': 'current', 'impl': 'leonida', 'queues': 3, 'window_mib': 64, 'zero_fill': 1}, 'out_mibs') - 1:+.0%} / {V([E7], {'shape': BVS, 'design': 'current', 'impl': 'leonida', 'queues': 3, 'window_mib': 64, 'zero_fill': 0}, 'out_mibs') / V([E7], {'shape': BVS, 'design': 'current', 'impl': 'leonida', 'queues': 3, 'window_mib': 64, 'zero_fill': 1}, 'out_mibs') - 1:+.0%} / {V([E7], {'shape': TTS, 'design': 'current', 'impl': 'leonida', 'queues': 3, 'window_mib': 64, 'zero_fill': 0}, 'out_mibs') / V([E7], {'shape': TTS, 'design': 'current', 'impl': 'leonida', 'queues': 3, 'window_mib': 64, 'zero_fill': 1}, 'out_mibs') - 1:+.0%} (levels / core_bvh4 / TTPF; no gain: noise). Not worth touching.
- Memory copy bandwidth (`iobench membw`, 16 MiB blocks, best of 20, median of 3, one thread): arm64 {f0(V(MEMBW, {'arch': 'arm64'}, 'memcpy_mibs'))} MiB/s `memcpy` and {f0(V(MEMBW, {'arch': 'arm64'}, 'memset_mibs'))} `memset`; Rosetta {f0(V(MEMBW, {'arch': 'x86_64'}, 'memcpy_mibs'))} and {f0(V(MEMBW, {'arch': 'x86_64'}, 'memset_mibs'))}. Every output byte is written by the inflate, zero-filled before it and copied once more when published, so
  high-ratio data pays three or four passes at Rosetta's copy speed.

**Where decompression should run.** In the x86_64 host process, on the shared pool (what the patch does), with the system zlib.
- An arm64 helper process would give {min(rosetta):.1f}-{max(rosetta):.1f}x per thread for zlib (the Rosetta cost above) and {a64('leonida', TTS, 'bcpack1') / x64('leonida', TTS, 'bcpack1'):.1f}x for BCPack in the interleaved A/B, against an extra copy of every output byte through shared memory and a second process to build and supervise. In the shim the pool is the larger gain (section 8: 2.7-3.4x for the pool, 2.0-2.4x for the system zlib).
- libdeflate adds {min(ld_over_libz):.2f}-{max(ld_over_libz):.2f}x over libz for one vendored MIT library. Not in the patch (it stays on the SDK's zlib); the next step if inflate is still the limit once the pool is in.
- BCPack is the actual problem: {f0(D1('arm64', 'leonida', TTS, 14, codec='bcpack1'))} MiB/s (arm64) and {f0(D1('x86_64', 'leonida', TTS, 14, codec='bcpack1'))} (Rosetta) with all 14 cores, for the BC1 chunks that are 89.6% of the BCPack bytes. It is the one stage where native arm64, or a faster decoder (NEON, or a Metal compute rANS decoder; neither tried), pays for itself.
  BC6H and BC7 were **not benchmarked**: they need `history.bcph`, which these runs do not have (the real BC7 requests above all failed with `missing-profile`).

## 7. The shim as it is, and the proposed shape (`iobench model`)

`iobench model` re-implements the shim's queue behaviour around real work (real file reads, real inflate, a `memcpy` into a destination): one worker thread per queue (requests spread round-robin), a helper that overlaps one adjacent pair, at most two pairs at once, the shared staging window with ticket order, the 16 MiB per-request preparation limit
(`dstorage_staging.h`). Output MiB/s, arm64, cold cache, 128 MiB (48 MiB for bvh4) of stored data per run, medians of 2-3:

""")
rows = []
for shape, label in (("rpf_levels", "levels"), ("rpf_core_bvh4", "core_bvh4"), ("ttpf_sample", "TTPF")):
    def m(files, **wh):
        return f0(V(files, dict({"shape": SHP(shape)}, **wh), "out_mibs"))
    rows.append(f"| {label} | {m([E7], design='current', impl='leonida', queues=1, window_mib=64, zero_fill=1)} | {m([E7], design='current', impl='leonida', queues=3, window_mib=64, zero_fill=1)} | {m([E7], design='current', impl='leonida', queues=6, window_mib=64, zero_fill=1)} | "
                f"{m([E7], design='current', impl='leonida', queues=3, window_mib=16, zero_fill=1)} / {m([E7], design='current', impl='leonida', queues=3, window_mib=256, zero_fill=1)} | {m([E7], design='current', impl='libz', queues=3, window_mib=64, cache='cache')} |")
w("| shape | today, 1 queue | today, 3 queues | today, 6 queues | today, 3 queues, window 16 / 256 MiB | today + system zlib, 3 queues |\n|---|---|---|---|---|---|\n" + "\n".join(rows) + "\n")
cur_peak = [float(r["peak_staging_mib"]) for r in parse([E7]) if r["design"] == "current"]
w(f"""
The window size changes nothing for today's design (peak staging use {min(cur_peak):.0f}-{max(cur_peak):.0f} MiB: the helper holds at most two requests of 16 MiB or less); a faster inflate roughly doubles it; the queue count scales it because each queue is one worker.

A shared pool of prepare threads (the patch's design: any queue's next requests are prepared in parallel, published in queue order), 3 queues, system zlib, window 256 MiB, MiB/s out:

""")
rows = []
for shape, label in (("rpf_levels", "levels"), ("rpf_core_bvh4", "core_bvh4"), ("ttpf_sample", "TTPF")):
    for cache, files in (("nocache", [E7B]), ("cache", [E7B])):
        cells = []
        for pool in (8, 12, 16, 24, 32):
            try:
                cells.append(f0(V(files, {"shape": SHP(shape), "pool": pool, "cache": cache, "advise": 0, "eb": 0}, "out_mibs")))
            except SystemExit:
                cells.append("")
        if any(cells):
            rows.append(f"| {label} | {cache} | " + " | ".join(cells) + " |")
w("| shape | cache | pool 8 | pool 12 | pool 16 | pool 24 | pool 32 |\n|---|---|---|---|---|---|---|\n" + "\n".join(rows) + "\n")
rows = []
for shape, label in (("rpf_levels", "levels"), ("rpf_core_bvh4", "core_bvh4"), ("ttpf_sample", "TTPF")):
    cells = []
    for pool in (2, 4, 8, 12):
        cells.append(f0(V([E7], {"shape": SHP(shape), "design": "pool", "impl": "libz", "queues": 3, "window_mib": 256, "pool": pool, "cache": "cache", "advise": 0, "fresh": 0}, "out_mibs")))
    rows.append(f"| {label} | " + " | ".join(cells) + " |")
w("\nThe smaller pools, page cache (from `iobench model`, `results/e7_model_arm64.txt`):\n\n| shape | pool 2 | pool 4 | pool 8 | pool 12 |\n|---|---|---|---|---|\n" + "\n".join(rows) + "\n")
pk = [float(r["peak_staging_mib"]) for r in parse([E7B]) if int(r["pool"]) >= 24]
win = lambda shape, wm: f0(V([E7], {"shape": SHP(shape), "design": "pool", "impl": "libz", "queues": 3, "window_mib": wm, "pool": 8, "cache": "cache", "advise": 0, "fresh": 0}, "out_mibs"))
w(f"""
Pools of 16-32 threads on 14 cores keep improving (levels {f0(V([E7B], {'shape': LVS, 'pool': 16, 'cache': 'nocache', 'advise': 0, 'eb': 0}, 'out_mibs'))} -> {f0(V([E7B], {'shape': LVS, 'pool': 32, 'cache': 'nocache', 'advise': 0, 'eb': 0}, 'out_mibs'))} MiB/s from 16 to 32): the threads likely spend much of their time waiting for small reads, so extra threads act as queue depth. Peak staging use with pool 24-32 was {min(pk):.0f}-{max(pk):.0f} MiB.
Window size with the pool (3 queues, pool 8, libz, page cache; MiB/s, levels / core_bvh4 / TTPF; 2-6 trials, noisy): 16 MiB {win('rpf_levels', 16)} / {win('rpf_core_bvh4', 16)} / {win('ttpf_sample', 16)}; 64 MiB {win('rpf_levels', 64)} / {win('rpf_core_bvh4', 64)} / {win('ttpf_sample', 64)}; 256 MiB {win('rpf_levels', 256)} / {win('rpf_core_bvh4', 256)} / {win('ttpf_sample', 256)}; 512 MiB {win('rpf_levels', 512)} / {win('rpf_core_bvh4', 512)} / {win('ttpf_sample', 512)}.

## 8. Through the real shim: baseline vs proposal (`tests/dstorage_replay_bench.cpp`)

The same request lists (`iobench export`: 128 MiB of levels.rpf runs = 2,634 requests, 48 MiB of core_bvh4 = 1,918, 128 MiB of TTPF chunks over 46 files = 2,234, after dropping requests the zlib rules refuse) go to the actual `dstorage.cpp` through `IDStorageQueueX2` (zlib requests, 3 queues unless stated, one status entry per queue at the end,
cold cache every run, 3 repetitions per program run), once for the shim as it is in `port/shims_gdk` and once for the patched copy. The patched copy is linked with a test-side `FileNativeFd` row (`GetFinalPathNameByHandleW` + `open`), because shims_core has no such row yet. MiB/s out; arm64 cells are the median of 9 trials (the experiment ran three times, below), x86_64 cells of 3; `proposal` = the patch's defaults
(F_NOCACHE, pool 21, depth 32, window 256 MiB, system zlib).

""")
rows = []
for q in (1, 3, 6):
    rows.append(f"| {q} queue(s), baseline | " + " | ".join(f0(ab("arm64", r, "baseline", q)) for r in (LV, BV, TT)) + " |")
    rows.append(f"| {q} queue(s), **proposal** | " + " | ".join(f"**{f0(ab('arm64', r, 'proposal', q))}** ({ab('arm64', r, 'proposal', q) / ab('arm64', r, 'baseline', q):.1f}x)" for r in (LV, BV, TT)) + " |")
w("| arm64 native | levels.rpf runs | core_bvh4.rpf | TTPF chunks |\n|---|---|---|---|\n" + "\n".join(rows) + "\n")
rows = []
for lab in ("baseline", "proposal"):
    for r, rn in ((LV, "levels"), (BV, "core_bvh4"), (TT, "TTPF")):
        cells = [f0(V(f"_tmp_ab_run{i}.txt", {"req": r, "label": lab, "queues": 3}, "out_mibs")) for i in (1, 2, 3)]
        rows.append(f"| {lab}, {rn} | " + " | ".join(cells) + " |")
runv = lambda i, r, lab: V("_tmp_ab_run%d.txt" % i, {"req": r, "label": lab, "queues": 3}, "out_mibs")
spread = max(max(runv(i, r, lab) for i in (1, 2, 3)) / min(runv(i, r, lab) for i in (1, 2, 3)) for lab in ("baseline", "proposal") for r in (LV, BV, TT))
minratio = min(runv(i, r, "proposal") / runv(j, r, "baseline") for r in (LV, BV, TT) for i in (1, 2, 3) for j in (1, 2, 3))
w(f"\nThe same experiment in three separate runs (3 queues, MiB/s out; the table above pools all trials of the three runs). The machine's load changed between runs: the same configuration moved by up to {spread:.1f}x between runs, and the proposal stayed {minratio:.1f}x or more above the baseline in every pairing of runs:\n\n| 3 queues | run 1 | run 2 | run 3 |\n|---|---|---|---|\n" + "\n".join(rows) + "\n")
w("\nWhat each piece buys, 3 queues (each row changes one thing from `proposal`; MiB/s out):\n\n")
rows = []
for label, name in (("proposal", "proposal (all)"), ("proposal_nofd", "no native fd (kernel32 rows, page cache)"), ("pool0", "no pool (queue thread only; fd, F_NOCACHE, system zlib, 256 MiB)"), ("pool0_leonida_inflate", "no pool, leonida inflater (fd + F_NOCACHE + 256 MiB only)"),
                    ("leonida_inflate", "pool, leonida inflater"), ("cache", "page cache instead of F_NOCACHE"), ("cache_advise16", "page cache + F_RDADVISE 16"), ("nocache_advise16", "F_NOCACHE + F_RDADVISE 16"),
                    ("pool8", "pool 8"), ("pool16", "pool 16"), ("pool32", "pool 32"), ("depth4", "window depth 4"), ("depth8", "window depth 8"), ("depth64", "window depth 64"), ("window16", "staging 16 MiB"), ("window64", "staging 64 MiB")):
    rows.append(f"| {name} | " + " | ".join(f0(ab("arm64", r, label, 3)) for r in (LV, BV, TT)) + " |")
w("| variant (3 queues) | levels | core_bvh4 | TTPF |\n|---|---|---|---|\n" + "\n".join(rows) + "\n")
wl = lambda label, r: 100 - 100 * ab("arm64", r, label, 3) / ab("arm64", r, "proposal", 3)
w(f"""
- Largest single piece: the pool ({x(ab('arm64', LV, 'proposal', 3), ab('arm64', LV, 'pool0', 3))} / {x(ab('arm64', BV, 'proposal', 3), ab('arm64', BV, 'pool0', 3))} / {x(ab('arm64', TT, 'proposal', 3), ab('arm64', TT, 'pool0', 3))} over `pool0`), then the system zlib ({x(ab('arm64', LV, 'proposal', 3), ab('arm64', LV, 'leonida_inflate', 3))} / {x(ab('arm64', BV, 'proposal', 3), ab('arm64', BV, 'leonida_inflate', 3))} / {x(ab('arm64', TT, 'proposal', 3), ab('arm64', TT, 'leonida_inflate', 3))} over the leonida inflater),
  then the native descriptor ({x(ab('arm64', LV, 'proposal', 3), ab('arm64', LV, 'proposal_nofd', 3))} / {x(ab('arm64', BV, 'proposal', 3), ab('arm64', BV, 'proposal_nofd', 3))} / {x(ab('arm64', TT, 'proposal', 3), ab('arm64', TT, 'proposal_nofd', 3))}; TTPF is inside the noise).
- **Staging window.** 16 MiB costs {wl('window16', BV):.0f}% on core_bvh4 (outputs up to 17 MiB), {wl('window16', LV):.0f}% on levels and {wl('window16', TT):.0f}% on TTPF. 64 MiB is within {abs(wl('window64', LV)):.0f}% / {wl('window64', BV):.0f}% / {abs(wl('window64', TT)):.0f}% of 256 MiB (levels / bvh4 / TTPF): only core_bvh4 shows a difference
  ({100 * ab('arm64', BV, 'proposal', 3) / ab('arm64', BV, 'window64', 3) - 100:.0f}%, about the noise level). The fork's "at least 256 MiB" is therefore not contradicted but not required by these shapes: they need more than 16 MiB and are content with 64 MiB. 256 MiB is a ceiling, not a cost: the model's peak use with pool 24-32 was {min(pk):.0f}-{max(pk):.0f} MiB.
- Window depth 32 is where the curve flattens at 3 queues (levels: depth 4 {f0(ab('arm64', LV, 'depth4', 3))}, 8 {f0(ab('arm64', LV, 'depth8', 3))}, 32 {f0(ab('arm64', LV, 'proposal', 3))}, 64 {f0(ab('arm64', LV, 'depth64', 3))}). `F_RDADVISE` on top of the pool did not help (`nocache_advise16` is {wl('nocache_advise16', LV):.0f}% / {wl('nocache_advise16', BV):.0f}% / {wl('nocache_advise16', TT):.0f}% below `proposal`), so it is off.
- Page cache against `F_NOCACHE` has no consistent throughput winner in the pool design (levels {f0(ab('arm64', LV, 'cache', 3))} vs {f0(ab('arm64', LV, 'proposal', 3))}, TTPF {f0(ab('arm64', TT, 'cache', 3))} vs {f0(ab('arm64', TT, 'proposal', 3))}, core_bvh4 {f0(ab('arm64', BV, 'cache', 3))} vs {f0(ab('arm64', BV, 'proposal', 3))}), so the memory evidence of 5.5 decides for `F_NOCACHE`.

Rosetta (the x86_64 build of the same test programs, `results/e10_shim_ab_x86_64.txt`), MiB/s out, 3 queues:

""")
rows = []
for label, name in (("baseline", "baseline"), ("proposal_nofd", "proposal, no native fd"), ("proposal", "proposal"), ("pool0", "proposal without the pool"), ("leonida_inflate", "proposal with the leonida inflater")):
    rows.append(f"| {name} | " + " | ".join(f"{f0(ab('x86_64', r, label, 3))} (arm64 {f0(ab('arm64', r, label, 3))})" for r in (LV, BV, TT)) + " |")
w("| x86_64 (Rosetta) | levels | core_bvh4 | TTPF |\n|---|---|---|---|\n" + "\n".join(rows) + "\n")
w(f"""
Proposal over baseline under Rosetta: {x(ab('x86_64', LV, 'proposal', 3), ab('x86_64', LV, 'baseline', 3))} / {x(ab('x86_64', BV, 'proposal', 3), ab('x86_64', BV, 'baseline', 3))} / {x(ab('x86_64', TT, 'proposal', 3), ab('x86_64', TT, 'baseline', 3))}. The system zlib is worth {x(ab('x86_64', LV, 'proposal', 3), ab('x86_64', LV, 'leonida_inflate', 3))} / {x(ab('x86_64', BV, 'proposal', 3), ab('x86_64', BV, 'leonida_inflate', 3))} / {x(ab('x86_64', TT, 'proposal', 3), ab('x86_64', TT, 'leonida_inflate', 3))} over the leonida inflater end to end
(levels / bvh4 / TTPF) under Rosetta, against {x(ab('arm64', LV, 'proposal', 3), ab('arm64', LV, 'leonida_inflate', 3))} / {x(ab('arm64', BV, 'proposal', 3), ab('arm64', BV, 'leonida_inflate', 3))} / {x(ab('arm64', TT, 'proposal', 3), ab('arm64', TT, 'leonida_inflate', 3))} on arm64.

Raw (non-transform) requests through the shim (`results/e12_raw_ab_arm64.txt`, arm64, stored MiB/s; `levels` = the 2,634 levels.rpf stream ranges read as raw, `64 KiB` / `1 MiB` = 256 MiB / 384 MiB of sequential raw reads in one file):

""")
rows = []
for req, name in (("req_levels.tsv", "levels ranges (median 4 KiB)"), ("req_raw_64k.tsv", "64 KiB sequential"), ("req_raw_1m.tsv", "1 MiB sequential")):
    for q in (1, 3):
        cells = [f0(V(E12, {"req": req, "label": l, "queues": q}, "src_mibs")) for l in ("baseline", "proposal", "proposal_cache")]
        rows.append(f"| {name} | {q} | " + " | ".join(cells) + " |")
w("| raw requests | queues | baseline | proposal | proposal, `NOCACHE=0` |\n|---|---|---|---|---|\n" + "\n".join(rows) + "\n")
r64 = lambda l, q: V(E12, {"req": "req_raw_64k.tsv", "label": l, "queues": q}, "src_mibs")
r1m = lambda l, q: V(E12, {"req": "req_raw_1m.tsv", "label": l, "queues": q}, "src_mibs")
w(f"""
Raw reads now ride the pool too (read into the prefetch buffer, one `memcpy` at publish). With several queues or small ranges that is a large gain (64 KiB, 3 queues: {f0(r64('baseline', 3))} -> {f0(r64('proposal', 3))}; 1 MiB, 3 queues: {f0(r1m('baseline', 3))} -> {f0(r1m('proposal', 3))}). **One queue reading one file sequentially in 64 KiB requests is
slower than today's cached readahead** ({f0(r64('baseline', 1))} -> {f0(r64('proposal', 1))} MiB/s, {V(E12, {'req': 'req_raw_64k.tsv', 'label': 'proposal', 'queues': 1}, 'cpu_s_per_gib_out'):.1f} against {V(E12, {'req': 'req_raw_64k.tsv', 'label': 'baseline', 'queues': 1}, 'cpu_s_per_gib_out'):.1f} s of CPU per GiB); at 1 MiB it is on par ({f0(r1m('baseline', 1))} -> {f0(r1m('proposal', 1))}).
That is the price of not filling the page cache: the number is still far above what an audio or video stream needs, and `LEONIDA_DSTORAGE_NOCACHE=0` restores the cache (`proposal, NOCACHE=0` column: the pool, not the cache, is what limits the single-queue case).

""")
w(f"""
## 9. Recommendation (with the numbers it rests on)

| setting | recommended | evidence |
|---|---|---|
| read API | `pread` on a native descriptor, from a shared pool | 5.3: `mmap` {min(ratios['mmap']):.2f}-{max(ratios['mmap']):.2f}x and `dispatch_io` {min(dio_t + dio_l):.2f}-{max(dio_t + dio_l):.2f}x of cached `pread` at equal depth |
| page cache | `F_NOCACHE` on every DirectStorage file | 5.5: 68 GiB stream, cache +{P('cache', 68, 'file_growth_mib') / 1024:.1f} GiB, compressor +{(P('cache', 68, 'compressor_peak_mib') - P('cache', 68, 'compressor_base_mib')) / 1024:.1f} GiB, victim sweeps {f0(P('cache', 68, 'sweep_p50_ms'))} ms vs {P('cache', 68, 'sweep_base_ms'):.1f} ms; with `F_NOCACHE` +{P('nocache', 68, 'file_growth_mib') / 1024:.1f} GiB, {P('nocache', 68, 'sweep_max_ms'):.0f} ms, {x(P('nocache', 68, 'mibs'), P('cache', 68, 'mibs'))} the throughput, {x(P('cache', 68, 'cpu_s'), P('nocache', 68, 'cpu_s'))} less CPU |
| in-flight reads | pool of 1.5 x cores threads (8..32; 21 here), window of 32 per queue | 5.2, 7, 8: levels {f0(V([E7B], {'shape': LVS, 'pool': 16, 'cache': 'nocache', 'advise': 0, 'eb': 0}, 'out_mibs'))} (pool 16) -> {f0(V([E7B], {'shape': LVS, 'pool': 32, 'cache': 'nocache', 'advise': 0, 'eb': 0}, 'out_mibs'))} (32); window 4 -> 32: {x(ab('arm64', LV, 'proposal', 3), ab('arm64', LV, 'depth4', 3))} |
| staging window | 256 MiB shared (64 MiB is enough for these shapes; 16 MiB is not) | 8: 16 MiB costs {wl('window16', TT):.0f}-{wl('window16', BV):.0f}%; 256 vs 64 MiB {100 * ab('arm64', BV, 'proposal', 3) / ab('arm64', BV, 'window64', 3) - 100:+.0f}% on core_bvh4, noise elsewhere. `SetStagingBufferSize` keeps its console values (only stored) |
| inflate | system zlib on the pool threads; leonida's inflater only to name errors | 6: {x(a64('libz', BVS), a64('leonida', BVS))}-{x(a64('libz', LVS), a64('leonida', LVS))} per thread; 8: {x(ab('arm64', LV, 'proposal', 3), ab('arm64', LV, 'leonida_inflate', 3))} / {x(ab('arm64', BV, 'proposal', 3), ab('arm64', BV, 'leonida_inflate', 3))} / {x(ab('arm64', TT, 'proposal', 3), ab('arm64', TT, 'leonida_inflate', 3))} end to end (arm64) |
| where to decode | x86_64 host process, same pool; no arm64 helper | 6: helper buys {min(rosetta):.1f}-{max(rosetta):.1f}x per thread and costs a copy and a process; revisit for BCPack only |
| `F_RDAHEAD`, `F_RDADVISE` | default readahead, no `F_RDADVISE` | 5.2: help at qd 1-2 on a single stream only; 8: no gain on the pool; advised pages stay cached (5.2, 5.4) |
| adjacent-request coalescing | **not in the patch**; first follow-up | 5.4: {x(V([E5], dict(cw, shape=LVS), 'mibs'), V([E5, E3], {'shape': LVS, 'cache': 'nocache', 'advise': 0, 'eb': 0, 'coalesce': -1, 'rdahead': -1, 'qd': 8}, 'mibs'))} on adjacent RPF streams at qd 8 for {100 * V([E5], dict(cw, shape=LVS), 'read_mib') / V([E5], dict(cw, shape=LVS), 'bytes_mib') - 100:.0f}% extra bytes; nothing for random TTPF |
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
""")

open(ROOT + "/STATUS.md", "w").write("".join(OUT))
for f in glob.glob(R + "_tmp_*"):
    os.remove(f)
print("wrote STATUS.md", sum(len(s) for s in OUT))
