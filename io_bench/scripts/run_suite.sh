#!/bin/sh
# usage: run_suite.sh BIN OUTDIR [quick|full]
# Runs the io_bench experiments one after another (quick: about 10 minutes here) (never in parallel: they measure each other).
# BIN is build/io_bench-<arch>/iobench. x86_64 binaries run under scripts/with_timeout.sh.
# IOB_GAME overrides the game package folder. Do not run while other IO-heavy work is active.
# The pressure runs (cached mode) fill the page cache by about 4 GiB and push about 2.5 GiB of other processes' memory into the compressor
# on this 24 GiB Mac, which slows every other worker. They are skipped unless IOB_PRESSURE=1 (the nocache and evict-behind modes stay on).
BIN=$1
OUT=$2
MODE=${3:-quick}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
mkdir -p "$OUT"
case "$($BIN sysinfo)" in *arch=x86_64*) T="$HERE/with_timeout.sh 1800" ;; *) T= ;; esac
{ uptime; $T "$BIN" sysinfo; } > "$OUT/env.txt"
cd "$ROOT" || exit 1

if [ "$MODE" = full ]; then
  $T "$BIN" bulk --file levels.rpf --base-gib 30 --span-mib 4096 --bytes-mib 512 --order seq --io 16384,65536,262144,1048576,4194304,16777216 --backend pread --cache cache,nocache --qd 1,2,4,8,16 --trials 3 > "$OUT/bulk_seq.txt"
  $T "$BIN" bulk --file levels.rpf --base-gib 36 --span-mib 8192 --bytes-mib 256 --order rand --io 4096,16384,65536,262144,1048576 --backend pread --cache cache,nocache --qd 1,2,4,8,16,32 --trials 3 > "$OUT/bulk_rand.txt"
  "$HERE/e3_replay.sh" "$BIN" "$OUT/replay_pread.txt" "$T"
  "$HERE/e4_backends.sh" "$BIN" "$OUT/replay_mmap_dio.txt" "$T"
  "$HERE/e5_hints.sh" "$BIN" "$OUT/replay_hints.txt" "$T"
else
  $T "$BIN" bulk --file levels.rpf --base-gib 30 --span-mib 4096 --bytes-mib 256 --order seq --io 65536,1048576 --backend pread --cache cache,nocache --qd 1,4,16 --trials 2 > "$OUT/bulk_seq.txt"
  $T "$BIN" bulk --file levels.rpf --base-gib 36 --span-mib 8192 --bytes-mib 128 --order rand --io 4096,65536 --backend pread --cache cache,nocache --qd 1,8,32 --trials 2 > "$OUT/bulk_rand.txt"
  for s in "rpf_levels runs" "ttpf_sample rand"; do
    set -- $s
    $T "$BIN" read shapes/$1.iobs --order $2 --limit-mib 256 --trials 2 --backend pread --cache cache,nocache --qd 1,8,32 --advise 0,16 >> "$OUT/replay_pread.txt"
  done
fi

"$HERE/e6_decode.sh" "$BIN" "$OUT/decode.txt" "$T"
"$HERE/e7_model.sh" "$BIN" "$OUT/model.txt" "$T"
if [ "${IOB_PRESSURE:-0}" = 1 ]; then
  $T "$BIN" pressure --mode cache --gib 12 --victim-gib 2 --base-gib 8 > "$OUT/pressure.txt" 2>&1
else
  : > "$OUT/pressure.txt"
  echo "pressure --mode cache skipped (it evicts other processes' memory); IOB_PRESSURE=1 runs it" | tee -a "$OUT/pressure.txt"
fi
$T "$BIN" pressure --mode nocache --gib 12 --victim-gib 2 --base-gib 8 >> "$OUT/pressure.txt" 2>&1
$T "$BIN" pressure --mode evict-behind --gib 12 --victim-gib 2 --base-gib 8 >> "$OUT/pressure.txt" 2>&1
echo "done: $OUT"
