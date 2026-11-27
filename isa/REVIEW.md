# Independent review of port/isa (S1.2 + Rosetta gaps)

Reviewer run: 2026-11-26. Load average on the machine was 11 to 14 during the runs. Nothing outside this file was
changed except two private build trees (`port/build/review-isa-arm64`, `port/build/review-isa-x86_64`) and scratch
files in the session scratchpad.

Verdict: APPROVED WITH FIXES. Two items are listed as blocking: one false claim (a vshift site reported as
"genuinely needs the patch" is in fact provably safe), and one verified crash mechanism in the code-write path
that the loader will use at runtime. Everything else I checked holds.

## What I rebuilt and re-ran

| item | result |
|---|---|
| clean configure + build of isa targets, arm64 and x86_64 (`-j3`) | 0 warnings on both |
| `isa_unit_test` arm64 native | PASS, 343 checks |
| `isa_unit_test` x86_64 Rosetta | PASS, 343 checks |
| `isa_rosetta_test` x86_64 Rosetta | PASS, 90,820 checks |
| `isa_rosetta_test` with `ROSETTA_ADVERTISE_AVX=1` | PASS, 90,819 checks (the report says 90,820; some check is counted a timing-dependent number of times, see nits) |
| `isa_reexec_test` | PASS, 3 checks, same pid, advertised 0x0 then 0xff |

x86_64 runs were launched one at a time through `isa/tools/run_timeout.sh`. No hang.

Measured here, all under heavy load, so only the order of magnitude is comparable with STATUS.md:
SHARE thunk round trip 35 to 48 ns, SWAP 888 to 1203 ns; SIGILL emulation 44 to 84 us per trap (STATUS: 19 to 24 us);
patched insertq stub 9.5 to 10.2 ns; patched vshift 7.1 to 7.8 ns against 2.9 to 3.0 ns raw. The vshift rule is
reproduced: 525 of 1,360 runs had a triggering lane, raw Rosetta was wrong in exactly those, the low-32-bit model
matched in all, and the patched version was wrong in 0. With the variable set, Rosetta's cpuid answer equals the
policy for leaves 0, 1, 7, 0xd, 0x80000001 (leaf 1 ecx 0x7ed8320f, leaf 7 ebx 0x128, ext ecx 0x21).

## Independent checks of the report's factual claims

- **SSE4a census.** A byte-pattern scan of `.text` for `f2/66 [rex] 0f 78/79 modrm` finds exactly 19 hits, all
  `f2 0f 78` (imm-form insertq), at the same 19 RVAs as the STATUS table, and none of `f2 0f 79`, `66 0f 78`,
  `66 0f 79`. No extrq. Confirmed.
- **GS offsets.** A byte-pattern scan of the exe finds `gs:[0x58]` 53,246 times and `gs:[0x30]` and `gs:[0x10]` once
  each (the other hits are random disp32 noise). The same scan over the 16 packaged DLLs reproduces every per-DLL
  count in `data/dll_census.json`. Confirmed.
- **Fault decoding under Rosetta.** I checked in a throwaway x86_64 program: a write fault at 0x10 gives
  `__es.__err` 0x6, a read fault 0x4 (so `params[0]` = `err & 2` is right), and `int 0x29` gives SIGSEGV with
  trapno 0xd, err 0x14a and rip at the `cd 29`, which is what the `__fastfail` branch in `exception_code` tests.
  Neither the write flag nor `__fastfail` is exercised by the project's tests.
- **TSD slots.** On this OS the first dynamic `pthread_key_create` returns 258 to 259 (checked arm64 and x86_64), so
  slots 11 and 12 are not handed out as pthread keys here. This lowers risk #4 in the report; it is still untested
  with MoltenVK/SDL threads.
- **Bridge code read line by line** (`rosetta_asm.S`, `rosetta_backend.cpp`). call-guest: rsp is 16-byte aligned at
  the `call`, 32 bytes of home space, arg 5 at `[rsp+0x20]`, args 1 to 4 duplicated in GPR and xmm, callee-saved
  rbx/r12-r14 saved by the bridge, rbp-relative epilogue is correct. FRAME entry: callee sees rsp = 8 mod 16, frame
  fields land at the offsets `gta6rt_guest_frame` declares, `f->rsp` points at the return address so stack args are at
  +0x28. NATIVE entry: tail jump under SHARE; under SWAP the pop/call pair keeps alignment and the home space, and
  rsi/rdi/rbx are parked because the syscall wrapper uses them. The generated thunk is 23 bytes and the encoding is
  right. LLP64 types are fixed-width throughout; there are no wide strings in this module (only `char*` debug names).
- **S0.3 bug table.** Every row of the STATUS table maps to code I read and to a test that I saw run (frame6 with args
  5/6, 4 contiguous xmm slots, float in/out, shadow-space scribble, framed entry, generated thunks, SWAP, re-entry).
- **Stubs are listed honestly.** Every fexcore function that executes guest code is named in STATUS.md and returns
  `GTA6RT_E_UNSUPPORTED`, or 0 for `call_guest4`/`host_features`/`advertised_features`/`cpuid_enforced`. I found no
  unlisted stub. `thread_attach` succeeding on arm64 is listed (TEB is recorded, not handed to FEX).
- **SSE4a semantics.** Decoder field mapping (`dst`/`src` per form, len/idx from the two immediates, 0 length = 64,
  register forms take len/idx from the source's high qword bits 5:0 and 13:8) matches the AMD APM. The stub
  preserves flags and rax/rcx/rdx and restores the rest. Verified by the run above.
- **vshift stub.** Encoding, temp-register choice, rsp-relative memory-count adjustment (+0x48), rip-relative reach
  check, and the `count>>6 == 0` mask all read correctly and match the test results.

## Vshift analysis: review

The analyzer (`tools/vshift_analyze.py`) is sound on everything I traced. Calls are treated as clobbering every vector
register, the function entry and loop-carried values are TOP, non-const memory is TOP, read-only constants are used
only from non-writable sections without relocations, and `vpand`/`vpshufb`/`vpmovzx`/`vpsrlq`/`vpblendd` transfer
functions are correct. The 22-function count is right (S0.2's "at most 24" was explicitly per-mnemonic and
overlapping). I hand-checked the loops behind the 4 sites at 0x930363..0x9303c3 (exactly two iterations, r8 0 then
0x10, counts at most 19 + 12) and the xmm7/xmm11 copy cycle behind 0x517511b/0x517512e (xmm7 is overwritten at
0x51751bd and 0x51751e6 but restored from xmm11 at 0x517522f, the only path to the single back edge 0x5175255);
both proofs hold.

**The one claim that is wrong: 0x3265efb.** The report, STATUS and `vshift_manual.json` say it "reaches 2^32 only if
dword [rsi+0x10c] >= 0xfffffffd" and is the only site that strictly needs the patch. That is false. The disassembly:

```
3265e6a mov edx, [rsi+0x10c]        ; n, zero-extended, n >= 1
3265e79 lea rcx, [rdx+3]
3265eb0 shr rcx, 2 ; shl rcx, 4     ; rcx = ceil(n/4)*16
3265ebf vpmovsxbq ymm0, [0x5f43800] ; 00 01 02 03 -> lanes {0,1,2,3}
3265efb vpsllvq ymm10, ymm6, ymm0   ; the site
3265f00 vpaddq ymm0, ymm0, ymm7     ; ymm7 = broadcast 4 (0x5fc1608 = 04 00 ..)
3265eea add rdx, 0x10 ... cmp rcx, rdx ; jne 3265ecc   ; ceil(n/4) iterations
```

The shift sees the count before the increment, so the largest count is 4 * ceil(n/4) - 1. For every 32-bit n,
ceil(n/4) <= 2^30, so the largest count is 2^32 - 1 and its high dword is always 0. The bound "n + 2" in the manual
proof is loose and was then read as if it were attainable. All 112 sites are therefore provably safe, and
`GTA6RT_ISA_FIXUP_VSHIFT` is not needed for correctness on this title. Keeping the 7-site patch is harmless (about
4 ns per execution at those sites) and I would keep it as defence in depth, but the documentation must not say a patch
is required, and open issue 6 in STATUS.md ("keep that one") should go.

## Defects

### Blocking

1. **False claim about 0x3265efb** (above). Fix is documentation only: STATUS.md section 5 and open issue 6,
   `data/vshift_manual.json`, and the report. Change the verdict for that site to SAFE with the proof above.

2. **`code_write` removes execute permission while it writes, so it crashes any other thread running code on that
   page.** `x86_patch.cpp: code_write` does `mprotect(page, RW)`, memcpy, `mprotect(page, orig)` for every write,
   including writes into the stub/thunk arena (`arena_write`, called by `gta6rt_isa_make_thunk` and by the SSE4a and
   vshift patchers). I reproduced the effect on this machine under Rosetta with a standalone x86_64 program: one
   thread calls a function in an RX page in a loop, another toggles the same page RW and back; the first thread
   takes SIGSEGV after about 142,000 calls (and immediately if it has not run the code yet). In this code base the
   trigger is: any guest thread executing a thunk or stub while another thread creates a new thunk (a `GetProcAddress`
   or `LoadLibraryExW` that binds imports, which S0.6 says is the next step inside WinMain) or patches an image whose
   stubs share a 4 KiB page. Thunks are 32 bytes each, so about 128 share a page with their neighbours. The tests do
   not catch this because they are single-threaded at the time of every write. Suggested fix: map the arena RWX once
   (the host is not hardened-runtime on x86_64 dev builds) or write through a second RW alias of the same pages
   (`mach_vm_remap`), and only use the mprotect path for image pages that are not yet executing. Also document that
   `gta6rt_isa_patch_code` is not safe on live code.

### Non-blocking

3. `SA_ONSTACK` is set but no thread gets a `sigaltstack`, so a guest stack overflow (SIGSEGV on the guard page)
   cannot run the handler and the process dies instead of delivering `STATUS_STACK_OVERFLOW` to seh. Needs an
   alternate stack per attached thread before seh's stack-overflow handling can work.
4. `gta6rt_isa_call_guest4` returns 0 on failure, which a guest can legitimately return. It is only a convenience
   wrapper, but the loader should use `call_guest` and check the status.
5. SWAP NATIVE thunks keep a 64-deep return stack and `ud2` when it is full; a non-local exit through a host frame
   (SEH unwind past a thunk) leaves `depth` incremented. SHARE (the default) has neither limit.
6. `gta6rt_isa_reexec_for_cpuid` exports `GTA6RT_ISA_REEXECED=1` and `ROSETTA_ADVERTISE_AVX=1` to every child
   process the host later spawns. Harmless, but the host should know.
7. TLS block constructors and destructors run under the internal `g_mu`; a ctor/dtor that calls back into isa
   (`call_guest`, `thread_attach`, `tls_slot_*`) deadlocks. The loader's template-copy ctor does not, but it is an
   undocumented constraint.
8. `isa_rosetta_test` check count varies between runs (90,819 vs 90,820 here, so the "90,820" in the report is not a
   fixed number). The `ill_hook` in the test returns CONTINUE_SEARCH on both ternary arms, so the ud2 test only
   proves the hook was called. `tools/__pycache__/` was left in the tree.
9. Report item 1 (skip `FIXUP_CPUID` when Rosetta advertises the policy) is sound, but note that
   `gta6rt_isa_cpuid_enforced` requires equality, so a Rosetta that advertises extra bits (for example RDRAND, which
   it does) is still "equal" only because the policy leaves bit 30 alone. A future policy change that masks RDRAND
   would silently make `enforced` return 0 under the env variable.

## Not verified by me

- SHARE with MoltenVK/SDL3 event-loop threads and long sessions (report item 4; I only checked pthread key numbers).
- fexcore has no executing code to test.
- The per-trap SIGILL cost and the nanosecond timings are load-dependent; I did not re-measure on an idle machine.
- The full vshift analyzer (690 lines): I read every transfer function and the reaching-definition code, hand-checked
  8 of the 112 sites against the disassembly, and spot-checked constants in the exe, but did not re-derive all 105
  tool verdicts.

## Files

- `/Users/a/Desktop/Projects/GTA-VI/port/isa/REVIEW.md` (this file)
- Reviewed: `port/isa/common/{isa_core,x86_patch,sse4a,cpuid_policy}.cpp`, `port/isa/rosetta/{rosetta_backend.cpp,rosetta_asm.S}`,
  `port/isa/fexcore/fexcore_backend.cpp`, `port/include/gta6rt/isa.h`, `port/isa/tools/vshift_analyze.py`,
  `port/isa/data/*.json`, `port/isa/tests/isa_rosetta_test.cpp`.
- Private build trees: `port/build/review-isa-arm64`, `port/build/review-isa-x86_64`.
