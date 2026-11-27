#!/bin/sh
# usage: e10_shim_ab.sh BENCH_DIR OUT REQ_DIR [TIMEOUT_PREFIX]
# BENCH_DIR (build_shim_ab.py output) holds bench_a (shim as is), bench_b (proposal, no native fd row), bench_b_fd (proposal + native fd row),
# bench_b64_fd / bench_b16_fd (proposal with a 64 / 16 MiB staging window).
# REQ_DIR holds req_levels.tsv, req_bvh4.tsv, req_ttpf.tsv written by `iobench export`.
D=$1
OUT=$2
R=$3
T=${4:-}
: > "$OUT"
run() {
  label=$1; shift
  bin=$1; shift
  req=$1; shift
  q=$1; shift
  env "$@" $T "$D/$bin" "$R/$req" "$q" 3 "$label" 2>/dev/null | grep RESULT | sed "s/^RESULT /RESULT req=$req /" >> "$OUT"
}
for req in req_levels.tsv req_bvh4.tsv req_ttpf.tsv; do
  for q in 1 3 6; do
    run baseline bench_a $req $q A=1
    run proposal_nofd bench_b $req $q A=1
    run proposal bench_b_fd $req $q A=1
  done
  q=3
  run pool0 bench_b_fd $req $q LEONIDA_DSTORAGE_POOL=0
  run pool0_leonida_inflate bench_b_fd $req $q LEONIDA_DSTORAGE_POOL=0 LEONIDA_DSTORAGE_SYSTEM_ZLIB=0
  run pool8 bench_b_fd $req $q LEONIDA_DSTORAGE_POOL=8
  run pool16 bench_b_fd $req $q LEONIDA_DSTORAGE_POOL=16
  run pool32 bench_b_fd $req $q LEONIDA_DSTORAGE_POOL=32
  run leonida_inflate bench_b_fd $req $q LEONIDA_DSTORAGE_SYSTEM_ZLIB=0
  run cache bench_b_fd $req $q LEONIDA_DSTORAGE_NOCACHE=0
  run cache_advise16 bench_b_fd $req $q LEONIDA_DSTORAGE_NOCACHE=0 LEONIDA_DSTORAGE_ADVISE=16
  run nocache_advise16 bench_b_fd $req $q LEONIDA_DSTORAGE_ADVISE=16
  run depth4 bench_b_fd $req $q LEONIDA_DSTORAGE_DEPTH=4
  run depth8 bench_b_fd $req $q LEONIDA_DSTORAGE_DEPTH=8
  run depth64 bench_b_fd $req $q LEONIDA_DSTORAGE_DEPTH=64
  run window64 bench_b64_fd $req $q A=1
  run window16 bench_b16_fd $req $q A=1
done
