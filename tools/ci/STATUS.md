# tools/ci status (2026-11-26)

## Scripts

- `run_all.sh [--arch arm64|x86_64|both] [--units a,b] [--test-timeout S] [--build-timeout S] [--no-build] [--build-only] [--out DIR]` (wrapper for `ci_run.py`, python3 stdlib only). Never runs the gta6rt host.
- `title_smoke.sh [SECONDS] [gta6rt args]`: opt-in, for the integrator. Wraps `host/tools/run.sh` with `--stop-at-winmain`, exits 0 only when rc 0 and the log mentions WinMain; exit 125 if another gta6rt is running. Not run by me (host slot belongs to others).

## What run_all.sh does

1. Units are discovered, not listed: `port/` itself (`top`, all modules the top CMakeLists adds) plus every `port/<dir>/CMakeLists.txt` and `port/<dir>/<dir>/CMakeLists.txt` that calls `project()` (today: `metal`, `shaderc_msl`, `media/dev` as `media_dev`, and a new `io_bench` that appeared during the session).
2. Per arch, per unit: `rm -rf` the tree, `cmake -G Ninja -DCMAKE_OSX_ARCHITECTURES=<arch> -DCMAKE_BUILD_TYPE=RelWithDebInfo`, `ninja -k 0 -j 2` (keeps going after failures). Trees: `build/ci-arm64`, `build/ci-x86_64` for `top`; `build/ci-<unit>-<arch>` for the others. If the top configure fails with `CMake Error at <module>/CMakeLists.txt`, that module is dropped from `GTA6RT_MODULES` and configure is retried (up to 5 times; excluded modules are reported).
3. Failed ninja targets (`FAILED:` lines) are mapped to a module; configure/build failures are reported per module and the run continues.
4. Tests: every `CTestTestfile.cmake` root in each tree is run with `ctest --timeout 120 -j N --output-junit` (N = 2 on arm64, 1 on x86_64, so x86_64 Rosetta processes run one at a time). Tests whose executable is missing are `build_failed`. Modules with no CTest registration are listed as "no CTest tests discovered". Tests that a CTest property gives its own TIMEOUT keep it (`--timeout` is only the default).
5. Output in `build/ci-results/<stamp>/`: `summary.json` (builds, per-test records with module/arch/status/seconds/timestamp/detail, ctest dirs with rc/end time, totals, `ok`), `table.txt`, per-unit build logs, JUnit XML and ctest logs. Exit 0 only if every configure/build succeeded and every test passed (1 otherwise, 2 if under 60 GiB free). Runs at `nice 10`.

## Full run of the unit-test part (2026-11-26 11:25:22 to 11:41:20, 16 min, 145.8 GiB free at the end)

Build times (s): top arm64 115.8 / x86_64 138.3; metal 27.3 / 28.5; shaderc_msl 10.0 / 14.3; media_dev 84.0 / 103.2; io_bench 5.7 / 8.6.

```
module        arch     pass fail notrun
media_dev     arm64      41    0    0
media_dev     x86_64     41    0    0
metal         arm64      19    1    0
metal         x86_64     20    0    0
shaderc_msl   arm64       4    0    0
shaderc_msl   x86_64      4    0    0
shims_gdk     arm64      41    0    0     (top tree)
shims_gdk     x86_64     42    0    0     (top tree)
io_bench      both        -    -    -     builds, no CTest tests
totals: 212 pass, 1 fail, 2 build_failed (exit 1)
```

Failures (timestamps from `summary.json`):
- `top` arm64 and x86_64, 11:28 / 11:33: `FAILED: gta6rt`: link errors, undefined `host::win::MessageQueue::{post,peek,drop_window}` referenced from `host/src/window_shim.cpp`. The `host/` worker is mid-change (the first run at 11:14 failed the same way). Not touched. Nothing else in top failed to build.
- `metal_draw2` on arm64 only, 11:37:27: `test_draw2.cpp:351: d2 / d1 > 1.4 && d2 / d1 < 2.6 (timestamp deltas track the work)`; ratio 1.06 here, 4.43 in the earlier run at 11:21:48 (load average 6-10). Timing-based, load-sensitive; x86_64 passed both runs.
- `media_dev` arm64 and x86_64, 11:38 / 11:41: `gdk_dstorage` not built: `shims_gdk/tests/dstorage_test.cpp` includes `portable/src/bcpack_stream.h`, which needs `bcpack_tables.inc`, not on the include path in the `media/dev` tree. The same test builds and passes in the top tree (`gdk_dstorage` in `shims_gdk` rows).
- First run (11:14 to 11:25, same logic, before a table fix) gave 207 pass, 1 fail (`metal_draw2` arm64), 2 build_failed; the same three issues.

## Facts and caveats

- Only modules that register tests with `add_test` are covered: `shims_gdk` (top tree), `metal`, `shaderc_msl`, and the gdk/media tests reached through `media/dev`. `os`, `isa`, `loader`, `shims_core`, `seh`, `d3d12_vulkan`, `portable`, `compat` build test executables (for example `os_test_vm`, `loader_selftest`, `compat_abi_test`, `isa` rosetta tests) but do not register them with CTest, so run_all.sh does not run them. Integrator hook: `include(CTest)`/`enable_testing()` plus `add_test` in those modules' CMakeLists (module owners' files).
- The top-level `port/CMakeLists.txt` has no `enable_testing()`, so `ctest` at a tree root finds nothing; the script runs ctest per `CTestTestfile.cmake` directory instead. Adding `enable_testing()` to the top file would make `ctest --test-dir build/ci-<arch>` work as one call.
- On arm64, `isa/fexcore` configures and builds a vendored FEX with 3 jobs (`gta6rt: building vendored FEX with 3 jobs`), which is above the -j2 budget; that is in `port/isa`, not changed.
- Only unit runs were done. The title smoke run was not run.

## Integration

Standalone scripts, nothing to add to `port/CMakeLists.txt`. Optional: a `ci` custom target or CI job calling `tools/ci/run_all.sh`, and `tools/ci/title_smoke.sh` after a green run.
