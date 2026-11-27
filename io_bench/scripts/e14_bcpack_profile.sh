#!/bin/sh
# usage: e14_bcpack_profile.sh BIN OUT [TIMEOUT_PREFIX]
# BC6H / BC7 decode throughput and residual failures by cause over every BPTC chunk of the 57 TTPF pools (data/ttdb and pack/*/data/ttdb),
# without and with the predictor profile. BIN is build/io_bench-<arch>/iobench. The profile is port/build/shaders_vk/apple/bcpack/history.bcph
# (scripts/ensure_bcpack_history.sh writes it). Failed chunk lists go to OUT.failures_bcpackN.tsv.
BIN=$1
OUT=$2
T=${3:-}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
H=${IOB_HISTORY:-$ROOT/../build/shaders_vk/apple/bcpack/history.bcph}
[ -f "$H" ] || { echo "no profile at $H: run scripts/ensure_bcpack_history.sh" >&2; exit 1; }
SHAPE=${IOB_SHAPE_DIR:-$ROOT/../build/io_bench-shapes}/ttpf_all57.iobs
mkdir -p "$(dirname "$SHAPE")"
[ -f "$SHAPE" ] || "$BIN" shapes ttpf --packs 1 --out "$SHAPE" >/dev/null || exit 1
: > "$OUT"
for c in bcpack6 bcpack7; do
  $T "$BIN" decode "$SHAPE" --codec $c --impl leonida --threads 1 --reps 1 --limit-mib 100000 >> "$OUT"
  $T "$BIN" decode "$SHAPE" --codec $c --impl leonida --threads 1 --reps 3 --limit-mib 96 --history "$H" >> "$OUT"
  $T "$BIN" decode "$SHAPE" --codec $c --impl leonida --threads 4,14 --reps 3 --limit-mib 100000 --history "$H" --failures-out "$OUT.failures_$c.tsv" >> "$OUT"
done
