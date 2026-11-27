#!/bin/sh
# usage: with_timeout.sh SECONDS command [args...]   (no timeout(1) on this machine)
limit=$1
shift
"$@" &
pid=$!
(
  i=0
  while [ $i -lt "$limit" ]; do
    sleep 1
    kill -0 $pid 2>/dev/null || exit 0
    i=$((i + 1))
  done
  echo "with_timeout: killing $pid after ${limit}s" >&2
  kill -9 $pid 2>/dev/null
) &
watcher=$!
wait $pid
rc=$?
kill $watcher 2>/dev/null
exit $rc
