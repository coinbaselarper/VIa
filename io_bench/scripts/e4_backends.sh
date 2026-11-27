#!/bin/sh
# usage: e4_backends.sh BIN OUT [TIMEOUT_PREFIX]   (mmap and dispatch_io against the replay shapes)
B=$1
OUT=$2
T=${3:-}
cd "$(dirname "$0")/.." || exit 1
: > "$OUT"
run() { $T "$B" read "$@" >> "$OUT"; }
for spec in "rpf_levels runs" "ttpf_sample rand"; do
  set -- $spec
  common="shapes/$1.iobs --order $2 --run-mean 16 --limit-mib 384 --trials 3 --qd 1,4,16,32"
  run $common --backend mmap --cache cache --madv none,random --advise 0
  run $common --backend mmap --cache cache --madv willneed --advise 0,16
  run $common --backend dio --cache cache,nocache --advise 0,16 --hw 0
  run $common --backend dio --cache nocache --advise 0 --hw 65536,1048576
done
