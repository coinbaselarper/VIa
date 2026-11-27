# Review, round 3: performance and correctness-policy fixes (isa, os, shims_core)

Reviewer run 2026-11-26. Verdict: APPROVED WITH FIXES (integration items below; no defect found in the code that
would corrupt the boot path once the integrator items are done).

## What was rebuilt and run

All from the current sources, in my own trees (`port/build/review-perf-policy-*`):

| tree | content |
|---|---|
| `review-perf-policy-x86_64` | full host (`-pagezero_size 0x10000`, `host_overlay.cmake` for the window layer), real-env shims_core tests, isa tests, os tests |
| `review-perf-policy-t-{x86_64,arm64}` | `os;isa;shims_core` with the stub-env `shims_core_test`, default `__PAGEZERO` |
| `review-perf-policy-tsan-arm64` | `os` with `-fsanitize=thread` |

Real-title runs (`GTA6RT_BIN` = my host, `--trace=0 --hidden-window`, x86_64 MoltenVK, load average 10-16 from other
workers): logs `port/run/logs/rpp-*.log`, `rpp2-*.log`.

## Tests re-run (all pass, counts equal the builder's)

- isa x86_64: `isa_unit_test` 343, `isa_rosetta_test` 92,827 (also with `GTA6RT_ISA_DIRECT_THUNKS=0`), `isa_reexec_test` 3,
  `isa_zen_wait_test` 27, `isa_vshift_check_test` 26.
- isa arm64: `isa_unit_test` PASS 343 (the builder reported 3 failing checks in the fexcore backend; not reproduced
  with the sources as they are now).
- os, both arches: vm 170, sync 166, threads 230, files 247, misc 209.
- shims_core: `shims_core_test` 91 cases, 1,241 checks (arm64) / 1,239 (x86_64); `leonida_bcrypt_rng_test` 21;
  `shims_core_real_smoke` 46.
- ThreadSanitizer (arm64, os): threads and misc clean; sync reports exactly one race, in `test_wait_on_address`
  (the test's own plain stores), as the builder said.

## Claims checked against my own runs

| claim | result |
|---|---|
| MWAITX loops wait on `[rdi]` at exe+0x4fb0bd0..0x4fb0d52, windows have no inner branch target | confirmed by disassembly of 0x4fb0ad0..0x4fb0e90: no branch lands inside any of the 4 windows; first-byte targets only |
| import calls, Sleep: NOP vs emulated | 20 s window, same binary: 21.5 M calls / 11.86 M Sleep (55%) with `GTA6RT_ISA_MWAITX=nop`; 9.6 M calls / 75.8 k Sleep (0.8%) emulated. First run pair (50 s): 14.4 M / 4.78 M vs 9.9 M / 71 k |
| CPU | process 1.36 vs 1.26 cores (20 s, `ps -M` per thread); first pair 1.28 vs 1.31 (30 s): inside noise. Waiting thread 2.95 vs 0.80 CPU-s per 20 s (14.8% vs 4.0% of a core). The builder's "1.41 to 1.30 cores" is the right direction but the process-level gain is not separable from noise here; the thread-level gain and the Sleep/import-rate drop are real |
| wake latency | unit test p50 19.2 us polling, 5.5 us with `gta6rt_isa_monitor_wake` (builder 14.6 / 4.9-7.5) |
| "title wakes come from the 20 us poll" | log: 14% of waits end by a write, 86% at the 200 us budget; nobody calls `gta6rt_isa_monitor_wake` yet |
| thunk overhead 0-3.5 ns | `shims_core_import_bench`: GetLastError 0.1, TlsGetValue 0.9, CS 0.8, SRW 0.6, strcmp 0.7 ns; HeapAlloc+Free 7.7 (noise, was 3.3 before) |
| waits: 8 threads own event 1,577 to 78 ns | 84.8 ns here; shared-semaphore pattern 592 / 982 / 3,576 ns at 2 / 4 / 8 threads (the disclosed regression is real) |
| Series S default, 36,765 calls | confirmed: 36,765 (series-s) vs 36,739 (`LEONIDA_CONSOLE_PROFILE=series-x`), also at `--force-reloc` |
| heap below 4 GiB, address order | confirmed: 193,986,560 bytes at 0x1c8c0000, 2,960 pages, 0 not fresh, "one stretch of the pool"; memory report shows 185 MiB, 184 MiB resident |
| memory report | confirmed (periodic via `GTA6RT_MEMORY_REPORT_S`); footprint 4,672 MiB, resident 1,904 MiB in that run |
| vshift check | confirmed: 1 of 112 sites executed (rva 0x1645a5b), 64 executions in 20 s, 0 with a lane >= 2^32; `vshift=0 vshift_safe=105` in the fixups line as documented |
| `_putenv_s` found by the new default | confirmed in the log (`crt: _putenv_s("FFX_FSR4_XBOX_ACO", "1") -> ok`); 0 unimplemented imports called |
| relocated exe | WinMain reached at 0x300000000 with the 4 wait stubs, 36,765 calls |
| `os` waits under stress (my probe, both arches) | no lost wakeup: 200,000 semaphore items to 1/4/8 consumers blocked in `WaitForMultipleObjects`, 3,000 auto-event hand-offs with 1/3/8 timed waiters, 5,000 thread joins, 6-thread recursive mutex counter 600,000 exact |
| zen stub ABI | stub aligns rsp to 16 with 0x80 bytes of shadow/xmm space, passes rcx/rdx/r8 for the ms_abi host function, saves flags, rax, rcx, rdx, r8-r11, xmm0-5; `isa_zen_wait_test` checks rbx, r8-r11, rdi, flags and xmm unchanged |

## Findings

### Blocking (integration; the module code is correct, but the boot path breaks without these)

1. **A host without `-pagezero_size 0x10000` aborts the title at its first 185 MB heap request.** Verified: I relinked my
   objects with the default 4 GiB `__PAGEZERO` (`scratchpad/rpp/gta6rt_nopz`); the log shows `xmem: 32-bit reservation of
   193986560 bytes (attributes 0x7) refused`, the title's `XMemVirtualAlloc(0, 0xb900000, 0x42000, 0x7)` fails and
   the title hits `int3` (rc 134, 7 s into the run). `host/CMakeLists.txt` in the repo does not have the flag yet, so the
   next integrator rebuild of `build/macos/gta6rt` crashes past WinMain until it is added. This is the intended
   fail-loud behaviour, and the builder lists the flag as integrator item 1, so it is not a false claim.
2. **arm64 cannot satisfy item 1.** arm64 macOS refuses a smaller `__PAGEZERO` (the os test prints `no room` on arm64 and
   on x86_64 with the default page zero), so the fexcore host reaches the same abort when the title is run past
   WinMain unless `LEONIDA_XMEM_32BIT=anywhere` is set. Nothing in the builder's report mentions arm64 here.

### Non-blocking defects

3. **Unimplemented-import fail policy still returns success for most classes.** `classify` only knows D3D12/DXGI/XAudio/
   GameInput/MF/DStorage/audio-HAL/GDK families and CRT `*_s`. I probed it: NTSTATUS (`ntdll!Nt*`, `Rtl*`), LSTATUS
   (`advapi32!Reg*`), Winsock (`WSAStartup`, `connect`), `bcrypt!BCrypt*`, `ole32!Co*`, `d3dcompiler!D3DCompile`,
   `winhttp!*`, `wintrust!*` all classify as "other" and so return 0, which is STATUS_SUCCESS / ERROR_SUCCESS / S_OK /
   a 0 success code, with out-pointers unwritten and last error 120 that these callers never read. For the title's 12
   unserved imports this is correct (6 MF + `xaudio2_9!#1` are HRESULT, 4 crypt32 and `MFGetSystemTime` are
   BOOL/pointer/value; the report's "8 of 12" is 7 of 12), but the stated goal ("E_NOTIMPL-class / NULL, never success")
   is only met for those names. Suggest NTSTATUS to STATUS_NOT_IMPLEMENTED (0xC0000002), `Reg*` to 120, Winsock to
   `SOCKET_ERROR` plus `WSAEOPNOTSUPP`, and the remaining `Co*`/`BCrypt*`/`D3DCompile*` to E_NOTIMPL / their status.
4. **`os` `sleep_on_word` converts the timeout to mach ticks, which is wrong on arm64.** The API takes nanoseconds for
   `OS_CLOCK_MACH_ABSOLUTE_TIME` (probe `scratchpad/rpp/ulock_probe.cpp`: argument 50,000,000 waits 52 ms on arm64, whose
   timebase is 125/3). The conversion shortens every finite wait by 41.7x, so a timed wait loops hundreds of times on
   arm64. Measured with `WaitForSingleObject(unsignaled event, 500 ms)`: 1.28 ms of CPU on arm64, 0.05 ms on x86_64 (10 ms:
   0.10 vs 0.11; 50 ms: 0.21 vs 0.03). Results stay correct (the deadline is re-checked); only the arm64/fexcore build
   pays. No effect on the shipped x86_64 host (timebase 1/1). `zen_wait.cpp` correctly passes nanoseconds.
5. **`verify.sh s1` will fail with the new default.** Its regex is `3673[0-9]`; the default Series S run reaches
   36,765 exe calls. The builder lists this; the lead owns `verify.sh`.
6. **Weak evidence behind two statements.** "`gta6rt_vm_reserve_range(0x10000, 4 GiB, ...)` returns `NO_MEMORY` on a
   default-`__PAGEZERO` host (tested)": `test_reserve_range_and_report` only prints `ok` or `no room` and asserts
   nothing for the failing case (I ran it by hand: `no room` with default page zero, `ok` with the flag). The
   "3 arm64 isa_unit_test failures" were not reproducible (343 pass).
7. **Shared-object contention regressed** (disclosed by the builder, confirmed above): 2/4/8 threads on one semaphore
   at 592/982/3,576 ns (were 230/802/2,541). Not on the title's path.
8. `os/threads.cpp` calls `os_sync_wait_on_address` without a `__builtin_available(macOS 14.4)` guard while `zen_wait.cpp`
   and `x86_patch.cpp` guard it. Harmless with the current deployment target; will be a build error if the target is
   ever pinned below 14.4 (X8).

### Process note

One ad-hoc host start of mine (`rpp-nopz`, 7 s) overlapped another worker's `review-pixels-x86_64/gta6rt` that was
running; all other title runs waited for a free slot. I also overwrote a few scratch logs of the same names
(`isa_unit.log`, `zen_wait.log`, `vshift.log`, ...) in the shared scratchpad. Nothing under `port/` other than this
file, my build trees and `port/run/logs/rpp*` was written.

## Not verified

- The builder's `sample`-based blocked-thread numbers (13.05 to 0.38 thread-equivalents) were not re-measured; process
  CPU and per-thread CPU were.
- Frame rate was not compared (GPU-bound, other workers' load).
- The `isa/fexcore` backend and any arm64 run of the title were not exercised beyond the unit tests.
- The checks of the guest-facing ABI were by reading and by the existing register-preservation tests; no new guest
  code was written.

## Changes the integrator must make (from the builder, confirmed; plus mine)

1. `host/CMakeLists.txt` (x86_64): `LINKER:-pagezero_size,0x10000`. Without it the title aborts (finding 1).
2. `host/src/main.cpp`: `--console-profile` option calling `gta6rt_shims_set_console_profile` before `gta6rt_shims_init`;
   `gta6rt_os_memory_report(...)` and `gta6rt_isa_vshift_check_report()` where the trace is dumped; print `zen_wait` and
   `vshift_checked` in the `exe fixups` line.
3. D3D12 layer: `gta6rt_isa_monitor_wake(&value)` after each store of fence slot 12's completed value.
4. `verify.sh s1`: accept 36,765 (Series S default) or force `LEONIDA_CONSOLE_PROFILE=series-x`.
5. arm64 host: decide how the 32-bit heap request is served (finding 2) before running the title past WinMain there.
6. Optional: broaden the unimplemented-return classes (finding 3); drop the mach-tick conversion in `os/threads.cpp`
   (finding 4).
