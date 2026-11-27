# Review of os/ (gta6rt_os, S1.1)

Reviewer pass of 2026-11-26. Verdict: APPROVED WITH FIXES. No blocking defect: nothing found that is shown to crash
or corrupt the guest on the boot path, and no claim in the builder's report or STATUS.md is false. One finding
(SuspendThread, H1) is understated by STATUS.md and should be fixed before the title is run for real. Several
guest-visible behaviours that STATUS.md does not list are in section "Not in STATUS.md" below.

## What I ran

All builds were private trees (`port/build/review-os-arm64`, `review-os-x86_64`, `review-os-full-arm64`), `-j3`,
x86_64 programs one at a time under a kill timer. Probe sources and logs are under
`/private/tmp/claude-501/-Users-a-Desktop-Projects-GTA-VI/eeb510fb-badf-42ab-9e97-2a01126ed0b0/scratchpad/p/` (p1..p4).

| check | result |
|---|---|
| fresh configure + build, `GTA6RT_MODULES=os`, arm64 and x86_64 | rc=0, 38/38 steps, 0 warnings each; `lipo` confirms arch |
| `os/tests/run_tests.sh` on arm64 | 5 programs PASS, 160/140/230/247/209 checks, matches the report |
| same on x86_64 under Rosetta (serial, timers) | 5 programs PASS, same counts |
| repeats | arm64 3 more full runs, x86_64 1 more: 0 failures |
| `compat_abi_test` | PASS on both arches |
| consumer tree `os;isa;shims_gdk` arm64 | builds 108/108; `isa_unit_test` 343 checks PASS; 17 plain `shims_gdk` tests PASS; `wait_registration_test` 19/19 scenarios PASS (it needs the scenario name as argv[1], bare `rc=1` is its usage exit) |
| every compat header function declared `WINAPI` (355) against `nm` of the library | all defined except those owned by seh (7), shims bcrypt (3), shims_core Co*/StringFromGUID2 (8): the "declared but missing" gap the report says it closed is closed |
| inventory: 235 imports tagged S1.1 vs `nm` | 75 defined, 160 missing, all 160 are ucrt/ucrt-stdio/vcruntime: report and STATUS numbers are correct |
| own probes (CS/SRW/condvar stress, 8 threads, 200k iterations each; both arches) | CS counter 914288/914288, SRW 800000/800000, producer/consumer 50000/50000, no lost wakeups |

Not run by me: anything with real guest code, the `gta6rt` executable (does not link yet, as the report says), x86_64
consumer tree, the isa test on x86_64.

## Findings

### H1. SuspendThread on a running thread hangs the process within tens of calls (STATUS understates it)

STATUS says suspending a running thread "can deadlock if the target holds a host lock ... Windows has the same hazard
with its own locks". Measured (`p3.cpp`): four threads looping `SetEvent / WaitForSingleObject(0) / ResetEvent`, main
thread looping `SuspendThread(t0); ResumeThread(t0)`. It hung 3 of 3 runs on arm64, after 15 to 1570 pairs
(process stuck in `__psynch_mutexwait`). That is not "can", it is "will, quickly", because `SuspendThread` is Mach
`thread_suspend` on an arbitrary instruction and the victim is usually inside the handle-table mutex (`g_h_mx`,
taken by every `SetEvent`/`Wait*`/`CloseHandle`) or the global wait mutex (`g_wait_mx`). Windows has no equivalent:
its handle lookup and wait queues are not user-mode mutexes a suspended thread can strand.

I also tried the obvious one-line fix (hold `g_wait_mx` across `thread_suspend` in `SuspendThread`, patched copy in
scratch): still 3 of 3 hangs, so the cure needs every host lock the victim can hold (`g_h_mx`, `g_wait_mx`, `g_vm_mx`,
the 1024 parking-lot bucket mutexes, `g_reg_mx`, `g_tls_mx`, malloc, the log mutex) to be taken by the suspender in a
fixed order, or cooperative suspension (signal the target and have it park at the next os call or in a signal handler
that first drops nothing it holds is no better). The unit test passes only because its target spins on a counter and
takes no host lock.

Boot-path relevance: the exe imports `SuspendThread` (inventory, design item), S0.3 treated it as "ignored" and still
reached WinMain, and I found no evidence it is called on the boot path. Not blocking, but a hang with no diagnostic
if the title suspends a worker. Suggested minimum: log a warning and make `SuspendThread` a counted no-op until
there is a safe implementation, or implement the all-locks-held version and add a stress test like p3.

### M1. ReadFile/WriteFile of 2 GiB or more fail with ERROR_INVALID_PARAMETER

macOS `pread`/`pwrite`/`read`/`write` return EINVAL for a count above INT_MAX (verified, `p2.cpp`). `do_io` and
`run_req` pass `n - done` unclamped, so any single `ReadFile` of at least 0x80000000 bytes returns FALSE with error 87
(verified with a 2.25 GiB buffer on both arches, `p4.cpp`); Windows reads what is there and succeeds. The 258 MiB
effect-database read mentioned in leonida is below the limit, so this is not on a known boot read, but DStorage
style big-buffer reads go through the same loops. Fix: clamp each syscall to 1 GiB. Not in STATUS.

### M2. Host reads into MEM_WRITE_WATCH or PAGE_GUARD memory fail with ERROR_NOACCESS

Write watch and guard are enforced with PROT_NONE / read-only host pages, and the file paths do not pre-fault them.
`ReadFile` into a freshly committed `MEM_WRITE_WATCH` region returns FALSE, error 998, zero bytes, and no page becomes
dirty (verified, `p1.cpp`). On Windows a kernel write into a write-watched region succeeds and sets the watch bits
(leonida's `title_page_watch.h` relies on write-watch reservations being the case where kernel writes are tracked).
The port's `shims_core/xmem.cpp` only passes write-watch through when the title asks for it, so this bites only if the
title ReadFiles/DStorage-reads into its own write-watched allocation. Fix: before the syscall, mark the destination
pages dirty via the VM layer (or call `gta6rt_vm_handle_fault` per page) in `do_io`, `run_req`,
`gta6rt_file_pread`. Not in STATUS. The PAGE_GUARD case (ERROR_NOACCESS) is plausibly the Windows behaviour and fine.

### M3. Transitive S1.1 imports are not covered and STATUS does not say so

The coverage section counts only the exe's own imports. The inventory also tags S1.1 imports of the bundled DLLs the
loader must map: for `libHttpClient.GDK.dll` (statically imported, so mapped at boot) `EventRegister`,
`EventSetInformation`, `EventUnregister`, `EventWriteTransfer` (ADVAPI32 ETW, "trivial, no-op returning
ERROR_SUCCESS") are undefined in `libgta6rt_os.a`. Run-time-reached DLLs (XCurl, vccorlib140, amd upscaler) add
23 more Win32 names, among them `QueueUserWorkItem`, `GetLogicalProcessorInformationEx`, `CreateRemoteThreadEx`,
`InitializeProcThreadAttributeList`/`UpdateProcThreadAttribute`/`DeleteProcThreadAttributeList`, `RaiseFailFastException`,
`GetSystemTimeAdjustment`, `GetStringTypeExW`, `FindResourceExW`, `WriteConsoleW`, `CoTaskMemAlloc` (the last is
shims_core's). Whoever owns S1.4 import binding needs this list; either add the four ETW stubs now or record who does.

### M4. Thread-pool limits not in STATUS

Pool workers are created with a 1 MiB stack (`enqueue_locked`), while Windows pool threads use the image default
(the title's /STACK is 8 MiB per os.h); a deep job-system callback that is fine on Windows can overflow. The wait
service (`wait_main`) services at most 63 armed `SetThreadpoolWait` objects; the rest are silently never signalled
until earlier ones fire. Worker count is capped at max(2*ncpu, 16).

### L. Smaller items

- `HeapSize` returns `malloc_size` (usable size, 112 for a 100-byte request, verified) not the requested size;
  `GlobalSize`/`LocalSize` style callers and `_msize` see the larger value. Not listed.
- Named objects: `named()` takes an existing object after checking `refs > 0` and then increments, while releases
  decrement outside `g_wait_mx`; a release that reaches 0 between the check and the increment resurrects a dying
  object. `thread_by_id` (OpenThread) has the same check-then-increment shape. Narrow windows, named objects and
  OpenThread of a dying thread only.
- `VirtualProtect` over a range spanning two allocations falls into the raw `mprotect` branch and changes host
  protection without updating the region table; Windows fails the call.
- `VirtualQuery` reports `AllocationProtect` as PAGE_NOACCESS unless the first page is committed; Windows reports the
  protection given at reservation.
- `fold()` applies the Win32 trailing dot/space rule to the host prefix directory too, so a mapped host directory
  component ending in "." or " " is altered.
- `IsProcessorFeaturePresent(PF_FASTFAIL_AVAILABLE)` returns TRUE, which makes the CRT use `int 29h`; confirm isa/seh
  handle that trap, otherwise return FALSE.
- `ExitProcess` -> `exit()`: with other threads running (Vulkan loader, thread pool) static destructors can hang or
  crash at exit. `_exit` after flushing stdio is safer for a game.
- `OpenMutexW`/`OpenSemaphoreW`/`OpenWaitableTimerW` do not exist (only `OpenEventW`); not imported by the exe, not
  declared in compat.
- Directory enumeration returns host-only names (`.DS_Store`, `._*`) in wildcard finds.
- `GetCurrentProcessorNumber` is derived from the thread id, so `GetThreadIdealProcessor` style pinning decisions in
  a job system see a fixed fake CPU per thread (listed in STATUS).

## ABI and semantics checked and found correct

- ms_abi: all 355 compat `WINAPI` declarations map to definitions compiled against the same header, so a mismatch
  would be a compile error; no extern "C" Win32 definition in os/src lacks `WINAPI`/`GTA6RT_GUEST_ABI`. Guest
  callbacks (thread start, InitOnce, FLS, APC, thread pool, completion routines) all go through
  `gta6rt_os_invoke` with at most 6 integer args, BOOL results truncated to 32 bits.
- LLP64 and layouts: handles are multiples of 4 below 2^32; `DWORD`/`LONG` are 32-bit; the new layout asserts in
  `compat/tests/abi_test.cpp` pass on both arches; SLIST header encoding matches the x64 Windows format
  (depth 16 / sequence 48 / pointer in the high word) and uses 128-bit CAS (`-mcx16` on x86_64).
- Wide strings are `char16_t` throughout, surrogates handled both ways, invalid UTF-8 becomes U+FFFD, `ERROR_*`
  codes for MultiByteToWideChar/WideCharToMultiByte limits match Windows.
- In-guest sync: SRW, CRITICAL_SECTION (futex word in LockSemaphore, owner = Windows-style tid, recursion), condition
  variables (sequence-number design, no lost wakeup in my stress), InitOnce, WaitOnAddress (1/2/4/8 bytes) are
  race-free in my stress runs and zero-initialised guest structs work without an Initialize call.
- Error semantics the guest depends on: `ERROR_ALREADY_EXISTS` for existing named objects and OPEN_ALWAYS /
  CREATE_ALWAYS, `ERROR_FILE_NOT_FOUND` vs `ERROR_PATH_NOT_FOUND`, `ERROR_HANDLE_EOF` only with OVERLAPPED,
  `ERROR_IO_PENDING` on overlapped handles, `ERROR_INVALID_ADDRESS` (487) for reserve-over-reserve, protect of
  uncommitted pages and release of a non-base address, `ERROR_INVALID_PARAMETER` for 65 wait handles, abandoned
  mutex codes, `TlsGetValue` clearing last error.
- VM: commit never wipes, decommit zeroes, two 24 GiB reserve-only windows work on both arches with RSS flat, the
  anywhere hint (0x300000000) keeps 0x140000000 free, kernel writes to read-only pages fail with ERROR_NOACCESS as
  they do on Windows.

## Stubs and honesty of STATUS.md

The list of "fail cleanly" and "accept and do nothing" functions matches the code; I found no function that silently
returns success that is not listed, apart from the unlisted behaviours above. The accurate-but-incomplete items are
H1 (severity), M1, M2, M3, M4 and `HeapSize`. The "what was not run" sections of the report and STATUS are accurate.

## Decisions for the lead

1. Owner for the 160 CRT imports (ucrt, `__stdio_common_*`, vcruntime mem/str): unchanged from the report; shims_core
   binding host libc through `ms_abi` wrappers is the cheap route.
2. Owner for the transitive Win32 imports in M3 (at least the four ETW stubs, needed before libHttpClient.GDK loads).
3. Whether SuspendThread becomes a logged no-op (what S0.3 did) or gets the all-locks implementation (H1).

## Files

- /Users/a/Desktop/Projects/GTA-VI/port/os/REVIEW.md (this file; the only file written in the project)
- Review build trees: /Users/a/Desktop/Projects/GTA-VI/port/build/review-os-arm64, review-os-x86_64, review-os-full-arm64
- Probes and patched copy: the scratchpad `p/` directory named above
