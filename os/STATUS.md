# os/ (gta6rt_os) status: S1.1 Win32 primitive layer

State 2026-11-26: the library implements `include/gta6rt/os.h` (API v4; v4 is round 3 below) and the kernel32 surface that compat/ marks
"Implemented by os/". It builds on x86_64 and arm64 with 0 warnings. 5 unit-test programs (986 checks) pass on both
arches. The sections below separate what was run from what was not. The S1 follow-up (lock-free waits and handle
lookups, memory report) is the next section; its test counts supersede the table further down.

## S1 follow-up (2026-11-26): locks out of the hot paths, memory API

Re-run after these changes (`os/tests/run_tests.sh`, both trees): vm 170, sync 166, threads 230, files 247, misc 209
checks, all PASS on arm64 and x86_64. A ThreadSanitizer tree (`build/os-tsan-arm64`, `-fsanitize=thread`) runs sync,
threads, files and misc clean except one report that is in the test itself (`test_wait_on_address` writes the
watched word with plain stores by design); it was there before these changes.

New tests: `test_lock_free_paths` (4 consumers on a semaphore fed 80,000 releases, a 4-thread mutex counter with an
overlap detector, 20,000 auto-event ping-pong rounds, 5,000 wait-all rounds racing a lock-free taker of the same auto
event: the main thread took it first in about 4,900 of them and every round still completed), `test_handle_churn`
(30,000 create/close cycles while 3 threads wait on and set whatever handle value was last published), and
`test_reserve_range_and_report`.

### What changed

- **Waits (`waits.cpp`).** Object state (event flag, semaphore count, mutex owner) is atomic. An uncontended wait or
  signal takes no lock: wait-any (and single-object wait-all) first tries a CAS on each object; Set/Reset/Release only
  look at the object's waiter count. Each object has its own waiter-list lock (`os_unfair_lock`); `g_wait_mx` is kept
  for names, timers, the mutex list, APC state and multi-object wait-all checks, and is always taken before a list
  lock. A blocked waiter sleeps on its own per-thread wake word (`os_sync_wait_on_address`, `threads.cpp`), so nothing
  sleeps holding a lock and nobody re-acquires a global lock on wake-up. Ordering: the waiter links (waiter count
  +1, seq_cst) before its last re-check; a signaller changes state, then reads the count (seq_cst) and bumps the
  linked words under the list lock. A wait-all that loses an object to a lock-free taker gives back what it took and
  waits again. Timer thread and file completion still signal under `g_wait_mx` (rare).
- **Handle table (`handles.cpp`).** Lookups no longer take the table mutex (every WaitForSingleObject took it twice).
  Slots live in fixed 4,096-entry chunks that never move; entry objects are never freed (recycled), so a reader
  takes a reference only if the count is nonzero and re-checks that the slot still holds the entry. Insert, close and
  recycling take the mutex. Waits and Set/Reset/ReleaseSemaphore/ReleaseMutex pin the handle entry instead of
  retaining the waitable (one atomic pair instead of two), and a wait resolves a handle in one lookup.
- **Overlapped I/O (`files.cpp`).** Unchanged pool of min(4, ncpu) workers; it now logs `overlapped I/O: first
  request` and `N requests queued for 4 workers` when the queue doubles past the worker count. The title never logged
  the first line in any 40-60 s run, so the pool is not on its path and was not tuned.
- **API 3 (`include/gta6rt/os.h`, additive, first half of this task):** `gta6rt_vm_reserve_range(lowest, highest_end,
  size, alignment, flags, &base)` (xmem uses it for the title's below-4-GiB and address-width flags) and
  `gta6rt_os_memory_stats` / `gta6rt_os_memory_report(why, min_bytes, max_regions)`: resident, footprint, compressed,
  anonymous and virtual from `task_vm_info`, then per reservation (largest resident first) size, committed,
  resident and compressed. `GTA6RT_MEMORY_REPORT_S=N` logs it every N s from a background thread. The kernel walk
  now runs after the region table is copied, outside the vm lock (it took 49-87 ms per report in the title).
  - In the title at ~30 s (`run/logs/f4-memreport.log`, Series S): resident 395-909 MiB, footprint 4,659-4,671 MiB,
    compressed 981-1,489 MiB (other workers kept the machine under memory pressure), 375 regions, 49.4 GiB
    reserved, 14,484 MiB committed. The title's 185 MiB heap reservation sits below 4 GiB with 184 MiB resident
    (`i5-vshift-check-mem.log`).
  - On a host with the default 4 GiB `__PAGEZERO`, `gta6rt_vm_reserve_range(0x10000, 4 GiB, ...)` returns
    `GTA6RT_E_NO_MEMORY` (tested); with `-pagezero_size 0x10000` it succeeds (title runs).

### Measured (x86_64 under Rosetta)

`shims_core_import_bench` (in `build/shims_core-real-x86_64`), before = the sources at the start of this pass,
after = final; median of 3-5 interleaved runs, ns per call (per pair where two calls), load average 10-24:

| case | before | after |
|---|---:|---:|
| WaitForSingleObject on a set event, timeout 0, through the thunk | 46.6 | 46.2 |
| SetEvent on a set event | 22.9 | 10.7 |
| WaitForSingleObject(mutex) + ReleaseMutex | 66.1 | 60.2 |
| ReleaseSemaphore + WaitForSingleObject | 66.3 | 54.2 |
| WaitForSingleObject(own set event, 0), 1 / 2 / 4 / 8 threads in parallel | 45.9 / 178 / 501 / 1,577 | 44.8 / 46.1 / 61.9 / 78.0 |
| SetEvent → WaitForSingleObject(INFINITE) ping-pong, 2 threads, round trip | 7,117 | 7,680 (min of 4: 12,097 vs 8,737 in a busier run; scheduler-bound) |
| ReleaseSemaphore + WaitForSingleObject(0) on ONE shared semaphore, 2 / 4 / 8 threads | 230 / 802 / 2,541 | 373 / 1,326 / 4,013 |

The last row is worse: all threads CAS the same count and entry lines, where the old mutex serialised them. The title
pattern (many threads, mostly distinct objects, blocking waits) is the one that improved. QPC, critical sections,
SRW, TlsGetValue and HeapAlloc were already within 0-4 ns of the bare host function and are unchanged (table in
`shims_core/STATUS.md`); none takes a lock.

Title (`sample` of the process for 10 s at t=30 s, `run/logs/i2-*-s*.cpu.sample`; counts of samples inside
`_pthread_mutex_firstfit_lock_slow`, i.e. threads blocked on a pthread mutex):

| binary | blocked samples | thread-equivalents blocked | where |
|---|---:|---:|---|
| before (global wait lock) | 1,970 of 151 per thread | 13.05 | wait_core entry/cond-wait re-lock 1,614, ReleaseSemaphore 109, SetEvent 44 |
| global lock kept, lock-free fast paths only (`after3`) | 2,568 of 216 | 11.89 | wait_core 1,746, SetEvent 154 |
| per-thread wake words, global list lock (`after7`) | 3,383 of 201 | 16.83 | wait_core link/unlink 3,097, wake 223 |
| final: per-object list locks | 50 of 131 | 0.38 | none in os: 43 with no gta6rt frame on the stack, 7 in the D3D12 layer's `FenceWaitDispatcher` |

Process CPU and frame rate did not change measurably: 1.26-1.29 cores before and 1.31 after in two interleaved
60 s pairs (`i2-before2-{c,d}`, `i2-after8-{c,d}`), 17-26 frames/s in both (GPU-bound and varying with other
workers' load). The busy core is the D3D12 render thread (`ShadowPages::refresh` → `mach_vm_read_overwrite`).

## Build and test commands

```sh
cd port
cmake -S . -B build/os-arm64  -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA6RT_MODULES=os
cmake -S . -B build/os-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA6RT_MODULES=os
ninja -C build/os-arm64 -j3 && ninja -C build/os-x86_64 -j3
os/tests/run_tests.sh build/os-arm64 120     # serial, each test under a 120 s kill timer
os/tests/run_tests.sh build/os-x86_64 120    # same, under Rosetta, one process at a time
```

Logs from the last fresh-tree run: `build/os-{configure,build,tests}-{arm64,x86_64}.log`, per-test output in
`build/os-<arch>/os/test-logs/`.

## Test results (measured)

Both trees were deleted, configured from scratch and rebuilt: configure rc=0, build rc=0, 38/38 steps, 0 warnings
on each arch. `lipo -archs` reports arm64 for the arm64 library and tests, and x86_64 for the x86_64 ones.

| test | checks | arm64 | x86_64 (Rosetta) | covers |
|---|---|---|---|---|
| os_test_vm | 160 | PASS | PASS | 2×24 GiB reserve-only (RSS grows < 64 MiB), commit inside reservation keeps data, decommit zeroes, fixed address, 4 KiB pages inside a host page, PAGE_GUARD one-shot, write watch + reset, placeholders (split/coalesce/replace, MapViewOfFile3), alias (`mach_vm_remap`), pagefile sections, VirtualQuery outside the table, heap |
| os_test_sync | 140 | PASS | PASS | events (auto/manual, named), semaphores, mutexes (recursive, owner check), WaitForMultipleObjects any/all, SRW (exclusive/shared contention), critical sections, condition variables (CS and SRW), InitOnce, WaitOnAddress (1/4 byte, timeout), SList (multi-thread), waitable timers + timer APCs |
| os_test_threads | 230 | PASS | PASS | CreateThread/exit codes/64 threads, ExitThread, OpenThread, DuplicateHandle(pseudo), CREATE_SUSPENDED, Suspend/ResumeThread counts on a running thread (counter verified frozen), priority/affinity/ideal processor/description, TLS (1000+ slots), FLS callbacks at exit and FlsFree, thread hooks (guest=1 only), `gta6rt_thread_*` direct (suspended create, join timeout, stack bounds, user slots), callback invoker, QueueUserAPC + alertable SleepEx, abandoned mutex, toolhelp snapshot, thread pool work/timer/wait/simple + SetEventWhenCallbackReturns |
| os_test_files | 247 | PASS | PASS | path translation (root, prefixes, longest match, `\\?\`, Z:, `..`, trailing dots, inverse mapping), CreateFile dispositions and error codes, sync I/O + file pointer + EOF + append, 64 concurrent overlapped 64 KiB reads verified against a pattern, overlapped EOF, IOCP (key, Ex, Post, timeout, low-bit event skip), ReadFileEx completion routine, overlapped write past EOF, Find* (wildcards, sorted, dirs-only, on-disk case), attributes, move/copy/delete/rmdir errors, DELETE_ON_CLOSE, full path, cwd, file mappings (shared write-through, copy-on-write, read-only refusal, named pagefile section), `gta6rt_file_*`, map_readonly, std handles |
| os_test_misc | 209 | PASS | PASS | QPC (10 MHz, monotonic over 100k reads, vs Sleep), tick counts, FILETIME vs `time()`, SYSTEMTIME round trips (2000-01-01 Sat, 2024-02-29, invalid 2023-02-29, 1601/1970 epochs), local time, RNG (64 KiB histogram), environment (case-insensitive, size query, expand, block), command line, handle table (multiples of 4, < 2^32, dup keeps object alive, PROTECT_FROM_CLOSE, 20,000 handles), per-thread last error, UTF-8/1252 conversions + surrogates + invalid input, ordinal compares, system info, GlobalMemoryStatusEx vs `hw.memsize`, logical processor info, module table (case/extension-insensitive names, by address, ordinals, load hook, GetModuleFileName, set_main, unregister), psapi, pipes/WER stubs, logging |

Repeat runs to look for timing flakiness: 5 more full arm64 runs and 3 more full x86_64 runs, all serial, gave 0
failures.

Consumer check (arm64 only): a tree with `GTA6RT_MODULES=os;isa;loader;shims_core;shims_gdk` (`build/os-full-arm64`)
links `isa_unit_test` and 18 shims_gdk tests against this library with no duplicate or undefined symbols. All of
them pass, including the 19 `wait_registration_test` scenarios. shims_core did not compile in that tree
(`file_system.cpp`, its owner was mid-edit). The loader tests did not link because seh was left out of the subset.
The full tree could not be configured: `seh/CMakeLists.txt` adds a missing `seh/tests` directory. The `gta6rt`
host executable was therefore not linked against the real os.

Host page size (measured): x86_64 under Rosetta reports 4096, arm64 reports 16384.

## Design notes

- **VM** (`vm.cpp`, `win_memory.cpp`): own region table (base → per-4 KiB-page runs of state/protection). Release
  needs only the base. A reserve is `mach_vm_map` PROT_NONE with no backing; commit is `mprotect` only, so it
  never wipes committed data (S0.3 bug #8); decommit remaps fresh pages. Reservations without an address start
  at `GTA6RT_VM_BASE_HINT` (default 0x300000000) to keep 0x140000000 free. On a 16 KiB host page the effective
  host protection is the union of the 4 KiB guest pages it holds. Under Rosetta (4 KiB pages) protection is exact.
  On arm64, EXEC is never mapped executable, because guest code goes through the JIT. PAGE_GUARD and write watch
  are enforced by PROT_NONE / read-only host pages and `gta6rt_vm_handle_fault`.
- **Waits** (`waits.cpp`): atomic object state, per-object waiter lists, per-thread wake words (see the S1
  follow-up section; until then: one global wait mutex and a per-thread condvar). Each waiter links onto every
  object it waits on. Wait-all is atomic. Mutexes are abandoned at thread exit. Waitable timers run on one timer thread.
- **In-memory sync** (`inmem_sync.cpp`): SRW, CRITICAL_SECTION, CONDITION_VARIABLE, INIT_ONCE and
  WaitOnAddress all keep their state inside the guest's own struct, with a hashed parking lot (1024 buckets).
  Nothing is allocated per lock. SList uses 128-bit CAS (`-mcx16` on x86_64).
- **Threads** (`threads.cpp`): pthreads, Windows-style ids (0x100, step 4). Suspend on a running thread uses
  Mach `thread_suspend`; a thread created suspended waits on a gate. TLS (1088 slots) and FLS (128) live on the
  host side, not in TEB TlsSlots. The exe only uses `gs:[0x58]` (static TLS, owned by isa/loader).
- **Files** (`files.cpp`, `paths.cpp`): fds, plus an overlapped pread/pwrite pool of min(4, ncpu) workers.
  Completion goes to the OVERLAPPED's event, the file's own event, an IOCP or an APC. Path translation uses the
  table in os.h v2. On the data volume (case-insensitive APFS) the caller's spelling resolves as given; the
  directory scan only runs where a component does not exist.
- **Callbacks**: every guest function pointer is called through `gta6rt_os_invoke`. These are thread starts,
  thread-pool callbacks, APCs, InitOnce, FLS callbacks and completion routines.

## Stubs and partial implementations (every one)

Fail cleanly (return FALSE/NULL and set an error, logged at warn/debug):
`TerminateThread`, `GetThreadContext`, `SetThreadContext`, `CreateProcessW`, `ReadFileScatter`,
`DeviceIoControl`, `SetNamedPipeHandleState`, `WaitNamedPipeW`, `OpenThreadToken` (ERROR_NO_TOKEN),
`FindResourceW` (not found; `LoadResource` returns NULL, `SizeofResource` returns 0, `LockResource` returns its
argument), `VirtualAlloc2` with alignment above 2 MiB, `VirtualAllocEx`/`VirtualQueryEx`/`Read`/`WriteProcessMemory`
on another process, `CreateToolhelp32Snapshot` for another process.

Accept and do nothing (or only record the value):
`LockFile`, `UnlockFile`, `HeapDestroy` (heaps are the host malloc; `HeapCreate` returns distinct fake handles),
`VirtualLock`/`VirtualUnlock` (best-effort mlock), `MEM_RESET_UNDO`, `PrefetchVirtualMemory` (madvise),
`DisableThreadLibraryCalls`, `SetDefaultDllDirectories`, `FreeLibrary` (refcount only, never unloads),
`FreeLibraryWhenCallbackReturns`, `CreateThreadpool`/`CloseThreadpool`/`SetThreadpoolThreadMinimum`/`Maximum`
(one global pool; `PTP_CALLBACK_ENVIRON` ignored), `CallbackMayRunLong` (TRUE), `SetThreadPriority`,
`SetThreadAffinityMask`/`SetThreadGroupAffinity`/`SetThreadIdealProcessor(Ex)` (recorded, not applied),
`SetProcessPriorityBoost`, `PowerCreateRequest`/`PowerSetRequest`/`PowerClearRequest`, `SetThreadExecutionState`,
`OpenProcessToken` (dummy handle), `LookupPrivilegeValueW`/`AdjustTokenPrivileges`/`PrivilegeCheck` (always
granted), `WerRegisterFile`/`WerUnregisterFile`, `SetErrorMode`, `EncodePointer`/`DecodePointer` (identity),
`MoveFileExW(MOVEFILE_DELAY_UNTIL_REBOOT)`, `SetFileInformationByHandle` for FileAllocationInfo/FileBasicInfo/
FileIoPriorityHintInfo, the creation-time argument of `SetFileTime`, `dwShareMode` and security attributes in
`CreateFile*`, `NumberOfConcurrentThreads` in `CreateIoCompletionPort`, the alertable flag of
`GetQueuedCompletionStatusEx`, `INIT_ONCE_ASYNC` (handled as synchronous).

Fixed or synthetic values:
`GetVersionExW` (10.0.19041), `GetSystemDirectoryW`/`GetWindowsDirectoryW` (C:\Windows...), `GetVolumeInformationW`
("NTFS", fixed serial), `IsProcessorFeaturePresent` (fixed table incl. AVX/AVX2), `GetSystemInfo` processor
level/revision, `GetLogicalProcessorInformation` (one core per logical CPU, sysctl cache sizes),
`GetCurrentProcessorNumber` (derived from the thread id, not the real CPU), `GetExitCodeProcess` (always
STILL_ACTIVE), the locale functions (en-US only: `GetUserDefaultLocaleName`, `GetLocaleInfoEx` with 12 LCTYPEs,
`LocaleNameToLCID` for 7 names, `GetDate/TimeFormatEx` with an English picture formatter), `GetACP`/`GetOEMCP`
(65001), code pages limited to UTF-8/ACP/OEM/20127/1252/28591, `CompareStringEx` (ordinal, no linguistic rules),
`LCMapStringEx` (upper/lower only, Latin-1/Greek/Cyrillic), `GetStringTypeW` (CT_CTYPE1 only), `FormatMessageW/A`
(11 system messages, inserts not expanded), `GetLongPathNameW` (identity), `GetStartupInfoW` (zeroed,
SW_SHOWDEFAULT).

Semantics that differ from Windows:
- `ExitProcess` calls `exit()`, so atexit handlers and static destructors run while other threads are still
  running. `TerminateProcess(self)` calls `_exit`.
- `SuspendThread` on a running thread is Mach `thread_suspend`. If the target holds a host lock (malloc, the
  wait mutex, the log mutex), the suspender can deadlock. Windows has the same hazard with its own locks.
- `CancelIoEx` cancels only requests still queued; a request already in pread/pwrite completes normally.
- SRW locks wake all waiters on release and are not fair. The CRITICAL_SECTION `LockCount` field is only -1/-2
  and `DebugInfo` is -1.
- On arm64 (16 KiB pages), a PAGE_NOACCESS or guard 4 KiB page that shares a host page with an accessible one is
  accessible. Exact under Rosetta.
- `gta6rt_vm_alias` needs host-page alignment (16 KiB on arm64).

## Surface coverage against the S1 inventory (measured with nm)

From `spike/s1-win32-surface/imports_by_dll.json`, the exe imports in the S1.1 kernel32-type families (win32-kernel,
wait-objects, guest-inplace-sync, threads, handle-table, virtual-query, time, process-*, file-simple, string/locale,
affinity-priority, debug-diag, last-error) all have a definition in `libgta6rt_os.a`. Of the 235 imports the
inventory tags S1.1, 75 are defined (50 trivial, 25 design) and the 160 missing ones are all CRT. That covers the 25 "design"
items of S1.1 and every kernel32 "trivial" item, including the 4 that were missing before this pass
(SetNamedPipeHandleState, WaitNamedPipeW, SetProcessPriorityBoost, WerRegisterFile). 24 imports that leonida served
from its shims are also defined here.

Not in os, and where they belong:
- **160 CRT imports that the inventory tags S1.1** (142 ucrt, 9 ucrt-stdio `__stdio_common_*`, 9 vcruntime
  mem/str functions) are not implemented. They are guest-ABI C-runtime entry points, not Win32 primitives, and
  were not in this task's list. **The lead must decide who owns them** (shims_core binding to host libc through
  `ms_abi` wrappers looks natural).
- Window/message imports belong to S1.6. `Register/UnregisterAppConstrainedChangeNotification` belong to S1.5
  (psm-appnotify) and `freeaddrinfo` to S1.5 (winsock).
- Declared in compat but implemented elsewhere: seh/ (`RaiseException`, vectored handlers,
  `Set/UnhandledExceptionFilter`, `Rtl*`), shims_core (`Co*`, `StringFromGUID2`), shims (bcrypt).

## Changes outside os/ (all additive)

- `include/gta6rt/os.h` (round 3): API version 4, `gta6rt_vm_lowest_mappable()`.
- `compat/include/processthreadsapi.h`: `SetProcessPriorityBoost`, `GetProcessPriorityBoost`.
- New `compat/include/namedpipeapi.h` (`SetNamedPipeHandleState`, `WaitNamedPipeW`, NMPWAIT_*) and
  `compat/include/werapi.h` (`WerRegisterFile`, `WerUnregisterFile`, WER_REGISTER_FILE_TYPE). Neither name exists
  in the macOS SDK. `windows.h` does not include them (nor does the Windows SDK's windows.h include werapi.h).
  Consumers include them directly.
- `compat/tests/abi_test.cpp`: layout asserts for PROCESS_MEMORY_COUNTERS_EX, MODULEINFO, OVERLAPPED_ENTRY,
  FILE_STANDARD_INFO, WIN32_MEMORY_RANGE_ENTRY, CREATEFILE2_EXTENDED_PARAMETERS, GROUP_AFFINITY, PROCESSOR_NUMBER
  and OSVERSIONINFOEXW. They pass on both arches.
- An earlier pass of this task (00:27-00:56) added os.h API v2 (placeholders, MEM_RESET, the fault hook, the
  invoker, path translation, command line, load hook, set_main) and the "Added by os/ (S1.1)" blocks in
  `compat/include/{fileapi,sysinfoapi,memoryapi,ioapiset,threadpoolapiset,processthreadsapi,winbase}.h`.

## Fixed in this pass

- `GetQueuedCompletionStatusEx` read `lpOverlapped->Internal`, which segfaults on a
  `PostQueuedCompletionStatus` packet that carries an arbitrary pointer. It also stopped early on a failed packet
  with a NULL overlapped. It now dequeues packets directly (`port_take`) and reports the packet's own status.
- A non-wildcard `FindFirstFile` returned the caller's spelling of the name. It now returns the on-disk name.
- `HeapReAlloc(HEAP_ZERO_MEMORY)` could leave stale bytes between the old requested size and the allocator's
  usable size. HeapAlloc now zeroes that slack.
- test_sync used `__attribute__((ms_abi))` lambdas, which broke the arm64 build. It now uses `GTA6RT_GUEST_ABI`.
- Added: psapi (`GetProcessMemoryInfo`, `K32GetProcessMemoryInfo`, `GetModuleInformation`, `EnumProcessModules`)
  and toolhelp (`CreateToolhelp32Snapshot`, `Thread32First/Next`). compat already declared them as implemented by
  os, but they were missing.

## Open issues / not done

- Not run: the `gta6rt` host with the real os, and anything with real guest code. os has only been exercised by
  its own tests and by isa/shims_gdk tests.
- Wait/lock contention under the title was measured and removed (S1 follow-up). Not measured: overlapped-I/O
  throughput at DStorage scale (4 workers); the title did not use the pool in any run.
- TLS slots are host-side, not mirrored into TEB `TlsSlots`/`TlsExpansionSlots`. That is fine for the exe
  (`gs:[0x58]` only); it needs revisiting if a package DLL reads `gs:[0x1480]` directly.
- The CRT ownership question above.

## Review response (round 3)

To `isa/rosetta/REVIEW-round3-perf-policy.md`. The xmem side and the title runs are in `shims_core/STATUS.md`
("Review response (round 3)").

- **Finding 1 (blocking): page zero.** `CMakeLists.txt`: `gta6rt_os` adds `LINKER:-pagezero_size,0x10000` to every
  x86_64 executable that links it (INTERFACE option, `$<TARGET_PROPERTY:TYPE>` = EXECUTABLE; `GTA6RT_OS_LOW_PAGEZERO`
  ON by default). The host gets it without an edit to `host/CMakeLists.txt`: my `build/os-host-x86_64/gta6rt`,
  rebuilt with empty `CMAKE_EXE_LINKER_FLAGS`, has `__PAGEZERO` vmsize 0x10000 and the title's heap lands at
  0x2fd80000 (`run/logs/r3fix-iface.log`).
- **Finding 2 (blocking): arm64.** New `gta6rt_vm_lowest_mappable()` (os.h API 4, additive): end of the main
  executable's `__PAGEZERO` from its load commands (no deprecated `getsegbyname`). xmem uses it to tell "no address
  space below 4 GiB" (arm64 always) from "that range is full". Re-measured that it cannot be worked around at run
  time on either arch: deallocating the page zero succeeds but a fixed map at 0x10000000 still fails.
- **Finding 6 (weak evidence).** `test_reserve_range_and_report` now asserts: lowest mappable 0x10000 on x86_64 and
  0x100000000 on arm64, and the below-4-GiB reservation succeeds exactly when the lowest mappable address is below
  4 GiB (and then lies inside [lowest, 4 GiB)). x86_64: `ok (0xb60000)`; arm64: `no room`.
- **Finding 4 (arm64 timed waits): fixed.** `sleep_on_word` converted nanoseconds to mach ticks, but
  `os_sync_wait_on_address_with_timeout` takes nanoseconds for `OS_CLOCK_MACH_ABSOLUTE_TIME`, so every finite wait on
  arm64 slept 1/41.7 of its time and looped. It now passes nanoseconds. Measured (`scratchpad/r3/waitcpu.cpp`,
  `gta6rt_wait_any` on an unsignalled event, process CPU, two runs each): arm64 before 0.48-0.78 / 0.77-0.88 /
  1.44-1.79 ms CPU for 10 / 50 / 500 ms; after 0.04-0.06 / 0.013-0.016 / 0.009-0.015 ms, the same as x86_64
  (0.08-0.11 / 0.013-0.025 / 0.013-0.016 ms, unchanged since its timebase is 1/1). Cost: a timed wait now ends up to
  about 1 ms late (10 ms waits took 11.0-11.1 ms on arm64, as they already did on x86_64; before the fix arm64
  ended within 0.2 ms because it re-checked the deadline on every short wake).
- Not addressed: finding 7 (shared-semaphore contention regression, not on the title's path) and finding 8 (no
  `__builtin_available(macOS 14.4)` guard; only matters if X8 pins the target below 14.4).
- Tests after these changes: vm 172 (arm64) / 173 (x86_64), sync 166, threads 230, files 247, misc 209, all PASS on
  both arches.
