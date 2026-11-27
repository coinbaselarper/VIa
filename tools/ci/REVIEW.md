# Review: port/tools/ci and port/packaging (2026-11-26, 11:43-11:50)

Verdict: APPROVED_WITH_FIXES. The headline numbers are correct, and no script writes into xbox/, deletes outside port/build, or runs the host as part of CI. The fixes below are non-blocking.

## Headline numbers, recomputed from raw output

- `build/ci-results/20261126-112522/summary.json`, recounted from `tests[]`: pass 212, fail 1, build_failed 2, not_run 0. `ok=false`. Started 11:25:22, finished 11:41:20. Disk free 151.5 GiB at start, 145.8 GiB at end. This matches the report.
- Per module and arch, recounted from summary.json and the junit files: the counts match the builder's table. shims_gdk has 41 tests on arm64 and 42 on x86_64. The extra x86_64 test is `gdk_gameinput_mac_census`, from a junit set difference.
- Build failures:
  - top on arm64 (end 11:27:51) and x86_64 (end 11:33:08): `FAILED: gta6rt`, with undefined `host::win::MessageQueue::{drop_window,peek,post,size,posted}`. The 11:14 run shows the same failure, with 6 MessageQueue lines.
  - media_dev on arm64 and x86_64: `bcpack_stream.h:18: 'bcpack_tables.inc' file not found` while compiling dstorage_test.cpp.
- metal_draw2 on arm64: `ratio 1.06` (11:37:27). The 11:14 run shows `ratio 4.43`. Both match the report.
- Test binaries are the expected arch. `lipo` gives x86_64 for `ci-x86_64/shims_gdk/acphal_test` and `ci-shaderc_msl-x86_64/shaderc_msl_vs_lane_test`, and arm64 for `ci-arm64/shims_gdk/acphal_test`.

## Re-run (tests only, existing trees, output in scratchpad)

Command: `run_all.sh --no-build --units top,shaderc_msl,metal --arch both --out <scratchpad>/ci-review-rerun`. It ran 11:44:09 to 11:48:31 at load average 11 to 15, with rc 0:

| module | arch | pass | fail |
|---|---|---|---|
| metal | arm64 | 20 | 0 |
| metal | x86_64 | 20 | 0 |
| shaderc_msl | arm64 | 4 | 0 |
| shaderc_msl | x86_64 | 4 | 0 |
| shims_gdk | arm64 | 41 | 0 |
| shims_gdk | x86_64 | 42 | 0 |

metal_draw2 passed on arm64 this time, so the failure is intermittent. The pass and fail counts per row agree with the builder's run, apart from the draw2 flip.

## Safety

- `ci_run.py`:
  - `rmtree` only removes `port/build/ci-<arch>` and `port/build/ci-<unit>-<arch>`. Unit names come from paths relative to port/, so they cannot leave port/build.
  - It builds only default ninja targets. The shaderc_msl corpus targets that write to `port/build/shaderc_msl` are not ALL targets, so they never run.
  - CTest does not run gta6rt: no `add_test` in the discovered projects references it.
  - x86_64 ctest uses `-j1 --timeout 120`. Builds use ninja `-j2` under `nice 10`. It refuses to run with less than 60 GiB free.
- `make_app.sh`: only `rm -rf <out>/GTA6RT.app`. It copies the binary and the drivers.
- `launcher.sh`: it writes only under `~/Library/Application Support/GTA6RT` and `~/Library/Logs/GTA6RT`. It reads the package with `find -maxdepth 2` and passes `--run-dir` for the state. After the builder's map-only launch, `find xbox exe -newermt 11:00` finds nothing. The builder's state directory (overlay/.gta6rt-registry, saves, drives) and log are in the scratchpad HOME.
- Builder's launch log (scratchpad `home/Library/Logs/GTA6RT/gta6rt.log`): "12 bound to unimplemented stubs", "fixups ok" and "--map-only: not running any image code" are all present. The log contains only one run, so the claim that the `--hardened` build behaves the same cannot be checked from the artifacts.
- Bundle checks:
  - `codesign --verify --deep --strict` passes. The signature is ad hoc and the 4 entitlements are present.
  - The host has LC_RPATH `@executable_path/../Frameworks`. The dylib ids are `@rpath/...` and both dylibs are x86_64.
  - minos is 27.0 for the host and KosmicKrisp and 12.0 for MoltenVK. `LSMinimumSystemVersion` is 14.0.
  - The bundle is 28 MB.

## Non-blocking issues

1. **Missed test project.** `port/host/src/win/tests/CMakeLists.txt` is a standalone `project()` with `add_test(win_core)` and `add_test(win_shim)`. `discover_units` stops at depth 2 (`rel.count(os.sep) > 1`), so CI never builds or runs it. The builder's "untested modules" list does not mention host. Fix: allow deeper `project()` dirs that contain `add_test`, or add an explicit include list.
2. **Second-host guard is narrower than the docs say.** `title_smoke.sh` relies on `host/tools/run.sh`'s `pgrep -f build/macos/gta6rt`. That pattern misses hosts started from other paths. During this review, `build/os-host-x86_64/gta6rt.after3` was running, and so would `GTA6RT.app/.../gta6rt-host`. The launcher has no single-instance check at all. Fix: check `pgrep -f 'gta6rt|gta6rt-host'` in title_smoke.sh and the launcher. run.sh belongs to the host owner and should not be edited here.
3. **title_smoke PASS check is weaker than the S1 check.** It greps only for "winmain" with rc 0. verify.sh s1 also requires `=== REACHED MARKER: RVA 0x5a91bd0`, `args: rcx=0x140000000 rdx=0` and the `calls: ... (0 unimplemented)` line. The script also omits `--trace=0 --hidden-window` by default.
4. **Table conflates modules.** For a standalone unit tree, a build failure is reported under the failing source directory (`shims_gdk`), while its tests are reported under the unit (`media_dev`). Rows like `shims_gdk arm64 41 0 0 1` therefore look like a top-tree shims_gdk build failure. Fix: key build rows as `<unit>:<module>`.
5. **`--no-build` reports wrong build status.** In this mode every existing tree is reported as `build ... ok`, even when its last build failed. The re-run above shows `build top ok`, although gta6rt does not link.
6. **Possible harness factor in metal_draw2 (not measured).** arm64 runs ctest `-j2`, so the GPU-timestamp test can overlap other Metal tests. The builder's failures were on arm64 at -j2, and x86_64 at -j1 passed twice. This review's arm64 re-run at -j2 passed. Consider `RUN_SERIAL` or `-j1` for the metal unit.
7. **Hard-coded module lists.** `module_of` and the configure-exclusion `allmods` list are both hard-coded. A module added to port/CMakeLists.txt later is silently dropped when any exclusion triggers.
8. **Rosetta-through-macOS-27 claim not checked.** STATUS.md correctly labels it as taken from the task and unverified.

## Integrator hooks (unchanged from the builder's report)

- Neither piece needs an `add_subdirectory`.
- For the 14.0 pin to be true, set `CMAKE_OSX_DEPLOYMENT_TARGET=14.0` before `project()` in port/CMakeLists.txt.
- The KosmicKrisp dylib is minos 27.0 and needs its own rebuild, or the bundle has to fall back to MoltenVK only on 14 to 26.
