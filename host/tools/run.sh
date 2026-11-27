#!/bin/bash
# usage: host/tools/run.sh SECONDS LOGNAME [gta6rt args...]
# One host process at a time under a kill timer; output to port/run/logs/LOGNAME.log. GTA6RT_BIN picks another binary.
# Refuses (exit 125) while any process whose executable is named gta6rt or gta6rt_* runs, whatever its build tree.
here="$(cd "$(dirname "$0")/../.." && pwd)"
secs=$1; name=$2; shift 2
bin="${GTA6RT_BIN:-$here/build/macos/gta6rt}"
mkdir -p "$here/run/logs"
log="$here/run/logs/$name.log"
running="$(ps -axo pid=,comm= | awk '{ n = $2; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) print $1, $2 }')"
if [ -n "$running" ]; then echo "run.sh: another gta6rt is running: $running" >&2; exit 125; fi
start=$(date +%s)
echo "run.sh: start $(date '+%H:%M:%S') bin=$bin load $(sysctl -n vm.loadavg)" >"$log"
"$bin" "$@" >>"$log" 2>&1 &
pid=$!
for ((i = 0; i < secs * 10; i++)); do
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid"; rc=$?
        echo "run.sh: rc=$rc after $(( $(date +%s) - start )) s, load $(sysctl -n vm.loadavg)" >>"$log"
        echo "rc=$rc log=$log"
        exit $rc
    fi
    sleep 0.1
done
kill -9 "$pid" 2>/dev/null
echo "run.sh: killed after ${secs}s, load $(sysctl -n vm.loadavg)" >>"$log"
echo "rc=124 (killed) log=$log"
exit 124
