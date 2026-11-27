# Origin of the files in d3d12_vulkan/

Source: `leonida/` at commit d3b8251bc390418cdbc1f2d30727b6b31d889cfa (2026-11-25), GPL-3.0-or-later. Nothing
under `leonida/` was edited; every file here is a copy.

## Copied from leonida, unchanged

- `shims/*.h` and `src/*` not listed below: verbatim copies of `leonida/runtime/shims/<name>` and
  `leonida/runtime/src/<name>` (`shims/xbox_present.cpp`, the other 26 `shims/` headers and 84 of the 86 `src/` files).
- `tools/gen_d3d12_stubs.py`, `tools/report_paths.py`: verbatim copies of `leonida/tools/`.

## Copied from leonida, modified for the macOS host

Line counts are `diff | grep -c '^[<>]'` against the leonida original.

| file | from | changed lines | what changed |
|---|---|---:|---|
| `shims/d3d12_vulkan_device.cpp` | runtime/shims | 198 | Metal WSI (VK_EXT_metal_surface on the window's CAMetalLayer) at the presenter's 2 surface sites; driver from `platform/vk_driver` instead of `vulkan-1.dll`; VK_KHR_portability_subset enabled when listed; RetVal form for 10 struct-returning methods; `__try` copies replaced by checked copies; arm64 LDNP body for `copy_streaming`; `std::u16string` pipeline-library names; no DISPLAYCONFIG SDR white level |
| `shims/dxgi_com.cpp` | runtime/shims | 158 | displays and modes from CoreGraphics (`platform/display_info`), window resize through `mac_window`, `u"..."` literals, no d3dkmthk |
| `shims/package_splash.cpp` | runtime/shims | 122 | WIC -> ImageIO (`platform/image_decode`), XmlLite -> regex over the two manifests, no GDI paint |
| `shims/d3d12_xs_device.cpp` | runtime/shims | 79 | fallback PresentX window is a `gta6rt_mac_window`; window resize through `mac_window` |
| `shims/title_window.h` | runtime/shims | 43 | the title's window is the largest visible window of the `mac_window` registry |
| `shims/dxgi_swapchain.h` | runtime/shims | 23 | Metal WSI instead of Win32 WSI |
| `shims/d3d12_xs.cpp` | runtime/shims | 16 | Vulkan device is the default backend; export table exposed to `platform/register.cpp` instead of `SHIM_TABLE` |
| `shims/title_hud.h` | runtime/shims | 10 | window title through `mac_window`; no VRAM figure |
| `shims/d3d12_com.cpp` | runtime/shims | 6 | RetVal form of `ID3D12CommandQueue::GetDesc` |
| `shims/d3d12_xs_placed_depth.h` | runtime/shims | 6 | RetVal form of `ID3D12Resource::GetDesc` |
| `shims/startup_trace.h` | runtime/shims | 4 | UTF-32 path |
| `shims/d3d12_com_object.h` | runtime/shims | 2 | `SetName` length in UTF-16 units |
| `src/title_page_watch.h` | runtime/src | 36 | page checks instead of `__try` |
| `src/guarded_copy.h` | runtime/src | 2 | `LONG` for the interlocked flag |
| `tools/patch_d3d12_retval.py` | new | — | generates `d3d12.h` in the RetVal form plus `__CRT_UUID_DECL` for every interface |

## New for the macOS host

`platform/` (all new): `win32_host.{h,cpp}` (force-included kernel32/user32 subset), `vk_driver.{h,cpp}`,
`mac_window.h` + `metal_window.mm`, `display_info.{h,cpp}`, `image_decode.{h,cpp}`, `register.{h,cpp}`,
`xmem_fallback.cpp`. `include/dxgi*.h` (DXGI interfaces; DirectX-Headers has none), `tools/check_dxgi_iids.py`,
`tests/`.

## Integration changes (S1 integrator, 2026-11-26)

- `platform/rts0_serializer.{h,cpp}` (new): `D3D12SerializeVersionedRootSignature` / `D3D12SerializeRootSignature`
  write a DXBC container with one RTS0 part (the layout `rt::rootsig::parse_rts0` reads) when there is no host
  `d3d12.dll`. `shims/d3d12_xs.cpp`: the two `if (!fn) return E_NOTIMPL;` lines call it instead (+1 include).
- `shims/d3d12_vulkan_device.cpp` `CreateCommandSignature`: a DISPATCH_MESH signature is created on a device without
  mesh shaders (logged once) instead of E_NOTIMPL; ExecuteIndirect already skips mesh records on every device.

## Integration changes (round 3 integrator, 2026-11-26)

- `shims/d3d12_vulkan_device.cpp` `copy_guarded` (macOS path, not leonida's): the plain (non-streaming) copy of the
  slot-133 shadows and console heaps checks the pages with `rthost::readable` and copies with `memcpy`, as the
  streaming copy already did, instead of `rthost::safe_copy` (`mach_vm_read_overwrite`). `LEONIDA_SHADOW_KERNEL_COPY=1`
  restores the kernel copy. Measured in `port/STATUS.md` ("Measure and optimise").

## Vulkan-path changes (first pixels, 2026-11-26)

New files (port/, not from leonida):
- `src/vk_run_counters.h`: per-run counters printed after each "submit phases" line (`[rt] vk-counters ...`): PSOs
  created/refused (by reason), Vulkan pipelines built/failed and their build time (all threads, compiler threads),
  ExecuteCommandLists wall time, uses of a pipeline not built yet, draws emitted/skipped, session-cache traffic;
  `process_age_s()` (sysctl process start time); the per-submission wait budget (`LEONIDA_PIPELINE_WAIT_MS`).
- `src/float_controls_check.h`: a module's float-controls execution modes against the device's properties.
- `src/a8_red_fragment.h`: A8_UNORM render targets stored as R8_UNORM (pixel shader alpha to red) and the registry of
  such resources for clears.
- `platform/title_memory_probe.{h,cpp}`: names the guest allocation (os/ region table, else the task's VM map) that
  holds a title address the allocator never reported.
- `tools/run_vk.sh` (the title on the Vulkan path through `host/tools/run.sh`), `tools/png_stats.py`,
  `tools/vk_run_summary.py`.

Changed leonida copies (diff lines against leonida, all changes of this module included):

| file | lines | change |
|---|---:|---|
| `src/title_va_alias.h` | 36 | `set_memory_probe`: a miss in the title-memory table asks the probe and adds its range |
| `src/present_dump.h` | 85 | `LEONIDA_PRESENT_FIRST_LIT=dir[,N]`: the first frame with a non-black texel is written as PNG |
| `src/submit_phases.h` | 4 | process age on macOS; prints the run counters with each line |
| `src/vulkan_replay.h` | 8 | counts emitted and skipped draws, and draws skipped for a pipeline not built yet |
| `shims/d3d12_pipeline_compile.h` | 67 | `PipelineCompiler` runs `LEONIDA_PIPELINE_COMPILE_THREADS` workers (default 4, leonida 1); `LEONIDA_ASYNC_PIPELINES` is on unless `=0` (leonida: off unless `=1`); creation feedback is always chained (cache hits counted untraced) |
| `shims/d3d12_com.cpp` | 38 | counts submissions and the draws/dispatches of failed ones; A8-as-R8 clear colour; each ExecuteCommandLists sets the thread's wait budget and is timed |
| `shims/d3d12_com.h` | 2 | `VulkanDeviceInfo::float_controls` |
| `shims/d3d12_vulkan_device.cpp` | 558 | the S1.6/S1 changes listed above, plus: float-controls property query and refusal (`LEONIDA_FLOAT_CONTROLS=ignore` opts out); PSO and Vulkan pipeline counters; A8_UNORM render targets as R8_UNORM where no A8 attachment exists (`LEONIDA_A8_RED=0` refuses them as before); rect-list and stream refusals counted and named; a use whose pipeline the compiler threads build waits within the submission's budget, then is skipped (`wait_for_build`); the session pipeline cache is written by a per-device thread every `LEONIDA_CACHE_CHECKPOINT_SECONDS` (default 30, `0` off) from a merged private copy (MoltenVK cannot serialize a cache other threads are adding to), with a margin and retries, and read on a thread of its own, which then replaces the device's empty cache; not read on MoltenVK unless `LEONIDA_SESSION_CACHE_LOAD=1`; cache bytes loaded/written counted; host memory is not imported on KosmicKrisp (`LEONIDA_HOST_MEMORY_IMPORT=1` imports it, `=0` turns it off everywhere); the rect-list refusal and a pipeline the compiler threads could not build are named in the log |
| `platform/register.cpp` | — | installs the title memory probe; `gta6rt_d3d12_vulkan_at_exit` (stop the compiler threads, write the session cache) |
| `CMakeLists.txt` | — | `platform/title_memory_probe.cpp` |

Shared header (additive): `include/gta6rt/shims.h` declares `gta6rt_d3d12_vulkan_at_exit` (API version 4).

## Review round 3 (PSO accounting, pipeline cache), 2026-11-26

New files: `tests/pso_accounting_test.cpp` (the counting rules of `src/vk_run_counters.h`, both arches; target
`d3d12vk_pso_accounting_test` in `tests/CMakeLists.txt`), `tests/metal_cache_probe.mm` (a measurement tool, built by
hand: `clang++ -std=c++20 -fobjc-arc -O2 -arch x86_64 tests/metal_cache_probe.mm -framework Metal -framework Foundation`).
`tools/kk_cache_msl.py` (the MSL a KosmicKrisp session file holds, the probe's input).
`src/vk_run_counters.h` (port/ file): `PsoCall` counts each title PSO call once at its entry with one reason,
`count_path`, per-entry and per-reason report lines with the failed-calls/reasons check, build time split by creation
feedback (`build_feedback`). `tools/vk_run_summary.py` reads the new lines.

Changed leonida copies (diff lines against leonida now, all changes of this module included):

| file | lines | change in round 3 |
|---|---:|---|
| `shims/d3d12_vulkan_device.cpp` | 789 | `PsoCall` at `CreateGraphicsPipelineState`, `CreateComputePipelineState`, `CreatePipelineState` and `PipelineState::create_{graphics,compute}` (bodies moved to `create_*_pipeline_state`); reasons for the no-VS/MS, FSSM/FSPX-miss and no-program refusals; `refused_at`: every silent E_INVALIDARG of `parse_pipeline_stream` and `create_{graphics,compute}_impl` records its source line; the console stream reading drops the PC reading's reason; conservative rasterization named; `g_session_cache_writes` / `decide_session_cache_writes`: MoltenVK's session file is neither read nor written unless `LEONIDA_SESSION_CACHE_LOAD=1` or `LEONIDA_SESSION_CACHE_WRITE=1` |
| `shims/d3d12_xs_device.cpp` | 174 | `PsoCall` at slots 55, 57, 62, 67, 86; slot 57 records the factory's reason, retries the fallback with it taken, and records both on failure; reasons for the slot-57 factory pre-checks, the fallback's refusals, slot 67's payload refusal and every slot-55 refusal (two new `refuse_derived` reasons) |
| `shims/d3d12_xs_native_pso.h` | 18 | the factory records the plan's reason (with block index), a missing or mismatched container, and exceptions |
| `src/native_pso.h` | 8 | `plan()` sets `reason` for missing, ambiguous and unsupported plans too |
| `shims/d3d12_pipeline_compile.h` | 68 | `CompileTrace::end` passes hit/miss and build time to `build_feedback` |
| `src/vulkan_replay.h` | 8 | unchanged in round 3 (the report text of its counter now says "not built yet or never buildable") |

## Round 4 (submission copies, device loss, rect lists, PSO census, compile scheduling, S3 requests), 2026-11-26

New files (port/ code, no leonida origin): `platform/fault_guard.{h,cpp}` (fault-safe copy/compare by a chained
SIGSEGV/SIGBUS handler and known fault exits; the layer's vectored handlers behind `LEONIDA_HOST_VEH=1`),
`src/copy_pool.h` (helper threads for the per-submission copies), `src/boot_manifest.h` (pipelines in first-use order,
built first next boot), `src/rect_list_vertex.h` (rectangle lists expanded in the vertex stage, after
`port/metal/src/rect_list.cpp`'s approach), `tests/fault_guard_test.cpp`, `tests/rect_list_vertex_test.cpp`,
`tests/nocopy_unmap_probe.mm` (measurement tool, built by hand), `tools/phase_summary.py`, `MESH_PLAN.md`.

Changed leonida copies:

| file | change in round 4 |
|---|---|
| `shims/d3d12_vulkan_device.cpp` | `copy_guarded`/`hash_guarded` read through `fault_guard` (no readability pre-check; leonida's streaming copy and raw hash removed; the sampled hash gathers its 64-byte samples in one guarded call); console resource heaps compared in place with `previous` and only changed 4 KiB pages copied (`scan_console_heap`, `LEONIDA_CONSOLE_HEAP_SCAN`); shadow copies run on `CopyPool`; movie frame-buffer sets found from the guest stack at Map, matched to their textures through one address offset (the CPU and GPU windows) and refreshed by the set index (`BinkGroup`, `LEONIDA_BINK_SETS`; a search that finds nothing is logged); host-imported memory (slot-133 imports, imported console heaps) freed through the deferred queue (`LEONIDA_HOST_IMPORT_FREE=now`: as before); `create_host_buffer` holds the submit mutex from `vkCreateBuffer` until bound or declined; boot manifest noted at a pipeline's first demanded use (also when warm-up built it first), planned at PSO creation, manifest priority below warm-up; rect-list variant built from `rect_list_vertex` without a geometry stage (PSOs of topology type 19 too); T# views Metal cannot make refused in `bind_image_locked` and `bind_image` (`view_format_incompatible`); `make_refused_pipeline_state` and the refused placeholder; skipped draws named in the census; the session file is written only when it was loaded or absent (`g_session_file_state`); manifest written at exit; Map's pointer slot logged |
| `shims/d3d12_pipeline_compile.h` | per-stage creation feedback counts MoltenVK's cache hits; one worker kept for demanded builds; warm-up (`kWarmupPriority`) held until the first present with `LEONIDA_WARMUP_DRAWS` (4) of the title's draws (`LEONIDA_WARMUP_START=now`), on `LEONIDA_WARMUP_THREADS` (1) threads |
| `shims/d3d12_xs_device.cpp` | the present hook opens warm-up; refused PSOs at slots 55/57/62/67/86 hand the title a placeholder (`LEONIDA_REFUSED_PSO_PLACEHOLDER=0`: null); the package splash logs that it was presented or why not |
| `shims/d3d12_xs.cpp` | the package splash is configured from `GAME:\` at the first device |
| `shims/d3d12_xs_native_pso.h` | census of the factory's outcomes (plan status, stage kinds, device answer) |
| `shims/d3d12_title_image_upload.h` | the transfer waits on the GPU for the D3D12 submissions so far |
| `shims/d3d12_com.h` | `make_refused_pipeline_state` |
| `src/vulkan_backend.h` | `write_staged_title_image` and `write_host_texture` wait for the D3D12 queues' timelines, not `vkDeviceWaitIdle` (`LEONIDA_TITLE_UPLOAD_IDLE_WAIT=1`: as before) |
| `src/vulkan_replay.h` | `GraphicsPipeline::rectangle_by_vertex` (draw as instances of 6 vertices, vertex buffers bound per corner); `refused` pipelines skipped and counted |
| `src/vk_run_counters.h` | `outcome()` census lines, `current_reason()` |
| `platform/win32_host.{h,cpp}` | `guarded_copy`, `guarded_compare`, `guarded_gather`, `fault_guard_recovered`; `AddVectoredExceptionHandler` routed to `fault_guard` |
