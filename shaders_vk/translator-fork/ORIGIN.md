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

Index entries of translations with `--fold-waterfalls` carry `waterfalls_folded`.
