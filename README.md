# VIa

A native macOS host for the Xbox Series build of a certain open-world game: no Wine, no CrossOver, no virtual machine.
The game's own x86-64 executable runs unchanged under Rosetta 2; VIa supplies everything it expects from the console
around it and draws through Metal.

**This repository contains no game files, code or assets, and none are provided anywhere.** You need your own copy of
the game's Xbox Series package and executable. VIa is not affiliated with or endorsed by the game's developer or
publisher.

## Status

Experimental. On the one Mac it has been run on, the native Metal path reaches gameplay, but it is far from playable.

| | State |
|---|---|
| Boot, menus, intro movie with sound, first-run pages | Working |
| Story load into the open world | Working |
| Lit world on screen, HUD, the game hands over control | Working, with visual faults |
| Keyboard and controller input in the world | Working |
| Audio | Working |
| Frame rate in the world | About 2 frames/s (1.8-3) |
| Run length | Runs end 1-3 minutes into the world, stopped by the memory guards (below) |
| Visual faults | Speckled surfaces, black minimap, dark and pale blocks, missing parts of the frame for the first seconds |
| Vulkan path (MoltenVK) | Boots and loads; does not draw the world |
| Apple silicon without Rosetta (embedded FEXCore) | Reaches the game's `WinMain` only |

Memory: 12-13.5 GiB footprint in the world, about 6.7 GiB of it wired (GPU memory that cannot be paged out).

Tested hardware: **one** M4 Pro MacBook Pro with 24 GB, macOS 27.0.1. Nothing else has run it. Machines with less
memory are not expected to work yet.

## How it works

```
  game executable (x86-64 Windows PE, unchanged)
            |
  VIa host (Mach-O, x86_64 under Rosetta 2)
    loader        maps the PE and the packaged DLLs, relocations, imports, TLS
    isa           x86-64 execution: Rosetta 2 today, embedded FEXCore for native arm64
    os, seh       Win32 primitives and structured exception handling over .pdata
    shims         the console runtime: storage, input, audio, saves, users (offline)
    d3d12_metal   the game's Direct3D 12 calls on Metal (the path that reaches gameplay)
    d3d12_vulkan  the same calls on Vulkan through MoltenVK (development only)
            |
  Metal -> Apple GPU
```

The game's GPU shaders are console machine code. They are translated once on your Mac: carved from your package,
translated to SPIR-V, then compiled to Metal libraries (about 29,000 shaders), with several sets of rebuilt variants
("overlays") on top and a cache of the world's pipelines. `tools/setup.sh` does all of it.

Networking is offline by design: sockets are loopback-only and nothing leaves the machine.

## Requirements

- An Apple silicon Mac with 24 GB of memory or more. macOS 26.4 or later is what the binary declares; only 27.0.1 has
  been tested.
- Rosetta 2 (`softwareupdate --install-rosetta`).
- Xcode with the Metal toolchain (`xcodebuild -downloadComponent MetalToolchain` if `xcrun metal` is missing).
- Homebrew: `brew install cmake ninja python llvm spirv-cross spirv-tools`.
- Rust with the x86_64 target: `rustup target add x86_64-apple-darwin`.
- About 35 GiB of free disk for setup, plus room to spare.
- Your own copy of the game's Xbox Series package (version 1.0.2.1) and its executable.

## Build

Two binaries, both x86_64:

```sh
# the Metal binary that tools/play.sh runs: build/metal-play-x86_64/bin/gta6rt
d3d12_metal/tools/build_play.sh

# the plain host (Vulkan path only): build/macos/gta6rt
cmake -S . -B build/macos -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build/macos gta6rt
```

Each takes one to two minutes on an M4 Pro. `tools/setup.sh` builds the Metal binary too. Module layout, tests and
conventions are in [DEVELOPING.md](DEVELOPING.md).

## Setup

```sh
tools/setup.sh --dry-run --package /path/to/package --exe /path/to/game.exe   # what it will do, how long, how much disk
tools/setup.sh --package /path/to/package --exe /path/to/game.exe
```

It runs, in order: a check of the tools above, the binaries, shader extraction and translation (about 3 h, 22 GiB),
the Metal shader store (about 5 h, 5.4 GiB), the overlays (about 1 h, 1 GiB), the world pipeline cache (1.5-3 h,
3.1 GiB, specific to your Mac and macOS build), and a small config file for the launcher. Expect 10-12 hours and
about 32 GiB on an M4 Pro; the times are partly measured, partly estimated (the dry run says which). It can be
stopped and started again: finished stages are skipped and the long ones resume. `--data DIR` puts the large outputs
on another disk. Your game files are only read, never copied.

Some overlays and the pipeline cache are built from lists of shader names and pipelines recorded while the game ran
(`--lists DIR`). Those lists come from the game and are not in this repository. Without them setup builds the base
store and the overlays it can find by itself; the first runs are then slower and the world stays dark for a minute or
more while its pipelines compile. After a run, `tools/setup.sh --only tier --tier-from run/play/metal-archive`
builds the cache from what that run recorded.

## Run

```sh
tools/play.sh             # native Metal path
tools/play.sh --dry-run   # the checks, guards, environment and command, without starting
```

The window stays black for 15-40 s while it boots. Keyboard: W/A/S/D move, I/J/K/L camera, Space or Return = A,
Backspace = B, Escape = pause, arrows = D-pad; a controller works too; `GTA6RT_MOUSE=on` for mouse look. The overlay
shows frame time and memory (F1 cycles it, F2 hides it). State, saves and logs go to `run/play` (`--run-dir`).

The launcher refuses to start on a Mac without Apple silicon, without Rosetta, on a macOS older than the binary needs,
or with less than 24 GB (`--force` starts anyway, untested). It runs the game with the guards every test run here
uses: a memory footprint cap of 60% of the machine's memory, the end of the run at the first GPU error or a GPU
command buffer over 2 s, and a check of the machine's wired memory 4 times a second that ends the run before the Mac
can stall. These guards are what end runs today.

`tools/play.sh --vulkan` starts the Vulkan path from `build/macos/gta6rt`; it needs `GTA6RT_VK_DRIVER` (an x86_64
MoltenVK dylib) and a shader corpus for Apple Vulkan drivers (`shaders_vk/`), and it does not draw the world.

## Credits and licence

VIa is built on [leonida](https://github.com/ubiquitarianism/leonida) and takes findings from [VIctory](https://github.com/Marvisak/VIctory) and its forks; each ported file's
origin is recorded in the `ORIGIN.md` next to it. Vendored third-party code under `third_party/` and `xma/` keeps its
own licence. Everything else is GPL-3.0-or-later; see [LICENSE](LICENSE).
