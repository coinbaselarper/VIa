# S1.4 loader: independent review

Verdict: **APPROVED WITH FIXES.** The PE mapping, relocation, import binding, export/forwarder resolution, data-import
handling and package-DLL loading are correct on the real files. I checked this independently of the loader's own
tests (below). The fixes concern how the loader fits the current `isa.h` / `os.h` v2 interfaces, two STATUS
statements that are not true, one ordering divergence from the leonida code being ported, and one flaky check.

Nothing in `loader/` other than this file was changed. Review build trees and logs are
`port/build/review-loader-{x86_64,arm64}` and `port/build/review-loader-*.log`. The independent cross-check harness
lives outside the project in the session scratchpad (`xcheck/xcheck.cpp`, `xcheck/verify.py`).

## What I ran

Built `loader_pe_stats` and `loader_selftest` from scratch for both arches (`-DGTA6RT_MODULES="os;isa;seh;loader"`,
ninja -j3). Both built with 0 warnings. x86_64 binaries ran one at a time under Rosetta with a 300 s alarm.

| run | result |
|---|---|
| `loader_pe_stats` x86_64 | 96 passed, 0 failed (matches the report) |
| `loader_selftest` x86_64 | 151 passed, 0 failed (matches) |
| `loader_pe_stats` arm64, 4 runs | 94 passed, 0 failed each time (matches) |
| `loader_selftest` arm64, 8 runs | **3 runs 150/0, 5 runs 149/1**. The failing check is `iai.base % 0x10000 == 0 && ibi.base % 0x10000 == 0` (see F4) |
| full-tree configure (all modules) | fails today in `isa/CMakeLists.txt:22` (`isa/tests` has no CMakeLists.txt). That is another worker's tree in progress, not the loader |

**Independent cross-check.** I wrote a harness that links the built `gta6rt_loader` with the posix test ops. It
maps the exe and all 16 package DLLs, binds them through `gta6rt_loader_package_import_resolver` with no shim
resolver, and dumps every IAT slot. A separate Python parser (no loader code) then rebuilds each export
table, follows forwarders, and compares. I ran it on arm64 at the preferred base, on arm64 with the exe forced away
from its base (0x10cc70000), and on x86_64.

- **Imports:** 467 package-resolved imports across the exe and all 16 DLLs. Each one points at the right DLL and
  export RVA: 0 mismatches, 0 trapped imports that the package could have served. The 1,457 imports of DLLs that are
  not in the package are all trap thunks.
- **Mapped bytes:** I compared each image against its file. Every section byte equals the file byte, except for IAT
  slots and relocation sites, and the zero-fill tails are all zero. In the exe that covers 184,966,180 bytes, with 0
  differences, both at the preferred base and at the forced base.
- **Relocations:** every DIR64 site equals the file value plus the delta. That is 598,282 of 598,282 in the exe and
  0 bad in any DLL.
- **The only byte differences** are 1 byte each in libHttpClient.GDK, XboxBvhBuilder and amd_fidelityfx_upscaler.
  These are the `_tls_index` writes for indices 1, 2 and 3. I confirmed the RVAs are each DLL's `AddressOfIndex`, in
  `.data`.
- **Data imports:** the exe has exactly 6 data imports (3 `??_7` vftables and 3 locale items). Of the 1,353 decorated
  `msvcp140` exports, 142 sit in non-executable sections. Both numbers match the report and correct S0.3. The
  package DLLs' own 5 data imports all bind into `msvcp140`, so none falls back to zero storage.

**Other reported numbers I reproduced:**
- The exe has 10 sections, 587 imports in 68 DLLs, 24 by ordinal, and 299,257 `.pdata` entries.
- Its TLS template is 5,248 bytes with align 16 and 2 callbacks.
- The exe binding splits into 202 package, 7 shim and 378 trap.
- 3 DLLs have static TLS.
- On x86_64 the exe lands at 0x140000000 at the first attempt.
- The forced relocation took 75 ms including the map (reported: about 50 ms).

**Not run (and not claimed):** the real `gta6rt_os` / `gta6rt_isa` / `gta6rt_seh`, `--os` mode, module or seh
registration, real thunks, and any guest code. `os/CMakeLists.txt` still builds only `os_placeholder.cpp`, so
`os_available()` is false. The report and STATUS say this correctly.

## Blocking

**B1. The TLS model does not match `isa.h` API 2, and guest code on threads that isa attaches itself would crash.**
- **What isa.h now specifies:** isa.h (v2, "TLS images/slots, foreign threads") defines
  `gta6rt_isa_tls_register_image(tls_index, ctor, dtor, user)`. isa owns the growth of `ThreadLocalStoragePointer`.
  Threads that call into the guest without a TEB (dispatch, CoreAudio, Metal callbacks) are attached by
  `ensure_thread_attached` (`isa/common/isa_core.cpp:142`). Unless a foreign-thread hook is set, that path passes
  `tls_pointer == NULL` and gets an array built only from images registered with isa.
- **What the loader does:** it keeps its own 64-slot arrays (`tls.cpp`) and never calls
  `gta6rt_isa_tls_register_image`. On such a thread slot 0 is NULL, so the exe's `/GA` TLS access
  (`gs:[58h]` then `[0]`) dereferences NULL.
- **Second problem:** if anything does register with isa, isa replaces a caller-supplied array with its own copy on
  the first growth (isa.h comment). After that, the loader's late `tls_assign` writes land in an array the thread no
  longer uses.
- **STATUS:** finding 5's thread-hook recipe covers only os-created threads.
- **Fix, either way:** have `tls_assign` / `tls_release` call `gta6rt_isa_tls_register_image` /
  `gta6rt_isa_tls_unregister_image`, using `make_block` and `free` as ctor and dtor, and stop passing loader arrays
  as `tls_pointer`. Or document that the host must install `gta6rt_isa_set_foreign_thread_hook`, and that the hook
  calls `tls_alloc_thread` and then attaches. The first keeps a single owner of the arrays.
- **Why it was missed:** isa.h was edited (00:33) while the loader was being finished, so this is interface drift. It
  still has to be resolved before integration.

**B2. A STATUS statement is false against the current os.h.** "`gta6rt_vm_region.is_image` cannot be set: there is no
os.h call to mark a region as image-backed." os.h v2 defines `GTA6RT_VM_IMAGE 32u` ("region is a mapped PE image
(VirtualQuery reports MEM_IMAGE)"). `os/src/vm.cpp:369` honours it. The loader reserves images without the flag
(`pe_map.cpp:67`, `:39`, `:47`, `:52`), so VirtualQuery on image pages would report MEM_PRIVATE. Fix: OR in
`GTA6RT_VM_IMAGE` on every image reserve, and correct STATUS.

**B3. A STATUS statement is only partly true.** "A DLL that fails to load is remembered and its imports are trapped (no
retry storm)." `g_pkg.failed` is filled only when `finish_load` fails (`package.cpp:214`).
- **Not remembered:** a read failure (`:179`) or a map failure such as BAD_FORMAT (`:206`). Every later import of that
  DLL re-reads and re-parses the file and logs the error again.
- **Explicit loads:** `package_load` never consults `failed`.
- **Evidence:** the selftest log shows `broken.dll: not an MZ image` twice.
- **Fix:** insert into `failed` on those two paths, or reword STATUS. The fix is small.

## Non-blocking, should fix

**F1. Thread-notification order differs from Windows and from the leonida code being ported.**
`gta6rt_loader_notify_thread` builds its list as exe first, then the package DLLs (`package.cpp:431-434`). On
DLL_THREAD_ATTACH the exe's TLS callbacks therefore run before every DLL's DLL_THREAD_ATTACH.
`leonida/runtime/src/pe_loader.cpp:1144-1148` documents a measurement on Windows 11 26300: the exe's TLS callbacks
run after every DllMain, for both attach and detach. Detach is correct here, because the reversed list puts the exe
last; attach is not. Separately, `package_detach` calls DllMain before the TLS callbacks, while leonida runs TLS
callbacks first; that difference only matters at exit.

**F2. TLS callbacks and DllMain run outside the package lock in `notify_thread`.**
- **During shutdown:** the guest calls run after `g_mu` is released, so a concurrent `package_shutdown` or `unmap` can
  free an image that is being notified. This only happens at exit.
- **DllMain returning FALSE:** the DLL stays mapped, stays `initialised`, and keeps getting thread notifications.
  Windows unloads it.
- **Status code:** the failure is reported as `GTA6RT_E_IO`, which `shims_core` cannot tell apart from a real I/O error
  when it maps the result to `ERROR_DLL_INIT_FAILED`. A dedicated status, or a documented mapping, is needed.

**F3. The arm64 address-layout note is partly wrong.** STATUS finding 1 says neither 0x140000000 nor 0x180000000 was
available and that "the host link layout has to reserve that range".
- **0x140000000:** in my arm64 runs the exe mapped there in 3 of 5 processes. It depends on ASLR, not on the link
  layout.
- **0x180000000:** this is inside the arm64 dyld shared cache, which `vmmap` shows at 0x182dac000 and up, with the
  shared `__LINKEDIT` reaching 0x33c4b8000. No link layout can reserve it.
- **Consequence:** on arm64 every package DLL is relocated. Because the shared cache occupies that whole range, all 5
  fixed candidates (0x180000000 up to 0x240010000) fail for every DLL. The note should say that 0x180000000 is
  unreachable on arm64 and that the DLL bump base should start above the shared region (or simply use
  ALLOW_RELOCATE).

**F4. The 64 KiB re-placement in `reserve_any_aligned` is unreliable, and the arm64 selftest is flaky because of it.**
- **What it does:** it releases a page-aligned reservation and re-requests it FIXED at the next 64 KiB boundary. That
  fails whenever the bytes past the old end are occupied, which is the usual case for a first-fit allocator.
- **What happened in my runs:** the code then warns and uses the misaligned base. It happened in every arm64
  `pe_stats` run (all relocated DLLs got 16 KiB-aligned bases, e.g. vcruntime140 at 0x100d54000) and in 5 of 8 arm64
  selftest runs.
- **Effect on real boot:** with the real os this path is moot, because `gta6rt_vm_reserve` aligns to 64 KiB through
  a mach_vm mask. The report's statement that the loader re-places a misaligned image itself is best-effort only.
- **Fix:** over-reserve by 64 KiB and trim, or drop the fallback and rely on os.
- **Tests:** `pe_stats` should also check DLL alignment.

**F5. Smaller items.**
- **`loader.h` header text:** the comments at lines 8 and 62 still say "three of them MSVCP140 data imports" and
  "@@[0-3]". The interface header contradicts the implementation and the corrected finding.
- **`resolve_forwarder`:** it passes `kind_guess = 0` (not a valid enum value) to the shim resolver. It should use
  `guess_kind(sym)`.
- **`gta6rt_loader_stats`:** it ignores errors from `enumerate_imports` and still returns OK with partial counts.
- **`trap_thunk` failure:** `bind_imports` maps it to `NOT_IMPLEMENTED` instead of passing on the `make_thunk` status.
- **Stack sampler:** `g_sampler` is a global `std::thread`. If `GTA6RT_STACKS` is set and the host exits without
  `gta6rt_loader_stop_stack_sampler`, the joinable thread's destructor calls `std::terminate` during static
  destruction. Detach it, or document the stop call in the boot order.
- **`describe_address` / `find_image`:** they take a `shared_mutex`, so they are not safe to call from a signal
  handler. seh should not use them in its fault path.

## Checked and found correct

- **ABI:**
  - All host-to-guest calls go through `call_guest` with Win64 argument order: base, reason, reserved.
  - The DllMain `BOOL` is read as 32 bits (`static_cast<uint32_t>(r.rax)`).
  - `_tls_index` is written as a 4-byte ULONG.
  - No `ms_abi` host functions live in the loader, and there are no wide strings: paths are UTF-8 only.
- **Bounds checks:** every image RVA is checked, including import descriptors and thunks, names and hints, the
  delay-import table, export tables, the relocation table, the TLS directory and callbacks, and the exception table.
  All 25 checks in the selftest's malformed-image section pass on both arches.
- **`lpvReserved`:** static loads get non-NULL (a zeroed 1,232-byte CONTEXT-sized buffer, matching leonida's captured
  CONTEXT). Dynamic loads and their dependencies get NULL.
- **Initialisation order:** dependencies come first (`init_order` is filled after binding). Cycles are safe because
  exports are parsed before imports are bound. Re-entrant `package_initialize` from DllMain works through
  `init_started`.
- **Exports:** forwarders by name and by ordinal across DLLs resolve, with a depth limit of 8.
- **Unresolved imports:** they get `GTA6RT_THUNK_TRAP` thunks with a `dll!sym` name. No import silently gets a stub
  that returns 0. Unserved data imports get 64-byte zeroed, 64-aligned storage and a warning.
- **TLS:**
  - Blocks are aligned to at least 16 and are template plus zero-fill.
  - When an image is assigned an index later, a block is placed in every live array under the mutex.
  - A DLL never takes index 0.
- **Protections:** `finalize_protections` refuses to run before relocation. `bind_imports` refuses to run after
  finalize, so the IAT is never written read-only.
- **`crt_timed_wait.cpp`:** leaving it unported is justified. It works around Wine 9's `_Mtx_t` layout inside
  leonida's own /MD host and has no counterpart here.
- **Stubs and gaps:** the sampler's missing context provider, `PEB.Ldr`, `FreeLibrary` and
  `DONT_RESOLVE_DLL_REFERENCES` are all listed in STATUS. None of them silently returns success, except that
  `start_stack_sampler` returns OK when no provider is set, and it says so each time it would sample.

## For the lead / integrator

1. **B1 is a decision:** either isa owns the TLS arrays (the loader registers ctor and dtor with isa, which I
   recommend), or the loader owns them and the host must set a foreign-thread hook. One owner, not two.
2. **On arm64,** plan for every package DLL to be relocated (F3). Rely on the os module for 64 KiB alignment (F4).
3. **First integration run:** once `os/CMakeLists.txt` switches to the real sources, run `loader_pe_stats --os` on
   x86_64. It is the first test of the real reserve, commit and protect paths and of module and seh registration.
