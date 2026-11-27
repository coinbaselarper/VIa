#!/bin/bash
# Run one Metal probe in the gta6rt slot: waits until no gta6rt* process exists and no other probe holds the lock.
# The probe binary is named gta6rt_tmem_probe so title-run scripts that poll the slot wait for it in turn.
lock="$(cd "$(dirname "$0")/../.." && pwd)/build/tmem-x86_64/.probe.lock"
mkdir -p "$(dirname "$lock")"
busy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) f = 1 } END { exit !f }'; }
until mkdir "$lock" 2>/dev/null; do sleep 0.2; done
trap 'rmdir "$lock"' EXIT
while busy; do sleep 0.2; done
"$@"
