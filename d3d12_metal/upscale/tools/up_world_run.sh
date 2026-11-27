#!/bin/bash
# usage: d3d12_metal/upscale/tools/up_world_run.sh NAME SECONDS [ENV=value ...]
# One Metal world-phase run (imported save, A every 5 s from 40 s, hidden window, no audio) of build/up7-x86_64 with a
# private run directory: saves and storage are reset from rundir-template before every run, so the shared run/saves and
# run/overlay are never written; the pipeline archive is a private copy. Waits for the machine's gta6rt slot and backs
# off when another title started in the same instant.
port=/Users/a/Desktop/Projects/GTA-VI/port
name="$1"; secs="$2"; shift 2
tree=$port/build/up7-x86_64
rd=$tree/rundir; tpl=$tree/rundir-template; bin=$tree/run/gta6rt
mkdir -p "$tree/run" "$rd"
cp -p "${UP_BIN:-$tree/gta6rt}" "$bin" || exit 2
busy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) f = 1 } END { exit !f }'; }
for attempt in $(seq 1 100); do
  while busy; do sleep 1; done
  rm -rf "$rd/saves" "$rd/overlay"; cp -R "$tpl/saves" "$tpl/overlay" "$rd/"
  env GTA6RT_GPU=metal GTA6RT_METAL_ARCHIVE_DIR="$tree/pipeline-archive" GTA6RT_AUDIO=off "$@" \
      GTA6RT_BIN="$bin" S3_SAVES="$rd/saves" S3_EXTRA="--run-dir $rd" S3_WAIT=5 S3_DUMP_EVERY="${S3_DUMP_EVERY:-300}" \
      "$port/host/tools/s3_storyload.sh" "$name" "$secs" &
  spid=$!
  sleep 8
  others=$(ps -axo pid=,comm= | awk -v me="$bin" '{ n = $2; sub(/.*\//, "", n); if ((n == "gta6rt" || n ~ /^gta6rt_/) && $2 != me) print $1 }')
  if [ -n "$others" ]; then
    pkill -9 -f "$bin"; wait $spid
    echo "attempt $attempt collided with $others at $(date +%T); backing off" >> "$port/run/logs/$name.meta"
    sleep $((10 + RANDOM % 20)); continue
  fi
  wait $spid; rc=$?
  echo "attempt $attempt rc $rc $(date +%T) load $(sysctl -n vm.loadavg)" >> "$port/run/logs/$name.meta"
  exit $rc
done
