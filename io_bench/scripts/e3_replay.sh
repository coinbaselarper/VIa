#!/bin/sh
# usage: e3_replay.sh BIN OUT [TIMEOUT_PREFIX]   (pread sweep over the replay shapes)
B=$1
OUT=$2
T=${3:-}
cd "$(dirname "$0")/.." || exit 1
: > "$OUT"
run() { $T "$B" read "$@" >> "$OUT"; }
for spec in "rpf_levels runs" "ttpf_sample rand" "rpf_core_bvh4 seq" "rpf_anim runs"; do
  set -- $spec
  common="shapes/$1.iobs --order $2 --run-mean 16 --limit-mib 384 --trials 3 --qd 1,2,4,8,16,32"
  run $common --backend pread --cache nocache
  run $common --backend pread --cache cache --rdahead -1,0 --advise 0,16
done
