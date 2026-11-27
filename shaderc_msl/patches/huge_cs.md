# Huge compute shaders: what is measured, and the translator change proposed

Applies to the 7 compute shaders that built no pipeline within 300 s in spike/s5.2 (`tools/huge_cs.py` `HUGE`).
Measured numbers are in `../STATUS.md` section 5 (`port/build/shaderc_msl/cs_check/cs_check.jsonl`). The proposal
below is for the translator's owner (`spike/s2-make-game/tree/tools/rdna2spirv.py`, a leonida file); it is not
applied, built or run here.

## Where the size is (measured on the SPIR-V)

| shader | SPIR-V words | entry function words | callee called most (calls x words) |
|---|---:|---:|---|
| `Artdirectionalshadows_CS_RTShadowTrace32HQ_Tiled` | 3,248,694 | 3,067,473 | 204 x 448 |
| `Hindirect_diffuse_lighting_CS_ScreenProbes_ActivateRays` | 3,981,043 | 3,889,876 | 1,890 x 713, 1,890 x 679, 1,890 x 679 |
| `Irtdirectionalshadows_CS_RTShadowTrace32TemporalHQ_Tiled` | 3,313,191 | 3,130,709 | 225 x 448 |
| `Jindirect_diffuse_lighting_CS_ScreenProbes_ActivateProbes` | 6,399,972 | 6,317,327 | 3,281 x 713, 3,281 x 679, 3,281 x 679 |
| `Pindirect_diffuse_lighting_CS_ScreenProbes_ActivateProbesTarget` | 6,356,494 | 6,293,420 | 3,281 x 713, 3,281 x 679, 3,281 x 679 |
| `Rcascadeshadows_rendering_CS_ShadowRenderTiledCascadesAlphaCBPPPC` | 2,286,818 | 2,215,277 | 1,036 x 448 |
| `Urtdirectionalshadows_CS_RTShadowTrace32TemporalHQ_OneOver2RPP_Tiled` | 3,394,431 | 3,210,421 | 249 x 448 |

95-99% of each module is the single entry function: the translator emits the program straight-line, expanding
each instruction in place (its `image_bvh` emitter expands every `image_bvh*_intersect_ray` into a full software
traversal step; whether, and how often, these 7 use it was not counted here: that needs the ISA listing). SPIRV-Cross then marks every helper `static inline __attribute__((always_inline))`, so the three
~700-word helpers of the ScreenProbes kernels are inlined 3,281 times each on top of a 6.3 M-word body.

## Change 1 (toolchain side, done and measured): `noinline` hot helpers

Recipe pass `noinline_hot_helpers` (`tools/shaderc_msl.py`): for SPIR-V over 8 MiB whose callees with at least 100
call sites would inline 1,000,000 words or more, the MSL of those helpers is printed `static
__attribute__((noinline))` instead of SPIRV-Cross's `static inline __attribute__((always_inline))`. It applies to
the three ScreenProbes kernels (4-5 helpers each) and to none of the other huge ones.

| shader | `metal -c` before | pipeline before | `metal -c` noinline | pipeline noinline |
|---|---:|---:|---:|---:|
| `Hindirect...ActivateRays` | > 600 s (timeout; `-O0` 5.8 s) | > 1,200 s (from the `-O0` library) | 526.8 s | **347.3 s, built** |
| `Jindirect...ActivateProbes` | 392.8 s | > 1,200 s | 80.2 s | **469.3 s, built** |
| `Pindirect...ActivateProbesTarget` | 385.7 s | > 1,200 s | 80.3 s | **442.2 s, built** |

(`tools/huge_cs.py --noinline-min-calls 100`, one shader per process, other compile jobs running at the same
time; `port/build/shaderc_msl/huge_cs_noinline*/huge_cs.jsonl`.) The kernels' output was not run on data; a
`noinline` call is a real call on the GPU, so the runtime cost per dispatch is not known.

## Change 2 (translator): outline the BVH step and the format decode

1. `image_bvh` (about line 15008): emit the traversal step once per module as an `OpFunction`
   (`%bvh_step = (uint4 node_ptr_lo_hi, float3 origin, float3 dir, float3 inv_dir, float tmax, uint4 desc) -> uint4`)
   and replace each in-place expansion by one `OpFunctionCall`. The per-site state (ray registers, descriptor
   SGPRs) is already in SSA values at the call site, so the change is local to the emitter: collect the body into a
   function the first time `self.uses_bvh` is set, and emit a call thereafter.
2. The typed-buffer format decode used by `dynamic_format` shaders (the 713/679-word helpers above are already
   functions): mark them `OpFunction ... DontInline`. SPIRV-Cross 1.4.363 ignores that control for MSL: it prints
   `static inline __attribute__((always_inline))` before every non-entry function unconditionally
   (`spirv_msl.cpp` `CompilerMSL::emit_function_prototype`, "must be static force-inline otherwise they will cause
   problems when linked together in a single Metallib"). Each shader here is its own metallib, so either this
   module's MSL post-pass (change 1) or a 3-line SPIRV-Cross change (print `static __attribute__((noinline))` when
   `func`'s function control has `DontInline`) carries the hint through.
3. Leave the scheduler/loop structure unchanged.

Expected effect (not measured): the entry function shrinks by (sites - 1) x step size for each outlined step; `metal -c` and the device compiler's time scale with function size, which is the measured bottleneck
(221-393 s `metal -c`, 385 s to more than 1,200 s pipeline build).

## Runtime policy regardless of the patch

- Build these pipelines off the render thread, at load, into an `MTLBinaryArchive` (the measured cold builds of the
  ones that finish take 384-385 s each); never on first use.
- Do not use the `-O0` fallback for them: for `Hindirect...ActivateRays` the default `metal -c` timed out at 600 s,
  the `-O0` library compiled in 5.8 s, and its pipeline then did not build within 1,200 s (all the optimisation moves
  to the device compiler). In `tools/huge_cs.py` (before this module's batch) `Rcascadeshadows...` compiled in 271 s
  at default and did not finish in 1,800 s at `-O0`.
