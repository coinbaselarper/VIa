# Mesh output limits: what the toolchain does and what still needs the translator or runtime

> Round 3: the meshlet split below is no longer "not done" and needs no translator change: it is a SPIR-V transform in
> `tools/mesh_split.py` (object stage + chunked mesh stage), 1,135 of 1,135 shaders built and created Metal pipelines;
> the 50 shaders over 124 scalars take mesh-as-compute (`tools/mesh_msc.py`). See STATUS.md R3.2-R3.3. The text
> below is the original design note, kept for the record; its "two halves" scheme was replaced by a greedy
> first-fit walk with a per-shader chunk count.

Measured numbers are in `../STATUS.md` (section "Mesh"). This file is the design for the parts not done here.

## Done in this module (SPIR-V / MSL side)

1. `drop_outputs` (SPIR-V): user outputs no paired pixel shader reads are deleted (stores and access chains removed,
   variable and interface entry dropped). Live set = union of the `Input` locations of every pixel shader the
   title's pipeline library pairs with this mesh shader (`tools/pipelines.py`, `tools/mesh_fit.py`).
2. `trim_outputs` (SPIR-V) + `pack_mesh_struct` (MSL): each kept output keeps only the leading components the paired
   pixel shaders read (`ps_reads`: OpCompositeExtract / OpVectorShuffle / component access chains); the MSL vertex
   struct is then sorted by alignment. Metal links a narrower mesh output to a wider fragment input (measured:
   float2 -> float4 builds).

Validity condition the runtime must check: a variant compiled for live set L may only be used with a pixel shader
whose input locations are a subset of L. The library pairs are known offline; a pipeline the title creates at run
time with another pixel shader needs a new variant (the passes take milliseconds; the compile is the cost).

## Not done: unpaired mesh shaders

Mesh shaders that appear in no pipeline-library record have no known reader. Their dead outputs can only be
removed when the runtime sees the pipeline: run `drop_outputs`/`trim_outputs` at pipeline creation with the pixel
shader's input set and compile then (async, cached by the same content key).

## Not done: meshlet split (translator)

The rest exceed the size limit at 128 vertices but fit at 64 vertices / 64 primitives (PSO measured with the
declaration rewritten, body unchanged; such a library is not usable). A correct split needs the translator to emit
the mesh program as two 64-vertex halves: the 128-thread group still runs the whole program (cross-lane wave64
emulation needs all lanes), but each half's threadgroup emits only the primitives whose three indices fall in its
half, and primitives that straddle the halves are emitted by both with their vertices duplicated into the half.
That needs an object (amplification) stage launching two mesh groups per original group, or a payload of the
straddling primitives. Until then these shaders go to mesh-as-compute.

## Not done: more than 124 unique scalars

Shaders above 124 scalars after dead-output elimination cannot pass at any vertex count; they need fp16 packing of
varyings the pixel shader reads at 16 bits (index.json `ps_fp16_interpolants`), or mesh-as-compute.
