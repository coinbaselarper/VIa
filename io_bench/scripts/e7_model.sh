#!/bin/sh
# usage: e7_model.sh BIN OUT [TIMEOUT_PREFIX]   (the shim's queue/helper/staging behaviour against a shared prepare pool, real archive reads + inflate)
B=$1
OUT=$2
T=${3:-}
cd "$(dirname "$0")/.." || exit 1
: > "$OUT"
run() { $T "$B" model "$@" >> "$OUT"; }
for spec in "rpf_levels runs 128" "rpf_core_bvh4 seq 48" "ttpf_sample rand 128"; do
  set -- $spec
  c="shapes/$1.iobs --order $2 --limit-mib $3 --trials 2"
  run $c --design current --impl leonida --queues 1,2,3,6 --window-mib 64
  run $c --design current --impl leonida --queues 3 --window-mib 16,256
  run $c --design current --impl libz --queues 3 --window-mib 64
  run $c --design current --impl leonida --queues 3 --window-mib 64 --zero-fill 0
  run $c --design current --impl libz --queues 3 --window-mib 64 --cache nocache
  run $c --design pool --impl leonida,libz,libdeflate --queues 3 --window-mib 256 --pool 2,4,8,12
  run $c --design pool --impl libz --queues 1,3,6 --window-mib 256 --pool 8
  run $c --design pool --impl libz --queues 3 --window-mib 16,64,256,512 --pool 8
  run $c --design pool --impl libz --queues 3 --window-mib 256 --pool 8 --cache nocache
  run $c --design pool --impl libz --queues 3 --window-mib 256 --pool 8 --cache nocache --advise 16
  run $c --design pool --impl libz --queues 3 --window-mib 256 --pool 8 --cache cache --advise 16
  run $c --design pool --impl libz --queues 3 --window-mib 256 --pool 8 --cache nocache --fresh 0
done
