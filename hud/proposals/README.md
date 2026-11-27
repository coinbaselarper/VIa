# Integration proposals for port/hud

Three patches, one per module, each checked with `patch -p1 --dry-run` from the repository root against the
sources of 2026-11-26. None is applied. Apply in any order (they touch disjoint files):

```sh
cd /Users/a/Desktop/Projects/GTA-VI
for p in host d3d12_vulkan metal; do patch -p1 --dry-run < port/hud/proposals/$p.patch && patch -p1 < port/hud/proposals/$p.patch; done
```

Re-check with `--dry-run` first: other workers edit these files, and a hunk that no longer applies needs its context
refreshed (every hunk is 1 to 12 lines).

| patch | files | what it does |
|---|---|---|
| `host.patch` | `host/CMakeLists.txt`, `host/src/main.cpp`, `host/src/win/appkit_window.mm` | builds `port/hud` into the host (`add_subdirectory` + link), `--hud MODE` and `GTA6RT_HUD`, attach/detach in `host_win_attach/detach` (the window exists, main thread), F1/F2 in the existing key monitor, driver/profile/import-counter strings, `gta6rt_hud_shutdown()` before `_exit` |
| `d3d12_vulkan.patch` | `CMakeLists.txt`, `src/hud_feed.h` (new), `src/vk_run_counters.h`, `src/vulkan_replay.h`, `src/vulkan_backend.h`, `shims/d3d12_xs_device.cpp`, `shims/d3d12_pipeline_compile.h`, `shims/d3d12_vulkan_device.cpp` | one present hook (`gta6rt_hud_note_present` + the vk-counters published as running totals), a dispatch counter, the compiler's pending gauge, textures waiting, GPU busy time from the existing fence callback |
| `metal.patch` | `CMakeLists.txt`, `src/mtl_queue.mm`, `src/mtl_present.mm`, `src/mtl_device.mm` | present/draw/dispatch/submission counts, CPU time in `execute`, per-command-buffer GPU time (`GPUStartTime`/`GPUEndTime`), present-to-display latency, compile-queue gauge |

Every producer call is `if (!active) return` on one relaxed load while the overlay is off, so the default (no
`--hud`, no `GTA6RT_HUD`) costs the producers a load and a branch per call and creates no window, no timer and no
dispatch source.
