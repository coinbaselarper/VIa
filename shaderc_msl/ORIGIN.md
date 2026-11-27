# Origins

Everything in this module was written for gta6rt (GPL-3.0-or-later, see `../LICENSE`). No leonida, VIctory or
`spike/ref-*` code was copied or imported. Round 3 *runs* (does not copy or modify) the translator tools in
`spike/s2-make-game/tree/tools` (`rdna2spirv.py --mesh-compute`, `mesh_compute_vs.py`) to make the unsplit mesh-as-compute
modules; that tree is the project's own translator copy that produced the corpus.

| file | what it is based on |
|---|---|
| `tools/shaderc_msl.py` | the Route A recipe of `spike/s0.4-shaders/routeA_run.py` and `port/metal/tools/route_a_msl.py` (flags, `-O0` retry), extended |
| `tools/spv.py` `split_bindings` | the decoration edit of `port/metal/tools/route_a_msl.py` (`split_sets`), rewritten on a shared SPIR-V reader |
| `tools/pipelines.py` | own reader for the pipeline-library layout as measured and documented in `leonida/docs/shader_library.md` and `leonida/docs/pso_cache.md` (read only; `tools/shader_library.py` was not used) |
| `tools/shaderc_msl.py` `lds_allocation` | FSSM header offset 0x138 (COMPUTE_PGM_RSRC2) and the XBCH layout as documented in `leonida/docs/fssm_crosscheck.md`; LDS_SIZE field as in RDNA2 ISA docs |
| `tools/msl_probe.mm` | modes and stub logic follow `spike/s5.2-msl-corpus/probe/probe2.m`; adds pipeline reflection and a JSON-lines job loop |
| `tools/mesh_scalars_check.py` | scalar counter of `spike/s5.2-msl-corpus/mesh_shapes.py`, reimplemented |
| `tools/soft_f64.metal` | own IEEE-754 binary64 add/sub/compare/convert in 64-bit integer code |
| `tools/cs_check.py`, `tests/vs_lane_identity_test.mm` | own |
| `tools/gs_census.py` runtime-log part, `tools/batch.py` runtime order | read the title runs' logs in `port/run/logs` (`pipeline-compile-begin`, `vk-counters` lines written by `port/d3d12_vulkan`); no code taken |
| `tools/mesh_split.py`, `tools/mesh_split_check.py`, `tools/mesh_split_pso.py`, `tools/mesh_route.py`, `tools/partition_emul.py`, `tools/synthetic_check.py`, `tests/synthetic_mesh/` | own (round 3); the unsplit reference module is made by the translator copy in `spike/s2-make-game/tree` (`rdna2spirv.py --mesh-compute`, run, not modified) |
| `tools/mesh_msc.py` | drives the translator's `--mesh-compute` and `tools/mesh_compute_vs.py` (leonida code under `spike/s2-make-game/tree`, run, not copied) |
| `tools/cs_xbuf.py`, `tools/xbuf_check.py`, `tools/cs_sweep.py`, `tools/vs_hang_run.py`, `tools/final_counts.py`, `tools/baseline.sh` | own (round 3) |
| `tools/mesh_run.mm` | own: job-loop harness for compute / mesh (object + mesh) pipelines on a patterned heap |
| `tools/warmup_list.py`, `tools/warmup/` | own; reads `port/run/logs/r4pso-*.log` (census build of `spike/r4-pso-census`, read only); the builder links `port/metal` (`gta6rt_metal`) by path |

Third-party tools used (not vendored): Homebrew `spirv-cross` 1.4.363.0 (Apache-2.0), `spirv-val`, Apple Metal
toolchain 27B47 (`metal`, `metallib`). The SPIRV-Cross source tarball for the same tag is downloaded to
`port/build/shaderc_msl/third_party/` only to read where the vertex lane-id refusal comes from; it is not built.
