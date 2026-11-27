#!/bin/bash
# usage: host/tools/s4_metal_world.sh [NAME] [WORLD_SECONDS]      (defaults: s4-metal, 180)
# The S4 run on the native Metal path: the canonical playable binary (d3d12_metal/tools/build_play.sh, rebuilt first
# when the sources are newer; S4_BIN=path skips that) with play.sh metal's settings (GTA6RT_GPU=metal, footprint cap
# 14,848 MiB or 13,312 while a build/shader job runs, GTA6RT_METAL_CB_ABORT_MS=2000, the backend's stop at the first GPU
# error, --trace=0 --hud min --memory-report 30), hidden window, no audio, a private run dir with a fresh copy of the
# imported saves and its own pipeline archive. Ended WORLD_SECONDS after the world phase starts (the title watchdog
# kept on), or by the guards: machine wired memory >= 9,728 MiB (polled 4x a second), a [metal] command-buffer
# failure / FAULTED line (fault_stop.sh), or 150 s without reaching the world.
# Waits for the machine's single gta6rt slot and the tmem probe lock, as the other launchers do.
# Evidence: world presents every S4_DUMP_EVERY-th frame (default 4) into run/present/NAME/world, the HUD log line every
# 5 s, request shapes in run/logs/NAME.dstorage.tsv; afterwards host/tools/s4_evidence.py appends "[s4] " lines to
# run/logs/NAME.log (world entry, still view / HUD, lit presents, camera turns, end, GPU errors, frames/s, memory).
# Input (S4_INPUT_SCRIPT overrides): A every 5 s from 40 to 70 s (landing prompt and start pages; nothing is pressed in
# the world, so the run also says whether the still view needs A), then the right stick right for 3 s every 15 s from
# 120 s (a camera turn once the title has handed over control).
# Exit: 0 when the evidence script ran, 2 on a setup failure.
set -u
port="$(cd "$(dirname "$0")/../.." && pwd)"
root="$(cd "$port/.." && pwd)"
name="${1:-s4-metal}"
world_secs="${2:-180}"
log="$port/run/logs/$name.log"
meta="$port/run/logs/$name.meta"
present="$port/run/present/$name"
shapes="$port/run/logs/$name.dstorage.tsv"
rd="$port/run/s4/$name"
tpl="$port/run/s4/template"
savesrc="$root/saves/manual-save-2026-11-26/extracted/Manual Save/victory/data/7952987A"

if [ -n "${S4_BIN:-}" ]; then
    bin="$S4_BIN"
else
    bin="$("$port/d3d12_metal/tools/build_play.sh" --quiet)" || { echo "s4: build_play.sh failed" >&2; exit 2; }
fi
[ -x "$bin" ] || { echo "s4: no binary at $bin" >&2; exit 2; }

"$port/io_bench/scripts/ensure_bcpack_history.sh" >/dev/null || exit 2
if [ ! -d "$tpl/saves/local-user" ]; then
    rm -rf "$tpl"
    mkdir -p "$tpl/saves" "$tpl/overlay/.gta6rt-persistent"
    python3 -I "$port/host/tools/s3_import_saves.py" "$savesrc" --saves "$tpl/saves" \
        --storage "$tpl/overlay/.gta6rt-persistent" >"$port/run/logs/$name.import.log" 2>&1 || exit 2
fi
mkdir -p "$rd" "$port/run/logs"
if [ ! -d "$rd/pipeline-archive" ]; then
    mkdir -p "$rd/pipeline-archive"
    cp -p "$port/build/d3d12_metal-x86_64/pipeline-archive/"* "$rd/pipeline-archive/" 2>/dev/null
fi
script="${S4_INPUT_SCRIPT:-$(python3 -I -c '
import sys
w = int(sys.argv[1])
p = ["a@%d" % t for t in range(40, 71, 5)] + ["rsright@%d:3" % t for t in range(120, 150 + w, 15)]
print(",".join(p))' "$world_secs")}"

busy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) f = 1 } END { exit !f }'; }
gpu_events() { ls /Library/Logs/DiagnosticReports 2>/dev/null | grep -c gpuEvent-gta6rt; }
wired_mib() { vm_stat | awk '/wired down/ {gsub(/\./,"",$4); print int($4*16384/1048576)}'; }

copy="$rd/gta6rt"
probe_lock="$port/build/tmem-x86_64/.probe.lock"
mkdir -p "$(dirname "$probe_lock")"
until mkdir "$probe_lock" 2>/dev/null; do sleep 1; done
trap 'rmdir "$probe_lock" 2>/dev/null' EXIT
ev0=$(gpu_events)
: >"$meta"
for attempt in $(seq 1 400); do
    while busy; do sleep 0.5; done
    rm -rf "$rd/saves" "$rd/overlay"
    cp -R "$tpl/saves" "$tpl/overlay" "$rd/"
    cp -p "$bin" "$copy"
    rm -rf "$present"; mkdir -p "$present/world"; rm -f "$shapes" "$port/run/logs/$name.wired.tsv" "$log.meta"
    heavy=$(ps -axo comm= | grep -E "prepare_shaders|ninja|clang|swift-frontend|metal-objdump|cc1plus" | grep -v grep | head -1)
    cap=14848; [ -n "$heavy" ] && cap=13312
    w0=$(wired_mib)
    echo "attempt $attempt start $(date +%T) epoch $(date +%s) bin $bin cap $cap (heavy: ${heavy:-none}) load $(sysctl -n vm.loadavg) wired $w0 MiB gpuEvent before $ev0" >>"$meta"
    echo "script $script" >>"$meta"
    env -u GTA6RT_METAL_GPU_ERRORS GTA6RT_BIN="$copy" GTA6RT_GPU=metal GTA6RT_AUDIO=off GTA6RT_HUD_LOG=5 \
        GTA6RT_METAL_FOOTPRINT_CAP_MB="$cap" GTA6RT_METAL_CB_ABORT_MS=2000 \
        GTA6RT_METAL_ARCHIVE_DIR="$rd/pipeline-archive" LEONIDA_SAVE_DIR="$rd/saves" \
        GTA6RT_METAL_SCENE_LOG=1 GTA6RT_METAL_WORLD_PRESENT_DUMP="$present/world,${S4_DUMP_EVERY:-4},100000" \
        LEONIDA_DSTORAGE_SHAPES="$shapes" \
        "$port/host/tools/run.sh" "$((world_secs + 400))" "$name" --hidden-window --run-dir "$rd" \
        --trace=0 --hud min --memory-report 30 --input-script "$script" &
    rpid=$!
    sleep 3
    others=$(ps -axo pid=,comm= | awk -v me="$copy" '{ n = $2; sub(/.*\//, "", n); if ((n == "gta6rt" || n ~ /^gta6rt_/) && $2 != me) print $1 }')
    if [ -n "$others" ]; then
        pkill -9 -f "$copy"; wait $rpid
        echo "attempt $attempt collided with $others at $(date +%T); backing off" >>"$meta"
        rm -f "$log"; sleep $((10 + RANDOM % 20)); continue
    fi
    (
        start=$(date +%s); world_at=""
        while kill -0 $rpid 2>/dev/null; do
            w=$(wired_mib)
            echo "$(date +%s) $w" >>"$port/run/logs/$name.wired.tsv"
            if [ "$w" -ge 9728 ]; then
                echo "end wired $w MiB >= 9728 at $(date +%T)" >>"$meta"; pkill -9 -f "$copy"; break
            fi
            now=$(date +%s)
            if [ -z "$world_at" ] && grep -qE '^\[host\] stage t=[0-9.]+s: world' "$log" 2>/dev/null; then
                world_at=$now; echo "world seen by the launcher at $(date +%T) epoch $now" >>"$meta"
            fi
            if [ -n "$world_at" ] && [ $((now - world_at)) -ge "$world_secs" ]; then
                echo "end watchdog world+${world_secs} s at $(date +%T)" >>"$meta"; pkill -9 -f "$copy"; break
            fi
            if [ -z "$world_at" ] && [ $((now - start)) -ge 150 ]; then
                echo "end no world after 150 s at $(date +%T)" >>"$meta"; pkill -9 -f "$copy"; break
            fi
            sleep 0.25
        done
    ) &
    wpid=$!
    "$port/d3d12_metal/cache/tools/fault_stop.sh" "$log" "$copy" &
    fpid=$!
    wait $rpid; rc=$?
    kill $wpid $fpid 2>/dev/null
    echo "attempt $attempt rc $rc $(date +%T) load $(sysctl -n vm.loadavg)" >>"$meta"
    [ $rc -eq 125 ] && continue
    break
done
rm -rf "$rd/saves" "$rd/overlay"
sleep 5
echo "end $(date +%T) gpuEvent after $(gpu_events) (before $ev0)" >>"$meta"
[ -f "$log.meta" ] && cat "$log.meta" >>"$meta"
python3 -I "$port/host/tools/s4_evidence.py" "$log" "$meta" "$present/world" "$shapes" >>"$log" 2>&1
grep '^\[s4\]' "$log"
exit 0
