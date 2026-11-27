# shims_core (S1.5a) status

Library `gta6rt_shims_core` (static, link with `-force_load` from host/). Both arches build with 0 warnings; unit tests pass on arm64 (native) and x86_64 (Rosetta).
Source is about 14.5k lines (`src/` plus `tests/`). Origin of every `src/` file is the leonida `runtime/shims/*` file of the same name (GPL-3.0-or-later); the CRT/kernel32 files (`crt_*.cpp`, `k32_misc.cpp`, `api.cpp`, `registry.cpp`, `frame_adapter.h`, `util.h`) are new.

## S1 follow-up (2026-11-26): call cost, unimplemented-import policy, memory facts

Tests after these changes: `shims_core_test` 91 cases, 1,241 checks (arm64) / 1,239 (x86_64), 0 failed;
`shims_core_real_smoke` 46 checks, 0 failed; `leonida_bcrypt_rng_test` 21 checks, 0 failed on both arches. The
registry holds 844 rows in the host (`shim registry: 844 rows`).

### Cost of the hot imports (guest→host call)

`shims_core_import_bench` (new, `tests/import_bench.cpp`, built with `-DGTA6RT_SHIMS_CORE_TEST_REAL_ENV=ON` in
`build/shims_core-real-x86_64`; real os + isa under Rosetta): each import is called through the thunk the resolver
binds (the path the exe takes) and directly (the host function alone). `shims_core_import_bench 1 waits` runs only
the multi-thread wait cases. Before = sources at the start of this pass, after = final, median of 3 interleaved
runs, load average 11 (`build/shims_core-import-bench-final.txt`):

| row | thunk ns before | thunk ns after | host fn alone before | after | thunk overhead before | after |
|---|---:|---:|---:|---:|---:|---:|
| Sleep(0) | 239.4 | 229.0 | 229.0 | 234.2 | 10.4 | -5.2 |
| QueryPerformanceCounter | 19.4 | 19.5 | 19.5 | 19.5 | -0.1 | 0.0 |
| GetCurrentThreadId | 11.8 | 9.4 | 9.4 | 9.4 | 2.4 | 0.0 |
| GetLastError | 12.1 | 8.4 | 7.7 | 7.6 | 4.4 | 0.8 |
| TlsGetValue | 13.5 | 11.5 | 9.1 | 10.7 | 4.4 | 0.8 |
| Enter+LeaveCriticalSection | 19.3 | 17.2 | 16.5 | 16.5 | 2.8 | 0.7 |
| Acquire+ReleaseSRWLockExclusive | 14.6 | 9.6 | 8.3 | 8.3 | 6.3 | 1.3 |
| HeapAlloc(64)+HeapFree | 50.2 | 50.9 | 46.9 | 49.7 | 3.3 | 1.2 |
| WaitForSingleObject(set event, 0) | 46.6 | 46.2 | 43.2 | 44.4 | 3.4 | 1.8 |
| SetEvent(set event) | 22.9 | 10.7 | 18.5 | 10.1 | 4.4 | 0.6 |
| WaitForSingleObject(mutex)+ReleaseMutex | 66.1 | 60.2 | 58.7 | 56.7 | 7.4 | 3.5 |
| ReleaseSemaphore+WaitForSingleObject | 66.3 | 54.2 | 58.9 | 53.1 | 7.4 | 1.1 |
| strcmp(31 chars) | 14.2 | 12.4 | 10.5 | 11.5 | 3.7 | 0.9 |
| _strnicmp(16) | 17.7 | 16.5 | 14.2 | 15.6 | 3.5 | 0.9 |
| _stricmp(31 chars) | 24.8 | 23.5 | 20.7 | 23.1 | 4.1 | 0.4 |
| strncmp(16) | 15.0 | 13.3 | 12.5 | 12.4 | 2.5 | 0.9 |
| strlen(31 chars) | 10.4 | 8.9 | 8.0 | 8.1 | 2.4 | 0.8 |
| exp2f | 10.1 | 9.5 | 6.8 | 7.5 | 3.3 | 2.0 |
| log10f | 10.0 | 8.1 | 7.5 | 7.8 | 2.5 | 0.3 |
| cosf | 10.8 | 8.4 | 7.7 | 7.8 | 3.1 | 0.6 |
| qsort(16 ints) | 1522.7 | 1619.7 | 1565.4 | 1610.0 | -42.7 | 9.7 |
| __stdio_common_vsprintf("%d %s") | 133.2 | 141.1 | 128.7 | 138.0 | 4.5 | 3.1 |
| strtod | 23.9 | 22.1 | 19.8 | 21.1 | 4.1 | 1.0 |
| ReadFile(4 KiB) | 571.0 | 589.4 | 564.1 | 598.3 | 6.9 | -8.9 |
| XMemMapPhysicalPages(map 1)+(unmap 1) | 3069.0 | 3234.0 | 3063.4 | 3213.2 | 5.6 | 20.8 |
| WaitForSingleObject(own set event, 0), 1 thread(s) | 45.9 | 44.8 | | | | |
| WaitForSingleObject(own set event, 0), 2 thread(s) | 178.2 | 46.1 | | | | |
| WaitForSingleObject(own set event, 0), 4 thread(s) | 501.2 | 61.9 | | | | |
| WaitForSingleObject(own set event, 0), 8 thread(s) | 1577.2 | 78.0 | | | | |
| SetEvent -> WaitForSingleObject(INFINITE) ping-pong, 2 threads | 7117.3 | 7680.1 | | | | |
| ReleaseSemaphore+WaitForSingleObject(0) on one shared semaphore, 2 threads | 229.7 | 373.3 | | | | |
| ReleaseSemaphore+WaitForSingleObject(0) on one shared semaphore, 4 threads | 801.6 | 1326.3 | | | | |
| ReleaseSemaphore+WaitForSingleObject(0) on one shared semaphore, 8 threads | 2540.9 | 4013.2 | | | | |

- The rows cover the host-served imports among the 20 most-called in the frame loop (`f1-mwaitx-on.log`):
  `_strnicmp` 1.60 M, `strcmp` 1.47 M, `strlen` 316 k, `QueryPerformanceCounter` 222 k, `WaitForSingleObject`
  175 k, `Sleep` 151 k, `qsort` 108 k, `exp2f` 82 k, `__stdio_common_vsprintf` 50 k, `ReleaseSemaphore` 30 k,
  `XMemMapPhysicalPages` 29 k, `SetEvent` 26 k, `ReadFile` 24 k, `ReleaseMutex` 24 k, `strncmp` 24 k, `log10f` 22 k
  per 60 s. The other 4 of the top 20 (`memset` 2.77 M, `memcpy` 2.54 M, `memmove`, `memcmp`) are vcruntime140,
  a package DLL that runs as guest code: no host call.
- Calls × cost: about 0.3 s of host time per 60 s for all of them (0.5% of one core); `qsort` (guest comparator
  callbacks) 0.17 s and `XMemMapPhysicalPages` 0.05 s are the largest. So the title's CPU is not in call overhead.
- Thunk overhead fell from 2.4-7.4 ns to 0-3.5 ns per call for the short calls (isa: NATIVE thunks jump straight to
  the host function under SHARE); for rows over 100 ns the difference is inside run-to-run noise. The wait/signal rows improved from os's lock-free paths (os/STATUS.md). QPC (rdtsc would save ~5 ns
  of 19.5; at 3.7 k calls/s not worth it), critical sections, SRW, TlsGetValue and HeapAlloc were left as they are:
  each is within a few ns of the host primitive and takes no lock.
- Trace stubs: with tracing off the host does not install them (`host::trace::wrap` returns the target). With
  `--trace=0` each call goes through the host's counting trampoline; title CPU 1.30 cores with `--trace=0` and
  1.27 without (`f1-mwaitx-on` vs `f2-notrace`, same binary, 60 s): within run-to-run noise.

### Unimplemented-import policy: default is now fail

- `LEONIDA_UNIMPLEMENTED` unset or empty means `fail` (`parse_unimplemented_policy`); `log` and `stop` stay
  selectable, and an unknown value logs a warning and uses `fail`.
- `fail` returns by return class and sets last error 120 (ERROR_CALL_NOT_IMPLEMENTED):
  - HRESULT names → E_NOTIMPL (0x80004001). New in this pass: `mfplat`/`mfreadwrite`/`mf` `MF*` except
    `MFGetSystemTime`, `xaudio2_9.dll` ordinal 1 (XAudio2Create, per leonida's `docs/movies.md` and
    `docs/d3d12_vulkan.md`) and `gameinput.dll` ordinal 1. These are 8 of the title's 12 bound-but-unserved imports
    (`MFStartup`, `MFCreateAttributes`, ... would otherwise have returned S_OK with unwritten out-pointers).
  - New class errno_t: CRT `*_s` names (not the printf/scanf `_s` family, not `strnlen_s`/`wcsnlen_s`) → EINVAL (22).
  - Everything else → 0 (NULL/FALSE).
- Each call is counted; the log line `unimplemented: dll!name called from module+rva (call N; policy ...)` is printed
  on calls 1, 2, 4, 8, ...; `stop` prints the caller and registers and exits with code 3.
- Found by the title with the new default: `api-ms-win-crt-environment-l1-1-0.dll!_putenv_s`, called once by
  `amd_fidelityfx_upscaler_xbox.dll+0xcd23`. As class "other" it got 0, which for errno_t means success. It is now
  implemented (`crt_extra.cpp`, `gta6rt_env_set`; empty value unsets): the title sets `FFX_FSR4_XBOX_ACO=1`
  (`crt: _putenv_s("FFX_FSR4_XBOX_ACO", "1") -> ok`). Title runs since: 0 unimplemented imports called.
- Tests: `registry_return_classes_of_the_titles_unserved_imports`, `registry_putenv_s_sets_and_clears`, and the
  existing fail/log/stop cases.

### Memory facts (first half of this task; verified in the title here)

- **xmem address flags (`xmem.cpp`, `address_range`).** Attribute bit 2 asks for an address below 4 GiB; bits 12-14
  give an address width of 47 - field (no field: 40 bits), as in VIctory's `xmem.rs` (GPL-3.0-or-later).
  `XMemVirtualAlloc` without an address reserves through `gta6rt_vm_reserve_range` inside that range. What happens
  when nothing fits below 4 GiB is `LEONIDA_XMEM_32BIT` (round 3 review response below): `auto` (default) fails with
  an ERROR line and ERROR_NOT_ENOUGH_MEMORY when the process has address space below 4 GiB, and places the
  reservation above 4 GiB with a WARN line and a count when it has none (arm64; an x86_64 executable linked without
  `-pagezero_size 0x10000`); `require` always fails; `anywhere` always places it above.
  - Title, host linked with `-pagezero_size 0x10000` (my tree): the 185 MB request (193,986,560 bytes, attributes 7)
    is reserved at 0x1cb80000-0x1e180000 (varies per run), below 4 GiB, 184 MiB resident in the memory report.
- **Heap pages in address order.** A below-4-GiB reservation holds a run of pool slots in its own address order;
  a fresh physical page mapped into it gets the slot of its address (contents are zero either way). In the title all
  2,960 pages moved (0 not fresh), and the log reports `32-bit reservation ... is one stretch of the pool at host
  0x...`: one contiguous host range a no-copy GPU buffer can wrap.
- **Console profile (`console_registry.cpp`, `gta6rt_shims_set_console_profile`, shims.h API 3).** `series-s`
  (device type 6, title memory 7,776 MiB, xmem pool 8 GiB), `series-x` (7, 13,824 MiB, 14 GiB) or `auto` (Series S
  when `hw.memsize` <= 32 GiB). Order: the API call, then `LEONIDA_CONSOLE_PROFILE`, then auto. On this 24 GiB Mac:
  `console profile series-s (auto: 24 GiB of physical memory)`. Effect seen: the exe makes 36,765 import calls
  before WinMain with Series S against 36,739 with Series X (`f0-winmain*.log`).
- **Memory report:** os's `gta6rt_os_memory_report` (os/STATUS.md).

### For the integrator

1. ~~`host/CMakeLists.txt` (x86_64): add `LINKER:-pagezero_size,0x10000`.~~ No longer needed (round 3 review
   response): `gta6rt_os` now adds the flag to every x86_64 executable that links it, and a host without it no
   longer aborts.
2. `host/src/main.cpp`: a `--console-profile series-s|series-x|auto` option calling
   `gta6rt_shims_set_console_profile` before `gta6rt_shims_init`; and `gta6rt_os_memory_report("watchdog" / "exit" /
   "fatal signal", 64 << 20, 12)` where the trace is dumped (plus an optional `--memory-report S` that sets
   `GTA6RT_MEMORY_REPORT_S`).
3. `verify.sh s1` proposal in `port/STATUS.md`: the exact count is now 36,765 under the default Series S profile
   (36,739 with `LEONIDA_CONSOLE_PROFILE=series-x`); the looser `367[0-9][0-9]` form covers both.
4. The host no longer needs to set `LEONIDA_UNIMPLEMENTED`; `fail` is the default.

## What is implemented

Registry and binding (`registry.cpp`, `shims.h`, `frame_adapter.h`, `api.cpp`):
- `SHIM_TABLE` rows `{dll, name, fn, frame}` are kept as in leonida. `fn` is the `ms_abi` function (x86_64, `static_assert`ed at compile time so a forgotten `WINAPI` cannot build); `frame` is a generated `gta6rt_frame_handler` for arm64/FEX (1/2/4/8-byte aggregates such as `LARGE_INTEGER` travel in the integer slot).
- `gta6rt_shims_import_resolver` binds code, ordinal and data imports. `api-ms-win-core-*`/`ext-ms-win-*`/`kernelbase` fall back to the `kernel32.dll` family and `api-ms-win-crt-*`/`ucrtbase*` to `ucrtbase.dll`.
- An import with no row gets a named TRAP thunk (`dll!name` in the log). Calling it is counted, logged and follows `LEONIDA_UNIMPLEMENTED` (default fail: E_NOTIMPL / EINVAL / 0 by return class with last error 120; log: return 0; stop: exit code 3; see the S1 follow-up). Unserved data imports return NOT_FOUND so the loader binds zeroed storage. `gta6rt_shims_report_unimplemented()` lists what was called.
- Host controls from `include/gta6rt/shims.h` (API version 3; 3 adds `gta6rt_shims_set_console_profile`): `configure_paths`, `set_command_line`, `run_entry`, `request_suspend`, `thread_detach`, `report_unimplemented`.

Ported leonida shims (all registered): `registry` + `console_registry` (directory-backed store under `<overlay>/.gta6rt-registry`, ConsoleMode from `LEONIDA_DEVICE_TYPE`), `process`, `lifecycle`, `library_loader` (+ bundled-module hooks for host/loader), `file_system` (GAME:/G:/T: drives, package read-only, writes copied up into the overlay, delete markers, case-insensitive lookup, `..` refused), `winhttp_offline`, `ws2_offline` (loopback only), `bcrypt_rng`, `xmem` (heap, physical-page aliasing, placeholders, virtual allocation modes), `xg_xs`, `com_shim`, `xbox_bvh_builder` (all 7 imports).

Trivial imports (the 223 of `spike/s1-win32-surface/inventory.json`): all 223 resolve to real code (`coverage_all_223_trivial_imports_resolve_to_real_code`). They are in `k32_misc.cpp`, `process.cpp`, `file_system.cpp`, `crt_string.cpp`, `crt_math.cpp`, `crt_convert.cpp`, `crt_stdio.cpp`, `crt_runtime.cpp`, `crt_time.cpp` over libc and the os.h interface. Beyond the 223, the printf family (`__stdio_common_vf/vs/vsw printf[_s]`, with a Win64 `va_list`) is served.
- LLP64: `long` is 32-bit in every signature, `time_t`/`tm` use the Win64 layouts, `rand` is MSVC's LCG.
- Wide strings are `char16_t` (UTF-16) throughout, never `wchar_t`: `wcslen`, `wcscpy`, `wcsncmp`, `_wcsicmp`, `wcstol`, `_wtoi`, `__stdio_common_vswprintf` and friends are implemented over 16-bit units, and `MultiByteToWideChar`/`WideCharToMultiByte` do surrogate pairs. Test: `crt_wide_strings_are_utf16`, `crt_convert_is_llp64`, `crt_printf_wide_and_win64_va_list`.

Win32 functions already implemented by gta6rt_os (S1.1) are bound as guest imports by `src/os_bindings.cpp` (317 kernel32 + 5 advapi32 rows, `ExitThread`/`FatalAppExitA`/`FreeLibraryAndExitThread` through non-noreturn wrappers). It is compiled only when the `gta6rt_os` target exists at configure time, and it never overrides a row shims_core serves itself (`register_entry_if_absent`). With os present the registry holds 757 rows (`shim registry: 757 rows` in `build/shims_core-real-run-x86_64.log`); a without-os count was never measured (the earlier 523 figure is withdrawn). These cover the 27 "design" imports (CloseHandle, CreateEventW, WaitForSingleObject, critical sections, SRW locks, semaphores, CreateThread, ...) that os implements today.

`com_shim.cpp` also defines the host-side `CoInitializeEx`, `CoUninitialize`, `CoGetApartmentType`, `CoCreateInstance`, `CoTaskMemAlloc/Free`, `CoCreateGuid`, `StringFromGUID2` that `compat/include/objbase.h` declares (bookkeeping COM only; no host COM on macOS).

## Stubbed or intentionally partial (every one logs, none is a silent zero)

Named logging stubs (no row; resolve to the TRAP thunk, listed per owner):
- Owned by other modules: `Rtl*` unwind/raise, `SetUnhandledExceptionFilter`, `UnhandledExceptionFilter`, `__C_specific_handler`, `__CxxFrameHandler3/4`, `_CxxThrowException`, `__RTDynamicCast`, `__current_exception*`, `__intrinsic_setjmp`, `longjmp`, `_seh_filter_exe` (seh, S1.3); MSVCP140 (113 imports) and the non-SEH VCRUNTIME140 exports (package DLLs via loader, S1.4); `DefWindowProcA`, `RegisterClassExA`, `ShowWindow`, `PeekMessageA`, `DispatchMessageA`, `TranslateMessage`, `CreateWindowExA` (d3d12_vulkan/host, S1.6); MFPlat/MFReadWrite (S2.3, 7 imports); GameInput/xapu/chad/AcpHal/XCRDAPI/d3d12_xs (shims_gdk, d3d12_vulkan).
- Not served by design: `WSAConnect*`, `WSAIoctl`, `ConnectEx`, `WSASend*`, `GetAddrInfoW/ExW`, `getnameinfo`, `WSALookupService*`, `WSAAsync*`.
- Math functions the exe does not import (for example `sqrt`, `floor`) are not registered; the 41 imported ones are.

Served rows that return a defined failure or no-op (log line on use):
- `SetNamedPipeHandleState`, `WaitNamedPipeW`: FALSE, named pipes are not available.
- `WerRegisterFile`: S_OK, no crash report is produced.
- `SetThreadAffinityMask`, `SetThreadGroupAffinity`: accepted, the thread stays (macOS has no affinity).
- `XMemIsToolsMemAvailable`: FALSE. `XMemTransferMemory`: TRUE, moves nothing (no title/system partition).
- `CoCreateInstance`: REGDB_E_CLASSNOTREG unless a class was registered. `BCryptOpenAlgorithmProvider`: only RNG is served.
- `MultiByteToWideChar`/`WideCharToMultiByte`: UTF-8, Latin-1, 1252 only; other code pages fail with a log. `FormatMessageA`: only the flags the title uses.
- `winhttp`: every request fails with ERROR_WINHTTP_CANNOT_CONNECT, no network; `ws2_32`: non-loopback fails with WSAENETUNREACH.
- `xg_xs`: layouts without a modelled addressing mode, thick layouts, element coordinates and view descriptors return E_NOTIMPL (0 for number slots).
- `XboxBvhBuilder`: needs the host device to expose `ID3D12Device5` (S1.6). Without it, builds are skipped and prebuild sizes are 0 (logged). Stream records and `Set64BitMortonCodes` are logged/ignored; compressionParams of rehydrate are not applied; source-less rehydrate streams return E_NOTIMPL.
- `xmem` differences from leonida (it had a Windows vectored fault handler): physical pages are committed whole at allocation, no zero-page fault planting for unmapped window pages (such an access faults), no write-watch shadows or `XMemResetWriteWatch`, no title-memory/label/geometry notifications for D3D12, no stream-pool mirror of `CopyPageMappingsBatchX`. `XMemVirtualQuery` zero-fills the fields past `MEMORY_BASIC_INFORMATION`.

## Tests

`shims_core_test`: 84 cases, built against `tests/env_stub.cpp` (a libc-backed stand-in for the os.h/isa.h subset the shims use, so the suite does not depend on S1.1/S1.2).
- arm64 native: 84 cases, 1176 checks, 0 failed (`build/shims_core-test-arm64.log`).
- x86_64 under Rosetta, one process, 120 s kill wrapper: 84 cases, 1174 checks, 0 failed (`build/shims_core-test-x86_64.log`). The check count differs because some checks are arch-conditional (ms_abi pointer call versus frame-handler call).
- On arm64 the table is called through the generated frame handlers with a hand-packed `gta6rt_guest_frame` (no FEX is present); that proves the adapter, not a guest running under FEX.

`leonida_bcrypt_rng_test`: leonida's `bcrypt_rng_test.cpp`, with `L"` -> `u"` and `wchar_t` -> `WCHAR` the only edits, plus the new `compat/include/bcrypt.h`. 21 checks, 0 failures on arm64 and x86_64 (Rosetta).

`shims_core_real_smoke` (configure with `-DGTA6RT_SHIMS_CORE_TEST_REAL_ENV=ON`, x86_64 only; skips on the fexcore backend): real `gta6rt_os` + `gta6rt_isa` (Rosetta backend). Binds imports through `gta6rt_shims_import_resolver`, calls the resulting thunks with `gta6rt_isa_call_guest` after `gta6rt_isa_init`/`set_peb`/`thread_attach`: `GetTickCount64`, `GetCurrentProcessId`, `wcslen` on a UTF-16 string with a surrogate pair, `pow` with xmm arguments, `CreateFileW`/`WriteFile`/`GetFileSizeEx`/`ReadFile`/`CloseHandle` on `T:\` (all core rows), and an unserved import (named stub, counted by `report_unimplemented`). 46 checks, 0 failed (`build/shims_core-real-run-x86_64.log`); the 21 added checks are the os-created-handle cases listed under Review response.

Build commands (from `port/`): `cmake -S . -B build/shims_core-<arch> -G Ninja -DCMAKE_OSX_ARCHITECTURES=<arch> -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA6RT_MODULES="os;isa;shims_core"` then `ninja -C build/shims_core-<arch> -j3`. Trees: `shims_core-x86_64`, `shims_core-arm64`, `shims_core-real-x86_64`, `shims_core-real-arm64`.

## Not done or not verified

- Leonida's own test sources were not run verbatim except `bcrypt_rng_test`. The others (`file_system_test` 1664 lines, `xmem_test` 2688, `winhttp_offline_test`, `ws2_offline_test`, `xg_xs_test`, `xbox_bvh_builder_test`/`bvh_builder_test`, `process_cmdline_test`, `lifecycle_test`, `console_registry_test`, `com_shim_test`, `xmem_wine_commit_test`) rely on Windows-only pieces: `L""` literals with `wchar_t` APIs, window/user32 calls, host COM and host registry seams, `winhttp.h`/`winsock2.h`, a real D3D12 device. Their scenarios were re-expressed in the 80 cases above, not mechanically ported, so a leonida assertion that is not in `tests/` is not covered. Mechanical porting needs `winhttp.h`/`winsock2.h` compat headers that do not exist.
- Everything but the 25-check smoke ran against the env stub, not the real os/isa; nothing ran inside a guest (no PE was executed by this module).
- The os_bindings rows are only as correct as gta6rt_os; they were not tested beyond the smoke calls above.
- `ID3D12Device5`/DXR for the BVH builder depends on S1.6.
- No Mach-O deployment target is pinned (X8); not measured here.

## Interface and shared-file changes

- `include/gta6rt/shims.h`: API version 2 host controls listed above (additive).
- `compat/include/bcrypt.h`: new, minimal (3 BCrypt entry points, `BCRYPT_ALG_HANDLE`, STATUS_* guards), no layout assert needed (no structs).
- Link graph note for the integrator: `gta6rt_shims_core` links `gta6rt_dxguids` privately (BVH builder IIDs) and, when the `gta6rt_os`/`gta6rt_isa` targets exist, links them; `os_bindings.cpp` is only added then. host/ already uses `-force_load` for the archive, which is required (rows register from static initialisers). The host must call `gta6rt_shims_configure_paths`, `gta6rt_shims_set_command_line` before `run_entry`, `gta6rt_shims_request_suspend` on window close and `gta6rt_shims_thread_detach` in the thread on_exit hook.

## Review response (2026-11-26)

### B1: os-created FILE handles were type-confused by the core file shims (fixed)

Cause confirmed: `file_of()` cast any `GTA6RT_HANDLE_FILE` object to `FileObj*`, but os also allocates `gta6rt_file*` under that kind (`GetStdHandle`, os `CreateFileW`, os `CreateFile2`).

Fixes (`src/file_system.cpp`, `src/file_system.h`, `src/os_bindings.cpp`):
- Every `FileObj` is recorded in a live set under a mutex when published and removed in `close_file`. `file_ref()` returns the core object only if the looked-up pointer is in that set; any other FILE-kind object is "foreign". Nothing reads a foreign object's memory.
- Foreign handles are forwarded to the os implementation through `rt::fs::ForeignFileOps`, which `os_bindings.cpp` installs from a static initialiser (os wrappers for `ReadFile`, `WriteFile`, `GetFileSizeEx`, `SetFilePointerEx`, `SetEndOfFile`, `FlushFileBuffers`, `GetFileTime`, `SetFileTime`, `GetFileInformationByHandle`, `ReadFileEx`, `ReadFileScatter`, `QueueUserAPC`, `GetCurrentThread`). `GetFileSize` and `SetFilePointer` go through the `_Ex` rows. When the hooks are not installed (unit tests against `env_stub.cpp`) a foreign handle fails with ERROR_INVALID_HANDLE (6), `*read`/`*written` zeroed, and the foreign object is not touched.
- The six dropped rows are back in the `file_system` table: `CreateFile2` (same overlay/path domain as `CreateFileW`, flags = `dwFileAttributes | dwFileFlags`, `dwSize` below 32 is ERROR_INVALID_PARAMETER), `CopyFile2`, `CopyFileExA`, `CreateDirectoryExA`, `ReadFileEx`, `ReadFileScatter`. They were omitted from the first STATUS by mistake. Core rows win over the os rows of the same name, so a title's `CreateFile2` now yields core handles, and the Bink path (`CreateFile2` + `GetFileSizeEx` + `SetFilePointerEx` + `ReadFile`) stays in one handle domain.
  - `CopyFile2`: returns HRESULT; `dwSize` < 32 is E_INVALIDARG, a set cancel flag is `HRESULT_FROM_WIN32(1235)`, flags outside the set leonida accepts are `HRESULT_FROM_WIN32(50)`; the progress routine is ignored (as in `CopyFileExW`).
  - `ReadFileEx` on a core handle reads synchronously, fills the OVERLAPPED, and queues the completion routine as an os APC (`QueueUserAPC` on the calling thread with a host trampoline that calls the routine through `gta6rt_os_invoke`), so it runs at the next alertable wait. Without the os hooks it fails with ERROR_NOT_SUPPORTED and a log line. EOF delivers ERROR_HANDLE_EOF to the routine.
  - `ReadFileScatter` on a core handle (needs an overlapped handle, a 4096-multiple length, a non-null OVERLAPPED, null reserved) fills the page segments synchronously, completes the OVERLAPPED and returns TRUE. No static import in the inventory uses `ReadFileEx`/`ReadFileScatter`, so both are covered by tests only.
- Tests added. `shims_core_test` (env stub): `fs_handles_not_created_by_core_are_never_cast_to_file_objects` (a foreign FILE handle over zeroed bytes, every file row fails with 6 and the bytes stay zero; then fake os hooks check every row forwards the right handle/arguments), `fs_core_handles_keep_working_beside_foreign_handles`, `fs_create_file2_copy_file2_and_the_rest_of_the_family`, `fs_read_file_scatter_and_read_file_ex_on_core_handles`. `shims_core_real_smoke` (real os): an os `CreateFile2` handle on a 16-byte file driven through the bound core `GetFileSizeEx` (16), `SetFilePointerEx`, `ReadFile` (12 bytes) and `CloseHandle`; the bound `CreateFile2` on a host path; os `GetStdHandle(STD_OUTPUT)` followed by the bound core `WriteFile` (TRUE, full line written); core `ReadFileEx` with a completion routine delivered by a real `SleepEx(0, TRUE)` (`WAIT_IO_COMPLETION`, 6 bytes, error 0).
- Results: arm64 `shims_core_test` 84 cases, 1176 checks, 0 failed; x86_64 under Rosetta (one process, `alarm` kill) 84 cases, 1174 checks, 0 failed; `leonida_bcrypt_rng_test` 21/0 on x86_64; x86_64 real smoke 46 checks, 0 failed (rc 0); arm64 real smoke prints SKIP (fexcore backend). The reviewer's exact failing probe (bound `GetFileSizeEx` size 0, bound `ReadFile` abort) was not re-run verbatim; the smoke's os-created-handle cases exercise the same calls and pass. The smoke was not run against the pre-fix code, so the failure it would show is the reviewer's measurement, not mine.
- Not covered: handle lookups still take no reference (reviewer non-blocking 4), so a concurrent `CloseHandle` during a core `ReadFile` is unsafe as before. os-only calls on core handles (`GetFileType`, `GetFileInformationByHandleEx`, `GetFinalPathNameByHandleW`, `DuplicateHandle`, `LockFile`, `GetOverlappedResult*`, `WriteFileEx`) still fail with ERROR_INVALID_HANDLE because core handles carry no os retain callback; none of these were verified against the static-import set beyond the reviewer's list.

### B2: row count (corrected)

750 was wrong. Measured with os present: 755 before this fix (the builder's and the reviewer's runs), 757 now (`CreateDirectoryExA` and `CopyFileExA` are new keys; `CreateFile2`, `CopyFile2`, `ReadFileEx`, `ReadFileScatter` already existed as os rows and are now core rows). The 523 without-os figure could not be reproduced and is withdrawn.

### Non-blocking findings

Not addressed in this pass: path-domain split for os-served `CopyFile2`-family rows (the core `CopyFile2`/`CopyFileExW` now cover those names in the core path domain; `CreateHardLinkW`, `CreateSymbolicLinkW`, `GetTempPathW` remain os), msvcp140/Bink2/libHttpClient ucrt and kernel32 imports without rows (needs an owner assigned by the lead), narrow printf not byte-preserving, `TitleVirtualAlloc` unaligned commit address, `MB_ERR_INVALID_CHARS` with a literal U+FFFD, the cosmetic duplicate-library linker warning.

## Review response (round 3)

To `isa/rosetta/REVIEW-round3-perf-policy.md`, blocking findings 1 and 2. Both are fixed in os/ and shims_core/;
nothing in host/ needs to change.

### Finding 1: a host linked with the default 4 GiB `__PAGEZERO` aborted the title at its 185 MB heap

- `os/CMakeLists.txt`: `gta6rt_os` carries `LINKER:-pagezero_size,0x10000` as an INTERFACE link option for x86_64
  executables (generator expression on the consumer's `TYPE`, so static/shared consumers are untouched; option
  `GTA6RT_OS_LOW_PAGEZERO`, default ON). The shipped `gta6rt` links `gta6rt_os`, so it gets the flag without an
  edit to `host/CMakeLists.txt`.
- Shown: I reconfigured my host tree `build/os-host-x86_64` with `CMAKE_EXE_LINKER_FLAGS` empty (it had the flag
  there before) and rebuilt `gta6rt`. `otool -l`: `__PAGEZERO` vmsize 0x10000; the link line has the flag once.
  Title run `run/logs/r3fix-iface.log` (`--trace=0 --hidden-window`, 40 s, load average 16): the heap is reserved at
  0x2fd80000 (below 4 GiB), 2,960 pages in address order, 0 not fresh, "one stretch of the pool"; the frame loop runs
  to the kill timer (347 submissions at t=43 s). The 3.4-4.1 frames/s against the reviewer's 15 is machine load
  (another worker's host build and title runs were going); the frame rate was not compared on a quiet machine.

### Finding 2: arm64 cannot have address space below 4 GiB

- Measured again (`scratchpad` probe, native arm64 and x86_64 default layout): `mach_vm_deallocate` of the page
  zero returns success, but a `VM_FLAGS_FIXED` map at 0x10000000 still fails (KERN_INVALID_ADDRESS) and an anywhere
  map from 0x10000 lands above 4 GiB. So the only route to low memory is the link flag, which arm64 refuses at exec.
- New `gta6rt_vm_lowest_mappable()` (os.h API 4): the end of the main executable's `__PAGEZERO`, read from its load
  commands. xmem's `LEONIDA_XMEM_32BIT` now has three values:
  - `auto` (default, also an empty value): a 32-bit request that finds no room below 4 GiB fails (ERROR line naming
    the lowest mappable address, ERROR_NOT_ENOUGH_MEMORY, as Windows would when that range is full) if the process
    has address space below 4 GiB, and is placed above 4 GiB if it has none, with a WARN line
    `xmem: 32-bit reservation of N bytes (attributes 0x7) placed above 4 GiB (this process has no address space below
    4 GiB, lowest mappable address 0x100000000; K so far)` and a counter (`rt::xmem::low32_placed_above()`).
  - `require`: always fail (the round 3 behaviour).
  - `anywhere`: always place above 4 GiB (as before). Any other value logs an ERROR and is treated as `auto`.
- Placement above 4 GiB is what xmem did for every title run before round 3 (frame loop to the 120 s watchdog). It
  keeps the address-order slot layout, so the GPU-side "one stretch" property still holds there.
- Shown on the real title with the arm64 condition (no address space below 4 GiB): the same host objects relinked
  without the flag (`scratchpad/r3/gta6rt_nopz`, `__PAGEZERO` 0x100000000, the reviewer's failing configuration),
  `LEONIDA_XMEM_32BIT` unset. `run/logs/r3fix-nopz.log`: the WARN line above, the heap at 0xdb6200000, 2,960 pages,
  one stretch, the frame loop to the 40 s kill timer (409 submissions at t=41 s), no `int3`. The reviewer's run of
  that configuration ended rc 134 after 7 s.
- Not shown: an arm64 title run past the heap request. The fexcore host stops earlier past WinMain
  (`isa/fexcore/STATUS.md`: `xgameruntime.dll` not found in its tree); arm64 is covered by the unit tests below.

### Tests

- `shims_core_test` (env stub; the stub implements `gta6rt_vm_lowest_mappable` the same way plus a test override
  `stub::set_lowest_mappable`): new `xmem_32bit_heap_served_by_default_on_this_host` (the title's exact request
  `XMemVirtualAlloc(0, 0xb900000, 0x42000, 7)`: below 4 GiB on x86_64, above 4 GiB with the counter +1 on arm64,
  where it also checks the lowest mappable address is >= 4 GiB), `xmem_32bit_heap_in_a_process_without_low_address_space`
  (auto serves above, `require` fails with 8, `anywhere` serves above, a bogus value acts as auto),
  `xmem_32bit_heap_refused_when_low_space_exists_but_is_full` (x86_64: fills the low 4 GiB, then auto fails with 8
  and does not place above). The old `xmem_32bit_heap_refused_without_room_below_4gib` is replaced. `shims_core_test`
  on x86_64 is now linked with the 64 KiB page zero so it exercises the low path.
- Results: arm64 93 cases, 1,258 checks, 0 failed; x86_64 (Rosetta) 93 cases, 1,258 checks, 0 failed;
  `shims_core_real_smoke` x86_64 46 checks, 0 failed.

### For the integrator

1. Nothing to add to `host/CMakeLists.txt`; an explicit `LINKER:-pagezero_size,0x10000` there is harmless.
2. `d3d12_vulkan/tools/run_vk.sh` (not mine) defaults `LEONIDA_XMEM_32BIT=anywhere`; with an x86_64 host rebuilt from
   these sources that forces the heap above 4 GiB. It can be dropped so `auto` places it below 4 GiB.
3. Rebuilding with this change gives every x86_64 executable that links `gta6rt_os` a 64 KiB page zero. I ran in
   my trees with it: os (5), isa (5), seh unit/dispatch, shims_core, shims_core real smoke, shims_gdk (19 programs
   plus 19 wait-registration cases): all pass. `loader_selftest` fails 3 checks (`file_map_count("/broken.dll")`,
   `file_map_count("/noread.dll")`, `reserves_with_image_flag`); it fails the same 3 when relinked without the flag,
   so that is the loader's current state, not this change.

## Integration change (round 3 integrator, 2026-11-26)

`src/xmem.cpp`: a window reserved with CPU `PAGE_NOACCESS` plus `PAGE_GRAPHICS_*` bits (the title's 16 GiB window at
0x700010000, protect 0x801) maps its physical pages read/write instead of no-access (`window_vm_prot`, logged once per
window). The D3D12 layer is host code and reads textures placed there (the Bink movie planes the title fills through
another alias of the same pages); with no-access pages it reported them "inaccessible" and never filled them, so the
launch movie presented as one solid colour (#FF79FF). The title's own code never touches such a window on the CPU (it
would fault on the console; it did not fault here with no-access pages in any run). Side effect: VirtualQuery on those
pages now reports PAGE_READWRITE. Test: `xmem_gpu_only_window_is_host_readable` (`tests/test_modules.cpp`);
`shims_core_test` 94 cases, 1,266 checks, 0 failed on arm64 and x86_64 (Rosetta). Title evidence in `port/STATUS.md`.
