#!/bin/sh
# Runs the gta6rt_media tests of one build tree serially, each under a kill timer.
#   media/tests/run_tests.sh build/media-x86_64 [timing-seconds]
# The CoreAudio timing run opens the default output device and writes silence.
set -u
tree=${1:?build tree}
secs=${2:-3}
bin=$tree/media
fails=0
run() {
  name=$1; shift
  printf '=== %s\n' "$name"
  perl -e 'alarm shift; exec @ARGV or die "exec: $!"' 120 "$@"
  rc=$?
  printf '=== %s rc=%d\n' "$name" "$rc"
  [ $rc -eq 0 ] || fails=$((fails + 1))
}
run xaudio2_test "$bin/media_xaudio2_test"
run mf_test "$bin/media_mf_test"
run timing_silent "$bin/media_xaudio2_timing_test" silent "$secs"
run timing_coreaudio "$bin/media_xaudio2_timing_test" coreaudio "$secs"
[ -x "$bin/media_link_check" ] && run link_check "$bin/media_link_check"
[ -x "$bin/media_xma_fix_test_unpatched" ] && run xma_unpatched "$bin/media_xma_fix_test_unpatched"
[ -x "$bin/media_xma_fix_test_patched" ] && run xma_patched "$bin/media_xma_fix_test_patched"
printf 'run_tests: %d failing\n' "$fails"
[ $fails -eq 0 ]
