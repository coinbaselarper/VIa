#!/bin/sh
# Runs the os unit tests of one build tree serially, each under a timeout (no timeout(1) on macOS).
# usage: run_tests.sh <build-tree> [timeout-seconds] [test ...]
tree=${1:?build tree}
limit=${2:-120}
shift 2 2>/dev/null
tests=${*:-"vm sync threads files misc"}
logdir="$tree/os/test-logs"
mkdir -p "$logdir"
fail=0
for t in $tests; do
    bin="$tree/os/os_test_$t"
    log="$logdir/$t.log"
    "$bin" >"$log" 2>&1 &
    pid=$!
    n=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$n" -ge "$limit" ]; then
            kill -9 "$pid" 2>/dev/null
            echo "TIMEOUT after ${limit}s" >>"$log"
            break
        fi
        sleep 1
        n=$((n + 1))
    done
    wait "$pid"
    rc=$?
    echo "os_test_$t rc=$rc ${n}s: $(tail -1 "$log")"
    [ "$rc" -eq 0 ] || fail=1
done
exit $fail
