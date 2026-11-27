# third_party/fex: vendored libFEXCore (arm64 hosts only)

| item | value |
|---|---|
| upstream | https://github.com/FEX-Emu/FEX, commit `3ee25399232a6431c77c6314c437ea523ec29d0c` (2026-11-25), the X6 spike pin |
| licence | MIT (`src/LICENSE`); bundled externals keep their own licences (`src/External/*/LICENSE*`) |
| copied from | `spike/x6-fexcore/src/fex` (the spike's clone with `patches/0001` applied), without `.git` |
| used by | `isa/fexcore` only (`include("${GTA6RT_THIRD_PARTY}/fex/fex.cmake")`); never built for x86_64 |

## What is here

- `src/`: FEX at the pin plus the two patches below. Pruned (not needed to build FEXCore, see `verify_vendor.sh`):
  `Source/` (Linux and Windows frontends), `ThunkLibs/`, `docs/`, `unittests/`, `Data/{AppConfig,binfmts,CI,nix}`,
  `External/{vixl,zydis,rpmalloc,Catch2,Vulkan-Headers,drm-headers,fex-*,jemalloc_glibc,tracy,code-format-helper}`,
  the tests/docs/examples of `External/{fmt,range-v3,unordered_dense,xxhash}`, and the top-level
  `AGENTS.md`, `CLAUDE.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, nix files. Kept submodules: fmt `c07e2aa`,
  range-v3 `ca1388f`, unordered_dense `3234af2`, xxhash `e626a72`; tiny-json, SoftFloat-3e, cephes are in-tree.
- `patches/0001-fex-darwin.diff`: the X6 Darwin patch (18 files, +131/-13; table in `spike/x6-fexcore/RESULT.md`).
- `patches/0002-callback-level-sp.diff`: new here. The JIT callback entry stores its stack pointer in
  `CpuStateFrame::ReturningStackLocation`, so the embedder can re-enter that callback level at
  `AbsoluteLoopTopAddressFillSRA` after a guest exception. 7 lines, `__APPLE__` only.
- `compat/`: the spike's Darwin shim (`fex_darwin_compat.{h,cpp}`, `malloc.h`, `syscall.h`, `linux/`, `sys/`),
  force-included into every FEX translation unit and into `isa/fexcore`'s C++ sources (FEX headers use `SYS_futex`
  and `MADV_*` inline). Changed from the spike (review round 3): futex emulates Linux on `os_sync_wait_on_address`;
  a wake filtered by bitset (`FUTEX_WAKE_BITSET` without `MATCH_ANY`) wakes every waiter, because `os_sync` cannot
  select by bitset and FEX's `WritePriorityMutex` deadlocked when "wake one writer" woke a reader; `FUTEX_WAIT_BITSET`
  honours its absolute deadline; unknown futex ops and unimplemented syscalls log `[fex compat] ... unimplemented`
  and return ENOSYS. Tested by `isa/fexcore/tests/fex_futex_test`.
- `fex.cmake`: the build (below). `verify_vendor.sh`: rebuilds `src/` from a FEX clone (`git archive` of the pin and
  of the four submodules, then the patches) and compares; it printed `verify_vendor: PASS` on 2026-11-26.

FEX's `AGENTS.md` says AI must not be used to generate code for contributions to FEX. Both patches are AI-written, so
they stay a private fork and are rebased by us on every pin bump; an upstream Darwin port would have to be written by
a person.

## Build (`fex.cmake`)

- FEX is its own CMake project (own options, generated headers under its build dir), so it is configured and built in
  `${CMAKE_BINARY_DIR}/third_party/fex-build` with `-DCMAKE_BUILD_TYPE=Release` (`GTA6RT_FEX_BUILD_TYPE`), clang from
  the outer project, `-arch arm64`, LTO/jemalloc/FEX allocator/ccache/tests off.
- The first build runs at **configure time** with `GTA6RT_FEX_JOBS` (default 3) jobs, so it never overlaps the outer
  ninja (project rule: at most `-j3`). Measured: 32.7 s for the whole configure of `build/fexcore-arm64` including
  FEX's 170 steps (load average about 6). It is redone only when the patches, the compat files or the arguments change
  (a stamp holds their hash).
- At build time `gta6rt_fex_libs` re-runs FEX's own ninja (a no-op unless `src/` changed) with the same job count.
- Result: INTERFACE target `gta6rt_fex` (7 static archives: FEXCore, FEXCore_Base, JemallocDummy, fmt, xxhash,
  softfloat_3e, cephes_128bit; include dirs; `ARCHITECTURE_arm64=1`; the compat force-include for C++).
- Warnings: 0 `warning:` lines in FEX's build log.
