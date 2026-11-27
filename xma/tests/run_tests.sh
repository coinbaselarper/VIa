#!/bin/bash
# usage: xma/tests/run_tests.sh BUILD_DIR [TIMEOUT_S]   (serial, kill timer per test)
b="$1"; limit="${2:-120}"
[ -x "$b/xma_engine_test" ] || { echo "no $b/xma_engine_test" >&2; exit 2; }
rc=0
for t in xma_engine_test; do
    "$b/$t" > "$b/$t.log" 2>&1 & p=$!
    for ((i = 0; i < limit * 10; i++)); do kill -0 $p 2>/dev/null || break; sleep 0.1; done
    kill -9 $p 2>/dev/null; wait $p; r=$?
    echo "$t rc=$r :: $(tail -1 "$b/$t.log")"
    [ $r = 0 ] || rc=1
done
exit $rc
