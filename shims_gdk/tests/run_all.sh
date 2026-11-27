#!/bin/sh
# Usage: run_all.sh <build-tree/shims_gdk> [timeout_seconds]   (serial; one process at a time, each killed after the timeout)
dir=$1; t=${2:-180}
here=$(cd "$(dirname "$0")" && pwd)
pass=0; fail=0
run() {
  name=$1; shift
  out=$("$here/run_one.sh" "$t" "$@" 2>&1); rc=$?
  last=$(printf '%s\n' "$out" | grep -v '^\[rt\]' | tail -2 | tr '\n' ' ' | cut -c1-160)
  if [ $rc -eq 0 ]; then pass=$((pass+1)); echo "PASS $name :: $last"; else fail=$((fail+1)); echo "FAIL $name rc=$rc :: $last"; fi
}
for b in api_ref_state api_global_refs context_pinning context_rundown context_reentrant context_concurrent_detach \
         context_attach_detach_stress xgameruntime xgameruntime_user xgameruntime_gamesave xgameruntime_store \
         xgameruntime_offline dstorage acphal xapu_chad silent_audio gameinput_mac gameinput_pad; do
  run $b "$dir/${b}_test"
done
run acphal_noncached_only "$dir/acphal_test" --noncached-only
run gameinput_mac_script "$dir/gameinput_mac_test" --input-script
run gameinput_mac_synthetic "$dir/gameinput_mac_test" --synthetic
run gameinput_mac_census "$dir/gameinput_mac_test" --census
run gameinput_mac_sticky "$dir/gameinput_mac_test" --sticky
run gameinput_mac_late "$dir/gameinput_mac_test" --late
run gameinput_mac_alongside "$dir/gameinput_mac_test" --alongside
for c in unregister undispatched terminate parked self presignaled monitor close composite snapshot cross concurrent \
         held_snapshot admission cancel_unregister reentrant_admission reentrant_vs_external_unregister \
         multi_dispatcher_terminate multi_dispatcher_unregister_replace; do
  run wait_registration_$c "$dir/wait_registration_test" $c
done
run real_core "$dir/gdk_real_core_test"
run link_check "$dir/gdk_link_check"
echo "TOTAL pass=$pass fail=$fail"
