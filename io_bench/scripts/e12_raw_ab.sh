#!/bin/sh
# usage: e12_raw_ab.sh BENCH_DIR OUT REQ_DIR   (raw, non-transform requests through the real shim: baseline vs patch)
D=$1
OUT=$2
R=$3
: > "$OUT"
run() {
  label=$1; shift
  bin=$1; shift
  req=$1; shift
  q=$1; shift
  env DSREPLAY_RAW=1 "$@" "$D/$bin" "$R/$req" "$q" 3 "$label" 2>/dev/null | grep RESULT | sed "s/^RESULT /RESULT req=$req /" >> "$OUT"
}
for req in req_levels.tsv req_raw_64k.tsv req_raw_1m.tsv; do
  for q in 1 3; do
    run baseline bench_a $req $q A=1
    run proposal bench_b_fd $req $q A=1
    run proposal_cache bench_b_fd $req $q LEONIDA_DSTORAGE_NOCACHE=0
  done
done
