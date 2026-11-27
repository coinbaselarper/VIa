#!/bin/sh
# usage: make_requests.sh BIN OUTDIR   writes the request lists scripts/e10_shim_ab.sh and e12_raw_ab.sh replay through the shim
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
mkdir -p "$2" || exit 1
OUT=$(cd "$2" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
G=${IOB_GAME:-/Users/a/Desktop/Projects/GTA-VI/xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK}
cd "$ROOT" || exit 1
"$BIN" export shapes/rpf_levels.iobs --order runs --limit-mib 128 > "$OUT/req_levels.tsv"
"$BIN" export shapes/rpf_core_bvh4.iobs --order seq --limit-mib 48 > "$OUT/req_bvh4.tsv"
"$BIN" export shapes/ttpf_sample.iobs --order rand --limit-mib 128 > "$OUT/req_ttpf.tsv"
awk -v g="$G/levels.rpf" 'BEGIN { b = 30 * 1073741824; for (i = 0; i < 384; i++) printf "%s\t%d\t%d\t%d\n", g, b + i * 1048576, 1048576, 1048576 }' > "$OUT/req_raw_1m.tsv"
awk -v g="$G/levels.rpf" 'BEGIN { b = 31 * 1073741824; for (i = 0; i < 4096; i++) printf "%s\t%d\t%d\t%d\n", g, b + i * 65536, 65536, 65536 }' > "$OUT/req_raw_64k.tsv"
wc -l "$OUT"/req_*.tsv
