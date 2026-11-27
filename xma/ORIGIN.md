# Origin and licences of port/xma

| Path | Origin | Licence |
|---|---|---|
| `third_party/xma/` | Rust package `xma`, `crates/compat/xma` of https://github.com/omonkeyz/xbox-emulator at commit `27122f7dfcceb9a899555362ee0dde2d530e033c` (the commit leonida's `runtime/xma_decoder/Cargo.toml` pins). It is a Rust port of FFmpeg's `libavcodec/wmaprodec.c` (WMA Pro / XMA decoder), FFmpeg commit `76fa18983e3849595fdc332bf9fa583d5fe7a147`; FFmpeg copyright (c) 2007 Baptiste Coudurier, Benjamin Larsson, Ulion; (c) 2008-2011 Sascha Sommer, Benjamin Larsson. Fetched 2026-11-26 into `port/build/xma-deps/xbox-emulator` and copied verbatim (`UPSTREAM.sha256` holds the hashes of the fetched files). One local change, listed in `third_party/xma/LOCAL_CHANGES.md` (three state accessor methods appended to `src/wmapro.rs`). | LGPL-2.1-or-later (`third_party/xma/COPYING.LGPLv2.1`, `third_party/xma/NOTICE`) |
| `rust/` | Written for this module. Its C ABI follows the shape of leonida's `runtime/xma_decoder` wrapper (one decoder per stream, `catch_unwind` at every entry), but it is a different API: caller-owned output buffers, one packet per call, decoder state export/import. No leonida code is copied. | GPL-3.0-or-later |
| `src/engine.cpp`, `include/gta6rt/xma.h` | Written for this module. The SHAPE XMA context word layout is the one port/shims_gdk's `acphal.cpp` uses (from leonida); field semantics (frame bit offsets, loop end frame plus subframe, loop start subframe skip, initial subframe skip, current buffer, input retirement) were checked against the disassembly of `game_scarlett_master.exe` (STATUS.md section 2) and against Xenia's `src/xenia/apu/xma_context_new.cc` (canary_experimental, read as a reference only, BSD-3-Clause, not copied; fetched to `port/build/xma-deps/xenia-ref`). | GPL-3.0-or-later |
| `tests/`, `tools/` | Written for this module. `tests/synth_xma.h` reuses the frame layout of `port/shims_gdk/tests/acphal_test.cpp` (`write_xma_packet`). `tools/awc_census.py` and `tools/bink_audio_census.py` re-implement the container rules documented in leonida's `docs/audio.md`, `docs/rsc8_audio.md` and `docs/movies.md` (no leonida code is run or copied). | GPL-3.0-or-later |

Not used: FFmpeg itself (the Homebrew ffmpeg is arm64-only and is used only as an offline reference decoder by
`tools/xma_proof.py`; nothing links it), leonida's `runtime/xma_decoder` crate (read only), AudioToolbox (has no
XMA decoder).

Extracted game audio (`port/build/xma-proof/`) is diagnosis data only: it stays in that build directory.
