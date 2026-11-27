#!/bin/bash
# usage: d3d12_metal/upscale/tools/up_run.sh NAME SECONDS [ENV=value ...]
# One Metal world-phase run of build/up7-x86_64 (imported save, A every 5 s from 40 s, hidden window, audio off), with
# host/tools/s3_storyload.sh's arguments but everything prepared before the machine's gta6rt slot is polled, so run.sh
# starts the moment it is free. Private run directory: saves and storage reset from rundir-template before every run
# (the shared run/saves and run/overlay are never written); private pipeline archive. Backs off when another title
# started first in the same instant. Afterwards s3_evidence.py appends the [s3] lines.
port=/Users/a/Desktop/Projects/GTA-VI/port
name="$1"; secs="$2"; shift 2
tree=$port/build/up7-x86_64
rd=$tree/rundir; tpl=$tree/rundir-template; bin=$tree/run/gta6rt
present="$port/run/present/$name"; log="$port/run/logs/$name.log"; shapes="$port/run/logs/$name.dstorage.tsv"
tool="$port/build/macos/tools/screen_text"
mkdir -p "$tree/run" "$rd"
cp -p "${UP_BIN:-$tree/gta6rt}" "$bin" || exit 2
"$port/io_bench/scripts/ensure_bcpack_history.sh" >/dev/null || exit 2
script="$(python3 -I -c 'import sys; s=int(sys.argv[1]); print(",".join("a@%d" % t for t in range(40, s - 19, 5)))' "$secs")"
secs_of='function s(t,  a, n, d) { d = 0; if (index(t, "-")) { split(t, a, "-"); d = a[1]; t = a[2] } n = split(t, a, ":"); return d * 86400 + (n == 3 ? a[1] * 3600 + a[2] * 60 + a[3] : a[1] * 60 + a[2]) }'
busy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) f = 1 } END { exit !f }'; }
for attempt in $(seq 1 300); do
  rm -rf "$rd/saves" "$rd/overlay"; cp -R "$tpl/saves" "$tpl/overlay" "$rd/"
  rm -rf "$present"; mkdir -p "$present"; rm -f "$shapes"
  while busy; do sleep 0.1; done
  start=$(date +%s)
  env GTA6RT_GPU=metal GTA6RT_METAL_ARCHIVE_DIR="$tree/pipeline-archive" LEONIDA_SAVE_DIR="$rd/saves" \
      LEONIDA_PRESENT_FIRST_LIT="$present" LEONIDA_DSTORAGE_SHAPES="$shapes" GTA6RT_BIN="$bin" "$@" \
      "$port/host/tools/run.sh" "$((secs + 15))" "$name" --hidden-window --run-dir "$rd" \
      --trace=0 --trace-match BinkOpenWithOptions,BinkClose --watchdog "$secs" --memory-report 30 \
      --present-dump "$present,${S3_DUMP_EVERY:-300}" --peek 0x7446510,0x98f828c,0x7453ed0,0x77ae260:4,0x77ae340:8 \
      --title-watchdog on --input-script "$script" &
  rpid=$!
  sleep 6
  mine=$(ps -axo etime=,comm= | awk -v me="$bin" "$secs_of"' $2 == me { print s($1); exit }')
  others=$(ps -axo pid=,etime=,comm= | awk -v me="$bin" -v m="${mine:-0}" "$secs_of"' { n = $3; sub(/.*\//, "", n); if ((n == "gta6rt" || n ~ /^gta6rt_/) && $3 != me && (m == 0 || s($2) >= m)) print $1 }')
  if [ -z "$mine" ] || [ -n "$others" ]; then
    pkill -9 -f "$bin"; wait $rpid
    echo "attempt $attempt: not started or collided with ${others:-?} at $(date +%T); backing off" >> "$port/run/logs/$name.meta"
    sleep $((2 + RANDOM % 6)); continue
  fi
  wait $rpid; rc=$?
  echo "attempt $attempt rc $rc $(date +%T) load $(sysctl -n vm.loadavg)" >> "$port/run/logs/$name.meta"
  echo "[s3] saves: private copy (LEONIDA_SAVE_DIR=$rd/saves)" >> "$log"
  [ -x "$tool" ] && python3 -I "$port/host/tools/s3_evidence.py" "$log" "$present" "$start" "$tool" "$shapes" >/dev/null 2>&1
  exit $rc
done
