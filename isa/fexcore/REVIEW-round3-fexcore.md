# Review, round 3: isa/fexcore (arm64 backend on embedded libFEXCore)

Reviewer run, 2026-11-26 11:50-12:25, load average 8-24 from other workers. Everything below was measured in fresh trees
`port/build/review-fexcore-arm64` (os;isa), `review-fexcore-host-arm64` (all, `GTA6RT_FEX_HOST=ON`) and
`review-fexcore-host-x86_64` (all, stock `host/src`, Rosetta). Probes are in the session scratchpad, not in the tree.
Nothing outside `port/isa/fexcore/REVIEW-round3-fexcore.md`, `port/build/review-fexcore-*` and `port/run/logs/review-fexcore-*`
was written.

## Verdict: APPROVED WITH FIXES

The boot-to-WinMain claims reproduce exactly. One real defect was found that the builder's tests do not reach: a deadlock
in the vendored FEX Darwin shim when several guest threads compile code at the same time. It is not on the measured
boot path (one guest thread), but every title run past WinMain will hit it. It must be fixed before the integrator
relies on the arm64 backend for the frame loop.

## Defect 1 (blocking for integration, not for the measured boot path): futex wake ignores the bitset, deadlocks FEX's lock

`third_party/fex/compat/fex_darwin_compat.cpp`, `Futex()`:

```
if (Cmd == FUTEX_WAKE || Cmd == FUTEX_WAKE_BITSET) {
    int R = Val == 1 ? os_sync_wake_by_address_any(...) : os_sync_wake_by_address_all(...);
```

FEX's `WritePriorityMutex` (used by `LookupCache::FindBlock` and `AddBlockExecutableRange`, i.e. every block compile)
keeps readers and writers waiting on the same futex word and tells them apart with the wait/wake bitset
(`FUTEX_BITSET_WAIT_READERS` / `..._WRITERS`). `FutexWakeWriter` wakes one waiter. On Linux the bitset makes that waiter
a writer. The shim drops the bitset, so `wake_any` can wake a reader, which re-checks and sleeps again, and the writer
is never woken. The lock then has no owner and every thread sleeps.

Reproduction (probe `race`: 256 distinct small guest functions, each calling a FRAME thunk, N threads attach, each runs
all functions once in a different order, first execution so all compile concurrently; fresh process per iteration,
hang = still alive after 8 s):

| threads | hangs / 60 processes (unmodified backend) |
|---|---|
| 2 | 0 |
| 4 | 4 |
| 8 | 7 |
| 16 | 6 (and 2 of 20 in the first batch, rc=124 from `run_timeout.sh`) |

lldb backtrace of a hung process: 15 threads in `WritePriorityMutex::Mutex::lock_shared` from `LookupCache::FindBlock`
from `ContextImpl::CompileBlock`, one thread in `LookupCache::AddBlockExecutableRange` waiting for the write lock, main
thread in `pthread_join`; all in `fex_darwin_syscall` -> `os_sync_wait_on_address`.

Fix verified in scratch: a copy of the shim with the wake always `os_sync_wake_by_address_all` (waiters re-check the
word, spurious wakeups are harmless), linked ahead of the FEX archive: 0 hangs in 60 runs at 16 threads (6 of 60 hang
without it, same probe). The vendored file is the integrator's/builder's to change; it is outside what this review may
edit. Also add a multi-thread first-compile test to `isa_fex_test` (the existing 7-thread test runs the same blob
after the main thread already compiled it, so it never contends on the compile lock).

The spike's "4 threads PASS" and `isa_fex_test`'s "7 concurrent threads" are true statements about those runs; they do
not show the backend is deadlock-free.

## Claims checked

| claim | result |
|---|---|
| arm64 host reaches WinMain, 37,062 total / 36,765 exe calls / 84 distinct / 0 unimplemented, 0 isa traps | **reproduced**, 11 of 11 runs of my own build (`review-fexcore-arm64-winmain-1`, `-r1..r10`) |
| exe at 0x140000000 in most runs, 0x7000000000 in some, identical counts | reproduced: 3 of 10 runs relocated, all identical counts |
| per-import list identical to the x86_64 (Rosetta) host | **reproduced**: my x86_64 build of the same tree gives the same 37,062/36,765/84/0, and the "top imports" section diffs empty after stripping addresses |
| Rosetta not involved | `file`: Mach-O 64-bit executable arm64; the x86_64 binary is what runs under Rosetta |
| stops after WinMain at `LoadLibraryExW(xgameruntime.dll)` then the title's int3, abort, no seh handler | reproduced (`review-fexcore-arm64-past`: rc=134 after 37,068 calls, last call `LoadLibraryExW`) |
| hardened runtime: `allow-jit` alone is killed, `allow-jit` + `allow-unsigned-executable-memory` works, no entitlement killed | reproduced for both `isa_fex_test` (rc 137 / PASS / rc 137) and the real host (`jit` rc=137, `jit-arena` reaches WinMain with the same counts) |
| arena mapped RW only reaches WinMain with `allow-jit` alone | not re-run (needs a modified `common/x86_patch.cpp`); logged result is plausible and consistent with the other rows |
| `isa_fex_test` PASS | reproduced, 12 runs, 111,925-113,309 checks (count varies with the thread loops, as stated) |
| `isa_unit_test` PASS 343 on arm64 and x86_64 | reproduced, but the count varies run to run: arm64 342-343, x86_64 339-343 |
| `verify_vendor.sh` PASS | reproduced (commit 3ee2539 + 2 patches) |
| `div` by zero gives 0 with no exception | reproduced in the test output (`div 100/0: rax=0x0, 0 guest exception(s)`) |
| 16K-page model: 11,292 host pages, 8 read-only 4K pages become writable, 0 code pages | reproduced from the PE section table; I count 5 mixed host pages where the report says 6 (page count of mixed pages is not material, the 8 and the 0 are) |
| 8 hunks in main.cpp.patch, 7 in trace.cpp.patch, apply to the current tree | built, so they apply |
| stock `host/CMakeLists.txt` does not link without `win.cmake` | **true for x86_64 too** (see below) |
| `run.sh` guard only matches `build/macos/gta6rt` | confirmed: while I reviewed, another worker's `build/d3d12_vulkan-host-x86_64/gta6rt` was running and `run.sh` would not have refused. I used a `ps` based guard before every run |

## Speed (measured by me, same machine, load 10-11 unless noted)

Boot to WinMain, `--trace=0 --stop-at-winmain --hidden-window`, 8 interleaved runs, `time -p` real/user:

| | real s | user s |
|---|---|---|
| arm64 FEX (`gta6rt_fexhost`) | 0.24-0.26 | 0.21-0.22 |
| x86_64 Rosetta `--no-reexec` | 0.27-0.28 (one 0.36) | 0.21 |
| x86_64 Rosetta default | 0.29-0.32 | 0.22-0.24 |

`--map-only`: arm64 0.11-0.12 s, x86_64 0.16-0.17 s. So the guest+import part is about 0.13 s on FEX against 0.11 s on
Rosetta here (about 1.2x; the builder reports 1.3-1.5x at a higher load). Total wall time to WinMain is equal or
slightly better on arm64. The builder's statement ("same wall time, FEX slower on the guest part") holds.

CPU microbenchmark (`isa_fex_test --bench` against `isa_fex_bench_x86`, alternated, 3 rounds, load 10.5, both slowed by the
load so absolute values are higher than the builder's; ratios are what to compare):

| kernel | FEX ns | Rosetta ns | FEX / Rosetta | builder's ratio |
|---|---|---|---|---|
| int LCG chain | 2.31-2.67 | 2.31-2.71 | 1.0 | 0.95 |
| branchy + calls | 2.25-2.76 | 1.75-2.15 | 1.2-1.3 | 1.0-1.12 |
| load/store | 0.61-0.66 | 0.42-0.53 | 1.3-1.45 | 1.3 |
| SSE | 1.05-1.17 | 1.06-1.32 | 1.0 | 1.0 |
| AVX2 | 2.3-2.6 | 11.0-14.2 | 0.19-0.21 (FEX 4.7-5.5x faster) | 0.21 |
| guest->host thunk | 50-55 | 2.1-2.4 (direct ms_abi call, not a thunk) | | 38-40 vs 1.5 |
| host->guest call | 178-215 | 1.7-1.9 | | 134-137 vs 1.2 |

The ratios reproduce within noise; the builder's absolute FEX numbers (taken at an unusually quiet moment) were not
reproduced at this load, and the branchy kernel is somewhat worse than reported. The thunk row compares a FEX import
thunk with a Rosetta direct call and says so in STATUS.md; the real Rosetta import path also costs more than 1.5 ns.

## Probes of the riskiest claims (all PASS except where noted)

- **Guest state across a thunk** (`keep`): ymm6-ymm15 (all 256 bits) and rsi/rdi/r12-r15 filled by the guest, the host
  handler clobbers v0-v7, v16-v31 and x9-x15, then the guest reads everything back. All preserved.
- **Rounding mode and denormals per thread** (`fp`, 12 threads, 512 KiB / 1 MiB / 8 MiB stacks, RC = nearest/down/up/zero,
  20,000 iterations each across thunks): the guest's `divsd` always follows its own RC, the host handler always sees
  round-to-nearest and unflushed denormals. 20 of 20 runs PASS.
- **Thread churn** (`churn`, 300 threads each at 512 KiB, 1 MiB and 8 MiB, auto-attached, exit through the TSD destructor
  with no explicit detach): no growth in RSS or virtual size after the first batch.
- **Guest stack overflow** (infinite recursion on an 8 MiB, a 1 MiB and a 256 KiB thread, i.e. split stack and
  separate-mapping paths): `STATUS_STACK_OVERFLOW` (0xC00000FD) with params {write, address} in all three.
- **SSE4a against the isa reference** (`s4`, 5,200 randomized cases for insertq and extrq with the title's immediates
  (8,0), (0x18,0), (0x10,0x10) and others): low qword identical in every case. The upper qword differs in every case:
  FEX zeroes it, the reference leaves it unchanged (AMD says undefined). The title's sites use only the low half
  (`insertq xmm, xmm, imm, imm` feeding scalar code), so this is not a defect, but it is a behaviour difference that
  the STATUS file's "FEX runs SSE4a correctly" does not mention.
- Race on first compile: **FAIL**, see Defect 1.

## Other findings (not blocking)

1. **FEX's divide error is silent.** `div`/`idiv` by zero returns 0 with no exception and no log line. STATUS.md lists
   it as a gap, but "every unimplemented path must fail loudly" is not met: the title cannot tell. Raise
   `X86_TRAPNO_DE` (it is defined and never used) and map it to 0xC0000094, or at least log it.
2. **Host part of a small thread stack is small.** A 512 KiB thread keeps 128 KiB for host code (import handlers, Vulkan
   and D3D12 shims, loader work on `LoadLibrary`); the boot path was measured at 12-64 KiB but the frame loop calls
   MoltenVK from worker threads. A host stack overflow lands in the 16 KiB PROT_NONE page and the SA_ONSTACK handler has no
   alternate stack on non-main threads, so it would be a hard crash with no message. Watch this when threads with
   `STACK_SIZE_PARAM_IS_A_RESERVATION` sizes appear (integrator item 8).
3. **x86_64 stock host tree also fails to link today** without `win.cmake` (`host::win::*`, `_host_win_*` undefined). It
   builds with `-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=port/host/src/win/host_overlay.cmake`. That is an integration item
   for `host/CMakeLists.txt`, not only for arm64.
4. `isa_fex_test.cpp` has a vacuous check (`CHECK(t_fault.p0 == 0 || true, "")`) and the `thread_attach: StackBase ...
   already used` line from `isa_unit_test` is expected output, not an error.
5. A first build of `review-fexcore-arm64` failed once in `os/src/threads.cpp` (mach_timebase types) while another worker
   was editing that file (modified 11:51); the identical command passed three minutes later. Not related to this work.
6. FEX's SMC/icache gap (STATUS section 8, integrator item 6) is as documented. On the measured boot path nothing is
   patched after it runs; `FlushInstructionCache`, `VirtualProtect` and `Suspend/GetThreadContext` are not among the 84
   imports called. Thread suspend and get-context on a guest thread are documented as unavailable; I did not test what
   the os layer does when the title calls them.

## Not verified

- The scratch build with the arena mapped RW (item 4 of the integrator list) and its "reaches WinMain with allow-jit
  only" result.
- That the old backend failed 2 mxcsr checks (needs the old code); the new code passes, and my `fp` probe passes.
- The "7 of 33" relocation statistic (I saw 3 of 10); why the preferred base is sometimes taken.
- Why 36,765 differs from S1's 36,739 (both hosts give 36,765 today; the builder did not look into it either).
- Anything past WinMain: the run aborts there for the stated reasons.

## What the integrator must change (confirmed or added)

1. **`third_party/fex/compat/fex_darwin_compat.cpp`**: wake all waiters on `FUTEX_WAKE` / `FUTEX_WAKE_BITSET` (Defect 1), then re-run the
   `race` probe at 4-16 threads. Add a concurrent first-compile test to `isa_fex_test`.
2. `host/src/main.cpp`: apply `isa/fexcore/host_integration/main.cpp.patch` (applies, builds, reaches WinMain).
3. `host/src/trace.cpp`: apply `trace.cpp.patch` (works: `--trace` run above has 765 wrapped imports).
4. `host/CMakeLists.txt`: `include(src/win/win.cmake)` for both architectures; sign hardened builds with
   `isa/fexcore/sign/jit-arena.entitlements` (verified), or after the arena change with `jit.entitlements`.
5. `isa/common/x86_patch.cpp`: RW-only arena on arm64 (unverified by me), plus a way for host `address_is_code()` to
   recognise arena addresses.
6. `isa/rosetta`: define the API 6/7 functions (still undefined on x86_64; link errors only if called).
7. `isa/common`: `gta6rt_isa_flush_icache` and `code_write` should invalidate FEX translations.
8. seh on arm64: register the isa fault handler, use API 7 for context capture/restore, and make `current_stack()`
   use the guest rsp.
9. shims_core: `GetCurrentThreadStackLimits` should read the TEB; os: round reservation-flag CreateThread sizes up on arm64.
10. `host/tools/run.sh`: the guard must match any host binary (confirmed it does not today).
11. Docs: `third_party/README.md`, `isa/STATUS.md` Stubs section, `INTERFACES.md` isa section (API 6/7).
12. Optional hardening of FEX: raise #DE for `div`/`idiv` (finding 1).

## Files

- Logs: `port/run/logs/review-fexcore-arm64-winmain-1.log`, `review-fexcore-arm64-winmain-r1..r10.log`, `review-fexcore-arm64-past.log`,
  `review-fexcore-x86-winmain-1.log`, `review-fexcore-hardened-jit.log`, `review-fexcore-hardened-jit-arena.log`.
- Build logs: `port/build/review-fexcore-*-configure.log`, `*-build.log`, `review-fexcore-fextest.log`.
- Probe sources (scratchpad, not in the tree): `.../scratchpad/p/probe.cpp`, `g.S`, `s4/s4.cpp`, `so/so.cpp`.
