#!/bin/bash
# usage: d3d12_metal/cache/tools/compile_load.sh NAME [START_S] [STEP_S]
# Frame rate against Metal compiler load: waits for run NAME's world phase (the recorder's "phase ... -> world" line in
# run/logs/NAME.log), then from world+START_S (default 60) runs STEP_S-long steps (default 20) of 0, 1, 2, 3 cold
# compile jobs at utility QoS (each job a prepare_shaders --jobs 1 on its own slice of the world-tier specs, Metal's
# per-user cache off, its own scratch archive), then 3 at default QoS, then 0. Steps go to run/logs/NAME.load.tsv;
# cache/tools/load_fps.py puts them beside the log's "submit phases" frame rates.
port="$(cd "$(dirname "$0")/../../.." && pwd)"
name="$1"; start="${2:-60}"; step="${3:-20}"
log="$port/run/logs/$name.log"; out="$port/run/logs/$name.load.tsv"
specs="$port/build/pcache/tiers/tier-world-2.specs"
scratch="$port/build/pc7/load-scratch"
tool="$port/build/pc7-cache-x86_64/prepare_shaders"
rm -rf "$scratch"; mkdir -p "$scratch"
grep -v '^#' "$specs" | split -l 170 - "$scratch/slice-"
slices=("$scratch"/slice-*)
for ((i = 0; i < 7200; i++)); do
    grep -q "phase .* -> world at" "$log" 2>/dev/null && break
    sleep 0.5
done
w0=$(date +%s)
printf "step\tjobs\tqos\tstart_epoch\tend_epoch\n" > "$out"
pids=()
stop_all() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; pids=(); }
run_step() {
    local jobs=$1 qos=$2 t0
    stop_all
    t0=$(date +%s)
    for ((j = 0; j < jobs; j++)); do
        mkdir -p "$scratch/a$j-$qos"
        MTL_SHADER_CACHE_SIZE=0 GTA6RT_METAL_COMPILE_QOS_BACKGROUND=$qos "$tool" --specs "${slices[$j]}" \
            --archive-dir "$scratch/a$j-$qos" --jobs 1 --title-jobs 1 --fresh > "$scratch/job$j-$qos.log" 2>&1 &
        pids+=($!)
    done
    sleep "$step"
    printf "%s\t%d\t%s\t%d\t%d\n" "$jobs-$qos" "$jobs" "$qos" "$t0" "$(date +%s)" >> "$out"
}
sleep $((w0 + start - $(date +%s)))
run_step 0 utility
run_step 1 utility
run_step 2 utility
run_step 3 utility
run_step 3 default
run_step 0 utility
stop_all
rm -rf "$scratch"
