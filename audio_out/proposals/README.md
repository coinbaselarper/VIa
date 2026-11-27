# Patches for files outside port/audio_out (not applied)

All are `patch -p1` from `port/`, made with `diff -u` against the sources as they stood on 2026-11-26 and checked with
`patch -p1 --dry-run` against the current tree (see STATUS.md, "Integrator steps", for the order and the checks).

| file | changes | needed for |
|---|---|---|
| `01-silent_audio-host-output.patch` | `shims_gdk/src/silent_audio.cpp`: with `GTA6RT_HAVE_AUDIO_OUT` and `GTA6RT_AUDIO=on\|null`, the spatial stream mixes its static objects (per-object buffers, object volume) into a 7.1 bed and queues it on a `gta6rt_audio_out` stream; its event is set by a pacer thread that follows the output's consumption (target padding `GTA6RT_AUDIO_TARGET_MS`, default 50 ms). IAudioClient streams (48 kHz PCM 16/24/32 or float, 1..8 ch) do the same with padding/position from the output and the title's volumes. `GTA6RT_AUDIO` unset or `off`: the code path is the old one, unchanged | sound at all |
| `02-cmake-audio_out.patch` | `CMakeLists.txt`: `add_subdirectory(audio_out)` after `portable`; `shims_gdk/CMakeLists.txt`: link `gta6rt_audio_out` and define `GTA6RT_HAVE_AUDIO_OUT` when the target exists, plus the two tests of patch 04 | building 01/03/04 |
| `03-xapu-opus-decode.patch` | `shims_gdk/src/xapu_chad.cpp`: XApu command type 3 (one Opus packet to float PCM in the slot's output) decoded with AudioToolbox, the type 3 result with the byte count at +0x34; type 1/2 reset the slot's decoder. On only with `GTA6RT_AUDIO=on\|null` (or `GTA6RT_XAPU_OPUS=1`); otherwise exactly the old behaviour (no type 3 result) with a named log line | UI sounds on the boot path; most in-game sound (Opus) |
| `04-shims_gdk-tests.patch` | new `shims_gdk/tests/audio_out_spatial_test.cpp`, `shims_gdk/tests/xapu_opus_test.cpp` | checks for 01 and 03 |
| `diag-apu-counters.patch` | `acphal.cpp`/`xapu_chad.cpp` counters and command dumps used for STATUS.md section 1 | diagnosis only; do not integrate |

The two XMA fixes (voices that stay ended after running dry; half-filled blocks that click) are already a ready patch
from the media worker: `port/media/patches/acphal-xma-fixes.patch` (tests `media_xma_fix_test_{patched,unpatched}`). It
still applies to the current `acphal.cpp` (`patch -p1 --dry-run` from `port/`: ok) and is independent of these patches.
