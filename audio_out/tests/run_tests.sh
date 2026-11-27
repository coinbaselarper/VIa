#!/bin/bash
# usage: audio_out/tests/run_tests.sh BUILD_DIR [TIMEOUT_S] [SCRATCH_DIR]
# Runs the audio_out tests one at a time, each under a kill timer. The device test opens the default output with the
# master muted (silence reaches the speakers; the tap WAV holds the signal).
build="$1"; secs="${2:-90}"; scratch="${3:-$build}"
rc_all=0
run() {
    local name=$1; shift
    "$build/$name" "$@" &
    local pid=$! i
    for ((i = 0; i < secs * 10; i++)); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid"; local rc=$?
            echo "run_tests: $name rc=$rc"
            [ "$rc" -eq 0 ] || rc_all=1
            return
        fi
        sleep 0.1
    done
    kill -9 "$pid" 2>/dev/null
    echo "run_tests: $name killed after ${secs}s"
    rc_all=1
}
run audio_out_downmix_test
run audio_out_src_test
run audio_out_opus_test
run audio_out_stream_test "$scratch/audio_out_stream_test.wav" 8
run audio_out_device_test "$scratch/audio_out_device_test.wav" 10
exit $rc_all
