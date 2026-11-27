# compat: the Windows header set for the macOS host

`compat/include` is a self-contained replacement for the Windows SDK headers leonida's runtime includes. It is
not the SDK and not the DirectX-Headers WSL stubs (those define `WCHAR` as 4-byte `wchar_t` and
`HWND`/`HDC` as `int`, which is wrong for the guest). CMake targets:

| target | what |
|---|---|
| `gta6rt_wincompat` | INTERFACE: `compat/include` as a system dir, `UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX` |
| `gta6rt_dxheaders` | INTERFACE: wincompat + DirectX-Headers `include`, `include/directx`, `include/dxguids` (compat first) |
| `gta6rt_dxguids` | STATIC: DirectX-Headers `src/dxguids.cpp` (`IID_*` definitions) |
| `gta6rt_vulkan_headers` | INTERFACE: Vulkan-Headers 1.4.363 |
| `compat_abi_test` | executable: layout `static_assert`s + a run-time check (UTF-8/16, uuids, an ms_abi call with stack and xmm args) |

## Decisions (fixed; change only with the lead)

- **LLP64 types.** `LONG`/`ULONG`/`DWORD`/`BOOL` are 32-bit (`int`/`unsigned int`), `LONGLONG`/`DWORD64` are
  64-bit, `ULONG_PTR`/`SIZE_T` are the host's `uintptr_t`/`size_t`. Never use host `long` where Windows code
  meant `LONG`: `long` is 8 bytes here (example: `guarded_copy.h` casts to `volatile long*` for
  `_InterlockedExchange`; that must become `LONG`).
- **`WCHAR` = `char16_t`** (UTF-16, 2 bytes), `TEXT("x")`/`_T("x")` = `u"x"` under `UNICODE`. `L"..."` is
  4-byte `wchar_t` and does not convert; ported code uses `u"..."` and `std::u16string`. No `-fshort-wchar`.
- **Calling convention.** `WINAPI`, `CALLBACK`, `APIENTRY`, `NTAPI`, `STDMETHODCALLTYPE`, `STDAPICALLTYPE`,
  `__stdcall`, `__cdecl`, `CDECL`, `__fastcall` all expand to `GTA6RT_WINABI`:
  - x86_64: `__attribute__((ms_abi))`. The guest runs under Rosetta and calls host exports directly with the
    Windows x64 convention. Measured: `compat_abi_test` calls a 7-argument `WINAPI` function through a
    pointer and gets the right sum on x86_64 under Rosetta; its disassembly reads args from
    `ecx, rdx, xmm2, r9d, [rbp+0x30..0x40]` (Win64 layout).
  - arm64: empty. The guest never calls host code natively; isa/fexcore marshals the guest register file
    through `GTA6RT_THUNK_FRAME` thunks (include/gta6rt/isa.h). Shims that must work on arm64 therefore need
    a frame adapter; `GTA6RT_THUNK_NATIVE` is an x86_64 fast path only.
  - Consequences on x86_64: captureless lambdas do **not** convert to `WINAPI`/`CALLBACK` function pointers
    (write a named `WINAPI` function, or `[](LPVOID p) __attribute__((ms_abi)) -> DWORD {...}`); template
    specialisations on `R(*)(A...)` do not match `ms_abi` function types (seen in `xgameruntime.cpp`
    `Method<&XAsyncGetStatus>`); `qsort`-style callbacks passed to libc must not be declared `__cdecl`.
- **`_WIN32` is never defined** (the config header errors if it is). Code paths keyed on `_WIN32` must be
  ported, not enabled.
- **`__declspec(x)`** maps to `GTA6RT_DECLSPEC_x` (dllexport, dllimport, noinline, noreturn, nothrow,
  novtable, selectany, thread, naked, align(n), uuid(...), deprecated, restrict, noalias). An unknown
  specifier fails to compile on purpose. `__declspec(allocate(".CRT$X*"))` has no Mach-O meaning: port to
  `__attribute__((constructor))`. No `-fms-extensions`, no `-fdeclspec`.
- **SAL** expands to nothing (`sal.h`).
- **No GS intrinsics.** `__readgsqword` and friends are absent on purpose; use `gta6rt_isa_current_teb()`.
  `__cpuid`/`__cpuidex`/`_xgetbv`/`__rdtsc` exist on x86_64 only (host CPU). `intrin.h` includes clang's
  `<cpuid.h>` and replaces its 5-argument `__cpuid` macro with the MSVC 2-argument function.
- **Objective-C.** `BOOL` here is `int`; Objective-C's is `bool`/`signed char`, and `interface` is a macro.
  `.m`/`.mm` files must not include `windows.h`; keep Cocoa code behind a C interface (host/ and d3d12_vulkan/).
- **D3D12 struct-returning methods.** Without `_WIN32`, DirectX-Headers declare 21 methods in `d3d12.h` and
  6 in `d3d12video.h` (`GetDesc`, `GetCPUDescriptorHandleForHeapStart`, ...) with natural struct return.
  MSVC returns them through a hidden pointer. Guest-facing implementations (S1.6) must use the RetVal form.

## Layout and rules for adding to it

- One header per Windows SDK header name, flat in `compat/include` (`synchapi.h`, `fileapi.h`, ...).
  `windows.h` includes the common set. Internal pieces live in `compat/include/gta6rt/` (`wincompat_config.h`,
  `winnt_*.h`, `msvc_crt.h`, `dx_extra_uuids.h`).
- Put a declaration in the header where the SDK has it. Function declarations carry a comment naming the
  implementing module (mostly os/; Rtl* unwind functions are seh/).
- Struct layouts must be the Windows x64 layout. Add a `SZ()`/`OFF()` line to `tests/abi_test.cpp` for every
  struct you add that crosses the guest boundary.
- Never create a header whose name exists in the macOS SDK `usr/include` (case-insensitive): `compat/include`
  is searched before the SDK and would shadow it.
- Additive only: do not change an existing typedef, constant value or struct layout without the lead.
- Both trees must still build (`ninja -C build/<module>-x86_64` and `-arm64`), and `compat_abi_test` must pass.
- Header-only helpers are fine for CRT extensions (`msvc_crt.h`); anything with state belongs in os/.

## Measured coverage (2026-11-25, `tools/compat_census.sh`)

Syntax-only compile of all 44 `leonida/runtime/{src,shims}/*.cpp` against this header set, unchanged sources:

| | x86_64 | arm64 |
|---|---:|---:|
| syntax-clean TUs | 11 / 44 | 12 / 44 |
| scout census baseline (no compat set) | 4 / 44 | 4 / 44 |

Clean on both: `src/{bcpack,host_cpu,root_signature,tls_host,vulkan_aftermath}.cpp`,
`shims/{d3d12_xs,d3d12_xs_pso_stream,d3d12_xs_rootsig,gameinput_no_sdl,xbox_bvh_builder,xgameruntime_user}.cpp`;
`shims/xgameruntime_store.cpp` is clean on arm64 only (lambda to `CALLBACK` pointer on x86_64, see above).
Fatal errors stop a TU, so the remaining counts are lower bounds. What the remaining first errors are:

| kind | examples | owner |
|---|---|---|
| headers not written yet | `bcrypt.h`, `propidl.h`, `winhttp.h`, `winsock2.h`, `winioctl.h`, `mmdeviceapi.h`, `dxgi1_6.h`, `xthreads.h` | shims_core, shims_gdk, d3d12_vulkan, loader |
| `wchar_t`/`L"..."` meets `LPCWSTR` | `startup_trace.h:140`, `d3d12_com_object.h:137`, `shader_directory.h:24`, `LoadLibraryExW(L"...")` | every porter |
| host `long` vs `LONG` | `guarded_copy.h:47` `_InterlockedExchange(volatile long*)` | loader / shims |
| lambdas and templates vs `ms_abi` (x86_64) | `main.cpp:59` `CreateThread`, `xgameruntime.cpp:206`, `xgameruntime_store.cpp:146` | host, shims_gdk |
| Win32 windowing/input (not declared on purpose) | `GetWindowThreadProcessId`, `WM_SYSCOMMAND`, `GetAsyncKeyState` | host, d3d12_vulkan, shims_gdk |

Per-TU logs: rerun `tools/compat_census.sh <out_dir>`.
