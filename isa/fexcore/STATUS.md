# isa/fexcore status (X5/X6: arm64 backend on embedded libFEXCore)

All numbers were measured on 2026-11-26 on this M4 Pro (macOS 27.0.1). Other workers were running builds and Metal
compiles the whole time; the load average is given with every timing.

## Result

The native arm64 host runs the title's guest x86-64 code inside the vendored libFEXCore and reaches WinMain with the
same import calls as the x86_64 (Rosetta) host built from the same tree. Rosetta is not involved: the binary is
`Mach-O 64-bit executable arm64` and the test binary reports `proc_translated=0`.

| run (`host/tools/run.sh`, `--trace --stop-at-winmain --hidden-window`) | log (`port/run/logs/`) | result |
|---|---|---|
| arm64, `build/fexcore-host-arm64/gta6rt_fexhost` | `fexcore-arm64-winmain.log` | `REACHED MARKER: RVA 0x5a91bd0`, `args: rcx=0x140000000 rdx=0 ... r9=0xa`, `calls: 37062 total, 36765 from the exe, 84 distinct imports called (0 unimplemented), 765 wrapped`, 0 isa traps |
| arm64, exe relocated (preferred base not free in that run) | `fexr2-arm-winmain-trace-r2.log` | `rcx=0x7000000000`, same 37062 / 36765 / 84 / 0 |
| x86_64, `build/fexcore-host-x86_64/gta6rt` (same sources, Rosetta) | `fexr2-x86-winmain-trace.log` | same 37062 / 36765 / 84 / 0 |

- The 84-entry first-call list with per-import counts is identical between the two backends (diffed). Only the
  last-30 ring differs, in stack addresses and in an unused r9 value.
- The exe count is 36,765, not S1's 36,739. Today's x86_64 build of the same tree also gives 36,765, so the difference
  comes from shared sources that changed since S1, not from the backend. Which change it is was not investigated.
- On arm64 the exe lands at its preferred base in some runs and at 0x7000000000 in others (26 of 33 arm64 runs today at
  0x140000000, 7 at 0x7000000000). Both reach WinMain identically. Why the range
  is taken in some runs was not investigated; it is the host link-layout open item.

**Where it stops after WinMain (exact wall, `fexcore-arm64-past-winmain.log`).**
- The title calls `LoadLibraryExW("xgameruntime.dll")`; the result is `[W shims] LoadLibrary(xgameruntime.dll): module not found`.
- It then executes its fail-fast `int3` at exe+0x5a91c21 (rax=0x89240101).
- isa reports `EXCEPTION_BREAKPOINT` with the full register dump. No seh handler is registered on arm64, so it aborts
  after 37,069 import calls.
- Cause: `gta6rt_shims_gdk_register()` and `gta6rt_d3d12_vulkan_register()` both return `unsupported` on arm64. Those
  modules have no arm64 tables yet; they are not isa work.
- The next walls are the same two modules, then seh dispatch on arm64 (see "For the integrator").

## Build and test

```sh
cd port
cmake -S . -B build/fexcore-arm64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA6RT_MODULES="os;isa"
ninja -C build/fexcore-arm64 -j3 isa_fex_test fex_futex_test isa_unit_test isa_fex_bench_x86_build
build/fexcore-arm64/isa/fexcore/tests/isa_fex_test [--bench]            # --race=THREADS,ROUNDS runs only the compile race
build/fexcore-arm64/isa/fexcore/tests/fex_futex_test                   # the Darwin futex shim, host threads only
isa/tools/run_timeout.sh 120 build/fexcore-arm64/isa/fexcore/tests/isa_fex_bench_x86      # Rosetta side, x86_64
# pre-integration host (host/src + host_integration/*.patch), opt-in:
cmake -S . -B build/fexcore-host-arm64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA6RT_FEX_HOST=ON
ninja -C build/fexcore-host-arm64 -j3 gta6rt_fexhost
GTA6RT_BIN=$PWD/build/fexcore-host-arm64/gta6rt_fexhost host/tools/run.sh 60 NAME --trace --stop-at-winmain --hidden-window
```

| test | arch | result | log |
|---|---|---|---|
| `isa_fex_test` | arm64 | PASS, 112,179 checks (the count varies because the thread loops count their calls); includes the 16-thread concurrent first-compile race | `build/fexcore-arm64-fex-test-round3.log` |
| `fex_futex_test` | arm64 | PASS, 12 checks; 20 of 20 repeated runs of its lock and bitset parts PASS | stdout, see "Review response (round 3)" |
| `isa_unit_test` | arm64 | PASS, 343 checks. It was FAIL 3/343 before this session's stack-split change, see section 2 | `build/fexcore-arm64-unit-round3.log` |
| `isa_unit_test` | x86_64 (Rosetta) | PASS, 343 checks (after the isa.h API 7 edit) | stdout |
| `isa_fex_test`, hardened runtime + `jit-arena.entitlements` | arm64 | PASS | `build/fexcore-arm64-hardened-jit-arena.log` |
| `isa_fex_test`, hardened runtime + `jit.entitlements` | arm64 | killed (137), see section 9 | `build/fexcore-arm64-hardened-jit.log` |
| `third_party/fex/verify_vendor.sh` | - | `verify_vendor: PASS (src = 3ee2539... + 2 patches)` | stdout |

What `isa_fex_test` covers. The guest code is `tests/fex_guest.S`, assembled for x86_64 and copied into RW data
memory, so it only ever runs through FEX.
- **Host->guest calls:** 16 arguments with shadow-space scribbling and an alignment check; mixed int/double
  arguments with a stack double; `call_guest4`; `run_entry`.
- **Thunks:**
  - FRAME with 6 arguments (2 on the stack), and callee-saved registers checked;
  - a float FRAME thunk;
  - guest->host->guest re-entry, nested 10 deep;
  - TRAP;
  - NATIVE without a signature is refused;
  - 9 NATIVE-with-signature cases (section 3).
- **Other interfaces:** the os invoker, for guest and host targets.
- **TEB through GS:** `gs:[0x30]/[0x08]/[0x10]/[0x60]/[0x58]->[0]` on the main thread and 7 concurrent threads, 2,000
  probes each, nested thunks and a benchmark on every thread. These threads run code the main thread already
  compiled, so they do not contend on FEX's compile lock.
- **Concurrent first compile (round 3):** 16 threads (8 MiB, 512 KiB and 256 KiB stacks) attach, then all run 256
  freshly written guest functions (each calls a FRAME thunk) in different orders, 32 rounds with new code each round.
  A watchdog fails the run with a named line and exit code 3 when no guest call finishes for 10 s.
- **CPUID and xgetbv** against the policy.
- **Zen and AVX2 instructions:** SSE4a, CLZERO, CLWB/CLFLUSHOPT, MONITORX/MWAITX, and vpsllvq/vpsrlvq with counts
  ≥ 2^32.
- **MXCSR rounding:** across calls, thunks and nested callbacks.
- **Context capture/resume** inside thunks.
- **Atomics:** an unaligned `lock xadd` that crosses 16 bytes.
- **Faults:** `ud2`, `int3`, AV read/write with parameters, `int 0x29`, a fault in a nested call resumed there, and
  `div` by zero.
- **`__chkstk`:** the title's real bytes, unpatched, at 3 sizes.

## 1. Vendored FEX (`port/third_party/fex`, see its README.md)

- **Source.** FEX `3ee25399232a6431c77c6314c437ea523ec29d0c` with two patches:
  - `0001-fex-darwin.diff`: the X6 spike's 18-file Darwin patch;
  - `0002-callback-level-sp.diff`: new, 7 lines. The JIT callback entry stores its sp in
    `CpuStateFrame::ReturningStackLocation`, so the embedder can resume at that level after an exception.
- **Licence and pruning.** MIT. The tree is pruned to what FEXCore needs (11 MB, 872 files). `verify_vendor.sh`
  rebuilds it from the clone and compares.
- **Build.** `fex.cmake` builds FEX as its own CMake project in `<tree>/third_party/fex-build`. The first build runs at
  configure time with 3 jobs; afterwards the outer build runs FEX's own ninja incrementally. The configure-time path
  is redone only when the patches, the compat files or the arguments change.
  - Measured: the configure of a fresh tree, including FEX's 170 steps, took 32.7 s (`build/fexcore-arm64-configure.log`).
  - 0 warnings in FEX's build log.
  - The result is 7 static archives; `libFEXCore.a` is 4.1 MB.
- **Arch guard.** `fex.cmake` refuses non-arm64 trees.
- **Upstream.** FEX's AGENTS.md forbids AI-written contributions, so both patches stay a private fork.
- **Darwin shim** (`third_party/fex/compat`, ours, not a FEX patch): Linux futex on `os_sync_wait_on_address`.
  Round 3 fixed a deadlock in it (bitset wakes); see "Review response (round 3)".

## 2. Threads, guest stacks, TEB

- **Per-thread state.** `backend_plant` creates one FEX `InternalThreadState` per attached thread, plus FEX's 4 MiB call-ret
  stack (`CALLRET_STACK_SIZE`) with a 16 KiB guard page on each side.
- **TEB.** FEX keeps the guest GS base per thread (`gs_cached` = TEB). Every `gs:` access reads the real TEB, so no TSD
  slot is touched and `gs:[0x00]` and `gs:[0x10]` work as well. `gta6rt_isa_thread_teb_mode()` reports SWAP.
  `backend_publish` is a no-op because guest reads go to TEB memory.
- **Guest stack: split.** FEX runs host code (dispatcher, JIT compile, thunk handlers) on the thread's sp, so the
  pthread stack is split:
  - the host keeps the top quarter, clamped to 128 KiB..2 MiB;
  - a 16 KiB PROT_NONE page sits below it;
  - the guest stack runs from that page down to the pthread limit;
  - the TEB keeps the pthread bounds, which is what os/, seh/ and `GetCurrentThreadStackLimits` report.
- **Guest stack: separate mapping.** Stacks under 512 KiB, or threads with less than 64 KiB between sp and the split
  point at attach, get a separate guest stack of max(1 MiB, their size), and the TEB describes that mapping instead.
- **Measured host-stack use, boot to WinMain.** I used a temporary mincore probe of the host part, now removed
  (`fexr2-arm-stackprobe.log`). It found 64 KiB resident on the main thread (AppKit included) and 12 KiB on the game
  thread.
- **Change this session.** The split used to need ≥ 2 MiB, so default 512 KiB pthreads had a separate stack.
  `isa_unit_test` asserts that a worker's StackBase is its pthread top, and it failed 3 checks. It now passes.
  `isa_fex_test` checks both paths: 8 MiB and 512 KiB threads split, a 256 KiB thread gets a separate stack.
- **Shared code.** TLS arrays and slots are common code.

## 3. Guest->host thunks

- **Thunk code.** A thunk is 47 bytes of x86 code in the common arena: `mov r11, rdi; mov rdi, rec; 0F 3F <32-byte id>`.
  FEX's thunk opcode spills the guest state, calls `gta6rt_fex_thunk_entry(rec)` on the host stack, and then performs
  the guest `ret`. rdi is restored from r11 before the handler runs.
- **FRAME and TRAP** fill `gta6rt_guest_frame` from FEX's state (rcx/rdx/r8/r9, xmm0-3 low halves, rsp at the
  return address, stack arguments at rsp+0x28) and write rax, or xmm0 for float returns, back.
- **NATIVE without a signature** returns `GTA6RT_E_UNSUPPORTED` with a log line, because arm64 has no ms_abi.
- **NATIVE with a signature (isa.h API 6, `fexcore_native.cpp`)** re-marshals the Win64 call into an AAPCS64 call of a
  plain host function. Every row is tested:

  | case | what was checked |
  |---|---|
  | mixed int32/float/double/int64/float/uint8/double, 7 arguments, 3 on the stack | double result exact |
  | 10 int64 arguments (6 on the stack) | result |
  | int8/uint16/int16/bool/int32 with garbage upper bits; two land on Darwin's packed stack | sign and zero extension |
  | 24-byte struct return | Win64 hidden pointer in rcx -> AAPCS x8; rax = buffer |
  | 16-byte struct return | AAPCS x0/x1 -> Win64 hidden buffer |
  | 2-float struct return | Win64 rax <- AAPCS s0/s1 (HFA) |
  | 4-float HFA by value in and out | Win64 passes it by reference |
  | `__m128` arguments | Win64 passes by pointer; returned in xmm0 |
  | variadic `printf`-style call (1 named argument) | Darwin puts variadic arguments on the stack; float promoted to double |

  Not supported, and refused with `GTA6RT_E_INVALID_ARG`: more than 16 arguments, aggregates over 64 bytes.
  Stack-argument overflow over 512 bytes aborts with a log line. No caller uses API 6 yet.
- **Return value of `gta6rt_isa_make_thunk`.** The address is stable. The arena never loses its mapping, so creating a
  thunk while other threads run thunks is safe.

## 4. Host->guest calls and floating-point state

- **Entry.** `gta6rt_isa_call_guest` lays out a Win64 frame: 32-byte home space, arguments 5..16 at +0x20, entry
  rsp ≡ 8 mod 16 after FEX pushes its callback-return trampoline. It then calls `Context::HandleCallback`.
  - At depth 0 the frame goes at the guest stack top.
  - Nested calls (from inside a thunk) put their frame below the current guest rsp.
  - Each level saves and restores guest rsp, rip and the level's `ReturningStackLocation`. The limit is 64 levels.
- **Callers.** `run_entry` and `call_guest4` use it. `gta6rt_os_set_invoker` routes guest addresses through it and
  calls host functions directly.
- **Foreign threads** attach on the spot through common code.
- **FPCR bug, found and fixed this session.** FEX keeps MXCSR in its state, but keeps the rounding mode and FTZ only in
  the hardware FPCR. FillSpecialRegs sets AH/NEP/FIZ, never RMode/FZ.
  - Before: the backend restored a raw FPCR snapshot. A guest `ldmxcsr` was lost at the next host->guest entry, and a
  callback nested in a thunk ran with the host's rounding mode.
  - Now: every entry into guest code (call_guest, thunk return, exception resume) derives RMode/FZ from the guest
  MXCSR, and FPCR is written only when it differs.
  - `test_mxcsr` failed 2 checks against the old backend and passes with the new one.

## 5. CPUID

FEX's CPUID and XGETBV callbacks (`Pointers.CPUIDFunction`/`XCRFunction`) answer from `gta6rt_isa_cpuid` and
`policy_xcr0`, so the guest's own `cpuid` sees the policy.
- Measured: leaves 0, 1, 7, 0xD, 0x80000000/1/8 equal the policy, and xgetbv(0) = 7.
- `advertised_features` = 0xff (GTA6RT_CPUID_DEFAULT) and `cpuid_enforced` = 1.
- FEX's raw feature set is 0xeff: the policy bits plus ADX, SHA and VAES. The policy hides those three. FEX does not
  advertise SSE4a, although it executes it.
- No image site needs patching, and `reexec_for_cpuid` is a no-op.

## 6. Image fixups: which are FEX's job and which are ours (measured)

| instruction | under FEX (isa_fex_test) | needed on fexcore |
|---|---|---|
| `insertq`/`extrq` imm (SSE4a) | executed by FEX, result equals the reference, 0 traps | no (`SSE4A_PATCH` off) |
| `vpsllvq`/`vpsrlvq`, counts {1, 63, 64, 2^32+1} | x86 results; the Rosetta bug is absent | no (`VSHIFT` off) |
| `cpuid`/`xgetbv` | policy via FEX callback | no (`CPUID` off) |
| `clzero` | FEX (DC ZVA, 64-byte line on this host), exactly one line zeroed, 0 traps | no; the CLZERO loop stubs are harmless |
| `clwb`/`clflushopt` | FEX, 0 traps | no; the NOP sites are harmless |
| `monitorx`/`mwaitx` | FEX logs `Invalid instruction in entry block` and raises its invalid-opcode exception (`FAULT_SIGILL`, no host signal); the exception path's `zen_decode` skips it (2 traps for 2 instructions) | yes: `ZEN` makes the title's sites NOPs |
| `__chkstk` (reads `gs:[0x10]`) | the title's real bytes run correctly unpatched (gs:[0x10] = TEB StackLimit) | no; `CHKSTK` kept to skip the probes (S0.6) |

- **Host flags.** On arm64 the host passes `CHKSTK | ZEN` (0x11) for the exe; that is in `main.cpp.patch`. The report
  reads `chkstk=1 sse4a=0 vshift=0 cpuid=0 xgetbv=0 mismatched=0 vshift_safe=105 zen_nop=12 clzero_loops=10`. WinMain
  counts are identical to the run with the x86_64 flag set 0x17.
- **MWAITX still spins.** fexcore has no `set_zen_wait_provider` entry yet, so the 4 MONITORX/MWAITX wait windows
  become NOPs and that wait loop spins, as on Rosetta before API 5. This is not implemented because no arm64 run
  reaches the frame loop where it executes (rule: do not tune unexercised code).
  - Port when it does: `rosetta/zen_wait.cpp`'s monitorx/mwaitx as FRAME thunks, with `cntvct_el0` for `__rdtsc` and
    `yield` for `_mm_pause`.

## 7. Faults -> seh

- **Synchronous FEX exceptions** (ud2 and invalid opcodes, int3, `int 0x29`/0x2c/0x2d, privileged instructions,
  `into`, page faults) arrive at `Pointers.SynchronousExceptionHandler` = `gta6rt_fex_exception_entry`.
- **Host signals raised by JIT code** (SIGSEGV/SIGBUS from guest loads and stores) are reconstructed from the
  mcontext. That uses FEX's `RestoreRIPFromHostPC`, the SRA register map, and `ReconstructCompactedEFLAGS` with the
  host NZCV. The handler is then redirected to the same entry.
- **Signal-handler internals:**
  - it first gives os `gta6rt_vm_handle_fault` (write watch, guard pages) a chance;
  - it handles FEX's JIT guard page (restart long-jump), call-ret stack overflow (resets x25), and unaligned
    atomics (`HandleUnalignedAccess`);
  - it finds the thread through the TSD key, not `thread_local`, because Darwin allocates a thread's TLV block with
    malloc on first use (changed this session).
- **The CONTEXT built from FEX's state:**
  - GPRs, rip, eflags, cs 0x33, ss/ds/es/gs 0x2b, fs 0x53;
  - MXCSR, x87 control/status/tag and mm registers in the FXSAVE area;
  - xmm0-15 low 128 bits (the ymm upper halves stay in FEX's `avx_high`);
  - debug registers.
- **Records:**
  - AV with {read 0 / write 1, address}; `STATUS_GUARD_PAGE_VIOLATION`; `STATUS_STACK_OVERFLOW` for the 16 KiB below
    the guest stack;
  - `0xC000001D`, `0x80000003` with rip on the `int3`, `0xC0000409` noncontinuable with rcx;
  - `0xC0000096`, `0xC0000095`, `0xC0000420`.
- **Handler order.** Zen instructions are emulated first, then the illegal-instruction hook, then the guest fault
  handler. If the fault handler returns RESUME, the guest continues with the edited CONTEXT at the same host->guest
  level. A resume rsp above the current call level aborts with a dump (non-local exit through host frames). With no
  handler registered, the backend dumps the registers and aborts; that is the arm64 run above.
- **Tested:** each record kind and its parameters, resume at a chosen rip with a chosen rax, and a fault inside a
  nested call.
- **New: isa.h API 7, for seh on arm64.** `gta6rt_isa_capture_guest_context` inside a thunk handler gives
  RtlCaptureContext's view: rip = return address, rsp + 8. `gta6rt_isa_resume_guest_context` drops the thunk's host
  frames and continues the guest with a CONTEXT; that is RtlRestoreContext and the end of RtlUnwindEx.
  - Tested: callee-saved values, rip/rsp, cs/mxcsr; resume at another rip; resume inside a nested call while the outer
    thunk returns normally; 100,000 resumes in a row with no host-stack leak.
- **Gap: FEX does not raise #DE.** `div` by zero returns rax=0 with no exception (`div 100/0: rax=0x0, 0 guest
  exception(s)`); Windows raises `EXCEPTION_INT_DIVIDE_BY_ZERO`. FEX defines `X86_TRAPNO_DE` but never uses it. Not
  observed in the title so far.

## 8. 16 KiB host pages vs the PE's 4 KiB sections: protection model

**Decision.** The model is os/'s `vm.cpp`, kept as it is: guest protections are tracked per 4 KiB page, and each
16 KiB host page gets the union of its four guest pages.
- On arm64 EXEC is never set on the host. Guest code is data to FEX, which translates from readable memory.
- PAGE_GUARD and NOACCESS on a 4 KiB page are enforced only when the whole host page has them.
- The alternative was to protect the host page with the intersection of its guest pages and emulate the faulting
  access. That costs a signal plus instruction emulation per access on mixed pages, and FEX has no single-access replay.

Measured on the real images (section tables; script in the session scratchpad, not in the tree):

| image | host pages | mixed | 4 KiB pages loosened |
|---|---:|---:|---|
| `game_scarlett_master.exe` (SectionAlignment 0x1000) | 11,292 | 6 | 8 read-only pages become writable: the last 2 of `.rdata` (they share a host page with `.data`) and the `.retplne`/`.voltbl`/`.reloc` pages next to `.tls`/`.xbld`. **0 code pages become writable** |
| the 16 package DLLs | 2-61 each | 1-5 each | 0-7 read-only->RW each; 1 code page becomes RW (`msvcp140_codecvt_ids.dll`) |

- **Consequence.** A guest write into one of those few pages succeeds where Windows would raise an AV. Nothing
  depends on that so far.
- **FEX-side page handling** is in the Darwin patch: 16 KiB guard pages for the JIT code buffer, the thread pool
  allocator and the call-ret stack.
- **Not available:** FEX's `InterruptFaultPage` (a 4 KiB page inside the thread state) cannot be protected on its own,
  so FEX's async thread interrupt is unavailable. SuspendThread/ResumeThread (imported by the exe) do not need it.
- **Self-modifying code.** FEX's SMC mode is MTRACK, which relies on the frontend. We do not write-protect translated
  pages, and `gta6rt_isa_flush_icache` (common) does not invalidate FEX's cache. Code changed after it has run, or new
  code mapped where old code ran (an image unloaded and another loaded at the same address), would run stale
  translations. Everything patched today is patched before it runs.

## 9. Signing and JIT entitlements (measured)

FEX's code buffers are `MAP_JIT`, with W^X toggled per thread (`JITWriteGuard`).

| binary | signature | result |
|---|---|---|
| `isa_fex_test`, `gta6rt_fexhost` as linked | linker ad-hoc, no hardened runtime | PASS / WinMain |
| `isa_fex_test` hardened, no entitlements | `-o runtime` | killed at init, `SIGKILL (Code Signature Invalid)` at the arena, before FEX's `MAP_JIT` (the spike, without the arena, saw `MAP_JIT` -> EINVAL) |
| hardened + `allow-jit` (`sign/jit.entitlements`) | | **killed, `CODESIGNING Invalid Page`**: `ic ivau` on the common isa arena's RX view at init (crash report `isa_fex_test.hardened_jit-*.ips`). The title host is killed the same way (`fexr3-arm-winmain-hardened-jit.log`, rc 137) |
| hardened + `allow-jit` + `allow-unsigned-executable-memory` (`sign/jit-arena.entitlements`) | | `isa_fex_test` PASS; `gta6rt_fexhost` reaches WinMain, 37062/36765/84/0 (`fexr3-arm-winmain-hardened-jit-arena.log`) |
| hardened + `allow-jit` only, with the arena mapped RW (no RX view) on arm64 | scratch build of a modified `common/x86_patch.cpp` | `isa_fex_test` PASS; host reaches WinMain (`fexr3-arm-winmain-hardened-jit-rwarena.log`) |

- **Where the RX view comes from.** The arena holds x86 bytes that only FEX reads, so the RX view on arm64 is what
  forces the second entitlement. The fix is in common code; see "For the integrator".
- **Side effect of the RW-arena variant.** It counted 36,647 exe calls and 763 wrapped. That is because host
  `address_is_code()` uses `VM_PROT_EXECUTE` to decide whether a GetProcAddress result gets a trace stub, so 2 results
  (118 `WakeAllConditionVariable` calls) were not traced. Title behaviour was the same.
- `sign/sign_hardened.sh BINARY [ENTITLEMENTS]` signs a binary with the hardened runtime. Development builds need
  nothing.
- **Not tested:** loading a third-party Vulkan/Metal driver dylib under the hardened runtime. Library validation would
  probably need `disable-library-validation`.

## 10. Performance (guest speed vs Rosetta)

**Boot to WinMain.** `--trace=0 --stop-at-winmain --hidden-window`, `/usr/bin/time -p`, 11:24, load average 8.5-9.8.
Exe flags: 0x17 on arm64; 0x1f on x86_64, because `--no-reexec` leaves Rosetta's cpuid un-advertised and the exe's
cpuid sites are trapped (4 traps).

| | runs, real s | user s | `--map-only` real s (user) | WinMain - map-only |
|---|---|---|---|---|
| arm64 FEX `gta6rt_fexhost` | 0.27, 0.27, 0.29, 0.31, 0.30 | 0.24-0.25 | 0.11 x5 (0.08-0.09) | ~0.18 s real, ~0.15 s user |
| x86_64 Rosetta `gta6rt` (`--no-reexec`) | 1.09 (first run after relink: Rosetta translating the host), 0.29, 0.30, 0.29, 0.29 | 0.21-0.22 | 0.16-0.17 (0.10-0.11) | ~0.13 s real, ~0.11 s user |

- **Wall time** to WinMain is the same on both backends (0.29 s). The native arm64 host side, mapping and binding, is
  0.05 s faster.
- **Guest-plus-import part.** FEX takes about 1.3-1.5x Rosetta's time here. This path is CRT init code that runs once,
  so most of it is JIT compile time.
- **FEX knobs.** `GTA6RT_FEX_MULTIBLOCK=0/1` and `GTA6RT_FEX_TSO=0` gave 0.28-0.30 s, no difference at
  `time`'s 10 ms resolution.
- **Final binary, flags 0x11.** Interleaved with x86_64 at 11:48, load average 17.6-18 (`fexr4-*`): arm64 0.33,
  0.34, 0.32, 0.29, 0.30 s (user 0.26-0.29); x86_64 0.41, 0.33, 0.33, 0.31, 0.34 s (user 0.24-0.28). Same picture
  as the table under twice the load.
- **Not comparable:** an earlier 0x11 set under load 13-25 (0.47-0.49 s, `fexr2-arm-winmain-f11-*`).

**CPU microbenchmark.** The same x86 bytes (`tests/fex_guest.S`) run through FEX via `gta6rt_isa_call_guest` and
natively under Rosetta (`isa_fex_bench_x86`, the kernels linked into an x86_64 executable). Best of 5, ns per inner
iteration, two runs each, 11:25, load average about 9.5. Checksums match native.

| kernel | FEX | native arm64 | Rosetta |
|---|---:|---:|---:|
| int LCG chain | 1.644 / 1.701 | 1.733 / 1.789 | 1.715 / 1.784 |
| branchy integer + calls | 1.481 / 1.660 | 0.420 / 0.435 | 1.462 / 1.482 |
| load/store chain | 0.467 / 0.443 | - | 0.342 / 0.352 |
| SSE mulps/addps (per 8 floats) | 0.793 / 0.793 | - | 0.783 / 0.868 |
| AVX2 vpmulld/vpaddd (per 8 ints) | 1.774 / 1.777 | - | 8.274 / 8.468 |
| guest->host call + return | 38.0 / 39.9 (FRAME thunk) | - | 1.6 / 1.5 (direct ms_abi call) |
| host->guest call + return | 137.2 / 133.5 (`call_guest`) | - | 1.3 / 1.2 |

- **Compute.** Integer, branchy and SSE code run at Rosetta's speed.
- **TSO.** Loads and stores pay for FEX's TSO emulation (it is on; Apple's hardware TSO bit is not available to third
  parties): 1.3x Rosetta. `GTA6RT_FEX_TSO=0` turns it off and is unsafe for the title's threads.
- **AVX2.** FEX is 4.7x faster than Rosetta here.
- **Boundaries.** These are the costs:
  - a guest->host import call through the FEX thunk opcode costs about 38-40 ns, against about 1.5 ns under Rosetta;
  - a host->guest callback costs about 135 ns.
  - The thunk cost was 41-45 ns before the `set_fpcr` change (logs `build/fexcore-arm64-bench-r1/r2.log`, earlier
    session, different load).
  - Boot to WinMain makes 37k import calls, about 1.4 ms of thunk cost. The x86_64 frame loop made about 1.3 M import
    calls/s, which at 38 ns is about 5% of a core. This path is not tuned further until an arm64 run reaches the frame
    loop.

## 11. Known gaps (all fail loudly)

- NATIVE thunks without a signature: `GTA6RT_E_UNSUPPORTED` with a log line. Use FRAME, or API 6.
- Guest `syscall`: a log line for the first 16, then rax=STATUS_NOT_IMPLEMENTED. Never seen from the title.
- `div` by zero: no #DE (section 7).
- MONITORX/MWAITX wait emulation (section 6).
- SMC invalidation (section 8).
- `gta6rt_isa_enter_host_from_signal` returns 0 and `leave` is a no-op. Under FEX, host handlers never see guest GS.
- `gta6rt_isa_host_features` reports FEX's raw set (0xeff). `reexec_for_cpuid` is OK (nothing to do).
- FEX internal traps (host SIGILL/SIGTRAP/SIGFPE inside JIT code) log `FATAL` and chain to the previous handler.
- Darwin shim syscalls without an implementation (`tgkill`, `statx`, `renameat2`, `pidfd_open`, futex ops other than
  WAIT/WAKE and their BITSET forms): `[fex compat] ... unimplemented: ENOSYS` on stderr, then -1/ENOSYS.
- Thread suspend and get-context through FEX's interrupt page: unavailable (section 8).

## For the integrator (changes outside isa/fexcore)

1. **`host/src/main.cpp`.** Apply `isa/fexcore/host_integration/main.cpp.patch` (8 hunks; applies to the current file).
   All of it is `#if !defined(__x86_64__)` except one `#include "register.h"`. The hunks:
   - (a) seh exports. `RtlPcToFileHeader`, `SetUnhandledExceptionFilter` and `RtlLookupFunctionEntry` are served as
     FRAME handlers over seh's public API, because `gta6rt_seh_native_export` returns 0 on arm64. `RtlPcToFileHeader`
     is called by msvcp140's DllMain before WinMain.
   - (b) `GetProcAddress` hook. It wraps the shim row's frame handler instead of an ms_abi function, with the same
     missing-proc notes and trace wrapping.
   - (c) `--stop-at` marker. A FRAME thunk plus `movabs rax; jmp rax` at the RVA, instead of `GTA6RT_E_UNSUPPORTED`.
   - (d) `d3d12_xs.dll!D3D12ConfigureVirtualMemoryX`. It is called before WinMain and is served by a frame handler
     that calls d3d12_vulkan's export directly, while `gta6rt_d3d12_vulkan_register()` is unsupported on arm64.
   - (e) exe fixup flags on arm64 = `GTA6RT_ISA_FIXUP_CHKSTK | GTA6RT_ISA_FIXUP_ZEN` (section 6).
2. **`host/src/trace.cpp`.** Apply `host_integration/trace.cpp.patch` so `--trace` works on arm64. The 24-byte stubs
   stay x86 code; the trampoline is the bytes of `trace_x86_64.S` with its call replaced by a FRAME thunk, and FEX runs
   it from RW memory.
3. **`host/CMakeLists.txt`.** Nothing is needed for development builds. A hardened/notarized arm64 build needs a
   post-build `codesign -s - -f -o runtime --entitlements isa/fexcore/sign/jit-arena.entitlements`, or `jit.entitlements`
   after item 4. Also `include(src/win/win.cmake)`: the stock arm64 `gta6rt` in my tree does not link without it
   (undefined `host::win::*`), which is why `host_integration/CMakeLists.txt` mirrors it.
4. **`isa/common/x86_patch.cpp` (isa owner).** In `arena_map`, on `!__x86_64__`, map the arena
   `mmap(hint, kArenaSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON)` with `wdelta = 0` instead of the RX view plus
   RW alias. With that, `allow-jit` alone is enough (section 9). If that lands, host `address_is_code()` must recognise
   arena addresses another way on arm64, or the GetProcAddress trace stubs are skipped.
5. **`isa/rosetta`.** Define `gta6rt_isa_make_native_thunk` (forward to `make_thunk`, signature ignored), and the API 7
   pair (return `GTA6RT_E_UNSUPPORTED` / abort). Today they are declared in isa.h but undefined on x86_64, which only
   matters if someone calls them.
6. **`isa/common` (isa owner).** `gta6rt_isa_flush_icache` and `code_write` should call a backend hook. fexcore would
   implement it with `InvalidateGuestCodeRange`; that is needed before any code is patched after it ran, or an image
   unloaded and reloaded.
7. **seh (arm64).**
   - `fault.cpp`'s `register_isa_fault_handler` is x86_64-only, so no handler is registered on arm64. That is the
     "no handler registered" abort after WinMain.
   - The isa side is ready: records plus CONTEXT, RESUME with an edited CONTEXT, and API 7 for
     RtlCaptureContext/RtlRestoreContext/the RtlUnwindEx end.
   - `current_stack()` uses the host frame address. On arm64 it should use the guest rsp from the CONTEXT against the
     TEB bounds.
8. **shims_core.** `GetCurrentThreadStackLimits` reads os's pthread bounds. On threads with a separate guest stack
   (under 512 KiB) it should read TEB+0x08/+0x10, as Windows does. Also on arm64, os's CreateThread could round
   `STACK_SIZE_PARAM_IS_A_RESERVATION` sizes up to 8 MiB like the others, so every title thread splits.
9. **`host/tools/run.sh`.** Its guard is `pgrep -f "build/macos/gta6rt"`, which misses every other binary (other
   workers' trees, `gta6rt_fexhost`). Matching the process name (`ps -axo comm` ending in `/gta6rt` or
   `/gta6rt_fexhost`) would enforce one host at a time.
10. **Docs.** These are now out of date:
    - `port/third_party/README.md` ("libFEXCore: not vendored yet");
    - `port/isa/STATUS.md` "Stubs" section;
    - `INTERFACES.md`'s isa section (API 6/7).

## Interface changes (`include/gta6rt/isa.h`, additive)

- API 6: `gta6rt_isa_make_native_thunk` and the `gta6rt_native_sig` / `gta6rt_abi_arg` types. These were added in the
  previous session and are unchanged here.
- API 7 (this session): `gta6rt_isa_capture_guest_context`, `gta6rt_isa_resume_guest_context`, and
  `GTA6RT_ISA_API_VERSION` 7. No existing declaration changed. The x86_64 isa, seh, loader and shims_core libraries
  compile with it, and the x86_64 `isa_unit_test` passes 343 checks.

## Files

- **Backend:**
  - `fexcore_backend.cpp`: init, threads/stacks, thunks, call_guest, signals, exceptions, API 7.
  - `fexcore_native.cpp`: API 6 marshalling.
  - `fexcore_asm.S`: exception entry, resume, native call.
  - `fexcore_internal.h`.
- **Tests:** `tests/isa_fex_test.cpp`, `tests/fex_guest.S`, `tests/bench_x86.c`, `tests/fex_futex_test.cpp`,
  `tools/fex_blob.py` (origin: `spike/x6-fexcore/harness/mkblob.py`).
- **Pre-integration host:** `host_integration/{CMakeLists.txt, main.cpp.patch, trace.cpp.patch}`. Built only with
  `-DGTA6RT_FEX_HOST=ON`; before this session it was built in every all-modules arm64 tree.
- **Signing:** `sign/{sign_hardened.sh, jit.entitlements, jit-arena.entitlements}`.
- **Vendored FEX:** `port/third_party/fex/{README.md, fex.cmake, verify_vendor.sh, patches/, compat/, src/}`.
- **Build trees:**
  - `port/build/fexcore-arm64` (os;isa);
  - `port/build/fexcore-host-arm64` (all, `GTA6RT_FEX_HOST=ON`);
  - `port/build/fexcore-host-x86_64` (all, the x86_64 comparison host).

## Review response (round 3)

Review: `REVIEW-round3-fexcore.md` (APPROVED WITH FIXES, one blocking defect). Measured 2026-11-26 12:21-12:31, load
average 11-33 from other workers.

### Defect 1: futex wake ignored the bitset, deadlocking FEX's `WritePriorityMutex` - fixed

**Cause** (as the review says). FEX's `WritePriorityMutex` (`LookupCache::FindBlock` / `AddBlockExecutableRange`, so
every block compile) keeps readers and writers asleep on one futex word. It wakes "one writer" with
`FUTEX_WAKE_BITSET(1, WRITERS)`. The shim dropped the bitset and called `os_sync_wake_by_address_any`, which can wake a
reader; the reader sleeps again and the writer's wake is lost. The lock then has no owner and every thread sleeps.

**Fix, in `third_party/fex/compat/fex_darwin_compat.cpp`:**
- `os_sync` cannot pick waiters by bitset, so a wake filtered by bitset (`FUTEX_WAKE_BITSET` with a bitset other than
  `FUTEX_BITSET_MATCH_ANY`) wakes every waiter on the word. Futex users must re-check the word after any wake, and
  FEX's do.
- An unfiltered wake of one (`FUTEX_WAKE`, `InterruptableConditionVariable::NotifyOne`) still wakes one; all its waiters are
  equivalent.
- The shim now reads the 6th argument (`val3`, the bitset) for the two bitset commands.
- Two more silent paths fixed while there:
  - `FUTEX_WAIT_BITSET` with a timeout used to wait forever. It now honours the absolute deadline on
    `CLOCK_MONOTONIC`, or `CLOCK_REALTIME` with `FUTEX_CLOCK_REALTIME`.
  - An unknown futex op, and any shim syscall without an implementation (`tgkill`, `statx`, `renameat2`,
    `pidfd_open`), returned ENOSYS without a word. They now log `[fex compat] ... unimplemented: ENOSYS`; a syscall
    logs on its first call only.
- FEX's own build picks the change up automatically: the configure-time stamp hashes the compat files. Rebuilt in
  `build/fexcore-arm64` and `build/fexcore-host-arm64`; 0 warnings in FEX's build log.

**New tests.**
- `tests/fex_futex_test.cpp` (new target `fex_futex_test`) tests the shim with host threads only:
  - `bitset`: 16 readers and 1 writer (in the middle) asleep on one word, then `WAKE_BITSET(1, WRITERS)`. The writer
    must wake within 2 s.
  - `mutex`: FEX's real `WritePriorityMutex` with 4 and then 16 threads, 4,000 operations each. One in 8 is a write
    held 15-35 us, longer than the mutex's 10 us WFE spin, so both sides reach the futex. Exclusion and torn reads
    are checked.
  - `timeout`: relative `FUTEX_WAIT`, absolute `FUTEX_WAIT_BITSET`, a deadline in the past, and a value mismatch.
  - `cv`: `InterruptableConditionVariable` timeout and `NotifyOne`.
  - `unknown`: ENOSYS plus the log line.
  - A watchdog exits with code 3 and a named `FAIL ... deadlock` line after 5 s without progress.
- `isa_fex_test` gains the concurrent first-compile race the review asked for (`test_concurrent_compile`, see "What
  `isa_fex_test` covers"). The default run is 16 threads x 32 rounds; `--race=THREADS,ROUNDS` runs only the race.

**Before and after, the same test sources.** "Before" binaries were built against the unmodified shim and kept in the
session scratchpad.

| test | old shim | fixed shim |
|---|---|---|
| `fex_futex_test bitset` | FAIL: `writer NOT woken after 2 s, 1 reader(s) woken too` | PASS: writer woken (all 16 readers woken too, by design) |
| `fex_futex_test mutex` (4 threads first) | **5 of 5 runs deadlocked** at 4 threads, futex word `0x40010000` (read-waiter bit, 1 writer waiting, no owner) 4 times, `0x40020000` once | 20 of 20 runs PASS at 4 and 16 threads; 16 threads take 475-755 ms |
| `fex_futex_test timeout` | `FUTEX_WAIT_BITSET` absolute +30 ms never returned (watchdog) | ETIMEDOUT after 30.0 ms; relative `FUTEX_WAIT` 30.5 ms |
| `isa_fex_test --race=2,32` | 3 of 3 PASS; 10 of 10 in a second batch | 15 of 15 PASS |
| `isa_fex_test --race=4,32` | 3 of 3 PASS in the first batch, **7 of 10 deadlocked** in the second (load 14-22) | 15 of 15 PASS |
| `isa_fex_test --race=8,32` | **3 of 3 deadlocked** (rounds 24, 24, 1) | 15 of 15 PASS |
| `isa_fex_test --race=16,32` | **3 of 3 deadlocked** (rounds 6, 0, 0) | 15 of 15 PASS |
| `isa_fex_test --race=32,32` | not run | 15 of 15 PASS |
| `isa_fex_test` (full, includes 16 x 32 race) | (deadlocks in the race, as above) | 6 of 6 PASS, 112,179-113,115 checks |

So the fixed shim passed 75 processes x 32 rounds = 2,400 contended first-compile rounds at 2-32 threads, with 0
hangs and 0 wrong results. The old shim hung in 13 of 19 processes at 4-16 threads (7 of 13 at 4, 3 of 3 at 8, 3 of 3 at 16).

**Cost of waking everybody.** It is only measurable where the old shim survives.
- 2 threads x 128 rounds, 8 interleaved pairs, load 17-18: old 176-210 ms total (median 182.8), new 171-201 ms
  (median 185.5). Per-round medians 1.28-1.50 ms against 1.28-1.46 ms.
- 4 threads x 32 rounds: new 33-72 ms against the 3 old runs that survived, 36-57 ms.
- No difference is resolvable above the noise.

**Real title, arm64 host with the fixed shim** (`build/fexcore-host-arm64/gta6rt_fexhost`, rebuilt, through
`host/tools/run.sh`; before each run I checked with `ps` that no other host was running, because run.sh's guard
still matches only `build/macos/gta6rt` and another worker's `build/d3d12_vulkan-host-x86_64/gta6rt` was running):
- `fexr5-arm-winmain-round3.log`: `REACHED MARKER: RVA 0x5a91bd0`; `calls: 37062 total, 36765 from the exe, 84 distinct
  imports called (0 unimplemented), 765 wrapped`; 0 isa traps. The same as before the fix.
- `fexr5-arm-past-winmain-round3.log`: the same wall as before. `LoadLibrary(xgameruntime.dll): module not found`,
  then `exception 0x80000003 at 0x145a91c21`, no handler, rc 134 after 37,068 calls.
- Boot-to-WinMain time, interleaved with the reviewer's old-shim build of the same host
  (`build/review-fexcore-host-arm64/gta6rt_fexhost`, 11:56), 6 pairs, `--trace=0`, `fexr5-ab-{old,new}-N.log`, load 14-33:
  - real: old 0.35-0.92 s, new 0.42-0.66 s;
  - user: old 0.31-0.41 s, new 0.37-0.43 s;
  - every run reached WinMain.
  - No regression is visible; the load makes these slower than the 0.24-0.31 s measured earlier at load 9-11.
- **What the title run does not show.** The boot path has one guest thread, so it never contends on the compile
  lock. The worker-thread case is shown only by the tests above. No arm64 run gets past the `xgameruntime.dll` wall
  yet.

### Other findings

- **4 (vacuous check)** removed from `isa_fex_test` (`CHECK(t_fault.p0 == 0 || true, "")`).
- **1 (silent divide error)** is not fixed. FEX's JIT has to emit the zero check, and that would be a third FEX
  patch. It stays in "Known gaps" and is still integrator item 12 (optional hardening).
- **2 (small host stack on 512 KiB threads)** and the integrator items are unchanged.
- The claims "4 threads PASS" (spike) and "7 concurrent threads" (`isa_fex_test`) do not show the backend is
  deadlock-free. The review is right: those threads did not compile concurrently. The concurrent first-compile race
  above is the evidence for that now.

### Integrator list

- Review item 1 (the shim) is done in `third_party/fex`. Existing arm64 trees rebuild FEX's `fex_darwin_compat.o` on
  their next configure (stamp hash); nothing to do by hand.
- Every other item in "For the integrator" stands as written.
