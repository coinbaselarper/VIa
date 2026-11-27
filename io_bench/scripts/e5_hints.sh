#!/bin/sh
# usage: e5_hints.sh BIN OUT [TIMEOUT_PREFIX]   (F_RDADVISE distance, nocache+advise, evict-behind, coalescing)
B=$1
OUT=$2
T=${3:-}
cd "$(dirname "$0")/.." || exit 1
: > "$OUT"
run() { $T "$B" read "$@" >> "$OUT"; }
for spec in "rpf_levels runs" "ttpf_sample rand" "rpf_core_bvh4 seq"; do
  set -- $spec
  common="shapes/$1.iobs --order $2 --run-mean 16 --limit-mib 384 --trials 3 --backend pread"
  run $common --qd 1,2,4,8 --cache nocache --advise 0,4,16,64,256
  run $common --qd 1,4,8 --cache cache --advise 4,16,64,256
  run $common --qd 1,4,8 --cache cache,nocache --advise 16 --evict-behind 1
  run $common --qd 1,4,8,16 --cache cache,nocache --advise 0,16 --coalesce 0
  run $common --qd 1,4,8,16 --cache cache,nocache --advise 0,16 --coalesce 65536
done
