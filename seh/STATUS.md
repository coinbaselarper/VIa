# seh (S1.3): status

Own `.pdata` unwinder and dispatcher, Wine's model (spike/s1-seh RESULT.md option (b), NOTES.md S0.6), with the
packaged `vcruntime140.dll` / `vcruntime140_1.dll` mapped unchanged as the language handlers. Step 1
(`GTA6RT_SEH_LOG_AND_ABORT`: log code, throw site, C++ type and a `.pdata` stack walk, then abort) is the default the
host passes and stays selectable at run time. Runtime parts are x86_64 (Rosetta); the arm64 build compiles and its
data-level parts run (stubs listed below).

## What is implemented

| file | contents |
|---|---|
| `src/tables.cpp` | function-table registry: images (`gta6rt_seh_add_function_table` with image size), `RtlAddFunctionTable`/`RtlDeleteFunctionTable`, `RtlInstallFunctionTableCallback` (guest callback, x86_64), binary search on sorted tables, linear on unsorted, indirect (bit 0) entries, `RtlPcToFileHeader`/`image_base_of`; append-only slots so fault paths read without locks |
| `src/unwind.cpp` | `RtlVirtualUnwind`: UNWIND_INFO v1 and v2, all opcodes (PUSH_NONVOL, ALLOC_SMALL/LARGE both forms, SET_FPREG, SAVE_NONVOL(_FAR), SAVE_XMM128(_FAR), PUSH_MACHFRAME with/without error code; v2 `UWOP_EPILOG` slot rule and epilog descriptors), chained infos, Windows (WRK) establisher-frame rule (frame register only after SET_FPREG has executed, or outside the prolog, or chained), WRK epilog rule (add/lea rsp, pops incl. REX pops, `ret`/`rep ret`/`ret n`, `jmp` out of the function or to its own start, `jmp [rip+x]`, `rex.w jmp r/m`; a `jmp` into another fragment of the same chained function is not an epilog), epilog check only on the primary entry, KNONVOLATILE_CONTEXT_POINTERS, handler + HandlerData from the chain root, prolog → no handler; stack walk with a leaf rule inside registered images and validated reads for backtraces |
| `src/dispatch.cpp` | `RtlDispatchException`: vectored handlers, then (DISPATCH) the frame walk calling language handlers with a real `DISPATCHER_CONTEXT`, nested (`EXCEPTION_NESTED_CALL`) and collided dispositions with handler repeat, noncontinuable check, `STATUS_STACK_BUFFER_OVERRUN` (fastfail) never dispatched, unhandled → `SetUnhandledExceptionFilter` filter → fatal report. `RtlUnwindEx`/`RtlUnwind`: target frame/ip, `EXCEPTION_UNWINDING`/`EXIT_UNWIND`/`TARGET_UNWIND`/`COLLIDED_UNWIND`, invalid-target check, `STATUS_UNWIND_CONSOLIDATE` (calls the callback on a frame whose unwind leads to the target, then continues at its return value) and `STATUS_LONGJUMP` (restores the `_JUMP_BUFFER`). Fatal report: code name, parameters, AV kind/address, fastfail code, C++ type with catchable types and `what()`, throw site, `.pdata` backtrace, registers. Under LOG_AND_ABORT the informational codes 0x406D1388 (SetThreadName), 0x40010006/0x4001000A (OutputDebugString) are logged and continued |
| `src/seh_x86_64.S` | Windows-ABI entries that capture their caller's context: `RtlCaptureContext`, `RaiseException`, `RtlRaiseException`, `RtlUnwindEx`, `RtlUnwind`, `RtlCaptureStackBackTrace`, the step-1 `_CxxThrowException` stand-in; `seh_restore_raw` (RtlRestoreContext, with/without YMM upper halves); the four host→guest trampolines with their own UNWIND_INFO/RUNTIME_FUNCTIONs (registered as an image), whose PUSH_MACHFRAME leads the unwinder from a handler straight back to the guest context it was called for, and whose handlers implement nested-exception and collided-unwind detection (WRK `RtlpExecuteHandlerForException/ForUnwind`) |
| `src/fault.cpp` | hardware faults: isa fault callback and `gta6rt_seh_handle_signal` copy `EXCEPTION_RECORD` + `CONTEXT` (+ YMM upper halves when the ucontext has them) onto the faulting thread's stack below the live handler frames and redirect there, so dispatch runs after the signal handler returns (KiUserExceptionDispatcher model); Darwin ucontext ↔ CONTEXT; signal → record (`int3` → BREAKPOINT with rip on the int3, `int 29` → fastfail with rcx, `int 2c`, AV read/write/execute with address, SIGFPE codes, SIGILL/priv); stack overflow (no room) → fatal report |
| `src/api.cpp` | `include/gta6rt/seh.h` (API v2), the Win32 surface in compat (`RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlRestoreContext`, `RtlPcToFileHeader`, Add/Remove Vectored Exception/Continue Handler, `SetUnhandledExceptionFilter`, `UnhandledExceptionFilter`), `gta6rt_seh_native_export` (19 names) and `gta6rt_seh_vcruntime_fallback` (4 names), `GTA6RT_SEH_POLICY` / `GTA6RT_SEH_VERBOSE` |

Shared-file changes: `compat/include/gta6rt/winnt_seh.h` gained one declaration,
`PVOID NTAPI RtlPcToFileHeader(PVOID, PVOID*)` (additive). `include/gta6rt/seh.h`: comments only (init env vars,
`gta6rt_seh_raise`/`gta6rt_seh_unwind` semantics); no signature changed, API version stays 2.

## Stubs (every one)

arm64 (`src/arch_other.cpp`, `#if` blocks in `api.cpp`/`fault.cpp`/`dispatch.cpp`):
- `RtlCaptureContext`: zeroes the CONTEXT and sets `CONTEXT_FULL` (no x86 state on an arm64 host).
- `RaiseException`, `RtlRaiseException`: dispatch with a zero context; vectored handlers run (guest ones through
  `gta6rt_isa_call_guest4`), then frame-based dispatch aborts with the fatal report.
- `RtlUnwindEx`, `RtlUnwind`, `gta6rt_seh_unwind`, `RtlRestoreContext` (`seh_restore_raw`, `seh_restore_raw_ymm`):
  fatal "x86_64-only".
- `gta6rt_seh_raise`: fatal if a handler continues.
- `RtlCaptureStackBackTrace`: returns 0. `RtlInstallFunctionTableCallback`: returns FALSE.
- `gta6rt_seh_handle_signal` returns 0; `gta6rt_seh_context_from_ucontext`, `_to_ucontext`, `gta6rt_seh_record_from_signal`
  return `GTA6RT_E_UNSUPPORTED`; `gta6rt_seh_native_export` and `gta6rt_seh_vcruntime_fallback` return 0; the isa
  fault handler is not registered.

x86_64, by design: the step-1 stand-ins from `gta6rt_seh_vcruntime_fallback` — `__CxxFrameHandler3`,
`__CxxFrameHandler4`, `__C_specific_handler` log once and return ExceptionContinueSearch; `_CxxThrowException` is
real (builds the 4-parameter record, dispatches). They are only for running without the packaged vcruntime.

Not implemented / ignored: `UNWIND_HISTORY_TABLE` caching (accepted, not used); CONTEXT_XSTATE; WRK's
`RtlpSameFunction` is approximated by comparing chain roots.

## Tests (all run on 2026-11-26, this machine, load average 9-14 from other workers)

Build: `build/seh-arm64` and `build/seh-x86_64` (`-DGTA6RT_MODULES="os;isa;seh"`), 0 warnings. `seh/tools/run_tests.sh
[full]` rebuilds and runs everything below, x86_64 binaries one at a time under `seh/tools/run_timeout.sh`. Outputs are
in `build/seh-vectors/`.

1. `seh_unit_test` (synthetic images, both arches): **arm64 native 284 checks PASS; x86_64 under Rosetta 301 checks
   PASS**. Covers every opcode incl. the ones the exe never uses (SAVE_*_FAR, ALLOC_LARGE 32-bit, MACHFRAME both
   forms), v2 infos (the vcruntime140 memcpy shape, and an odd EPILOG count that a v1 slot rule would misparse),
   frame register inside the prolog before SET_FPREG, chained fragments with jumps into the parent vs tail calls,
   ten epilog terminators, handler/HandlerData/type filter, context pointers, indirect entries, dynamic tables,
   leaf rule, backtrace; x86_64 also RtlCaptureContext/RtlRestoreContext round trip, RaiseException continued by a
   vectored handler, informational code under LOG_AND_ABORT, ucontext ↔ CONTEXT and signal → record.
2. `seh_pdata_exec_test` + `tools/gen_vectors.py` (the spike's executed check, ported to the module's
   `RtlLookupFunctionEntry`/`RtlVirtualUnwind`): maps the image at its preferred base, runs real prolog/epilog code
   under Rosetta, traps with int3 and requires rip = sentinel, rsp = entry+8, the eight nonvolatile GPRs and
   xmm6-15 bit-exact, and the establisher frame equal to the WRK rule (and to the post-prolog rsp at body points).
   Also every entry found at its first/last byte and every handler-bearing entry's handler + HandlerData.
   - exe, every function (stride 1, `build/seh-vectors/exe_all.out`): **3,113,905 vectors, 0 fail, 0 signalled**
     (P 1,562,265; E 1,550,210, all detected as epilogs; C 739; F 691), 269 s. Lookup: 299,257 entries, 0 misses.
     Handlers: 660 handler-bearing entries (after chains) report the right handler and HandlerData, 298,519 others
     report none. Establisher frame: 3,113,905 match the WRK rule; 623,617 body-point checks equal the post-prolog
     rsp. Negative control (leaf-style unwind) would pass only 300,173 P and 326,480 E. Not run: 23 functions whose
     prolog branches or reads memory through body registers, 99 chained fragments (53 same reason, 46 whose codes
     are performed by a sibling fragment). The exe has no v2 infos.
   - the 16 packaged DLLs, every function: **all PASS, 0 fail, 0 signalled**; lookup 0 misses and handler checks
     0 wrong in each. Totals: P 42,684, E 23,726, C 1,321, F 1,468 vectors. The **seven version-2 functions**
     (vcruntime140 0x12d0/0x1980, XCurl 0x1e1b0/0x1e850, libHttpClient 0x5d014/0x5d064/0x5d14c) have 32 trap points
     covering every prolog and epilog boundary: **32/32 pass**.
   - The reviewer's 12 SIGSEGV epilog vectors (0x1febdd0, 0x2d02bd0, 0x513ef20) are fixed in the generator: restore
     moves are only `[rsp + imm]` loads. Those 12 trap points (3 + 5 + 4 E vectors) are in the stride-1 set above
     and pass. Further changes, each explained in the generator's
     docstring: all canonical epilogs per function (not only the first) including pops + `jmp` tail calls;
     `__chkstk` prologs (the harness patches `__chkstk`, or its jmp thunk, to `ret`, and leaves it unpatched for its
     own vectors); chained fragments (C/F); prologs that read memory through body registers are not run; for
     epilog vectors the harness reloads the move-saved registers at the jump to the epilog start and sets pushed
     registers to junk, so pops must be emulated by the unwinder (needed for DLLs whose bodies interleave
     restores with code that cannot run).
3. `seh_dispatch_test` (x86_64, Rosetta): **42 checks PASS**. Real dispatch path end to end, packaged
   `vcruntime140.dll` and `vcruntime140_1.dll` mapped unchanged (DllMain run against host stand-ins for their
   CRT/kernel32 imports; Rtl*/RaiseException/filters bound to seh's native exports). Guest code: `tests/fixture/
   guest_eh.cpp` compiled at build time by the same clang for `x86_64-pc-windows-msvc` (COFF, MSVC-ABI FH3 tables and
   SEH scope tables) and linked in memory by `tests/coff_link.h`; plus the exe mapped at 0x140000000.
   - `__CxxFrameHandler3`: catch(int) two frames up with destructors in order; derived caught by base&; catch by
     value (copy ctor + both dtors); catch(...); `throw;` caught outside; throw/catch inside a catch block;
     non-matching inner try; 2000 throw/catch iterations (**2.0-2.2 µs per throw/catch** over three runs).
   - `__C_specific_handler`: RaiseException into an `__except` filter function; filter returning
     CONTINUE_EXECUTION; `__finally` run during the unwind; hardware fault (read of 0x10, SIGSEGV → isa → seh) into
     `__except`; fault unwinding through an /EHs C++ frame (FuncInfo EHFlags 0x1: vcruntime does not run the
     destructor for a non-C++ code, as on Windows); vectored handler that emulates the faulting load and resumes.
   - **`__CxxFrameHandler4` with the title's own code**: the exe's catch-by-type translator at rva 0x55f3b70 (FH4,
     15 handlers), called from inside a fixture catch(...) block, rethrows (`throw;` through the exe's
     `_CxxThrowException` import) and catches: `std::bad_alloc` → 0x8007000E, `long` → the HRESULT itself, others →
     0x80004005 (E_FAIL). This exercises FH4 metadata generated by MSVC, a nested dispatch from a catch funclet
     through the consolidation frame, and FH3/FH4 sharing vcruntime's per-thread state.
   - Abort paths (forked children): LOG_AND_ABORT via the API and via `GTA6RT_SEH_POLICY=log_and_abort` (report has
     the C++ type, catchable types, the throw site inside `fx_thrower` and the callers); LOG_AND_ABORT hardware fault
     ("read at address 0x10"); DISPATCH uncaught; a throw through a clang `noexcept` function (clang removed the
     caller's try, so no handler exists in phase 1, as on Windows → unhandled report); a destructor throwing during
     phase 2 (nested dispatch during an unwind; vcruntime's `__FrameUnwindToState` filter calls `terminate()`); a host
     SIGSEGV handler using `gta6rt_seh_handle_signal` instead of isa (fault into `__except`, and VEH resume); the
     vcruntime stand-ins (fixture linked against `gta6rt_seh_vcruntime_fallback`).

## What was not exercised

- `__CxxFrameHandler4` noexcept/terminate in phase 1 and FH4 catch funclets of the exe other than the translator.
- `RtlInstallFunctionTableCallback` callbacks, `STATUS_LONGJUMP` (`longjmp` with a frame), collided unwinds proper
  (an unwind started from inside an unwind handler; the nested-exception-during-unwind case above goes through
  phase 1 only), exceptions on threads other than the main thread, guest code on a non-pthread stack, stack overflow.
- Anything on arm64 beyond the data-level unit test (frame dispatch is not supported there; see stubs).
- The real host boot path (loader + shims binding the exe's imports to these exports) — that is integration work.

## Open issues / notes for the integrator

1. **Bind, do not thunk**: on x86_64, bind the guest imports in `gta6rt_seh_native_export` (exe: RtlCaptureContext,
   RtlLookupFunctionEntry, RtlVirtualUnwind, SetUnhandledExceptionFilter, UnhandledExceptionFilter; vcruntime140(_1):
   RtlUnwindEx, RtlUnwind, RtlPcToFileHeader, RaiseException and the rest) directly to those addresses (NATIVE kind,
   no frame-adding thunk): the capture/raise/unwind entries read their caller's frame.
2. **Register every image with its size** (`gta6rt_seh_add_function_table(base, pdata, n, SizeOfImage)`), and also
   with `gta6rt_isa_register_guest_code` so hardware faults in it reach seh. `RtlPcToFileHeader` (used by
   `_CxxThrowException` for the ThrowInfo image base) and the leaf rule depend on the size; the loader already does
   the seh part.
3. **YMM upper halves on the isa fault path**: `gta6rt_guest_fault_handler` receives a CONTEXT only, so when a
   vectored handler resumes a fault that came through isa, ymm0-15 upper halves are whatever the host code in the
   dispatch path left (legacy SSE host code preserves them; libSystem AVX routines can clear them). The
   `gta6rt_seh_handle_signal` path keeps them. Fix needs an isa change (pass the ucontext to the fault callback, or
   let it call a seh-provided raw-signal hook); not done here.
4. **Host frames between guest frames**: the walk stops at the first host (non-registered) frame. A guest exception
   thrown in a guest callback that a host shim called, and caught by a guest frame above the shim, is reported as
   unhandled. Windows would unwind through the (PE) API frame. If this shows up, give host→guest call trampolines a
   machine-frame RUNTIME_FUNCTION like `seh_tramp_*` (the mechanism exists in `seh_x86_64.S`).
5. `type_info`'s vtable (`??_7type_info@@6B@`) is not a vcruntime140 export (MSVC links it statically); only the
   test fixture needed a stand-in.
6. The test build compiles `tests/fixture/guest_eh.cpp` with `${CMAKE_CXX_COMPILER} -target x86_64-pc-windows-msvc`
   (Apple clang 21 does this). If an integrator toolchain cannot emit COFF, drop `seh_dispatch_test` there.
