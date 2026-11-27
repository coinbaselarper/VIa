# Module interfaces

The headers in `include/gta6rt/` let the S1 modules be built in parallel. They are plain C (C linkage, no
`windows.h`), small, and versioned (`GTA6RT_<MOD>_API_VERSION`). Each module's placeholder implements every
function of its header with `GTA6RT_E_NOT_IMPLEMENTED`, so the whole tree links from day one; a worker replaces
its placeholder file with real code. Status codes are in `abi.h` (0 = OK, negative = error).

| header | implemented by | consumed by | task |
|---|---|---|---|
| `abi.h` | os (`gta6rt_status_name`) | everyone | — |
| `nt_layout.h` | constants only | isa, loader, os | S1.2 |
| `x64_state.h` | types only (`CONTEXT` / `EXCEPTION_RECORD` layouts, asserted in compat/tests) | isa, seh | S1.2/S1.3 |
| `os.h` | `os/` → `gta6rt_os` | isa, seh, loader, shims_core, shims_gdk, d3d12_vulkan, host | S1.1 |
| `isa.h` | `isa/rosetta` (x86_64) or `isa/fexcore` (arm64) → `gta6rt_isa` | loader, seh, shims_core, host | S1.2, X2, X5 |
| `seh.h` | `seh/` → `gta6rt_seh` | loader (tables), shims_core (RaiseException etc.), host | S1.3 |
| `loader.h` | `loader/` → `gta6rt_loader` | host, shims_core (LoadLibrary of package DLLs) | S1.4 |
| `shims.h` | `shims_core/` (table, resolver); `shims_gdk/` and `d3d12_vulkan/` (their `_register`) | host, loader (via resolver callback) | S1.5a/b, S1.6 |

The Win32-named functions declared in `compat/include` (kernel32 surface) are implemented by `os/` unless the
header says otherwise (`Rtl*` unwind/raise and vectored handlers: `seh/`; COM `Co*`: `shims_core/`). They are
what leonida's host code calls and what shims_core binds the guest's kernel32 imports to.

## Dependency direction (link graph)

```
compat, include ─┬─ os ── isa ─┬─ seh
                 │             ├─ loader
                 │             └─ shims_core ── shims_gdk
                 ├─ portable ─────────────────── d3d12_vulkan (also os, shims_core)
                 └─ host: everything (shims archives with -force_load)
```

No cycles: os never calls isa/seh/loader directly. Upward calls go through registered callbacks
(`gta6rt_thread_set_hooks`, `gta6rt_isa_set_guest_fault_handler`, the import resolver).

## os.h (S1.1)

- **vm**: `gta6rt_vm_reserve/commit/decommit/protect/release/query`, `gta6rt_vm_alias` (XMem physical pages,
  `mach_vm_remap`), write watch. Keeps a region table so release needs only the base (S0.3: the title makes
  2x24 GiB reserve-only windows plus ~4.5 GiB of large-page regions in static constructors). Commit inside a
  reservation must not wipe committed data (S0.3 bug #8).
- **threads**: create (optionally suspended), adopt the current thread, join, id, stack bounds, per-thread
  user slots (0 = isa TEB, 1 = loader TLS block), name, priority. `gta6rt_thread_set_hooks` registers
  on_start/on_exit that run on every guest-capable thread.
- **sync**: events, semaphores, mutexes as `gta6rt_waitable`, threads are waitable, wait any/all with ns
  timeouts, address waits. These back `CreateEventW`/`WaitForMultipleObjects`/`WaitOnAddress`.
- **handles**: the Win32 `HANDLE` table (small integers, never pointers).
- **files**: open/pread/pwrite/size/truncate/sync, read-only file mapping (for the 120 MB image).
- **time/RNG/env/last error/log**: QPC at 10 MHz, FILETIME, `arc4random`-class bytes, UTF-8 env, per-thread
  last error, `gta6rt_log` and an async-signal-safe logger.
- **module table**: register/find (name, address, base)/enumerate modules with an export callback; backs
  `GetModuleHandle*`, `GetProcAddress`, `LoadLibrary*`.

## isa.h (S1.2, X2/X5) — the only home for Rosetta/FEX-specific code

- **Backend**: `gta6rt_isa_backend_id/name`, `gta6rt_isa_init(teb_mode, cpuid_features)`.
- **TEB (S0.6: SHARE)**: `gta6rt_isa_set_peb`, `gta6rt_isa_thread_attach(stack, tls_pointer, peb, ids)`,
  `detach`, `current_teb`, `set_tls_pointer`. SHARE plants TEB/TLS/PEB in TSD slots 6/11/12 so
  `gs:[0x30]`/`[0x58]`/`[0x60]` work; SWAP stays selectable. `gs:[0x00]` is not available under SHARE.
- **Signals**: `gta6rt_isa_enter_host_from_signal`/`leave` around every host signal handler (S0.1).
- **host→guest**: `gta6rt_isa_call_guest` (16 args, float mask, xmm0 return; 32-byte shadow space, aligned
  stack: S0.3 bugs #5/#6), `call_guest4`, `run_entry`.
- **guest→host**: `gta6rt_isa_make_thunk` with kind NATIVE (x86_64: `ms_abi` function), FRAME
  (`gta6rt_guest_frame` handler, both arches; stack args at `rsp+0x28`) or TRAP (logged unresolved import).
- **CPUID**: policy bitmask, `GTA6RT_CPUID_DEFAULT` = AVX, AVX2, FMA3, F16C, BMI1/2, MOVBE, LZCNT (S0.6);
  never SSE4a, ADX, SHA, VAES, AVX-512. `gta6rt_isa_cpuid_enforced()` reports whether the backend can make
  the guest see it (under Rosetta the guest's `cpuid` is answered by Rosetta).
- **Faults**: `gta6rt_isa_set_guest_fault_handler` (seh registers; gets `EXCEPTION_RECORD` + `CONTEXT`
  layouts) and `gta6rt_isa_set_illegal_instruction_hook` (tried first; S0.6 SSE4a `insertq`/`extrq`).
- **Code patching**: `gta6rt_isa_patch_code`, `flush_icache` (`__chkstk` → `ret`, S0.6).
- `isa/common/host_cpu.{cpp,h}` (leonida's AVX preflight, with the `ROSETTA_ADVERTISE_AVX` hint) moved here
  from gta6rt_portable per the scout review (item 8).

## seh.h (S1.3)

Policy LOG_AND_ABORT first, DISPATCH later (S0.6). Function tables from `.pdata` (`add/remove/lookup`),
`gta6rt_seh_virtual_unwind` (RtlVirtualUnwind), `backtrace`, `dispatch` (called from the isa fault handler),
`raise` (RaiseException path), `unwind` (RtlUnwindEx), vectored handlers and the unhandled filter as guest
addresses. The exe's `.pdata` is v1 UNWIND_INFO only; vcruntime140/XCurl/libHttpClient contain v2.

## loader.h (S1.4)

Explicit steps so host/ controls ordering and logging: `map_file`/`map_memory` → `relocate` →
`bind_imports(resolver)` → `patch_rva` → `finalize_protections`. Plus the exception table for seh, static TLS
(`tls_info`, process-wide `tls_assign_index`, per-thread `tls_alloc_thread` → `gta6rt_teb_desc.tls_pointer`,
`run_tls_callbacks`), exports for package DLLs, and `entry`. The resolver returns NOT_FOUND to get a logging
TRAP thunk; returning a do-nothing stub is not allowed (S0.3: zero-returning stubs send the title down wrong
paths). `gta6rt_import_ref.kind_guess` flags MSVC data imports (S0.3 bug #12).

## shims.h (S1.5, S1.6)

`gta6rt_shims_register_dll(name, exports[])` with export kinds CODE_NATIVE (x86_64 fast path), CODE_FRAME
(both arches), DATA; `gta6rt_shims_import_resolver` matches `gta6rt_import_resolver`.
`gta6rt_shims_gdk_register()` and `gta6rt_d3d12_vulkan_register()` are called by host/ after
`gta6rt_shims_init()`.

## Boot sequence the interfaces assume (host/, integrator)

1. `gta6rt_isa_init`; `gta6rt_seh_init(LOG_AND_ABORT)`; `gta6rt_shims_init` + the two `_register` calls.
2. `gta6rt_loader_map_file(exe)` → `relocate` → `bind_imports(gta6rt_shims_import_resolver)` →
   `patch_rva(__chkstk)` → `finalize_protections`; register the module with `gta6rt_module_register` and its
   table with `gta6rt_seh_add_function_table`; `tls_assign_index`.
3. Build the PEB, `gta6rt_isa_set_peb`; `gta6rt_thread_set_hooks` with on_start = `tls_alloc_thread` +
   `isa_thread_attach`, on_exit = detach + `tls_free_thread`.
4. `gta6rt_thread_adopt_current(guest=1)`, `run_tls_callbacks(DLL_PROCESS_ATTACH)`, `gta6rt_isa_run_entry`.

## Changing an interface

Additions (new functions, new enum values at the end) are fine; bump the API version. Changing or removing an
existing signature or struct field needs the lead and every consumer listed above. Keep headers C, keep
`windows.h` out of them.
