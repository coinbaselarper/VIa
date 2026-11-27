# translator-fork: origin

GPL-3.0-or-later, as leonida (`LICENSE` here is leonida's, copied unchanged).

`tools/` holds the 20 modules `rdna2spirv.py` imports (cfg_structure, file_mode, fssm_header, fssm_unpack, lane_masks,
rdna2_decode, rdna2_isa_tables, rdna2spirv, replace_retry, report_paths, shader_ir, shader_meta,
shader_stage_manifest, spirv_asm, spirv_asm_tables, spirv_loop_guard, spirv_opt, spirv_stubs, wave64_emulate_opt,
wave64_halves) and `gfx10_formats.json`, copied from `leonida/tools/` at commit
d3b8251bc390418cdbc1f2d30727b6b31d889cfa, then `spike/s2-make-game/patch/rdna2spirv-computed-ij-f16-fallback.diff`
applied: the result was byte-identical to `spike/s2-make-game/tree/tools/` (the tree that translated the corpus the Metal
path uses). Nothing in `leonida/` or `spike/` was edited.

Without the new flags the fork writes the corpus byte for byte (checked: `+MS_VS_PropFoliageDeferred_7cf79608_Wrapped~8`,
`library/lib_cs_00977`, the top-10 pixel shaders and 7 top compute kernels with the flags on, which do not touch them).

## Added (all off unless the flag is given; flags only act under `--wave64-emulate`)

| flag | where | what |
|---|---|---|
| `--fold-waterfalls` | `tools/waterfall_fold.py` (new), `rdna2spirv.py` (`translate`, `v_readfirstlane_b32`, `scalar_load` exec guard) | a waterfall loop over a per-lane descriptor runs once with each lane's own descriptor words: no readfirstlane exchange, no EXEC vote |
| `--readlane-batch` | `rdna2spirv.py` (`v_readlane_b32`, `x_round`, `x_split`, `insn`) | v_readlane_b32 of a VGPR whose value the current exchange round already published reads that round's copy |
| `--uniform-votes` | `tools/wave_uniform.py` (new), `rdna2spirv.py` (`wave_mask`, `g_vote`, `wave_equal`, `scc_any_half`, `quad_any`, branch votes), `wave64_emulate_opt.py` (`UNIFORM_IDS`) | votes over lane masks whose bit is the same in all 64 lanes, readfirstlane/readlane of a VGPR that is the same in all 64 lanes: the lane's own value (workgroups of whole waves only) |
| `--barrier-tail` | `wave64_emulate_opt.py` (`BARRIER_TAIL`, `plan_structured_emulation`) | multi-wave groups: an s_barrier inside a wave-divergent region starts a tail scheduler instead of refusing the structured plan |
| `--wave-rounds` | `tools/wround_halves.py` (new), hooks in `rdna2spirv.py` (`translate`, `main`, block loop `term_uniform`, `branch_condition`, `emit_folded_region` inner branch) and `wave64_emulate_opt.py` (`lane_local_ok`, own-bit inner vcc branches in folds, `E64_MASK_EXACT`) | read-alone and wave-uniform v_readfirstlane, gated uniform-branch votes, folds with own-lane reads and own-bit inner vcc branches, execz fall-through (round 15 wave-rounds worker; `port/shaderc_msl/STATUS.md`) |
| `--half-shuffle` | `tools/wround_halves.py`, `rdna2spirv.py` (`g_shuffle`, `dpp_insn`, `assemble` emulation check) | DPP, ds_swizzle, ds_bpermute, v_permlane(x)16 as SIMD-group shuffles (whole waves only) |
| `--heap-tindex` | `tools/wround_halves.py` | console-heap T# by slot index before the hash; needs the layer's slot table (off) |
| `--spill-lanes` | `tools/wround_halves.py` (`park_spills`; reuses leonida's parked-slot path in `v_readlane_b32` / `v_writelane_b32`) | SGPR spill slots in VGPR lanes (a VGPR only constant-lane writelane/readlane touch; slots that hold no lane mask) kept as parked SGPRs: no exchange per v_readlane (whole-wave emulated groups) |
| (tool) | `tools/wround_census.py` (new) | census of the exchange points by cause (wraps the translator's `main`) |
| `--image-atomic64` | `tools/speckle_fix.py` (new), `rdna2spirv.py` (`image_misc` atomic branch, `main`) | two-VGPR image_atomic_umax/umin on 2D images: a marked write through the 2D uint storage view that `shaderc_msl/tools/speckle_msl.py` turns into a 64-bit `texture2d<ulong>` atomic (high dword decides); only a found descriptor of format 32_32_UINT (round 15 speckle worker) |
| `--cs-implicit-lod` | `tools/speckle_fix.py`, `rdna2spirv.py` (image-sample lowering, `assemble` emulation check) | compute image_sample / _b / _c / _c_b: gradients from the console quad (lane 1 - lane 0, lane 2 - lane 0, scaled by 2^bias) instead of LOD 0 |
| `--mesh-index-clamp` | `tools/speckle_fix.py`, `rdna2spirv.py` (hw mesh epilogue list) | primitive indices clamped below the vertex count; no vertices, no primitives |

Index entries of translations with `--fold-waterfalls` carry `waterfalls_folded`.
