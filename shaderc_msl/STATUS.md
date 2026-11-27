# gta6rt_shaderc_msl: status

Offline Route A toolchain: translated SPIR-V corpus (`spike/s2-make-game/work/shaders/spirv`, read only) to
content-addressed Metal libraries plus a manifest the backend loads (`include/gta6rt_shaderc_msl/catalog.h`).
Host: M4 Pro, macOS 27.0.1, Metal compiler 32023.922 (`metal -c`), Homebrew `spirv-cross` 1.4.363.0 (arm64), Python 3.9
(system) for the tools. All shader tools ran as native arm64; the Metal tests ran as arm64 (the warm-up archive tool
also as x86_64 under Rosetta). Numbers are measured in this module unless a spike is named. The machine was shared
with other workers (load average 10-42 during this round); all timings below are loaded timings.

## Round 4 addition (2026-11-26, by port/d3d12_metal): `--dual-source`

The adapter measured 121 of the first 400 mesh PSOs refused by Metal with "Fragment shader does not write to render
target color(0), index(1) that is required for blending": dual-source-blend PSOs (SRC1 blend factors). The console
exports blend source 1 as MRT1, so the translation writes it at Location 1 (e.g. `+PS_Cable_MirrorReflection_5863fb9a`:
outputs at Locations 0 and 1). New SPIR-V pass `spv.dual_source` (flag `--dual-source`, pixel shaders only): the
Location 1 output moves to Location 0 Index 1, so SPIRV-Cross writes `[[color(0), index(1)]]` (checked on that shader:
`float4 m_4 [[color(0), index(1)]]`, metallib built). A no-op when the shader has no Location 0 and 1 outputs; the pass
is in the cache key, so variants never collide with the store's libraries. The adapter builds the variant on demand for
PSOs whose blend state names SRC1 factors.

## Round 3 summary (2026-11-26, 15:20-19:40)

Every shader of the corpus now has a defined path; counts are exact and recomputed from the files named in each line
(`tools/final_counts.py`, `port/build/shaderc_msl/final_counts.json`).

- **Batch (item 1):** resumed 15:20, finished 19:40 (real wall time 4 h 20 min (15658 s), at most 3 jobs; the batch driver
  keeps huge kernels (SPIR-V >= 6 MiB) one at a time and reads the job count from a file). Final rows per stage in
  section R3.1. 29,082 of 29,277 shaders built (main 26,308 of 26,503, library 2,683, exe 12, fspx 27, bvh 52), 0 failures other than the 195 geometry shaders.
- **64-vertex meshlet split (item 2): implemented as a SPIR-V transform** (`tools/mesh_split.py`, no translator
  patch): object stage + mesh stage, each original group launches `chunks` mesh groups that each emit at most `vmax`
  vertices. **1,135 of 1,135** shaders that need it (1,051 step-3 + 84 step-2 whose own pipeline fails) built and
  created a Metal mesh pipeline (object + mesh); outputs equal the unsplit shader run as compute on the GPU for 7
  real shaders x 16 groups (V=94, P=84: 1,344 triangles each; 4 of them also at V=P=128: 2,048 triangles each); the generated
  partition code, run on the CPU on 65,000 synthetic meshlets (about 6.9 M triangles), emits every triangle exactly once
  with 0 violations. Limits of that evidence: R3.2.
- **Remaining buckets (item 3):** route flag in the manifest for all 9,282 mesh shaders: `hw` 8,097, `split64`
  1,135, `mesh_as_compute` 50 (the 50 over 124 scalars: translator `--mesh-compute` kernel + generated vertex
  shader, both built, layout in the manifest). The 4 threadgroup-memory compute shaders: built with the exchange buffer
  moved to device memory (SPIR-V transform `tools/cs_xbuf.py`), 4/4 compute pipelines created. GS: 195 have no Metal
  stage (`unsupported.tsv`).
- **Review findings (item 4):** the never-built shaders counted as passing are built (R3.1); 1-job baseline measured
  (R3.1); the compute pipeline sweep (1,942 compute pipelines created, 0 refused, 0 timeouts, max 548 s) finds no hang;
  the reviewer's hung probe is attributed by elimination to one of the huge kernels (R3.5).
- **3 vertex shaders that hang the GPU (item 5):** cause found with a watchdog (R3.6): a data-dependent loop limit
  read from the constant table, bounded by the translator's 65,536-iteration guard; with the limit read as 64 or
  1,024 the draw takes 0.03-0.05 ms; hangs with limits >= 1,048,576. Not a lowering fault.
- **Warm-up (item 6):** 392 boot-path pipelines (3 graphics + 1 compute used in the logged boot window, 120 sibling
  UI/blit/Bink graphics, 268 warm-up compute) built into a `MTLBinaryArchive` (arm64 and x86_64 keys, 11.4 MB each);
  warm run 392 of 392 from the archive (0.14-0.15 s). Files and contract: R3.7.
- **GPU hangs I caused:** 16 (12 intended: the vertex-shader hang matrix of R3.6; 4 unintended: a synthetic mesh shader,
  R3.2). Each run started while no gta6rt process existed, under a watchdog; a hang fails other processes' command buffers
  on this GPU. The synthetic shader's cause is not found and is unrelated to the transform (the unsplit shader hangs identically).


## R3.1 Batch to completion (item 1) and the never-built shaders (item 4)

`tools/batch.py` now: at most 3 jobs (`--jobs`, default cap 3; `--jobs-file` re-read after every shader so the count can be
lowered while other Metal work runs), shaders with SPIR-V >= 6 MiB (`--huge-mb`) run one at a time in a free slot,
`--names-file` for exact lists. Resumed with `--fast-resume` at 15:20 over the same corpus (`spike/s2-make-game/work/shaders/spirv`
and its `library/ exe/ fspx/ bvh/` sub-corpora; recipe version unchanged, so every old key is still valid).

| corpus:stage | total | built (ok) | not built | failed (status) |
|---|---:|---:|---:|---|
| bvh:cs | 52 | 52 | 0 | 0 |
| exe:ps | 11 | 11 | 0 | 0 |
| exe:vs | 1 | 1 | 0 | 0 |
| fspx:cs | 27 | 27 | 0 | 0 |
| library:cs | 2,683 | 2,683 | 0 | 0 |
| main:cs | 2,689 | 2,689 | 0 | 0 |
| main:gs | 195 | 0 | 0 | cross_fail 195 |
| main:ms | 9,282 | 9,282 | 0 | 0 |
| main:ps | 12,101 | 12,101 | 0 | 0 |
| main:vs | 2,236 | 2,236 | 0 | 0 |
| **all** | **29,277** | **29,082** | **0** | **195** (all 195 geometry shaders: no Metal stage) |

Real wall time of the resumed run: 4 h 20 min (15658 s) (start `runs/main/final_batch_start.epoch`, end `runs/main/final_batch_end.epoch`),
the run was at 3 jobs, lowered to 2 or 1 through the jobs file for about 1.5 h while my own Metal work (mesh route build 2 jobs, sweeps) ran; `results.jsonl` rows appended in
this run: 9,429 (5,988 compiled, 3,441 cache hits). Failures other than the 195 geometry shaders (no Metal stage, `cross_fail`): 0.

The built fraction per bucket that round 1's review flagged (rows counted as passing without having been built):
ps atomics/MSL 3.1 bucket, vs subgroup (lane route) bucket and cs atomics are now built together with everything
else: main ps 12,101, main vs 2,236, main cs 2,689 built rows, 0 failures among them.

### Throughput: 1-job baseline (round 1 claim "no loss from parallelism")

144 shaders not yet built (seeded: 60 ps, 45 ms, 15 vs, 24 cs, none above 2 MB SPIR-V; list
`port/build/shaderc_msl/baseline/names.txt`), each run into a fresh store, alternating 1 and 3 jobs (`tools/baseline.sh`,
log `baseline/baseline.log`). 16 of the 144 rows are cache hits inside the sample (identical SPIR-V), so 128 compiles.

| run | jobs | wall s | shaders/h | tool-seconds summed | machine load at start |
|---|---:|---:|---:|---:|---:|
| 1 | 1 | 152.5 | 3,400 | 155.5 | 12.2 |
| 2 | 3 | 61.1 | 8,490 | 177.2 | 16.0 |
| 3 | 1 | 185.2 | 2,800 | 189.9 | 18.9 |
| 4 | 3 | 64.6 | 8,030 | 187.9 | 22.0 |

3 jobs give 2.5x (run 1 to 2) and 2.9x (run 3 to 4) the throughput of 1 job on the same shaders: 83-96% of linear, per-shader tool
time +14% and -1% (the load changed between runs 1 and 3). So the round 1 statement "no measurable scaling loss at 4"
is not shown; at 3 jobs under this load the loss is 4-17%. The 10,435 shaders/h figure of round 1 stands as a 4-job,
ps/ms/vs-mix number. During this round the machine carried 10-42 load from other workers; the resumed run compiled 5,988 shaders in 15,642 s of batch wall time (1,378 per hour; the library compute shaders of 4-5 MB SPIR-V,
25-30 s each, dominate the tail), far below the baseline rates.

## R3.2 Meshlet split as a SPIR-V transform (item 2)

### Why it is feasible at SPIR-V level

The translator's mesh shaders compute every output into per-lane registers (lane t holds vertex t and primitive t), call
`OpSetMeshOutputsEXT(V, P)` after a workgroup barrier, then store lane t's outputs at index t (vertex outputs guarded by
`t < V`, `PrimitiveTriangleIndicesEXT` and the cull flag guarded by `t < P`). `tools/mesh_split.py` keeps the program and
rewrites only that epilogue:

- **Object stage** (a SPIR-V `TaskEXT` module generated per chunk count, one library per chunk count, shared by every
  shader): one thread per original group, writes the original `WorkgroupId` and `NumWorkgroups` to a 24-byte
  `TaskPayloadWorkgroupEXT`, launches `chunks` mesh groups.
- **Mesh stage**: `WorkgroupId`/`NumWorkgroups` of the program come from the payload (loads and access chains of the two
  builtins are redirected); the real `WorkgroupId.x` is the chunk number. Output arrays are retyped to `vmax` (per-vertex)
  and `pmax` (per-primitive) entries and `OutputVertices/OutputPrimitivesEXT` set to them. Before `SetMeshOutputs`: every
  lane publishes its primitive (the three 7-bit indices and a valid bit, from a copy of the pure instructions that
  compute the stored triple); lane 0 walks the P primitives in order keeping a per-vertex tag, cuts a new chunk when
  the next primitive would take the current chunk past `vmax` distinct vertices or `pmax` primitives, and for the chunk
  this group has the number of renumbers its vertices in first-use order and records each primitive's new triple. After a
  barrier `SetMeshOutputs(vertices of this chunk, primitives of this chunk)`; the vertex lane stores to its slot (a vertex
  used by two chunks is stored by its lane in both groups, because every group runs the whole program), primitive lane t
  stores the remapped triple and its cull flag. Vertex/primitive conditions are replaced by "has a slot in this chunk".
- A vertex index the original program leaves unwritten stays unwritten; a primitive past the last chunk is not emitted
  (see "chunk bound" below).
- Anything that does not match the expected epilogue (one `SetMeshOutputsEXT`, local size 128x1x1, the lane index from
  `LocalInvocationIndex`, stores indexed by it) raises `SplitError`: 0 of the 1,135 raised.

Cost: every chunk group runs the whole program (`chunks` times the original) plus lane 0's walk (about 128 loop trips).
Metal's mesh limit is on vertices (and a little on primitives: 19.75 B each, 3 B index + flags); measured with Metal's own
error text: `size = vmax * 16 * ceil(scalars / 4) + pmax * 19.75`.

### Chunk count and vmax (per shader, in the manifest)

`mesh_route.py build` compiles the split variant into the store with a vmax from the size model (`mesh_fit` scalars),
creates the Metal pipeline from the store's libraries, and on Metal's "Total mesh size (S)" refusal derives the largest vmax
that fits and retries (pmax 128, then 64): 1,106 of 1,135 fit on the first try, 25 on the second, 4 needed 3-4.
Result for the 1,135: pmax 128 for 1,134 and 64 for 1; vmax from 64 to 127 (366 at 64, 461 at 100 or more, 18 at 127; median 82);
`chunks` = 2 when vmax >= 90 (499 shaders), else 3 (636 shaders). The mesh library does not depend on the chunk count (only the
object stage does), so `runs/main/split_objects.tsv` lists object libraries for 1..7 chunks and a dispatch may choose more.

**Chunk bound (stated, not hidden):** a meshlet whose primitives share vertices badly can need more chunks than the
manifest gives; the primitives past the last chunk are not emitted. CPU run of the generated walk (`tools/partition_emul.py`,
compiled from the production MSL), chunks needed per meshlet shape:

| shape (128 vertices unless noted) | vmax 64 | vmax 90 | vmax 100 | vmax 127 |
|---|---:|---:|---:|---:|
| strip (p, p+1, p+2) / fan / shuffled strip | 3 | 2 | 2 | 2 |
| clustered (16-vertex groups) | 2 | 2 | 2 | 1 |
| small (10 vertices) / V=64 random | 1 / 2 | 1 / 1 | 1 / 1 | 1 / 1 |
| random indices over 128 vertices | 5 | 3 | 2-3 | 1 |
| every primitive 3 far vertices (worst case) | 7 | 5 | 4 | 4 |

The theoretical bound at vmax 64 is 7 chunks for any input (>= 21 primitives per full chunk); the manifest's 2 or 3 covers
strips, fans, clusters and shuffled strips, not random or adversarial index order. Nothing detects an overflow at run
time (a counter would need a bound buffer); the adapter can raise the chunk count per dispatch from `split_objects.tsv`.

### Evidence

1. **Pipeline creation:** 1,135/1,135 split variants (object + mesh, 128-thread mesh groups) created Metal mesh pipelines
   (`meshsplit/split_rows.jsonl`, `meshsplit/sweep_split*.jsonl`); a first sweep with other settings is kept for the record.
   Unsplit, these shaders are refused by Metal ("Total mesh size" 33,280-63,488 B).
2. **Output comparison against the unsplit shader run as compute on the same input**
   (`tools/mesh_split_check.py`, `tools/mesh_run.mm`): the unsplit reference is the translator's `--mesh-compute` module
   (the same program as a compute kernel that writes counts, vertices and primitives to a buffer), built by this module's
   recipe; the split is run as a capture variant (every output store mirrored to a buffer, chunk bound 7) on the same patterned
   heap and push constants. Per group the multiset of triangles (three corners with position and every user parameter,
   plus the cull flag), the header counts and, for each triangle, the original vertex indices behind the new slots must be
   equal. Results: 7 distinct real shaders x 16 groups with V=94/P=84: all EQUAL (1,344 triangles per shader); 4
   with V=P=128: all EQUAL (2,048 triangles per shader). Files `meshsplit/eq12.jsonl`, `meshsplit/eq4_full.jsonl`.
   **Limit of this evidence:** on every heap pattern I could build, the primitive index words these shaders load are
   all zero (all triangles are (0,0,0)), and the vertex data of different lanes is identical, so the GPU comparison checks
   the plumbing (counts, header, cull flags, outputs, chunking by vertex count) but not the index remapping; the negative
   control (a build that writes a wrong second index) is not detected on this data (`meshsplit/mutant*.jsonl`, EQUAL).
3. **The remapping is checked on the CPU on the generated code** (`tools/partition_emul.py`: the lane-0 walk's MSL, extracted
   from the production build of a real shader, compiled as C++ and run on varied meshlets): 65,000 meshlets (strips,
   fans, random, shuffled, clustered, repeated/degenerate indices, V and P up to 128, vmax 64/90/100/127, 4 different real
   shaders incl. trimmed outputs), about 6.9 million triangles: every valid triangle emitted by exactly one chunk, no chunk
   over vmax/pmax, every emitted triangle's slots map back to its original vertices, slot -> vertex injective, 0 violations.
4. **Performance** (`--perf`, 1,024 groups, V=94/P=84 data, loaded machine, command-buffer GPU time): chunks=3/vmax=64
   split 11.4-22.2 ms against 2.9-5.5 ms for the unsplit shader as compute writing its output buffer (2.8-4.2x), for 4
   shaders (`meshsplit/perf.jsonl`). Not comparable as a frame cost: the compute run writes a buffer and draws nothing.

### The synthetic shader that hung the GPU (incident, cause not found)

To get meshlets with controlled indices I wrote `tests/synthetic_mesh/synth_mesh.glsl` (same epilogue shape; a transform
test with strips, fans, random, shuffled, clustered and repeated indices and a CPU-side expected set,
`tools/synthetic_check.py`). Run through the harness it ended with `kIOGPUCommandBufferCallbackErrorHang` four times
(split with and without capture, with Metal validation on, 1 group, and the **unsplit** shader as a plain mesh pipeline),
each started in a window where no gta6rt process existed and under a watchdog; the unsplit shader hangs too, so the hang is
independent of the transform. I stopped GPU runs of it after the 4th. Every run of the translator's real mesh shaders in the same harness (comparison, negative control, performance) completed without a hang. A hang caused by one process fails other processes' command buffers: if a title run of another worker failed
in that interval, this is a candidate cause. The shader itself remains in the tree, un-run; `tools/synthetic_check.py` is its driver.

## R3.3 Mesh-as-compute and the route for every mesh shader (items 2-3)

Route counts over all 9,282 mesh shaders (`mesh/mesh_route.json`, manifest column `mesh_route`; basis in the json):

| step (mesh_fit) | shaders | hw | split64 | mesh_as_compute |
|---|---:|---:|---:|---:|
| 0 fits as translated | 4,651 | 4,651 | 0 | 0 |
| 1 dead outputs dropped | 3,408 | 3,408 | 0 | 0 |
| 2 + component trim | 122 | 38 | 84 | 0 |
| 3 needs the split (hw refused at 128 vertices) | 1,051 | 0 | 1,051 | 0 |
| 4 over 124 unique scalars | 50 | 0 | 0 | 50 |
| total | 9,282 | **8,097** | **1,135** | **50** |

`hw` evidence: Metal pipelines created from the store's own libraries for 300 (round 1) + 2,811 (round 1) + 597 +
500 (this round: a random sample of the unmeasured step-0 shaders, 500/500 OK); step 2: 36 + 2 built, 49 + 35 refused; 0 refused
among steps 0 and 1; the other 3,851 step-0 shaders are by the size model, which matched 800 of 800 measured (300 + 500).

The 50 (review claim 3: "50 over the scalar limit even when split" was worded wrongly): all 50 have no pipeline-library record, so steps 1-2 were never
applied to them and the scalar count is the as-translated one; measured here: 39 of the 50 were refused by Metal at every vmax
(unique scalar count 125 for 23, 129 for 16; the other 11 were not swept, same model count). Route `mesh_as_compute` (`tools/mesh_msc.py`): the translator's `--mesh-compute` compute shader and the vertex shader
from `tools/mesh_compute_vs.py` (leonida's own generator), each built with the recipe (50/50 both). Contract (also in
`catalog.h`): dispatch the compute kernel with the ORIGINAL mesh grid (threadgroup size 128), the buffer address in push
dwords 36..37 (push block 152 bytes), one slot of `slot_bytes` per workgroup (linear workgroup id x + y*nx + z*nx*ny);
then one non-indexed draw of `groups * max_primitives * 3` vertices with the vertex library (same push block); a
primitive past the count or null is clipped by the vertex shader. Slot layout: 16-byte header (vertex count, primitive
count), vertices (position, then each param at its offset), primitives (three indices, null flag). Manifest columns
`msc_*`. Verified here: both libraries build, the compute kernel runs (it is the reference of the split comparison);
the vertex shader was not run on data and no pixel shader was paired with it.

Geometry shaders: 195 `GS_VoxelizeGeneric`, no Metal stage; the census (section 4) found them in no pipeline record or
logged pipeline; the manifest has no row for them and `runs/main/unsupported.tsv` names them, so a PSO naming one is
refused with a reason instead of looking like a missing build.

## R3.4 Compute shaders over 32 KiB of threadgroup memory (item 3)

The 4 whose LDS allocation is the full 32 KiB plus the wave64 exchange buffer (`%x_buf`, 2151 words): `tools/cs_xbuf.py`
moves `%x_buf` to a storage buffer (descriptor set 0 binding 29, one slice of 2151 words per workgroup indexed by the
linear workgroup id, the base kept in a Private variable because the accesses sit in helper functions): SPIR-V only, no
translator patch. All 4 validate (`spirv-val`), build, and create compute pipelines (static threadgroup memory 32,768 B, 1,024
threads); manifest `cs_route=x_buf_device`, `x_buf_words=2151`, `x_buf_buffer` = the `[[buffer(n)]]` of the scratch
(0 for these 4, push block 1). Runtime contract: bind `x_buf_words*4*groups_x*y*z` bytes at that buffer for every dispatch
(contents need not survive; an indirect dispatch needs the largest group count it can issue). The adapter's
`lds_bytes` pre-check must skip shaders with this route and the 24 `shrink_lds` shaders (their index.json `lds_bytes` is the
translated size, not what Metal sees).

Equivalence check (`tools/xbuf_check.py`): 6 random table-free wave64-emulated compute shaders built both ways, run on the same
heap, whole-heap digest compared: 6/6 equal, but only 1 of the 6 changes the heap on this data (`lens_flares_CS_ResetWorkQueue`), so this is
weak evidence for the transform on data; the 4 target shaders themselves were not run (no data for their tables).
Cost: every cross-lane exchange of the emulated wave64 becomes a device-memory round trip (not measured).

## R3.5 Compute pipeline sweep and the hung reviewer probe (item 4)

`tools/cs_sweep.py` builds each shader's Metal compute pipeline in a probe process with a watchdog (300-2,400 s) and records
the time: 1,811 manifest compute shaders (smallest metallib first, up to 41 s each; stopped there because the 1-3 MB ones take 30-55 s each)
+ 120 more drawn at random from the rest + the 11 metallibs above 3 MB (9 distinct builds): **1,942 distinct compute pipelines created,
0 refused, 0 timeouts.** The slowest are the huge kernels, 263-548 s (loaded): ShadowRenderTiledCascadesCBPPPC 263 s, ShadowRenderTiledCascadesAlphaCBPPPC 378 s, ScreenProbes_ActivateRays
453 s, ScreenProbes_ActivateProbesTarget 512 s, ScreenProbes_ActivateProbes 529 s, RTShadowTrace32TemporalHQ_OneOver2RPP_Tiled
548 s, RTShadowTrace32TemporalHQ_Tiled 530 s, RTShadowTrace32HQ_Tiled 422 s; the library copies of the same libraries
hit Metal's cache (0.01-0.02 s) or take as long (library/lib_cs_21514: 505 s). The slowest non-huge one: 55 s (RaytracingResolve family, 1-3 MB metallibs,
30-55 s each). The reviewer's probe options (`BindingInfo | BufferTypeInfo` reflection) were also tried on the 422 s one: 427 s.
So a pipeline build above 30 minutes is only plausible for one of the huge kernels on a machine several times more loaded
than here; the three of them in the round 1 manifest were RTShadowTrace32HQ_Tiled, RTShadowTrace32TemporalHQ_Tiled and
..._OneOver2RPP_Tiled. The reviewer did not record which shader hung and I could not reproduce a hang, so the attribution to
one of those three is by elimination, not observation. The huge kernels must be built off-thread (7-9 min) as the adapter's
builder threads do.

## R3.6 The 3 vertex shaders that hang the GPU (item 5)

`VS_Transform_PositionOnly_4e3f4a0e`, `VS_Transform_PositionOnly_f7bf2df9`, `VS_TransformEntityMask_99490227`
(`tools/vs_hang_run.py`, `tools/vs_lane_gen.py --stems`, `SHADERC_VS_LANE_HEAP_INT` in `tests/vs_lane_test.mm`; every run a
64-vertex draw, 60 s watchdog, only while no gta6rt process existed; rows in `vs_hang*.jsonl`).
The lane-route MSL has one data-driven loop (`for (;;)` with the translator's guard `_1546 < 65536u`): a per-vertex counter
against a limit word (`_275[21] >= _275[20]`) loaded from the constant table through a V# (base, stride, num_records);
each iteration reads buffer data (loads bounds-checked against num_records) and runs a matrix-vector body. With every heap
word set to the same value N:

| heap word | loop limit | 64-vertex draw, lane route | kernel route |
|---|---:|---|---|
| 1, 2, 4, 8 | tiny | runs (no timing line kept) | |
| 64 | 64 | 0.033-0.044 ms | 0.030-0.049 ms |
| 1,024 | 1,024 | 0.037-0.046 ms | 0.034-0.052 ms |
| 4,096 | 4,096 | 0.94-1.21 ms | 0.58-0.97 ms |
| 16,384 | 16,384 | 3.66-3.69 ms | 1.23-1.47 ms |
| 65,536 | 65,536 | 10.9-12.3 ms | 3.24-3.50 ms |
| 0x00100000, 0x3F000000, 0x3F800000, the float pattern of round 1 | 1M ... 1e9 | **GPU hang** (`kIOGPUCommandBufferCallbackErrorHang`, ~3.4 s watchdog) | not run for the constants; ran ok under the pattern |

So the cause is the loop limit, read from data: time grows with it (about 0.17 us per iteration for a 64-vertex draw);
limits of a million or more hang even though the guard caps them at 65,536 iterations, the same count that takes 11 ms at a limit
of exactly 65,536; I have not explained that difference (the limit word also feeds the V# num_records the loads are
bounds-checked against, so the two cases differ in how many reads are in range). It is not the lane-id lowering (the kernel route runs the same loop, 3.2 ms at 65,536; both routes
run the same SPIR-V). Real data supplies a small limit (bone / entity count). Which limit values real draws use is not known;
the translator's guard (65,536) is what makes a bad limit a hang instead of a longer loop: a smaller guard for these loops
(4,096 costs about 1 ms per 64 vertices) would turn a bad table into a visibly wrong draw instead of a device reset.
(My explanation of why the cap exit itself is slower than the data exit at the same trip count is not verified.)

## R3.7 Warm-up list and pipeline archive for the adapter (item 6)

Sources: `port/run/logs/r4pso-{a,b,c}.log` (census build `spike/r4-pso-census`: every graphics/compute PSO creation with state
and shader names, and per frame the PSO ids drawn/dispatched) and the `pipeline-compile-begin` lines. Outputs (this module):

- `warmup/boot_pipelines.json` (+ `.tsv`, the form the builder reads): ordered list; `tools/warmup_list.py`.
  Tier 1 = used in the boot window of all 3 runs, in first-use order (median seconds): `VS_Blit_6e5520d1 + PS_BlitColour_6e5520d1`
  (15.9-23.2 s), `%VVertexCxformEAlpha_dc4780ad` (34.4-45.8 s), `_VBatchVertexCxformEAlpha_bc8bd7de` (36.8-46.6 s), compute
  `library/lib_cs_01726` (29.2-41.9 s). Tier 2 = the first 120 other graphics creations of run a in creation order (blit, debug
  draw, Bink x5, default UI, Scaleform), deduplicated by (shaders, state). Tier 3 = the 268 compute programs the layer's warm-up
  compiled at boot, in log order. The census saw only 3 graphics PSOs and 1 compute program drawn in 88 s (the title
  stayed on the landing prompt); the later movie and first-run pages are not in any census log.
- The archive: `port/build/shaderc_msl/warmup-archive/` holds `gta6rt-<hash>.metalarchive` for arm64 and for x86_64 (the key
  contains the process architecture; the adapter host is x86_64), 11.4 MB each, 392 pipelines, built by
  `tools/warmup/warmup_build.cpp` through `gta6rt::gpu` (`open_pipeline_cache` + eager `create_render_pipeline` /
  `create_compute_pipeline`, private build `port/build/shaderc_msl-warmup-<arch>` via `tools/warmup/CMakeLists.txt`, which pulls the live
  `port/metal` by path). Cold: 392 created in 10.4 s (arm64) / 10.3 s (x86_64); warm: 392 of 392 from the archive, 0.14-0.15 s.
  The key contains `translator_version`: built with the string the adapter uses (`"shaderc_msl recipe 1"`, d3d12_metal_device.cpp).
  To use: `GTA6RT_METAL_ARCHIVE_DIR=port/build/shaderc_msl/warmup-archive`.
- **What makes it a hit:** the archive matches a pipeline descriptor exactly. Mine come from the logs: topology, render-target and
  depth formats, sample count, blend enable/factors; the input layout is inferred from the vertex shader's signature
  (float components, packed) because the logs do not carry the title's layout; depth test/stencil/raster state are defaults.
  If the adapter's descriptor for the same shaders differs (input layout, blend ops, depth state) it misses and
  compiles as before. The adapter can check cheaply: `Pipeline::from_archive()` on the first creations; the builder
  prints hit/miss counts. A better source is an adapter-side census of the descriptors it builds: give me that list
  (`G`/`C` TSV lines, see `warmup/boot_pipelines.tsv` header) and `warmup_build` makes the archive from it.

Coordination (files only): I read `port/d3d12_metal/STATUS.md` and the adapter's `d3d12_metal_device.cpp`/`metal_shaders.cpp`: it loads this
manifest and `catalog.h` (at the time 20,619 rows), refuses the routes `split64` / `mesh_as_compute` as "no Metal pipeline yet", opens its archive
with `translator_version = "shaderc_msl recipe 1"` in `GTA6RT_METAL_ARCHIVE_DIR` (default `build/d3d12_metal-<arch>/pipeline-archive`), and
builds shaders the store lacks through `tools/shaderc_msl.py`. What I provide is below; I wrote nothing under `port/d3d12_metal`.

Adapter-facing contract:

1. Manifest v3 (`runs/main/manifest.tsv`, 29 tab-separated columns; the catalog in `include/gta6rt_shaderc_msl/catalog.h`,
   `kApiVersion` 3, parses them). Columns 0-10 as before; 11-17 split variant (mesh metallib, object metallib, chunks, vmax,
   pmax, push buffer, tables); 18-25 mesh-as-compute (compute metallib, vertex metallib, their push buffers and tables, layout, params);
   26-28 `cs_route`, `x_buf_words`, `x_buf_buffer`. `mesh_route` is `hw`, `split64` or `mesh_as_compute` for every mesh row.
2. `split64`: create `MeshPipelineDesc{object = object_metallib/main0, mesh = split_metallib/main0, payload_bytes = 24,
   mesh_threads = 128, object_threads = 1}` and dispatch the ORIGINAL grid with `draw_mesh`/`dispatch_mesh`; buffer indices
   (push, tables) are the mesh function's: `split_push`, `split_tables`.
3. `mesh_as_compute`: contract in R3.3.
4. `cs_route=x_buf_device`: R3.4.
5. `runs/main/unsupported.tsv`: shaders with no path (GS): refuse the PSO with the stated reason.

## R3.8 Not done, risks, next steps (round 3)

- Compute pipelines were created for 1,942 of the 5,451 compute shaders in the store (the rest compile to metallibs; a pipeline
  build was not tried for them); no vertex/pixel pipeline with the title's real input layouts and formats was created here
  (the adapter creates those).
- No translated pixel, mesh or compute shader was compared with console output; the split is compared with its own unsplit
  program (R3.2), on data whose index words are degenerate on the real shaders (the CPU run of the generated walk covers
  the index logic). The mesh-as-compute vertex shader and the 4 x_buf compute shaders were not run on data. `shrink_lds`
  (24 shaders, review claim 10) is still unchecked against LDS use on data.
- A split meshlet that needs more chunks than the manifest's `split_chunks` loses its last primitives silently (chunk bound,
  R3.2). Overflow detection needs a diagnostics binding from the adapter; raising the chunk count per dispatch needs no new library.
- 3,851 step-0 `hw` mesh shaders are by the size model (800/800 measured agree); the first pipeline creation shows a miss.
- Metal refusals the adapter reported that are not mine to fix here: dual-source-blend pixel shaders without a second output
  (121 of the first 400 mesh PSOs it probed) and mesh PSOs with conservative rasterization.
- The warm-up archive hits only for descriptors equal to mine (R3.7); the census logs predate the movie and first-run pages.
- The synthetic mesh shader's hang is unexplained (R3.2); the vertex-shader hang matrix hung the GPU 12 times by design.
- Next, ranked: (1) adapter builds `split64` and `mesh_as_compute` pipelines from the manifest and logs chunk-bound misses on
  real meshlets (a real index buffer replaces my data limits); (2) an adapter-side descriptor census to rebuild the archive;
  (3) choose the translator's loop guard for the three transform vertex shaders (R3.6) or leave it; (4) run the 4 x_buf shaders
  and the mesh-as-compute vertex shader on real data; (5) pipeline-create the remaining 3,509 compute shaders in a background
  sweep (about 30 s each for the RaytracingResolve family).


## Module

| path | what |
|---|---|
| `tools/shaderc_msl.py` | one shader: SPIR-V pre-passes, SPIRV-Cross, MSL fix-ups, `metal -c` (timeout, `-O0` retry), `metallib`, content key |
| `tools/spv.py` | SPIR-V reader and the pre-passes (`split_bindings`, `lower_vertex_lane`, `drop_outputs`, `trim_outputs`, `shrink_lds`) |
| `tools/batch.py` | resumable parallel driver (at most 4 processes), ordering, summary, manifest |
| `tools/verify_bindings.py` | binding check by Metal pipeline reflection against index.json |
| `tools/vs_lane_gen.py`, `tests/vs_lane_test.mm`, `tools/vs_lane_run.py`, `tests/vs_lane_identity_test.mm` | vertex subgroup routes run on data |
| `tools/pipelines.py`, `tools/mesh_fit.py`, `tools/mesh_pso.py` | pipeline pairing, mesh output steps (model), mesh steps with real pipelines |
| `tools/gs_census.py` | geometry-shader census |
| `tools/cs_check.py`, `tools/huge_cs.py`, `tools/soft_f64.metal`, `tests/soft_f64_test.mm` | compute shaders at Metal limits |
| `src/catalog.cpp` | manifest reader (`gta6rt_shaderc_msl` static library, C++20) |
| `patches/` | translator / design proposals for what this module cannot fix |

Build: `cmake -S port/shaderc_msl -B port/build/shaderc_msl-arm64 -G Ninja && ninja -C port/build/shaderc_msl-arm64 -j3`
(configured and built here for arm64 and x86_64). The corpus tools are custom targets (`shaderc_msl_batch`,
`shaderc_msl_verify_bindings`, `shaderc_msl_mesh_fit`, `shaderc_msl_mesh_pso`, `shaderc_msl_gs_census`,
`shaderc_msl_cs_check`, `shaderc_msl_pairs`); outputs go to `port/build/shaderc_msl/` and are reproducible from them.
Tests (`ctest`): `shaderc_msl_soft_f64`, `shaderc_msl_vs_lane` (40 corpus vertex shaders), `shaderc_msl_vs_lane_identity`,
`shaderc_msl_catalog` (skips with 77 when the manifest is absent). ctest runs all four with
`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` (since review round 2).

Integrator line for `port/CMakeLists.txt`: `add_subdirectory(shaderc_msl)` (target `gta6rt_shaderc_msl`; macOS only;
tests are off unless configured standalone or `-DGTA6RT_SHADERC_MSL_TESTS=ON`).

## Recipe (version 1)

1. SPIR-V pre-passes: `split_bindings` always; `lower_vertex_lane` for vertex shaders that load the subgroup lane id;
   `drop_outputs` + `trim_outputs` for mesh shaders with a pairing-derived live set; `shrink_lds` for compute shaders
   over 32 KiB whose RSRC2 allocation fits.
2. `spirv-cross --msl --msl-version 30100 --msl-argument-buffers --msl-argument-buffer-tier 1
   --msl-device-argument-buffer 1..29`.
3. MSL fix-ups: local `gl_CullDistance` (vertex shaders that write cull distances), strip `raster_order_group` from
   argument-buffer members (Metal rejects it there), `soft_f64` (Float64 shaders), `noinline_hot_helpers` (huge
   kernels whose forced inlining would add a million SPIR-V words or more), mesh vertex struct sorted by alignment.
4. `metal -c -std=metal3.1 -w` (600 s, then one `-O0` retry), `metallib`.

Content key = SHA-256 over the SPIR-V bytes, translator fingerprint (`shards.json`, `translation_sources.sha256`),
the recipe dict, tool versions (`spirv-cross --revision`, `metal --version`) and every pre-pass parameter. Output
`port/build/shaderc_msl/cas/<k[:2]>/<k>.metallib` + `<k>.json` (the row: timings, peak child RSS, binding plan).

## 1. Binding fix (`split_bindings`)

The S0.4 recipe gives every set-0 binding to one argument buffer `spvDescriptorSet0`, and SPIRV-Cross numbers the
members by binding, so different tables land at the same offsets (port/metal STATUS). The pre-pass rewrites each
`DescriptorSet 0, Binding b` decoration to `DescriptorSet 1+b, Binding 0`. With `--msl-device-argument-buffer 1..29`
each contract binding becomes its own argument buffer `spvDescriptorSet{1+b}` at `[[buffer(1+b)]]` with one member at
offset 0; the push block takes the first free index after them (taken from the plan, not fixed).

Second finding from the check: runtime-sized descriptor arrays (heap-indexed image arrays, e.g. `texture3d<uint,
access::write>` tables) cannot live in a tier-1 argument buffer. SPIRV-Cross passes them as a plain
`device const void* spvDescriptorSet{1+b}Binding0 [[buffer(n)]]` (an array of texture handles). The plan and the
manifest now carry them in their own column (`descriptor_arrays`, binding:buffer); the backend must bind a buffer of
resource IDs there, not an argument-buffer struct. Before this fix the manifest did not list them at all.

Verification (`tools/verify_bindings.py`, seed 7101): 600 shaders, one per distinct (stage, binding set) signature
(75) plus random fill to vs 130, ps 170, cs 130, ms 150, gs 20. Each built with the recipe and with the unmodified
recipe; Metal pipeline reflection (`MTLPipelineOptionBindingInfo | BufferTypeInfo`) compared with index.json and with
the binding plan parsed from the MSL entry point. Mesh shaders used their batch options (dead-output lists).

| stage | n | pipelines reflected | split recipe correct | unmodified recipe: one buffer with overlapping offsets |
|---|---:|---:|---:|---:|
| vs | 130 | 130 | 130 | 1 of 63 reflected (others fail SPIRV-Cross: subgroup builtins) |
| ps | 170 | 170 | 170 | 147 of 170 |
| cs | 130 | 130 | 130 (32 with descriptor-array buffers) | 119 of 129 |
| ms | 150 | 127 | 127 | 22 of 72 |
| gs | 20 | 0 (no Metal stage) | — | — |
| all | 600 | 557 | **557 / 557** | 289 |

"Correct" means for every shader: every reflected table is one of its index.json bindings, at the buffer index the
plan says, with all members at offset 0; descriptor-array buffers at the plan's index; the push block found with
index.json's size; the MSL plan equals reflection. 1,780 tables and 65 descriptor arrays checked. 85 shaders declare
bindings Metal does not reflect; for all 85 the SPIR-V never uses those variables through a pointer operand (load,
store, access chain, image texel pointer, atomics, copy, call), i.e. they are dead in the shader, so the backend may
leave them unbound. The 23 ms without a pipeline are mesh-limit failures (20 step 3 of the mesh table, 3 step 2;
section 3). Run times: 520 s compile (4 threads), 2,073 s reflection of 580 rows (one probe process).

Not covered: argument-buffer contents (what the backend writes into each table) and root-signature semantics; only
the layout is checked.

## 2. MSL 3.1 and vertex shaders with subgroup operations

**MSL 3.1:** the recipe is `--msl-version 30100` with `-std=metal3.1` for every shader (one std, deployment floor
macOS 14). That removes the storage-image-atomics bucket (spike/s5.2: 1,037 shaders); in the batch, 0 shaders fail
with that message (table in section 7).

**Vertex shaders that use subgroup operations (spike/s5.2: 1,252).** SPIRV-Cross refuses the subgroup lane builtin in
a vertex function. Two routes were built and run on data:

- *lane* (chosen): SPIR-V pre-pass `lower_vertex_lane` replaces the `SubgroupLocalInvocationId` input by a Private
  variable set at entry to `OpGroupNonUniformIAdd ExclusiveScan 1` (MSL `simd_prefix_exclusive_sum(1u)`) and
  `SubgroupSize` by 32. SPIRV-Cross then accepts the shader as an ordinary vertex function. This is what a "small
  SPIRV-Cross patch" would produce (a vertex function using simd ops), without patching SPIRV-Cross.
- *kernel*: `--msl-vertex-for-tessellation` (the vertex shader becomes a compute kernel writing `main0_out` to a
  buffer, then a pass-through draw).

Tests, all on this GPU:

1. `tests/vs_lane_identity_test.mm` (ctest): is `simd_prefix_exclusive_sum(1)` the hardware lane index in a vertex
   function? It is iff the active lanes of each vertex SIMD group are 0..n-1. 12 draw shapes (triangles 3,000 /
   3,001 / 37, rasterization off, strip 4,099, points 5,000, lines 998, instanced 96x70 and 33x9, indexed shuffled,
   indexed with reuse 6,000 of 1,000, sparse indices): **12/12 pass**; every SIMD group's ballot is contiguous from lane
   0, `simd_shuffle(vid, lane) == vid` for every vertex. Measured on the way: vertex SIMD groups are often partial
   even in the middle of a draw (16..32 active lanes; 1..32 with instancing), and indexed draws with repeated indices
   ran one invocation per index (6,000 for 6,000 indices of 1,000 vertices), i.e. no post-transform reuse here.
2. `tests/vs_lane_test.mm` over every corpus vertex shader that uses the lane id and needs no descriptor tables:
   **640** of the 1,252 (`vs_lane_gen.py`; the other 612 need table setup the harness does not have). Each shader
   runs as the lane vertex function and as the kernel, on 3,000 vertices with hashed attributes and a 1.1 GiB
   patterned heap behind the push-block pointers; outputs are compared per `main0_out` member by name (the two
   routes lay the struct out differently: the kernel route omits SPIRV-Cross's `user(clipN)`/`user(cullN)` copies,
   which it leaves unwritten). Controls: lane route twice (determinism), index-shuffled draw, kernel at threadgroup
   64 and 32, and a mutant lane route with `lane -> 31 - lane`.

   | result (640 shaders) | count |
   |---|---:|
   | outputs identical, lane vs kernel | **635** |
   | differ; output depends on SIMD-group composition (the reversed-lane mutant also changes it) | 3 (`VS_OctaFire_e6901d4b`, `VS_OctaVsplitFire_e6901d4b`: 2 words on 1,488 of 3,000 vertices; `VS_FillingMeshToBoatHullAtlas_f26af053`: position on 1,395) |
   | lane route: GPU hang (`kIOGPUCommandBufferCallbackErrorHang`) | 2 in the final run; 3 distinct over 5 full runs (`VS_Transform_PositionOnly_4e3f4a0e`, `_f7bf2df9`, `VS_TransformEntityMask_99490227`) |
   | kernel route: GPU hang | 0 (in all runs) |
   | lane nondeterministic / kernel threadgroup-size dependent | 0 / 0 |

   For the 3 composition-dependent shaders the reversed-lane mutant changes the output too (`FillingMeshToBoatHullAtlas`
   multiplies the lane id into a buffer address; the two Octa shaders were not traced further). No route reproduces the title's 64-lane AMD waves: the kernel groups 32
   consecutive vertices, the vertex function 16..32 hardware-chosen ones. Both are valid groupings; neither is
   bit-exact to the console for these 3.

   The hangs are not caused by the lowering: with the lane id replaced by the constant 0 (no simd op left from the
   pass) the same two shaders still hang 3/3 in the vertex stage; with the lowering they hang 7/8 and 8/8 when run
   alone. Both contain a translator loop capped at 65,536 iterations that the synthetic heap data drives to the cap
   (one vertex takes about 20 ms in either route). The kernel route ran the same work without a hang. Whether real
   title data reaches that loop count is not known.

   GPU time over 262,146-vertex draws, sum over the 636 shaders that ran: lane 2,316 ms, kernel 3,500 ms
   including the pass-through draw (kernel alone is close to lane; the second pass costs the rest).

**Decision:** the lane pre-pass (vertex function). It gave identical outputs to the kernel route on 635 of 640, the
3 differences are grouping-dependent by construction, it needs no vertex-pulling design, no SPIRV-Cross patch and no
extra pass, and it is 34% cheaper on the large draws. Open: 3 shaders hang the vertex stage on synthetic data
(independent of the lowering); `vs_route=kernel` stays available per shader in the recipe if real data hangs them
too. The SPIRV-Cross patch was not built: it would emit the same vertex function.

## 3. Mesh output limits

> Round 3 note: the "not implemented" statements below about step 3 and the 976/926 unpaired shaders are superseded by
> R3.2-R3.3 (the split is implemented; all 1,135 shaders that need it built pipelines).

Metal rule (spike/s5.2, checked there on 400 real pipelines): mesh size = declared vertices x per-vertex bytes must
be at most 32,768, and at most 124 unique scalars. All 9,282 mesh shaders declare 128 vertices / 128 primitives.

Live outputs come from pairing (`tools/pipelines.py` over the title's pipeline library; `tools/mesh_fit.py`): for a
mesh shader, the union of the input locations (and the highest component read) of every pixel shader any library
record pairs it with; a record with no pixel shader means only position is live. 6,432 pixel shaders were analysed.
3,655 of the 4,631 over-limit mesh shaders appear in records; 976 appear in none, so nothing offline says what reads
them.

Steps, each applied only to what is still over a limit. "Model" is `mesh_fit.py`'s prediction (its scalar count
equals spike/s5.2's for all 9,282); "measured" is a real Metal mesh pipeline from the recipe's own metallib
(`tools/mesh_pso.py`).

| step | model: rescued | measured with real pipelines | remaining over a limit (model) |
|---|---:|---|---:|
| 0 as translated | 4,651 fit | 300/300 of a seeded sample build | 4,631 |
| 1 dead-output elimination (`drop_outputs`) | 3,408 | **2,811 of 2,811 measured build** (597 not reached before the run was stopped) | 1,223 |
| 2 component trim + struct sort (`trim_outputs`, `pack_mesh_struct`) | 122 | 85 measured: **36 build, 49 fail** on size (33,280-38,400 B: the model under-counts float2/float members) | 1,101 (model); about 1,170 if the measured step-2 rate (36 of 85) holds |
| 3 meshlet split to 64 vertices / 64 primitives | 1,051 | shape check on a seeded sample of 200 of steps 3+4: **191/191 step-3 build** with the declaration at 64/64 (body not split; see below) | 50 |
| 4 left for mesh-as-compute | 50 (all > 124 unique scalars even after steps 1-2) | 9/9 in the sample fail at 64/64 ("unique scalar count 125/126 exceeds 124") | — |

Also measured: all 3,147 mesh pipelines that built in this pass have the split binding layout correct by reflection.

What each step is, and the limits:
- Steps 1-2 are SPIR-V passes in the recipe; the batch applies them from `mesh/mesh_fit.jsonl`. A variant built for
  live set L is valid only with pixel shaders whose inputs are within L; the library pairs are known offline, a pipeline
  created at run time with another pixel shader needs a new variant (`patches/mesh_outputs.md`). For the 976
  unpaired shaders this can only happen at pipeline creation.
- Step 3 is not implemented: the check only shows Metal accepts the 64/64 shape for these outputs. A correct split
  needs the translator to emit two 64-vertex halves (design in `patches/mesh_outputs.md`). 926 of the 1,051 step-3
  shaders are unpaired: for them dead-output elimination at pipeline creation may still help first.
- The manifest column `mesh_route` marks every mesh shader `hw` (builds as is or after steps 1-2),
  `split64` (needs the split; until it exists: mesh-as-compute) or `mesh_as_compute`; step-2 shaders whose pipeline
  failed are marked `split64`.


## 4. Geometry shaders: census (`tools/gs_census.py`, `port/build/shaderc_msl/gs/gs_census.json`)

Question: is any of the 195 `GS_VoxelizeGeneric` shaders used by a pipeline the title creates (a fork says unused)?

| evidence | result |
|---|---|
| title pipeline library (`shaders.rpf` zlib part, own reader `tools/pipelines.py`): 25,471 graphics + 2,693 compute records, programs resolved to carved containers by HASH digest (21,509 of 21,522 resolve) | **0 of 195** GS referenced. Stage slots used: ps 24,111, vs 13,409, ms 12,062, slot 6: 2. |
| the family around them | `VS_VoxelizeGeneric` (195, the two-payload VS built as export stage for the GS) 0 in records; `PS_VoxelizeVisiblity` (301) 0 in records; `EntityCoarseVoxelization` CS: 1 of 1 in records |
| title runs (`port/run/logs`, 5 logs with `pipeline-compile-begin`, 2,314 lines) | 430 distinct shaders compiled at run time (424 library cs, 3 vs, 3 ps; 2 of them exe-embedded); 0 GS |
| runtime PSO refusals in the vk-r3/r4/r5 runs (about 59,340 graphics PSO creations each, 54,433 created) | refusal reasons logged: mesh shader (2,930 + 628), depth bounds (672 + 9), rectangle list needs `geometryShader` (400 + 393, the console RECTLIST topology, not a title GS). `port/d3d12_vulkan` refuses a PSO with a GS stage on a device without `geometryShader` with its own message ("geometry shaders need the Vulkan geometryShader feature"); that message occurs in **0** of the logs. About 900 graphics failures per run carry no refusal reason, so they are not covered. |
| executable strings | 0 hits for `GS_VoxelizeGeneric`, `VS_VoxelizeGeneric`, `PS_VoxelizeVisiblity` (1 for `Voxeliz`, 1 for `EntityCoarseVoxelization`) |

Open: 2 library records use stage slot 6 (one program, 21469, plus VS 21468 and PS 21467); none of the three has a
carved source and slot 6 is not identified (leonida's shader_library.md says the same). If slot 6 were a geometry
stage, that pipeline's GS is not among the 195.

Conclusion from this evidence: the 195 carved GS are referenced by no library pipeline and by no pipeline the title
compiled or tried to create in the logged runs. That supports the fork's claim for the library and the logged scenes;
it does not prove that no code path creates a GS pipeline from the containers directly. They are built by nothing in
this toolchain (no Metal stage); the batch records them as `cross_fail`.

## 5. Compute shaders at Metal limits

> Round 3 note: the 4 shaders "needing the translator patch" are fixed by a SPIR-V transform (R3.4); the 7 huge kernels'
> pipeline builds were measured again (R3.5).
 (`tools/cs_check.py`, `port/build/shaderc_msl/cs_check/cs_check.jsonl`)

Each built with the batch recipe and options into the shared store, then a compute pipeline built (one at a time,
1,200 s timeout).

**Threadgroup memory (all 28 cs with index.json `lds_bytes` > 32,768).** The translator declares an 8,192-dword LDS
array for every shader plus the wave64 exchange buffer `%x_buf`. SPIR-V pass `shrink_lds` cuts the LDS array to the
program's own allocation (FSSM RSRC2 `LDS_SIZE`, read from the carved DXBC) and redirects accesses at or past it to
two sink words (reads from a word nothing writes, writes and atomics to another), which matches RDNA2 behaviour for
out-of-allocation LDS accesses only up to the read value (Metal does not zero threadgroup memory, so a read past the
allocation returns an unspecified value instead of 0; a correct program never does this). It is applied when
allocation + `%x_buf` fits 32 KiB.

| | count |
|---|---:|
| over 32,768 B as translated (spike/s5.2: all fail at PSO) | 28 |
| `shrink_lds` applied, pipeline built | **24** (static threadgroup memory 17,472-31,376 B after the pass) |
| left: allocation is the full 32 KiB and 1,024 threads, so `%x_buf` (8,604 B) has no room | 4 (`*vfx_triangleClip_CS_CollectTriangles*` x2, `*vfx_proj_decal_hit_detection_CS_ComputeHitDetection*Results` x2; 41,372 B) |

The 4 need the translator change in `patches/x_buf_device_memory.md` (exchange buffer in device memory, binding 29,
runtime contract written there). `--max-shared-memory` cannot help them: it shrinks the LDS array, which here is the
program's real 32 KiB.

**fp64 (5 cs, all `water_shading_CS_TransmissionVolumetrics*`).** MSL post-pass `soft_f64` replaces `double` by
`spvSoftF64` (`tools/soft_f64.metal`: IEEE binary64 add, sub, negate, compare, isnan/isinf, f64->f32 in 64-bit
integer code; these are the only f64 operations the 5 use). **5/5 compile and build a pipeline** (`metal -c`
3.5-25.9 s, pipeline 1.6-18.3 s). `tests/soft_f64_test.mm` (ctest) runs the library on the GPU against the CPU's
IEEE results: 4,194,304 cases including 60,726 NaN results and 268,879 subnormal sums, 0 mismatches in add, sub,
f64->f32 and compares (3.3 ms GPU). Not run: the 5 shaders on data.

**Huge compute shaders (the 7 with no pipeline in 300 s in spike/s5.2).**

| shader (SPIR-V MB) | `metal -c` | peak RSS | pipeline (as translated) | with `noinline_hot_helpers` |
|---|---:|---:|---|---|
| `Artdirectionalshadows_CS_RTShadowTrace32HQ_Tiled` (13.0) | 241.8 s | 1.5 GB | **built, 385.5 s** | pass not applied |
| `Irtdirectionalshadows_CS_RTShadowTrace32TemporalHQ_Tiled` (13.3) | 221.3 s | 2.0 GB | **built, 384.8 s** | pass not applied |
| `Urtdirectionalshadows_CS_RTShadowTrace32TemporalHQ_OneOver2RPP_Tiled` (13.6) | 242.9 s | 2.3 GB | **built, 384.0 s** | pass not applied |
| `Rcascadeshadows_rendering_CS_ShadowRenderTiledCascadesAlphaCBPPPC` (9.1) | 340.7 s | 0.9 GB | **built, 362.9 s** | pass not applied |
| `Hindirect_diffuse_lighting_CS_ScreenProbes_ActivateRays` (15.9) | > 600 s, `-O0` 5.8 s | 3.8 GB | none in 1,200 s (`-O0` library) | `metal -c` 526.8 / 438.6 s, **built, 347.3 / 339.3 s** |
| `Jindirect_diffuse_lighting_CS_ScreenProbes_ActivateProbes` (25.6) | 392.8 s | 5.9 GB | none in 1,200 s | `metal -c` 80.2 / 131.8 s, **built, 469.3 / 485.8 s** |
| `Pindirect_diffuse_lighting_CS_ScreenProbes_ActivateProbesTarget` (25.4) | 385.7 s | 3.4 GB | none in 1,200 s | `metal -c` 80.3 / 62.1 s, **built, 442.2 / 432.5 s** |

(noinline column: first `tools/huge_cs.py`, then the recipe itself through `cs_check.py`; both while other jobs ran,
which is why the same build differs by up to 2x.) So 4 of the 7 just needed more than spike/s5.2's 300 s, and the other 3 build once SPIRV-Cross's forced inlining of
their 4-5 format-decode helpers (3,281 call sites each in J/P, 1,890 in H) is undone. That is the MSL pass
`noinline_hot_helpers` (now in the recipe; condition in `patches/huge_cs.md`). **7/7 build a pipeline**, at
339-486 s each (cold, one at a time, other compile jobs running): these must be built off-thread at load into a
binary archive. `-O0` is harmful here (it moves all optimisation into the device compiler: H's `-O0` library compiled
in 5.8 s and its pipeline did not build in 1,200 s); the batch still has the `-O0` retry after a 600 s timeout, and
in this batch it fired once (H, before the pass existed). Not run: these kernels on data (the
runtime cost of real calls instead of inlined helpers is unknown).


**Translator / runtime proposals:** `patches/x_buf_device_memory.md` (4 LDS shaders), `patches/huge_cs.md` (huge
kernels: outlining, `noinline`, build policy).

## 6. Batch driver and throughput

> Round 3 note: the run described below stopped at 19,852 shaders; it was resumed and finished in round 3 (R3.1). The "at most 4
> processes" driver is now at most 3, the cs mean/sum rows below include superseded rows (review claim 5: the files
> give mean 8.68-8.74 s, sum 15,964-16,067 s, max 445 s for the round 1 compute rows), and the claimed order
> (log shaders first) was only added after the first session (review claim 6): UI shaders were built first, the
> library/exe compute shaders after about 15,000 others.
 (`tools/batch.py`, `port/build/shaderc_msl/runs/main/`)

- Process pool of at most 4 workers (`--jobs`, capped at 4); each worker runs one shader at a time through
  `compile_one` (so at most 4 tool processes). Resumable two ways: the content-addressed store skips any shader whose
  key exists (recomputing keys costs about 0.01-0.03 s per shader), and `--fast-resume` skips names already in the
  run's `results.jsonl`. Killed tool processes (signal) are not cached. Per-shader row: pre-pass, SPIR-V-Cross,
  `metal -c`, `-O0`, `metallib` seconds, peak child RSS, binding plan. Timeouts 300 s (SPIRV-Cross) and 600 s
  (`metal -c`, then one `-O0` retry).
- Corpora: the main 26,503 plus `library/` (2,683 cs: the pipeline-library builds the runtime actually compiles),
  `exe/` (12), `fspx/` (27), `bvh/` (52), named `<dir>/<name>` as the runtime logs name them.
- Order: (1) the 430 shaders named by `pipeline-compile-begin` in the title runs' logs, in first-seen order (the
  only pipeline-creation order available: 424 library cs from the warm-up, then UI/exe draws); (2) UI (Scaleform
  `V*`/`F*`, 1,328), blit (99), movie (Bink, 15); (3) pipeline-library records in record order (graphics vs/ms/ps,
  then compute); (4) the rest.
- Outputs: `runs/main/manifest.tsv` (v2: name, key, stage, entry, metallib, push buffer, tables binding:buffer,
  vertex-as-kernel, passes, descriptor arrays binding:buffer, mesh route), `manifest.json`, `summary.json`;
  `src/catalog.cpp` reads the TSV (`tests/catalog_test.mm` loads all 19,852 rows, 589 of them `#`-prefixed names,
  and builds 6 first-scene UI/blit/Bink pipelines from it: 6/6, 0 plan mismatches; see Review response round 2).

**Run (this session plus the earlier one of this module), stopped at 13:58:** 19,852 shaders built, **0 failures**
(the two earlier failures, a vertex shader writing cull distances and one `metal` killed by a signal, were fixed
and rebuilt).

| group | built / total | job seconds | mean | median | p95 | max |
|---|---:|---:|---:|---:|---:|---:|
| main ps | 8,993 / 12,101 | 9,700 | 1.08 | 0.46 | 4.33 | 17.1 |
| main ms | 7,156 / 9,282 | 8,172 | 1.14 | 0.55 | 5.16 | 19.3 |
| main vs | 1,438 / 2,236 | 428 | 0.30 | 0.09 | 1.02 | 3.7 |
| main cs | 1,839 / 2,689 | 16,727 | 9.10 | 2.41 | 28.97 | 617 |
| main gs | 0 / 195 (no Metal stage) | | | | | |
| library cs | 424 / 2,683 (all runtime-log ones) | 77 | 0.18 | 0.21 | 0.28 | 0.36 |
| exe | 2 / 12 | | | | | |

`metal -c` is 90-95% of the tool time in every group (ps: 1.34 s of 1.50 s wall per shader; cs 7.09 of 7.71); the
Python passes take 0.005-0.025 s per shader. Peak `metal` RSS: median 82 MB, p99 930 MB, max 5.9 GB (a 25 MB
ScreenProbes kernel), so 4 parallel jobs fit in 24 GiB even with two huge kernels at once. MSL written for the built
set: 12.8 GB (not kept; only failed builds keep a `.metal.gz`); metallibs: 12,184 files, 3.11 GB.

Measured throughput:
- 4 jobs, ps/ms/vs mix (04:49-05:36): **10,435 shaders/h**, 3.94 job-seconds per wall second (no measurable
  scaling loss at 4).
- cs-heavy, 2 jobs (12:01-13:08): 837 shaders/h.
- Whole run, all phases: 12,352 compiled in 12,917 s of active wall time = 3,443/h (job count 4, 3, 2, 1 at
  different times, while GPU tests and other workers ran; one 5.5 h stall of the earlier session's process is
  excluded).

Projection for everything (measured job seconds of built rows + unbuilt rows x their SPIR-V bytes x the stage's
measured seconds per MiB: cs 5.9, ms 3.5, ps 2.5, vs 1.9): **70,500 job-seconds = 4.9 h at 4 jobs** (ideal
scaling, which held at 4 jobs on the measured mix); main corpus alone 48,300 job-s = 3.4 h; library 22,700 job-s
= 1.6 h. The huge kernels add their own 80-530 s each (included). Device pipeline builds are separate and not
projected here (spike/s5.2 estimated about 12.6 h cold; mesh pipelines in this session took about 2 s each with other jobs running).

Not done for throughput: no change gave a measured speed-up of `metal -c` itself; ordering by size (longest first)
inside the "rest" bucket would shorten the tail at the end of a full run, not measured.

## 7. Counts per stage after each fix (main corpus, 26,503)

> Round 3 note: the last two rows' "about 1,170" and the bucket table are superseded; exact final counts are in R3.1 and the
> route table of R3.3. After round 3 the only unbuilt-by-design shaders are the 195 geometry shaders; every mesh shader
> has a pipeline route (hw / split64 / mesh_as_compute), the 28 threadgroup-memory compute shaders are fixed, fp64 and
> the huge kernels build.


Known-failing shaders per stage. Row 1 is spike/s5.2's measurement (26,296 + the 207-shader supplement, which added
1 atomics case); each later row applies one fix, with the evidence named. Stage sizes: vs 2,236, ps 12,101,
cs 2,689, ms 9,282, gs 195.

| after | vs | ps | cs | ms | gs | total | evidence |
|---|---:|---:|---:|---:|---:|---:|---|
| S0.4 recipe unchanged | 1,252 | 1,018 | 60 | 4,631 | 195 | **7,156** | spike/s5.2 |
| + `split_bindings` | 1,252 | 1,018 | 60 | 4,631 | 195 | 7,156 | no change in counts; layout now correct (557/557 reflected, section 1) |
| + MSL 3.1 | 1,252 | 0 | 40 | 4,631 | 195 | 6,118 | 0 atomics failures in 19,852 built |
| + `lower_vertex_lane` | 0 | 0 | 40 | 4,631 | 195 | 4,866 | all 1,438 vs built compile; 640 run on data (section 2) |
| + mesh step 1 (dead outputs) | 0 | 0 | 40 | 1,223 (model); measured: 2,811 rescued, 597 of the model's 3,408 not run | 195 | 1,458 | section 3 |
| + mesh step 2 (trim) | 0 | 0 | 40 | about 1,170 (36 of 85 measured rescued) | 195 | about 1,405 | section 3 |
| + `shrink_lds` | 0 | 0 | 16 | about 1,170 | 195 | about 1,381 | 24/24 pipelines |
| + `soft_f64` | 0 | 0 | 11 | about 1,170 | 195 | about 1,376 | 5/5 pipelines, library tested on GPU |
| + `noinline_hot_helpers` and no 300 s limit | 0 | 0 | **4** | about 1,170 | 195 | **about 1,369 (5.2%)** | 7/7 pipelines (section 5) |
| + round 3: meshlet split (SPIR-V) and mesh-as-compute | 0 | 0 | 4 | **0** (1,135 split64, 50 mesh_as_compute, 8,097 hw) | 195 | 199 | R3.2-R3.3 |
| + round 3: `x_buf_device` | 0 | 0 | **0** | 0 | 195 | **195** (the geometry shaders only) | R3.4 |

What remains failing, bucketed:

| bucket | count | what it needs | where |
|---|---:|---|---|
| mesh over the size limit at 128 vertices, fits at 64/64 | about 1,120 (1,051 model step 3 + about 70 step-2 measured failures) | meshlet split in the translator, or dead-output elimination at pipeline creation for the 926 unpaired; until then mesh-as-compute | `patches/mesh_outputs.md`; manifest `mesh_route=split64` |
| mesh over 124 unique scalars even after steps 1-2 | 50 | mesh-as-compute (or fp16 varyings) | manifest `mesh_route=mesh_as_compute` |
| geometry shaders | 195 | no Metal stage; no pipeline the title creates uses them in the evidence of section 4 | section 4 |
| cs: LDS allocation 32 KiB + wave64 exchange buffer | 4 | translator: exchange buffer in device memory | `patches/x_buf_device_memory.md` |

Known open items that are not failures: 3 vertex shaders that hang the vertex stage on synthetic data (section 2);
all 7 huge kernels need 6-8 minutes of cold pipeline build each; 2,236 vs / 12,101 ps paired with real vertex
inputs and formats were not built as full pipelines here (the ps pipelines used a stub vertex function).

## Not run / not done (round 2 list; round 3 changes marked)

- Round 3: the batch finished; the meshlet split, the 4 LDS cases and mesh-as-compute are implemented; the GPU comparison of the
  split has the data limit stated in R3.2; the mesh-as-compute vertex shader and the x_buf compute shaders were not run on
  data; the three-shader hang cause is established (R3.6) but real draws' loop limits are unknown; the reviewer's hung
  probe is not identified (R3.5).

- No translated shader was checked for correct output against the console, except the vertex-route comparison and
  the soft-f64 library; nothing here ran a pixel, mesh or compute shader of the corpus on data.
- The batch did not finish the corpus: 7,077 of the main 26,503 (and 2,259 library, 79 fspx/bvh, 10 exe) are
  unbuilt; `batch.py --fast-resume` continues it (projected 4.9 h at 4 jobs for all of it).
- `mesh_pso.py` was stopped during its step 1/2 pass (597 step-1 and 37 step-2 shaders not measured; split check
  done on a 200-shader sample only).
- The meshlet split itself, the 4 LDS cases and GS emulation are not implemented here (design/patch files only).
- The 612 subgroup vertex shaders that need descriptor tables were compiled but not run on data.
- x86_64: module and tests built; no x86_64 binary was run.

## Review response (round 2)

Both blocking findings of `REVIEW.md` (round 1) are fixed, each with a test that asserts the measured result. Run
2026-11-26 14:45-14:49 on the arm64 build `port/build/shaderc_msl-arm64` (load average 5.6-11, other agents active).

**1. `Catalog::load` dropped 589 shaders.** Cause confirmed: every line starting with `#` was taken as a comment;
589 manifest names start with `#` (209 ms, 338 ps, 42 vs). Fix (`src/catalog.cpp`): a line is a comment only if it
starts with `#` and contains no tab. The two header lines `batch.py` writes contain no tab; every data line has at
least 8 tabs (11 columns), and `batch.py` already skips names containing a tab or newline, so the rule is
unambiguous. No manifest or batch change was needed (keys and metallibs unchanged).

`tests/catalog_test.mm` now asserts:
- a synthetic manifest (2 header lines, rows named `#FHash_1`, `# space`, `PlainName`) loads 3 of 3 rows with the
  right fields and root;
- the loaded count equals the manifest's data lines counted independently (lines containing a tab), and every row
  resolves by name to an entry with the same key and stage, including every `#`-prefixed row;
- 2 more pipelines built from `#`-named shaders: `%VBatchTexTGCxformAc_d17d2c71` + `#FSColor2FullBevel_d17d2c71`
  (the Scaleform pixel shader the review names) and `#VBatchTexTGCxform_eca9e4f0` + `#FTexTGCxformAcMul_eca9e4f0`
  (a `#` vertex shader), with the bindings checked by reflection like the other 4.

Measured (under API + shader validation): 19,852 data lines, 19,852 loaded, 19,852 resolve; `#` names 589 of 589;
per stage cs 2,263, ms 7,156, ps 8,994, vs 1,439 rows, all resolve (cs and ps include the library/exe rows);
pairs 6/6 built, 0 reflection mismatches; test 1.9 s. Negative control: the same test compiled against the old
loader (scratch copy) fails at the synthetic step (1 of 3 rows loaded), exit 1.

**2. The two vertex tests aborted under Metal validation.** Fix: the render target in `tests/vs_lane_identity_test.mm`
and `tests/vs_lane_test.mm` is now created with `usage = MTLTextureUsageRenderTarget` (and private storage; it is
never read back). The earlier reported results were measured without validation; that was not stated before.

Assertions added: `vs_lane_identity` passes only if all 12 cases ran and passed (was: no failing case);
`vs_lane_test --expect-equal=N` (ctest passes N = 40, the `vs_lane_gen.py --limit 40` input) passes only if N shaders
ran, N are lane == kernel, and there are 0 GPU errors, 0 compile/pipeline failures, 0 differing and 0 unexplained.
Both tests print whether validation was on. `CMakeLists.txt` sets `MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1` on all
four ctest tests (soft_f64, vs_lane, vs_lane_identity, catalog). The full 640-shader run (`tools/vs_lane_run.py`)
does not pass `--expect-equal` and behaves as before.

| test (ctest, validation on) | result | wall | peak RSS |
|---|---|---:|---:|
| `shaderc_msl_vs_lane_identity` | 12 of 12 cases pass | 0.34 s | |
| `shaderc_msl_vs_lane` | 40 of 40 lane == kernel, 0 GPU errors, 0 compile failures (1 differs only in the unwritten clip/cull copies, masked as before) | 186 s | 3.43 GB |
| `shaderc_msl_soft_f64` | 4,194,304 cases, 0 mismatches | 0.31 s | |
| `shaderc_msl_catalog` | as above | 1.9 s | |

Without validation the same `vs_lane` run takes 47 s wall, 1.86 GB peak RSS, same 40/40 result; shader validation
multiplies its GPU time about 5x (lane 454 vs 86 ms over the large draws), so the validated GPU timings are not
performance numbers; the performance figures in section 2 are from unvalidated runs. Not rerun under validation:
the full 640-shader `vs_lane_run.py` comparison (and therefore the 3 hanging shaders).

Builds: arm64 and x86_64 trees rebuilt clean (x86_64 Mach-O confirmed; no x86_64 binary run). Logs:
`port/build/shaderc_msl-ctest-r2-catalog-identity.log`, `port/build/shaderc_msl-ctest-r2-vs_lane-soft_f64.log`,
`port/build/shaderc_msl-vs_lane-novalidation-r2.log`.

The non-blocking points of round 1 (weaker-than-stated claims 1-11) were not reworked in this round; their wording in
the sections above is unchanged.
