# shaders_vk status (2026-11-26)

The title's translated shaders in a form Apple's Vulkan drivers (MoltenVK 1.4.2, KosmicKrisp 26.3.1) accept: the
`LEONIDA_SHADERS` folder of the Vulkan path. Tooling here, outputs under `port/build/shaders_vk/` (not sources).
Origins: `ORIGIN.md` (the translator is a copy of `spike/s2-make-game/tree` + patch, GPL-3.0).

## Result

`port/build/shaders_vk/apple/spirv` holds all **29,277** modules (main 26,503, exe 12, fspx 27, bvh 52, library
2,683), as this translator copy writes them with make_game's flags plus two:

```
--wave64-emulate --f16-interp-fallback --apple-float-controls --vs-lane-from-ballot
```

- `--apple-float-controls`: declares only `SignedZeroInfNanPreserve 32`, the one float-controls mode both drivers
  report. Dropped: `DenormPreserve 16` (every module, no Apple driver reports `shaderDenormPreserveFloat16`) and
  `SignedZeroInfNanPreserve 64` (5 compute modules that use `double`; no Apple driver has `shaderFloat64`, so they
  cannot run anyway). This is `--no-float-controls` minus one mode, chosen on measurement (below).
- `--vs-lane-from-ballot`: vertex shaders compute their subgroup lane from the entry ballot and an exclusive scan
  instead of reading `SubgroupLocalInvocationId`, which a Metal vertex function cannot declare
  (`translator/tools/vs_lane_ballot.py`).

`apple/spirv/shaders_vk.json` records flags, translator digest (39d82ddc…) and per-part counts; `native_hashes.txt` and
`wave64_emulate.txt` are copied and `apple/dxbc/` holds hard links to make_game's containers (the runtime's slot-57
native PSO path reads `../dxbc/*.dxbc` beside the corpus; the earlier overlay corpus lacked it, so those PSOs failed).

### Where each module came from (`shaders_vk.json`, `assemble.log`; checked by `tools/corpus_provenance.py`)

| part | modules | re-translated with the Apple flags | stripped from make_game's corpus |
|---|---:|---:|---:|
| main VS | 2,236 | 2,236 (1,432 twice: byte-identical both times) | 0 |
| main PS/CS/MS/GS | 24,267 | 15,715 | 8,552 |
| exe, fspx, bvh, library | 2,774 | 2,774 | 0 |
| **total** | **29,277** | **20,725** | **8,552** |

"Stripped" is `tools/strip_float_controls.py`: the same preamble edit `--apple-float-controls` makes (drop
`OpCapability DenormPreserve`, `OpExecutionMode … DenormPreserve 16`, `… SignedZeroInfNanPreserve 64`; no id changes).
Where a non-vertex module exists in both forms the two were compared byte for byte, and the index entries compared
equal: 18,488 by `assemble_corpus.py` (all identical; 15,715 main + 2,773 in the sub-parts), and 2,017 more from the
last, stopped re-translation (`retrans-rest/.stage/main/spv`), **which were compared but not merged**: of them 2,012
are stripped modules in the shipped corpus (byte-identical) and 5 are already re-translations there. Merging them would
change no byte, only the count above (17,727 / 6,540). Vertex shaders are never stripped (the lane rewrite changes
their code). The earlier text here said 22,742 re-translated and 6,535 stripped; that counted the 2,017 as merged and
was wrong (review round 3, B2).

`tools/corpus_provenance.py CORPUS [--extra DIR] [--verify-stripped N]` reads the provenance off the files:
`assemble_corpus.py` hard-links a module it takes from a re-translation and writes a stripped one as a new file, so a
shipped module is a re-translation exactly when it shares its inode with a `.spv` under one of the manifest's
`--retranslated` dirs. On `apple/spirv` (`build/shaders_vk/provenance-r3.txt`): per part and in total it matches the
manifest (20,725 / 8,552), 2,012 stripped modules have an identical unmerged re-translation (`--extra`), and 300
random stripped modules re-stripped from make_game's corpus are identical to the shipped files (0 differ). The
manifest records counts only, not per-module sources (the docstring of `assemble_corpus.py` claimed otherwise; fixed).

Order of work, as the task asked (first scenes first): `boot_built_order.txt` lists the 430 distinct modules the
title built into Vulkan pipelines in the first 60–110 s of five runs (pipeline-compile trace lines of
`vk-b0-baseline` … `vk-r5-cachetest`): 424 library compute shaders, the Scaleform UI pair
`%VVertexCxformEAlpha_dc4780ad`/`_VBatchVertexCxformEAlpha_bc8bd7de` + `%FVertexCxformEAlpha_*`, and
`exe/container_00` (VS) + `exe/container_06` (PS). All 430 were re-translated first. PSO *creation* order (the
`--trace-d3d12` run `vk-b0-baseline`) does not narrow it further: the title creates ~55,700 graphics and 2,703
compute PSOs in its first ~20 s, and each resolves its modules in the index.

Cost (3 shards, native arm64, Homebrew python 3.14): 2,236 VS in 84 s; library 424 in 7 s + 2,259 in 1,189 s; the
main part ran 6 h for 17,129 modules before that session ended (64 % of it, MS dominate); assembly 221 s. The
remaining 6,301 main PS/CS: 2,017 done in ~50 min of shard time, then stopped (its own ETA was ~2 h more).

## Float controls: what is lost, measured

Probes: `tests/fc_vulkan_probe.cpp` + `tests/fc_probe.comp` run one compute module through each driver in three
variants: **none** (`--no-float-controls`), **apple** (`--apple-float-controls`), **full** (unmodified translator:
`SignedZeroInfNanPreserve 32` + `DenormPreserve 16`). Native arm64, M4 Pro, results in
`build/shaders_vk/tests/fc-{moltenvk,kosmickrisp}-arm64.txt`.

**f16 denormals.** Over all 2,046 f16 subnormals, 21 operations: f16 arithmetic (`+ - * / fma`, min/max, abs, neg,
clamp, mix, dot, sqrt, exp2 into the subnormal range, compare), conversions both ways, and the three conversion
instructions the translator emits (`UnpackHalf2x16`, `PackHalf2x16`, `OpQuantizeToF16`):

| driver | none | apple | full |
|---|---|---|---|
| MoltenVK 1.4.2 | 0 wrong except QuantizeToF16 | same | same |
| KosmicKrisp 26.3.1 | 0 wrong except QuantizeToF16 | same | same |

`OpQuantizeToF16` flushes every subnormal to zero in all three variants on both drivers (the SPIR-V spec allows it;
the translator's `f16_roundtrip` intends exactly that flush). Every other result is exact. `DenormPreserve 16` changes
nothing on either driver. The Metal-level probe (`tests/f16_denorm_probe.mm`) gives the same for safe and fast math.

Where f16 denormals can reach the corpus (`tools/f16_census.py`, `build/shaders_vk/f16_census.txt`): **no module
declares `Float16`** (0 of 29,277): the translator does RDNA f16 VALU math in f32 and widens a zero-exponent half
with integer operations (`f16_in_bits`), so no f16-typed instruction exists for the mode to govern. f16 bits become
f32 only through `UnpackHalf2x16` (25,812 modules: 10,746 PS, 2,236 VS, 8,862 MS, 1,904 + 2,040 + 9 + 3 CS, 11 + 1
exe): half-float data read from memory (d16 image and buffer loads, packed 16-bit formats, f16 constants). That
instruction is outside what `DenormPreserve 16` governs and measured exact above. `OpQuantizeToF16` and
`PackHalf2x16` appear in 0 modules; `OpFConvert` only in the 5 `double` compute modules.

So: **correctness lost by dropping `DenormPreserve 16`: none measured on these two drivers.** What is lost is the
driver's guarantee; a driver that flushed f16 subnormals in `UnpackHalf2x16` would turn half-float inputs below
6.1e-5 in magnitude into 0 (small HDR colour values, unit-vector components near zero, small velocity or depth
deltas stored as f16). Neither driver does.

**NaN, infinity, -0 (why not `--no-float-controls`).** 16 f32 checks (isnan, NaN compares, 0/0, inf-inf, inf*0,
signed zeros):

| driver | none | apple | full |
|---|---|---|---|
| MoltenVK 1.4.2 | **5 wrong**: `NaN != NaN` false, ordered compare of NaN true, `0/0` = 1.0, `NaN == NaN` true, `inf - inf` = 0 | 0 wrong | 0 wrong |
| KosmicKrisp 26.3.1 | 2 wrong (`0/0`, `inf - inf`) | 1 wrong (`inf - inf` = 0, a KosmicKrisp folding bug) | 1 wrong |

MoltenVK compiles a module with fast math unless it declares `SignedZeroInfNanPreserve`; `--no-float-controls` would
change NaN semantics in every shader on the working driver. `--apple-float-controls` keeps them. Not measured: the
speed cost of fast math off (S0.4 measured MoltenVK slower on precise float math).

## Vertex shaders on MoltenVK (the subgroup refusal)

`vk_pipeline_check` (spike/s2-make-game/build-runtime, arm64, Homebrew MoltenVK 1.4.2, `--stage graphics --jobs 3`)
on VS-only views of the two corpora (`build/shaders_vk/tests/vscheck-*`):

| corpus | VS pipelines ok | failed | note |
|---|---:|---:|---|
| make_game's (index float controls stripped so the checker does not skip) | 984 | 1,252 | every failure uses `SubgroupLocalInvocationId` (1,317 do; 65 build) |
| Apple (`--vs-lane-from-ballot`) | **2,234** | **2** | 861 s wall, 1.15 s mean, slowest 45.7 s |

The 2 left fail in Metal compilation: `$VS_Trail_Heightmap_84f10edd` and `,VS_Decal_Dynamic_Heightmap_9ba80689`
write `CullDistance`, which SPIRV-Cross removes from the MSL output struct but still assigns
(`no member named 'gl_CullDistance' in 'main0_out'`, `tests/vscheck-fail2.log`). A MoltenVK/SPIRV-Cross defect, not
the rewrite; not fixed.

The lane rewrite was checked on the GPU (`tests/vs_lane_probe.mm`, Metal vertex function): for 1 to 65,537 vertices,
including partial simdgroups, the derived lane equals the index `simd_shuffle` reads it at (0 mismatches).

Boot shaders affected (from `vk-b0-baseline`, unmodified corpus, MoltenVK): `exe/container_00`, the executable's one
inline VS (quad/UI draws, `leonida/docs/embedded_shaders.md`), refused 608 times; the Scaleform UI VS were not
affected. After the rewrite it builds (`vk-r1-overlay`). Movie (Bink) shaders: no Bink frame was decoded in any run,
not measured.

## Tools

```sh
P=/opt/homebrew/bin/python3.14        # make_game needs >= 3.10; /usr/bin/python3 is 3.9
W=../spike/s2-make-game/work          # carve/fssm/disasm outputs, read only
$P -I shaders_vk/tools/build_corpus.py --work $W --out build/shaders_vk/retrans-X --parts library,main --names LIST --jobs 3
$P -I shaders_vk/tools/assemble_corpus.py --base $W/shaders/spirv --out build/shaders_vk/apple/spirv \
      --retranslated DIR [--retranslated DIR ...]     # later DIRs win; refuses an existing output
$P -I shaders_vk/tools/strip_float_controls.py --src $W/shaders/spirv --verify RETRANSLATED_DIR
$P -I shaders_vk/tools/f16_census.py build/shaders_vk/apple/spirv build/shaders_vk/apple/spirv/library ...
```

Probe builds: `fc_probe.comp` → `glslangValidator --target-env vulkan1.2 -V`, the three variants by adding the
capability and execution-mode lines to the disassembly (`spirv-dis`/`spirv-as`, all pass `spirv-val`);
`clang++ -std=c++20 -I third_party/Vulkan-Headers/include tests/fc_vulkan_probe.cpp`; run as
`fc_vulkan_probe DRIVER.dylib DIR`.

## Not done / open

- 8,552 main PS/CS/MS/GS are stripped only, not re-translated in the shipped corpus. 2,012 of them have an identical
  re-translation in `build/shaders_vk/retrans-rest/.stage/main/spv` (stopped at 2,017 of 6,301, not merged); the rest
  were never re-translated (no Apple driver has mesh shaders, so the MS among them cannot run either).
  `assemble_corpus.py` takes any later re-translation and re-checks it.
- 2 VS fail on MoltenVK (CullDistance, above). 28 compute modules exceed 32 KiB shared memory and 195 GS, 9,282 MS
  have no stage on either driver; the runtime refuses those PSOs by name.
- KosmicKrisp lacks fragment barycentrics: the 527 PS that read them (S2) cannot run there.
- `build/shaders_vk/overlay` (the earlier boot-only corpus: hard links of make_game's plus 2,248 re-translated) and
  `build/shaders_vk/spirv/.stage/main-all` (the 6 h partial run) are superseded; `apple/spirv` hard-links their files.

## Review response (round 3)

B2 (`../d3d12_vulkan/REVIEW-round3-pixels.md`): the provenance numbers above were wrong; corrected to the manifest
(20,725 re-translated, 8,552 stripped). Shown by `tools/corpus_provenance.py` (new) on the shipped corpus: its
per-part counts, read from the files' hard links, match `shaders_vk.json` (exit 0, "provenance: matches the
manifest"); 2,012 stripped modules have a byte-identical re-translation that was not merged; 300 random stripped
modules re-stripped from make_game's corpus: 0 differ. Output: `build/shaders_vk/provenance-r3.txt`. The corpus was
not changed (another worker was running the title on it). `assemble_corpus.py`'s docstring no longer claims per-module
sources in the manifest.

Nothing else in this module changed in round 3. The slot-57 failures of B1 are not a corpus gap: every slot-57
description resolved through `native_hashes.txt` in all seven round-3 title runs (`../d3d12_vulkan/STATUS.md`).
