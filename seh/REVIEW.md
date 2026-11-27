# Review of S1.3 SEH (gta6rt_seh): port/seh

Verdict: **APPROVED_WITH_FIXES**. No blocking finding. Every test result in the builder's report reproduced exactly. The
code follows Wine/WRK semantics closely and I found no ABI, alignment or LLP64 defect on the paths the exe and the packaged
DLLs use. The "fixes" are documentation gaps and latent risks for the integrator (listed below), none of which crash or
corrupt the guest on the boot path as it exists today. I changed nothing outside this file. My build trees are
`port/build/review-seh-{arm64,x86_64,loader-x86_64}`, outputs in `port/build/review-seh-out/`, scratch tests in the
session scratchpad.

## What I re-ran (x86_64 strictly one at a time under `run_timeout.sh`, load average 2 to 4)

| Check | Result |
|---|---|
| Clean configure + build, `-DGTA6RT_MODULES="os;isa;seh"`, arm64 and x86_64, `-j3` | both succeed, 0 warnings. Also 0 warnings with `-Wall -Wextra -Wshadow -Wconversion` over the five seh `.cpp` files (the project itself sets no warning flags, so the builder's "0 warnings" is true but weak; the stronger check also passes) |
| Consumers still compile against the edited `seh.h` / `winnt_seh.h` (`os;isa;seh;loader;shims_core;host`, x86_64) | builds, no errors |
| `seh_unit_test` arm64 native | 284 checks, PASS |
| `seh_unit_test` x86_64 (Rosetta) | 301 checks, PASS |
| `seh_dispatch_test` | 42 checks, PASS; 1.9 us per throw/catch (report: 2.0 to 2.2) |
| `seh_pdata_exec_test`, exe, every function (stride 1, regenerated vectors) | **3,113,905 vectors: P 1,562,265 / E 1,550,210 / C 739 / F 691, all pass, 0 fail, 0 signalled**, 276 s; lookup 299,257 entries 0 misses; handlers 660 ok / 0 wrong, 298,519 none ok; establisher 3,113,905 match, 623,617 body-point matches, 0 bad. Skip counts from the generator (23 + 53 + 46 = 23 functions + 99 fragments) match the report |
| Same, all 16 packaged DLLs, stride 1 | all PASS; totals P 42,684 / E 23,726 / C 1,321 / F 1,468 = report; v2 trap points 10 + 10 + 12 = **32/32** (vcruntime140, XCurl, libHttpClient) |
| The 12 "harness segfault" vectors | present in the generated set (E vectors inside 0x1febdd0: 3, 0x2d02bd0: 5, 0x513ef20: 4, so they were fixed, not dropped) and pass; whole run has 0 signalled |

## Extra checks I added (all PASS; not in the builder's list)

Scratch copy of `dispatch_test.cpp` with extra sections (`.../scratchpad/dispatch_threads.cpp`, `guest_lj.cpp`), run against
the same package/exe: 65 checks, 0 failed.

1. **Non-main threads**: 4 concurrent pthreads, each doing catch(int), 300 throw/catch, a hardware fault into `__except`,
   and RaiseException into `__except`. All correct. (The report lists non-main threads as not exercised.)
2. **Isa-attached thread**: `gta6rt_isa_thread_attach` with a real TEB (so `current_stack()` takes the TEB StackBase/Limit
   branch), then `gta6rt_isa_call_guest4` into catch / AV / SEH / loop. All correct, so the host-to-guest trampoline plus
   TEB path works for dispatch.
3. **Non-pthread stack** (makecontext stack from mmap, as a fiber would be): catch, AV, SEH all correct.
4. **`longjmp` with a frame (STATUS_LONGJUMP)**: guest fixture calls the packaged vcruntime140's `__intrinsic_setjmp` and
   `longjmp` across a `__try/__finally` and a C++ destructor frame; `longjmp` calls `RtlUnwindEx` in seh. Result:
   returns 1005 (and 1001 for `longjmp(jb, 0)`, as C requires), `__finally` and destructors ran in order
   (`1:22 1:11 2:11 9:1 2:22`). The exe imports exactly these two (`__intrinsic_setjmp`, `longjmp` from vcruntime140), so this
   was a real open risk for the title and it is now covered. The jump buffer's Frame was computed with seh's own
   `RtlVirtualUnwind` establisher; the establisher rule itself is independently validated by the 3.1 M-vector run.

## Code read

Read in full: `unwind.cpp`, `dispatch.cpp`, `seh_x86_64.S`, `fault.cpp`, `api.cpp`, `tables.cpp`, `arch_other.cpp`,
`seh_internal.h`; read the harness `pdata_exec_test.cpp` and the first 500 lines of `dispatch_test.cpp`.

Checked and found correct:
- Win64 vs SysV: every guest-callable entry is ms_abi or an asm stub; the stubs save the full caller context, keep the
  stack 16-aligned (0x518 frame, 0x28+0x110 in the trampolines), restore rsi/rdi/xmm6-15 where they return
  (`RtlCaptureStackBackTrace`) and restore everything from the captured CONTEXT otherwise. Argument shuffles for
  RaiseException, RtlUnwindEx (5th/6th stack args from entry rsp), RtlUnwind, `_CxxThrowException` are right.
- Trampoline UNWIND_INFO (39 slots, ALLOC_LARGE + SAVE_NONVOL + SAVE_XMM128 + PUSH_MACHFRAME with the padding slot, handler
  after the padded codes): byte counts add up; the machine-frame trick makes the unwinder jump from a handler frame
  straight back to the guest context.
- `RtlCaptureContext` (flags 0x10000F, Rcx = pointer as on Windows, fxsave alignment as on Windows), `RESTORE_GPRS_AND_JUMP`
  (scratch writes land in dead stack below the target rsp, ctx is not overwritten in any of the three callers).
- DISPATCHER_CONTEXT (80 bytes), KNONVOLATILE_CONTEXT_POINTERS (256), UNWIND_HISTORY_TABLE (216) layouts; `_JUMP_BUFFER`
  offsets in the longjmp branch (Frame 0, Rbx 8, Rsp 0x10 ... Rip 0x50, MxCsr 0x58, FpCsr 0x5c, Xmm6.. 0x60).
- Establisher-frame rule, epilog rule, chained info, v2 `UWOP_EPILOG` slot sizes, handler from chain root, no handler in
  prolog or epilog.
- Nested/collided dispositions and flag handling follow WRK; consolidate and longjmp follow Wine's `RtlRestoreContext`.
- Thread safety: policy/filter atomics, VEH list under a mutex with a snapshot, function tables append-only with release/
  acquire, all per-thread dispatch state `thread_local`. Verified by the 4-thread run above.
- Error semantics the guest sees: BOOLEAN/ULONG/PVOID/WORD return types of the Rtl*/VEH/filter exports, `RtlAddFunctionTable`
  FALSE on exhaustion, `SetUnhandledExceptionFilter` returns the previous filter, VEH `first` flag, `RtlPcToFileHeader`
  returning base through both outputs.
- Stubs: STATUS.md lists every arm64 stub and the x86_64 vcruntime stand-ins; none silently succeeds in a way that
  matters. arm64 `RtlCaptureContext` returning a zeroed CONTEXT is listed.

## Findings (none blocking)

1. **Isa fault path loses YMM upper halves on resume (builder's item 3; confirmed, and broader than stated).** It applies to
   any dispatch that ends in "continue execution" after a hardware fault (VEH fix-up, `__except` filter returning -1),
   not only to VEH, and the damage depends on host code in the dispatch path executing `vzeroupper` (libSystem's AVX
   `memcpy`/`memset` can). Silent corruption of live AVX values, only in the resume-after-fault case. Needs the isa
   change the builder proposes (pass the ucontext, or the raw ymm halves, to the fault callback). Bink2Scarlett imports
   `AddVectoredExceptionHandler`, so someone should find out what that handler does before this is left open.
2. **Native exports assume TEB mode SHARE.** The seh entries run host code (C++ `thread_local`, libc) directly on the
   guest's GS state because they are bound without a thunk, and the post-fault redirect runs host code without
   `gta6rt_isa_enter_host_*`. Under SHARE (the S0.6 decision) the host TSD slots are intact and this works (verified with
   an attached thread). Under SWAP (kept as a fallback) these paths would run host TLS code with the guest GS. STATUS
   should say "SHARE only".
3. **Stack-overflow reporting needs a sigaltstack that nobody installs.** `grep` over isa/os/loader/shims_core/host finds
   no `sigaltstack`. Without one a guest stack overflow cannot even deliver the signal, so the "stack overflow (no room)
   -> fatal report" path in `fault.cpp` is unreachable today (the report lists stack overflow as not exercised but
   STATUS presents the path as implemented). `seh.h` documents the handler as running "on a sigaltstack".
4. **Latent bug in `place_frame` once an altstack exists.** The rule `cur < rsp && rsp - cur < 1 MiB` is meant to detect "the
   signal handler runs on the faulting stack". If the handler runs on an altstack that is mapped within 1 MiB below the
   guest rsp (mmap'd regions are often adjacent), the fault frame and the whole dispatch would be placed in or under the
   altstack instead of on the guest stack. Safer test: whether `cur` lies inside the pthread stack range already computed
   in that function. Not reachable with today's integration (no altstack).
5. **Host frames between guest frames end the walk (builder's item 4).** Agreed, correctly documented. Practical effect is
   limited to DISPATCH mode; under LOG_AND_ABORT it only truncates the stack report.
6. **`RaiseFailFastException` has no owner.** XCurl imports it; seh does not export it and nothing else in `port/` defines
   it. Not an seh task as written, but it is the same family and will be an unbound import (trap) at XCurl load.
7. **Behaviour that differs from Windows and is not in STATUS's stub list**: a noncontinuable exception continued by a
   handler aborts instead of raising STATUS_NONCONTINUABLE_EXCEPTION (and hardware faults are never checked); the default
   `UnhandledExceptionFilter` logs and returns EXCEPTION_EXECUTE_HANDLER without any WER step; `RtlPcToFileHeader` falls
   back to host module bases for host addresses; `RtlCaptureStackBackTrace`'s hash is a plain sum; `RtlLookupFunctionEntry`
   reports ImageBase 0 for a leaf pc inside a registered image (Windows reports the image base); `__fastfail` is never
   dispatched (as on Windows). All low impact.
8. **Function-table slots are never reclaimed**: 8,192 lifetime adds (including removed ones) then `RtlAddFunctionTable`
   returns FALSE. Relevant only if something JIT-registers repeatedly. A freed/unmapped image still being read lock-free by
   another thread is a theoretical use-after-unmap.
9. **Test scope notes (the report states most of them):** all dispatch tests use the test's own mini PE mapper and stand-in
   imports (FLS, critical sections, `terminate`, ...), not the project loader; vcruntime140_1's preferred base collides with
   vcruntime140's, so the relocated-DLL case is what actually ran for FH4. Real boot (loader + shims binding the exe's
   imports to `gta6rt_seh_native_export`) is still untested; nothing in `loader/` or `shims_core/` calls
   `gta6rt_seh_native_export` or `gta6rt_seh_vcruntime_fallback` yet. Not covered by anyone: table callbacks,
   `RtlUnwind` (old entry), collided unwinds, FH4 noexcept/terminate in phase 1, FH3/FH4 interplay with FLS from the
   real shims.
10. **Cosmetic**: the fatal report names a vcruntime frame by `image 0x180000000+0x6480` rather than by DLL name; the
    "throw site" index is hard-coded as frame 1 for real C++ throws (right while `_CxxThrowException` calls RaiseException
    directly, as in the packaged DLL); anonymous-namespace types print as `?A0x56938760::name`.

## Claims check

Report/STATUS claims I could test were all true (counts, pass/fail, skip numbers, 32/32 v2, the 12 vectors, FH4 results
including `bad_alloc` -> 0x8007000E, abort paths in forked children, both arches build). Not verified: "comment-only
changes in `include/gta6rt/seh.h`" (no baseline to diff; all consumers still compile) and the 2.0 to 2.2 us timing (I
measured 1.9 and 2.1). The "Not exercised" list is honest; items 1 to 4 of my extra checks above shrink it (non-main
threads, longjmp with a frame).

## Files

Reviewed: everything under `/Users/a/Desktop/Projects/GTA-VI/port/seh/`, plus isa fault code
(`port/isa/rosetta/rosetta_backend.cpp`) and `port/loader/src/package.cpp` for the registration call.
Written: this file only.
