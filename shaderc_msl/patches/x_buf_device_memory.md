# Translator change: wave64 exchange buffer in device memory (4 compute shaders)

> Round 3: implemented without a translator change as a SPIR-V transform of the corpus modules, `tools/cs_xbuf.py`
> (recipe option `x_buf_device`; the exchange buffer is a storage buffer at contract binding 29, base in a Private
> variable because the accesses are in helper functions). 4/4 build compute pipelines. The runtime contract below is
> what the manifest records (`cs_route`, `x_buf_words`, `x_buf_buffer`). See STATUS.md R3.4.

Applies to `spike/s2-make-game/tree/tools/rdna2spirv.py` (the translator copy that produced the corpus; a leonida
file, so this is a proposal for its owner, not applied here). Not built, not run.

## Why

After the SPIR-V-side `shrink_lds` pass (LDS array cut to the program's RSRC2 LDS_SIZE, accesses bounds-checked),
24 of the 28 compute shaders over Metal's 32,768 B threadgroup limit build a pipeline. The other 4 cannot be fixed by
shrinking: they allocate the full 32 KiB of LDS and run 1,024 threads, so the wave64 exchange buffer `%x_buf`
(4 x (2L + 6W + 7) = 8,604 B for L = 1,024 lanes, W = 16 waves) has no room:

| shader | LDS_SIZE (RSRC2) | %x_buf | total |
|---|---:|---:|---:|
| `6vfx_triangleClip_CS_CollectTriangles_cb481f38_Wrapped` | 32,768 | 8,604 | 41,372 |
| `_vfx_triangleClip_CS_CollectTriangles_Meshlets_cb481f38_Wrapped` | 32,768 | 8,604 | 41,372 |
| `Lvfx_proj_decal_hit_detection_CS_ComputeHitDetectionResults_7b0d06e8_Wrapped` | 32,768 | 8,604 | 41,372 |
| `Svfx_proj_decal_hit_detection_CS_ComputeHitDetectionSkinnedResults_7b0d06e8_Wrapped` | 32,768 | 8,604 | 41,372 |

`--max-shared-memory 32768` cannot help them: `fit_shared_memory` only shrinks the LDS array to the RSRC2
allocation, and here the allocation itself is 32 KiB.

## Change

A new option `--x-buf-storage device` (default `workgroup`, the current behaviour), used by the build only for
shaders where `lds_declared * 4 + x_buf_bytes() > MAX_SHARED_BYTES`:

1. Declarations (`assemble`, the `if gen.emulate:` block that declares `%x_arr`/`%x_buf`, about line 16279):
   ```
   %x_rt = OpTypeRuntimeArray %uint                     ; ArrayStride 4
   %x_blk = OpTypeStruct %x_rt                          ; Block, member 0 Offset 0
   %_ptr_SB_x_blk = OpTypePointer StorageBuffer %x_blk
   %_ptr_WG_uint_x = OpTypePointer StorageBuffer %uint  ; same name, so every OpAccessChain site keeps its text
   %x_dev = OpVariable %_ptr_SB_x_blk StorageBuffer     ; DescriptorSet 0, Binding 29, Coherent
   ```
   Binding 29 is unused by the corpus contract (bindings 0-28, index.json maximum 19), so it does not collide.
2. One helper replaces the 29 `OpAccessChain %_ptr_WG_uint_x %x_buf {idx}` emissions:
   `x_ptr(idx) -> OpAccessChain %_ptr_WG_uint_x %x_dev %uint_0 (%x_base + idx)`, where `%x_base` is computed once in the
   emulated prologue (`x_prologue`, about line 18840) as
   `(wg_id.z * num_wg.y + wg_id.y) * num_wg.x + wg_id.x` times `x_sink + 1` words.
3. Barriers need no change: the emulated scheduler already uses semantics 0x948 (AcquireRelease | UniformMemory |
   WorkgroupMemory | ImageMemory), which orders storage-buffer accesses within the workgroup. The `OpAtomicStore` /
   `OpAtomicOr` sites keep Workgroup scope, which Vulkan and SPIRV-Cross allow on storage buffers.
4. index.json gets `"x_buf_binding": 29` and `"x_buf_words": x_sink + 1`, and `lds_bytes` drops by `x_buf_bytes()`.

## Runtime contract

The backend binds, for every dispatch of such a pipeline, a private buffer of
`x_buf_words * 4 * groups_x * groups_y * groups_z` bytes at binding 29 (this toolchain's split pre-pass turns it into
`spvDescriptorSet30 [[buffer(30)]]`). Contents need not survive between dispatches; the prologue zeroes its slice.
For an indirect dispatch the size comes from the largest group count the title can issue, or the dispatch is split.

## Cost to expect (not measured)

Every cross-lane exchange of the emulated wave64 becomes a device-memory round trip (L2) instead of threadgroup
memory. These are VFX passes (triangle clip, decal hit detection), not on the first-scene path.
