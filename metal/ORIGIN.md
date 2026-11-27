# Origins of ported logic in port/metal

No file in this module is a copy of a leonida or VIctory file; nothing was copied into `port/` from either tree.
The items below are logic ported (re-implemented in C++/Objective-C++ from reading the source). Both upstreams
are GPL-3.0-or-later, as is this module.

| here | ported from | what was taken | changes |
|---|---|---|---|
| `src/rect_list.cpp` `right_angle` | VIctory `crates/victory-shader/src/emit/rect.rs` `right_angle` (lines 17-33) and its rule tests (`rule_tests`, lines 144-160) | corner opposite the longest side; ties, NaN and degenerate triangles take the second corner | C++; the rule tests are repeated in `tests/test_rect_list.cpp` `cpu_rules` |
| `src/rect_list.cpp` `expand_rect_vertices`, `components`, half/unorm/snorm read/write | VIctory `crates/victory-gpu/src/primitive.rs` `expand_rects`, `Kind`, `components`, `half_to_f32`, `f32_to_half` (lines 1-185) | input-side fourth vertex `va + vb - vc`, format table by DXGI number, per-format component codecs | emits 4 vertices plus 6 indices per rectangle (index-buffer rewrite) instead of 6 vertices; triangle order from the leonida table below; optional source index list |
| `src/rect_list.cpp` `triangle_corners`, wrapper triangle table | leonida `runtime/src/rect_list_geometry.h` header comment (as ported in `port/d3d12_vulkan/src/rect_list_geometry.h`, lines 1-30) | the two triangles per corner, both starting at the first vertex with its winding; the fact that the fourth corner is made after the vertex stage; the `VS_Blit_6e5520d1` corner-at-second-vertex example | rule only; the Metal vertex-function wrapper (`generate_rect_wrapper`) is new code |
| `src/mtl_formats.mm` `quantize_d24`, D24S8 as Depth32Float_Stencil8 | leonida `runtime/src/depth24_fragment.h` (port copy lines 1-25) and `depth24.h` (port copy `depth24_quantize` :34-39, `depth24_clear_value` :48) | D24 code = round(d * (2^24 - 1)) with a float product, stored as a float that reads back as that code | round 2: the float product replaced round 1's double product |
| `src/msl_rewrite.cpp` D24 fragment wrapper (`gta6rt_d24_quantize`, `gta6rt_d24_biased`) | leonida `depth24_fragment.h` (port copy: rule in the header comment, `depth24_bias_constant` :45) | the pixel stage writes quantise(z + DepthBias * r + slope term) and the rasterizer bias is off | MSL source rewrite of the SPIRV-Cross fragment entry instead of leonida's SPIR-V patch |
| `src/mtl_device.mm` `gta6rt_quad_expand` kernel, shared quad pattern | leonida `runtime/src/quad_index_expand.comp` (port copy `quad_index_expand.h`, `vulkan_replay.h:1228-1231`) | 6 indices (0 1 2 0 2 3) per quad; a shared pattern buffer for non-indexed quad lists | MSL kernel run as a hoisted pre-pass |
| `src/msl_rewrite.cpp` mesh estimate | `spike/s5.2-msl-corpus/mesh_shapes.py` rule (RESULT.md section 4) and the S0.5 scalar fit (SCOPE 4.3) | mesh size = nv x 16 B x ceil(vertex scalars / 4); unique scalars with `user(clipN)` copies counted once | own parser of the SPIRV-Cross MSL |
| `tests/test_hostmem.cpp` sizes (185 MiB heap at 0x34310000, 174 MiB ring at +0xB00000, ~600 draws x 28 KiB) and the ocean formats | `spike/ref-victory-7900xtx/ASSESSMENT.md` (P1) and `spike/ref-cool-fox/ASSESSMENT.md`, both read only | facts about the title only; no code | none |
| `tools/route_a_msl.py` base recipe | `spike/s0.4-shaders/routeA_run.py`, `spike/s5.2-msl-corpus/step1_cross.py` (`ARGFLAGS`) | the SPIRV-Cross flags | split pre-pass (set-0 binding b to set 1 + b) is new |
| root contract in `gpu.h` (`RootLayoutDesc` defaults 144 / 32) | leonida `docs/d3d12_vulkan.md` section 5 table (lines 4171-4177) | s0..s31, heap_base, code_base in one 144-byte push block; set-0 binding numbers | none |
| object split in `gpu.h` | VIctory `crates/victory-gpu/src/host.rs`, `descriptor/host.rs`, `pipeline/host.rs`, `root.rs` | the device / descriptor / root / pipeline division and the "root signature of our own" shape | structure only |
| barrier joins (`MetalQueue::open_encoder`, `barrier_is_noop`) | leonida `vulkan_replay.h` (port copy): every D3D12 barrier becomes a `vkCmdPipelineBarrier` | that every barrier orders work, whatever resource it names | own fence-pool design for Metal; the read-only-to-read-only no-op is new |
| `tests/test_hazards.cpp`, `tests/shaders/hazards.metal` | the reviewer's probe for `REVIEW.md` (modes A-A4, C, G, G2), a session-scratch file that is not in the tree | heavy pass then bindless sample (2048x2048, 6 draws), `heap_base` read, draw before a satisfied value wait | rewritten as a test with assertions; barriers added where D3D12 needs them; new cases: two executes, placed render target reuse, device-local arena, committed buffer by address |
| `src/mtl_window.mm` | pattern of `port/d3d12_vulkan/platform/metal_window.mm` (NSView with a CAMetalLayer backing layer, layer-only fallback) | approach | own, smaller code; that module's file is untouched and not linked |

The test shaders in `tests/shaders/` are written here in the shape SPIRV-Cross emits (`main0_in`, `main0_out`,
`spvDescriptorSetN`, `spvDescriptor<T>`), so the backend's reflection and wrapper code see the same forms as the
Route A output. Game shaders are never copied into the source tree: `tools/route_a_msl.py` reads them from
`spike/s2-make-game/work/shaders/spirv` and writes into the build tree.
