#!/bin/sh
# usage: e13_ab_confirm.sh AB_DIR REQ_DIR OUT [ROUNDS] [TIMEOUT_PREFIX]
# Interleaved baseline (bench_a) / live backend (bench_b_fd) replay through the real shim, 3 queues, 3 repetitions per cell, ROUNDS rounds.
D=$1
R=$2
OUT=$3
N=${4:-2}
T=${5:-}
: > "$OUT"
i=1
while [ "$i" -le "$N" ]; do
  for req in req_levels.tsv req_bvh4.tsv req_ttpf.tsv; do
    for v in a b_fd; do
      $T "$D/bench_$v" "$R/$req" 3 3 "round$i" 2>/dev/null | grep RESULT | sed "s/^RESULT /RESULT req=$req variant=$v /" >> "$OUT"
    done
  done
  i=$((i + 1))
done
