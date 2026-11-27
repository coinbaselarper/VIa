#!/bin/sh
# usage: e7b_model_pool.sh BIN OUT [TIMEOUT_PREFIX]   (pool size beyond the core count, prefetch hints, evict-behind)
B=$1
OUT=$2
T=${3:-}
cd "$(dirname "$0")/.." || exit 1
: > "$OUT"
run() { $T "$B" model "$@" >> "$OUT"; }
for spec in "rpf_levels runs 128" "rpf_core_bvh4 seq 48" "ttpf_sample rand 128"; do
  set -- $spec
  c="shapes/$1.iobs --order $2 --limit-mib $3 --trials 3 --design pool --impl libz --queues 3 --window-mib 256"
  run $c --cache nocache --pool 8,12,16,24,32
  run $c --cache nocache --pool 12,16,24 --advise 16
  run $c --cache nocache --pool 12,16,24 --advise 16 --evict-behind 1
  run $c --cache nocache --pool 16,24 --evict-behind 1
  run $c --cache cache --pool 16,24
done
