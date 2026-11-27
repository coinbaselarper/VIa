#!/bin/bash
# usage: d3d12_metal/cache/tools/pcache_series.sh landing|far|prepared|prepared2
# The title runs behind STATUS-cache.md, one after another (each waits for the gta6rt slot). Archive directories under
# build/pcache/runs/. A prepare_shaders whose pid is in build/pcache/prepare.pid is sent SIGSTOP during each run (this had
# no effect under the measuring session's sandbox, so offline builds were run between title runs instead).
port="$(cd "$(dirname "$0")/../../.." && pwd)"
runs="$port/build/pcache/runs"
mkdir -p "$runs"
run() {
    local name="$1" secs="$2" dir="$3"; shift 3
    local pause=""
    [ -f "$port/build/pcache/prepare.pid" ] && pause="$(cat "$port/build/pcache/prepare.pid")"
    echo "=== $name $(date '+%H:%M:%S') load $(sysctl -n vm.loadavg) dir=$dir env: $*"
    env "$@" PCACHE_PAUSE_PIDS="$pause" "$port/d3d12_metal/cache/tools/pcache_run.sh" "$name" "$secs" "$dir"
    echo "=== $name rc=$?"
}
case "$1" in
landing)
    rm -rf "$runs/cold-a" "$runs/cold-b" "$runs/sys-a"
    run pcm-cold-a 45 "$runs/cold-a" MTL_SHADER_CACHE_SIZE=0
    run pcm-warm-a 45 "$runs/cold-a" MTL_SHADER_CACHE_SIZE=0
    run pcm-cold-b 45 "$runs/cold-b" MTL_SHADER_CACHE_SIZE=0
    run pcm-warm-b 45 "$runs/cold-b" MTL_SHADER_CACHE_SIZE=0
    run pcm-sys-a 45 "$runs/sys-a"
    run pcm-noprebuild-a 45 "$runs/cold-a" MTL_SHADER_CACHE_SIZE=0 GTA6RT_METAL_PCACHE=0
    ;;
far)
    rm -rf "$runs/far"
    mkdir -p "$runs/far"
    cp "$port/build/pcache/bootstrap/pcache-bootstrap.txt" "$runs/far/"
    run pcm-far-a 100 "$runs/far" MTL_SHADER_CACHE_SIZE=0 S2_INPUT_SCRIPT=a@40,a@55,a@70,a@85
    run pcm-far-b 100 "$runs/far" MTL_SHADER_CACHE_SIZE=0 S2_INPUT_SCRIPT=a@40,a@55,a@70,a@85
    ;;
prepared)
    # prep-a, prep-b: built offline by prepare_shaders from far's manifest; big-a: the world tier's archive (offline,
    # about 100 MB) with far's manifest, to measure a large archive's open, footprint and flush in the title.
    rm -rf "$runs/big-a"; mkdir -p "$runs/big-a"
    cp "$port/build/pcache/tiers/archive-world/"gta6rt-* "$runs/far/pcache-manifest.txt" "$runs/big-a/"
    run pcm-prep-a 100 "$runs/prep-a" MTL_SHADER_CACHE_SIZE=0 S2_INPUT_SCRIPT=a@40,a@55,a@70,a@85
    run pcm-prep-b 100 "$runs/prep-b" MTL_SHADER_CACHE_SIZE=0 S2_INPUT_SCRIPT=a@40,a@55,a@70,a@85
    run pcm-big-a 100 "$runs/big-a" MTL_SHADER_CACHE_SIZE=0 S2_INPUT_SCRIPT=a@40,a@55,a@70,a@85
    ;;
prepared2)
    # Repeat of the offline-prepared pair, and the story-load pair: story-b runs on big-a's directory (world archive +
    # the manifest big-a recorded through the story load) plus the bootstrap.
    for x in a b; do
        rm -rf "$runs/prep-$x"; mkdir -p "$runs/prep-$x"
        cp "$runs/far/pcache-manifest.txt" "$runs/prep-$x/"
        MTL_SHADER_CACHE_SIZE=0 "$port/build/pcache-x86_64/prepare_shaders" --manifest "$runs/prep-$x/pcache-manifest.txt" \
            --archive-dir "$runs/prep-$x" --jobs 3 | grep -E "^built|^archive /"
    done
    cp "$port/build/pcache/bootstrap/pcache-bootstrap.txt" "$runs/big-a/"
    run pcm-prep-a 100 "$runs/prep-a" MTL_SHADER_CACHE_SIZE=0 S2_INPUT_SCRIPT=a@40,a@55,a@70,a@85
    run pcm-story-b 120 "$runs/big-a" MTL_SHADER_CACHE_SIZE=0 S2_INPUT_SCRIPT=a@40,a@55,a@70,a@85
    run pcm-prep-b 100 "$runs/prep-b" MTL_SHADER_CACHE_SIZE=0 S2_INPUT_SCRIPT=a@40,a@55,a@70,a@85
    ;;
*) echo "usage: $0 landing|far|prepared|prepared2" >&2; exit 2 ;;
esac
