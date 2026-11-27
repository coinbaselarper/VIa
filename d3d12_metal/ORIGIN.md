# Origins of the code in port/d3d12_metal

The module is leonida's D3D12 layer (`port/d3d12_vulkan`, itself from `leonida/`) on `gta6rt::gpu`
(`port/metal`). Three kinds of source:

1. **Compiled from `port/d3d12_vulkan` by path, unchanged.** `d3d12_metal.cmake` lists them
   (`D3D12MTL_UPSTREAM_SOURCES`, `D3D12MTL_UPSTREAM_PLATFORM`): `shims/d3d12_xs_device.cpp`, `shims/d3d12_xs.cpp`,
   `shims/xbox_present.cpp`, `shims/package_splash.cpp`, `platform/image_decode.cpp`, `platform/display_info.cpp`,
   `platform/win32_host.cpp`, `platform/register.cpp`, `platform/rts0_serializer.cpp`,
   `platform/rts0_deserializer.cpp` (added round 5, upstream's 19:11 file `d3d12_xs.cpp` now calls),
   `platform/title_memory_probe.cpp`, `platform/fault_guard.cpp`, plus every header of `src/` and `shims/` they and
   the copies include (command-list IR, root-signature parsing, gfx10 descriptor encoding, title-VA aliases, page
   watches, console heaps, queue holds, the PSO stream and cached-PSO code, present dumps). Those TUs are built
   against this module's headers through a clang VFS overlay (`d3d12_metal_overlay.yaml` in the build tree), which
   replaces five Vulkan-bound headers for this module only:

   | upstream header | replaced by |
   |---|---|
   | `shims/d3d12_com.h` | `shims/d3d12_com.h` (copy, below) |
   | `shims/dxgi_com.h` | `shims/dxgi_com.h` (copy) |
   | `shims/d3d12_pipeline_compile.h` | `src/metal_pipeline_compile.h` (`note_present_for_warmup`, `compile_us` only) |
   | `src/vulkan_present.h` | `src/metal_present_planes.h` (copy of the plane structs + `MetalPresenter`) |
   | `src/vulkan_backend.h` | `src/metal_context.h` (own) |

2. **Copies of upstream files that mix Vulkan and backend-neutral code** (edited; listed below). The exact snapshot
   each copy started from is kept in `origin/upstream/` (sha256 prefixes below) and `tools/origin_hunks.sh`
   regenerates `origin/hunks.txt`, every changed line range as `upstream a,b -> copy c,d`.

3. **Own code**: `src/metal_context.{h,cpp}` (resource/VA/pipeline registry, `MetalQueue` with the layer's
   `QueueHolds` cross-queue holds, label completions), `src/metal_replay.cpp` (IR -> `gpu::CommandList`),
   `src/metal_present.cpp` (`MetalPresenter`: `gpu::Swapchain` + present dumps), `src/metal_shaders.{h,cpp}`
   (shader store and on-demand builds), `src/metal_bindless.{h,cpp}` (set 0 over `gpu::DescriptorHeap`s),
   `src/metal_formats.h`, `src/metal_ledger.h` (round 5: GPU memory by the Vulkan path's categories),
   `platform/register_metal.cpp`, `platform/backend_select.cpp`, `superbuild/attach.cmake`, `tools/*`. (`cache/` is
   the cache worker's sub-module.)

## Copies

| copy | upstream file (snapshot time, sha256/16) | hunks | upstream lines removed / lines added |
|---|---|---|---|
| `shims/d3d12_com.h` | `d3d12_vulkan/shims/d3d12_com.h` (05:20:43, `0f6c0415e34c15df`), plus the 15:43 `make_refused_pipeline_state` declaration merged by hand | 42 | 190 / 61 |
| `shims/d3d12_com.cpp` | `d3d12_vulkan/shims/d3d12_com.cpp` (11:30:41, `9b7bf405edc45557`) | 64 | 409 / 118 |
| `shims/dxgi_com.cpp` | `d3d12_vulkan/shims/dxgi_com.cpp` (00:48:37, `a27ff9c6a9dd4d8b`) | 21 | 733 / 38 |
| `shims/dxgi_com.h` | `d3d12_vulkan/shims/dxgi_com.h` (`989f7ffa430920c6`) | 3 | 8 / 4 |
| `shims/d3d12_metal_device.cpp` | `d3d12_vulkan/shims/d3d12_vulkan_device.cpp` (15:15:50, `29ed5caf014dbf12`), plus the 16:05 `make_refused_pipeline_state` / `refused_pipeline_state` / `PipelineState::make_refused` merged by hand | 453 | 7,559 / 1,712 |
| `src/metal_present_planes.h` | `d3d12_vulkan/src/vulkan_present.h` (`1bd1e31f71128ab3`): `PresentRect`, `PresentPlane`, `PresentPlanes` kept verbatim, `VulkanPresenter` replaced by `MetalPresenter` | 23 | 1,825 / 39 |
| `src/metal_formats.h` (part) | `d3d12_vulkan/src/vulkan_replay.h`: `BlockFormat`/`block_format` and `typeless_view_family` verbatim; `metal_format` own | - | - |

Upstream moved on during the round: `d3d12_vulkan_device.cpp` at 16:05 differs from the snapshot in 532 diff lines
(the refused-PSO placeholder, merged; a boot manifest, a BC view-format check, a Bink frame-buffer stack scan and a
parallel console-heap scan on `CopyPool`, not merged). The files compiled by path (`d3d12_xs_device.cpp` and the rest)
are always the current ones, so a new upstream entry point they call shows up as a link or compile error here.

**Round 5 merges by hand** (from `d3d12_vulkan_device.cpp` as of 17:55; the merged blocks are byte-identical to the
18:52 file, sha256/16 `0e73ea80f4b5ba65`, checked with a block diff):
- The Bink frame-buffer scan (upstream round 4, `port/d3d12_vulkan/STATUS.md` "Movie plane sets"):
  `DeviceState::note_bink_candidates` / `take_bink_candidates` (guest-stack words around Map's ppData), the call in
  `Resource::Map` for R8 2D textures at their first map, and the whole `BinkGroup` block (`find_bink_groups` over the
  candidates, `report_bink_miss`, `try_bink_group` with the one offset that takes every plane entry from the CPU window
  0x3... to the textures in the GPU window 0x7..., `refresh_bink_groups` with `idle`). Verbatim, plus one diagnostic
  (`GTA6RT_METAL_BINK_PROBE`). `TitleTexture::bink_tries` (the old search) is unused now.
- Retiring copies the title dropped (`LEONIDA_TITLE_GONE_RETIRE`, upstream 17:03): `gone_candidates_` in the map-change
  walk of `refresh_title_textures` and the retire loop after it, with `gone_retired_` / `gone_retired_bytes_` counters.
- `d3d12_xs_native_pso.h` (compiled by path) gained a fifth argument to `create` at 18:52 (a mesh refusal before the
  key is made): the copy's `create_native_graphics` passes one that refuses nothing (Metal builds mesh PSOs).
- `origin/hunks.txt` regenerated (the copy against the 15:15 snapshot: 524 hunks, 7,477 / 2,407 lines), so the merged
  blocks and this round's Metal changes show there as hunks of the copy.

**Round 6 merges by hand**: `retire_overlapped_title_textures` (upstream round 5 "stale copies retired", from
`d3d12_vulkan_device.cpp` as of 20:00) with `TitleTexture::span` and `max_title_span_`, called where a made image or
a slot-133 texture enters the registry; its retired images wait for queue progress here instead of 3 s. Own code added
this round (no upstream counterpart): `land_outside_arena`, `reserve_arena_va` / `register_metal.cpp`'s
`reserve_arena_vas` and crash hooks, the split64 / dual-source / shrink_lds paths of `PipelineState`, `report_work`.
`origin/hunks.txt` was not regenerated.

What changed in each copy, by area (line ranges: `origin/hunks.txt`):

- **`d3d12_com.h`**: includes `src/metal_context.h` instead of the Vulkan backend; `create_command_queue`,
  `create_fence` and `set_event_on_multiple_fence_completion` take `leonida::MetalContext&` (no `VkQueue`, family,
  clock or lease); `metal_queue_of` / `metal_fence_of` added; `WindowPresenter` holds a `leonida::MetalPresenter`
  and `read_back`/`extent` return plain sizes; `VulkanDeviceInfo` keeps only backend-neutral fields with the Metal
  answers (no `VkInstance`, `VkPhysicalDevice`, `VkDevice`, `VkQueue`, queue families, `VkPhysicalDeviceFeatures`);
  the Vulkan-only test hooks (`vulkan_image`, `vulkan_buffer`, `bindless_set`, `tile_heap`, ...) removed.
- **`d3d12_com.cpp`**: `CommandQueue` executes through `leonida::MetalQueue` (replay, holds, pins) and `Fence` is a
  `gpu::Fence` (CPU signal/wait, `on_completion` for SetEventOnCompletion, slot-12 value address); the command
  allocator and list recording (all of `GraphicsCommandList`) are unchanged.
- **`dxgi_com.cpp`**: the factory/adapter/output answers kept; adapter description from `gpu::DeviceCaps`; the PC
  DXGI swap chain (`CreateSwapChainForHwnd`; the console title presents through PresentX, `xbox_present.cpp` ->
  `WindowPresenter`) answers E_NOTIMPL with a log line, and its Vulkan surface/swapchain/blit code is removed
  (`dxgi_swapchain.h` is not used).
- **`d3d12_metal_device.cpp`**: kept unchanged: argument validation and the host-measured answers of every
  method, console heaps, the GPU address arena allocator (as a `gpu::Heap` with `console_arena`), title memory
  (slot-133 windows, shadows, title textures, XG layouts), gfx10 T#/V#/S# encoding and the descriptor hash table,
  root signatures, PSO parsing and the shader-index lookup, query heaps, pipeline libraries, shader cache sessions.
  Replaced: device creation (`create_vulkan_device_standalone` -> `gta6rt::metal::create_device`), resources
  (`gpu::Buffer` / `gpu::Texture` / `create_host_buffer` / `create_texture_view` instead of `VkBuffer`/`VkImage`
  /memory), set 0 (`MetalBindless`), pipelines (descs kept at creation, Metal pipelines built at first use from the
  shader store), CheckFeatureSupport format answers (from `gpu::Device::describe_format`), allocation info
  (`texture_allocation_info`), the present/window presenter functions. Removed: Vulkan instance/device selection,
  queue pools, calibrated clocks, host image copy, VK_EXT_external_memory_host imports, sparse binding, ray tracing
  (prebuild info answers 0; builds dropped and counted), LEONIDA_GEOMETRY_MB geometry copies (their pool is Vulkan
  memory), the A8 red stand-in, compute/memory probes.

## Shared state across the module boundary

The prelink (`ld -r -exported_symbols_list`) hides every symbol except `gta6rt_d3d12_metal_*` and the inline
functions/statics of the namespaces the front end shares with the host and shims (title-memory notes, XG layouts,
value sinks, GPU labels, DirectStorage, traces; list in `d3d12_metal.cmake`), which stay weak so they coalesce with
the host's copies. `tools/check_prelink.sh` runs after the prelink and fails the build if a layer symbol is left
undefined (it would bind to `gta6rt_d3d12_vulkan`'s copy).
