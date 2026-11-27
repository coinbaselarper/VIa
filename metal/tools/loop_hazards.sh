#!/bin/sh
# Repeats ctest with --output-on-failure until metal_hazards fails or RUNS runs are done.
# usage: tools/loop_hazards.sh <build dir> [runs] [ctest -R regex] [out dir name]
# Every run keeps its ctest output and LastTest.log in <build dir>/<out dir name>/.
build=${1:?build dir}
runs=${2:-60}
regex=${3:-}
out="$build/${4:-hazards-loop}"
mkdir -p "$out"
i=0
while [ "$i" -lt "$runs" ]; do
  i=$((i + 1))
  log="$out/run-$i.log"
  { date '+start %Y-%m-%d %H:%M:%S'; uptime; } > "$log"
  if [ -n "$regex" ]; then
    ctest --test-dir "$build" --timeout 300 --output-on-failure -R "$regex" >> "$log" 2>&1
  else
    ctest --test-dir "$build" --timeout 300 --output-on-failure >> "$log" 2>&1
  fi
  rc=$?
  cp "$build/Testing/Temporary/LastTest.log" "$out/run-$i.LastTest.log" 2>/dev/null
  date '+end %H:%M:%S' >> "$log"
  failed=$(grep -E '^[[:space:]]+[0-9]+ - metal_' "$log" | tr -s ' \t' ' ' | tr '\n' ';')
  echo "run $i rc $rc $(date '+%H:%M:%S') $failed" >> "$out/summary.txt"
  if grep -Eq '^[[:space:]]+[0-9]+ - metal_hazards \(' "$log"; then
    echo "metal_hazards failed in run $i" >> "$out/summary.txt"
    exit 1
  fi
done
echo "no metal_hazards failure in $runs runs" >> "$out/summary.txt"
