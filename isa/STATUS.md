# isa/ status (S1.2 + X2/X5)

`gta6rt_isa` = `common/` + one backend: `rosetta/` on x86_64 (real), `fexcore/` on arm64 (compiling stub with the
same interface). Interface: `include/gta6rt/isa.h`, API version 7 (5: this module; 6/7: fexcore). All numbers below
were measured on 2026-11-26 on this machine (macOS 27, Rosetta, load average 7 to 24 from other workers during the
runs). The S1 follow-up (MONITORX/MWAITX, thunks, vshift check) is in section 0.

## Build and test

```sh
cd port
cmake -S . -B build/isa-x86_64 -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA6RT_MODULES="os;isa"
cmake -S . -B build/isa-arm64  -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA6RT_MODULES="os;isa"
ninja -C build/isa-x86_64 -j3 isa_unit_test isa_rosetta_test isa_reexec_test
ninja -C build/isa-arm64  -j3 isa_unit_test
build/isa-arm64/isa/tests/isa_unit_test
isa/tools/run_timeout.sh 120 build/isa-x86_64/isa/tests/isa_unit_test      # one x86_64 process at a time
isa/tools/run_timeout.sh 240 build/isa-x86_64/isa/tests/isa_rosetta_test
isa/tools/run_timeout.sh 60  build/isa-x86_64/isa/tests/isa_reexec_test
```

| test | arch | result | log |
|---|---|---|---|
| `isa_unit_test` | arm64 (native) | PASS, 343 checks | `build/isa-arm64-unit.log` |
| `isa_unit_test` | x86_64 (Rosetta) | PASS, 343 checks | `build/isa-x86_64-unit.log` |
| `isa_rosetta_test` | x86_64 (Rosetta) | PASS, 92,827 checks (re-run after the review fixes) | `build/isa-x86_64-rosetta.log` |
| `isa_rosetta_test` with `ROSETTA_ADVERTISE_AVX=1` | x86_64 (Rosetta) | PASS, 92,827 checks (re-run after the review fixes) | `build/isa-x86_64-rosetta-adv.log` |
| `isa_reexec_test` | x86_64 (Rosetta) | PASS, 3 checks | `build/isa-x86_64-reexec.log` |

A clean rebuild of the isa targets gives 0 warnings on both arches (`build/isa-<arch>.isa-targets.log`).

The Python tools in `tools/` need capstone and pefile; `tools/py.sh` runs them through
`uv run --with capstone==5.0.9 --with pefile python3 -I`.
- `isa_sites.py`: census of the exe's sites → `data/sites_raw.json`.
- `vshift_analyze.py` → `data/vshift_analysis.json`.
- `gen_site_table.py` → `common/title_sites.inc`. Regenerating it from the current json gives a byte-identical file.
- `dll_census.py` → `data/dll_census.json`.
- `vshift_query.py` / `disasm_range.py`: inspection helpers.
- `run_timeout.sh`: background-and-kill wrapper for x86_64 runs. The `os;isa` tree's full `ninja` fails on arm64 in
`os/tests/test_sync.cpp`, which belongs to os and is not isa code, so I built only the isa targets.
The tests read `exe/game_scarlett_master.exe` (sha256 e95020b2…08993); a different file can be passed as `argv[1]`
or `GTA6RT_EXE`.

## 0. S1 follow-up (2026-11-26): MONITORX/MWAITX wait, direct thunks, vshift run-time check

New tests (x86_64 only, built in `rosetta/CMakeLists.txt`):

```sh
isa/tools/run_timeout.sh 120 build/isa-x86_64/isa/rosetta/isa_zen_wait_test
isa/tools/run_timeout.sh 120 build/isa-x86_64/isa/rosetta/isa_vshift_check_test
```

| test | arch | result |
|---|---|---|
| `isa_unit_test` | x86_64 | PASS, 343 checks |
| `isa_rosetta_test` | x86_64 | PASS, 92,827 checks (also PASS with `GTA6RT_ISA_DIRECT_THUNKS=0`) |
| `isa_reexec_test` | x86_64 | PASS, 3 checks |
| `isa_zen_wait_test` | x86_64 | PASS, 27 checks |
| `isa_vshift_check_test` | x86_64 | PASS, 26 checks |
| `isa_unit_test` | arm64 (`build/isa-arm64`, fexcore backend) | 3 of 343 fail, all at `isa_unit_test.cpp:213` (worker StackBase), in `fexcore_backend.cpp`, not in this module's code; arm64 compiles `common/` with my changes (`build/shims_core-arm64`) |

Title runs below: `host/tools/run.sh` with `GTA6RT_BIN=build/os-host-x86_64/gta6rt.*` (my host tree, configured with
`-DCMAKE_EXE_LINKER_FLAGS=-Wl,-pagezero_size,0x10000`), `--trace=0 --watchdog 60 --hidden-window`, x86_64 MoltenVK
1.4.2, Series S profile (the new default here). CPU is the process's cputime from `ps` between t=20 s and t=55 s.

### MONITORX/MWAITX: a real wait instead of NOPs

- **The loops (exe+0x4fb0ad0..0x4fb0e84).** Two waits for the 64-bit value at `[rdi]` to pass `rsi`, with a callback
  in r12. Each does `mov rax, rdi; xor ecx, ecx; xor edx, edx; monitorx` (0x4fb0bd7 and 0x4fb0d21), re-checks `[rdi]`,
  then `mov ecx, 2; xor eax, eax; mwaitx` (0x4fb0c00 and 0x4fb0d52: timer enabled, EBX = max(2, 100.0 /
  [exe+0x9e4f5c4]) TSC ticks, 1000 in every run), re-checks, then `Sleep(0)` (loop 1) or `Sleep(1)` (loop 2).
  `[rdi]` is the D3D12 layer's fence mirror (fence slot 12, the completed value).
- **Patch (`common/x86_patch.cpp`, `kZenWaitSites`).** The 4 windows (10, 10, 7 and 10 bytes, each ending in the
  instruction, no relocation, no branch target except the first byte) become `call stub` + NOPs. The stub replays the
  window's setup bytes, saves every GPR it touches, xmm0-5 and the flags, and calls a NATIVE thunk. The 8 CLWB sites
  stay NOPs (`zen_nop=8`; it was 12 when the MONITORX/MWAITX pairs were NOPs too).
- **Emulation (`rosetta/zen_wait.cpp`).** MONITORX snapshots the 64-byte line at rAX for the thread. MWAITX returns
  when that line differs from the snapshot or its budget ends: a 2 µs spin, then 20 µs steps of
  `os_sync_wait_on_address_with_timeout` on the monitored qword. Budget = the title's EBX ticks (rdtsc measured at
  0.999 ns/tick under Rosetta) clamped to 200 µs..1 ms. `gta6rt_isa_monitor_wake(addr)` wakes it at once. Tunables:
  `GTA6RT_ISA_MWAITX_{SPIN,STEP,MIN,MAX}_US`; `GTA6RT_ISA_MWAITX=nop` restores the NOP windows. Every 10 s it logs
  `mwaitx: N waits ... woken by a write ... at the budget`.
- **Unit test (`isa_zen_wait_test`, a writer thread stores to the monitored value, 300 rounds).** Emulated: wake
  latency p50 14.6 µs, p90 23.9 µs (poll); p50 4.9-7.5 µs, p90 7.8-18.7 µs with `gta6rt_isa_monitor_wake`. The
  waiter used 8.5-9.7% of a core. NOP windows (the S1 state): p50 0.2-0.3 µs, but 99.9-100% of a core.
- **Title, same final binary, 60 s each (`run/logs/f1-mwaitx-nop.log`, `f1-mwaitx-on.log`):**

| | `GTA6RT_ISA_MWAITX=nop` (S1 state) | emulated (default) |
|---|---:|---:|
| import calls | 56,442,264 (941 k/s) | 10,053,847 (168 k/s) |
| `Sleep` calls | 46,571,309 (776 k/s, 82.5% of all) | 150,573 (2.5 k/s, 1.5%) |
| process CPU | 1.41 cores | 1.30 cores |
| the waiting thread (`ps -M` at t=50 s) | 20.6%, 10.5 s CPU so far | 9.6%, 3.3 s CPU so far |
| MWAITX | — | 36,520 waits in 10 s; 1,914 of 74,082 ended by a write, the rest at the budget |
| frames/s | 27.0 | 24.2 (other runs of both binaries: 16 to 26; GPU-bound noise from other workers) |

  The earlier measurement of this task (Series X, `perf-mwaitx-nop-60s.log` / `perf-mwaitx-on-60s.log`): `Sleep`
  18,163,755 → 86,859 in 60 s, calls 27.97 M → 9.99 M, 1.41 → 1.30 cores. The remaining ~1 core is the render
  thread in the D3D12 layer (`ShadowPages::refresh` → `mach_vm_read_overwrite`, sampled), not isa.
- **Not done:** nothing in the D3D12 layer calls `gta6rt_isa_monitor_wake` yet, so title wakes come from the 20 µs
  poll (measured p50 14.6 µs in the unit test). Integrator item below.

### Direct NATIVE thunks (thunk shape)

- A thunk is now `movabs r10, rec; jmp [rip+0]; .quad target` (24 bytes, 16-byte aligned, so the target slot is an
  aligned qword). Under SHARE the target of a NATIVE thunk is the host function itself; before, every call went
  through `gta6rt_rosetta_native_entry` (a `gs:[0x30]` load, a TEB mode compare and a second indirect jump).
- The first thread that has to use SWAP (configured SWAP, or SHARE with TSD slots 11/12 taken) retargets every NATIVE
  slot to the entry with one aligned 8-byte store each before it runs guest code (logged `thunks: a SWAP thread
  appeared`; `isa_rosetta_test`'s SWAP section exercises it). `GTA6RT_ISA_DIRECT_THUNKS=0` keeps the entry for all.
  The title never fell back to SWAP in any log in `run/logs`.
- Measured (`shims_core_import_bench`, thunk minus direct call of the same host function, median of 3 interleaved
  runs): 2.4-7.4 ns before, 0-3.5 ns after (GetLastError 4.4 → 0.8, TlsGetValue 4.4 → 0.8, SRW pair 6.3 → 1.3, strcmp
  3.7 → 0.9). Table in `shims_core/STATUS.md`. At the title's ~170 k host import calls/s this is under 1 ms/s, so it
  is not visible in title CPU.
- `gta6rt_isa_make_native_thunk` (API 6) now exists on rosetta and forwards to `make_thunk` (the signature is not
  needed on x86_64). `gta6rt_isa_capture_guest_context` returns `GTA6RT_E_UNSUPPORTED` with a log line and
  `gta6rt_isa_resume_guest_context` logs and aborts (x86_64 seh does not use them). Requested in
  `isa/fexcore/STATUS.md`; before, they were declared but undefined on x86_64.

### `vpsrlvq`/`vpsllvq`: how "safe" was decided, and a run-time check

- **Decision (unchanged, details in section 5).** 105 sites: `tools/vshift_analyze.py`, per-dword interval abstract
  interpretation over each function's CFG with unknown inputs as TOP; a site is SAFE only if the upper bound of every
  count lane's high dword is 0 (`data/vshift_analysis.json`: 16 read-only constant counts, 89 register counts built by
  `vpmovzxdq`, `vpand` 63, `vpsrlq 32`/`vpblendd` zeroing or a zeroing `vpshufb`). 7 sites the tool could not prove:
  hand proofs in `data/vshift_manual.json` (all SAFE), patched anyway by `GTA6RT_ISA_FIXUP_VSHIFT`.
- **Check mode: `GTA6RT_ISA_VSHIFT_CHECK=1`** (read at fixup time). All 112 sites become `call` to a check stub that
  counts executions, counts executions whose count has any lane >= 2^32 (stricter than the bug condition, which also
  needs low dword < 64) and keeps the last such count, then computes the x86 result as the patch stub does, so
  check mode is also correct at every site. A thread logs `vshift check: N of 112 sites executed, M executions, K with
  a count lane >= 2^32` every 10 s, lists executed sites when the set grows, and logs `VIOLATION at rva ...` with the
  count for any site with K > 0. `gta6rt_isa_vshift_check_report()` logs the same on demand.
- **Cost (`isa_vshift_check_test`):** call + check stub 7.5-10.5 ns against call + raw instruction 4.3-5.6 ns; the
  stub's result is right in all 12,000 runs over the 3 encodings (raw Rosetta was wrong in 9,806 lanes; the stub in 0).
- **Title (`run/logs/f5-vshift-check.log`, 45 s; also `i5-vshift-check-mem.log`, 60 s):** 1 of 112 sites executed on
  this path, rva 0x1645a5b `vpsllvq xmm2, xmm2, [rip+...]`, a read-only constant count {2, 6} (tool verdict SAFE,
  kind mem-const): 92 executions, 0 with a high count dword. The other 111 sites, including the 7 patched ones, were
  not executed in the frame loop the title reaches (no pipelines, nothing drawn). Frames/s with the check on: 25.8.

### Interface (`include/gta6rt/isa.h`, additive; added in the first half of this task)

API 5: report fields `zen_wait` (MONITORX/MWAITX sites calling the emulation, not counted in `zen_nop`) and
`vshift_checked`; `gta6rt_isa_monitor_wake`, `gta6rt_isa_vshift_check_report`. No signature changed.

### For the integrator

1. D3D12 layer: after each store of fence slot 12's completed value, call `gta6rt_isa_monitor_wake(&value)` (8-byte
   aligned; no-op otherwise).
2. Host: print `zen_wait` and `vshift_checked` in the `exe fixups` line (with check mode on it prints `vshift=0`
   because the 7 patch sites are check sites), and call `gta6rt_isa_vshift_check_report()` where the trace is dumped.

## 1. TEB / GS (SHARE, SWAP selectable)

Implemented in `common/isa_core.cpp` and `rosetta/rosetta_backend.cpp`.

- **SHARE (default).** `gta6rt_isa_thread_attach` allocates a TEB with StackBase/StackLimit, Self, ClientId and the
  PEB, and writes TEB, ThreadLocalStoragePointer and PEB into the thread's own TSD slots 6/11/12. Guest
  `gs:[0x30]`, `gs:[0x58]` and `gs:[0x60]` then read them.
  - Before planting, attach checks that slot 6 is 0. If slots 11 or 12 are taken, that thread falls back to SWAP.
  - The thread's TSD array is located once (marker scan of the `pthread` page), so other threads can update its
    slot 11 when TLS grows. If it is not found, attach logs it and the thread re-syncs slot 11 at each
    host→guest call.
- **StackBase is thread-unique.** It comes from the desc or the pthread bounds, and an attach with a StackBase already
  in use returns `GTA6RT_E_ALREADY_EXISTS`. The title's single `gs:[0x30]` site and the packaged DLLs' CRT startup
  lock use `[TEB+8]` as a thread identity.
  - vcruntime140 also checks `TEB+0x10 <= rsp <= TEB+0x08` and calls `__fastfail(0xd)` if that fails, so both
    bounds must cover the real stack. They do: pthread bounds, or the caller's desc.
- **Implicit TLS** (`gta6rt_isa_tls_register_image`/`unregister`). Each thread has an isa-owned
  ThreadLocalStoragePointer array.
  - Registering an image later grows every attached thread's array: a new array is published to TEB+0x58 and to
    TSD slot 11, and the old array is kept until the thread exits, because the thread may be reading it.
  - A loader-supplied `tls_pointer` is honoured until the first growth.
  - Threads that attach later get blocks for every registered image.
- **TlsAlloc slots.** 64 in-TEB slots plus 1024 expansion slots. `slot_free` clears the slot in every thread.
- **Foreign threads.** A thread with no TEB is attached on the spot in `call_guest`/`run_entry`, through the hook if
  one is set, otherwise from pthread bounds. Its TEB is freed by a pthread key destructor at thread exit.
- **SWAP** is selected with `gta6rt_isa_config.teb_mode = GTA6RT_TEB_SWAP`. GS points at the TEB, switched with the
  `thread_set_tsd_base` syscall (0x3000003) on every guest↔host transition, signal handlers included
  (`enter_host_from_signal`/`leave`). `gta6rt_isa_thread_teb_mode()` reports the mode each thread actually uses.
- **`__chkstk`.** `gta6rt_isa_find_chkstk`/`gta6rt_isa_patch_chkstk` match the 78-byte MSVC body and write `ret`.
  `apply_image_fixups` patches the title's copy at RVA 0x5647140 and scans unknown images for it.
  - The 8 packaged DLLs that contain `__chkstk` (Bink2Scarlett, XboxBvhBuilder, amd_fidelityfx_upscaler,
    concrt140, libHttpClient, msvcp140, vccorlib140, vcomp140) match the same signature (`data/dll_census.json`).
  - The test executes the real bytes from the exe under SHARE, unpatched and patched, and checks that rsp, r10, r11
    and rax are preserved. This is the execution the S0.1 review could not run.
- **Tested in `isa_rosetta_test`:**
  - real `gs:` reads on the main thread and 3 pthreads: `[0x58][idx]`, `[0x30]→StackBase`, `[0x60]`, and the doubled
    `65 65` prefix form;
  - TLS index 12 registered while the workers are attached, then read through `gs:[0x58]` on each worker;
  - a foreign thread's TLS freed at exit;
  - a hooked foreign thread;
  - a SWAP thread, including `gs:[0x10]` = StackLimit, and libc float `printf` after guest calls and signals.

## 2. Guest↔host bridge

The bridge is in `rosetta/rosetta_asm.S` and `rosetta_backend.cpp`. Every S0.3 bridge bug has a fix and an
explicit test:

| S0.3 bug | here | test |
|---|---|---|
| #2 arg 5+ read from the r9 home slot | FRAME thunk: `f->rsp` = guest rsp (return address); stack args at rsp+0x28 | `frame6` checks args 5/6 at `st[5]`/`st[6]` |
| #3 xmm saved at a 16-byte stride | 4 contiguous 8-byte `movq` slots | `frame6` checks xmm0-3 |
| #4 float args/returns | handler sees the low 64 bits; returns `xmm0_ret` raw (float = low 32 bits) | `guest_fmix` (mixed int/double, stack double), `frame_float` |
| #5 no shadow space | `call_guest` gives 32 bytes of home space, args at [rsp+0x20+8i], rsp ≡ 0 mod 16 at the call | `guest_sum16` scribbles its home slots, checks rsp+8 alignment, 16 args |
| #6 entry with no caller frame | `run_entry` uses the same framed call | `run_entry` test |
| #7 raw host pointers to the guest | `gta6rt_isa_make_thunk` returns generated code (`movabs r10, rec; jmp [rip+0]; .quad target`, section 0) | all thunk tests |
| #9 global TEB/TSD | per-thread TEB; SWAP keeps the host TSD in each TEB's private area | SWAP thread test |
| #11 rdx across the syscall | the syscall wrapper clobbers rcx/rdx/r11; the NATIVE-SWAP entry parks rcx/rdx/r8-r10 in the TEB | SWAP thunk tests |

The thunk kinds:
- **NATIVE** is a tail jump under SHARE, so the `ms_abi` host function sees the guest frame unchanged; since the
  S1 follow-up the thunk jumps to the host function itself (section 0). Under SWAP it keeps a 64-deep per-thread
  return stack in the TEB.
- **FRAME/TRAP** go through an `ms_abi` dispatcher, so the guest's callee-saved rbx/rbp/rsi/rdi/r12-r15/xmm6-15
  survive host code that clobbers them. `guest_call6` checks all of them after each thunk call.
- **TRAP** logs the import name and calls its handler, or aborts if it has none.

The tests run guest→host→guest re-entry in both modes.

Thunks and patch stubs are written into RWX arenas that never lose execute permission, so creating a thunk while
other threads run thunks on the same page is safe (measured, see "Review response"). `gta6rt_isa_patch_code` on
code that another thread may be running is **not** safe.

Measured round trips (before the S1 follow-up; per-call thunk cost after it is in section 0):
- host→guest→NATIVE thunk: 30 to 36 ns under SHARE, 810 to 851 ns under SWAP;
- host→guest call alone: 26 to 39 ns under SHARE, 404 to 417 ns under SWAP.

## 3. CPUID policy

`common/cpuid_policy.cpp` defines the guest's answer (`gta6rt_isa_cpuid`).
- Advertised: AVX, AVX2, FMA3, F16C, BMI1/2, MOVBE, LZCNT, plus OSXSAVE/XSAVE and XCR0 = 7.
- Hidden: SSE4a, ADX, SHA, VAES, AVX-512*, FSGSBASE, RDSEED, CLWB, VPCLMULQDQ, RDPID, AVX-VNNI, XOP, FMA4, TBM,
  MONITORX, CLZERO, XSAVEC/XSAVES.

The guest's own `cpuid` is answered by Rosetta. There are two ways to make the guest see the policy:
- **Measured (new, API 3): `ROSETTA_ADVERTISE_AVX=1` at process start.** Rosetta's own answer then equals the policy
  for every image: leaf1 ecx 0x7ed8320f (the policy bits plus RDRAND), leaf7 ebx 0x128, ext ecx 0x21, xcr0 7.
  - Without the variable, Rosetta reports no AVX, AVX2, FMA, F16C, BMI or MOVBE, and no OSXSAVE.
  - `setenv` inside a running process has no effect (measured), so `gta6rt_isa_reexec_for_cpuid(argv)` sets the
    variable and re-executes itself once. `isa_reexec_test` shows the same pid with advertised 0x0, then 0xff.
  - `gta6rt_isa_advertised_features()` reports what Rosetta advertises.
- **Image sites:** `GTA6RT_ISA_FIXUP_CPUID` replaces the title's 9 `cpuid` and 2 `xgetbv` sites with `int3`. The
  SIGTRAP handler answers from the policy. Each execution is a signal round trip; the SIGTRAP cost was not timed
  separately, but the SIGILL round trip measured about 20 µs.
  - The packaged DLLs' CRT `cpuid`/`xgetbv` (3+1 each) are not patched. Without the env variable they see Rosetta's
    raw no-AVX answer and take SSE paths, which is safe but inconsistent with the exe.

`gta6rt_isa_cpuid_enforced()` is 1 when Rosetta's answer equals the policy or the title's sites are trapped.

## 4. SSE4a (`insertq`/`extrq`)

The title has 19 sites, all `insertq` with immediates and no `extrq`: 17 inside 11 `.pdata` functions and 2 in gaps.
len+idx ≤ 64 at every site (`data/sites_raw.json`). None of the 16 packaged DLLs has any.

| RVA | bytes | instruction |
|---|---|---|
| 0x27c8f2 | f2440f78cf0800 | insertq xmm9, xmm7, 8, 0 |
| 0x27e9f1 | f2440f78c71800 | insertq xmm8, xmm7, 0x18, 0 |
| 0x2e7ff1 | f20f78c11010 | insertq xmm0, xmm1, 0x10, 0x10 |
| 0x7f1ac9 | f2450f78dc1010 | insertq xmm11, xmm12, 0x10, 0x10 |
| 0x7f1b29 / 0x7f1b38 / 0x7f1b44 | f20f78e31010 / eb / f3 | insertq xmm4/xmm5/xmm6, xmm3, 0x10, 0x10 |
| 0xf75fb3 / 0xf762b7 / 0xf765b5 / 0xf768b0 | f2450f78f51010 | insertq xmm14, xmm13, 0x10, 0x10 |
| 0x269dcb2 | f20f78e51030 | insertq xmm4, xmm5, 0x10, 0x30 |
| 0x32bc150 | f20f78c21010 | insertq xmm0, xmm2, 0x10, 0x10 |
| 0x43d440e / 0x43d491b | f20f78c00808 | insertq xmm0, xmm0, 8, 8 |
| 0x455bac7 | f20f78c90810 | insertq xmm1, xmm1, 8, 0x10 |
| 0x5084782 | f20f78c00818 | insertq xmm0, xmm0, 8, 0x18 |
| 0x546d4a2 (gap) | f20f78c00818 | insertq xmm0, xmm0, 8, 0x18 |
| 0x55b1811 (gap) | f20f78c90818 | insertq xmm1, xmm1, 8, 0x18 |

- **SIGILL emulation (works under Rosetta).** The handler decodes all 4 forms (`[66|F2] [REX] 0F 78/79`, register
  operands only) and reads and writes `uc_mcontext->__fs.__fpu_xmm*`; Rosetta honours the xmm writes on return.
  - The upper ymm halves stay intact: the test loads all 16 ymm registers and compares all 4 lanes.
  - The upper 64 bits of the destination are left unchanged (AMD: undefined; QEMU does the same).
  - Faults outside the registered guest code ranges are passed on to the previous handler.
- **Load-time patch (also implemented).** `gta6rt_isa_patch_sse4a_site` rewrites an imm-form site as `call rel32`
  plus nops into a per-site stub in a ±2 GiB arena. The stub preserves flags and GPRs and uses up to 48 bytes below
  rsp (Win64 has no red zone).
  - `GTA6RT_ISA_FIXUP_SSE4A_PATCH` patches all 19 sites.
  - Register forms are left to the SIGILL path. The title has none.
- **Proof (`isa_rosetta_test`).** Results are compared with an independent software reference, which is itself
  checked against a bit-loop reference in `isa_unit_test` (300k random cases plus every len/idx pair).
  - 1,200 random real encodings of all 4 forms via SIGILL, with random xmm0-15 and random lengths/indices: 0
    mismatches, 1,200 traps counted.
  - The 600 imm-form cases among them, also run patched: 0 mismatches.
  - The 19 title encodings × 50 random inputs × {SIGILL, patched}: 1,900 runs, 0 mismatches.
  - All 19 sites patched in place in a mapped copy of the exe and executed there × 20: 0 bad.
- **Cost per execution:** SIGILL emulation 18.7 / 24.0 / 22.8 µs per trap (3 runs). The patched stub costs 5.8 to
  7.4 ns, against 0.5 to 0.7 ns for a 6-byte nop in the same loop. The patch is the shipping choice for the title;
  SIGILL remains the fallback for unknown code.

## 5. `vpsrlvq`/`vpsllvq` Rosetta count bug

- **Rule, re-measured here.** A lane is wrong iff (c>>32) ≠ 0 and (c & 0xffffffff) < 64. It is then shifted by the
  low dword.
  - 1,360 runs over 34 encodings: the raw Rosetta result was wrong in exactly the 525 runs where the low-32 model
    differs from x86, and matched the model in all 1,360.
- **Analysis (`tools/vshift_analyze.py`, output in `data/vshift_analysis.json`).** It covers all 112 sites: 41
  `vpsrlvq` + 71 `vpsllvq`, in 22 distinct `.pdata` functions. S0.2's "24" summed per-mnemonic counts that overlap.
  - Method: per-dword interval abstract interpretation over reaching definitions in each function's CFG.
  - Unknown inputs, loop-carried values, unmodelled instructions and the function entry are TOP.
  - Read-only constants are used only from non-writable sections and only where no relocation overlaps.
  - I reviewed it for soundness this session and found two defects. The `vpsrad` lower bound was too high (fixed).
    `[rsp+N]` tracking had no aliasing or rsp-adjustment model, so stack slots are now TOP; `--stack` restores the
    old behaviour.
  - Re-running after both changes gave identical verdicts and intervals. All 105 SAFE verdicts rest on the high
    dword's upper bound being 0, never on a lower bound.
  - I checked that every one of the 22 functions decodes to its end, has no exception handler and no external jump
    entries; one function has an indirect jump, which is handled conservatively.
- **SAFE: 105 sites.** Why each holds:
  - 16 read-only constant counts (all ≤ 0x3e);
  - 39 `vpmovzxdq` (high dword 0 by construction);
  - 28 `vpand` with a constant 63 mask (checked by hand at 0x4ef6fff and 0x4ef8983: read-only qword 0x3f at RVA
    0x6168fc8);
  - 12 `vpsrlq …, 0x20` (6) or `vpblendd …, 0xa` of a zeroed register into dwords 1 and 3 (6);
  - 10 `vpshufb` with a constant control that zeroes bytes 4-7 and 12-15.
- **Not proven by the tool: 7 sites.** Reviewed by hand in `data/vshift_manual.json`:
  - SAFE by hand: 0x930363, 0x93038d, 0x9303a6, 0x9303c3. The bit-test loop runs exactly twice, so counts are in
    [0,31].
  - SAFE by hand: 0x517511b, 0x517512e. A copy cycle xmm7→xmm11→xmm7 keeps the zeroing `vpshufb` control constant
    by induction.
  - SAFE by hand: 0x3265efb. The counts start at {0,1,2,3} and go up by 4 for ceil(n/4) iterations, and the shift
    reads the count before the increment, so the largest count is 4·ceil(n/4)−1 ≤ 2^32−1 for every 32-bit n. (The
    first version of this report said this site needs the patch; that was wrong. See "Review response".)
- **Verdict: all 112 sites are provably safe.** `GTA6RT_ISA_FIXUP_VSHIFT` is not needed for correctness on this
  title.
- **Patch (defence in depth).** `gta6rt_isa_patch_vshift_site` / `GTA6RT_ISA_FIXUP_VSHIFT` patch all 7 tool-unproven sites by default,
  as `call rel32` into a stub. The stub runs the original shift with the original count, then ANDs each lane with
  (count >> 6 == 0), so counts ≥ 64 give 0 as on x86. It handles register, `[rsp+…]`, `[base+…]` and `[rip+…]`
  counts and spills 2 free ymm registers below rsp.
  - Tested: 7 title sites, 24 random register encodings and the 3 memory forms × 40 inputs, 0 wrong after the patch.
  - The 7 sites were also patched and executed in place in the exe copy × 20: 0 bad.
  - Cost: 5.1 to 6.5 ns, against 2.3 to 2.7 ns for the raw instruction.

`apply_image_fixups` identifies the title by verifying the bytes at all 143 table sites
(`common/title_sites.inc`, generated by `tools/gen_site_table.py`). The report on the real exe is
`known=1 chkstk=1 sse4a=19 vshift=7 vshift_safe=105 cpuid=9 xgetbv=2 mismatched=0`. A second pass is a no-op.

## Guest DLL census (new, `tools/dll_census.py` → `data/dll_census.json`)

This covers the 16 DLLs in `xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK/`. It is a linear sweep, so counts are upper bounds.
- No SSE4a, no `vpsrlvq`/`vpsllvq`, no rd/wr fs/gs base instructions.
- `gs:` is used only at 0x10 (only inside `__chkstk`), 0x30 and 0x58.
- After `gs:[0x30]` they read only TEB+0x08 and +0x10.
- libHttpClient's `NtCurrentTeb` helper (RVA 0x6f450) has no references: no call, jmp, `lea` or pointer.
- The exe reads no TEB field other than 0x58, 0x30→+0x08 and 0x10, and no image reads the PEB through GS. The TEB
  and PEB size constants in `nt_layout.h` are therefore not exercised by any guest image in the package. They cannot
  be confirmed from the title, and nothing here depends on them beyond the TEB allocation (0x3000, private area at
  +0x1900).

## Stubs (arm64 `fexcore/fexcore_backend.cpp`: compiles, same interface)

These work on arm64 because they are common code, tested by `isa_unit_test`:
- TEB records and implicit TLS, including growth;
- TlsAlloc slots;
- the CPUID policy computation;
- image fixups (byte rewriting) and `__chkstk` find/patch.

Stubbed, with no libFEXCore embedded:
- `gta6rt_isa_call_guest` → `GTA6RT_E_UNSUPPORTED`
- `gta6rt_isa_call_guest4` → 0
- `gta6rt_isa_run_entry` → `GTA6RT_E_UNSUPPORTED`
- `gta6rt_isa_make_thunk` → `GTA6RT_E_UNSUPPORTED`
- `gta6rt_isa_set_guest_fault_handler` → `GTA6RT_E_UNSUPPORTED`
- `gta6rt_isa_set_illegal_instruction_hook` → `GTA6RT_E_UNSUPPORTED`
- `gta6rt_isa_enter_host_from_signal` → 0; `gta6rt_isa_leave_host_to_signal` → no-op
- `gta6rt_isa_host_features` → 0; `gta6rt_isa_advertised_features` → 0; `gta6rt_isa_cpuid_enforced` → 0
- `gta6rt_isa_reexec_for_cpuid` → `GTA6RT_OK` (nothing to do)
- internal `backend_plant` keeps the TEB in a `thread_local` that is not handed to FEX; `backend_unplant` clears it;
  `backend_publish` is a no-op
- internal `backend_cpuid_raw` returns a synthetic SSE4.2-class baseline; `backend_xcr0_raw` → 7

The x86_64 backend has no stubs.

## Interface changes (additive)

- `isa.h` is at API 7: API 4 (Zen fixup flag) and 5 (section 0) by this module, 6 and 7 by `isa/fexcore`. The
  previous run added API 2: TLS images and slots, foreign-thread hook, guest code ranges,
  image fixups and building blocks, trap count, per-thread TEB mode.
- This session added `gta6rt_isa_advertised_features()` and `gta6rt_isa_reexec_for_cpuid(argv)`.
- Review response: the `gta6rt_isa_patch_code` comment in `isa.h` now states that it is not safe on live code. No
  declaration changed.
- Other modules' test stubs of isa functions (`shims_core/tests/env_stub.cpp`) are unaffected.

## Open issues / for the integrator

1. **host/main:** call `gta6rt_isa_reexec_for_cpuid(argv)` first.
   - Then pass `GTA6RT_ISA_FIXUP_ALL & ~GTA6RT_ISA_FIXUP_CPUID` when `gta6rt_isa_advertised_features()` equals the
     policy, which avoids a signal round trip (SIGILL measured about 20 µs) per title `cpuid` execution.
   - How often the title's 9 `cpuid` sites run was not measured.
2. **Signal ownership:** `gta6rt_isa_init` installs SIGILL/SIGTRAP/SIGSEGV/SIGBUS/SIGFPE handlers that chain to the
   previous ones. seh must register through `gta6rt_isa_set_guest_fault_handler` and not install its own handlers
   afterwards.
   - The loader should call `gta6rt_isa_register_guest_code` for every mapped image. With no range registered, any
     fault on a thread with a TEB is treated as a guest fault.
3. Call `gta6rt_isa_apply_image_fixups` after relocation and before any thread can run the image (it rewrites image
   pages through `code_write`, which is not safe on live code). Stubs are placed within ±2 GiB
   of each site, using mmap hints up to ±1.9 GiB.
4. SHARE depends on Apple leaving TSD slots 6/11/12 alone. Attach checks this per thread and falls back to SWAP for
   11/12 or fails for 6. Not tested here: MoltenVK/SDL3 event-loop threads and long sessions.
5. The patched SSE4a and vshift stubs use up to 72 bytes below the guest rsp. That is valid on Win64, but stack
   overflow detection near the guard page is weaker by that amount.
6. All 112 vshift sites are provably safe. `GTA6RT_ISA_FIXUP_VSHIFT` (7 sites) is optional defence in depth and can
   be dropped from the flags without a correctness loss on this title.
7. fexcore: everything that executes guest code waits on embedding libFEXCore (X6 spike).

## Review response (2026-11-26, to `REVIEW.md`)

Both blocking items are fixed. After the fixes I rebuilt the isa targets clean (0 warnings on both arches) and re-ran
every test: `isa_unit_test` arm64 PASS 343, `isa_unit_test` x86_64 PASS 343, `isa_rosetta_test` PASS 92,827 (also with
`ROSETTA_ADVERTISE_AVX=1`: PASS 92,827), `isa_reexec_test` PASS 3. The x86_64 binaries ran one at a time through
`tools/run_timeout.sh`.

### 1. vshift 0x3265efb: the claim was wrong; corrected

I checked it again against the exe bytes (`tools/disasm_range.py`, `.rdata` read with pefile), and the reviewer is
right:
- n = `dword [rsi+0x10c]`, zero-extended, with n ≥ 1. rcx = ceil(n/4)·16, and rdx steps 0x10 until it equals rcx,
  so the loop runs ceil(n/4) times.
- The counts start at {0,1,2,3} (`.rdata` 0x5f43800 = `00 01 02 03`) and go up by 4 (`.rdata` 0x5fc1608 = `04 00…`).
- The shift at 0x3265efb comes before the `vpaddq` at 0x3265f00.
- In the function 0x3261fd0..0x326605b, the only branch into the loop region is the back edge at 0x3265f0f.

So the largest count is 4·ceil(n/4)−1 ≤ 2^32−1, its high dword is always 0, and **all 112 sites are provably safe**.
- Corrected: section 5, open issue 6, `data/vshift_manual.json` (verdict SAFE with this proof; all 7 manual verdicts
  are now SAFE).
- The 7-site patch stays enabled as defence in depth, so `title_sites.inc` and the code are unchanged.

### 2. `code_write` dropped execute permission while writing: fixed for thunks and stubs, documented for the rest

The change is in `common/x86_patch.cpp`:
- **Arenas are mapped once and never lose execute permission.**
  - On x86_64 an arena is mapped RWX.
  - If RWX is refused, and always on arm64, an arena is an RX view plus an RW `mach_vm_remap` alias of the same pages.
  - `arena_write` is a plain `memcpy` plus an icache flush, with no `mprotect`.
  - `gta6rt_isa_make_thunk` now writes through `arena_write`. `code_write` sends arena addresses there too, so the
    SSE4a and vshift stubs and the call rewrites inside arenas also go through it.
- **Other pages** (image `.text`) are still made writable for the duration of the write: RWX where allowed, else RW.
  This is documented as load-time only, in `isa.h` (`gta6rt_isa_patch_code`) and `x86_patch.h`.

Rosetta behaviour I measured with standalone x86_64 probes (one process at a time; the probe sources are in the
session scratchpad, not in the tree):

| scenario | result |
|---|---|
| plain `memcpy` into an RWX page while another thread executes code on it | 540,727 writes against 162.9 M calls: no fault |
| `mprotect(RWX)` before each such write | the **writer** gets SIGBUS (err 6) |
| old path: RW, write, RX on that page | the **executing** thread gets SIGBUS (err 0xf), as the review found |
| rewriting code that has already run, through the RWX view | 0 of 200 stale |
| rewriting code that has already run, through an RW alias | **196 of 200 stale**: Rosetta does not see writes through the alias |
| writing fresh bytes through the alias | 0 of 200 bad |

So x86_64 uses RWX. The alias is correct only for bytes that have not run yet. Arena bytes are written once in
production, but under the alias fallback, `patch_code` or `patch_chkstk` on arena code that has already run would
not take effect.

**I also found a new Rosetta quirk while testing.**
- Once Rosetta has raised SIGILL for an SSE4a instruction (`66 0f 78 …`) on a page, every later write to that RWX
  page faults (SIGBUS, err 6). It stays that way until `mprotect` is applied to the page again.
- Only that 4 KiB page is affected. A `ud2` does not cause it.
- Re-asserting RWX before every write is ruled out (second row of the table), so the isa fault handler handles it
  instead: on a write fault inside an RWX arena it re-asserts RWX on that page and retries the write
  (`arena_retry_write_fault`, signal-safe, lock-free table).
- In `isa_rosetta_test` this fired 11 times, because the test places SSE4a code in the arena. Production arenas hold
  only thunks and stubs, which never raise SIGILL.
- Related: under Rosetta, `mach_vm_region` reports RX for an RWX page whose code has been translated. So
  `code_write`'s non-arena path restores such a page to RX. That only matters if the loader maps `.text` RWX; it
  should map it RX and patch before execution.

**New test: `test_concurrent_code_writes` in `isa_rosetta_test`.** Results (default run; with
`ROSETTA_ADVERTISE_AVX=1`, 725,828 writes against 77.4 M calls):
- **Shared page.** One thread calls a function at the start of an arena page in a loop. Meanwhile this thread
  rewrites a scratch block on the same page and creates 2,000 NATIVE thunks, 96 of which land on that page.
  - 985,602 writes in 1.5 s against 186.9 M calls: 0 wrong results and 0 failures. All 2,000 thunks return the right
    value afterwards.
  - Cost: the executing thread slowed from 557 M to 125 M calls/s while the writer ran, because Rosetta re-translates
    the page.
- **Live non-arena page** (forked children, 3 s each, characterisation only). `gta6rt_isa_patch_code`
  (RX→RWX→RX) and the old RX→RW→RX path both get the child killed by SIGBUS. That is why `patch_code` is documented
  as unsafe on live code. The old-path crash is checked as a positive control.

New internal functions (`x86_patch.h`): `arena_write(at, ptr, n)`, `arena_retry_write_fault`,
`arena_fault_retries`, `arena_is_rwx`.

**Integrator note:** a hardened-runtime build needs `com.apple.security.cs.allow-unsigned-executable-memory` for RWX
arenas. Without it the alias fallback is used and logged. Image mapping needs the same entitlement anyway.

### Non-blocking items

- 8: `tools/__pycache__/` is removed.
- 3 to 7, 9 and the rest of 8 are unchanged and still open:
  - no `sigaltstack`;
  - `call_guest4` returns 0 on failure;
  - SWAP return-stack depth after non-local exits;
  - the re-exec env variables are inherited by children;
  - TLS ctor/dtor under `g_mu`;
  - the variable check count (two runs this session both gave 92,827, which is not proof that it is fixed);
  - the `ill_hook` ternary;
  - `cpuid_enforced` equality.


## Review response (round 3, `rosetta/REVIEW-round3-perf-policy.md`)

The two blocking findings (page zero on x86_64, no address space below 4 GiB on arm64) are fixed in os/ and
shims_core/, not in isa/; see `shims_core/STATUS.md` "Review response (round 3)". Effect here: isa's x86_64 test
programs link `gta6rt_os` and now get a 64 KiB `__PAGEZERO`; re-run with it: `isa_unit_test` 343, `isa_rosetta_test`
92,827, `isa_reexec_test` 3, `isa_vshift_check_test` 26, `isa_zen_wait_test` 27, all PASS. No isa source changed.
