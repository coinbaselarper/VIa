#!/bin/sh
# usage: e6_decode.sh BIN OUT [TIMEOUT_PREFIX]   (inflate and BCPack decode scaling; use scripts/with_timeout.sh for x86_64)
B=$1
OUT=$2
T=${3:-}
cd "$(dirname "$0")/.." || exit 1
: > "$OUT"
run() { $T "$B" decode "$@" >> "$OUT"; }
IMPL=libz,leonida,libdeflate
run shapes/rpf_levels.iobs --codec zlib --impl $IMPL --threads 1,2,4,8,14 --limit-mib 96 --reps 3
run shapes/rpf_core_bvh4.iobs --codec zlib --impl $IMPL --threads 1,2,4,8,14 --limit-mib 48 --reps 3
run shapes/rpf_anim.iobs --codec zlib --impl $IMPL --threads 1,4,8 --limit-mib 96 --reps 3
run shapes/ttpf_sample.iobs --codec zlib --impl $IMPL --threads 1,4,8 --limit-mib 96 --reps 3
run shapes/rpf_levels.iobs --codec zlib --impl leonida-bytewise --threads 1 --limit-mib 48 --reps 3
run shapes/ttpf_sample.iobs --codec bcpack1 --impl leonida --threads 1,2,4,8,14 --limit-mib 24 --reps 3
run shapes/rpf_levels.iobs --codec zlib --impl libz,leonida --threads 1 --limit-mib 96 --reps 3 --fresh 1
