# Developing VIa

Native Mach-O host for the x86-64 Windows title, built on leonida (GPL-3.0-or-later; see `LICENSE`).
x86_64 (runs under Rosetta 2) is the shipped build; arm64 is kept building from the same source (X5).
Module interfaces: `INTERFACES.md`. Each module's state is in its own `STATUS.md`; the whole port's in `STATUS.md`.
Plan and decision files (`PLAN.md`, `TASKS.md`, `NOTES.md`) live in the workspace above this repository and are not
part of it.

## Layout

| dir | target(s) | what |
|---|---|---|
| `CMakeLists.txt` | top level | arch checks, deployment target (default macOS 26.4), module list, common options |
| `include/gta6rt/` | `gta6rt_interfaces` | module interface headers (C), see INTERFACES.md |
| `compat/` | `gta6rt_wincompat`, `gta6rt_dxheaders`, `gta6rt_dxguids`, `gta6rt_vulkan_headers`, `compat_abi_test` | Windows header set, see compat/README.md |
| `portable/` | `gta6rt_portable`, `gta6rt_d3d12_portable`, `portable_link_check` | leonida files that compile unchanged (`ORIGIN.md`) |
| `os/` | `gta6rt_os` | Win32 primitives: memory, files, threads, sync |
| `isa/` | `gta6rt_isa` (+ `isa/rosetta` or `isa/fexcore`) | x86-64 execution: Rosetta 2 on x86_64, embedded FEXCore on arm64 |
| `seh/` | `gta6rt_seh` | `.pdata` unwinder and exception dispatch |
| `loader/` | `gta6rt_loader` | PE mapping, relocations, imports, TLS |
| `shims_core/`, `shims_gdk/` | `gta6rt_shims_core`, `gta6rt_shims_gdk` | console runtime: kernel, storage, DirectStorage, input, saves, users |
| `media/`, `audio_out/`, `xma/` | `gta6rt_media`, `gta6rt_audio_out`, `gta6rt_xma` | XAudio2 and movies, the CoreAudio output, XMA decode (Rust) |
| `hud/` | `gta6rt_hud` | performance overlay |
| `d3d12_vulkan/` | `gta6rt_d3d12_vulkan` | D3D12 on Vulkan (MoltenVK); also the D3D12 front end the Metal path reuses |
| `d3d12_metal/` | `gta6rt_d3d12_metal` (super-build: `superbuild/attach.cmake`) | D3D12 on Metal; `cache/` pipeline cache and `prepare_shaders`; `tools/build_play.sh` builds the play binary |
| `metal/` | `gta6rt_metal` | the Metal backend (devices, queues, pipelines, archives) |
| `shaderc_msl/` | `gta6rt_shaderc_msl` + Python tools | SPIR-V to Metal libraries: the shader store and the overlay generators (`tools/`) |
| `shaders_vk/` | Python tools | the shader translator copy (`translator/`, leonida's make_game.py and tools) and the Vulkan corpus tools |
| `host/` | `gta6rt` executable | main, window, input, title watchers; `tools/` run scripts |
| `io_bench/` | standalone | storage benchmarks; `scripts/ensure_bcpack_history.sh` |
| `packaging/` | scripts | app bundles around a checkout (`make_app.sh`, `make_play_app.sh`) |
| `tools/` | scripts | `setup.sh` (everything the Metal path needs from the game), `play.sh` (launcher with guards), `setup/`, `ci/`, `compat_census.sh` |
| `third_party/` | pinned | DirectX-Headers, Vulkan-Headers, FEX (`third_party/README.md`) |
| `build/`, `run/` | gitignored | build trees and setup outputs; run state, saves and logs. Never sources |

## Build

Requirements (measured on the development Mac): Apple clang 21.0.0, macOS SDK 27.2, CMake 3.24 or later (4.5.1),
Ninja, Python 3.10 or later, Rust with `x86_64-apple-darwin`. `tools/setup.sh` also needs Homebrew `llvm`
(llvm-mc), `spirv-cross` and `spirv-tools`, and Xcode's Metal toolchain; `tools/setup.sh --only preflight` checks
them. The deployment target defaults to macOS 26.4 for new build trees (`-DCMAKE_OSX_DEPLOYMENT_TARGET=` overrides);
a 14.0 build compiles but calls macOS 14.4, 15.0 and 26.4 APIs unchecked.
One architecture per build tree; `CMAKE_OSX_ARCHITECTURES` selects it and `GTA6RT_ISA_BACKEND` follows it
(rosetta on x86_64, fexcore on arm64). `_WIN32` is never defined.

```sh
cmake -S . -B build/<module>-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake -S . -B build/<module>-arm64  -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64  -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build/<module>-x86_64 -j3 [target]
ninja -C build/<module>-arm64  -j3 [target]
```

- **The play binary** (Metal path linked in): `d3d12_metal/tools/build_play.sh` configures
  `build/metal-play-x86_64` with `-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=d3d12_metal/superbuild/attach.cmake` and
  publishes `build/metal-play-x86_64/bin/gta6rt`.
- **Per-module build trees:** each worker uses `build/<module>-<arch>`. Never build in another module's tree.
- **Configuring a subset:** `-DGTA6RT_MODULES="os;isa"` adds only those module dirs (compat and portable are
  always added; host only with all modules). List your module's dependencies too (INTERFACES.md link graph).
- **Parallelism:** at most `-j3` on a shared machine. Compiling for x86_64 runs the native arm64 clang; only running
  x86_64 binaries goes through Rosetta.
- **Running x86_64 binaries:** one at a time, always under a timeout. Many parallel Rosetta processes have wedged new
  x86_64 processes; if one hangs before main, wait a few minutes and retry instead of starting more.
- Keep 60 GiB free on the data volume (`df -h /System/Volumes/Data`).

Unit tests: `tools/ci/run_all.sh --arch x86_64` (fresh trees under `build/ci-*`, results in `build/ci-results/`).
Tests that need the shader store or the corpus skip or fail without them (`pcache_*`, `metal_game_shaders`,
`metal_pipeline_cache_*`, `shaderc_msl_vs_lane`).

Smoke checks after a build:

```sh
build/<tree>/compat/compat_abi_test        # prints ... "compat_abi_test: PASS", exit 0
build/<tree>/portable/portable_link_check  # dxbc_checksum[0]=cd0553e5
```

## Game data

Nothing derived from the game goes into the repository: no shader binaries or listings, no SPIR-V, frame dumps, logs
with title data, saves, or name lists. `tools/setup.sh` writes everything it makes under `build/` (or `--data`), and
the launcher writes under `run/`. Both are gitignored. Keep file names generic (no release-group or package names).

## Rules for shared files

1. **Your module directory is yours.** Edit only `<your module>/` (and your own `build/<module>-*`).
   Add sources to your own `CMakeLists.txt`.
2. **`include/gta6rt/<your module>.h`** is owned by the implementing module. Additions are fine (bump
   `GTA6RT_*_API_VERSION`); changing or removing a signature or field needs the lead, because other workers
   compile against it. Keep these headers C and free of `windows.h`.
3. **`compat/` is shared and additive.** Add declarations to the SDK-named header they belong in, never change
   an existing type, value or layout, add a layout assert to `compat/tests/abi_test.cpp` for every new
   guest-visible struct, and keep both arches building. Do not add headers whose names exist in the macOS SDK.
4. **`portable/` holds verbatim leonida copies** that more than one module needs. Copy a leonida file into
   `portable/` only if it compiles unchanged on both arches; otherwise copy it into your module and port it
   there. Record every copy and every modification in `portable/ORIGIN.md`. A leonida file lives in exactly
   one place in the repository.
5. **Never edit the workspace folders beside this repository** (`leonida/`, `VIctory/`, `exe/`, `xbox/`) or
   `PLAN.md`, `TASKS.md`, `NOTES.md`, `verify.sh`. Copy, then edit the copy.
6. **Top-level `CMakeLists.txt`, `host/`, `third_party/`** are the integrator's. Ask for new pins or global
   options instead of editing them.
7. **No Rosetta- or FEX-specific code outside `isa/`** (X2). No `gs:` access, `thread_set_tsd_base`, `rdgsbase`
   or FEX headers anywhere else; use `include/gta6rt/isa.h`.
8. **Both arches build at every hand-off.** A module that cannot do something on arm64 yet returns
   `GTA6RT_E_UNSUPPORTED` there; it does not break the arm64 build.
9. Comments: minimal, only for non-obvious logic. No `.m`/`.mm` file includes `windows.h`.

## Known open items for the lead

- `.gitignore` excludes `*.metal`, so `metal/tests/shaders/*.metal` and `shaderc_msl/tools/soft_f64.metal` (sources)
  are not in the repository: 19 Metal unit tests and the store's Float64 shaders fail in a fresh clone.
- x86_64 linking against Homebrew libraries (SDL3, Vulkan loader/driver) is not possible: Homebrew is arm64-only.
  The Vulkan path loads an x86_64 MoltenVK dylib named by `GTA6RT_VK_DRIVER`.
- The overlay locations are compiled into `d3d12_metal/shims/d3d12_metal_device.cpp` as `build/<worker>-x86_64`
  paths; `tools/setup.sh` writes the overlays at those paths (proposal for one manifest file:
  `run/requests/d3d12_metal.md`).
