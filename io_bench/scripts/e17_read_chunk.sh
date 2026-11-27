#!/bin/bash
# usage: e17_read_chunk.sh AB_DIR REQ_DIR OUT [ROUNDS] [TIMEOUT_PREFIX]
# The live backend with the per-pread cap off (LEONIDA_DSTORAGE_READ_CHUNK_KIB=0) against the default (1 MiB under Rosetta), interleaved; 3 queues, 3 reps.
D=$1; R=$2; OUT=$3; N=${4:-2}; T=${5:-}
for ((i = 1; i <= N; i++)); do
  for req in req_levels.tsv req_bvh4.tsv req_ttpf.tsv req_title1.tsv; do
    q=3; [ "$req" = req_title1.tsv ] && q=17
    for cfg in "nocap LEONIDA_DSTORAGE_READ_CHUNK_KIB=0" "cap1m LEONIDA_DSTORAGE_READ_CHUNK_KIB=1024"; do
      set -- $cfg; label=$1; shift
      env "$@" $T "$D/bench_b_fd" "$R/$req" $q 3 "$label-r$i" 2>/dev/null | grep RESULT | sed "s/^RESULT /RESULT req=$req variant=$label /" >> "$OUT"
    done
  done
done
