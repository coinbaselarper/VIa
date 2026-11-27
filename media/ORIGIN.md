# Origins (port/media)

Licence of the module: GPL-3.0-or-later, like the rest of `port/` (`port/LICENSE`).

| file | origin |
|---|---|
| `src/*`, `include/gta6rt/media.h`, `dev/*` | new code. Structure layouts, vtable order, constants and HRESULTs are those of the public Windows SDK headers `xaudio2.h` (2.9, `#pragma pack(1)`), `mmreg.h`, `mfapi.h`, `mfidl.h`, `mfobjects.h`, `mfreadwrite.h`, `propidl.h`, re-declared here; no header text copied. Which methods Bink2Scarlett and the title call, and with which arguments, was read from the disassembly of `xbox/.../Bink2Scarlett.dll` and `exe/game_scarlett_master.exe` (`tools/`). |
| `tools/import_sites.py`, `tools/disasm.py`, `tools/xref.py`, `tools/ptrref.py` | new; run with `spike/s1-seh/venv/bin/python -I` (capstone 5.0.7, pefile 2024.8.26). Read-only on the PEs. |
| `patches/acphal-xma-fixes.patch` | a diff against `port/shims_gdk/src/acphal.cpp` (itself a leonida port). The two behaviours are the ones `spike/ref-cool-fox/ASSESSMENT.md` 4(b) #12 names, implemented in the cool_fox fork at `crates/victory-hle/src/modules/audio.rs:514-575` (GPL-3.0-or-later): publish only whole blocks except the final tail; revive an ended voice when the title refills an input while the context stays enabled. The C++ is written for acphal's structure, not translated line by line. |
| `tests/xma/xma_fix_test.cpp` | new; its harness (SHAPE context layout, AcpHal vtable slots 0/2/5, ApuAlloc flags, the context word layout) follows `port/shims_gdk/tests/acphal_test.cpp` (GPL-3.0-or-later, from leonida). |
| `tests/xma/leonida_xma2_decoder.h` | the function declarations of `leonida/runtime/xma_decoder/leonida_xma2_decoder.h` (GPL-3.0-or-later), with a stand-in implementation in `tests/xma/fake_xma2_decoder.cpp` (new). Test-only. |
| other `tests/*` | new. |

Nothing in this module is copied from `VIctory/` or the `spike/ref-*` clones (read only, never built or run).
