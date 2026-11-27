#!/bin/bash
# usage: run_timeout.sh SECONDS cmd args...   (no timeout(1) on this machine; one x86_64 process at a time)
secs=$1; shift
"$@" &
pid=$!
for ((i = 0; i < secs * 10; i++)); do
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid"
        exit $?
    fi
    sleep 0.1
done
echo "run_timeout: killing $* after ${secs}s" >&2
kill -9 "$pid" 2>/dev/null
exit 124
