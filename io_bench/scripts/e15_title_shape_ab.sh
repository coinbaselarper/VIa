#!/bin/bash
# usage: e15_title_shape_ab.sh AB_DIR REQ_TSV OUT ARCH [ROUNDS] [TIMEOUT_PREFIX]
# Baseline (bench_a) against the live backend (bench_b_fd) on a request list written by dstorage_shapes.py --replay-tsv from a real title
# run (one queue per title queue, the title's own request order).
D=$1; R=$2; OUT=$3; ARCH=$4; N=${5:-2}; T=${6:-}
for ((i = 1; i <= N; i++)); do
  for v in a b_fd; do
    $T "$D/bench_$v" "$R" 17 3 "$ARCH-$v-r$i" 2>/dev/null | grep RESULT | sed "s/^RESULT /RESULT arch=$ARCH variant=$v /" >> "$OUT"
  done
done
