#!/bin/sh
# usage: e6b_arch_ab.sh ARM64_BIN X86_64_BIN OUT   (interleaved single-thread arm64 vs Rosetta, 3 rounds)
A=$1
X=$2
OUT=$3
cd "$(dirname "$0")/.." || exit 1
: > "$OUT"
for round in 1 2 3; do
  for arch in arm64 x86_64; do
    if [ $arch = arm64 ]; then B=$A; P=; else B=$X; P="scripts/with_timeout.sh 300"; fi
    for spec in "rpf_levels zlib libz,leonida,libdeflate 64" "rpf_core_bvh4 zlib libz,leonida,libdeflate 32" "ttpf_sample bcpack1 leonida 12"; do
      set -- $spec
      $P "$B" decode shapes/$1.iobs --codec $2 --impl $3 --threads 1 --limit-mib $4 --reps 2 | sed "s/^RESULT /RESULT arch=$arch round=$round /" >> "$OUT"
    done
  done
done
