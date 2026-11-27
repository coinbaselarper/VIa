#!/bin/bash
# usage: host/tools/s3_storyload.sh [NAME] [SECONDS]      (defaults: s3-storyload, 300)
# The S3 run: the title from the landing prompt through the first-run pages to the story load, unattended, on the
# default driver (MoltenVK x86_64) with the translated shaders. Every 30th presented frame is dumped as PNG into
# run/present/NAME; the title's stage (game state, session, loading byte: host/src/title_watch.cpp) is logged on each
# change; DirectStorage request shapes go to run/logs/NAME.dstorage.tsv. Afterwards s3_evidence.py appends "[s3] "
# lines to run/logs/NAME.log: stages with times, the text on each still page (macOS Vision), memory over time,
# streaming by archive, pipeline counts.
#
# Environment:
#   S3_SAVES=imported (default)  the user's save set, imported into run/saves by s3_import_saves.py (idempotent), with
#                                its persistent local storage in run/overlay; the default run dir
#   S3_SAVES=empty               a fresh run dir run/fresh/NAME (no saves, no settings: the new-game path); the
#                                pipeline cache stays the shared one
#   S3_FLOW=save|newgame         the default presses (save: A every 5 s from 40 s; newgame: also the legal pages;
#                                default newgame with S3_SAVES=empty)
#   S3_INPUT_SCRIPT=...          the presses, overriding S3_FLOW
#   S3_VISIBLE=1                 a visible window (default --hidden-window; the overlay is then off)
#   S3_TITLE_WATCHDOG=on|off     the title's 30 s hang watchdog (default on: armed; off keeps it kicked)
#   S3_DUMP_EVERY=N              dump every N-th presented frame (default 30)
#   S3_EXTRA="..."               more gta6rt arguments
# Exit: run.sh's code if the run did not reach its watchdog (124 = killed by run.sh's timer too), else 0 when the
# story load was reached ("[s3] story load reached"), 1 when not.
port="$(cd "$(dirname "$0")/../.." && pwd)"
root="$(cd "$port/.." && pwd)"
name="${1:-s3-storyload}"
secs="${2:-300}"
present="$port/run/present/$name"
log="$port/run/logs/$name.log"
shapes="$port/run/logs/$name.dstorage.tsv"
tool="$port/build/macos/tools/screen_text"
savesrc="$root/saves/manual-save-2026-11-26/extracted/Manual Save/victory/data/7952987A"
if [ ! -x "$tool" ] || [ "$port/host/tools/screen_text.swift" -nt "$tool" ]; then
    mkdir -p "$(dirname "$tool")"
    swiftc -O "$port/host/tools/screen_text.swift" -o "$tool" || exit 2
fi
"$port/io_bench/scripts/ensure_bcpack_history.sh" >/dev/null || exit 2
args=()
case "${S3_SAVES:-imported}" in
    imported)
        python3 -I "$port/host/tools/s3_import_saves.py" "$savesrc" >"$port/run/logs/$name.import.log" 2>&1 || {
            cat "$port/run/logs/$name.import.log" >&2; exit 2; }
        export LEONIDA_SAVE_DIR="$port/run/saves" ;;
    empty)
        fresh="$port/run/fresh/$name"
        rm -rf "$fresh"
        mkdir -p "$fresh/saves"
        export LEONIDA_PIPELINE_CACHE_DIR="$port/run/pipeline_cache/libMoltenVK.x86_64"
        export LEONIDA_SAVE_DIR="$fresh/saves"
        args+=(--run-dir "$fresh") ;;
    *)
        export LEONIDA_SAVE_DIR="$S3_SAVES" ;;
esac
[ "${S3_VISIBLE:-0}" = 1 ] || args+=(--hidden-window)
# The imported save skips the first-run and legal pages: A on the landing prompt, then A every 5 s (ignored while
# loading). A new game (S3_SAVES=empty) also needs the legal pages: LEGAL NOTICES (down, down to "Next", A) and the
# terms page (two checkboxes, then Confirm), timed from s3r4-a/b; the terms sequence runs twice in case LEGAL was late.
flow="${S3_FLOW:-$([ "${S3_SAVES:-imported}" = empty ] && echo newgame || echo save)}"
script="${S3_INPUT_SCRIPT:-$(python3 -I - "$secs" "$flow" <<'PY'
import sys
secs, flow = int(sys.argv[1]), sys.argv[2]
p = []
if flow == "newgame":
    p += ["a@%d" % t for t in range(40, 121, 5)]
    p += ["down@128", "down@130", "a@132"]
    for t in (142, 172):
        p += ["up@%d" % t, "up@%.1f" % (t + 1.5), "a@%d" % (t + 3), "down@%d" % (t + 5), "a@%d" % (t + 7),
              "down@%d" % (t + 9), "a@%d" % (t + 11)]
    p += ["a@%d" % t for t in range(190, secs - 19, 5)]
else:
    p += ["a@%d" % t for t in range(40, secs - 19, 5)]
print(",".join(p))
PY
)}"
busy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) f = 1 } END { exit !f }'; }
for ((w = 0; w < ${S3_WAIT:-900}; w += 5)); do busy || break; sleep 5; done
rm -rf "$present"
mkdir -p "$present"
rm -f "$shapes"
start=$(date +%s)
LEONIDA_PRESENT_FIRST_LIT="$present" LEONIDA_DSTORAGE_SHAPES="$shapes" "$port/host/tools/run.sh" "$((secs + 15))" "$name" \
    "${args[@]}" --trace=0 --trace-match BinkOpenWithOptions,BinkClose \
    --watchdog "$secs" --memory-report 30 --present-dump "$present,${S3_DUMP_EVERY:-30}" \
    --peek 0x7446510,0x98f828c,0x7453ed0,0x77ae260:4,0x77ae340:8 --title-watchdog "${S3_TITLE_WATCHDOG:-on}" \
    --input-script "$script" ${S3_EXTRA}
rc=$?
[ -f "$log" ] || exit 2
echo "[s3] saves: ${S3_SAVES:-imported} (LEONIDA_SAVE_DIR=$LEONIDA_SAVE_DIR)" | tee -a "$log"
python3 -I "$port/host/tools/s3_evidence.py" "$log" "$present" "$start" "$tool" "$shapes"
[ "$rc" -eq 124 ] || [ "$rc" -eq 0 ] || exit "$rc"
grep -q '^\[s3\] story load reached' "$log"
