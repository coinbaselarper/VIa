# Developing VIa

Native Mach-O host for the x86-64 Windows title, built on leonida (GPL-3.0-or-later; see `LICENSE`).
x86_64 (runs under Rosetta 2) is the shipped build; arm64 is kept building from the same source (X5).
Plan and decisions: `../PLAN.md`, `../TASKS.md`, `../NOTES.md` §S0.6. Module interfaces: `INTERFACES.md`.

## Layout

| dir | target(s) | task | state |
|---|---|---|---|
| `CMakeLists.txt` | top level | integrator | arch checks, module list, common options |
| `include/gta6rt/` | `gta6rt_interfaces` | — | module interface headers (C), see INTERFACES.md |
| `compat/` | `gta6rt_wincompat`, `gta6rt_dxheaders`, `gta6rt_dxguids`, `gta6rt_vulkan_headers`, `compat_abi_test` | shared | Windows header set, see compat/README.md |
| `portable/` | `gta6rt_portable`, `gta6rt_d3d12_portable`, `portable_link_check` | shared | leonida files that compile unchanged (`ORIGIN.md`) |
| `os/` | `gta6rt_os` | S1.1 | placeholder |
| `isa/` | `gta6rt_isa` (+ `gta6rt_isa_backend` from `isa/rosetta` or `isa/fexcore`) | S1.2, X2/X5 | placeholder + leonida `host_cpu` |
| `seh/` | `gta6rt_seh` | S1.3 | placeholder |
| `loader/` | `gta6rt_loader` | S1.4 | placeholder |
| `shims_core/` | `gta6rt_shims_core` | S1.5a | placeholder |
| `shims_gdk/` | `gta6rt_shims_gdk` | S1.5b | placeholder |
| `d3d12_vulkan/` | `gta6rt_d3d12_vulkan` | S1.6 | placeholder |
| `host/` | `gta6rt` executable | integrator | skeleton main that calls each module once |
| `third_party/` | — | pinned | DirectX-Headers v1.618.2, Vulkan-Headers 1.4.363 (`third_party/README.md`) |
| `tools/compat_census.sh` | — | shared | compile all leonida runtime TUs against compat, both arches |
| `build/` | — | — | build trees only, never sources |

## Build

Requirements (measured on this machine): Apple clang 21.0.0, macOS SDK 27.2, CMake ≥ 3.24 (4.5.1 here),
Ninja, python3 (generator for `bcpack_tables.inc`). No deployment target is pinned yet (X8).
One architecture per build tree; `CMAKE_OSX_ARCHITECTURES` selects it and `GTA6RT_ISA_BACKEND` follows it
(rosetta on x86_64, fexcore on arm64). `_WIN32` is never defined.

```sh
cd port
cmake -S . -B build/<module>-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake -S . -B build/<module>-arm64  -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64  -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build/<module>-x86_64 -j3 [target]
ninja -C build/<module>-arm64  -j3 [target]
```

- **Per-module build trees:** each worker uses `build/<module>-<arch>` (`build/os-x86_64`, `build/os-arm64`,
  `build/isa-arm64`, ...). Never build in another module's tree. The integrator uses `build/host-<arch>`;
  `verify.sh s1` expects the shipped binary at `build/macos/gta6rt` (x86_64), which the integrator produces.
- **Configuring a subset:** `-DGTA6RT_MODULES="os;isa"` adds only those module dirs (compat and portable are
  always added; host only with all modules). Use it when another module's CMakeLists is mid-edit. List your
  module's dependencies too (INTERFACES.md link graph), e.g. seh needs `os;isa;seh`.
- **Parallelism:** at most `-j3`. Compiling for x86_64 runs the native arm64 clang and is safe; only running
  x86_64 binaries goes through Rosetta.
- **Running x86_64 binaries:** one at a time, never in parallel loops, always under a timeout (background +
  kill). On 2026-11-25 many parallel Rosetta processes wedged new x86_64 processes; if one hangs before main,
  wait a few minutes and retry instead of starting more. There is no `timeout(1)` on this machine.
- Keep ≥ 60 GiB free on the data volume (`df -h /System/Volumes/Data`).

Smoke checks after a build (all exist from the start):

```sh
build/<tree>/compat/compat_abi_test        # prints ... "compat_abi_test: PASS", exit 0
build/<tree>/portable/portable_link_check  # dxbc_checksum[0]=cd0553e5 (same value as the scout's phase-0 run)
build/<tree>/host/gta6rt                   # arch, isa backend, one status line per module
```

## Rules for shared files

1. **Your module directory is yours.** Edit only `port/<your module>/` (and your own `build/<module>-*`).
   Replace your `*_placeholder.cpp`; add sources to your own `CMakeLists.txt`.
2. **`include/gta6rt/<your module>.h`** is owned by the implementing module. Additions are fine (bump
   `GTA6RT_*_API_VERSION`); changing or removing a signature or field needs the lead, because other workers
   compile against it. Keep these headers C and free of `windows.h`.
3. **`compat/` is shared and additive.** Add declarations to the SDK-named header they belong in, never change
   an existing type, value or layout, add a layout assert to `compat/tests/abi_test.cpp` for every new
   guest-visible struct, and keep both arches building. Do not add headers whose names exist in the macOS SDK.
   If two workers need the same new header, the first one creates it, the second appends.
4. **`portable/` holds verbatim leonida copies** that more than one module needs. Copy a leonida file into
   `portable/` only if it compiles unchanged on both arches; otherwise copy it into your module and port it
   there. Record every copy and every modification in `portable/ORIGIN.md`. A leonida file lives in exactly
   one place in `port/`.
5. **Never edit `leonida/`, `VIctory/`, `exe/`, `xbox/`, `PLAN.md`, `TASKS.md`, `NOTES.md`, `verify.sh`.**
   Copy, then edit the copy.
6. **Top-level `CMakeLists.txt`, `host/`, `third_party/`** are the integrator's. Ask for new pins or global
   options instead of editing them.
7. **No Rosetta- or FEX-specific code outside `isa/`** (X2). No `gs:` access, `thread_set_tsd_base`, `rdgsbase`
   or FEX headers anywhere else; use `include/gta6rt/isa.h`.
8. **Both arches build at every hand-off.** A module that cannot do something on arm64 yet returns
   `GTA6RT_E_UNSUPPORTED` there; it does not break the arm64 build.
9. Comments: minimal, only for non-obvious logic. No `.m`/`.mm` file includes `windows.h`.

## Known open items for the lead

- Deployment target (X8) is unpinned; builds use the SDK default.
- x86_64 linking against Homebrew libraries (SDL3, Vulkan loader/driver) is not possible: Homebrew here is
  arm64-only. The x86_64 Vulkan driver under Rosetta is the open S0.6 gate.
- The host link layout (`-pagezero_size`/`-image_base` to keep 0x140000000 and the title's ~52 GiB of
  reservations free) is not set; S0.3 mapped at 0x140000000 with the default layout.
