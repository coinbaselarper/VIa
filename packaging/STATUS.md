# packaging status (2026-11-26)

`make_app.sh` builds `build/packaging/GTA6RT.app` around the x86_64 host. No game data is ever copied into the bundle.

```sh
packaging/make_app.sh [--binary build/macos/gta6rt] [--out build/packaging] [--version 0.1.0]
                      [--min-macos 14.0] [--hardened] [--allow-minos-mismatch]
```

## Bundle layout

| Path | Content |
|---|---|
| `Contents/Info.plist` | `LSMinimumSystemVersion` 14.0 (pinned, see below), id `org.gta6rt.host` |
| `Contents/MacOS/GTA6RT` | launcher (bash, `launcher.sh`) |
| `Contents/MacOS/gta6rt-host` | x86_64 host binary, LC_RPATH `@executable_path/../Frameworks`, signed with `entitlements.plist` |
| `Contents/Frameworks/libMoltenVK.dylib` | x86_64 MoltenVK 1.4.2, id `@rpath/libMoltenVK.dylib` (default driver) |
| `Contents/Frameworks/libvulkan_kosmickrisp.dylib` | x86_64 KosmicKrisp 26.3.1, id rewritten to `@rpath/...` |
| `Contents/Resources/entitlements.plist` | copy for reference |

The host binary is named `gta6rt-host`: the filesystem is case-insensitive, so `gta6rt` and the launcher `GTA6RT` collide.
Driver sources default to `spike/s1-vulkan-x86/drivers/...`; override with `GTA6RT_PKG_MOLTENVK` / `GTA6RT_PKG_KOSMICKRISP`.
The host loads a driver by path (`--vk-driver`), so the dylibs are found through `@rpath`/the launcher, not through an ICD json.

Entitlements (`entitlements.plist`): `cs.allow-jit`, `cs.allow-unsigned-executable-memory` (guest image/RWX mappings, and MAP_JIT for the arm64/FEX build), `cs.disable-library-validation` (dlopen of the driver dylibs), `cs.allow-dyld-environment-variables`. Signing is ad hoc (`codesign --sign -`), nested dylibs first, then the host with entitlements, then the bundle; `codesign --verify --deep --strict` passes. `--hardened` adds `--options runtime`; `--map-only` also runs under it.

## Launcher (`launcher.sh`)

1. Config file `~/Library/Application Support/GTA6RT/config` (`PACKAGE=`, `EXE=`, `VK_DRIVER=`; `VK_DRIVER` is `moltenvk` (default), `kosmickrisp` or a dylib path). Env `GTA6RT_PACKAGE`, `GTA6RT_EXE`, `GTA6RT_VK_DRIVER` override it.
2. If the package is missing: first-run folder picker (`osascript choose folder`), saved to the config. The exe is searched under the package (depth 2, `game_scarlett_master.exe`); if absent a file picker asks. Cancelling shows a dialog and exits 1. The title package in this project keeps its exe outside the package (`exe/`), so expect the second picker there.
3. Writable state: `~/Library/Application Support/GTA6RT/run` (`--run-dir`: overlay, registry, saves, drives). Logs go to `~/Library/Logs/GTA6RT/gta6rt.log` when stdout is not a terminal.
4. Every other argument is passed through to the host (`--map-only`, `--trace`, `--watchdog N`, `-- title args`...). Launcher-only flags: `--reset-config`, `--print-config`. Finder's `-psn_*` is dropped. Environment (`LEONIDA_*`, `GTA6RT_*`) is inherited.

## Minimum macOS (X8)

- **Pinned: macOS 14.0** (`--min-macos`, `LSMinimumSystemVersion`). Reason: MSL 3.1 (`-std=metal3.1`, storage-image atomics, S5.2 fix 1) needs macOS 14.
- **Rosetta 2:** Apple states availability through macOS 27 (task statement; not verified against Apple documentation here). The x86_64 host depends on it, so the product ceiling is whatever Apple ships Rosetta for; the arm64/FEX build has no such dependency.
- **Measured, not yet true in the binary:** `build/macos/gta6rt` has `LC_BUILD_VERSION minos 27.0` (no `CMAKE_OSX_DEPLOYMENT_TARGET` is set, so it defaults to the build OS) and the bundled KosmicKrisp is `minos 27.0`; MoltenVK is `minos 12.0`. Until the host is configured with `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0` and the SDK-symbol use checked, the binary will not load on macOS 14-26. `make_app.sh` refuses such a binary unless `--allow-minos-mismatch` is given. Integrator hook: add `set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "")` before `project()` in `port/CMakeLists.txt` (or pass it at configure) and rebuild; macOS 14 itself was not available to test.

## Verified (this machine, macOS 27.0.1, no other gta6rt running per `pgrep`)

- `make_app.sh --allow-minos-mismatch`: rc 0, bundle 28 MB, ad-hoc signature, entitlements present, rpath present, `plutil -lint` ok.
- Launch to `--map-only` through `GTA6RT.app/Contents/MacOS/GTA6RT --map-only` with a config file (HOME redirected to a scratch directory): rc 0, host mapped exe + DLLs, "exe fixups ok", `--map-only: not running any image code`, 12 unimplemented imports listed (same 12 as S1). State dir `run/{overlay,saves,drives}` created under Application Support. Same result for the `--hardened` build.
- Not tested: the first-run picker (non-interactive session, no GUI), a full run past `--map-only`, Finder double-click, macOS 14.
- Integrator launch test: `GTA6RT_PACKAGE=<xbox/...> GTA6RT_EXE=<exe/...exe> build/packaging/GTA6RT.app/Contents/MacOS/GTA6RT --map-only` (check `pgrep -f gta6rt-host` and `pgrep -f build/macos/gta6rt` first).

## Integration

Standalone, nothing to add to `port/CMakeLists.txt`. Optional later: a custom target calling `packaging/make_app.sh --binary $<TARGET_FILE:gta6rt>`.

## GTA6RT-Play.app: thin Metal launcher over the workspace (2026-11-27, round 12)

```sh
packaging/make_play_app.sh [--out build/packaging] [--workspace ..] [--version 0.2.0] [--min-macos 14.0]
build/packaging/GTA6RT-Play.app/Contents/MacOS/GTA6RT [--vulkan|--metal] [--dry-run] [newgame] [gta6rt options]
```

- Bundle (32 KB): `Info.plist` (id `org.gta6rt.play`), `MacOS/GTA6RT` = `play_launcher.sh`, `Resources/workspace` (the
  workspace path), `_CodeSignature` (ad hoc). No binary, game data, shader library or pipeline archive is copied.
- Metal (default) = `./play.sh metal`: binary `port/build/metal-play-x86_64/bin/gta6rt` rebuilt by
  `d3d12_metal/tools/build_play.sh` when the sources are newer (no prompt; `GTA6RT_APP_BUILD=0` skips, a failed build
  keeps the old binary; `PLAY_METAL_BIN` overrides), the gta6rt slot wait, footprint cap 14,848 / 13,312 MiB with a
  heavy job, `GTA6RT_METAL_CB_ABORT_MS=2000`, `GTA6RT_METAL_GPU_ERRORS` unset (stop at the first GPU error), the 4 Hz
  guard (wired >= 9,728 MiB or a `[metal]` command-buffer failure / FAULTED line kills the run), start refused at
  8,192 MiB wired, private save copy `port/run/play-saves`, archive `port/run/play-metal-archive`, `--hud min`,
  `GTA6RT_AUDIO=on`, `PLAY_METAL_POOL=0`. Log `port/run/logs/app-metal.log`.
- Vulkan: `--vulkan` or `GTA6RT_APP_GPU=vulkan` = `./play.sh` (binary `port/build/macos/gta6rt`, log `app.log`).
- Workspace-relative: `GTA6RT_SOURCE_ROOT=<ws>/port` (shader store, overlay list, catalog, `build/pc7/arch-world`
  tier) and explicit `--exe/--package/--run-dir` under the workspace; cwd is the workspace. Workspace =
  `GTA6RT_WORKSPACE`, else `Resources/workspace`, else the bundle's location inside `port/build/packaging`.
- Under Finder: PATH gets `/opt/homebrew/bin` (cmake/ninja for the build), a notification while building, a dialog
  when a guard ends the run (`GTA6RT_APP_DIALOGS=0` turns it off). Inferred, not tested: macOS asks for Desktop-folder
  access the first time (the workspace is under ~/Desktop).
- Verified offline: `codesign --verify --deep --strict` ok; dry runs (`spike/r12-packaging/dryrun-*.txt`); with a fake
  game the env and argv equal `play.sh metal`'s except the added `GTA6RT_SOURCE_ROOT` and `--exe/--package/--run-dir`
  (same values as the compiled-in defaults); the GPU-error line and a faked wired reading end the run, the wired
  pre-check refuses, no helper left (details: `spike/r12-packaging/STATUS.md`).
