#!/bin/bash
# usage: d3d12_metal/cache/tools/pcache_run.sh NAME SECONDS ARCHIVE_DIR|off [extra gta6rt args...]
# A Metal-path title run for the pipeline-cache measurements: metal_run.sh's S2 run (hidden window, scripted presses,
# run.sh's kill timer) on build/pcache-title-x86_64's binary, with the archive, manifest and bootstrap in ARCHIVE_DIR.
# Only the first lit frame is dumped (plus every D3M_DUMP_EVERY-th present when set). Waits for the machine's gta6rt
# slot (PCACHE_WAIT seconds, default 900). After the run: the log's pipeline-cache lines and machine load.
port="$(cd "$(dirname "$0")/../../.." && pwd)"
name="$1"; secs="${2:-45}"; archive="$3"; shift 3
present="$port/run/present/$name"
busy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) f = 1 } END { exit !f }'; }
for ((w = 0; w < ${PCACHE_WAIT:-1800}; w += 5)); do busy || break; sleep 5; done
if busy; then echo "pcache_run: the gta6rt slot stayed busy" >&2; exit 125; fi
# PCACHE_PAUSE_PIDS: processes stopped for the run (an offline prepare_shaders) and continued after it.
for p in $PCACHE_PAUSE_PIDS; do kill -STOP "$p" 2>/dev/null; done
rm -rf "$present"; mkdir -p "$present"
built="${GTA6RT_BIN:-$port/build/pcache-title-x86_64/gta6rt}"
bin="$port/build/pcache-title-x86_64/run/gta6rt"
mkdir -p "$(dirname "$bin")" && cp -p "$built" "$bin.tmp" && mv -f "$bin.tmp" "$bin" || exit 2
dump=()
[ -n "$D3M_DUMP_EVERY" ] && dump=(--present-dump "$present,$D3M_DUMP_EVERY")
# run.sh refuses (125, no log) when another gta6rt started between the check above and its own: wait again.
for ((try = 0; try < 20; try++)); do
    GTA6RT_BIN="$bin" GTA6RT_GPU=metal GTA6RT_METAL_ARCHIVE_DIR="$archive" LEONIDA_PRESENT_FIRST_LIT="$present" \
        "$port/host/tools/run.sh" "$((secs + 10))" "$name" --trace=0 --hidden-window \
        --watchdog "$secs" --memory-report 15 "${dump[@]}" \
        --input-script "${S2_INPUT_SCRIPT:-a@400}" "$@"
    rc=$?
    [ "$rc" = 125 ] && [ ! -s "$port/run/logs/$name.log" ] || break
    for ((w = 0; w < ${PCACHE_WAIT:-900}; w += 5)); do busy || break; sleep 5; done
done
for p in $PCACHE_PAUSE_PIDS; do kill -CONT "$p" 2>/dev/null; done
log="$port/run/logs/$name.log"
grep -E "^run.sh:|\[host\] stage|first non-black|pipeline archive|\[rt\] pcache|ending the run|Timeout|fatal signal" "$log" | cut -c1-600
exit $rc
