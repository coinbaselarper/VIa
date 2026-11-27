#!/bin/sh
# Usage: run_one.sh <timeout_seconds> <binary> [args...]   (runs once, kills after the timeout; prints rc)
t=$1; shift
"$@" > ${TMPDIR:-/tmp}/gdk_run_out.$$ 2>&1 &
pid=$!
n=0
while kill -0 $pid 2>/dev/null; do
  sleep 1; n=$((n+1))
  if [ $n -ge $t ]; then kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null; echo "TIMEOUT after ${t}s"; tail -5 ${TMPDIR:-/tmp}/gdk_run_out.$$; rm -f ${TMPDIR:-/tmp}/gdk_run_out.$$; exit 124; fi
done
wait $pid; rc=$?
cat ${TMPDIR:-/tmp}/gdk_run_out.$$; rm -f ${TMPDIR:-/tmp}/gdk_run_out.$$
echo "rc=$rc"; exit $rc
