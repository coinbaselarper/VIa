#!/bin/sh
# Runs every metal test binary of one build tree under the Metal API and shader validation layers, one at a time.
# usage: tools/run_validation.sh <build tree> [log]   (x86_64 trees run under Rosetta; never start two of these)
set -u
tree=$(cd "${1:?build tree}" && pwd) || exit 2
log=${2:-$tree/validation.log}
case $log in /*) ;; *) log="$PWD/$log" ;; esac
manifest="$tree/route_a/manifest.txt"
pcache="$tree/pipeline-cache-validation"
rm -rf "$pcache"
: > "$log"
fails=0
run() {
    echo "=== $*" >> "$log"
    env MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 "$@" >> "$log" 2>&1 &
    pid=$!
    ( sleep 600; kill -9 $pid 2>/dev/null ) &
    watchdog=$!
    wait $pid
    rc=$?
    kill $watchdog 2>/dev/null
    wait $watchdog 2>/dev/null
    echo "=== exit $rc" >> "$log"
    [ $rc -eq 0 ] || fails=$((fails + 1))
}
cd "$tree" || exit 2
run ./metal_test_device
run ./metal_test_triangle
run ./metal_test_rect_list
run ./metal_test_present
run ./metal_test_game_shaders "$manifest"
run ./metal_test_compute "$manifest"
run ./metal_test_draw2
run ./metal_test_mesh "$manifest"
run ./metal_test_hostmem
run ./metal_test_present2
run ./metal_test_memory
run ./metal_test_hazards --only bindless,wait_split,heap_base --iterations 40 --heavy 400
run env MTL_SHADER_CACHE_SIZE=0 ./metal_test_pipeline_cache --phase cold --dir "$pcache" "$manifest"
run env MTL_SHADER_CACHE_SIZE=0 ./metal_test_pipeline_cache --phase warm --dir "$pcache" "$manifest"
run env MTL_SHADER_CACHE_SIZE=0 ./metal_test_pipeline_cache --phase warm2 --dir "$pcache" "$manifest"
run env MTL_SHADER_CACHE_SIZE=0 ./metal_test_pipeline_cache --phase async "$manifest"
echo "binaries failing under validation: $fails" >> "$log"
echo "binaries failing under validation: $fails"
exit $fails
