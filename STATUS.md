# Round 5 layer status (port/, 2026-11-26) — the world-phase wall is not GPU memory

D3D12 layer worker (`d3d12_vulkan/STATUS.md`, "Round 5"; runs `run/logs/s3l5-*.log` with `.footprint.txt`).
`build/macos` rebuilt with the final sources (`build/macos-build-r5.log`, rc 0); `./verify.sh s1` PASS.

- **Accounting**: the layer prints its GPU memory by purpose, Metal's `currentAllocatedSize`, VK_EXT_memory_budget, the
  title textures by class and the render targets by shape at every `--memory-report` period (`[rt] gpu-memory ...`);
  `include/gta6rt/gpu_memory.h` gives the governor `gta6rt_gpu_memory_report` and `gta6rt_gpu_memory_release`
  (contract in `run/requests/governor.md`).
- **Where the 13.7 GiB went** (`s3l5-base`, world phase): GPU private 5.6 GB (layer title copies 3.2 GiB of which
  1.8 GiB were copies of textures the title had already overwritten, render targets 2.4 GiB), title pool 4.3 GB,
  host malloc 1.9 GB, shared GPU 1.2 GB.
- **Cut**: stale title copies retired (6.6 GiB of them by the world phase in `s3l5-r1` and `-r7`), title image pool heaps 2 MiB:
  title copies at the world phase 3,157 -> 1,308-1,605 MiB, peak footprint 13.7 -> 11.2-12.8 GiB.
- **In place**: Metal samples a title texture in place only as a linear, uncompressed, one-mip 2D buffer-backed
  texture: 21 MiB of ~5.1 GiB of the title's texture bytes; the rest is GFX10-swizzled or BC and stays copied.
- **Arena**: every S3 run's "GPU address arena full" at the world phase was the arena's own host pages counted as
  title memory; fixed, default 384 MiB.
- **Requests**: refused PSOs are placeholders returned with S_OK (FSR4 no longer asserts); the host's null-PSO and FSR
  patches are off by default; slot-70 views that reinterpret a slot-133 texture get their own image; vk_guard only
  reports. Title-written T#s naming another format still get the null view.
- **Still stopping it**: 5-18 s into the world phase: Metal Invalid Resource (code 9) in 7 of 9 runs, not memory
  (6 of 18 GiB Metal working set, no allocation failure with imports off, unchanged when frees wait for an idle GPU);
  a host SIGSEGV and a silent SIGABRT in the last two. **No world frame** (last dumps: loading artwork fading out).

# Round 4 status (port/, 2026-11-26) — S3: story load and world phase reached on MoltenVK x86_64

**Result.** `build/macos/gta6rt` (Mach-O x86_64, Rosetta, MoltenVK 1.4.2 x86_64, Series S profile, 1920x1080, hidden
window) takes the title, unattended, from the landing prompt through the story load into leonida's **world phase**,
by both paths:

| path | how | story load starts | world phase begins | story load | runs |
|---|---|---:|---:|---:|---|
| the user's save set (imported) | landing A → stinger → story load (no first-run or legal pages) | +56 to +75 s | +108 to +188 s | 52–126 s | `s3-storyload`, `s3r4-c`, `-d`, `-e`, `-f` |
| no saves (new game) | landing A → stinger → 5 first-run pages → LEGAL NOTICES → terms page → story load | +184.6 s | +217.6 s | 33 s | `s3r4-g` |

**World phase, as leonida defines it** (`leonida/docs/linux_proton.md`: "story load ends (about 38,000 streamed images
registered), world phase begins"; `leonida/tools/stage_timeline.py`: session 0 is the story load, and the loading
byte at `0x7453ed0` goes 2 → 0 when the loading screen ends). Both halves are measured here, independently:
- the title's own state, read every 0.5 s by the host (`host/src/title_watch.cpp`): game state (`0x7446510`) 3,
  session (low dword of `0x98f828c`) 0, loading byte (`0x7453ed0`) 2 during the load, then 0;
- the D3D12 layer's image registry passes 38,000 images within 10 s of that change (38,182 in `s3-storyload`).

Proof file: `run/logs/s3-storyload.log` (the `[s3]` lines at its end), made by `host/tools/s3_storyload.sh`.

**Where it stops now: GPU memory, at the start of the world phase.** `VK_ERROR_OUT_OF_DEVICE_MEMORY` on 64 KiB
allocations, then MoltenVK loses the device ("Invalid Resource"), ExecuteCommandLists fails and the title raises its
device-removed `int3` at exe+0x2474606, 25–35 s after the loading screen ends. The footprint is 13.1–13.7 GiB at the
end of every story load. No world frame was presented. Stop 9 below; `run/requests/d3d12_vulkan-world-memory.md`.

`./verify.sh s1` passes with the final binary (`verify-s1.log`: 36,739 exe calls before WinMain, 0 unimplemented).

## Evidence: `run/logs/s3-storyload.log`

Command: `host/tools/s3_storyload.sh` (defaults: name `s3-storyload`, 300 s, imported saves). It imports the save set
(idempotent), ensures the BCPack profile, waits for the machine's gta6rt slot, runs `run.sh` with `--hidden-window
--watchdog 300 --memory-report 30 --present-dump run/present/s3-storyload,30 --peek <stage, arena words>
--title-watchdog on --input-script a@40,a@45,...,a@280`, then appends `[s3]` lines with `host/tools/s3_evidence.py`.

```
run.sh: start 15:52:42 bin=.../port/build/macos/gta6rt load { 11.03 13.46 16.27 }
[host] vulkan driver guard: .../build/macos/libgta6rt_vkguard.dylib in front of the driver ...
[host] null-PSO bind patch at RVA 0x248c306: ok ...
[host] FSR null-pipeline patch at amd_fidelityfx_upscaler_xbox.dll+0x65ff: ok ...
[host] stage t=18.7s: landing (threads 137)
[host] stage t=40.9s: first-run pages (threads 128)          (session 0xd; with the save this is the stinger)
[host] stage t=56.6s: story load (loading byte 0x2) (threads 134)
[host] stage t=165.5s: world (loading screen ended) (threads 141)
[rt] d3d12: title textures (refresh): ... registry 38182 images (35076 made, ...
[mvk-error] VK_ERROR_OUT_OF_DEVICE_MEMORY: vkAllocateMemory(): Could not allocate a host-coherent or exportable VkDeviceMemory of size 65536 bytes ...
[mvk-error] VK_ERROR_OUT_OF_DEVICE_MEMORY: Lost VkDevice after MTLCommandBuffer ... (code 9): Invalid Resource ...
[seh] FATAL: ... EXCEPTION_BREAKPOINT ... at 0x142474606 (game_scarlett_master.exe+0x2474606)
=== 0 call(s) reached unimplemented imports; 0 isa traps (SSE4a, CLWB/CLZERO/MONITORX, cpuid); null_pso_skips 0, fsr_skips 34 ===
run.sh: rc=134 after 191 s, load { 19.18 15.96 16.69 }
[s3] story load reached at +56.6s (session 0, threads 134)
[s3] world phase reached at +165.5s: loading byte went 2 -> 0 in session 0 (threads 141)
[s3] story load took 108.9 s
[s3] registry passed 38000 images at +157s
[s3] save call: ReadBlobDataResult(buffer 2400) -> 0, 2 blob(s)
[s3] save call: EnumerateContainerInfo(prefix ""): 4 container(s) "Player", "SARC0000", "SARC0015", "SettingsProfileSettings"
[s3] memory peak footprint 13672 MiB at +179s (56% of 24 GiB); Series S title budget 7.6 GiB
[s3] vk_guard: 5 image views refused (Metal would abort on them)
[s3] dstorage: 81632 requests, 2080.0 MiB read, 4673.5 MiB written, over 187 s; 0 failed
```

How the lines are read:
- `[s3] stage +Ts: X`: the host's stage watcher, one line per change of (game state, session, loading byte).
  `story load (loading byte 0x2)` = session 0; `world (loading screen ended)` = session 0 with the byte back at 0
  after it was 2.
- `[s3] registry passed N images`: the layer's `title textures (refresh)` line, timed by the nearest earlier peek.
- `[s3] save call:`: the gamesave shim's call log (below).
- `[s3] page/video from frame`: present dumps, text read with macOS Vision (`host/tools/screen_text.swift`); "video"
  = 65,536+ colours, which the loading slideshow's panning artwork also is. I looked at the artwork frames (Jason,
  Lucia): `run/present/s3r4-c/present_002221.png`, `present_002461.png`.
- `[s3] dstorage`: `LEONIDA_DSTORAGE_SHAPES` records (`run/logs/<name>.dstorage.tsv`).

## Stops, in order, and what was done

All runs: x86_64 under Rosetta, one gta6rt at a time machine-wide (`run.sh` refuses otherwise; the S3 script waits
for the slot), MoltenVK, hidden window. Logs `run/logs/<run>.log`, frames `run/present/<run>/`.

| # | run | what stopped or was wrong | cause | done |
|---|---|---|---|---|
| 1 | `s3r4-a` (no saves, A every 5 s) | first-run pages (NAVIGATION EXPERIENCE, UI TEXT SIZE, SUBTITLES, BRIGHTNESS, FINISH SETUP?), then **LEGAL NOTICES** stays; A does nothing there | the page is a list (Copyright, Photosensitivity Health Warning, Next); A on the first item only shows it | input: down, down, A |
| 2 | `s3r4-b` | **ROCKSTAR GAMES LEGAL TERMS & POLICIES** stays | two checkboxes (Privacy Policy, Terms of Service) must be ticked before Confirm | the new-game flow in `s3_storyload.sh` (`S3_FLOW=newgame`, default with `S3_SAVES=empty`): up, up, A, down, A, down, A, twice; `s3r4-g` reached the story load |
| 3 | `s3r4-a` | the title's persistent local storage came out as folders `overlay/.gta6rt-persistentsettings`, `...cloudcache`, `...cloudpers` | the title appends its folder names straight to `XPersistentLocalStorageGetPath`'s answer (a drive root on the console); ours had no trailing separator | `shims_gdk/src/xgameruntime.cpp`: the path ends in `/`; `xgameruntime_test` checks it; the stray empty folders removed |
| 4 | `s3r4-b` | (found) 20 BC7 DirectStorage requests failed `missing-profile` in every S2 run | `history.bcph` was never where the shim reads it (`build/shaders_vk/apple/bcpack/history.bcph`) | copied `spike/r4-bcpack-history/history.bcph` there; `s3_storyload.sh` runs `io_bench/scripts/ensure_bcpack_history.sh` first. Since then 0 failed requests (148–163 BC7 per run) |
| 5 | `s3r4-c` (imported saves) | **story load** at +75.5 s, **world phase** at +174.0 s; 9 s later abort: `failed assertion ... source texture pixelFormat (MTLPixelFormatBC7_RGBAUnorm) not compatible with texture view pixelFormat (MTLPixelFormatBC1_RGBA_sRGB)` | the layer asks for a T# view in a format Metal cannot reinterpret the image as; Metal checks the arguments in release builds and aborts | workaround: `host/src/vk_guard.cpp`, a pass-through Vulkan driver the host puts in front of MoltenVK; it refuses such views with `VK_ERROR_FORMAT_NOT_SUPPORTED` (the layer then makes a null view). 2–27 views refused per run. Request: `run/requests/d3d12_vulkan-view-format.md` |
| 6 | `s3r4-d` | access violation at exe+0x248c30d reading 0x10, 10 s into the world phase | a draw-state flush (exe+0x248c2a0) binds the PSO at `[state+0x120]`: it copies `[pso+0x10..0x30]` into the console list's pipeline record (+0x544) and calls list slot 25 (SetPipelineState). The PSO is null: the layer refused its creation (15,027 refused PSO calls, 14,992 of them mesh) and the title never checks | workaround: host patch at exe+0x248c306 (bytes checked) to a trampoline that leaves the previous PSO bound when the pointer is null, counted (`null_pso_skips`, 0–5 per run); `--no-null-pso-patch` turns it off. Request: `run/requests/d3d12_vulkan-refused-pso-object.md` |
| 7 | `s3r4-e` | access violation in `amd_fidelityfx_upscaler_xbox.dll+0x65ff` reading 0x10 | FSR4 pass 4 (`FSPX` program 03): `no translated compute shader for FSPX kind-6 ... sha256 6eef6015...`; slots 67 and 62 refuse it, FSR4 asserts `CreateModel(...) == FFX_API_RETURN_OK` and dispatches with a null pipeline. Program 03 translates with neither the port's translator nor leonida's current one ("fall-through into undecodable words": the `v_dot4_i32_i8` VOP3P words with `op_sel` 2, `leonida/docs/fspx.md`); program 18 is missing too | workaround: DLL patch at +0x65ff (bytes checked at 5 places) to a trampoline that returns from that dispatch the way the DLL's own end at +0x6831 does, after reloading r12/rsi/rdi/rbx from its frame; counted (`fsr_skips`, 34–72 per run). The refused-PSO request covers it |
| 8 | `s3r4-c` … `s3-storyload` | the title's 30 s hang watchdog (exe+0x2215a60) never fired | — | `--title-watchdog on\|off` added (off = leonida's `LEONIDA_TITLE_WATCHDOG=0` kick of byte exe+0x71b6331, the trap bytes checked); S3 runs keep it armed |
| 9 | `s3r4-f`, `-g`, `s3-storyload` | **device lost at the world phase**: `VK_ERROR_OUT_OF_DEVICE_MEMORY` on 64 KiB allocations, `Lost VkDevice ... Invalid Resource`, staged texture uploads and `CopyPageMappingsBatchX` fail, ExecuteCommandLists fails, the title's int3 at exe+0x2474606 | GPU allocations exhausted: footprint 13.1–13.7 GiB, the machine at 4.5 of 5 GiB swap with the user's and other workers' processes running | not fixed (the layer's memory). Request: `run/requests/d3d12_vulkan-world-memory.md` |

Not seen in any S3 run: the arena int3 at exe+0x31f50 (`spike/r4-exe-analysis/VERIFY.md`). Its arena words are now
peeked every 10 s (`0x77ae260:4`, `0x77ae340:8` in the `[rt] peek` lines), so the next occurrence carries the values
VERIFY asks for. XStore slots 71/72 (`XStoreAcquireLicenseForDurables`) return E_FAIL once per boot; the title goes on.
`0 call(s) reached unimplemented imports` in `s3-storyload` (the fatal-signal path now prints the shim report).

## Saves

### What the title asks for (traced: `shims_gdk/src/xgameruntime_gamesave.cpp` now logs every slot with its names)

```
InitializeProviderAsync("00000000-0000-0000-0000-00007952987a", syncOnDemand=0)          (the SCID)
GetContainerInfo("Player")                   then ReadBlobDataAsync("Player", "Save Game Header", "Save Game Data")
GetContainerInfo("SettingsProfileSettings")  then ReadBlobDataAsync(... "Save Game Header"), then both blobs
EnumerateContainerInfo(prefix "")            at the landing prompt
EnumerateBlobInfo("Player", prefix "Save Game Header"), ReadBlobData("Player", "Save Game Header")
GetContainerInfo("PlayerCrossSlot")          as the story load starts (no such container in the set)
new game: CreateContainer("Player"), CreateUpdate, SubmitBlobWrite("Save Game Header", 296 B),
          SubmitBlobWrite("Save Game Data", 2,012 B), SubmitUpdateAsync; the same for "SettingsProfileSettings"
          (296 B, 7,344 B)
```

The exe holds the strings `SARC`, `%s%04d` (story slots SARC0000…), `PlayerCrossSlot`, `Save Game Header`,
`Save Game Data`. No XGameSaveFiles slot was called.

### The VIctory set and its import

VIctory's layout (`VIctory/crates/victory-hle/src/modules/xgamesave.rs`): `saves/<XUID>/<configId>/<name>~<fnv64>/vN/`,
`manifest` = u32-length strings (container name, display name, blob names), blob *i* in file *i*. The set:

| container | display | blob 0 "Save Game Data" | blob 1 "Save Game Header" |
|---|---|---|---|
| `Player` (v5) | Player | 2,012 B, ASAV v2 kind 3 (profile) | 296 B |
| `SettingsProfileSettings` (v1) | SettingsProfileSettings | 7,344 B, ASAV v2 kind 2 (settings) | 296 B |
| `SARC0000` (v1) | SARC0000 | 272,176 B, ASAV v2 kind 1, packed, "Gone Fishing" | 296 B |
| `SARC0015` (v3) | SARC0015 | 271,200 B, ASAV v2 kind 1, packed, "(Autosave) Gone Fishing" | 296 B |

plus `storage/settings/settings.xml` (persistent local storage). The header blob: u32 6, u32 data-blob size, 8 bytes
(checksum, unknown), u64 unix time, for story slots u32 0x04100200 + packed size + unpacked size, then the UTF-16
title. ASAV is version 2 here (`leonida/docs/saves.md` documents 1).

`host/tools/s3_import_saves.py SRC` writes `run/saves/local-user/<enc(container)>/<enc(blob)>` (the shim's %-encoding),
`.display`, the storage into `run/overlay/.gta6rt-persistent/`, and `run/saves/import-manifest.json` (source, size,
SHA-256; every file read back and compared; source mtimes kept). The originals are only read (`find -newer` over
them: nothing). The new-game run wrote exactly these container and blob names with the same sizes, which confirms the
mapping.

### What the title does with it

- It reads `Player` and `SettingsProfileSettings` back (2 blobs each: 2,308 and 7,640 bytes, the imported sizes) and
  **skips the first-run and legal pages**: the landing prompt reads "ACCOUNT PICKER (Y) PLAYER (A)", and A goes to the
  stinger and straight into the story load.
- It lists all four containers but **never opens `SARC0000` or `SARC0015`**, in any run, before or after the world
  phase begins, and no page offered Continue or Load. Whether this story load is the user's progress ("skips the
  first mission") cannot be told: no world frame was presented. The new-game path loads in 33 s against 52–126 s with
  the profile; both stop at the same memory wall.

## Story load against leonida (`leonida/docs/linux_proton.md`, `d3d12_xs_console_abi.md`)

| | Linux (Proton, RTX 4070, 16 GB) | Windows A | here (M4 Pro, Rosetta, MoltenVK) |
|---|---|---|---|
| A accepted | ~180 s | ~70 s | first A at +40 s (landing at +19–31 s) |
| story load starts | ~240–300 s | ~110 s | +56–75 s (save), +185 s (new game, through 7 pages) |
| story load ends / world phase | ~540–630 s, ~38,000 images | not given | +108–218 s; 38,000 images at +105–215 s |
| memory at the end of the load | ~14.6 GB (RAM + swap) | 11.9 GB host private | footprint 13.1–13.7 GiB |
| next stop | culling compute, device lost | first world frame not verified | GPU memory, device lost |

**Streaming (DirectStorage, `s3-storyload`; the other save runs within 5 %).** 81,632 requests, 2,080 MiB read,
4,674 MiB written, 0 failed. By archive: `levels.rpf` 30,082 requests / 824 MiB, `anim.rpf` 171, `shaders.rpf` 131,
`packs.rpf` 95, `data.rpf` 45, `octa_imposters.ttpf` 42, `story000.rpf` 32, `core_bvh4.rpf` 29 MiB, four
`ttdb_props_*.ttpf` 12–15 MiB each. Codecs: zlib 68,688, raw 11,004, BC1 1,671, BC7 161, BC5 107, BC6 1. Rate: 346 +
272 MiB in the first minute (boot), then 622 and 494 MiB per 30 s in the story load (~20 MiB/s): the load is not
bound by the file reads.

**Memory against 24 GiB** (`os` memory report every 30 s, `s3-storyload`):

| +s | stage | footprint | resident | compressed |
|---:|---|---:|---:|---:|
| 31 | landing | 6,100 MiB | 1,914 | 1,141 |
| 56 | stinger, story load starts | 6,572 | 2,635 | 1,091 |
| 87 | story load | 7,135 | 1,161 | 2,862 |
| 119 | story load | 9,118 | 1,871 | 3,470 |
| 150 | story load | 12,197 | 1,860 | 4,945 |
| 179 | world phase | 13,672 | 1,459 | 6,175 |

- At the world phase (`s3r4-c`) the vm table holds 18,302 MiB committed. Growth during the load: the two 16 GiB xmem
  windows (0x300010000 380 → 1,952 MiB committed, 0x700010000 234 → 2,227 MiB) and the layer's ~35,000 made images.
- **The Series S profile does not hold:** its title memory is 7.6 GiB; the process reaches 13.7 GiB, and Metal refuses
  64 KiB allocations (stop 9). Resident stays at 0.6–2.6 GiB because the machine compresses and swaps.

**Pipelines.** PSO calls at boot as in S2 (15,027 refused). During the story load, pipelines taking over 2 s per 30 s:
up to 19–26 (`s3r4-d`, `-e`), 1–2 in `s3-storyload` (the shared pipeline cache warmed across runs: 22 hits, mean 208
ms, 420 misses, mean 825 ms in `s3r4-f`). The title's 30 s watchdog never fired.

**Threads** (host `task_threads` every 0.5 s): 82–102 at init, 125–138 at the landing prompt, 128–150 in the story
load (at most 175), then 194–634 after the device loss (`s3r4-f`, `-g`, `s3-storyload`; not investigated).

**Frame rate** (the layer's submit phases): 24–110 presented frames/s at the landing prompt, 14–62 through the stinger,
0.2–8 in the story load, 1.4–1.6 at the world phase start.

**Time in each phase** (`s3-storyload`): init 0–18.7 s, landing until the A at 40 s, stinger 41–57 s, story load
56.6–165.5 s, world phase 165.5 s to the device loss (about +185 s).

## Changes this round

**`host/` (mine):**
- `src/title_watch.{h,cpp}` (new): the stage watcher (`[host] stage t=..s: ...` on every change of game state, session,
  loading byte; with the thread count), `--peek SPEC`/`--peek-seconds N` (leonida's `LEONIDA_PEEK` format, also read
  from the environment; peek lines carry threads and the two patch counters), `--no-stage-watch`, `--title-watchdog
  on|off` (`LEONIDA_TITLE_WATCHDOG=0`), the null-PSO bind patch and the FSR null-pipeline patch.
- `src/vk_guard.cpp` (new, `libgta6rt_vkguard.dylib` beside `gta6rt`): the image-view guard. `configure_vulkan()` puts
  it in front of the chosen driver (`GTA6RT_VK_GUARD_DRIVER`); `GTA6RT_VK_GUARD=0` turns it off. The pipeline-cache
  folder is still named after the real driver.
- `src/main.cpp`: the options above, `--no-null-pso-patch`; the FSR patch applied in `fix_new_images()` when the
  upscaler DLL is loaded (before its DllMain); the fatal-signal path prints the shim report and the patch counters.
- `CMakeLists.txt`: `title_watch.cpp`; the `gta6rt_vkguard` MODULE target.
- `tools/s3_storyload.sh` (new): the S3 launcher (`S3_SAVES=imported|empty|DIR`, `S3_FLOW=save|newgame`,
  `S3_INPUT_SCRIPT`, `S3_VISIBLE`, `S3_TITLE_WATCHDOG`, `S3_DUMP_EVERY`, `S3_WAIT`, `S3_EXTRA`);
  `tools/s3_evidence.py` (new); `tools/s3_import_saves.py` (new).

**`shims_gdk/` xgameruntime (mine):**
- `xgameruntime_gamesave.cpp`: every slot logs its call with container and blob names, sizes and results (at most 400
  lines).
- `xgameruntime.cpp`: `XPersistentLocalStorageGetPath`/`GetPathSize` end the path with `/` (stop 3);
  `tests/xgameruntime_test.cpp` checks it.

**Housekeeping.**
- `_putenv_s`: already served (`shims_core/src/crt_extra.cpp`, `w_putenv_s`, by the shims_core worker). The title calls
  it once per run (`FFX_FSR4_XBOX_ACO=1`) and gets ok. Nothing to do.
- verify s1 profile question, decided: keep the host's rule (`series-x` under `--stop-at`, logged by name) and
  verify.sh as it is; title runs use Series S. PASS (36,739).
- `run/requests/`: three requests from me to the D3D12 layer worker. Picked up `driver.md` (lead, 15:35: audio
  patches applied to the live tree): the 15:47–15:48 rebuild of `build/macos` configured and built `audio_out` and the
  changed shims_gdk audio sources without errors; `s3-storyload` and the last `verify-s1` ran that binary, with
  `GTA6RT_AUDIO` off (`[rt] xapu: Opus decode ... off (GTA6RT_AUDIO=off)`). `layer.md` is for the layer worker.
- `build/shaders_vk/apple/bcpack/history.bcph` installed (stop 4).
- `s3_storyload.sh` is replaced atomically (`mv`) when edited: bash reads a running script by offset, and an in-place
  edit nearly re-ran a section of a queued run.

**Not touched:** `d3d12_vulkan/**`, `dstorage*`, `file_system*`, `gameinput*`, `host/src/win/**`, `port/metal`,
`shaderc_msl`, `audio_out`, `hud`, `compat`, `include`, `shaders_vk`, verify.sh, NOTES.md, leonida, VIctory, exe,
xbox, spike/ref-*.

## Builds and tests

- `build/macos` (x86_64): incremental, rc 0 (`build/macos-build-r4.log`). It also took the other workers' current
  sources (d3d12_vulkan, dstorage, gameinput, appkit_window). `build/macos-arm64`: rc 0, 87 steps, 52 warnings of the
  known kinds (`build/macos-arm64-build-r4.log`).
- `xgameruntime_test` 175 checks OK, `xgameruntime_gamesave_test` 76 checks, 0 failures (arm64 native,
  `build/macos-arm64/shims_gdk`).
- `./verify.sh s1`: PASS, both checks.
- `s3_import_saves.py`: into a scratch folder and into `run/saves`; `cmp` and the SHA-256 read-back match.
- `s3_evidence.py`: on every S3 log above.
- Both trampolines: encodings checked with capstone before use; both counted in real runs.

Not re-run: the other module suites (their sources are their workers').

## Known wrong or approximate

- **Three workarounds stand in for layer fixes** (view guard, null-PSO bind skip, FSR dispatch skip). Each leaves
  rendering wrong where it acts (no view, the previous pipeline, no FSR4 pass) and each is counted in the log.
- **No world frame** has been presented; the last frames are the slideshow fading out.
- **The story saves are not loaded** at boot; the title never reads SARC0000/SARC0015.
- **Presses can be missed** at low frame rates ("delivered late ... the title did not ask during its 0.3 s hold"); the
  new-game flow repeats the terms sequence for that reason, and its timing comes from these runs.
- **The stage names are leonida's** ("for this build"); they match the pages seen (landing 3, first-run 0xd, story
  load 0), except that with the save session 0xd covers the stinger, and session 0x6 appeared once before the first-run
  pages in the new-game run.
- **Memory numbers** are taken on a machine under pressure from other processes (swap 4.5 of 5 GiB at the end).

## Next work items, ranked

1. **GPU memory at the world phase (D3D12 layer).** The wall now: 13.7 GiB footprint, `OUT_OF_DEVICE_MEMORY`, device
   lost. Title images in place on unified memory first (`run/requests/d3d12_vulkan-world-memory.md`).
2. **Refused PSOs as placeholder objects (D3D12 layer)**, so the two title/DLL patches can go
   (`run/requests/d3d12_vulkan-refused-pso-object.md`); **the T# view-format check**, so the vk guard can go
   (`run/requests/d3d12_vulkan-view-format.md`).
3. **FSPX programs 03 and 18 (FSR4)**: decode the `v_dot4_i32_i8` `op_sel` 2 words on the fall-through path in the
   translator, or FSR4 pass 4 stays skipped.
4. **Load the user's story save** once a world frame runs: the pause menu's load page with scripted input; SARC0000 and
   SARC0015 are imported and listed.
5. **Threads after the device loss** (194–634): find what spawns them.
6. **Host**: a GPU-allocation figure (Metal `currentAllocatedSize`) in the memory report and the HUD, to see the
   layer's share of the 13.7 GiB.

## Proposed `verify.sh s3` check (for the lead; verify.sh not edited)

```sh
s3() {
    B=port/build/macos/gta6rt
    if [ ! -f "$B" ]; then skip "S3 host binary"; return; fi
    L=port/run/logs/s3-storyload.log
    rm -f "$L"
    port/host/tools/s3_storyload.sh >/dev/null 2>&1
    if [ -f "$L" ] &&
       grep -Eq '^\[s3\] story load reached at \+[0-9.]+s' "$L" &&
       grep -Eq '^\[s3\] world phase reached at \+[0-9.]+s: loading byte went 2 -> 0 in session 0' "$L" &&
       grep -Eq '^\[s3\] registry passed 38000 images at \+[0-9]+s' "$L" &&
       grep -Eq '^\[s3\] save call: ReadBlobDataResult\(buffer [0-9]+\) -> 0, 2 blob\(s\)' "$L" &&
       grep -q '^=== 0 call(s) reached unimplemented imports' "$L"
    then pass "S3 story load -> world phase on macOS (MoltenVK x86_64, imported save)"
    else fail "S3 world phase not reached (see $L)"; fi
}
```

- The exit code is not tested: the run currently ends in the device loss after the world phase (rc 134).
- Run time about 4–6 minutes (300 s watchdog, ~10 s of evidence, plus waiting for the gta6rt slot). Hidden window;
  needs no other gta6rt and a GPU not loaded by other work. All five greps match `s3-storyload.log`.

## For the lead to decide

- The three workarounds stay on by default for title runs until the layer requests land (`--no-null-pso-patch`,
  `GTA6RT_VK_GUARD=0` turn them off).
- The verify.sh s3 check above.

## Files

- Logs (`run/logs/`): `s3-storyload.log` (+ `.dstorage.tsv`, `.import.log`, `.runout`), `s3r4-a` … `s3r4-g` (`.log`,
  `.runout`; `.dstorage.tsv` from b on), `verify-s1.log`.
- Frames: `run/present/s3-storyload/`, `run/present/s3r4-a` … `s3r4-g/` (pages: `s3r4-a/present_002521.png` LEGAL
  NOTICES, `s3r4-b/present_003151.png` terms page, `s3r4-a/present_002341.png` FINISH SETUP?,
  `s3r4-c/present_002221.png` loading artwork).
- Requests: `run/requests/d3d12_vulkan-view-format.md`, `d3d12_vulkan-refused-pso-object.md`,
  `d3d12_vulkan-world-memory.md`.
- Saves: `run/saves/local-user/{Player,SARC0000,SARC0015,SettingsProfileSettings}`, `run/saves/import-manifest.json`,
  `run/overlay/.gta6rt-persistent/settings/settings.xml`; the new-game run's own saves in `run/fresh/s3r4-g/`.
- Build logs: `build/macos-build-r4.log`, `build/macos-arm64-build-r4.log`, `build/macos-arm64-tests-r4.log`.

## Input and overlay round (2026-11-26, input worker; kept from round 3's file)

Details and evidence in `shims_gdk/STATUS.md` ("Input round"), `host/src/win/STATUS.md` ("Input round") and `hud/STATUS.md` ("Essentials round"). Built and run from `build/input-x86_64/gta6rt`. (S3 driver's note: `build/macos` now builds these sources too; the S3 logs show their "delivered late" press lines.)
- Mouse is the keyboard pad's right stick (left button RT, right button LT); the cursor is captured while the title window is key and released on focus loss or while Option is held; Escape is untouched.
- With `--input-script` the keyboard pad, mouse and real controllers stay live next to the scripted pad (`GTA6RT_INPUT=script` for the scripted pad alone). Scripted presses are held until the title has asked on 4 frames and are delivered
  late (up to 10 s) when a long frame skipped their hold: in `run/logs/input-r4b.log` presses at 70, 85, 100 and 115 s were delivered 0.7 to 9.7 s late. The "A at +100 s read but the page did not change" glitch was not reproduced and its cause is not established.
- Overlay: quiet test app run (28 checks), live numbers agree with `vk-counters`, `submit phases` and the os memory report; `gpu -` on Vulkan (no GPU time is reported); the min line says `stalled N s` in a title stall; `GTA6RT_HUD_LOG=5` logs the overlay's numbers.
- These runs showed the title with a cold pipeline cache at 0.2 to 2 frames/s with stalls up to 6.5 s (`input-r4b.log`, 1.6 k pipelines pending); the movie was reached, the first-run pages were not inside 130 s.
