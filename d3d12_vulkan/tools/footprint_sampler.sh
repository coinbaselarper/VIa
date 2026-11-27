#!/bin/bash
# usage: d3d12_vulkan/tools/footprint_sampler.sh OUT [SECONDS_BETWEEN] [MAX_SECONDS] [BINARY_PATH]
# Waits for a gta6rt process (the one whose executable is BINARY_PATH when given), then every SECONDS_BETWEEN (default 30) appends to OUT: the time since it started, the
# machine state (load average, swap, memory_pressure level, wired pages) and the process's footprint by category
# (footprint -p: "Owned physical footprint (unmapped) (graphics)" is GPU-private memory, "IOAccelerator (graphics)"
# shared GPU memory, "Untagged" mostly the title's own pages). Passive: it only reads.
out="$1"; every="${2:-30}"; max="${3:-420}"; want="${4:-}"
[ -n "$out" ] || { echo "usage: $0 OUT [EVERY] [MAX]" >&2; exit 2; }
pid=""
w=0
while [ -z "$pid" ] && [ "$w" -lt 1800 ]; do
    w=$((w + 1))
    pid="$(ps -axo pid=,comm= | awk -v want="$want" '{ n = $2; sub(/.*\//, "", n); if (n == "gta6rt" && (want == "" || $2 == want)) { print $1; exit } }')"
    [ -n "$pid" ] || sleep 1
done
[ -n "$pid" ] || { echo "no gta6rt" >"$out"; exit 1; }
start=$(date +%s)
echo "footprint_sampler: pid $pid from $(date '+%H:%M:%S')" >"$out"
while kill -0 "$pid" 2>/dev/null; do
    now=$(( $(date +%s) - start ))
    [ "$now" -gt "$max" ] && break
    {
        echo "=== +${now}s $(date '+%H:%M:%S') load $(sysctl -n vm.loadavg) swap $(sysctl -n vm.swapusage | sed 's/  */ /g')"
        echo "memory_pressure: $(memory_pressure -Q 2>/dev/null | tail -1)"
        vm_stat | awk '/wired down|stored in compressor|occupied by compressor|Pages free/ { gsub(/\./, "", $NF); printf "%s %.0f MiB; ", $0 ~ /wired/ ? "wired" : $0 ~ /stored/ ? "compressed" : $0 ~ /occupied/ ? "compressor" : "free", $NF * 16384 / 1048576 } END { print "" }'
        footprint -p "$pid" 2>/dev/null | awk '/Footprint:/ || /phys_footprint/ || / MB | KB | GB / && NR < 40'
    } >>"$out"
    sleep "$every"
done
echo "footprint_sampler: done at +$(( $(date +%s) - start ))s" >>"$out"
