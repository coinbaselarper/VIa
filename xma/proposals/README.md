# port/xma proposals (not applied)

Apply from `port/`, in order, after the dry-runs pass:

```sh
for p in 01-acphal-xma-host 02-cmake-xma 03-shims_gdk-xma-title-test; do patch -p1 --dry-run < xma/proposals/$p.patch || break; done
for p in 01-acphal-xma-host 02-cmake-xma 03-shims_gdk-xma-title-test; do patch -p1 < xma/proposals/$p.patch; done
```

| Patch | Files | What |
|---|---|---|
| `01-acphal-xma-host.patch` | `shims_gdk/src/acphal.cpp` | Under `GTA6RT_HAVE_XMA` (defined by linking `gta6rt_xma`): `AcpHalAllocateShapeContexts` binds the XMA array to `gta6rt_xma_bind` when `gta6rt_xma_wanted()` (GTA6RT_AUDIO on/null, or GTA6RT_XMA=1), `SubmitCommand` routes XMA commands to `gta6rt_xma_command` while bound, `AcpHalReleaseShapeContexts` unbinds. Otherwise the existing `XmaEngine` runs unchanged (silence). Without `GTA6RT_HAVE_XMA` the file compiles to the old code. |
| `02-cmake-xma.patch` | `CMakeLists.txt`, `shims_gdk/CMakeLists.txt` | `add_subdirectory(xma)` after `audio_out`; `gta6rt_shims_gdk` links `gta6rt_xma` (PRIVATE) when the target exists. |
| `03-shims_gdk-xma-title-test.patch` | `shims_gdk/CMakeLists.txt`, new `shims_gdk/tests/acphal_xma_title_test.cpp` | Shim test that drives acphal.dll the way the title does (port/xma/tests/title_driver.h); CTest runs it twice, `GTA6RT_AUDIO=off` (old silent path) and `GTA6RT_AUDIO=null` (host decode). |

Build requirement: `cargo` with the `x86_64-apple-darwin` and `aarch64-apple-darwin` targets (`rustup target add
x86_64-apple-darwin`). The crate builds offline (`--offline --locked`, vendored dependency). Without cargo, pass
`-DGTA6RT_XMA_DECODER_LIB=<libgta6rt_xma_decoder.a for that arch>` or configure fails with a named error.

Checks after applying (per arch, serially): `shims_gdk/acphal_test` (166/166 with GTA6RT_AUDIO unset),
`acphal_xma_title_test` with `GTA6RT_AUDIO=off` (9/9) and `=null` (12/12), `xapu_chad_test` (33/33),
`silent_audio_test` (129/129), `gdk_link_check`. `acphal_test` with `GTA6RT_AUDIO=null` fails 6 legacy checks by
design (see port/xma/STATUS.md section 5).
