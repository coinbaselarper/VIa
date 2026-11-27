#!/bin/bash
# usage: d3d12_metal/cache/tools/pcache_world.sh [DIR]
# Cold-then-warm runs into the world phase on one cache directory (default build/pcache/runs/world-c, a copy of a
# directory whose manifest reaches the story load): the first run records the world's first uses, the next two use
# them. Polls the gta6rt slot every 0.5 s (other workers' runs take it back to back).
port="$(cd "$(dirname "$0")/../../.." && pwd)"
dir="${1:-$port/build/pcache/runs/world-c}"
busy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) f = 1 } END { exit !f }'; }
go() {
    local name=$1; shift
    for ((i = 0; i < 7200; i++)); do busy || break; sleep 0.5; done
    echo "=== $name $(date '+%H:%M:%S') load $(sysctl -n vm.loadavg) dir=$dir env: $*"
    env "$@" "$port/d3d12_metal/cache/tools/pcache_run.sh" "$name" 100 "$dir"
    echo "=== $name rc=$?"
}
go pcm-world-rec-c MTL_SHADER_CACHE_SIZE=0 S2_INPUT_SCRIPT=a@40,a@55,a@70,a@85
go pcm-world-warm-c MTL_SHADER_CACHE_SIZE=0 S2_INPUT_SCRIPT=a@40,a@55,a@70,a@85
go pcm-world-warm-d MTL_SHADER_CACHE_SIZE=0 S2_INPUT_SCRIPT=a@40,a@55,a@70,a@85
