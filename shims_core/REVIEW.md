# shims_core (S1.5a) independent review

Reviewer date: 2026-11-26. Verdict: **APPROVED WITH FIXES**. There is one confirmed crash and silent-corruption defect, and one wrong number in STATUS.md. Both are listed under Blocking.

## What I built and ran

The private trees are under `port/build/`. They are separate from the builder's trees.

| Tree | Configure | Build | Run |
|---|---|---|---|
| `review-shims-core-arm64` | `GTA6RT_MODULES="os;isa;shims_core"`, arm64 | 88/88 steps, 0 compiler warnings | `shims_core_test`: **80 cases, 821 checks, 0 failed**. `leonida_bcrypt_rng_test`: **21 checks, 0 failures** |
| `review-shims-core-x86_64` | same, x86_64 | 92/92 steps, 0 warnings | Run under Rosetta, one process at a time, with a perl `alarm` kill. `shims_core_test`: **80 cases, 819 checks, 0 failed**. `leonida_bcrypt_rng_test`: **21 checks, 0 failures** |
| `review-shims-core-real-x86_64` | same, plus `-DGTA6RT_SHIMS_CORE_TEST_REAL_ENV=ON` | 76/76 steps. One *linker* warning: `ignoring duplicate libraries: isa, os`. It is cosmetic and also appears in the builder's logs | `shims_core_real_smoke`: **25 checks, 0 failed**. It logs `shim registry: 755 rows` |
| builder's `shims_core-real-arm64` (run only, native) | — | — | Prints `SKIP (backend fexcore ...)`, rc 0, as claimed |

I also checked the following:
- **bcrypt test edits:** `leonida/runtime/test/bcrypt_rng_test.cpp` with `L"`→`u"` and `wchar_t`→`WCHAR` applied is byte-identical to `tests/leonida/bcrypt_rng_test.cpp`. The claim holds.
- **Trivial import list:** `tests/trivial_imports.inc` matches the 223 `port_class == trivial` imports of `inventory.json` exactly; only the DLL-name case differs. The coverage test resolves each one with `serve_stub=false`, so that claim is real.
- **Registry rows:** I extracted all rows by preprocessing every `src/*.cpp`. That gave 520 distinct core rows and 322 os_bindings rows, 97 of them overlapping. Core rows always win, whatever the static-init order: `register_entry` is `insert_or_assign` and `register_entry_if_absent` is `emplace`. The claim holds.
- **Unserved exe imports:** every exe import without a row belongs to a module STATUS names (GDK HALs, d3d12_xs, window, SEH/`Rtl*`, MF, msvcp140, vcruntime EH, the `vsscanf` pair, `_get_stream_buffer_pointers`, `_seh_filter_exe`). That list is honest.
- **Unserved data imports:** the loader binds them to zeroed storage with a `log_warn` (`loader/src/pe_imports.cpp`), so that path is not silent.

## Blocking

### B1. Handles from os are type-confused by the core file shims (crash or silent wrong data)

**Where.** `src/file_system.cpp:521`:
```cpp
FileObj* file_of(HANDLE h) {
    return static_cast<FileObj*>(gta6rt_handle_lookup(reinterpret_cast<uintptr_t>(h), GTA6RT_HANDLE_FILE));
}
```

**Cause.** gta6rt_os creates its file handles with the same kind, `GTA6RT_HANDLE_FILE`, through `handle_alloc_internal(GTA6RT_HANDLE_FILE, gta6rt_file*, ...)`. Those handles come from `GetStdHandle`, `CreateFile2` and os `CreateFileW`. `gta6rt_handle_lookup` checks only the kind, so every core file function casts an os `gta6rt_file*` to `FileObj*`. The two layouts differ, as follows:
- `FileObj::fd` reads `gta6rt_file::refs`, which is 1.
- `FileObj::is_dir` reads the low byte of the real fd.
- `FileObj::m` (a `std::mutex`) lands on `gta6rt_file::path` (a `std::string`).

The reverse direction is safe. os `fref()` goes through `handle_ref`, which requires a `retain` callback. Core handles have none, so os calls on core handles fail cleanly with `ERROR_INVALID_HANDLE`.

**Why it is reached.**
- `os_bindings.cpp` serves `CreateFile2`, `GetStdHandle`, `ReadFileEx` and `ReadFileScatter` from os.
- The port's `file_system` table dropped six rows that leonida's table has: `CreateFile2`, `CopyFile2`, `CopyFileExA`, `CreateDirectoryExA`, `ReadFileEx` and `ReadFileScatter` (leonida `runtime/shims/file_system.cpp` around line 1478). STATUS does not list them anywhere.
- `Bink2Scarlett.dll` is a static import of the exe (`inventory.json` `transitive_static`). Its only file imports are `CreateFile2`, `GetFileSizeEx`, `SetFilePointerEx`, `ReadFile` and `CloseHandle` (`spike/s1-win32-surface/pe/Bink2Scarlett.dll.json`). The handle therefore comes from os, and every later call except `CloseHandle` goes to core.

**Measured.** I wrote a native program linking the review trees' `libgta6rt_shims_core.a` (`-force_load`), `libgta6rt_os.a` and `libgta6rt_isa.a`. It writes a 16-byte file, opens it with os `CreateFile2(u"Z:\\...")`, then calls the bound rows from `rt::shims::find("kernel32.dll", ...)`. The result was identical on arm64 native and on x86_64 under Rosetta:
```
os CreateFile2 -> 0x1000 (err 0)
os GetFileSizeEx -> 1 size=16
bound GetFileSizeEx -> 1 size=0 err=0 (file is 16 bytes)      <- TRUE with a wrong size (it fstat'd fd 1)
libc++abi: terminating due to uncaught exception of type std::__1::system_error: mutex lock failed: Invalid argument
                                                               <- bound ReadFile, process aborts (rc 134)
```

A second probe tested `GetStdHandle(STD_OUTPUT_HANDLE)`, which is os, followed by `WriteFile`, which is core. It returned FALSE with error 5 (`ERROR_ACCESS_DENIED`), while os `WriteFile` on the same handle wrote. vcomp140 imports exactly this pair.

**Impact.** The first Bink video open aborts the process. The intro or logo movies are a likely first use, which puts this on the boot path. Before the abort, `GetFileSizeEx` silently reports size 0. If `SetFilePointerEx` runs first, it locks a `std::mutex` placed over a `std::string`, and its `f->pos` store writes into os's `pos_mx`.

**Fix.**
1. Port leonida's `create_file2`, plus `ReadFileEx`, `ReadFileScatter`, `CopyFile2`, `CopyFileExA` and `CreateDirectoryExA`, into the `file_system` table. That keeps one handle and path domain: GAME:/T: overlay, case-insensitive lookup.
2. Make `file_of()` reject objects it did not create, for example with a magic word checked before use or a set of live `FileObj*`. Unknown FILE handles should then return `ERROR_INVALID_HANDLE`, or be forwarded to the os implementation. Forwarding fixes `GetStdHandle` + `WriteFile`.
3. Add a test that drives an os-created FILE handle through the core rows. The current suite cannot see this because `shims_core_test` links `env_stub.cpp`, not os, and the smoke only uses core-created handles.

### B2. The row count in STATUS.md is wrong

STATUS.md and the report say "With os present the registry holds 750 rows". The builder's own `build/shims_core-real-run-x86_64.log` prints `shim registry: 755 rows`, and so does my rerun. Correct the number. The claim of 523 rows without os is not printed by any log, so I could not check it.

## Non-blocking findings

1. **Path domain split.** Even after B1 is fixed, `CopyFile2`, `CreateHardLinkW`, `CreateSymbolicLinkW` and `GetTempPathW` are imported by msvcp140 and served by os. They resolve paths through `gta6rt_path_*` (Z:→/, root and prefixes), not through file_system's GAME:/T:/overlay mapping. A file the title writes to T: through core may not be visible at the same Windows path through these rows.
2. **msvcp140's own ucrt imports are not served and STATUS does not mention them.** This is outside the 223, which cover the exe only. msvcp140 is a static import, and the inventory classes these imports trivial/S1.1. They include `__pctype_func`, `___lc_codepage_func`, `___mb_cur_max_func`, `_lock_locales`, `setlocale`, `_beginthreadex`, `_endthreadex` and `rand_s`. Bink2 adds `_seh_filter_dll`; libHttpClient adds `_wassert`, `strerror` and `_callnewh`. In all, 63 ucrt and 13 kernel32 transitive-static imports have no row. Under the default `log` policy each returns 0. `__pctype_func` returning NULL and `_beginthreadex` returning 0 will matter as soon as msvcp140's locale or `std::thread` code runs. The lead should assign an owner.
3. **Narrow printf is not byte-preserving.** Format output goes through UTF-16, so narrow bytes that are not valid UTF-8 become U+FFFD. Measured on arm64: `__stdio_common_vsprintf(…, "%s", "caf\xE9")` returns 6 bytes, `63 61 66 EF BF BD`. MSVC copies the 4 bytes unchanged. Literal format text is affected the same way (`printf_engine.h`, the `widen()` run collection).
4. **Lookups take no reference.** Core handle lookups (`file_of`, `key_of`) use `gta6rt_handle_lookup` and never retain the object. A `CloseHandle` on one thread during `ReadFile`/`WriteFile` on another frees the `FileObj` under the reader.
5. **Default stub value means success for some APIs.** Under the default `log` policy a trap stub returns 0, which is success for int-status APIs. Examples are the `WSAIoctl`/`WSAConnect*`/`GetAddrInfoW`/`getnameinfo` rows that leonida served (loopback-checked) and the port dropped. No module in the inventory imports them statically, so the risk is only through `GetProcAddress`. STATUS does list them as unserved.
6. **Unaligned commit address.** `TitleVirtualAlloc` with an unaligned `address` and `MEM_COMMIT` only returns `address` unchanged. Windows returns the page-rounded base.
7. **`MB_ERR_INVALID_CHARS` check.** In `MultiByteToWideChar`, one literal `EF BF BD` anywhere in the input disables invalid-byte detection for the whole string.
8. **Linker warning.** "ignoring duplicate libraries" comes from listing `gta6rt_isa`/`gta6rt_os` twice for `shims_core_real_smoke`. It is cosmetic. The "0 warnings" claim is true for compiler warnings.

## Claims checked

| Claim | Result |
|---|---|
| Both arches build, 0 warnings | True for compiler warnings; one linker warning in the real-env tree |
| arm64: 80 cases, 821 checks, 0 failed | Reproduced |
| x86_64 (Rosetta): 80 cases, 819 checks, 0 failed | Reproduced |
| `leonida_bcrypt_rng_test` 21/0 on both arches; only the `L"`/`wchar_t` edits | Reproduced; diff verified |
| Real smoke: 25 checks, 0 failed; arm64 prints SKIP | Reproduced |
| All 223 trivial imports resolve to real code | True (list matches the inventory; resolved without stub fallback) |
| Unserved imports get a named logging stub, never a silent zero | True for code imports (TRAP thunk, logged on the first call). Data imports are logged by the loader. Returning 0 is still success for some APIs (finding 5) |
| os_bindings never overrides a core row | True by construction, independent of init order |
| Registry holds 750 rows with os | **False: 755** (B2) |
| file_system ported | Partially. Six leonida rows were dropped without being listed, which leads to B1 |
| ms_abi enforced | True: `IsGuestAbi` `static_assert` on x86_64 for every `SHIM` row, os rows included |
| Wide CRT over `char16_t`; LLP64 `long`; Win64 `tm`/`_stat64`/`MEMORY_BASIC_INFORMATION`/`lconv` layouts | Read and found consistent. `static_assert`s are present for the struct sizes |
| No `Co*` symbol clash | No other non-test definition in port/ (`shims_gdk/tests/support/com_stub.cpp` is test-only) |

## Not checked

- None of the 80 cases were re-derived against leonida's own test assertions. Like the builder, I did not run the other leonida tests.
- I did not execute any guest PE. FEX was not exercised, so the arm64 frame adapters were only exercised by the hand-packed frames in the suite.
- I did not check the 523-row count without os.
- I did not review xg_xs, xbox_bvh_builder or winhttp internals beyond their tables.
