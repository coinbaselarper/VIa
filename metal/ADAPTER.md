# ADAPTER: replaying leonida's D3D12 layer onto `gta6rt::gpu` instead of Vulkan (brief for round 3)

Read from `port/d3d12_vulkan` on 2026-11-26 at about 11:30 (read only; nothing there was edited). The integrator is
editing that tree (`d3d12_vulkan_device.cpp` and `d3d12_com.cpp` grew by 172 and 19 lines since round 2 first wrote
this plan), so every row names the function or symbol first; the line is where it was at this read. Interface:
`include/gta6rt_metal/gpu.h` api_version 2. What the backend does and has measured is in `STATUS.md`, why in
`DESIGN.md`. Every "gap" below is in STATUS.md's stub list by the same name.

## 0. Where the cut goes

Counts by grep (`\bvk[A-Z]\w*\(` calls, lines naming a `Vk[A-Z]\w*` type, `context(->|_.|.)\w+\(` calls):

| file | lines | `vk*(` calls | `Vk*` lines | context calls | role in the Metal build |
|---|---:|---:|---:|---:|---|
| `shims/d3d12_xs_device.cpp` (console ABI, slots, list cuts) | 6,693 | 0 | 0 | 2 | unchanged |
| `shims/d3d12_xs_list_values.h` (write/wait pump, gates) | 1,314 | 0 | 0 | 0 | unchanged |
| `shims/xbox_present.cpp` (PresentX decode, frame events, pacer) | 408 | 0 | 0 | 0 | unchanged above `presenter_` |
| `shims/d3d12_com.cpp` (list recording, queue, fence COM) | 2,871 | 13 | 23 | 34 | queue and fence bodies change; list recording unchanged |
| `shims/dxgi_com.cpp` (DXGI factory, swapchain) | 1,702 | 0 | 38 | 5 | swapchain body changes |
| `shims/d3d12_vulkan_device.cpp` (ID3D12Device, resources, views, PSOs) | 18,962 | 155 | 660 | 121 | creation bodies re-implemented |
| `src/vulkan_replay.h` (IR -> Vulkan commands through its dispatch table) | 5,486 | 0 | 502 | 0 | replaced by `metal_replay` (section 4) |
| `src/vulkan_backend.h` (`VulkanContext`, `VulkanQueue`, `VulkanFence`) | 4,165 | 119 | 316 | 119 | replaced by `MetalContext` (section 1) |

The console layer above `d3d12_com.cpp` makes no Vulkan call, and the list IR (`src/command_list.h`, `using Command =
std::variant<...>` at :705, 29 alternatives) is backend-neutral apart from `RtTriangleGeometry::vertex_format`
(a `VkFormat` number). Kept exactly as they are: the IR, the xs/console proxies, and the D3D12 semantics that own no
GPU object: `QueueHolds` (`src/queue_holds.h:37`), the list-value pump, `src/title_va_alias.h` (and its memory probe,
`platform/title_memory_probe.cpp`), `src/title_page_watch.h`, `src/frame_pacer.h`, the descriptor hash table
(`src/descriptor_table.h`), footprint arithmetic, `src/depth24.h`. What changes: (a) who creates GPU objects, (b) who
replays the IR, (c) the queue, fence and presenter bodies.

Proposed module: `port/d3d12_metal` (owned by the D3D12 worker; links `gta6rt_metal` and the d3d12_vulkan sources
that compile without Vulkan headers). `d3d12_vulkan_device.cpp` is too Vulkan-bound to share by `#ifdef`; its
creation methods are re-implemented by the same names in `d3d12_metal/shims/d3d12_metal_device.cpp`, and everything
it does that is not a GPU-object call (descriptor parsing, state-stream parsing, footprints, CheckFeatureSupport
tables, title-address resolution) moves verbatim into headers both devices include. That extraction is the first task
and needs the d3d12_vulkan owner.

## 1. Objects

| leonida today | Metal adapter | notes |
|---|---|---|
| `VulkanContext` (`src/vulkan_backend.h:1079`): resources by `ResourceId`, buffer-VA index, arena `set_heap_base`, pipelines by id, query pools, bindless hooks, deferred deletion, fence dispatcher | `MetalContext`: `unordered_map<ResourceId, Entry{gpu::Resource*, format, flags}>`, the arena `gpu::Heap`, 29 `gpu::DescriptorHeap`s, pipelines by id (`gpu::Pipeline*` + `code_base`), query heaps by id, `QueueHolds<gpu::Fence*>` | deferred deletion goes away (`unique_ptr` plus the backend's completion-handler retention); the IR pins stay (`pin_lists`, `d3d12_com.cpp:2199`; `PinFn`, `vulkan_backend.h:2532`) because the IR holds raw pointers |
| `VulkanQueue` (`vulkan_backend.h:2359`): `execute(lists, pin)` (:2534), `signal` (:2566), `wait`, `marker`, `update_tiles`, `copy_tile_mappings`, `idle` | `MetalQueueAdapter`: `gpu::Queue` + QueueHolds holder + `metal_replay` | QueueHolds keeps held work in the layer, so the backend never gets an `execute` behind an unmet wait; this is how the 255-command-buffer block (STATUS gap "held queues") is avoided |
| `VulkanFence` (:2260), timeline semaphore | `gpu::Fence` (`MTLSharedEvent` + slot-12 mirror) | |
| `FenceWaitDispatcher` (:680) | `gpu::Fence::on_completion` | SetEvent stays in the layer |
| `ResourceBinding` (`src/vulkan_replay.h:75`) | `gpu::Buffer` / `gpu::Texture` + the flags replay needs (`depth24`, `presentable`, `rgb96`) | `depth24_as_float` becomes `FormatInfo::emulated` |
| `QueryPool` (:847), `ComputePipeline` (:860), `GraphicsPipeline` (:880) | `gpu::QueryHeap`, `gpu::ComputePipeline`, `gpu::RenderPipeline` / `gpu::MeshPipeline` | `async_build`/`built` become `CompileMode::async` + `NotReady::skip` |
| bindless set (`src/vulkan_bindless.h`, `binding_count = 29` at :94, default counts in its header comment) | 29 `gpu::DescriptorHeap`s, one per binding (split recipe), counts from `BindlessFeatures::counts` | index 0 of every array is a fallback (a 1x1 zero image, a nearest clamp sampler, 1x1 storage images that never share an image with the sampled ones): write those objects into slot 0 and wherever the layer clears a slot. `DescriptorHeap::clear` writes a null handle, which a shader must never read |
| `WindowPresenter` (`shims/d3d12_com.h:444`) over `VulkanPresenter` (`src/vulkan_present.h`) | the same class over `gpu::Swapchain` + `Queue::present_planes` | `xbox_present.cpp` and `dxgi_com.cpp` call only `WindowPresenter`, so its body is the cut |

## 2. Device calls (`shims/d3d12_vulkan_device.cpp`)

| D3D12 method (line) | today | Metal adapter |
|---|---|---|
| `CheckFeatureSupport` (:15819) | Vulkan device feature chain | `gta6rt::metal::create_device()` once; answers from `caps()`: wave lane count 32 (`simd_width`), mesh shader tier 1 (with the refusal fallback below), raytracing none (XDXR compute only), resource binding tier 3, `MaxGPUVirtualAddressBitsPerResource` from the arena size |
| `CreateCommandQueue` (:13525) | `VulkanQueue` | `create_queue(direct/compute/copy, label)`, one per D3D12 queue (5 at startup) |
| `CreateFence` (:13632) | `VulkanFence` | `create_fence(initial)`; the claim logic of `d3d12_com.cpp` `Fence::claim` (:2099, `claim_locked` :2168) stays (Metal events also only increase) |
| `CreateCommandAllocator` / `CreateCommandList` / `CreateCommandList1` (:13593 / :13598 / :13610) | IR list | unchanged; no backend object until execute |
| `CreateHeap` (:15475) | arena range or own `VkDeviceMemory` | default/upload/readback heaps become ranges of the one arena (`create_heap{console_arena}` once at device creation, size from `arena_size.h` `decide_arena_size` capped by `caps().max_buffer_length`, 13,639 MiB measured); `ALLOW_ONLY_RT_DS_TEXTURES` heaps get their own placement `create_heap` |
| `CreateCommittedResource` / `1` / `2` / `3` (:14973, :15052, :15062, :15075) | `vkCreateBuffer/Image` + memory + `add_resource` (:5806, :5887, :6081, :6131, :6183, :6290; images :6613, :6715, :6720, :6770) | buffers: `create_placed_buffer(arena, va - arena_va, ...)` so console VA = arena offset; textures: `create_texture` or `create_placed_texture` when aliasable; `MetalContext::add_resource(id, entry)` |
| `CreatePlacedResource` / `1` / `2` (:15501, :15510, :15524) | bind into heap memory | `create_placed_buffer/texture(heap, offset)`; D3D12's 64 KiB offsets are multiples of Metal's alignments (128-256 B measured) |
| `CreateReservedResource` / `1` / `2` (:14986, :14993, :15006) | sparse | gap: return `E_NOTIMPL` (tolerated at P0) |
| `GetResourceAllocationInfo` / `1` (:15778 / :15786), `GetCopyableFootprints` / `1` (:15676 / :15114) | layer arithmetic + memory requirements | `texture_allocation_info` / `buffer_allocation_info`; footprints unchanged (256-byte pitch) |
| `OpenExistingHeapFromAddress` (:15138), device slot 133 | import title pages | `create_host_buffer({title alias, size, va = title VA})` over a `vm_remap` alias of the xmem pages (measured, STATUS item 4); x86_64 only: the 185 MB 32-bit heap at its below-4-GiB VA |
| `CreateDescriptorHeap` (:13832) | CPU-side console heap | unchanged (title memory owned by the layer); the shader-visible side is the 29 tables, written by the hash-table publish below |
| `CreateShaderResourceView` (:14503), `CreateUnorderedAccessView` (:14736), `CreateRenderTargetView` (:14139), `CreateDepthStencilView` (:14286), `CreateSampler` / `CreateSampler2` (:14841 / :14855) | `VkImageView`s, `VkSampler`s | SRV/UAV: `DescriptorHeap::write_texture(binding_of(dimension, class), index, texture)`; samplers: `create_sampler` + `write_sampler(1, index)` (LOD bias: gap); RTV/DSV: a `TargetView` kept in the layer's CPU descriptor (`TargetView::format`: gap, views use the texture's format) |
| `CopyDescriptors` / `CopyDescriptorsSimple` (:14324 / :14346) | CPU copies | unchanged (console heap is CPU memory); tables follow through the hash-table publish |
| `set_bindless_prepare` / `set_bindless_commit` hooks (:11614 / :11697) | write the descriptor set before submit | same hooks write `gpu::DescriptorHeap` entries; binding 2 (hash table) = `write_buffer(2, 0, hash_table_buffer, 0)` |
| `set_heap_base(arena.address, arena.size)` (:11846) | push constant dwords 32-33 | `CommandList::set_heap_base(arena->host_gpu_address())` at the start of every replayed list |
| `CreateRootSignature` (:13875) | lowered to user data | unchanged; the backend sees one shared `RootLayoutDesc{144, 32, tables 0..28, split_sets}` |
| `CreateGraphicsPipelineState` (:13984), `CreatePipelineState` stream (:14080), `add_graphics_pipeline` (:9190), SPIR-V lookup `library->spirv_bytes(*entry)` (:9271) | SPIR-V from the store -> `VkPipeline` | shader blob -> name -> **metallib store** of the split recipe (S5.2 to build; `tools/route_a_msl.py --split` shape) -> `create_library_metallib` (0.0002 s per library measured) -> `create_render_pipeline` with `CompileMode::async`, `NotReady::skip`, `Priority::demand`; rasterizer, blend, depth-stencil, input layout and RT formats from the parsed stream; topology type 19 -> `TopologyType::rectangle` (vertex_expand), 32 -> quad (`set_topology(quad_list)` at draw); D24 depth -> `D24Mode::fragment`, which needs the pixel shader's MSL (keep MSL next to the metallib for pixel shaders that meet D24, or bake the wrapper into the store); input slots 0-7 and the vertex formats of STATUS's gap list only |
| mesh PSO streams (console subobject 24) | `--mesh-compute` when no mesh shaders | `create_mesh_pipeline` (precheck on). `Status::limit_exceeded` with `limit().kind` `mesh_size` or `mesh_scalars` -> build leonida's mesh-as-compute variant instead (translator `--mesh-compute`: compute + generated VS). 4,631 of 9,282 corpus shaders take this path today (S5.2); 8 of 8 sampled verdicts matched |
| `CreateComputePipelineState` (:13967), `add_pipeline` (:9339), `lds_bytes` pre-check (:9254) | `VkPipeline` | `create_compute_pipeline({cs, threads = entry->local_size, root, async})`; keep the pre-check, against `caps().max_threadgroup_memory` (32,768); a Metal refusal comes back as `limit_exceeded` with the size (equal to `lds_bytes` for 2 of 2 measured) |
| `CreatePipelineLibrary` (:14002), `CreateShaderCacheSession` (:14017) | `VkPipelineCache` | `open_pipeline_cache({dir, translator_version})` once; `flush_pipeline_cache` at shutdown and every N new pipelines; the first-scene list -> `precompile(list, Priority::first_scene)`. The archive gives no hits under `MTL_SHADER_VALIDATION` (measured), so validation runs pay full compiles |
| `CreateQueryHeap` (:13856), `add_query_pool` (:10975) | `VkQueryPool` | `create_query_heap({occlusion/binary/timestamp/pipeline_statistics, count})`; SO statistics -> zeros in the layer, as today |
| `CreateCommandSignature` (:13648) | signature parse (type 105 as DISPATCH, `d3d12_xs_device.cpp:4452`) | unchanged: parsed into `ExecuteIndirect`'s kind and stride |
| `MakeResident` / `Evict` (:13441 / :13445) | no-op | no-op (the device residency set holds every allocation, DESIGN 3.5) |
| `SetEventOnMultipleFenceCompletion` (:13426; the wait is `vkWaitSemaphores` in `d3d12_com.cpp:2793`) | | one `on_completion` per fence + the existing `FenceWaiter` count (ANY / ALL) |
| title-memory governor | Windows commit | `memory_budget()` + `add_memory_pressure_handler` feed leonida's budget (DStorage staging growth, deferred backing); host windows count separately (`currentAllocatedSize` includes bytesNoCopy windows, measured) |

## 3. Queue, fence and present calls (`shims/d3d12_com.cpp`, `dxgi_com.cpp`, `xbox_present.cpp`)

| call (line) | today | Metal adapter |
|---|---|---|
| `ExecuteCommandLists` (:2257; `queue_.execute(recorded, pin)` at :2386) | QueueHolds, then `VulkanQueue::execute_now` -> replay | QueueHolds unchanged; when ready, `metal_replay` records one `gpu::CommandList` per IR list and calls `gpu::Queue::execute` once for the group; pins released by `Fence::on_completion` of a per-queue progress fence signalled after the execute |
| queue `Signal` (:2454) | `vkQueueSubmit` signal | `gpu::Queue::signal(fence, value)` behind QueueHolds |
| queue `Wait` (:2464) | wait semaphore | `gpu::Queue::wait(fence, value)`; the layer forwards only waits it knows will be met (QueueHolds), so a Metal queue is never parked behind a CPU-signalled value |
| `Fence::Signal` (:2114), `GetCompletedValue` (:2104) | `vkSignalSemaphore`, counter value | `Fence::signal_cpu`, `completed_value` |
| `Fence::SetEventOnCompletion` (:2146) | dispatcher or blocking wait | `on_completion(value, SetEvent)`, or `wait_cpu(value, UINT64_MAX)` for a null event |
| console fence slot 12 | GPU address of the value | `Fence::value_address()` (CPU mirror written by the listener; gap "not in GPU order") |
| `GetDeviceRemovedReason` (`d3d12_vulkan_device.cpp:13638`, symbol first) | `DXGI_ERROR_DEVICE_REMOVED` once `VulkanContext::is_device_lost()` (`vulkan_backend.h:1959`) is set by any `VK_ERROR_DEVICE_LOST` | `Queue::wait_idle` returns `device_lost` once per failed command buffer (DESIGN 3.12); `DeviceStats::command_buffer_victims` tells a discard by GPU recovery (another process hung the GPU; happened to three test runs on this machine) from an own error. Fences and the timeline still advance after a failure (CPU repair), so the layer can log the lost frame and keep `S_OK` for a discard instead of latching removal |
| `GetTimestampFrequency` (:2476) | `1e9 / timestampPeriod` | `Queue::timestamp_frequency()` = 1,000,000,000 (measured both arches) |
| `GetClockCalibration` (:2505, `VK_EXT_calibrated_timestamps` device + QPC domains) | | `Queue::clock_calibration()` (`sampleTimestamps` + `CLOCK_UPTIME_RAW`; not an atomic pair: gap); the layer converts `cpu_ns` to its QPC unit |
| queue `SetMarker` / `BeginEvent` / `EndEvent` (:2490-2500, `marker` :2611) | debug-utils labels | `CommandList::begin_event` / `end_event` at the front of the next replayed list, or dropped; list markers (:962-972) are IR `DebugMarker`s |
| `UpdateTileMappings` / `CopyTileMappings` (:2422 / :2437) | sparse binding | gap (`caps().placement_sparse` is 1; not in gpu.h) |
| PresentX compose path (`xbox_present.cpp:274-321`): builds `leonida::PresentPlanes` (`src/vulkan_present.h:69-86`) and calls `presenter_->present_planes(composed, sync_interval, tear)` (:321) | compose pass into a Vulkan swapchain | `WindowPresenter::present_planes` -> `Queue::present_planes(swapchain, desc)` with `plane_count = composed.count`; per plane: `source` (ResourceId) -> `PresentPlane::source`; `source_rect` (when `has_source_rect`) -> `src`; `dest[4]` (fractions of the output) -> `dst` in drawable pixels (`dest * drawable size`); `point_filter` -> `Filter::point`, else `linear`; plane 0 -> `PlaneBlend::opaque`; plane 1 -> `straight` when `straight_alpha`, else `premultiplied`; `hdr10`, `sdr_white_nits`, `sdr_eotf`, `hdr_peak_nits` -> gap (SDR only); `tear` -> `sync_interval = 0` |
| two planes with Flags 0 (`xbox_present.cpp:270-274`) | plane 1 is composed over plane 0 unless `LEONIDA_PRESENT_PLANE1=0` (retail GTA VI presents scene + UI with Flags 0) | unchanged in the layer; the backend composes whatever planes it is given (measured: 0 of 230,400 pixels off by more than 1, STATUS item 6) |
| single-plane path `presenter_->present(resource, sync_interval, rect, tear)` (:352), `present_blank` (:192) | blit into the swapchain | `Queue::present_planes` with one opaque plane (`src` = `rect`); blank = one plane of a 1x1 black texture |
| frame events 78-81 (`WaitFrameEventX`, `FramePacer`) | timer | `SwapchainDesc::frame_events = true` and `Swapchain::wait_frame_event` (CADisplayLink; 8.33-8.34 ms median at 120 Hz measured); `GetFrameStatisticsX` from `Swapchain::frame_stats()` |
| DXGI `Present` / `Present1` (`dxgi_com.cpp:304` / :525) | `vkQueuePresentKHR` | `Queue::present(swapchain, sync_interval)` |

`present_planes` blocks in `nextDrawable` when presents outrun the display (7.6-8.9 ms a call at 120 Hz with 3
drawables, measured); the compose pipelines are built at `create_swapchain`, so the first two-plane present no longer
compiles (STATUS item 6).

## 4. The replay: each IR command (`src/command_list.h`) onto `gpu::CommandList`

`metal_replay(const leonida::CommandList&, gpu::CommandList&, MetalContext&)`, one call per IR list, in place of
`vulkan_replay.h`'s prepare pass (:3484-4130) and record pass (:4512-5300). At the start of each list:
`set_root_layout(shared)`, `set_heap_base(arena)`, `set_descriptor_table(b, table[b])` for b = 0..28.

| IR command | `vulkan_replay.h` | gpu.h calls | status |
|---|---|---|---|
| `TransitionBarrier{resource, before, after}` | :3484, :4543 | `barrier({transition, res, before, after})` | direct (read-only to read-only is a no-op in the backend) |
| `UavBarrier` | :4559 | `barrier({uav, res})` | direct; inside a compute encoder that began an epoch it is a memory barrier (63 of 63 in the chain test) |
| `AliasingBarrier` | :4571 | `barrier({aliasing})` | direct |
| `GlobalBarrier`, `BufferBarrier`, `TextureBarrier` (enhanced) | :3495-3499, :4575-4600 | `barrier({transition, res, before, after})` with access masks mapped to states: any write access on either side -> a state with a write bit (fenced), read-only on both sides -> two read states (no-op) | adapter maps the masks |
| `ClearRenderTarget{target, color, rects}` | :3516, :4659 | `clear_render_target` | rects: gap ("clears with rectangles") |
| `ClearDepthStencil{target, flags, depth, stencil, rects}` | :3538, :4696 | `clear_depth_stencil` (D24 quantised by the backend) | rects: gap |
| `ClearUnorderedAccess` | :3557, :4739 | gap (planned: builtin compute clear) | gap |
| `CopyBuffer` | :3667 | `copy_buffer` | `tiled_resource`: gap |
| `CopyTexture{dst, src, box, footprint}` | :3690 | footprint<->texture: `copy_buffer_to_texture` / `copy_texture_to_buffer` (`depth_plane` for depth) | texture<->texture: gap; D24 footprint conversion: gap; `rgb96` repack: gap |
| `CopyResource` | :2975 (operands), prepare pass | buffers: `copy_buffer`; textures: gap | partial |
| `ResolveSubresource` | :3828 | gap | gap |
| `Predication` | :3914, :4512 | none: run the predicated work unconditionally, log once (DESIGN F42) | gap |
| `DiscardResource` | :3861 | `discard(resource)` | partial regions discard the whole resource |
| `WriteImmediate{address, value, mode}` | :3881, :5084 | `write_value(buffer_of(address), offset, value, 4)` | marker modes land like normal mode (after earlier commands) |
| `label::Write{target, value, clock}` | :3879, :5082 | clock 0: `write_value(..., 8)`; clock 1 (GPU timestamp): `write_timestamp(buffer, offset)`; clock 2: stays with the CPU pump | GPU-written labels only for targets in a GPU buffer; title-memory targets stay with `d3d12_xs_list_values.h` |
| `AtomicCopyBuffer` | :3890 | `barrier(uav)` + `copy_buffer` | approximation (orders all earlier work) |
| `BeginQuery` / `EndQuery` | :4241-4243, :4392-4396 | `begin_query` / `end_query` (timestamp: end only) | a query open across render passes counts the last pass: gap |
| `ResolveQueries` | :3953 | `resolve_queries(heap, first, count, dst, offset)` | pipeline statistics: zeros |
| `DebugMarker{set/begin/end}` | :5052 | `begin_event` (+ `end_event` for set) / `end_event` | direct |
| `ExecuteIndirect{kind, stride, max_count, argument_buffer, argument_offset, count_buffer, count_offset, pipeline, user_data}` | :3972, :5110 | kind dispatch with `pipeline != 0`: `set_compute_pipeline` + `set_user_data(0, user_data)` first; then `execute_indirect({kind, stride, max_count, args, argument_offset, count, count_offset})` (kinds map 1:1, `dispatch_mesh` included) | direct; count buffers use a hoisted compute pre-pass (1 hoisted, 0 pass splits measured); no group cap (`vulkan_indirect_clamp.h:60-64`'s 65,535 per dimension and 2^20 groups are Vulkan-path limits: do not port them) |
| `IndirectRasterDraw` (`shared_ptr<IndirectRasterPayload{Draw state; IndirectDrawArgs arguments}>`) | | the `Draw` state below, then `execute_indirect(draw / draw_indexed / dispatch_mesh)` from `arguments` | direct; quad and rect topologies drawn indirectly: gap |
| `Dispatch{pipeline, user_data, x, y, z}` | :4124, :5248 | `set_compute_pipeline`, `set_user_data(0, user_data)`, `set_code_base(pipeline.code_base)`, `dispatch(x, y, z, flags)` | the IR has no flags word: add `std::uint32_t flags` to `Dispatch` and record console list slot 80's fifth argument (`0x20` / `0x8020`, `leonida/docs/d3d12_command_objects.md:401`; slot 80 is not forwarded today, `d3d12_xs_device.h:428-441`); bit 0x20 -> `dispatch_flag::thread_counts` (measured exact) |
| `Draw{pipeline, user_data, views, depth, index, topology, viewport, scissor, blend_factor, stencil_front/back, indexed, count, instances, first, base_vertex, first_instance, mesh, viewports}` | prepare + record | `set_render_targets(views->render_targets[0..render_target_count], depth)`; `set_pipeline` or `set_mesh_pipeline` (`mesh`); `set_user_data(0, user_data)`; `set_code_base`; `set_vertex_buffers(0, views->vertex_buffers[0..7])` (console VAs; `resolve_va` finds arena ranges and host windows); `set_index_buffer(index)`; `set_topology(topology)` (17 rect, 19 quad handled by the backend); `set_viewport`, `set_scissor`, `set_blend_factor`, `set_stencil_refs(front, back)`; then `draw` / `draw_indexed` / `draw_mesh(count, instances, first)` | `viewports` (more than one viewport or scissor): gap; vertex slots 8-31: gap |
| `BuildAccelerationStructure`, `CopyAccelerationStructure`, `AccelerationStructurePostbuild` | :4025-4077, :5149-5194 (XDXR compute, `xdxr_build.inc`) | the XDXR builder is compute over arena addresses: port its kernels to MSL, run them as `dispatch` | gap (largest replay item; BVH-builder pipelines appear at P0b) |

Draw-state redundancy: the IR captures full state per draw and the backend's `DrawState` is copy-on-write, so
re-setting identical state costs a compare. Replay should still skip `set_*` calls equal to the previous draw's, to
stay near the measured 0.30-0.38 us per dispatch (arm64) / 0.57 us (x86_64) in `execute`.

## 5. Title memory, values and read-back

- **Arena**: one console-arena heap. `heap_base` = its `gpuAddress` (DESIGN F08); console VA = arena offset, exactly
  `title_va_alias.h`'s model. Size cap 13,639 MiB (one `MTLBuffer`); the Series S profile fits.
- **Title-owned memory** (slot 133 buffers, root views and V#s into the per-frame constants ring, the 185 MB 32-bit
  heap with its 174 MB ring). `TitleVaAliases` registers two hooks (`title_va_alias.h` header comment): a resolver
  that makes a GPU copy for a title address with none yet, and a scanner that rewrites buffer V#s of bound tables
  before a submission; addresses no allocator reported come from the memory probe (`title_memory_probe.cpp:58`,
  installed by `platform/register.cpp:39`). On Metal the resolver makes no copy: it calls `create_host_buffer` over a
  host alias of the same physical pages (page-aligned; 16 KiB arm64, 4 KiB x86_64) registered at the title VA, and
  returns "window `host_gpu_address()` - heap_base" (mod 2^64), the form `title_va_alias.h` already uses for buffers
  with their own memory. Measured: 0 wrong words over 60 frames x 600 blocks read in place; the copy it replaces
  costs 0.26-0.37 ms CPU (arm64; 0.73 ms x86_64) + 0.25 ms GPU and 32.8 MiB of buffers a frame for 16.4 MiB of
  constants. The page watch (`title_page_watch.h`) is not needed for windowed ranges. Requires xmem (os/) to expose
  the alias (`vm_remap` of its pool pages) and to honour the 32-bit flag (x86_64 low VA needs `-pagezero_size`,
  which arm64 refuses: killed at launch).
- **List values**: unchanged. `d3d12_xs_list_values.h` signals the queue's `written` fence behind each part and stores
  values with the CPU; waits become `Queue::wait(gate)` with the gate signalled from the CPU. The backend's own
  `write_value`/`wait_value` are not used on this path (they remain for a GPU-ordered variant).
- **Ocean read-back** (the CPU physics reads GPU-written R32F/RG32F/RG16UI textures): allocate those four textures with
  `create_buffer_texture` over a host window; after the list's fence the CPU re-tiles from that memory into the
  title's pages with `xg_xs_layout.h`. Measured: 0 wrong texels, no copy (884,736 B a frame saved), 0.15-0.16 ms
  execute-to-idle.
- **A8_UNORM render targets** (the title creates a 3840x2160 one at boot; KosmicKrisp refused it and
  `src/a8_red_fragment.h` stores it as R8 on Vulkan): Metal's `A8Unorm` is a colour attachment here and blends on
  alpha (measured at 3840x2160: clear, plain draw and one/inv_src_alpha blend exact on every texel, both arches).
  The Metal path needs no stand-in; do not carry `a8_red_fragment.h` over.

## 6. Order of work for round 3 (each with a gate)

1. Extract the non-Vulkan parts of `d3d12_vulkan_device.cpp` into shared headers (with the d3d12_vulkan owner). Gate:
   d3d12_vulkan still builds and its tests pass unchanged.
2. `MetalContext` + device creation for queues, fences, heaps (arena), committed/placed buffers and textures,
   samplers, the 29 tables with their slot-0 fallbacks. Gate: the 1,185-call P0 sequence (SCOPE work item 1) to the
   first `CreateDescriptorHeap` on Metal.
3. `metal_replay` for barriers, clears, copies (footprint), draws, dispatches, indirect, queries, markers, writes.
   Gate: replay the recorded lists of `d3d12_vulkan`'s own tests against both backends and compare readbacks.
4. Metallib store from the S5.2 split run, pipelines async + skip + archive + first-scene list. Gate: the P0b storm
   (53,967 creations) completes with no compile on title threads and fewer compiled than created.
5. `WindowPresenter` over `present_planes` + frame events. Gate: the launch movie paced at the console rate.
6. Host windows for title memory and the ocean read-back. Gate: constants ring and slot-133 buffers with no copies.
7. Gaps that block P3/P4 in the order the title meets them: UAV clears, texture copies, resolve, XDXR compute,
   multi-viewport, more vertex slots and formats, D24 footprint copies, predication.

## 7. Risks the adapter inherits

- The join model (DESIGN 3.3) orders every encoder after a barrier against all work before it; its overlap cost on
  a real frame is unmeasured.
- `MTL_SHADER_VALIDATION` changes limits (doubles reported threadgroup memory, refuses the exactly-32 KiB mesh shape,
  4 corpus mesh shaders crash the compiler service) and turns binary-archive lookups into misses, so validation runs
  cannot judge limit refusals or cache behaviour.
- Runtime MSL compile is 0.44-0.59 s for four pairs without the system cache; only metallibs + the archive make
  creation cheap (0.0011 s for four pairs). The store must ship metallibs.
- Residency-set cost with ~38,000 streamed images is unmeasured.
- Another process that hangs the GPU makes Metal discard this process's in-flight command buffers (measured: 9 discards
  in three `metal_hazards` runs today). Their work is lost; the layer sees `device_lost` from `wait_idle` and must not
  rely on the results of that frame (a GPU-to-CPU read-back such as the ocean textures has to be redone).
- A timestamp does not order later work (DESIGN 3.8): a profiling pair needs a barrier after the first timestamp.
