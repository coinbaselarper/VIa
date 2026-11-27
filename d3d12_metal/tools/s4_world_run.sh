#!/bin/bash
# usage: d3d12_metal/tools/s4_world_run.sh NAME SECONDS [ENV=value ...]   (S4_BIN: another binary; S3_DUMP_EVERY)
# host/tools/s3_storyload.sh's imported-save flow (A every 5 s from 40 s), with the import and input script prepared
# before the slot is taken (slot_lib.sh: probe lock, per-run binary copy, cap, CB abort, guards), so run.sh starts the
# moment the slot is free.
. "$(dirname "$0")/slot_lib.sh"
port="$SLOT_PORT"
root="$(cd "$port/.." && pwd)"
name="$1"; secs="$2"; shift 2
scratch=$port/run/logs
present="$port/run/present/$name"; log="$port/run/logs/$name.log"; shapes="$port/run/logs/$name.dstorage.tsv"
tool="$port/build/macos/tools/screen_text"
savesrc="$root/saves/manual-save-2026-11-26/extracted/Manual Save/victory/data/7952987A"
"$port/io_bench/scripts/ensure_bcpack_history.sh" >/dev/null || exit 2
python3 -I "$port/host/tools/s3_import_saves.py" "$savesrc" >"$port/run/logs/$name.import.log" 2>&1 || exit 2
export LEONIDA_SAVE_DIR="$port/run/saves"
script="$(python3 -I -c 'import sys; s=int(sys.argv[1]); print(",".join("a@%d" % t for t in range(40, s - 19, 5)))' "$secs")"
for attempt in $(seq 1 200); do
  slot_take "$name"
  slot_copy ${S4_BIN:-$port/build/d3m-s4-x86_64/gta6rt} || exit 2
  rm -rf "$present"; mkdir -p "$present"; rm -f "$shapes"
  start=$(date +%s)
  echo "attempt $attempt $SLOT_START_LINE copy $SLOT_BIN" >> $scratch/$name.meta
  env $(slot_env) GTA6RT_BIN=$SLOT_BIN GTA6RT_GPU=metal LEONIDA_PRESENT_FIRST_LIT="$present" \
      LEONIDA_DSTORAGE_SHAPES="$shapes" "$@" "$port/host/tools/run.sh" "$((secs + 15))" "$name" --hidden-window \
      --trace=0 --trace-match BinkOpenWithOptions,BinkClose --watchdog "$secs" --memory-report 30 \
      --present-dump "$present,${S3_DUMP_EVERY:-300}" --peek 0x7446510,0x98f828c,0x7453ed0,0x77ae260:4,0x77ae340:8 \
      --title-watchdog on --input-script "$script" &
  rpid=$!
  sleep 3
  if slot_collided 2>> $scratch/$name.meta; then
    wait $rpid; rm -f "$log"; slot_finish $scratch/$name.meta; sleep $((10 + RANDOM % 20)); continue
  fi
  slot_guard $rpid "$log" $scratch/$name.meta
  ( log stream --style compact --level default --predicate 'process == "gta6rt" AND (messageType == error OR messageType == fault)' > $scratch/$name.unified.log 2>&1 ) &
  lpid=$!
  wait $rpid; rc=$?
  pkill -P $lpid 2>/dev/null; kill $lpid 2>/dev/null
  echo "attempt $attempt rc $rc $(date +%T) load $(sysctl -n vm.loadavg)" >> $scratch/$name.meta
  [ $rc -eq 125 ] && { slot_finish $scratch/$name.meta; continue; }
  break
done
slot_finish $scratch/$name.meta
echo "[s3] saves: imported (LEONIDA_SAVE_DIR=$LEONIDA_SAVE_DIR)" >> "$log"
python3 -I "$port/host/tools/s3_evidence.py" "$log" "$present" "$start" "$tool" "$shapes"
