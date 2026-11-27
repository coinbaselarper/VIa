#!/bin/bash
# usage: e16_pool_vs_title.sh AB_DIR REQ_TSV OUT ARCH BURN [ROUNDS] [TIMEOUT_PREFIX]
# Prepare-pool size (and thread QoS) against BURN busy threads standing in for the title's own threads (tests/dstorage_replay_bench.cpp,
# DSREPLAY_BURN). burn_vs_solo in each RESULT line is the busy threads' progress during the replay relative to running alone: 1.0 means the pool cost them nothing.
D=$1; R=$2; OUT=$3; ARCH=$4; BURN=$5; N=${6:-2}; T=${7:-}
for ((i = 1; i <= N; i++)); do
  for cfg in "pool0 LEONIDA_DSTORAGE_POOL=0" "pool4 LEONIDA_DSTORAGE_POOL=4" "pool8 LEONIDA_DSTORAGE_POOL=8" "pool12 LEONIDA_DSTORAGE_POOL=12" \
             "pool21 LEONIDA_DSTORAGE_POOL=21" "pool32 LEONIDA_DSTORAGE_POOL=32" "pool8_utility LEONIDA_DSTORAGE_POOL=8 LEONIDA_DSTORAGE_POOL_QOS=utility" \
             "pool21_utility LEONIDA_DSTORAGE_POOL=21 LEONIDA_DSTORAGE_POOL_QOS=utility"; do
    set -- $cfg
    label=$1; shift
    env DSREPLAY_BURN="$BURN" "$@" $T "$D/bench_b_fd" "$R" 17 2 "$ARCH-$label-r$i" 2>/dev/null | grep RESULT | sed "s/^RESULT /RESULT arch=$ARCH cfg=$label burn=$BURN /" >> "$OUT"
  done
done
