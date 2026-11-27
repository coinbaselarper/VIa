#!/bin/bash
# usage: capture_title_shapes.sh NAME [SECONDS] [-- extra gta6rt args]
# One run of the title (GTA6RT_BIN, default port/build/macos/gta6rt) with LEONIDA_DSTORAGE_SHAPES on, a hidden window, the S2 scripted A presses
# and a per-thread CPU census of the title process every 5 s. Waits (up to 20 min) while another gta6rt runs.
# Writes port/run/logs/NAME.log, port/io_bench/results/title_NAME_shapes.tsv and title_NAME_threads.txt.
here="$(cd "$(dirname "$0")/../.." && pwd)"
name=$1
secs=${2:-150}
shift 2 2>/dev/null
[ "$1" = "--" ] && shift
bin="${GTA6RT_BIN:-$here/build/macos/gta6rt}"
res="$here/io_bench/results"
shapes="$res/title_${name}_shapes.tsv"
threads="$res/title_${name}_threads.txt"
rm -f "$shapes" "$threads"
(
  sleep 3
  while :; do
    pid=$(ps -axo pid=,comm= | awk -v b="$bin" '$2 == b { print $1; exit }')
    [ -n "$pid" ] && break
    sleep 1
  done
  while kill -0 "$pid" 2>/dev/null; do
    echo "## t=$(date +%s) pid=$pid" >>"$threads"
    ps -M -p "$pid" -o pcpu,pri,utime,stime >>"$threads" 2>/dev/null
    sleep 5
  done
) &
census=$!
for ((i = 0; i < 240; i++)); do
  GTA6RT_BIN="$bin" LEONIDA_DSTORAGE_SHAPES="$shapes" "$here/host/tools/run.sh" "$((secs + 10))" "$name" --hidden-window \
      --watchdog "$secs" --memory-report 15 --input-script "${S2_INPUT_SCRIPT:-a@40,a@55,a@70,a@85,a@100,a@115}" "$@"
  rc=$?
  [ "$rc" -ne 125 ] && break
  sleep 5
done
kill $census 2>/dev/null
exit $rc
