#!/bin/sh
set -eu
R=$(cd "$(dirname "$0")/../../.." && pwd)
B=$R/port/build/shaderc_msl
for spec in j1:1 j3:3 j1b:1 j3b:3; do
  tag=${spec%%:*}; jobs=${spec##*:}
  d=$B/baseline/$tag
  rm -rf "$d"; mkdir -p "$d"
  ln -s "$B/mesh" "$d/mesh"; ln -s "$B/pipelines" "$d/pipelines"
  echo "== $tag jobs=$jobs load: $(uptime | sed 's/.*load averages*: //')" 
  /usr/bin/time -p python3 "$R/port/shaderc_msl/tools/batch.py" --spirv-dir "$R/spike/s2-make-game/work/shaders/spirv" \
    --build "$d" --jobs $jobs --names-file "$B/baseline/names.txt" --extra-dirs "" --runtime-logs /nonexistent 2>&1 | tail -4
  echo "== $tag done load: $(uptime | sed 's/.*load averages*: //')"
done
