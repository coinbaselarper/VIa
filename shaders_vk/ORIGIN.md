# Origin of the files in shaders_vk/

GPL-3.0-or-later, as leonida.

## translator/ (copied, then modified)

`translator/make_game.py` and `translator/tools/` are a copy of `spike/s2-make-game/tree/` (without `__pycache__`,
`game/` and `runtime/`), which is itself a byte copy of `leonida/make_game.py` and `leonida/tools/` at commit
d3b8251bc390418cdbc1f2d30727b6b31d889cfa plus `spike/s2-make-game/patch/rdna2spirv-computed-ij-f16-fallback.diff`
(computed I/J under `--f16-interp-fallback`). Nothing in `leonida/` or `spike/` was edited.

Changed here:

| file | change |
|---|---|
| `translator/tools/rdna2spirv.py` | `--apple-float-controls` (only `SignedZeroInfNanPreserve 32`; no `DenormPreserve 16`, no `SignedZeroInfNanPreserve 64`) and `--vs-lane-from-ballot` (calls `vs_lane_ballot.rewrite` from `finish_module` for vertex shaders). 27 diff lines against the spike tree. |
| `translator/tools/vs_lane_ballot.py` | new: the vertex-stage lane rewrite. |

The translator digest recorded in every corpus manifest (`shaders_vk.json`, sha256 over `translator/tools/*.py`) is
39d82ddca437a1786d21b81e3d38e5d4e8cfb78e07b1b9958161ba0235171578.

## New

| file | what |
|---|---|
| `tools/build_corpus.py` | runs the translator copy (3 shards) over make_game's disassembly, a whole part or a name list |
| `tools/strip_float_controls.py` | the preamble edit `--apple-float-controls` makes, applied to an existing module; `--verify` compares |
| `tools/assemble_corpus.py` | the complete corpus from make_game's one plus every re-translation, comparing the two where both exist |
| `tools/corpus_provenance.py` | each shipped module's source read off its hard links, checked against the manifest's counts (round 3) |
| `tools/f16_census.py` | per stage, the modules declaring Float16/DenormPreserve and those using each f16 conversion instruction |
| `tests/f16_denorm_probe.mm`, `tests/nan_signed_zero_probe.mm` | f16 subnormals and NaN/inf/-0 through Metal, safe and fast math |
| `tests/vs_lane_probe.mm` | the lane `--vs-lane-from-ballot` computes, checked against `simd_shuffle` in a Metal vertex function |
| `tests/fc_probe.comp`, `tests/fc_vulkan_probe.cpp` | the same questions through a Vulkan driver (MoltenVK, KosmicKrisp) with the three float-controls variants |
