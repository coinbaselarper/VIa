# gta6rt_metal: design of the native Metal backend (S5.1, rounds 1 and 2)

Date 2026-11-26. Host: Apple M4 Pro, macOS 27.0.1, SDK 27.2, Apple clang 21.0.0. Status of each item (what is
built and measured) is in `STATUS.md`; this file is the design. Facts marked **measured** come from the tests in
`tests/` (numbers in STATUS.md); everything else is a design decision or a cited upstream/SDK fact. Revised after
`REVIEW.md` (findings B1-B3): sections 3.3, 3.5, F08, F09, F19 and 5.2 changed; STATUS.md "Review response" has
the measurements. Round 2 (gpu.h api_version 2): section 3.3 rewritten (epoch joins), items 3.6-3.10 and 3.12 and
section 8 added (Labels moved from 3.6 to 3.11), feature rows F06, F08, F13, F14, F17, F21, F22, F26 updated, rows
F28, F31, F32, F33 added, section 5.3 updated. (The first round-2 pass stated these additions in this header without
writing them; they were written in the re-verification pass.) Round-2 review response: 3.5 (resource-usage
validation as the residency check), 3.8 (timestamp ordering) and 3.12 (where GPU errors come from, the event-signal
repair, the injection hook) revised. `ADAPTER.md` is the call-by-call plan for leonida's D3D12 layer.

Inputs: `PLAN.md` (victory-gpu host split as the template), `NOTES.md` S0.6, `spike/s0.5-scope/SCOPE.md` with
`REVIEW.md` (feature IDs F01-F43 below are SCOPE's), `spike/s0.4-shaders/RESULT.md`, `spike/s5.2-msl-corpus`
(`results.json`), leonida's D3D12 layer as ported in `port/d3d12_vulkan` (`src/vulkan_backend.h`,
`src/command_list.h`, `src/vulkan_replay.h`, `src/rect_list_geometry.h`, `src/depth24_fragment.h`,
`src/queue_holds.h`, `src/value_write_sink.h`), VIctory `victory-gpu` (`host.rs`, `descriptor/host.rs`,
`pipeline/host.rs`, `root.rs`, `primitive.rs`) and `victory-shader/src/emit/rect.rs`.

## 1. Where the backend sits

```
 title ── d3d12_xs / d3d12_com (COM objects, console ABI, slot quirks: F02)     unchanged, in d3d12_vulkan
            │  records leonida::CommandList (command_list.h IR), lowers root signatures to s0..s31,
            │  owns the T#/S# hash table, queue holds, list-value pump, title memory model
            ▼
       gta6rt::gpu interface (include/gta6rt_metal/gpu.h)                       this module
            ├── today:  VulkanContext / VulkanQueue / VulkanFence (vulkan_backend.h)   behind an adapter, later
            └── new:    gta6rt::metal (src/*.mm)  ── Metal ── Apple GPU
```

leonida's backend today is three concrete classes, not an interface: `VulkanContext` (resources by
`ResourceId`, buffer-VA index, heap base, bindless prepare/commit, pipelines by id, deferred deletion),
`VulkanQueue` (`execute(span<const CommandList*>)` replays the IR, `signal`, `wait`, markers) and `VulkanFence`
(timeline semaphore). The COM objects in `shims/d3d12_com.cpp` are thin wrappers over them. `gpu.h` is the
backend-neutral version of exactly that surface, so the D3D12 layer can be pointed at either backend: the switch
is an adapter that replays `leonida::CommandList` into `gpu::CommandList` calls (section 2.4), not a rewrite of
the semantics layer.

The split follows victory-gpu's host modules, which keep the title-facing objects (`device.rs`, `descriptor.rs`,
`pipeline.rs`, `root.rs`) apart from the host GPU objects behind them (`host.rs`, `descriptor/host.rs`,
`pipeline/host.rs`):

| victory-gpu host module | role there | gpu.h part | Metal implementation |
|---|---|---|---|
| `host.rs` (`Host`: device, queue, fence, present) | one host device, direct queue, window | `Device`, `Queue`, `Fence`, `Swapchain` | `src/mtl_device.mm`, `mtl_queue.mm` (queues, fences, `MetalSwapchain`), `mtl_window.mm` |
| `descriptor/host.rs` (`HostHeap`: host heaps that keep the title heap's slot order) | descriptor tables the translated shaders index directly | `DescriptorHeap` (one bindless table per contract binding) | `src/mtl_resource.mm` (`MetalDescriptorHeap`: argument buffer tier 2 + residency set) |
| `root.rs` + the root signature of its own in `pipeline/host.rs` (one block of root constants per stage for user data, root views, static samplers, directly indexed heaps) | binding contract of translated shaders | `RootLayout` (leonida's contract: 144-byte push block + set-0 tables) | `src/mtl_pipeline.mm` (binding plan from reflection) |
| `pipeline/host.rs` (`GraphicsState`, host pipelines, rectangle GS) | title state + translated shaders → host PSO | `RenderPipeline`, `ShaderLibrary`, `RectMode` | `src/mtl_pipeline.mm`, `src/rect_list.cpp` |
| `list.rs` / `draw.rs` / `queue.rs` (encode at execute) | title lists replayed on host lists | `CommandList` (records an IR), `Queue::execute` (encodes) | `src/mtl_command.mm`, `src/mtl_queue.mm` |
| `primitive.rs` (CPU rectangle expansion) | fallback when no GS | `RectMode::cpu_rewrite` | `src/rect_list.cpp` |

## 2. The backend interface (C++)

Header: `include/gta6rt_metal/gpu.h` (namespace `gta6rt::gpu`, `api_version` 1), factory and window helper:
`include/gta6rt_metal/metal_backend.h` (`gta6rt::metal::create_device`, `create_window`), rectangle lists:
`include/gta6rt_metal/rect_list.h`. C++20, no Objective-C, Metal, Vulkan or Windows types, so leonida's C++ (and
the `windows.h`-shaped compat world it compiles in) can include it.

### 2.1 Object model

| object | created by | D3D12 counterpart | notes |
|---|---|---|---|
| `Device` | `metal::create_device` | `ID3D12Device` host side | caps, stats, format table (DXGI numbering), console VA space |
| `Queue` | `create_queue(direct/compute/copy)` | `ID3D12CommandQueue` | `execute`, `signal`, `wait`, `present`, `wait_idle` |
| `Fence` | `create_fence` | `ID3D12Fence` | CPU value, `on_completion` (SetEventOnCompletion), `value_address` (console slot 12) |
| `Heap` | `create_heap` | `ID3D12Heap` | placement heap; `console_arena` adds one buffer over the whole heap |
| `Buffer`, `Texture` | `create_buffer/texture`, `create_placed_*` | committed / placed `ID3D12Resource` | `gpu_va()` is a console-style VA, `host_gpu_address()` the Metal one |
| `Sampler` | `create_sampler` | sampler descriptor contents | |
| `DescriptorHeap` | `create_descriptor_heap(kind, count)` | the host side of shader-visible heaps | one table of 8-byte entries per contract binding |
| `RootLayout` | `create_root_layout` | root signature, after lowering | push block size, user dwords, tables |
| `ShaderLibrary` | `create_library_msl` / `create_library_metallib` | shader bytecode | MSL source (Route A) or a metallib |
| `RenderPipeline` | `create_render_pipeline` | graphics PSO | `CompileMode` eager / lazy / async; reflection and binding plan |
| `CommandList` | `create_command_list` | `ID3D12GraphicsCommandList` | records an IR; nothing touches Metal until `Queue::execute` |
| `Swapchain` | `create_swapchain(CAMetalLayer*)` | DXGI swapchain / PresentX target | owns the title's back buffers; present composes into the drawable |
| `ComputePipeline`, `MeshPipeline` (v2) | `create_compute_pipeline`, `create_mesh_pipeline` | compute PSO, mesh PSO (console subobject 24) | same compile machinery (3.10); `limit()` on refusal |
| `QueryHeap` (v2) | `create_query_heap` | `ID3D12QueryHeap` | occlusion, binary occlusion, timestamp, pipeline statistics (3.8) |
| host window, buffer texture (v2) | `create_host_buffer`, `create_buffer_texture` | title pages used in place | section 8 |

Ownership: every create returns `std::unique_ptr`; the D3D12 COM object that wraps it owns it. Descriptor slots
hold strong references to what they name (VIctory `descriptor/host.rs` `kept`), so a title that releases a
resource a shader may still read through a stale slot does not fault. Resources named by recorded commands must
stay alive until the GPU is done with them, as in D3D12; once encoded, Metal command buffers retain them.

Threads: device creation functions are thread-safe; a `CommandList` is used by one thread at a time (D3D12 rule);
`Queue` calls are serialised per queue; fence callbacks run on a backend dispatch queue.

### 2.2 The root contract

The D3D12 layer lowers every root signature to leonida's translated-shader contract (`d3d12_vulkan.md` section 5):
`s0..s31` user data (root constants copied, root CBV/SRV/UAV and descriptor-table base addresses as 64-bit
console VAs), `heap_base` (dwords 32-33), `code_base` (dwords 34-35) in one 144-byte push block, plus set-0
bindings: 0/3/5/6 sampled images (typed declarations alias one binding), 1 samplers, 2 the T#/S# hash table, 4, 7,
8-19, 26-28 storage images. The backend therefore never sees a D3D12 root signature: `RootLayoutDesc` is
`{push_bytes = 144, user_dwords = 32, tables = [(binding, kind)...]}`, and the draw-time calls are
`set_user_data`, `set_heap_base`, `set_code_base`, `set_descriptor_table(binding, heap)`. VIctory reached the same
shape independently (one root-constant block per stage for user data, directly indexed heaps).

How that lands in Metal is decided per pipeline from Metal's own reflection (the **binding plan**,
`PipelineReflection::plan`): the push block is the `constant` buffer argument of `push_bytes` bytes and goes in by
`set{Vertex,Fragment}Bytes`; an argument named `spvDescriptorSetN` is SPIRV-Cross's set N and gets the table of
contract binding `N - 1` (split recipe, section 6) by `set*Buffer`; vertex buffers use `[[buffer(30 - slot)]]`,
and a collision with a shader binding is reported. Indices therefore never need to be agreed between shaders,
which matters because SPIRV-Cross numbers the push block after the last set the shader uses (push at `buffer(0)` in
a VS without sets, `buffer(8)` in the PS of section 6).

### 2.3 Command recording and encoding

`CommandList` records a small IR (`std::variant` of commands, `src/mtl_internal.h`) and `Queue::execute` encodes it
into one `MTLCommandBuffer` per call. D3D12 lists can be executed many times and on any queue of their type; Metal
command buffers commit once; so recording into Metal directly is not possible (SCOPE F05). Encoding is where the
Metal-specific decisions are made: render pass formation, load/store actions, encoder splits and fence joins at
barriers (3.3), residency (3.5), command-buffer splits at value waits, rectangle expansion.

### 2.4 Mapping from leonida's backend calls

| leonida (`vulkan_backend.h`, `command_list.h`) | gpu.h | slice 1 |
|---|---|---|
| `VulkanContext::add_resource/remove_resource/resolve`, `ResourceBinding` | `create_buffer/texture/placed_*`, object lifetime | yes |
| `knows_buffer_va`, buffer-VA index (`buffer_va_index.h`) | `Device::resolve_va` | yes |
| `set_heap_base(heap_base, size)` | `CommandList::set_heap_base`, `Heap::host_gpu_address` of the arena | yes |
| `add_pipeline` (compute, by id), on-demand graphics pipelines (`LEONIDA_PIPELINE_COMPILE=on-demand`) | `create_render_pipeline` with `CompileMode::lazy/async` (compute: not yet) | graphics yes, compute no |
| `set_bindless_prepare/commit/current`, `vulkan_bindless.h` set 0 | `DescriptorHeap` per binding; table residency attached to every command buffer that binds the table, device residency for every allocation (3.5) | yes |
| `write_host_texture`, `write_staged_title_image` | `Texture::write_region` (shared), `copy_buffer_to_texture` | yes (no detile) |
| `VulkanQueue::execute(lists)` | `Queue::execute` | `Queue::execute` yes; the adapter that replays `leonida::CommandList` into it is not written |
| `VulkanQueue::signal/wait(VkSemaphore, value)` | `Queue::signal/wait(Fence*, value)` | yes |
| `VulkanFence::signal/wait` (CPU), `FenceWaitDispatcher` | `Fence::signal_cpu/wait_cpu/on_completion` | yes |
| `QueueHolds` (wait-before-signal across queues) | stays in the D3D12 layer (owns no GPU objects) | n/a |
| presenter (`vulkan_present.h`, `present_compose.inc`) | `Swapchain` + `Queue::present` | single plane |
| list-value pump (`d3d12_xs_list_values.h`, `value_write_sink.h`) | `CommandList::write_value/wait_value` | yes |
| IR `Draw` (pipeline, `user_data[32]`, views, depth, index, topology, viewport, scissor, blend factor, stencil) | `set_*` + `draw/draw_indexed` | yes |
| IR `ClearRenderTarget`, `ClearDepthStencil`, `DiscardResource` | `clear_*`, `discard` (folded into load/store) | yes (no rects) |
| IR `TransitionBarrier`, `UavBarrier`, `AliasingBarrier`, `Global/Buffer/TextureBarrier` | `barrier` | yes |
| IR `CopyBuffer`, `CopyTexture` (footprint), `CopyResource` | `copy_buffer`, `copy_buffer_to_texture`, `copy_texture_to_buffer` | partial |
| IR `WriteImmediate`, `label::Write` | `write_value` | yes |
| IR `DebugMarker` | `begin_event/end_event` | yes |
| IR `Dispatch`, `ExecuteIndirect`, `BeginQuery`/`EndQuery`/`ResolveQueries`, `label::Write` clock 1 | `dispatch`, `execute_indirect`, `begin/end/resolve_query`, `write_timestamp` (v2) | round 2 |
| IR `ClearUnorderedAccess`, `ResolveSubresource`, `Predication`, `AtomicCopyBuffer`, acceleration structures | not in gpu.h v2 | no (STATUS.md stubs); per-command plan in ADAPTER.md section 4 |

## 3. Metal-wide decisions

1. **API level: Metal 3 command model now, Metal 4 later.** Slice 1 uses `MTLCommandQueue`/`MTLCommandBuffer` and
   the classic encoders, plus the Metal 3 bindless model (argument buffers tier 2 written as raw `MTLResourceID` /
   `gpuAddress` values) and `MTLResidencySet` (macOS 15+). Reason: the encoders and argument buffers are what the
   Route A MSL (SPIRV-Cross) targets, and they are known to work in an x86_64 process under Rosetta (measured: the
   same tests pass there, STATUS.md). Metal 4 (`MTL4CommandQueue`, `MTL4CommandAllocator`, `MTL4ArgumentTable` bound
   by GPU address, `MTL4Compiler`, explicit barriers) is a closer match to D3D12 and is the planned second command
   layer behind the same `gpu.h`; it is not called anywhere yet. Device caps report `MTLGPUFamilyMetal4`.
2. **Storage.** Unified memory (measured `hasUnifiedMemory` 1): `upload`/`readback`/`shared` are
   `MTLStorageModeShared`, `device_local` is `MTLStorageModePrivate`. Write-combined CPU cache mode for upload.
3. **Hazard tracking: barriers order everything, Metal tracking is not relied on.** Committed resources are still
   created tracked and placed resources untracked (the D3D12 aliasing model needs it), but Metal's tracking only
   covers resources bound directly or declared with `useResource:usage:stages:` (`MTLRenderCommandEncoder.h`: the
   argument-buffer `useResource`/`useHeap` forms "do not protect against data hazards"). Every resource can reach a
   shader through a descriptor table, which Metal does not track, so the backend orders work itself, the way leonida
   turns every D3D12 barrier into a `vkCmdPipelineBarrier`:
   - **Join (round 2: epochs).** Each queue keeps a pool of 9 `MTLFence`s; every encoder updates one when it ends
     (render: after the vertex and fragment stages, plus object and mesh when it drew meshes). A barrier starts a
     new *epoch*: the fences of the encoders since the previous barrier become the epoch's wait set, and **every**
     encoder of the new epoch waits for that set (render encoders before the vertex, fragment, object and mesh
     stages). Waiting for the previous epoch's encoders covers the epochs before it, because each of those encoders
     waited for its own predecessor set. Two barriers with no encoder between keep the older set. Encoders of one
     epoch do not wait for each other (D3D12 needs no ordering without a barrier). Timestamp encoders and the present
     compose pass *observe*: they wait for the wait set and the current epoch without starting a new one. At most 9
     fences are referenced (wait set plus epoch); the encoder that would need a tenth forces an epoch. Round 1
     joined only the encoder right after a barrier, so a second reader after one barrier could run before the
     write: measured with the unmodified round-1 code, 8-10 of 10 stale lists in `metal_test_hazards --only
     bindless`; 0 of 10 after the change (STATUS "Join model fix").
   - **Join points:** every barrier except a transition between two read-only D3D12 states (UAV, aliasing, null,
     and any transition whose `before` or `after` includes a write state or COMMON); the start of every `execute`
     (D3D12 state decay: resources return to COMMON when an ExecuteCommandLists finishes, so work of the next one
     may touch them without a barrier); the encoder after a value wait (5.2); the present compose pass.
   - **Untracked render targets.** D3D12 orders render-target writes without barriers, so a render pass whose
     attachment is a placed (untracked) texture already attached since the last join also joins (`target_joins`).
   - Pending clears are flushed at a fenced barrier, so a clear is never moved past one.
   - **Compute encoders are concurrent** (`MTLDispatchTypeConcurrent`, as D3D12 dispatches may overlap). A barrier
     inside a compute encoder that started an epoch (so nothing earlier in its epoch is outside it) is an in-encoder
     `memoryBarrierWithScope:(buffers | textures)` instead of an encoder split, and the next encoder still joins
     (measured: 63 barriers in one encoder, 0 of 262,144 wrong; without barriers 56,835 wrong; the missing join made
     a copy after such a barrier read stale texels, 184 in the ocean test, before it was added).
   - A draw that reads a resource through a table *without* a barrier after the write is not ordered. D3D12 requires
     that barrier. The alternative, `useResource:usage:stages:` for every tracked resource of every bound table in
     every encoder, would cost O(table size) per encoder; the title's tables reach about 38,000 images (P3). The
     backend does not do it.
4. **Command buffers.** One per `execute` (more when a list contains a value wait, 5.2), plus one per `signal` and
   per `present`; a queue `wait` is encoded at the start of that queue's next command buffer. Each command buffer
   ends by signalling a per-queue `MTLSharedEvent` timeline, and a `signal` command buffer first waits on that
   timeline, so a D3D12 Signal is ordered after all earlier work on the queue regardless of how Metal overlaps
   command buffers. The same timeline gives `wait_idle` and resource-retire points. Submissions to one queue are
   serialised by a per-queue lock, so concurrent `execute` calls keep a defined order. The fence pool (3.3) is part
   of the queue and is only touched under that lock, in encoding order.
5. **Residency: everything resident from creation, as in D3D12.** Two levels:
   - The **device residency set** holds every `MTLHeap` (and so every placed resource and console arena) and every
     committed buffer and texture. Each allocation is added when it is created and removed when it is destroyed. The
     set is attached to every queue with `MTLCommandQueue addResidencySet:`, so it covers every command buffer of
     every queue, including the earlier command buffers of an `execute` that a value wait split. Pending changes are
     committed before each command buffer is created (`residency_commits`). This is what makes memory reached only
     by address resident: `heap_base` + offset into the arena, and buffers outside the arena reached by
     "device address - heap_base" (leonida `title_va_alias.h`).
   - Each descriptor table still owns an `MTLResidencySet` of what its slots name. It is attached with
     `useResidencySet:` to **every** command buffer the encoder produces in which that table was bound. It covers
     resources the device set dropped while a table slot still names them; descriptor slots keep strong references.
   - Directly bound resources (render targets, vertex/index buffers, copy operands) need neither.
   - Diagnostics: `GTA6RT_METAL_NO_RESIDENCY=1` turns off both levels; `GTA6RT_METAL_NO_DEVICE_RESIDENCY=1` only the
     device set.
   - **measured** under `MTL_SHADER_VALIDATION=1`, both arches:
     - With both levels off, all 2,132 checked triangle pixels are wrong; the bindless draws before and after a value
       wait are wrong (10 of 10); and `heap_base` reads of a shared arena, a device-local arena and a committed buffer
       return 0 0 0 0.
     - With the device set off and the table sets on, the triangle and both bindless cases are correct, while all
       three `heap_base` reads return zeros.
     - With both on, everything is correct.
   - **measured** without validation, with the device set off: the shared arena read returns zeros on both arches.
     The committed buffer read by address returns zeros on x86_64 and the right value on arm64. The device-local
     arena read is correct on both. Residency faults therefore show up without validation too, but not reliably, so
     validation runs remain the check.
   - **measured** (round-2 review response): `MTL_SHADER_VALIDATION_RESOURCE_USAGE=1` with
     `MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1` reports every access to a resource that is not resident ("Texture is
     not resident ...", "Invalid load in 'device' ..."): 43 reports with both levels off (bindless, wait split,
     heap_base), 0 with both on, also over the full-length hazard suite (400 iterations). Plain shader validation
     reports none of these and reads zeros. `metal_hazards_residency_check` fails on any report;
     `metal_hazards_residency_negative` requires them with residency off.
6. **Compute pre-passes (indirect counts, quad lists).** Metal indirect draws take no count, and Metal has no quad
   primitive, so two builtin kernels prepare arguments on the GPU:
   - `gta6rt_indirect_count` copies `max_count` records from the stride-spaced arguments into a packed scratch range
     and zeroes every record at an index of `min(*count, max_count)` or above; the command then issues `max_count`
     Metal indirect draws (or mesh draws, or dispatches) over the packed records, so the zeroed ones draw nothing.
     Without a count buffer no pre-pass runs and the records are read in place at their stride.
   - `gta6rt_quad_expand` writes 6 32-bit indices per quad (0 1 2 0 2 3, leonida's pattern) from the bound 16- or
     32-bit index buffer, honouring the first index and the view size. Non-indexed quad lists use one shared pattern
     buffer made at device creation and need no pre-pass.
   - Scratch: one shared buffer per `execute` (at least 1 MiB, 256-byte sub-allocations), kept until the command
     buffer completes and counted as `internal` memory.
   - **Hoisting.** A pre-pass cannot run inside a render pass. When an implicit pass is about to open (or at
     `begin_render_pass`), the encoder scans forward over the commands that will land in that pass (draws, mesh
     draws, events, non-dispatch indirect commands, occlusion queries, a repeat of the same render targets) and runs
     all their pre-passes in one compute encoder first; the pass waits on its fence. Any command that can write
     (barrier, copy, clear of another target, dispatch, value write) ends the scan, so the inputs a hoisted pre-pass
     reads cannot change before the draw that uses them: D3D12 requires a barrier between a GPU write and an
     indirect-argument or index read, and a barrier ends the pass. A pre-pass the scan did not reach ends the pass
     (`render_pass_splits`). An indirect dispatch with a count runs its pre-pass in the same compute encoder followed
     by `memoryBarrierWithScope:MTLBarrierScopeBuffers`. Measured: 1 hoisted, 0 splits (STATUS item 2).
   - Cost not measured: an `ExecuteIndirect` encodes `max_count` Metal commands on the CPU. An indirect command
     buffer encoded by a GPU kernel would remove that, but needs every pipeline created with
     `supportIndirectCommandBuffers`.
7. **Compute.** Pipelines use the same compile machinery as render pipelines (3.10). The threadgroup size comes from
   `ComputePipelineDesc::threads`, else from `gl_WorkGroupSize` in the MSL (SPIRV-Cross writes it only when the
   shader reads it). More than `maxTotalThreadsPerThreadgroup` threads is refused before compiling; Metal's
   threadgroup-memory error is parsed into `Status::limit_exceeded` / `PipelineLimit::threadgroup_memory` with the
   size, which measured equal to the translator's `lds_bytes`. `dispatch` is `dispatchThreadgroups:`; Metal grid
   dimensions are 32-bit, so there is no 65,535 cap and nothing is split (a split would need a group-base offset that
   the translated shaders do not read, so it could not be transparent). `dispatch_flag::thread_counts` (console
   DispatchX bit 0x20) uses `dispatchThreads:` with a partial last group. Compute encoders are concurrent; barriers
   inside one are memory barriers (3.3). The push block and tables bind by the same plan as draws.
8. **Queries and timestamps.**
   - Occlusion and binary occlusion: each query heap owns a shared visibility-result buffer of 8 bytes per query. A
     render pass takes the heap of the first occlusion query the pass holds (found by the same forward scan as 3.6);
     `begin_query` sets `setVisibilityResultMode:` counting or boolean at `index * 8`, `end_query` disables it.
     Metal restarts the counts in every render pass (measured), so a query open across passes counts only the last
     one (logged); a second heap inside one explicit pass is `not_implemented`. `resolve_queries` is a blit copy.
   - Timestamps: an `MTLCounterSampleBuffer` from the timestamp counter set. This GPU samples only at stage (encoder)
     boundaries, so `end_query` on a timestamp heap and `write_timestamp` are a one-thread compute encoder
     (`gta6rt_nop`) whose start-of-encoder sample is taken; the encoder observes (3.3: it waits for the earlier work
     of its queue without starting an epoch). Later work does not wait for it, as in D3D12: a draw recorded right
     after a timestamp may start before the sample is taken (measured under GPU load from another process: a 1.4 ms
     draw bracketed without a barrier measured 0.013 ms), so a caller that brackets work needs a barrier after the
     first timestamp, which the test now records. Samples reach memory after the encoder's fence, so resolves go into a
     following command buffer that waits for the previous one's completion on the queue timeline (measured: a
     resolve in the same command buffer read 0 for 2 of 3 samples). `write_timestamp` targets are resolved at the end
     of the `execute`. 1 GHz ticks (measured); `clock_calibration` is `sampleTimestamps` plus `CLOCK_UPTIME_RAW`.
   - Pipeline statistics resolve to zeros (88 bytes per query), logged once.
9. **Mesh pipelines and the refusal path.** `MTLMeshRenderPipelineDescriptor` with an optional object function, the
   mesh function, an optional fragment function (none: rasterisation off) and the payload size. Before compiling,
   `precheck` parses the SPIRV-Cross MSL (`mesh<V, P, nv, np, topology>` and the vertex struct `V`): vertex scalars
   are the summed component counts with the `user(clipN)` copies counted once and `primitive_culled` excluded; mesh
   bytes = `nv * 16 * ceil(scalars / 4)`. Over `max_mesh_bytes` (32,768) or over 124 unique scalars the pipeline is
   refused with `Status::limit_exceeded` and `PipelineLimit{mesh_size or mesh_scalars, value, limit,
   estimated = true, reason}`, without a compile. Without `precheck`, Metal's own error text is parsed into the same
   structure (`estimated = false`). The D3D12 layer answers either by building the mesh-as-compute variant (ADAPTER
   section 2). Draws: `drawMeshThreadgroups:` and its indirect form, threads per group from the description or the MSL.
10. **Pipeline creation (F13, F14, F06).**
    - One state machine for render, compute and mesh pipelines: `deferred`, `queued`, `compiling`, `ready`, `failed`.
      `eager` compiles on the caller; `lazy` at the first draw that binds it; `async` queues it at creation. A draw
      whose pipeline is not ready either waits (`NotReady::wait`) or is dropped and counted (`NotReady::skip`, which
      also queues a lazy pipeline at `Priority::demand`), so `skip` never compiles on the submitting thread.
    - Compile queue: `DeviceOptions::compile_threads` workers (3 by default) take `first_scene` before `demand`
      before `background`. `wait()` on a queued pipeline takes it off the queue and compiles it on the caller.
      `precompile(list, priority)` moves queued or deferred pipelines to the front at that priority (the first-scene
      list). Deferred MSL libraries (`MslOptions::deferred`) compile on the compile thread with their first pipeline.
    - `MTLBinaryArchive`: `open_pipeline_cache(dir, translator_version)` keys the archive with the translator
      version, the GPU name, the OS build and the process arch (hashed into the file name; a different key starts a
      new archive). Each pipeline is first created with `MTLPipelineOptionFailOnBinaryArchiveMiss` (a hit is
      `from_archive`), otherwise built normally and its functions added with `add*PipelineFunctionsWithDescriptor:`;
      `flush_pipeline_cache` serialises to a temporary file and renames it. Runtime-compiled MSL and offline metallib
      functions are separate entries, and under shader validation every lookup misses (measured).
11. **Labels.** Heaps, buffers, textures, queues and libraries carry the given debug name as `label`; `begin_event`
    maps to a command-buffer `pushDebugGroup`, or to an encoder signpost when an encoder is open.
12. **GPU errors.** Every committed command buffer has a completed handler; a command buffer that ends with
    `MTLCommandBufferStatusError` is counted (`DeviceStats::command_buffer_errors`), logged with its error code
    (the first 16), and the next `Queue::wait_idle` returns `Status::device_lost` (after the completed handlers of
    every command buffer up to that point have run). Revised in the round-2 review response:
    - **Where errors come from on this machine (measured from the unified log).** When another process hangs the GPU
      (here another worker's `shaderc_msl_vs_lane_test`, `kIOGPUCommandBufferCallbackErrorHang`), Metal recovers the
      GPU and fails the command buffers of other processes that were in flight with
      `kIOGPUCommandBufferCallbackErrorInnocentVictim` ("Discarded (victim of GPU error/recovery)"); WindowServer's
      too. Those are counted in `command_buffer_victims` (error code 5 or that text). All three historical
      `metal_hazards` failures were such discards (STATUS.md "Review response (round 2)").
    - **A failed command buffer skips its event signals.** Signals encoded before the point of failure happen, the
      rest do not (measured: a later command buffer waiting for the skipped value was still blocked after 2 s; an
      `MTLFence` update it skipped does not block later `waitForFence`). Every command buffer ends with the queue
      timeline signal and `signal()`'s ends with the fence signal, so one failure would block `wait_idle` until its
      timeout, every later `signal()` (its command buffer waits for the timeline) and the D3D12 fence wait behind it.
      `end_command_buffer` now records the signals it encodes; the completed handler of a failed command buffer does
      them on the CPU (only where the event is still below the value), and after any failure every completed handler
      raises the timeline to its own serial, which undoes a CPU write that raced past a later GPU signal. Counted in
      `event_signals_repaired`; `GTA6RT_METAL_NO_ERROR_REPAIR=1` turns the repair off (negative control: the fence
      wait and `wait_idle` time out).
    - **What the D3D12 layer does with it (design).** `device_lost` after a discard by recovery corresponds to
      `DXGI_ERROR_DEVICE_RESET` in `GetDeviceRemovedReason`, any other failure to `DXGI_ERROR_DEVICE_HUNG`. The queue
      keeps working after the injected failure (measured), and the three runs that lost command buffers to recovery
      ran to their normal length (51-53.5 s, from the log and ctest times), so the layer can log the lost frame and
      continue instead of reporting a removed device to the title.
    - **Test hook.** `inject_command_buffer_failure(queue, as_victim)` makes the next command buffer a real Metal
      failure: no retained references and a first copy from a released 64 MiB buffer, which the kernel driver fails
      as `InvalidResource` for this process only (the unified log showed no other process affected) while skipping the
      rest of the command buffer. It is reliable only on an idle queue with the command buffer waited for (both done by
      the hook), not under `MTL_DEBUG_LAYER` (it aborts on the released buffer), and a command buffer that samples
      GPU counters or copies a texture to a buffer completes without the error (measured), so it is a test device,
      not a fault model. Shader validation cannot drive this path: an out-of-bounds or non-resident access is
      reported on stderr and reads zero, and the command buffer completes (measured, `metal_hazards_validation_fault`).

## 4. Feature mapping (SCOPE.md P0, P0b, P1, P2)

Each row: SCOPE ID, the Metal mapping chosen, and what slice 1 has. "Stub" items are listed by name in STATUS.md.

### P0: process start to first descriptor heap

| ID | Feature | Metal mapping | slice 1 |
|---|---|---|---|
| F01 | Device, CheckFeatureSupport, adapter | `MTLCreateSystemDefaultDevice`; `DeviceCaps` from `supportsFamily` (Apple7/8/9, Metal3, Metal4), `argumentBuffersSupport`, `hasUnifiedMemory`, `maxBufferLength`, `maxThreadgroupMemoryLength`, `supportsShaderBarycentricCoordinates`, `supportsPlacementSparse`, `depth24Stencil8PixelFormatSupported`; SIMD width read by a builtin kernel (`[[threads_per_simdgroup]]`); timestamp frequency measured against the wall clock (**measured** 1.0 GHz on both arches). CheckFeatureSupport answers are the D3D12 layer's, fed by `caps()`. | done |
| F02 | Console ABI quirks (slot 26 sret, 48, 73-75, 133, allocator slot 9) | host-side proxy in the D3D12 layer, backend-neutral | n/a |
| F03 | Command queues (5 at startup) | one `MTLCommandQueue` per D3D12 queue, its own timeline event; compute and copy queues are plain Metal queues (Metal has no queue types) | done |
| F04 | Fences, SetEventOnCompletion, console slot 12 | `MTLSharedEvent` per fence (`signaledValue`, `encodeSignalEvent:`/`encodeWaitForEvent:`, `notifyListener:atValue:block:`, `waitUntilSignaledValue:timeoutMS:`). Slot 12 needs the value as memory: each fence owns an 8-byte shared buffer that the listener updates when the event reaches a value, so the title can poll it. GPU-ordered alternative (later): a blit copy of the value into that buffer before the signal. | done (CPU mirror) |
| F05 | Allocators and lists (581 created and closed at startup) | own IR per list (`src/mtl_internal.h`), encoded at `execute`; allocators are a D3D12-layer pool, no Metal object; closing a list costs nothing in Metal | done |
| F06 | Shader cache session, pipeline library | `MTLBinaryArchive` keyed by translator version, GPU, OS and process arch (3.10; later `MTL4PipelineDataSetSerializer`); the D3D12 session calls stay no-ops. **measured**: 4 real pairs 0.461 s cold, 0.0011-0.0020 s from the archive (STATUS item 5) | done (archive); MSL still compiles at run time |
| F07 | XMem pages, placeholders | S1.1 (os/), not GPU | n/a |
| F08 | GPU address model for title memory | **Arena**: a placement `MTLHeap` of the arena size (`HeapDesc::console_arena`) with one `MTLBuffer` placed over all of it; `heap_base` = that buffer's `gpuAddress`, console VA = arena VA base + offset, placed resources at title VAs are placed at the same offset. **measured** (both arches): a buffer placed at offset X in the heap has `gpuAddress` = arena `gpuAddress` + X and aliases its bytes for CPU and GPU, so leonida's arena model (GPU address = heap_base + console VA offset) holds on Metal. With `MTL_SHADER_VALIDATION=1` the top bits of every `gpuAddress` carry a per-allocation tag (arena 0x2100..., placed 0x4100...) and only bits 0-43 keep the relation; shader arithmetic from `heap_base` still addresses the arena buffer itself, which aliases the placed resource. Title pages outside the arena: `newBufferWithBytesNoCopy` windows (S0.5 measured up to 8 GiB, never at the CPU address), as leonida's `title_va_alias.h`. `Device::resolve_va` maps console VAs to (buffer, offset) for vertex/index views. Residency: memory reached only through `heap_base` arithmetic is never bound, so the arena heap is in the device residency set (3.5). **measured** (`tests/test_hazards` `heap_base`, both arches, with and without validation): a pixel shader reads `*(heap_base + 0x1000)` of a shared arena and `*(heap_base + 0x2000)` of a device-local arena, neither of them in any table, and a committed buffer by its address alone; without the device set the shared arena reads zeros. Size limit: one `MTLBuffer` over the whole arena is bounded by `maxBufferLength`, measured 13,639 MiB here, so the arena cannot cover the 13,824 MiB (13.5 GiB) Series X title memory. The 7.6 GiB profile that SCOPE/NOTES default to fits. Larger needs several arena buffers with `heap_base` per range, which is not designed yet. | arena + VA space + residency done; round 2: bytesNoCopy windows done (section 8) |
| F09 | Heaps, committed and placed resources, allocation info, slots 76/127/133 | `MTLHeapTypePlacement` heaps; `newBufferWithLength:options:offset:` / `newTextureWithDescriptor:offset:`; sizes and alignments from `heapBufferSizeAndAlignWithLength:` / `heapTextureSizeAndAlignWithDescriptor:`. **measured** (review probe F, re-run on both arches): buffer alignment 256 B for buffers of 256 B to 1 MiB, private and shared; RGBA8 textures align to 128 B (256x256: 264,192 B; 1024x1024: 4,227,072 B). These are not page granularities, and D3D12's 64 KiB placement offsets are multiples of them. Committed resources are device allocations. Copyable footprints are the D3D12 layer's arithmetic (linear layout, 256-byte pitch). | done (footprints in layer) |
| F10 | Descriptor heaps as addresses, 32 B / 16 B console descriptors, CopyDescriptors, title-written descriptors | The console heap stays title memory owned by the D3D12 layer (it parses V#/T#/S# and keeps the hash table). The host side is `DescriptorHeap`: one shared `MTLBuffer` per contract binding of 8-byte entries, tier 2: texture `gpuResourceID`, sampler `gpuResourceID` (samplers made with `supportArgumentBuffers`), buffer `gpuAddress + offset`. `copy` is a memcpy of entries plus the kept references. **measured**: argument buffers tier 2 on this GPU; a texture sampled through such a table draws correctly (tests/test_triangle). | done |
| F40 | Reserved resources | sparse placement heaps (`supportsPlacementSparse` measured 1 on this GPU); slice 1 returns `unsupported`, which the title tolerates at P0 | stub |

### P0b: pipeline storm

| ID | Feature | Metal mapping | slice 1 |
|---|---|---|---|
| F12 | Root signature, root constants/views/tables, static samplers | lowered by the D3D12 layer to the root contract (2.2). Metal side: push block by `set*Bytes` (144 B, below the 4 KiB limit), tables by `set*Buffer`. Static samplers are S# literals pre-registered in the sampler table. Two Metal gaps for samplers: no LOD bias field (`SamplerDesc::mip_lod_bias` must be applied by the shader with `bias()`), and border colour only transparent black / opaque black / opaque white (`Sampler::border_exact` reports a rounded one). | done (no LOD bias) |
| F13 | Graphics pipelines from state streams, lazily | `CompileMode::lazy` builds the Metal pipeline at the first draw that binds it; `async` starts `newRenderPipelineStateWithDescriptor:options:completionHandler:` at creation and the first draw waits; `eager` blocks. The state-stream parser (console subobject numbering, mesh 24) is the D3D12 layer's. Rasterizer state that Metal keeps on the encoder (cull, winding, fill, depth bias, depth clip) is stored in the pipeline and applied at bind; depth-stencil state is a cached `MTLDepthStencilState`. **measured**: lazy pipelines are not compiled until drawn (stats). | done; round 2: async compile queue with priorities, `NotReady::skip`, binary archive (3.10) |
| F14 | Compute pipelines (BVH builder at ~20 s) | `newComputePipelineStateWithFunction:` with the same compile modes and binding plan (3.7, 3.10) | done (round 2); the XDXR builder kernels are not ported |

### P1: launch movie

| ID | Feature | Metal mapping | slice 1 |
|---|---|---|---|
| F11 | Shader-side descriptor lookup (hash table, bindless arrays) | SPIRV-Cross `--msl-argument-buffers` (MSL 3.0). Each set-0 binding must be its own argument buffer (section 6: the unmodified recipe collapses the bindings); the hash table is a buffer entry (`gpuAddress`) of table 2. Residency of everything a table names through its `MTLResidencySet`. | done for the test shaders; game shaders: PSO + reflection only |
| F15 | Shader corpus to MSL | S5.2 (Route A). The backend accepts MSL source (`newLibraryWithSource:` with `MTLLanguageVersion3_0`) and metallibs. | done |
| F18 | Draw, copy, clear, viewport, scissor, vertex/index buffers | render/blit encoders; vertex buffers by console VA through `resolve_va`; index buffers likewise (offset alignment 2/4 bytes) | draw, draw_indexed, copies, clears done; dispatch no |
| F19 | Barriers (legacy and enhanced) | Layout transitions have no Metal meaning; the ordering does. Every barrier ends the current encoder, flushes pending clears and makes the next encoder join (3.3): it waits for every encoder of the queue since the last join, whether the resource is committed or placed. The exception is a transition whose `before` and `after` are both read-only states, which is a no-op. The earlier rule "committed: no-op, Metal tracks it" was wrong for table reads: the review measured 13-18 stale reads in 400, and STATUS has the numbers. UAV barriers inside a render pass would be `memoryBarrierWithScope:afterStages:beforeStages:`. Counts: `DeviceStats` `barriers_noop`, `barriers_fenced`, `fence_joins`, `target_joins`. **measured** (`tests/test_hazards`, 400 iterations each, both arches, also under validation): 0 stale reads for a render target written and then sampled through a table, in four cases: committed with a barrier; committed in two `execute` calls without a barrier; placed with a barrier; placed with another pass between the write and the barrier. | done |
| F20 | PresentX single plane, DXGI swapchain | `Swapchain` owns the title's back buffers (any DXGI format); `present` renders a compose pass (full-screen triangle, nearest sampling) from the current back buffer into `CAMetalLayer.nextDrawable` (BGRA8) and `presentDrawable:`. **measured**: a drawn frame's drawable equals the back buffer at every checked pixel; cleared frames match the clear colour within ±1 per channel. | done |
| F21 | Frame events and pacing (78-81) | `presentDrawable:afterMinimumDuration:` for sync interval 2+, `addPresentedHandler:` for presented times; frame events from a `CADisplayLink` (from `NSScreen`, macOS 14) on its own thread, a timer at the display rate without a screen. **measured** (120 Hz): interval 1 median 8.333 ms, interval 2 16.667 ms, frame events 8.333-8.342 ms | done (round 2) |
| F22 | Present planes: 2-plane compose, scaling, HDR | the same compose pass with up to 8 planes (plane 0 opaque, later planes premultiplied or straight alpha over it, src and dst rectangles, point or linear); the three compose pipelines are built at `create_swapchain`. EDR via `CAMetalLayer.wantsExtendedDynamicRangeContent` later. **measured**: two planes 0 of 230,400 pixels off by more than 1 | done (round 2), HDR stub |
| F23 | Slot-133 placed textures, R8 planes, streamed images | placed textures in the arena heap; staged uploads by blit (`copy_buffer_to_texture`) | placed textures + blit upload done; title-memory refresh no |
| F24 | Console tiled layout, detile | D3D12-layer CPU detile into staging (as leonida) or a compute detile kernel | n/a here |
| F25 | Rectangle lists | section 5.1 | prototype done |
| F27 | write32/64, wait32/64 in the command stream | section 5.2 | done |
| F35 | wave64 on 32 lanes | section 5.5 | n/a (shader side) |

### P2: first-run pages

| ID | Feature | Metal mapping | slice 1 |
|---|---|---|---|
| F16 | Blend (incl. dual source), rasterizer, depth-stencil, input layout, MRT | `MTLRenderPipelineColorAttachmentDescriptor` per target (`MTLBlendFactorSource1Color` etc. for dual source), write masks; `MTLVertexDescriptor` from the input layout (location = D3D input register, per-instance step rate); MRT up to 8 | blend, input layout, MRT formats done; dual source untested |
| F17 | D24S8, B4G4R4A4 target, BC, console formats | D24S8: section 5.3. B4G4R4A4 target: `MTLPixelFormatABGR4Unorm` renders on Apple GPUs; component order needs a swizzle check (`MTLTextureSwizzle`) before use. BC1-7 direct. | D24S8 storage + quantised clear done; round 2: D24 fragment depth (5.3), B4G4R4A4 sampling exact through a swizzle view (render target stub), A8_UNORM native as a render target (measured at 3840x2160) |
| F26 | Quad lists | index-buffer expansion (6 indices per quad, leonida `quad_index_expand`) in a hoisted compute pre-pass (3.6), one shared pattern buffer for non-indexed draws | done (round 2); indirect quad draws refused |
| F36 | Barycentrics, PerVertex inputs | `[[barycentric_coord]]` (measured supported in S0.5); SPIRV-Cross path unproven | n/a (shader side) |

### P3 and P4 rows taken in round 2

| ID | Feature | Metal mapping | round 2 |
|---|---|---|---|
| F28 | Queries: occlusion, timestamp, pipeline statistics | 3.8 | occlusion, binary, timestamp done; statistics zeros |
| F31 | Compute dispatch, UAV, storage images, UAV clears | 3.7; storage images through the tables | dispatch done; UAV clears stub |
| F32 | Mesh shaders | 3.9 | done, with the limit refusal for mesh-as-compute |
| F33 | ExecuteIndirect (draw, indexed, dispatch, mesh; type 105 = dispatch) | `max_count` Metal indirect commands over the records, a hoisted count pre-pass (3.6) | done for leonida's IR shapes; root-changing signatures and indirect quad/rect draws stub |

## 5. Items with no Metal equivalent, and the emulation chosen

### 5.1 Rectangle-list expansion without a geometry stage (F25)

Rule (both upstreams agree): three corners per rectangle; the right angle `vc` is the corner opposite the longest
side (ties, NaN, degenerate: the second corner); the fourth corner is `va + vb - vc` for the position and for every
output; it is made **after** the vertex stage (leonida `rect_list_geometry.h`: an input-side expansion is wrong for
any shader not linear in its inputs, and the title's own blit `VS_Blit_6e5520d1` puts the corner at the second
vertex). Triangles keep the first vertex and the winding of `(v1, v2, v3)`: corner at v1: `(v1,v2,v4)(v1,v4,v3)`;
at v2: `(v1,v2,v3)(v1,v3,v4)`; at v3: `(v1,v2,v3)(v1,v4,v2)`.

Chosen emulation: **vertex-stage expansion** (`RectMode::vertex_expand`, `src/rect_list.cpp`
`generate_rect_wrapper`). The SPIRV-Cross vertex function is rewritten into a callable `..._rect_body` (attributes
stripped), and a generated wrapper vertex function is drawn with 6 vertices per rectangle: vertex `6r + k` pulls the
three corners' inputs itself from the bound vertex buffers (formats from the input layout; indexed draws read the
index buffer), calls the body three times, picks the right angle from the three output positions, and returns
corner `k` of the expansion, computing `va + vb - vc` member by member for float outputs (integer outputs take
`va`). Exact after the vertex stage, no extra pass, no CPU work; cost 3 vertex-function evaluations per emitted
vertex, which for blits and tiled-light quads is negligible. Limits: the rewrite parses SPIRV-Cross's output shape
(`main0_in` / `main0_out`, one `[[position]]`); vertex inputs in formats the generator does not decode are refused
(listed in STATUS.md); a pixel shader reading `[[vertex_id]]`-derived values sees the expanded ids.

Fallback: **CPU rewrite** (`RectMode::cpu_rewrite`, `expand_rect_vertices`, ported from VIctory `primitive.rs`):
the fourth vertex is computed from vertex inputs and the draw becomes an indexed triangle list (index buffer
rewrite, 6 indices per rectangle over 4 vertices). Exact only for vertex shaders linear in their inputs; used where
the wrapper is refused.

Rejected: a mesh-shader variant (needs the VS as a mesh function; mesh output budget problems of S0.5 4.3), a
compute pre-pass (needs VS-as-compute capture of every output plus a pass-through VS; more passes). **measured**:
both implemented modes reproduce a reference blit bit-exact (one full-screen rectangle; two indexed rectangles with
the corner at the first and third vertex), for a vertex shader not linear in its inputs only vertex_expand matches
the hardware rule (cpu_rewrite: 496 of 1,024 pixels differ), and the wrapper also builds a pipeline for the real
`VS_Blit_fad9fd10` Route A MSL (not executed on data).

### 5.2 Command-stream write64 / wait64 (F27)

- **write32/write64**: in stream order on the GPU. `write_value` records the value; encoding stores it in a
  per-execute constants buffer and emits a blit `copyFromBuffer:` of 4/8 bytes into the target. Exact and ordered with
  the surrounding work (blit encoder boundaries). The timestamp-flag form (flag 1) maps to counter sampling at stage
  boundaries (only boundary sampling is supported on this GPU, S0.5 N6) or MTL4 `writeTimestampIntoHeap`: stub.
- **wait32/wait64**: no GPU wait on memory exists. Encoding splits the command buffer at the wait: everything
  before is committed; the next command buffer starts with `encodeWaitForEvent:` on a gate `MTLSharedEvent`; a CPU
  pump thread polls the value (unified memory: the target buffer is shared storage) and signals the gate when the
  compare holds. This is leonida's design (queue holds + pump) with the gate expressed as an event. **measured**: a
  wait released by a CPU store and by a write on another queue orders a later copy correctly (tests/test_device).
  - Every command buffer the split produces gets the residency sets of the tables bound in it, plus the device set
    through the queue (3.5). The review measured that the draws before a wait ran without residency when only the
    last command buffer got the sets. **measured**: bindless draws before and after a satisfied wait are correct
    under validation with the table sets as the only residency (`tests/test_hazards` `wait_split`).
  - The encoder after the wait joins (3.3).
  - **A held queue does block the submitter after 255 command buffers.** Queues are created with
    `newCommandQueueWithMaxCommandBufferCount:256`, and every `signal` is a command buffer. **measured** (review
    probe D, re-run after the fixes, arm64): behind a `wait_value` that is never met, 255 of 600 `signal()` calls
    on another thread return within 2 s, and the rest block until the value is stored. A title thread blocked there
    cannot submit the write that releases the hold. Not fixed. Planned: a `signal` becomes a timeline listener with
    no command buffer of its own, and executes behind a hold are queued by a submit thread (STATUS next steps).

### 5.3 D24_UNORM_S8_UINT (F17)

Never supported on Apple silicon (S0.5 R6: creating one aborts in validation). Stored as
`MTLPixelFormatDepth32Float_Stencil8` (`FormatInfo::emulated`). D3D12 semantics kept as leonida does
(`depth24_fragment.h`): clear values are quantised to the D24 code (`round(d * (2^24 - 1)) / (2^24 - 1)`, done in
slice 1, with leonida's float product since round 2); fragment depth is quantised and the D24 fixed-point depth
bias applied in the pixel stage (round 2, `D24Mode::fragment`): `src/msl_rewrite.cpp` rewrites the SPIRV-Cross
fragment entry into a wrapper that calls it and writes `[[depth(any)]] = quantise(z + DepthBias * r + slope term)`,
with the rasterizer bias off; a depth-only pipeline gets a generated fragment function. Fallback to the rasterizer
bias (logged) when the pixel shader has no MSL source, uses `[[early_fragment_tests]]` or cannot be parsed. Copies
between a D24 footprint and the texture convert through a compute pass (stub). **measured**: a D24S8 texture is
created as D32F_S8 and a cleared depth reads back as the quantised value; in fragment mode an EQUAL draw at the
cleared 0.3 passes on 4,096 of 4,096 pixels (0 in rasterizer mode), and the GPU quantiser equals leonida's CPU one on
all 16,777,216 codes.

### 5.4 GPU VA is not the CPU VA (F08)

No Metal API lets the host pick a buffer's GPU address, and `bytesNoCopy` buffers never get the CPU address
(S0.5). Neither upstream needs that: leonida makes GPU address = `heap_base` + console VA offset inside one arena,
VIctory keeps shadow copies plus a page table. On Metal the arena is a placement heap with a buffer over all of it
(measured, F08 row): console VAs inside it translate by one add in the shader (`heap_base` from the push block),
on the host by `Device::resolve_va`. Title memory outside the arena is aliased by `bytesNoCopy` windows (16 KiB
page granularity) or copied, with write tracking: that is the D3D12 layer's `title_va_alias.h` unchanged, with the
Metal window buffer in place of the imported `VkBuffer`.

### 5.5 wave64 on 32-wide SIMD (F35)

Nothing on Apple gives 64 lanes (every compute and mesh PSO reports width 32 in S0.4; measured again here: the
device's probe kernel reads `[[threads_per_simdgroup]]` = 32 on both arches, STATUS.md). Shader-side decision (S5.2): compute and mesh shaders
use leonida's `--wave64-emulate` (exact); VS/PS of partition-invariance tier A/B are exact on 32 lanes, tier C
(7,082 of 26,296 at the reviewed snapshot) run approximately and are logged once per shader. The backend's part is
to report the width (`DeviceCaps::simd_width`) and never to promise a required subgroup size.

### 5.6 Vertex lane id

Metal rejects `[[thread_index_in_simdgroup]]` in vertex functions (S0.4, lane-probe); 1,252 vertex shaders fail
SPIRV-Cross for it in the s5.2 corpus run (`results.json` step 1). Two lowerings, chosen per shader by S5.2:
(a) `simd_prefix_exclusive_sum(1)` as the lane id inside the vertex function, exact only for full SIMD groups
(S0.4 REVIEW finding 3); (b) **vertex shader as compute**: the s5.2 `vs_as_cs` variant (1,252 of 1,252 cross and
compile as kernels) runs the vertex shader as a compute kernel over the draw's vertices, where the lane id exists,
writes its outputs to a buffer, and a pass-through vertex function feeds the rasteriser. (b) is exact and costs a
pass plus a buffer per draw; the backend side is a compute dispatch before the render pass plus the pass-through
pipeline, which needs compute support in gpu.h (v2). Neither is implemented in slice 1.

### 5.7 The other SCOPE section 8.1 items (no Metal equivalent, fidelity or coverage loss)

None is on the P0-P2 path; the chosen behaviour is SCOPE's fallback, made explicit so the backend never fakes it.

| ID | Item | Chosen behaviour (designed; slice 1 implements none of it) |
|---|---|---|
| F34 | Geometry stage (195 `GS_VoxelizeGeneric`) | `RenderPipelineDesc` has no geometry stage, so the D3D12 layer gets `unsupported` for a GS pipeline, logs it once per shader and skips the voxelisation pass |
| F38 | float64 (5 CS) | refused at library creation; Metal itself rejects the MSL ("'double' is not supported", s5.2 step 2) |
| F38 | CullDistance (2 VS) | not lowered; refuse the pipeline when the VS writes it |
| F38 | logic ops, conservative rasterisation, stream output | not representable in `RenderPipelineDesc`; the D3D12 layer gets `unsupported` |
| F38 | depth bounds | ignored, as VIctory does (`list.rs:1775`) |
| F38 | pipeline-statistics queries | not in gpu.h v1; when queries arrive, statistics resolve to zeros and are logged |
| F42 | predication | not in gpu.h v1; the D3D12 layer runs the work unconditionally (logged) |
| F41 | XDXR BVH walked by title shaders | stays a compute workload over arena memory (needs compute in gpu.h v2); no `MTLAccelerationStructure` |

These are listed in STATUS.md as stubs.

## 6. Shader input: Route A MSL and the binding plan

Route A recipe (S0.4, s5.2 `results.json`): `spirv-cross --msl --msl-version 30000 --msl-argument-buffers
--msl-argument-buffer-tier 1 --msl-device-argument-buffer 0`, then `xcrun metal -std=metal3.0` (or runtime
`newLibraryWithSource:`). `tools/route_a_msl.py` reproduces it on SPIR-V files from
`spike/s2-make-game/work/shaders/spirv` into the build tree (no game data is copied into the source tree).

Measured on four real pairs (`tests/test_game_shaders`, both arches; STATUS.md has the numbers): every base MSL
is byte-identical to the s5.2 step 1 output for the same shader; all base and split pipelines build; the
unmodified recipe puts **all set-0 bindings of a
shader into one argument buffer `spvDescriptorSet0` and gives the unsized arrays overlapping ids**: in
`PS_BinkNoAlpha_e9aa19fd` the hash-table pointer (binding 2) is the member at offset 0, the 2D-image and sampler
arrays (bindings 0, 1) and the uint/int 3D-image declarations of binding 6 are declared overlapping offset 0, and
the float 3D-image declaration of binding 6 is a member at offset 8; indexing image `i` reads offset `8 i`. A host heap
cannot be laid out to satisfy that, so the recipe as-is is not bindable for bindless use (it compiles and builds
pipelines, which is all S0.4/s5.2 checked). Pairing caveat, measured: a shared hash suffix does not make a VS/PS a
pair: `VTexTGCxform_732599dc` + `CFTexTGBlipColorAndEffectCxformEAlphaDepthOccluded_732599dc` fails to link
(fragment input `user(locn3)` not written by the vertex function), so real pairs need the title's PSO streams.

Fix proposed for S5.2 and implemented in `tools/route_a_msl.py --split`: before SPIRV-Cross, move set-0 binding `b`
to descriptor set `1 + b` (an edit of `OpDecorate DescriptorSet/Binding` literals; nothing else changes). SPIRV-Cross
then emits one argument buffer per binding, `spvDescriptorSet{1+b} [[buffer(1+b)]]`, each a flat array of 8-byte
entries at offset 0, with typed declarations of one binding still aliasing (which is the intended mutable-descriptor
aliasing). That is exactly the `DescriptorHeap` layout, and the binding plan maps it from reflection without
per-shader tables (measured: for all four pairs the reflected tables equal the contract bindings `index.json`
lists for the pixel shader, e.g. 0, 1, 2, 6 for `PS_BinkNoAlpha_e9aa19fd`, with no mismatch). Bindings 0..28 need
buffer indices 1..29 plus the push block; vertex buffers take indices 30 downward; a shader that collides is
reported in `PipelineReflection::mismatches`.

## 7. Open items and risks (no measurement yet)

- Metal 4 command layer: not called; MTL4 availability in an x86_64 process is unmeasured.
- Predication, UAV clears, resolves, texture-to-texture copies: not in gpu.h v2 (compute, indirect, mesh and queries
  came in v2).
- MTLBinaryArchive and the 54k-pipeline storm: the archive and the compile queue are measured on 4 real pairs only;
  nothing measures archive size, load time or queue behaviour at 53,967 creations.
- Residency-set cost with ~38,000 streamed images (P3) is unmeasured, in one device-wide set (one add and one
  remove per resource, a commit before the next command buffer) and in the per-table sets.
- The join model (3.3) serialises every encoder after a barrier against all work since the previous barrier, and
  every `execute` against the previous one on its queue. Overlap between independent passes is only kept between
  barriers. The cost on real frames is unmeasured.
- The untracked render-target join (3.3) is not shown to be needed: with it disabled
  (`GTA6RT_METAL_NO_TARGET_JOIN=1`) `placed_target_order` read 0 wrong of 400. It is kept because Metal gives
  untracked attachments no ordering.
- The vertex-expand wrapper depends on SPIRV-Cross's output naming (`main0_in`, `main0_out`, `[[position]]`); a
  SPIRV-Cross upgrade can break it, so the test builds it for real corpus VS every run.

## 8. Title memory in place, read-back, memory accounting and pressure (round 2)

**Host windows** (`Device::create_host_buffer`, `src/mtl_memory.mm`). The title's graphics memory outside the arena
(the 185 MB below-4-GiB heap with its 174 MB constants ring, slot-133 buffers) is wrapped where it lies:
`newBufferWithBytesNoCopy:length:options:deallocator:nil` over a host alias of the title's pages, shared storage,
`MTLResourceHazardTrackingModeUntracked`. The pages stay owned by the caller (no deallocator) and must outlive every
command buffer that used the window. Pointer and size must be multiples of the process page size (16 KiB arm64,
4 KiB x86_64; Metal itself accepted a 4 KiB-offset pointer on arm64 in a probe, the backend requires page alignment
anyway). The window is registered in the console VA space at `HostBufferDesc::va` (the title VA) so `resolve_va` maps
title addresses into it, is in the device residency set from creation (3.5: it is reached by address, never bound),
and is counted as `host_window`. Being untracked, it is ordered only by barriers (3.3), the rule the round-1 review
established for placed resources. The GPU therefore reads constants where the guest writes them and writes results
the guest reads after the fence. The alias is made by the caller (`vm_remap` of the xmem pages in the test); in an
x86_64 process linked with a small `-pagezero_size` the heap itself can sit at its below-4-GiB title VA (arm64
processes with that flag are killed at launch). Measured: STATUS item 4.

**Buffer textures** (`create_buffer_texture`): a linear texture over buffer bytes (`newTextureWithDescriptor:offset:
bytesPerRow:`), untracked, row pitch a multiple of `minimumLinearTextureAlignmentForPixelFormat:` (16 B for R32F
here). Over a host window this is the GPU->CPU read-back path for textures the title's CPU code reads (the ocean
height and displacement maps): the compute pass writes the texture, the CPU reads the same pages after the fence, no
copy. The copy path (`copy_texture_to_buffer` into a readback buffer) remains for device-local textures.

**Accounting.** A ledger of `allocatedSize` and object counts per `MemoryCategory` (buffer, texture, heap,
descriptor_table, host_window, internal, swapchain), added at creation and removed at destruction; `internal` holds
the backend's own allocations (query result buffers, per-execute scratch until its command buffer completes; the
small per-execute value-write constants and timestamp sample buffers are not counted). `memory_budget()` returns the ledger with `recommendedMaxWorkingSetSize`, `currentAllocatedSize`,
`phys_footprint` (`task_info TASK_VM_INFO`), physical memory and the last pressure level. Measured: the ledger is
exact for every category, `currentAllocatedSize` also counts bytesNoCopy windows, a call costs 1.2-1.8 us.

**Pressure.** A `DISPATCH_SOURCE_TYPE_MEMORYPRESSURE` source (normal, warn, critical) on a backend dispatch queue
calls every handler from `add_memory_pressure_handler` with the level; `simulate_memory_pressure` drives the same path
for tests. The backend frees nothing on its own: the host or the D3D12 layer is the governor (drop cached textures,
shrink staging, defer backing), which is the shape the fork assessments call for with 24 GiB of RAM. The real source
has not fired in a test (levels were simulated); a system-wide simulated warn would also reach the other processes on
this machine, so it was not triggered.
