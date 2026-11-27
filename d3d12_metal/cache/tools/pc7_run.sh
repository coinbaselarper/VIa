#!/bin/bash
# usage: d3d12_metal/cache/tools/pc7_run.sh NAME SECONDS ARCHIVE_DIR [ENV=value ...]
# One Metal-path world run for the pipeline-cache measurements: host/tools/s3_storyload.sh's imported-save flow (A
# every 5 s from 40 s; the same gta6rt arguments) on a copy of build/pc7-x86_64/gta6rt (PC7_BIN: another binary) with
# a private run directory (build/pc7/rundir, reset from build/pc7/rundir-template before every run: the shared
# run/saves and run/overlay are never written), no audio, hidden window, the archive and manifest in ARCHIVE_DIR.
# FSR4's compute PSOs are refused unless the caller sets GTA6RT_METAL_REFUSE_CS (the FSR4 chain ran past the GPU
# watchdog in round 6). Everything is prepared before the machine's gta6rt slot is polled (every 0.1 s); a run that
# collides with another title's start is killed at once and retried. Then s3_evidence.py on the log.
port="$(cd "$(dirname "$0")/../../.." && pwd)"
name="$1"; secs="$2"; archive="$3"; shift 3
rd=$port/build/pc7/rundir
bin=$port/build/pc7/bin/gta6rt
present="$port/run/present/$name"; log="$port/run/logs/$name.log"; shapes="$port/run/logs/$name.dstorage.tsv"
tool="$port/build/macos/tools/screen_text"
mkdir -p "$(dirname "$bin")"
cp -p "${PC7_BIN:-$port/build/pc7-x86_64/gta6rt}" "$bin.tmp" && mv -f "$bin.tmp" "$bin" || exit 2
"$port/io_bench/scripts/ensure_bcpack_history.sh" >/dev/null || exit 2
script="$(python3 -I -c 'import sys; s=int(sys.argv[1]); print(",".join("a@%d" % t for t in range(40, s - 19, 5)))' "$secs")"
busy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) f = 1 } END { exit !f }'; }
others() { ps -axo pid=,comm= | awk -v me="$bin" '{ n = $2; sub(/.*\//, "", n); if ((n == "gta6rt" || n ~ /^gta6rt_/) && $2 != me) print $1 }'; }
for attempt in $(seq 1 200); do
    rm -rf "$rd/saves" "$rd/overlay" "$present"; mkdir -p "$rd" "$present"; rm -f "$shapes" "$log"
    cp -R "$port/build/pc7/rundir-template/saves" "$port/build/pc7/rundir-template/overlay" "$rd/"
    for ((w = 0; w < 72000; w++)); do busy || break; sleep 0.1; done
    start=$(date +%s)
    env GTA6RT_BIN="$bin" GTA6RT_GPU=metal GTA6RT_XAUDIO2_OUTPUT=silent GTA6RT_METAL_REFUSE_CS="${GTA6RT_METAL_REFUSE_CS-fspx/}" \
        GTA6RT_METAL_ARCHIVE_DIR="$archive" LEONIDA_SAVE_DIR="$rd/saves" LEONIDA_PRESENT_FIRST_LIT="$present" \
        LEONIDA_DSTORAGE_SHAPES="$shapes" "$@" "$port/host/tools/run.sh" "$((secs + 15))" "$name" --run-dir "$rd" \
        --hidden-window --trace=0 --trace-match BinkOpenWithOptions,BinkClose --watchdog "$secs" --memory-report 30 \
        --present-dump "$present,${S3_DUMP_EVERY:-600}" --peek 0x7446510,0x98f828c,0x7453ed0,0x77ae260:4,0x77ae340:8 \
        --title-watchdog on --input-script "$script" $PC7_EXTRA &
    rpid=$!
    sleep 2
    if [ -n "$(others)" ]; then
        pkill -9 -f "$bin"; wait $rpid
        echo "pc7_run: $name attempt $attempt collided at $(date +%T); retrying" >> "$port/run/logs/$name.meta"
        sleep $((3 + RANDOM % 5)); continue
    fi
    wait $rpid; rc=$?
    [ -f "$log" ] || { sleep 1; continue; }
    echo "pc7_run: $name attempt $attempt rc $rc $(date +%T) load $(sysctl -n vm.loadavg)" >> "$port/run/logs/$name.meta"
    break
done
rm -rf "$rd/saves" "$rd/overlay"
echo "[s3] saves: private copy of the imported set ($rd, removed after the run)" >> "$log"
[ -x "$tool" ] && python3 -I "$port/host/tools/s3_evidence.py" "$log" "$present" "$start" "$tool" "$shapes" > /dev/null 2>&1
exit $rc
