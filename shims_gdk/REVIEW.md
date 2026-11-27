# S1.5b shims_gdk: independent review

Reviewer date: 2026-11-26. Verdict: **APPROVED WITH FIXES**.

Most of the port holds up. It builds clean on both arches, and the 38 ported test cases pass on both. A compile-time check
of every function pointer handed to the guest found all of them `ms_abi`. The registration tables match leonida.

Two defects show up only when the module links against the real shims_core. The unit tests miss both, because they use
the `tests/support` stand-ins:

- **B1:** every DirectStorage file read fails.
- **B2:** the silent audio endpoint cannot be reached from the guest.

There is also one wrong number in the report (B3).

## What I ran

All of it ran in private trees `port/build/review-shims-gdk-{arm64,x86_64}`. These were clean configures with the
builder's settings: Ninja, RelWithDebInfo, `GTA6RT_MODULES=os;isa;seh;loader;shims_core;shims_gdk`, `-j3`. Logs are in
`port/build/review-shims-gdk-{configure,build,run}-<arch>.log`. Every x86_64 process ran one at a time under
`tests/run_one.sh` with a kill timeout.

| Check | arm64 (native) | x86_64 (Rosetta) |
|---|---|---|
| Clean build | 166/166 steps, 0 errors | 172/172 steps, 0 errors |
| `tests/run_all.sh` | 38/38 PASS | 38/38 PASS |
| Repeats: 11 threading/IO binaries plus 4 `wait_registration` cases, 3 rounds | 45 runs, 0 failures | not run |
| `gdk_link_check` | OK: `gdk_register: -6` (`E_UNSUPPORTED`) | OK: all tables registered, 10 probes resolved |
| `compat_abi_test` | PASS | PASS |
| ABI audit (below) | n/a | 0 non-`ms_abi` pointers |
| B1 probe: DStorage read through the real shims_core | **FAIL** `0x8007001E` | **FAIL** `0x8007001E` |
| B2 probe: `CoCreateInstance(CLSID_MMDeviceEnumerator)` | **FAIL** `0x80040154` | not run |

**ABI audit.** I copied `src/` to scratch and replaced every `reinterpret_cast<void*>(` and
`reinterpret_cast<const void*>(` with a checker. The checker `static_assert`s that any function pointer passing through it
is `R(__attribute__((ms_abi))*)(...)`. I then compiled all 13 sources for x86_64 with `-fsyntax-only`.

- That covers 99 sites: every `SHIM_TABLE` row and every hand-built vtable (`VT`, `XM`/`Method<>`, `NM`, `PLS`, `PN`,
  `FN`).
- I confirmed the checker rejects a plain sysv function.
- C++ COM classes are covered by clang itself: it rejects an override whose calling convention differs from the base,
  and every base interface is `STDMETHODCALLTYPE`.
- Every guest callback typedef (`XTaskQueueCallback`, `XAsyncProvider`, GameInput, XUser, gamesave, store and package
  callbacks) is `CALLBACK`.

**Alias check.** I checked `guarded_copy` (`mach_vm_read_overwrite`) on a `mach_vm_remap` alias, which is how os builds
`gta6rt_vm_alias`. The copy lands in the shared pages, the alias is not detached, and later writes still show through it
(`/private/tmp/claude-501/-Users-a-Desktop-Projects-GTA-VI/eeb510fb-badf-42ab-9e97-2a01126ed0b0/scratchpad/rov.c`).

## Blocking

### B1. DStorage reads the shims_core file handle as an os `gta6rt_file` (every request fails, or reads another file)

`src/dstorage.cpp` `file_size_of` and `pread_at` (lines 517-546) do this:

```cpp
if (auto* f = static_cast<gta6rt_file*>(gta6rt_handle_lookup(handle, GTA6RT_HANDLE_FILE))) gta6rt_file_pread(f, ...);
```

The handle comes from `open_title_file`, which goes through `create_file_w()`, which is
`rt::shims::find("kernel32.dll","CreateFileW")`. In a real link that row is shims_core's `rt::fs::create_file_w`, and its
row overrides os_bindings' (`register_entry_if_absent`). shims_core stores its own `FileObj*` under
`GTA6RT_HANDLE_FILE` (`shims_core/src/file_system.cpp:495-531`).

So os's `gta6rt_file_size` and `gta6rt_file_pread` read `f->fd` at offset 4 of a `FileObj`. Offset 4 holds the bytes
`{is_dir, can_read, can_write, overlapped}`, which is fd 256 for a read-only file.

- **Measured, on both arches:** I linked the real shims_core, ran `rt::gdk::register_all()` and
  `gta6rt_shims_configure_paths`, then ran OpenFile on `GAME:\data\b.bin` and one 4 KiB read. The read completes with
  `hr=8007001e` (`ERROR_READ_FAULT`) and the bytes do not match. Probe: `/private/tmp/claude-501/-Users-a-Desktop-Projects-GTA-VI/eeb510fb-badf-42ab-9e97-2a01126ed0b0/scratchpad/ds_real_fs.cpp`.
- **Why the tests pass:** they get their CreateFileW from `tests/support/fs_stub.cpp`, which returns os handles.
- **Impact:** every DirectStorage file request from the title fails. With 257 or more descriptors open, `pread(256, …)`
  silently returns bytes from an unrelated file instead.
- **The fallback does not help:** `::ReadFile` in `pread_at` is os's ReadFile and has the same confusion. It is not
  reached in any case, because the lookup succeeds.
- **Fix:** read and size through the registered rows, as `create_file_w()` already does for CreateFileW. That means
  `rt::shims::find("kernel32.dll","ReadFile")` with an OVERLAPPED offset (shims_core's `read_file` is positional `pread`
  for both its own and foreign handles) and `GetFileSizeEx`. Then add a test that links the real shims_core, which is
  what the probe does.

### B2. silent_audio is never registered with CoCreateInstance (unreachable from the guest)

In leonida, `com_shim.cpp` routed `CLSID_MMDeviceEnumerator` to `silent_audio::create_enumerator`. In the port,
shims_core's `com_shim.cpp` header comment says this class is "registered with register_class … by shims_gdk". No code
calls `rt::com_shim::register_class`: `nm -u` of the archive shows no reference, and a grep of `port/` finds only the
definition and shims_core's own test.

- **Measured, arm64, real shims_core:** `CoCreateInstance(MMDeviceEnumerator)` returns `80040154 REGDB_E_CLASSNOTREG`.
  Calling `silent_audio::create_enumerator` directly returns `S_OK` (`/private/tmp/claude-501/-Users-a-Desktop-Projects-GTA-VI/eeb510fb-badf-42ab-9e97-2a01126ed0b0/scratchpad/audio_probe.cpp`).
- **Impact:** `silent_audio.h` itself records that without a render endpoint "the title's … audio engine never mixes, so
  the boot stops". STATUS lists silent_audio as implemented and does not name this gap. Its integrator steps ("call
  `gta6rt_shims_init()` then `gta6rt_shims_gdk_register()`") leave the class unwired.
- **Fix:** register it from shims_gdk with an adapter to
  `ClassFactoryFn = int32_t(*)(uint32_t, const Guid*, void**)`, for example in `register_all()` or a static
  initializer. Then extend `gdk_link_check` to create the class.

### B3. False number in the report: "resolves 11 imports"

`tests/link_check.cpp` has 10 probes, and the x86_64 run prints 10 resolutions. STATUS.md and the report both say 11.

## Should fix (not blocking)

1. **`xgameruntime_offline.cpp:537` `device_audio_endpoint` uses `wchar_t*`.** This is IXUser slot 6,
   `XUserGetDefaultAudioEndpointUtf16`. Writing `id[0] = L'\0'` stores 4 bytes into the guest's UTF-16 buffer, so with
   `count == 1` it writes 2 bytes past it. It should be `WCHAR*` / `u'\0'`. This is the only `wchar_t` left in a
   guest-facing signature.
2. **`shared/` headers are stale copies of leonida's and disagree with shims_core's definitions (an ODR risk).**
   - `shared/file_system.h` declares `std::filesystem::path rt::fs::game_dir()/overlay_dir()`, while shims_core defines
     them returning `std::string`. The mangled name is the same, and it works only because libc++'s `path` is a lone
     `std::string`.
   - `rt::shims::Entry` has 3 fields here and 5 in shims_core.
   - The `XMemAllocHookFn` types differ.
   - The clean build warns `'__stdcall' calling convention is not supported` at `shared/shims.h:86-87` (in
     `link_check.cpp`). So "no real warnings" holds only for the incremental build the builder logged.

   The fix is to include shims_core's headers, or reduce `shared/` to the declarations actually used. Per `nm -u` those
   are `rt::fs::game_dir/overlay_dir`, `rt::shims::find/module_offset` and `rt::xmem::transfer_physical`.
3. **The registration tables do not check ABI at compile time.** shims_core's `SHIM()` `static_assert`s `ms_abi`. Here
   `SHIM_TABLE` rows are raw `reinterpret_cast<void*>`, so a future non-WINAPI row would compile and crash at the first
   call. The audit above found none today. Switching to shims_core's `SHIM()` would also bring the arm64 frame-adapter
   slot (`GTA6RT_FRAME_OF`), which is the X5 work STATUS describes.
4. **`ThreadPool_posix::Submit` can drop work silently.** If it cannot spawn a thread while `m_threads == 0`, it drops a
   pending item, so the task queue waits forever for that callback. Log it at least.
5. **`WaitTimer_posix` keeps one parked guest thread per timer from its first `Start` until `Terminate`.** Each queue
   port has a timer, so a title with many queues that use delayed callbacks keeps that many idle threads. Windows uses
   pooled threadpool timers.
6. **Smaller items:**
   - `pread_at` tests `errno == EFAULT` after any non-`E_ACCESS` status, so it trusts errno to come from that call. With
     B1 fixed through ReadFile, this mapping goes away.
   - `env_get` prefers `getenv` over `gta6rt_env_get`, so a guest `SetEnvironmentVariable` cannot override a host
     variable. Leonida read the process environment.
   - The gamesave commit is `fs::rename` with no fsync, where leonida used `MoveFileEx(…WRITE_THROUGH)`.
   - The `xapu_chad` APU buffer comes from a raw `mmap`, so os's VM table does not see it for `VirtualQuery`.
7. **Housekeeping:**
   - `port/shims_gdk/build/shims_gdk-arm64/` is a stray CMake configure tree with `shims_gdk/` as its source dir
     (00:39); delete it.
   - The CMakeLists header says "STATUS.md lists" the undefined references, but it does not.

## Report and STATUS claims checked

| Claim | Finding |
|---|---|
| All shims ported, leonida tables kept; counts 42/2/9/5/5/1+1+1/2 | True (`gdk_link_check` table dump) |
| acphal 0 changed lines; others 2-375 | True (diff vs `leonida/runtime/shims`) |
| WCHAR is 2-byte `char16_t` throughout | True for dstorage (layout `static_assert`s compile); false at `device_audio_endpoint` (fix 1) |
| xtaskqueue "two small edits" | True: `_WIN32` -> `GDK_TQ_HANDLE_WAITS` (in TaskQueue.cpp and TaskQueueImpl.h) and one `CALLBACK`; README updated |
| 38/38 on both arches | True (re-run) |
| 88 repeat runs; x86_64 "run twice" | No log of either; one run log per arch exists. Not contradicted: my 45 arm64 repeats passed |
| link check "resolves 11 imports" | **False: 10** (B3) |
| arm64 `gta6rt_shims_gdk_register()` = `E_UNSUPPORTED` | True (`-6`) |
| dstorage reads via `gta6rt_file_pread` | True, and this is the defect B1 against the real shims_core |
| silent_audio "silent path only" | The code is; it is unreachable from the guest (B2) |
| Stubs listed honestly; nothing silently succeeds | Yes for the listed stubs: gameinput slots `E_NOTIMPL`, `XAsyncBeginAlloc` `E_NOTIMPL`, offline classes as leonida. `title_commit` returning true with no hook matches shims_core's commit-on-allocation xmem |
| No top-level or `include/gta6rt/` change needed | True for linking. B2 needs a call into shims_core's `com_shim.h`, which is private to shims_core |

## For the lead

- **B1 and B2** are local fixes in this module, tens of lines each. Both need a test that links the real shims_core
  instead of `tests/support`; `gdk_link_check` is the natural place.
- **Handle kinds:** B1 is the mirror image of shims_core review B1. Two modules store different object types under
  `GTA6RT_HANDLE_FILE`. Either give shims_core's files their own handle kind, or have every module reach files only
  through the registered kernel32 rows.
- **STATUS decisions** (the `gdk_title_memory.h` placement and arm64 frame adapters) are unchanged by this review.
