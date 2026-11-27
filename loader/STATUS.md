# loader (S1.4): status

Implements `include/gta6rt/loader.h` (API version 2, additive over v1). Target `gta6rt_loader` (static lib), plus the test
tools `loader_pe_stats` and `loader_selftest`. Builds for x86_64 and arm64 with 0 warnings.

## What is implemented

| area | file | notes |
|---|---|---|
| map / info / sections | `src/pe_map.cpp` | `map_file`, `map_memory`, `info`, `section`, `stats`. Fixed-base reserve through `gta6rt_vm_reserve(FIXED)`, relocation fallback (`ALLOW_RELOCATE`), forced relocation (`FORCE_RELOCATE`), any-address fallback moved up to the 64 KiB granularity. Every RVA taken from the image is range-checked. |
| relocation | `src/pe_map.cpp` | DIR64, HIGHLOW (refused when the moved value leaves 32 bits), ABSOLUTE; any other type, an entry in the headers, or a block that overruns the table fails with `BAD_FORMAT`. |
| imports | `src/pe_imports.cpp` | Regular and delay imports, by name and by ordinal, through a resolver callback. `NOT_FOUND` gives a logging TRAP thunk (cached per `dll!name`). A data import (`kind_guess`) the resolver does not serve gets zeroed host storage and a warning (never a code thunk). Counts in `gta6rt_bind_stats`. `enumerate_imports` walks without binding. |
| exports | `src/pe_map.cpp` | By name and ordinal, forwarders resolved through the package resolver chain (`depth` limit 8). `enumerate_exports`. |
| patch, protections | `src/pe_map.cpp` | `patch_rva` (plain write before finalize, `gta6rt_isa_patch_code` after), `file_offset_to_rva`, `finalize_protections` (per-host-page union of section protections; headers read-only; `flush_icache`). |
| exception table | `src/pe_map.cpp` | `exception_table` validates sorted, non-overlapping, in-bounds, unwind data in image before handing it out. |
| TLS | `src/tls.cpp` | In-process TLS index. Exe gets index 0 (MSVC `/GA` code reads `ThreadLocalStoragePointer[0]` without `_tls_index`), DLLs 1..63 (a DLL never takes 0, in either load order). `_tls_index` is written into the image. Per-thread blocks come from the ctor registered with isa. `tls_info`, `run_tls_callbacks` (via `call_guest`), `notify_thread`. No `gta6rt_tls_host` DLL. The loader owns no per-thread arrays: `tls_assign_index` registers the template with `gta6rt_isa_tls_register_image` (ctor/dtor = block make/free) and unmap unregisters it; isa builds and grows every thread's array (see Review response B1). |
| package directory | `src/package.cpp` | `package_init/load/find/exists/initialize/detach/images/shutdown`, `package_import_resolver`. Case-insensitive lookup in the directory. DLLs placed at their preferred base if free, else on a bump address on the 64 KiB granularity, else anywhere. Imports resolved in order: resolver (shim table) unless the DLL is exclusive, then the package copy's exports (loading it on demand, dependencies first, cycles safe because exports exist before imports are bound), then a trap thunk. Default exclusive list: `vcruntime140*`, `msvcp140*`, `concrt140`, `vccorlib140`, `vcomp140`, `Bink2Scarlett`, `libHttpClient.GDK`, `XCurl`, `amd_fidelityfx_*`. `XboxBvhBuilder.dll` is not exclusive: a shim serves it, the package copy is the fallback. A DLL that fails to read, map or finish loading is remembered with its status (`failed` map, consulted by `load_locked`, so imports and explicit `package_load` do not re-read it) and its imports are trapped. DllMain `lpvReserved` is NULL for `GTA6RT_PKG_LOAD_DYNAMIC` loads and non-NULL otherwise. Each DLL is registered with `gta6rt_module_register` (export callback = loader) and `gta6rt_seh_add_function_table` unless the `NO_*` flags say otherwise. |
| diagnostics | `src/diag.cpp` | `describe_address` ("module+0xrva"), `format_backtrace` (over `gta6rt_seh_backtrace`), `parse_stack_times`, sampler thread started from `GTA6RT_STACKS`. |
| host ops | `src/host_ops.cpp` | `gta6rt_loader_set_host_ops` replaces the os/isa calls the loader makes (vm, file map, thunk, patch, icache, call_guest). Used by the tests, available to the host. |

Not ported on purpose:
- `crt_timed_wait.cpp`: it replaced three `msvcprt.lib` symbols inside leonida's own /MD host binary because Wine 9's
  `msvcp140` had a different `_Mtx_t` layout. The port's host is a libc++ Mach-O; the title's STL runs from the title's own
  `msvcp140.dll`, which this loader maps unchanged. Nothing to port.
- The `gta6rt_tls_host.dll` clone machinery, index-0 move, `/GA` `gs:[58h]` rewriting and existing-thread TEB patching in
  `pe_loader.cpp`: replaced by the in-process TLS index above.
- `LEONIDA_COMPAT_DIR` drop-in modules and the `prefer_host_copy` list (host CRT instead of the bundled one): decision S0.6
  maps the packaged vcruntime/msvcp unchanged.
- CLZERO stub patching and `__XMemAllocationHooks__` export lookup: host policy, not loader. `gta6rt_loader_get_export` and
  `gta6rt_loader_patch_rva` give host/ the hooks to do either.

## Stubbed or limited (by name)

- `gta6rt_loader_set_stack_provider` / sampler: **no provider exists.** os.h/isa.h have no "suspend a thread and read its
  context" primitive, so `GTA6RT_STACKS` prints "no thread-context provider is set, cannot sample". Backtrace formatting is real
  but untested against a real `gta6rt_seh_backtrace` (seh is still a placeholder).
- `gta6rt_loader_package_detach` and `gta6rt_loader_notify_thread` call guest code; they are exercised only with a recording
  `call_guest`, never with real guest code.
- No `FreeLibrary`: package DLLs stay mapped until `package_shutdown`. `gta6rt_loader_unmap` works on any image.
- `PEB.Ldr` / module list is not built (the module table in os.h is the registry).
- (removed: images are now reserved with `GTA6RT_VM_IMAGE`, see Review response B2)
- `DONT_RESOLVE_DLL_REFERENCES` is not representable in `package_load` (flag not defined).
- Delay imports are bound eagerly (as in leonida); the exe has none, the synthetic test covers the path.

## Tests

Build and run (from `port/`; x86_64 binaries one at a time under a kill timer):

```
cmake -S . -B build/loader-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA6RT_MODULES="os;isa;seh;loader"
cmake -S . -B build/loader-arm64  -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA6RT_MODULES="os;isa;seh;loader"
ninja -C build/loader-<arch> -j3 loader_pe_stats loader_selftest
build/loader-<arch>/loader/loader_pe_stats      # real exe + all 16 packaged DLLs; --exe/--package override paths
build/loader-<arch>/loader/loader_selftest      # synthetic PEs
```

Both tools use `loader/tests/posix_ops.cpp`, which supplies mmap-backed memory, synthetic thunk addresses and a recording
`call_guest`, because `gta6rt_os` / `gta6rt_isa` are placeholders at the time of writing; they switch to the real `gta6rt_os`
memory automatically (`--os`/`--posix` force it) once `gta6rt_vm_reserve` stops returning `NOT_IMPLEMENTED`. No guest code runs.

Results (logs in `port/build/loader-run-<arch>-*.log`):

| run | result |
|---|---|
| `loader_pe_stats` x86_64 (Rosetta) | 97 checks passed, 0 failed |
| `loader_pe_stats` arm64 | 95 checks passed, 0 failed (the 2 preferred-base checks are skipped, see below) |
| `loader_selftest` x86_64 | 160 checks passed, 0 failed |
| `loader_selftest` arm64 | 159 checks passed, 0 failed in 1 run; 158/1 in 3 further runs (the 64 KiB base alignment check, review F4, not addressed) |

Measured on the real files (`loader_pe_stats`):

| item | measured |
|---|---|
| exe sections / relocations | 10 / 598,282 (all DIR64, 1,394 ABSOLUTE padding entries not counted) |
| exe imports | 587 in 68 DLLs, 24 by ordinal (23 `WS2_32`, 1 `GameInput`), 0 delay |
| exe exception directory | 299,257 entries, validated sorted/non-overlapping/in bounds |
| exe TLS | template 5,248 bytes, zero-fill 0, align 16, 2 callbacks (RVAs 0x55dd4f4, 0x55e2ebc); index 0 written to `_tls_index` |
| exe entry RVA | 0x56a1ec8; file offset 0x5646540 (`__chkstk`) = RVA 0x5647140 |
| x86_64 exe map | at 0x140000000 on the first try, no relocation; `map_file` 49 to 316 ms (cold page cache) |
| forced relocation | second exe copy at a different base: 598,282 applied in about 50 ms; every DIR64 value equals the fixed-base copy plus the delta (598,282 compared, 0 differ) |
| bind through package resolver | 587 visited: 202 served by package DLLs (MSVCP140 113, VCRUNTIME140 23, VCRUNTIME140_1 1, Bink2Scarlett 23, libHttpClient.GDK 38, amd_fidelityfx_loader 4), 7 by the shim stand-in (XboxBvhBuilder), 378 trapped (61 DLLs with no shim in this test); bind 2 to 7 ms plus DLL loads |
| package DLLs | all 16 in the package map, relocate, bind, protect and initialise in dependency order; 3 have static TLS (libHttpClient.GDK, XboxBvhBuilder, amd_fidelityfx_upscaler: indices 1, 2, 3); exception tables valid for all |
| data imports of the exe | **6, not 3**: the S0.3 notes list 3 (`?_Id_cnt@id@locale@std@@0HA` and two `locale::id`: `?id@?$codecvt@DDU_Mbstatet@@@std@@2V0locale@2@A`, `?id@?$ctype@_W@std@@2V0locale@2@A`), but the exe also imports 3 `MSVCP140` vftables (`??_7ios_base@std@@6B@`, `??_7?$basic_ios@D...`, `??_7?$basic_ios@_W...`). Confirmed against the section flags of the bound `msvcp140.dll` exports: exactly those 6 land in non-executable sections. S0.3's `@@[0-3]` rule would have bound the 3 vftables to code thunks. |
| `kind_guess` accuracy | 0 mismatches over all 1,353 decorated `msvcp140.dll` exports (142 data). Undecorated data exports (`_Nan`, `_LDenorm`, `_FNan`, ...) cannot be told from code by name; the exe imports none of them. |
| per-thread TLS | blocks equal the template, zero-fill zero, independent per thread; 8 threads x 200 alloc/free cycles |

Synthetic cases covered (`loader_selftest`): forced relocation values, HIGHLOW refusal, bind of name/ordinal/data/delay/missing,
zero-storage fallback, exports by name/ordinal, forwarders (named and by ordinal) across DLLs, index-0 policy in both load
orders and late assignment into a live thread array, 17 malformed-image rejections (truncated, wrong machine, bad
`e_lfanew`, entry out of range, section over headers, section past EOF, no relocs when the base is taken, bad reloc type, reloc
into the headers, import table past the image, DLL name outside, unsorted `.pdata`, TLS index in the headers), package
policy (exclusive list vs resolver-first, cycle `aaa <-> bbb`, init order, dynamic `lpvReserved`, thread notify with
`DisableThreadLibraryCalls`, unusable DLL degrades to trap thunks, shutdown unmaps).

## Findings the integrator / other modules need

1. **In the arm64 (native) test runs neither 0x140000000 nor 0x180000000 was available** to a hinted `mmap` (the test binary uses
   the default Mach-O layout; whatever occupies the range was not identified). The loader falls back to
   relocation and works (the 598,282-relocation path is what ran). For arm64 builds the host link layout (open item in
   README) has to reserve that range if the preferred base is wanted; relocation keeps it correct either way.
2. **arm64 16 KiB host pages vs PE 4 KiB sections.** `finalize_protections` asks for the union of the section
   protections on each host page, so a page that holds the end of `.text` and the start of `.data` is requested as
   R|W|X. macOS arm64 refuses that. `gta6rt_vm_protect` on the arm64/FEX backend must drop `GTA6RT_PROT_EXEC` (the
   translator only reads guest code); the test ops do exactly that and the arm64 run passes with it. Under Rosetta (4 KiB
   pages) sections protect exactly and no union is needed.
3. **`gta6rt_vm_reserve(addr = 0)` must return 64 KiB aligned bases** (`GTA6RT_GUEST_ALLOCATION_GRANULARITY`). The loader
   re-places a misaligned image itself, but module handles are bases and guest code may round down by 64 KiB.
4. **Policy for bundled CRT DLLs is exclusive-by-default** (see above). If shims_core wants to serve `memcpy` etc. natively
   from `vcruntime140.dll`, pass an explicit `exclusive_dlls` list without it; mixing a shim and the real DLL for the
   same DLL is allowed only when it is not exclusive.
5. **Boot order that works** (tested here with a recording call_guest): `map_file(exe)` -> `relocate` -> `package_init` ->
   `bind_imports(exe, gta6rt_loader_package_import_resolver)` (this loads and relocates/binds the bundled DLLs it needs)
   -> `patch_rva(__chkstk)` -> `finalize_protections(exe)` -> `tls_assign_index(exe)` (index 0; may also come before binding) ->
   register the exe module and its table -> `package_initialize` (TLS callbacks + DllMain of the DLLs, dependencies first)
   -> `run_tls_callbacks(exe, 1)` -> `run_entry`. Threads are attached with `gta6rt_teb_desc.tls_pointer == NULL` (isa builds the array from the registered images; this also covers threads isa attaches itself). The thread hook then calls `gta6rt_loader_notify_thread(DLL_THREAD_ATTACH)`; on exit `notify_thread(DLL_THREAD_DETACH)` and detach. `run_tls_callbacks(img, 1)` marks an image for thread
   notifications, so the exe is included once its process-attach callbacks have run.
6. `shims_core` `LoadLibrary*` for a package DLL: `package_exists` -> `package_load(name, GTA6RT_PKG_LOAD_DYNAMIC)` ->
   `package_initialize`; `GetProcAddress` goes through the module table (`get_export` callback = `gta6rt_loader_get_export`);
   `DisableThreadLibraryCalls` -> `gta6rt_loader_disable_thread_notifications(base)`.

## Interface changes (all additive)

`include/gta6rt/loader.h`: `GTA6RT_LOADER_API_VERSION` 1 -> 2; now includes `isa.h`; `gta6rt_bind_stats.data_zero_storage`
appended; new: `gta6rt_loader_host_ops` (incl. `tls_register_image` / `tls_unregister_image`) / `set_host_ops`, `section`, `stats`, `enumerate_imports`, `enumerate_exports`,
`guess_import_kind`, `rva_section_flags`, `tls_index`, `notify_thread`, `find_image`, `describe_address`, the
`gta6rt_package_*` / `gta6rt_loader_package_*` group, `disable_thread_notifications`, and the diagnostics group.
No change to `compat/`, `os.h`, `isa.h`, `seh.h`, the top-level `CMakeLists.txt` or any other module.

## Open issues

- Never run against real `gta6rt_os` / `gta6rt_isa` / `gta6rt_seh` (all placeholders when this was built): `--os` mode, module
  and seh registration (`GTA6RT_PKG_NO_*` off), real thunk creation and `call_guest` are untested. The first integration run
  should be `loader_pe_stats --os` once os lands.
- Guest code in the mapped images has never been executed by this loader; S0.3's WinMain run used a different loader.
- Thread-safety of `gta6rt_loader_set_host_ops` after the first map is not guaranteed (call it first).
- `package_load` failure of a DLL during a cycle could leave a dependent bound to freed addresses; not hit by the real files.

## Review response

Source: `REVIEW.md`. Verdict was APPROVED WITH FIXES; B1 to B3 addressed below. Re-run after the fixes: both tools, both arches,
0 warnings; x86_64 run serially under a kill timer (logs `port/build/loader-run-<arch>-<tool>-r2.log`).

- **B1 (TLS model vs isa.h API 2): fixed in code; isa is now the single owner of the arrays.** `tls_assign` allocates a per-image
  record (template pointer, sizes, alignment) and calls `gta6rt_isa_tls_register_image(idx, make_block, free_block, record)`
  through new host ops `tls_register_image` / `tls_unregister_image` (default: the isa functions). `tls_release` (called by
  unmap) unregisters, then frees the record. The loader's own 64-slot arrays and the public
  `gta6rt_loader_tls_alloc_thread` / `gta6rt_loader_tls_free_thread` are **removed** from `loader.h` and `tls.cpp`. Threads must be
  attached with `tls_pointer == NULL`; threads isa attaches itself (foreign threads) then get blocks for every registered image,
  and late-loaded DLLs grow every live thread's array through isa. No foreign-thread hook is required. Tests: `posix_ops.cpp`
  now holds a mock of isa's ownership (`attach_thread`, `detach_thread`, registration with ctor/dtor); `selftest` and `pe_stats`
  attach threads after registration (the foreign-thread case) and check template bytes, zero fill, per-thread independence,
  late-assigned index growing a live array, unmap clearing the slot and unregistering. Not run: the real `gta6rt_isa`
  (this tree builds the loader against the mock ops); first integration run should check
  that isa's ctor/dtor calling convention matches (ctor called with the registering thread's id, on every attached thread).
- **B2 (`is_image`): the claim was wrong; fixed.** All four image `vm_reserve` calls in `pe_map.cpp` now pass
  `GTA6RT_VM_IMAGE` (OR-ed with `GTA6RT_VM_FIXED` where fixed). The STATUS line was removed. Tests: the mock counts reserves with
  and without the flag; `selftest` and `pe_stats` assert zero reserves without it. Not run: a `VirtualQuery` returning `MEM_IMAGE`
  against the real os (needs real `gta6rt_os`).
- **B3 (`failed` set): fixed in code.** `failed` is now a map name -> status. It is filled on read failure, map failure and
  `finish_load` failure, and checked at the top of `load_locked`, which both import resolution and `package_load` go through
  (so explicit loads return the remembered status, e.g. `BAD_FORMAT`). Tests: `broken.dll` bound once, then loaded twice
  explicitly: one file map in total (the log shows a single "broken.dll: not an MZ image"); a chmod-0 `noread.dll` imported three
  times and loaded explicitly: one file map.

Not addressed (non-blocking items from REVIEW.md, still open): F1 thread-notification order, F2 notify outside the package lock /
DllMain FALSE handling, F3 arm64 address-layout note in finding 1 above (REVIEW.md has the corrected facts: 0x180000000 is in
the dyld shared cache on arm64, so every package DLL relocates there), F4 64 KiB re-placement (the cause of the arm64 selftest
alignment check failing in 3 of 4 runs here), F5 small items.
