# Mesh pipelines on the Vulkan path: what `--mesh-compute` needs here, and the comparison with going to Metal

Round 4, 2026-11-26. A plan, not an implementation: nothing below was built this round (task item 7). Facts are cited
to where they were measured; estimates are labelled as such.

## 1. What is refused today

- Apple's Vulkan drivers (MoltenVK 1.4.2, KosmicKrisp 26.3.1) have no `VK_EXT_mesh_shader`. Every mesh PSO is refused
  with E_NOTIMPL: 14,992 PSO calls a boot (12,062 through Xbox slot 57, 2,930 through slot 86; `vk-counters` in
  `run/logs/r3-mvk-final.log`, `s2-firstrun.log`). By name they are world geometry: `MS_VS_*` 7,835, `MSNC_VS_*`
  6,145, `MSR_VS_*` 934, `MST_DS_*` 62 (spike/r4-pso-census, confirmed by its VERIFY).
- None is drawn before FINISH SETUP?: the boot pages draw with 3 graphics PSOs and 1 compute program
  (spike/r4-pso-census VERIFY 1a). The title's `DispatchMesh` command signature exists (its ExecuteIndirect records
  are skipped, logged). So the first stop that needs mesh pipelines is the first world frame. `run/logs/r4vk-s3-b.log` reached
  the world phase at +210.3 s (25,479 slot-86 mesh refusals by then) and ended 10 s later in FSR4.
- The corpus (`build/shaders_vk/apple/spirv/index.json`): 9,282 mesh shaders, all `MeshShadingEXT`, none translated
  with `--mesh-compute` (0 index entries carry `mesh_compute`). leonida's design.md row "Mesh shaders as compute":
  every corpus MS is 128x1x1 with 128 vertices / 128 primitives, wave64, 0-18 params, clip distances in 1,906, no cull
  distances, no per-primitive attributes, no task shaders.

## 2. What leonida's `--mesh-compute` provides, and what it does not

Provided (translator, `leonida/tools/rdna2spirv.py --mesh-compute`, `tools/mesh_compute_vs.py`, tests
`tests/test_mesh_compute.py`, `test_mesh_compute_vs.py`; GPU-checked by leonida on a GTX 1060 only):
- each MS becomes a GLCompute module, same workgroup size and code; the epilogue writes the counts, vertices and
  primitives into this workgroup's slot of an output buffer whose device address is push-constant dwords 36-37 (the
  block grows 144 -> 152 bytes);
- the slot layout per shader (`mesh_compute_layout`): 16-byte header, `max_vertices` x (position, clip/cull vec4s,
  one vec4 per param), then `max_primitives` x 16 bytes (three indices, culled flag);
- `mesh_compute_vs.py` writes the vertex shader that draws the slots: non-indexed `vkCmdDraw(groups x maxp x 3)`,
  vertex v = workgroup v / (3 maxp), primitive (v / 3) mod maxp, corner v mod 3; dead primitives are clipped.
  With `--f16-interp-fallback` (on for Apple) it also exports the f16 halves, paired per PSO (`pair_f16_halves`).

Not provided: the runtime side. leonida's design.md says so itself: "The runtime side, turning a `DispatchMesh` into
that dispatch, a barrier and the draw, is the follow-up". `port/d3d12_vulkan` has none of it.

## 3. What running it here needs (work items, in order)

1. **Corpus** (`port/shaders_vk`): re-translate the 9,282 MS with `--mesh-compute --wave64-emulate
   --apple-float-controls --f16-interp-fallback` into a second module per MS (`<name>.msc.spv`), and the generated VS
   per MS (`<name>.mesh_vs.spv`). Index entries gain `mesh_compute`. Cost estimate: the S2 shaders stage translated
   26,503 modules; 9,282 more is about a third of that job again (not measured for this flag set).
2. **PSO creation** (slots 57/86 and the PC stream path, `d3d12_vulkan_device.cpp` where `mesh PSO, no
   VK_EXT_mesh_shader` is refused): build two pipelines instead of one. A compute pipeline from the `.msc` module with
   the 152-byte push block, and a graphics pipeline from the generated VS plus the PSO's PS with the PSO's fixed-function
   state (topology triangle list, no vertex input). `spirv_f16_halves.h` links the VS's halves to the PS as it does for
   translated VS today. Two native pipelines per mesh PSO: up to ~30,000 pipelines for the 14,992 calls, built lazily
   as graphics pipelines are now.
3. **Replay of `DispatchMesh(x, y, z)`** (`vulkan_replay.h`, the `draw.mesh` branch that calls `draw_mesh_tasks`): end
   the render pass, `vkCmdDispatch(x, y, z)` with the output address in dwords 36-37, a compute->vertex-input barrier
   on the slot buffer, re-begin rendering, `vkCmdDraw(x y z * 128 * 3)`. Every mesh draw then splits the render pass in
   two (a load/store of the attachments on Apple's tile memory each time: a cost to measure, not estimated here).
   A batching alternative: run every mesh compute dispatch of a render pass before the pass, then all draws; correct
   only when no draw in between writes what a later mesh dispatch reads (UAV writes from pixel shaders), so it needs
   a check per pass.
4. **Slot memory**: groups x `slot_bytes` per dispatch. For the common shape (3 params, no clip):
   16 + 128 x 64 + 128 x 16 = 10,256 B per workgroup (from the layout formula); a 1,000-group draw needs 10 MiB. A
   per-submission ring sized from the frame's mesh draws, retired with the submission (like the indirect clamp
   scratch).
5. **Indirect mesh draws** (ExecuteIndirect with the DISPATCH_MESH signature, skipped today): an indirect dispatch,
   then a 1-thread compute pass that turns the group counts into the draw's vertex count, then an indirect draw. The
   65,535-groups-per-dimension cap and the console's thread-count flag (NOTES, fork findings) apply to the dispatch.
6. **Cost per draw** (estimate): every workgroup's 384 VS invocations run whether or not its primitives exist (128
   primitives x 3 corners), plus the slot write and read. leonida names compaction with an indirect draw as the fix.

Rough size: items 2-5 are several days of layer work plus a re-translation run, with no title stop to test them
against until the first world frame.

## 4. Against going straight to the native Metal backend

| | mesh-as-compute on MoltenVK/KosmicKrisp | `port/metal` (native) |
|---|---|---|
| mesh pipelines | none native; all 9,282 MS through compute + VS | native object/mesh pipelines exist and pass tests (`port/metal/STATUS.md` section 3: mesh-only 512/512 px, object+mesh, indirect mesh with count buffer) |
| shaders that cannot be native | all | 4,631 of 9,282 exceed Metal's mesh output limits as translated (S5.2: 128 x 16 B x ceil(scalars/4) <= 32,768; 401 also over 124 unique scalars). 4,230 would pass at 64/64 meshlets (measured), the 401 need fewer varyings or mesh-as-compute |
| mesh-as-compute needed | for 100 % of MS | for the 4,631 over-limit MS until meshlet splitting lands (port/metal/ADAPTER.md: `limit_exceeded` -> leonida's mesh-as-compute variant) |
| pass splits | one per mesh draw (Vulkan has no mid-pass compute) | the same for the compute fallback; none for native mesh draws |
| other blockers on the same frame | rect lists (vertex expansion now, this round), 195 GS with no stage, 28 CS over 32 KiB, host copies of title memory every submission | GS has no Metal stage either (S5.2 item 4); title memory can be read in place through no-copy buffers (ADAPTER.md section 5, measured 0 wrong words) |
| layer work | items 2-6 above, inside `d3d12_vulkan` | ADAPTER.md section 6: extract the non-Vulkan parts of `d3d12_vulkan_device.cpp` first, then MetalContext, metal_replay, metallib store |

The compute-expansion machinery (slot layout, generated VS, the dispatch + barrier + draw sequence, the indirect
conversion) is needed on Metal too, for the 4,631 over-limit shaders. Written once against the layer's IR it serves
both backends: the Vulkan path would use it for every MS, the Metal path for the over-limit half.

## 5. Recommendation

Do not build mesh-as-compute inside `d3d12_vulkan` before the first world frame is the next stop: it adds a second
pipeline per mesh PSO and a render-pass split per mesh draw to a path whose submitting thread is already the limit
(round 4 STATUS: 44-55 % of its time in copies and replay on the boot pages). Build the expansion as backend-neutral
replay logic (IR `Draw{mesh}` -> compute + draw) when the Metal adapter extracts the shared layer (ADAPTER.md step 1),
so it lands once, for the Metal path's over-limit shaders and, if still needed, for Vulkan. Until then mesh PSOs stay
refused with their named reason (counted; the title gets E_NOTIMPL from the factory and E_INVALIDARG from the slot-57
fallback, unchanged).
