#!/bin/bash
# usage: tools/ci/title_smoke.sh [SECONDS] [extra gta6rt args...]
# Opt-in S1 check: runs the gta6rt host to WinMain via host/tools/run.sh (one x86_64 process, kill timer).
# Not called by run_all.sh. Exit 0 only if the log shows the WinMain stop.
here="$(cd "$(dirname "$0")/../.." && pwd)"
secs=${1:-60}; [ $# -gt 0 ] && shift
name="title-smoke-$(date +%Y%m%d-%H%M%S)"
"$here/host/tools/run.sh" "$secs" "$name" --stop-at-winmain "$@"
rc=$?
log="$here/run/logs/$name.log"
if [ $rc -eq 125 ]; then echo "title_smoke: SKIP another gta6rt is running"; exit 125; fi
if grep -qi "winmain" "$log" 2>/dev/null && [ $rc -eq 0 ]; then
    echo "title_smoke: PASS (rc=0, WinMain reported) log=$log"; exit 0
fi
echo "title_smoke: FAIL rc=$rc log=$log"; tail -n 5 "$log" 2>/dev/null; exit 1
