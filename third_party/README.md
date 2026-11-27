# third_party (pinned, read-only)

Do not edit these trees. To change a pin, replace the whole directory and update this file (lead decision).

| dir | version | source | licence | notes |
|---|---|---|---|---|
| `DirectX-Headers/` | tag **v1.618.2**, commit `dde59d560da2760fec612d6634124edc2a26b82f` | https://github.com/microsoft/DirectX-Headers (shallow clone; `.git`, `googletest/`, `test/` removed) | MIT | Newest tag with the 40-byte `D3D12_UNORDERED_ACCESS_VIEW_DESC` (title SDK 26100 layout); v1.619.0 added `D3D12_BUFFER_UAV_BYTE_OFFSET` and makes it 48 (spike/s1-host-build-scout/REVIEW.md item 4). `compat/tests/abi_test.cpp` asserts UAV 40, SRV 40, RESOURCE_DESC 56, HEAP_PROPERTIES 20 and builds on both arches. Not yet checked against the rest of leonida's static_asserts. `include/wsl/stubs/` is not on any include path: `compat/include` replaces it. |
| `Vulkan-Headers/` | 1.4.363 (`VK_HEADER_VERSION 363`) | copied from Homebrew `vulkan-headers 1.4.363.0` (`/opt/homebrew/Cellar/vulkan-headers/1.4.363.0/include`) | Apache-2.0 / MIT (`LICENSE.md`) | Same version leonida pins (vulkan-sdk-1.4.363.0). Headers only; the driver (KosmicKrisp / MoltenVK) is loaded at run time by d3d12_vulkan/. |

Not vendored yet (owners add them here):
- libFEXCore (isa/fexcore): spike/x6-fexcore pins FEX `3ee25399232a6431c77c6314c437ea523ec29d0c` plus an 18-file Darwin patch.
- SDL3 (shims_gdk gameinput): leonida pins SDL3 3.4.20; Homebrew's copy is under `/opt/homebrew/include/SDL3` (arm64 only).
- x86_64 builds of any Homebrew library: Homebrew on this machine is arm64-only, so the x86_64 tree cannot link Homebrew dylibs.
