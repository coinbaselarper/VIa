#!/bin/bash
# usage: d3d12_metal/tools/s7_world_run.sh NAME SECONDS [ENV=value ...]   (S4_BIN: another binary; S3_DUMP_EVERY)
# s4_world_run.sh with a private run directory: saves, persistent storage, registry, drives and the Metal pipeline
# archive live in build/d3m-s4-x86_64/rundir, reset from rundir-template (the imported user set) before every run,
# so port/run/saves and port/run/overlay are never written. One gta6rt at a time: the slot is polled and a run that
# collided with another title in its first seconds is killed and retried after a back-off.
port="$(cd "$(dirname "$0")/../.." && pwd)"
root="$(cd "$port/.." && pwd)"
name="$1"; secs="$2"; shift 2
tree=$port/build/d3m-s4-x86_64
rd=$tree/rundir
tpl=$tree/rundir-template
scratch=$port/run/logs
present="$port/run/present/$name"; log="$port/run/logs/$name.log"; shapes="$port/run/logs/$name.dstorage.tsv"
tool="$port/build/macos/tools/screen_text"
savesrc="$root/saves/manual-save-2026-11-26/extracted/Manual Save/victory/data/7952987A"
"$port/io_bench/scripts/ensure_bcpack_history.sh" >/dev/null || exit 2
if [ ! -d "$tpl/saves/local-user" ]; then
  mkdir -p "$tpl/saves" "$tpl/overlay/.gta6rt-persistent"
  python3 -I "$port/host/tools/s3_import_saves.py" "$savesrc" --saves "$tpl/saves" \
      --storage "$tpl/overlay/.gta6rt-persistent" >"$scratch/$name.import.log" 2>&1 || exit 2
fi
mkdir -p "$rd"
if [ ! -d "$rd/pipeline-archive" ]; then
  mkdir -p "$rd/pipeline-archive"
  cp -p "$port/build/d3d12_metal-x86_64/pipeline-archive/"* "$rd/pipeline-archive/" 2>/dev/null
fi
# S7_B_AFTER=T: from T s on, B instead of A every 5 s (B is "GOT IT" on the title's COULDN'T AUTOSAVE page, which A
# retries: with A only the world phase stays behind that page from about +110 s).
script="$(python3 -I -c 'import sys; s, b = int(sys.argv[1]), int(sys.argv[2]); print(",".join(("b@%d" if t >= b else "a@%d") % t for t in range(40, s - 19, 5)))' "$secs" "${S7_B_AFTER:-100000}")"
busy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) f = 1 } END { exit !f }'; }
mkdir -p $tree/run
for attempt in $(seq 1 400); do
  while busy; do sleep 0.2; done
  rm -rf "$rd/saves" "$rd/overlay"
  cp -R "$tpl/saves" "$tpl/overlay" "$rd/"
  cp -p ${S4_BIN:-$tree/gta6rt} $tree/run/gta6rt
  rm -rf "$present"; mkdir -p "$present"; rm -f "$shapes"
  start=$(date +%s)
  env GTA6RT_METAL_FOOTPRINT_CAP_MB=13312 GTA6RT_BIN=$tree/run/gta6rt GTA6RT_GPU=metal LEONIDA_PRESENT_FIRST_LIT="$present" \
      LEONIDA_SAVE_DIR="$rd/saves" GTA6RT_METAL_ARCHIVE_DIR="$rd/pipeline-archive" \
      LEONIDA_DSTORAGE_SHAPES="$shapes" "$@" "$port/host/tools/run.sh" "$((secs + 15))" "$name" --hidden-window \
      --run-dir "$rd" --trace=0 --trace-match BinkOpenWithOptions,BinkClose --watchdog "$secs" --memory-report 30 \
      --present-dump "$present,${S3_DUMP_EVERY:-300}" --peek 0x7446510,0x98f828c,0x7453ed0,0x77ae260:4,0x77ae340:8 \
      --title-watchdog on --input-script "$script" &
  rpid=$!
  sleep 3
  others=$(ps -axo pid=,comm= | awk -v me="$tree/run/gta6rt" '{ n = $2; sub(/.*\//, "", n); if ((n == "gta6rt" || n ~ /^gta6rt_/) && $2 != me) print $1 }')
  if [ -n "$others" ]; then
    pkill -9 -f "$tree/run/gta6rt"; wait $rpid
    echo "attempt $attempt collided with $others at $(date +%T); backing off" >> $scratch/$name.meta
    rm -f "$log"; sleep $((10 + RANDOM % 20)); continue
  fi
  ( log stream --style compact --level default --predicate 'process == "gta6rt" AND (messageType == error OR messageType == fault)' > $scratch/$name.unified.log 2>&1 ) &
  lpid=$!
  wait $rpid; rc=$?
  pkill -P $lpid 2>/dev/null; kill $lpid 2>/dev/null
  echo "attempt $attempt rc $rc $(date +%T) load $(sysctl -n vm.loadavg)" >> $scratch/$name.meta
  [ $rc -eq 125 ] && continue
  break
done
rm -rf "$rd/saves" "$rd/overlay"
echo "[s3] saves: private copy of the imported set (LEONIDA_SAVE_DIR=$rd/saves, reset after the run)" >> "$log"
python3 -I "$port/host/tools/s3_evidence.py" "$log" "$present" "$start" "$tool" "$shapes"
