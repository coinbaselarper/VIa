#!/bin/bash
# usage: d3d12_metal/tools/world_run.sh NAME WORLD_SECONDS BIN [ENV=value ...]
# One world run on the Metal path under slot_lib.sh's rules (probe lock, one gta6rt at a time, per-run binary copy,
# cap, CB abort, first-GPU-error stop, 4 Hz wired stop, machine guard, gpuEvent count): host/tools/s4_metal_world.sh's
# settings (hidden window, no audio, --trace=0 --hud min --memory-report 30) with a private run directory
# (WORLD_RUN_DIR, default run/s4/NAME; saves reset from run/s4/template before every run, its own pipeline archive
# kept between runs) and extra environment from the arguments. Ends WORLD_SECONDS after the world phase starts, on a
# guard, or 150 s without the world. Input: A every 5 s from 40 to 70 s (WORLD_INPUT_SCRIPT overrides).
# Log run/logs/NAME.log, launcher notes run/logs/NAME.meta. Exit: run.sh's code, 3 when a new gpuEvent report appeared.
set -u
. "$(dirname "$0")/slot_lib.sh"
port="$SLOT_PORT"
name="$1"; world_secs="$2"; bin="$3"; shift 3
rd="${WORLD_RUN_DIR:-$port/run/s4/$name}"
tpl="$port/run/s4/template"
log="$port/run/logs/$name.log"
meta="$port/run/logs/$name.meta"
[ -x "$bin" ] || { echo "world_run: no binary $bin" >&2; exit 2; }
[ -d "$tpl/saves/local-user" ] || { echo "world_run: no s4 template (run host/tools/s4_metal_world.sh once)" >&2; exit 2; }
"$port/io_bench/scripts/ensure_bcpack_history.sh" >/dev/null || exit 2
mkdir -p "$rd" "$port/run/logs"
if [ ! -d "$rd/pipeline-archive" ]; then
    mkdir -p "$rd/pipeline-archive"
    cp -p "$port/build/d3d12_metal-x86_64/pipeline-archive/"* "$rd/pipeline-archive/" 2>/dev/null
fi
script="${WORLD_INPUT_SCRIPT:-a@40,a@45,a@50,a@55,a@60,a@65,a@70}"
: >"$meta"
rc=125
for attempt in $(seq 1 100); do
    slot_take "$name"
    rm -rf "$rd/saves" "$rd/overlay"
    cp -R "$tpl/saves" "$tpl/overlay" "$rd/"
    slot_copy "$bin" || exit 2
    rm -f "$meta.wiredpeak"
    echo "attempt $attempt $SLOT_START_LINE bin $bin copy $SLOT_BIN" >>"$meta"
    echo "env $*" >>"$meta"
    env $(slot_env) GTA6RT_BIN="$SLOT_BIN" GTA6RT_GPU=metal GTA6RT_HUD_LOG=5 \
        GTA6RT_METAL_ARCHIVE_DIR="$rd/pipeline-archive" LEONIDA_SAVE_DIR="$rd/saves" \
        "$@" \
        "$port/host/tools/run.sh" "$((world_secs + 400))" "$name" --hidden-window --run-dir "$rd" \
        --trace=0 --hud min --memory-report 30 --input-script "$script" ${WORLD_EXTRA_ARGS:-} &
    rpid=$!
    sleep 3
    if slot_collided 2>>"$meta"; then
        wait $rpid; rm -f "$log"
        slot_finish "$meta"
        sleep $((10 + RANDOM % 20)); continue
    fi
    slot_guard $rpid "$log" "$meta" "$world_secs" 150
    wait $rpid; rc=$?
    echo "attempt $attempt rc $rc $(date +%T) load $(sysctl -n vm.loadavg) wired peak $(cat "$meta.wiredpeak" 2>/dev/null)" >>"$meta"
    [ $rc -eq 125 ] && { slot_finish "$meta"; continue; }
    break
done
rm -rf "$rd/saves" "$rd/overlay"
sleep 5
slot_finish "$meta" || rc=3
[ -f "$log.meta" ] && cat "$log.meta" >>"$meta"
grep -h "G-buffer check" "$log" 2>/dev/null | tail -1
tail -3 "$meta"
exit $rc
