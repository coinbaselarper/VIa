#!/bin/sh
# usage: e9_rosetta_io.sh BIN OUT   (the same configurations on the arm64 and the x86_64 binary; pass the matching timeout prefix for x86_64)
B=$1
OUT=$2
T=${3:-}
cd "$(dirname "$0")/.." || exit 1
: > "$OUT"
run() { $T "$B" "$@" >> "$OUT"; }
run bulk --file levels.rpf --base-gib 30 --span-mib 4096 --bytes-mib 512 --order seq --io 16384,1048576 --backend pread --cache cache,nocache --qd 1,4,16 --trials 3
run bulk --file levels.rpf --base-gib 36 --span-mib 8192 --bytes-mib 256 --order rand --io 4096,65536 --backend pread --cache cache,nocache --qd 1,8,32 --trials 3
run read shapes/rpf_levels.iobs --order runs --limit-mib 384 --trials 3 --backend pread --cache cache,nocache --qd 4,16,32
run read shapes/ttpf_sample.iobs --order rand --limit-mib 384 --trials 3 --backend pread --cache cache,nocache --qd 4,16,32
run read shapes/rpf_levels.iobs --order runs --limit-mib 384 --trials 3 --backend mmap --cache cache --qd 4,16 --madv willneed
