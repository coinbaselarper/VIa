# Host memory governor and memory accounting (2026-11-26)

`port/host/src/governor/`: accounting, a process memory budget, and a governor thread with graded actions. Built in
`port/build/governor-x86_64` (frozen copies: `bin-ab/` for runs A2, B, C, D; `bin-final/` for E). The shared
`build/macos` was not rebuilt. All runs: x86_64 under Rosetta, MoltenVK, Series S profile, hidden window, imported save,
`host/tools/s3_storyload.sh NAME 300` with `GTA6RT_BIN` and `LEONIDA_MEM_PROBE=30`. One gta6rt at a time; other
workers loaded the machine during every run, so the machine state is given beside each number.

## Result

- **Accounting works.** One `[gov] memory` line per interval (and at every stage change) gives process footprint,
  resident, compressed, swap-ins; title pool committed vs touched; Metal `currentAllocatedSize` against
  `recommendedMaxWorkingSetSize`; the task's graphics ledger; every process's GPU memory (IOAccelerator
  PerformanceStatistics); the D3D12 layer's VkDeviceMemory by purpose (`gta6rt_gpu_memory_report`, the in-place
  worker's `include/gta6rt/gpu_memory.h`); malloc; what the title released; and the machine (pressure level,
  available %, swap, free+speculative, wired, file cache and pageins/s, compressor, load). A per-category table is
  logged at each `[s3]` evidence point (stage change); `gov_evidence.py` turns them into `[s3] gov ...` lines.
- **The governor fired under real pressure** (runs B, D, E): the layer released 778 MiB of retired title copies in
  B (5 calls), 550 MiB in D and 294 MiB in E; malloc pressure relief recovered 0 every time; the title issued no MEM_RESET, so (d)
  had nothing to advise; (b) has no reclaimer yet. **It did not lower the peak**: B (on) 11,862 MiB against A2
  (observe, same binary) 11,508 MiB. The layer releases the same retired copies itself after a 3 s grace, so the
  action only advances that by seconds.
- **Free win applied: the malloc medium zone is off by default** (`MallocMediumZone=0`, set before the cpuid
  re-exec; `GTA6RT_MALLOC_MEDIUM=on` keeps it). Under Rosetta the medium zone keeps freed 32 KiB-8 MiB blocks: a
  standalone test held 471 MiB after freeing all of 471 MiB, and 40 MiB with the zone off. In the title: −207 to −304
  MiB footprint at landing, first-run pages and story-load start (runs D and E against A2 and B: 4,073-4,101 vs
  4,308-4,321 at landing); no change at the world phase.
- **The world-phase "out of GPU memory" is not memory exhaustion.** In 10 of 11 world-phase device losses (A, A2, B, D, E
  of this round plus `s3-storyload`, `s3r4-f`, `r4vk-s3-c`, and the layer worker's `s3l5-base`, `s3l5-r1`) the failing
  `vkAllocateMemory` of 65,536 bytes is the line right after the layer's "a 64 KiB buffer over the title's memory at
  **0xb93680000**" (8 runs) or **0xb5e250000** (2 runs) for a vertex or index view (within 13 lines), a few lines after the
  first "GPU address arena full (raise LEONIDA_ARENA_MB)". At those moments Metal had 5.8-8.0 GiB allocated, 32-44 %
  of its 18,186 MiB working set, and process footprints ranged 11.0-13.7 GiB with the machine at 18-29 % available.
- `./verify.sh s1`: PASS (shared `build/macos`). The same check with the governor binary
  (`run/logs/gov-s1-final.log`): 36,739 exe calls before WinMain, 0 unimplemented.

## Runs

| run | binary | governor | load at start | landing | story load | world | end |
|---|---|---|---:|---:|---:|---:|---|
| `gov-s3-observe-a` | dev build 17:12 (layer sources of 17:06) | observe | 33.4 | +31.8 s | +56.6 s | +187.0 s | device lost, int3, rc 134 at 199 s |
| `gov-s3-on-b` | `bin-ab` (layer of 17:19, with `gta6rt_gpu_memory_release`) | on | 21.7 | +27.7 s | +56.5 s | +237.2 s | same, 278 s |
| `gov-s3-observe-a2` | `bin-ab` | observe | 17.8 | +24.4 s | +56.3 s | +202.8 s | same, 219 s |
| `gov-s3-on-arena1g-c` | `bin-ab`, `LEONIDA_ARENA_MB=1024` | on | 14.8 | - | - | - | C++ exception thrown in `amd_fidelityfx_upscaler_xbox.dll+0x1d07` at ~21 s |
| `gov-s3-on-nomedium-d` | `bin-ab`, `MallocMediumZone=0` | on | 12.7 | +22.8 s | +56.3 s | +175.9 s | same, 198 s |
| `gov-s3-final-e` | `bin-final` (defaults: on, medium zone off) | on | 10.9 | +23 s | +56.3 s | +188.6 s | same, 198 s |

Logs `run/logs/<run>.log` (each ends with `[s3]` and `[s3] gov` lines), snapshots in `run/logs/governor/`.

## Before / after (MiB; footprint from the governor's line at the stage change)

| stage | A (observe, older layer) | A2 (observe) | B (on) | D (on, medium zone off) | E (final defaults) |
|---|---:|---:|---:|---:|---:|
| landing | 4,314 (avail 60 %, swap 6365/7168, wired 7446, load 28.0) | 4,308 (64 %, 5490/6144, 6954, 17.3) | 4,321 (61 %, 4558/5120, 6928, 21.8) | **4,101** (60 %, 4650/6144, 6836, 13.6) | **4,073** (63 %, 5144/6144, 6869, 12.7) |
| first-run pages | 6,186 (51 %, 6341/7168, 9104, 26.0) | 6,167 (51 %, 5386/6144, 8620, 17.6) | 6,200 (47 %, 4550/5120, 8596, 19.8) | **5,911** (48 %, 4650/6144, 8522, 13.6) | **5,893** (52 %, 5144/6144, 8541, 12.6) |
| story load start | 6,606 (46 %, 6333/7168, 9227, 25.6) | 6,611 (50 %, 5354/6144, 8760, 16.0) | 6,608 (47 %, 4542/5120, 8706, 19.7) | **6,307** (44 %, 4650/6144, 8629, 13.3) | **6,305** (48 %, 5144/6144, 8649, 13.6) |
| world phase start | 13,278 (19 %, 5405/6144, 12493, 20.6) | 11,508 (24 %, 4746/6144, 10419, 20.0) | 11,272 (29 %, 4366/5120, 10164, 22.0) | 11,515 (22 %, 4873/6144, 10581, 18.3) | 11,000 (28 %, 5000/6144, 10237, 16.6) |
| peak (os report) | 13,685 | 11,508 | 11,862 | 11,515 | 11,419 |

Machine state in parentheses: available % (`kern.memorystatus_level`), swap used/total MiB, wired MiB, load average.
The early stages repeat within 0.5 % across A, A2 and B; the world phase varies with the machine. A → A2 is the layer
update (title copies at the world phase 3,164 → 1,520 MiB), not the governor.

Passive `footprint` of the same runs, 8 s after landing, malloc categories (MiB): A2 Small 443 Large 259 Medium 251
Tiny 227 (total footprint 6,073); B 442/259/274/226 (6,092); D 443/254/-/238 (**5,817**). At the world phase:
A2 1,755, B 1,660, D 1,486 MiB of malloc (`run/logs/governor/*.fp-*.txt`).

## What fills the footprint (run B at the world phase, `[s3] gov table +238s`)

| category (MiB) | reserved | committed | resident | compressed |
|---|---:|---:|---:|---:|
| title pool (xmem physical pages) | 8,192 | 4,679 | 755 | 3,602 |
| title windows (aliases of the pool, not extra) | 39,074 | 10,210 | 2,281 | 7,244 |
| title + host vm table, other | 3,189 | 3,188 | 631 | 1,115 |
| images (exe, package DLLs) | - | 179 | 39 | 118 |
| GPU charged to the process (graphics ledger) | - | - | 4,947 | 7 |
| Metal allocated, all kinds | - | 5,819 | - | - |
| layer: title copies / render targets / descriptors+arena / textures / staging / other | - | 1,315 / 2,388 / 256 / 170 / 32 / 101 | - | - |
| layer: title pages imported (read in place) | - | 755 | - | - |
| malloc (allocated / in use) | - | 1,445 | 1,416 | - |
| footprint not attributed above | - | - | 57 | - |

Machine: pressure warn, swap 4,366/5,120 MiB, free+speculative 150, wired 10,164, load 22.0.

- **Title pool:** the title takes 4,679 MiB of physical pages at +5-7 s (74,864 pages) and never frees one: 0
  XMemFreePhysicalPages, 0 MEM_DECOMMIT, 0 MEM_RESET through the world phase (429 VirtualAlloc/XMemVirtualAlloc and 5
  VirtualFree calls seen by the wrappers). Touched: 1.1 GiB at landing, 1.6 GiB in the story load, 4.36 GiB (93 %) at
  the world phase, 3.6-3.9 GiB of it compressed. xmem's MEM_RESET is a no-op today
  (`proposals/xmem-mem-reset.patch` makes it MADV_FREE); freed pages and MEM_DECOMMIT are returned (decommit maps fresh
  pages; "released but held": 0 MiB in every report).
- **GPU:** render targets 2,391 MiB from the first-run pages on (964 at landing), title copies 0.35 GiB before the
  world phase and 1.3-3.2 GiB in it. Owned unmapped graphics (`footprint`) 2.9 GiB at landing in the perf12
  run and 3.8-4.1 GiB at the world phase (A2, B, s3l5-r1).
- **Not costs:** guest stacks (8 MiB reserved per thread; 125-144 stacks in `vmmap`: 806-836 MiB virtual, 1-3 MiB
  resident + 5-7 MiB swapped);
  Rosetta JIT 47-142 MiB; page tables 17-31 MiB; mapped files are clean. The 16 GiB windows are reservations: 10.2 GiB
  of window pages are committed aliases of 4.7 GiB of pool slots.
- **malloc:** 0.95 GiB at landing, 1.4-1.5 GiB at the world phase, 1.0-3.3 M allocations; fragmentation 11-54 %
  (`vmmap`, up to 882 MiB held free in B). Under Rosetta `malloc_zone_pressure_relief` returns 0 (standalone test
  and 11 firings in B, D and E).

## 1. Accounting

- `os/src/vm.cpp`: `gta6rt_os_system_memory` (sysctl + host_statistics64), `gta6rt_os_memory_report_regions` (the old
  report, one walk, per-region usage out, "held in released pages" = resident/compressed in no-access kernel entries,
  machine state appended). The report line keeps its prefix (`s3_evidence.py` parses it unchanged).
  `GTA6RT_MEMORY_REPORT_OWNER=host` stops os/'s reporter thread; the governor then walks once per interval.
- `include/gta6rt/os.h`: the two declarations (additive).
- The governor line, example (B, world phase): `[gov] memory +238s (stage world (loading screen ended)): footprint
  11272 MiB (budget 14712, 77%), resident 2145, compressed 5087, swapped in 0 MiB since last; pool 4679 committed, 4357
  touched (755 resident + 3602 compressed) ...; gpu metal 5819 of 18186 working set, graphics ledger 4947 ..., all
  processes 9103 allocated 6931 in use; layer vk 4261 in 1800 (title copies 1315, render targets 2388, ...); malloc
  1416 in use of 1445; ...; machine: pressure warn, available 29%, swap 4366 of 5120, free+speculative 150, wired
  10164, file cache 2335 (1471 pageins/s), compressor 6611 holding 11950, load 22.0; level 1; walk 250 ms`.
- Per-process swap is not exposed by macOS: `compressed` includes compressed pages that were swapped out; the line
  gives the process's swap-ins (`ledger_swapins`) and the machine's swap.
- HUD (`hud/`): `gta6rt_hud_publish_memory` / `gta6rt_hud_set_mem_note`; the min line shows `mem F/BUDGETG gpu XG`
  (amber at 85 % of the budget or 90 % of the GPU working set, red and `LOW` at level 3); the full line adds `gov L
  gpu/ws`. `hud_unit_test`: 82 checks PASS (5 new). Not seen live: runs are hidden.

## 2. Budget (`--memory-budget auto|SIZE`, default auto)

`budget = min(physical − OS reserve, own footprint + available now − 15 % of physical)`, at least 4 GiB; OS reserve =
physical/4 clamped to 4-8 GiB; "available" is the kernel's `kern.memorystatus_level`. On this Mac: static ceiling 18.0
GiB; measured budgets 11.7-18.4 GiB during runs (it follows the machine). The profile follows the budget at start:
Series X when the budget ≥ 13.5 GiB title + 6.5 GiB host (20.0 GiB; host share measured in S3: 13.7 − 7.6 GiB), else
Series S. Checked: `--memory-budget 20G` → series-x, `12G` and auto (14.6 GiB at that moment) → series-s
(`run/logs/gov-budget-*.log`). `--console-profile`, `LEONIDA_CONSOLE_PROFILE` and `--stop-at` keep precedence.

## 3. Governor (`--governor on|observe|off`, `--governor-actions a-e|all|none`)

- Poll thread (500 ms, QoS user-initiated): footprint, budget, Metal, layer report, machine. Levels: 1 at 80 % of the
  budget, 80 % of the GPU working set or OS warn; 2 at 90 %/90 %, or OS warn with ≥ 80 %; 3 at OS critical, GPU ≥ 95 %,
  a layer allocation failure, footprint ≥ budget with OS warn or < 1 GiB swap free, or OS warn with ≤ 20 % available
  and < 1.5 GiB swap free. Report thread (QoS utility) does the region walk every interval and at each stage change.
- DISPATCH_SOURCE_TYPE_MEMORYPRESSURE is installed but **delivered no event in any run** while the sysctl level was
  warn for minutes (the kernel notifies selected processes); the sysctl poll is what drives the levels.
- Actions, each switchable and logged with bytes: (a) `gta6rt_gpu_memory_release` (the layer's) plus registered
  reclaimers; (b) registered reclaimers only (none exists); (c) `malloc_zone_pressure_relief` plus registered
  reclaimers (SPIR-V cache, DStorage staging, trace rings: none registered); after runs B-E the relief call is
  skipped under Rosetta (0 MiB in 11 calls, 57-254 ms holding malloc's zone locks, which title threads also take); (d) MEM_RESET advice through wrappers of
  VirtualAlloc/XMemVirtualAlloc (MADV_FREE via `gta6rt_vm_reset`) plus registered reclaimers; (e) HUD note `MEMORY
  LOW` and a `[gov] WARNING` line every 30 s at level 3. Cooldown 15 s per action. No action runs on a title thread;
  the wrappers only count (and advise when (d) is armed).
- `include/gta6rt/governor.h` (new): `gta6rt_governor_register(action, name, fn, ctx)` and `gta6rt_governor_level()`,
  weak imports.
- Measured firing (B): 5× (a) 356 + 25 + 364 + 33 + 0 = 778 MiB released by the layer's count (footprint change
  right after −0 to +343 MiB: the images wait for the deferred deletion); 5× (c) 0 MiB (footprint −114, −97, −12, +2,
  +310 in the next 74-157 ms, i.e. noise); 3× (d) 0 MiB (no MEM_RESET); (b) "no reclaimer registered"; level 3 never
  reached, so (e) never fired. D: (a) 503 + 26 + 21 MiB; level 3 (OS warn, 20 % available, 628 MiB swap free)
  raised the `[gov] WARNING` line and the HUD note, 116 log lines after MoltenVK's allocation failure and 105 before
  the title's int3.
- **(e) never warned before a device loss**: A, A2, B lost the device at OS warn with 18-29 % available, below
  level 3, and in D level 3 came after the failure. The layer's `failed_allocations` stayed 0 although MoltenVK
  logged the failure, so the governor cannot see the refusal itself.
- Overhead (both threads, CPU time): 1,331 ms over 193 s (A), 1,465/215 s (A2), 2,153/271 s (B), 1,519/181 s (D):
  0.68-0.83 % of one core; region walk 174-256 ms mean, max 904 ms, on the utility thread. E (summary line on the
  fatal path): poll thread 361 ms over 197 s, 388 polls, mean 1.3 ms, max 261 ms (a poll that ran (c): malloc
  relief takes 57-254 ms); report thread 1,342 ms, 14 walks, mean 141 ms, max 431 ms; together 0.865 % of one core.

## Next steps, ranked

1. **Layer (in-place / world-memory worker):** the world-phase failure is a 64 KiB host-memory import of title memory
   at 0xb93680000 / 0xb5e250000 after the GPU address arena is full, not exhaustion (see Result). That address lies
   in a committed 1,278 MiB title region (0xb48c00000, 513 MiB of it touched in B). Find why Metal refuses that import
   (page state of the range, overlap with another import) or keep such buffers in an arena that does not fill;
   `LEONIDA_ARENA_MB=1024` made the FSR4 DLL throw at boot (run C), so the arena size alone is not the fix.
2. Layer: count MoltenVK allocation failures in `failed_allocations` and report arena use/size in
   `gta6rt_gpu_bytes`, so level 3 and (e) can fire before the loss.
3. Layer: render targets are 2.4 GiB from the first-run pages on, the largest GPU item before the world phase; title
   copies grow to 1.3-3.2 GiB in it. Make live title copies evictable (today only retired ones are).
4. Register reclaimers: pipeline compiler threads and warm-up (b, perf worker's area), SPIR-V cache, DStorage
   staging window and trace rings (c). Each is a `gta6rt_governor_register` call.
5. xmem: apply `proposals/xmem-mem-reset.patch` (MEM_RESET → MADV_FREE). The title does not use MEM_RESET in the
   measured span, so no gain is expected until it does.
6. Malloc: 0.9-1.5 GiB with up to 54 % fragmentation; most of the churn is PSO creation at boot (perf worker's
   SPIR-V/PSO changes should cut it).

## Files

- New: `host/src/governor/{governor.h,governor.cpp,title_hooks.h,title_hooks.cpp,gpu_metal.h,gpu_metal.mm,
  gov_evidence.py,STATUS.md,proposals/xmem-mem-reset.patch}`, `include/gta6rt/governor.h`.
- Changed: `host/src/main.cpp` (options, profile from the budget, hooks, start, finish, summary on the fatal path),
  `host/CMakeLists.txt` (3 sources, Metal and IOKit), `os/src/vm.cpp` (memory report), `include/gta6rt/os.h` (two
  additive declarations), `hud/include/gta6rt_hud/stats.h`, `hud/src/{hud_model.h,hud_model.cpp,hud.mm}`,
  `hud/tests/hud_unit_test.cpp`, `shims_core/src/xmem.{h,cpp}` (read-only `pool_info`, `window_ranges` for the pool
  accounting).
- Tests: `hud_unit_test` 82/82 (`build/governor-hud-arm64`), `os_test_vm` 172/172 (`build/governor-os-arm64`),
  `gov-s1-final` 36,739 exe calls before WinMain, `./verify.sh s1` PASS.
- `gov_evidence.py LOG --append` adds the `[s3] gov` lines; `s3_storyload.sh` does not call it yet (one line after
  `s3_evidence.py`, for the lead).
