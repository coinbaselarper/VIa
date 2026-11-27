# VIa

A native macOS host for the Xbox Series build of a certain open-world game: no Wine, no CrossOver, no virtual machine.
The game's own x86-64 executable runs unchanged; VIa supplies everything it expects from the console around it and
draws through Metal.

**This repository contains no game files, code or assets.** You need your own copy of the game. It is not affiliated
with or endorsed by the game's developer or publisher.

## Status

Work in progress. It boots and loads; it does not draw the open world yet.

| Stage | State |
|---|---|
| Game starts under the host (587 imports served, 0 unimplemented) | Working |
| Landing prompt, intro movie with sound, first-run setup pages | Working |
| Story load into the world phase | Working |
| First frame of the open world | Not yet |
| Native Metal renderer (no MoltenVK) | Reaches the story load; not yet the default |
| Apple silicon without Rosetta (embedded FEXCore) | Reaches `WinMain` only |

Measured on an M4 Pro with 24 GB: about 4 GB of memory at the menus and 11-13 GB at the world phase.

## How it works

```
  game executable (x86-64 Windows PE, unchanged)
            |
  VIa host (Mach-O)
    loader        maps the PE and the packaged DLLs, relocations, imports, TLS
    isa           x86-64 execution: Rosetta 2 today, embedded FEXCore for native arm64
    os, seh       Win32 primitives and structured exception handling over .pdata
    shims         the console runtime: storage, input, audio, saves, users (offline)
    d3d12_vulkan  the game's Direct3D 12 calls on Vulkan (through MoltenVK)
    d3d12_metal   the same calls on Metal directly
            |
  Metal -> Apple GPU
```

Networking is offline by design: sockets are loopback-only and nothing leaves the machine.

## Requirements

- An Apple silicon Mac, macOS 14 or later, 24 GB of memory for now (16 GB is the target).
- Xcode command-line tools, CMake 3.24 or later, Ninja, Python 3, a Rust toolchain with the
  `x86_64-apple-darwin` target.
- Your own copy of the game's Xbox Series package and executable.

## Build

```sh
cmake -S . -B build/macos -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build/macos gta6rt
```

The binary is still called `gta6rt`. Module layout, per-module build trees, tests and conventions are in
[DEVELOPING.md](DEVELOPING.md); interfaces between modules are in [INTERFACES.md](INTERFACES.md); the current state
of the whole port is in [STATUS.md](STATUS.md).

## Run

```sh
build/macos/gta6rt --exe /path/to/game.exe --package /path/to/package --hud min
```

`--help` lists the options. Useful ones: `--hud off|min|full|title` for the performance overlay (F1 cycles, F2
toggles), `GTA6RT_AUDIO=on` for sound, `GTA6RT_GPU=metal` for the native Metal renderer, `GTA6RT_MOUSE=on` for
mouse-as-camera. A keyboard works as a controller: Return is A, Backspace is B, arrows are the D-pad, WASD the left
stick, IJKL the right stick, Escape is Menu.

The game's shaders have to be translated once before the first run; the tools are under `shaders_vk/` and
`shaderc_msl/`.

## Credits and licence

VIa is built on [leonida](https://github.com/ubiquitarianism/leonida) and takes findings from [VIctory](https://github.com/Marvisak/VIctory) and its forks; each ported file's
origin is recorded in the `ORIGIN.md` next to it. Vendored third-party code under `third_party/` and `xma/` keeps its
own licence. Everything else is GPL-3.0-or-later; see [LICENSE](LICENSE).
