# Sourced (not run) by every launcher that starts gta6rt or a GPU probe on this machine. One copy of the rules:
#   slot_take NAME        the probe lock (shared with shaderc_msl/tools/tmem_slot.sh), then no gta6rt* process, no
#                         heavy job (up to SLOT_HEAVY_WAIT_S, default 600), machine wired under SLOT_IDLE_WIRED_MB
#                         (4,200) for a clean start; sets SLOT_CAP_MB (14,848, or 13,312 while a heavy job runs)
#   slot_copy BIN         the binary copied to a path of this run alone (SLOT_BIN), so two runs never share a path
#   slot_env              the environment every run gets (cap, CB abort 2000 ms, first-GPU-error stop, machine guard)
#   slot_collided         after the start: another gta6rt* exists (a launcher without the lock): ours is killed
#   slot_guard PID LOG META [WORLD_S] [NOWORLD_S]   in the background: the 4 Hz machine-wired stop (9,728 MiB), the
#                         machine guard (wired + compressor 15,360 MiB), the first [metal] GPU error in LOG, the world
#                         watchdog; each stop is written to META
#   slot_finish           gpuEvent-gta6rt* count before/after into META (exit status 3 when it grew), lock released,
#                         the per-run copy removed
# Launchers call slot_take, slot_copy, start the run with $(slot_env) through host/tools/run.sh, then slot_collided,
# slot_guard, wait, slot_finish. Constants are not overridable on purpose (post-panic rules, run/requests 03:45).

SLOT_PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SLOT_LOCK="$SLOT_PORT/build/tmem-x86_64/.probe.lock"
SLOT_WIRED_STOP_MB=9728
SLOT_GUARD_MB=15360
SLOT_CB_ABORT_MS=2000
SLOT_HEAVY_RE='prepare_shaders|ninja|clang|cc1plus|swift-frontend|metal-objdump|metal-ar|metallib'

slot_busy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) f = 1 } END { exit !f }'; }
slot_heavy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); print n }' | grep -E "^($SLOT_HEAVY_RE)" | head -1; }
slot_wired_mib() { vm_stat | awk '/wired down/ { gsub(/\./, "", $4); print int($4 * 16384 / 1048576) }'; }
slot_compressor_mib() { vm_stat | awk '/occupied by compressor/ { gsub(/\./, "", $5); print int($5 * 16384 / 1048576) }'; }
slot_gpu_events() { ls /Library/Logs/DiagnosticReports 2>/dev/null | grep -c '^gpuEvent-gta6rt'; }

slot_lock_take() {
    mkdir -p "$(dirname "$SLOT_LOCK")"
    local said=0 holder age
    until mkdir "$SLOT_LOCK" 2>/dev/null; do
        holder=$(cat "$SLOT_LOCK/pid" 2>/dev/null)
        if [ -n "$holder" ] && ! kill -0 "$holder" 2>/dev/null; then
            rm -rf "$SLOT_LOCK"; continue
        fi
        age=$(( $(date +%s) - $(stat -f %m "$SLOT_LOCK" 2>/dev/null || date +%s) ))
        if [ -z "$holder" ] && [ "$age" -gt 1800 ] && ! slot_busy; then
            echo "slot: removing a probe lock without holder, ${age} s old, no gta6rt* running" >&2
            rmdir "$SLOT_LOCK" 2>/dev/null; continue
        fi
        [ "$said" = 0 ] && echo "slot: waiting for the probe lock (holder ${holder:-unknown})" >&2
        said=1
        sleep 1
    done
    echo $$ >"$SLOT_LOCK/pid"
}

slot_take() {
    SLOT_NAME="$1"
    SLOT_GPU_EVENTS_BEFORE=$(slot_gpu_events)
    slot_lock_take
    trap 'slot_finish >/dev/null 2>&1' EXIT
    local i heavy
    while slot_busy; do sleep 0.5; done
    for ((i = 0; i < ${SLOT_HEAVY_WAIT_S:-600} * 2; i++)); do [ -z "$(slot_heavy)" ] && break; sleep 0.5; done
    for ((i = 0; i < 480; i++)); do [ "$(slot_wired_mib)" -lt "${SLOT_IDLE_WIRED_MB:-4200}" ] && break; sleep 0.5; done
    while slot_busy; do sleep 0.5; done
    heavy=$(slot_heavy)
    SLOT_CAP_MB=14848
    [ -n "$heavy" ] && SLOT_CAP_MB=13312
    SLOT_HEAVY="${heavy:-none}"
    SLOT_START_LINE="start $(date +%T) epoch $(date +%s) cap $SLOT_CAP_MB (heavy: $SLOT_HEAVY) load $(sysctl -n vm.loadavg) wired $(slot_wired_mib) MiB compressor $(slot_compressor_mib) MiB gpuEvent before $SLOT_GPU_EVENTS_BEFORE"
}

slot_copy() {
    local src="$1"
    [ -x "$src" ] || { echo "slot: no binary $src" >&2; return 2; }
    SLOT_COPY_DIR="$SLOT_PORT/build/run-bins/${SLOT_NAME:-run}.$$.$(date +%s)"
    mkdir -p "$SLOT_COPY_DIR" && cp -p "$src" "$SLOT_COPY_DIR/gta6rt" || return 2
    SLOT_BIN="$SLOT_COPY_DIR/gta6rt"
}

slot_env() {
    echo "-u GTA6RT_METAL_GPU_ERRORS GTA6RT_METAL_FOOTPRINT_CAP_MB=$SLOT_CAP_MB GTA6RT_METAL_CB_ABORT_MS=$SLOT_CB_ABORT_MS GTA6RT_METAL_MACHINE_GUARD_MB=$SLOT_GUARD_MB GTA6RT_AUDIO=off"
}

slot_collided() {
    local others
    others=$(ps -axo pid=,comm= | awk -v me="$SLOT_BIN" '{ n = $2; sub(/.*\//, "", n); if ((n == "gta6rt" || n ~ /^gta6rt_/) && $2 != me) print $1 }')
    [ -z "$others" ] && return 1
    pkill -9 -f "$SLOT_BIN"
    echo "collided with pid $others at $(date +%T); ours killed" >&2
    return 0
}

slot_guard() {
    local pid="$1" log="$2" meta="$3" world_s="${4:-0}" noworld_s="${5:-0}"
    (
        local start world_at="" w c now peak=0 re='^\[metal\].*(command buffer [0-9]+ failed|FAULTED|kIOGPUCommandBufferCallbackError)'
        start=$(date +%s)
        while kill -0 "$pid" 2>/dev/null; do
            w=$(slot_wired_mib); c=$(slot_compressor_mib)
            [ "$w" -gt "$peak" ] && peak=$w && echo "$peak" >"$meta.wiredpeak"
            if [ "$w" -ge "$SLOT_WIRED_STOP_MB" ]; then
                echo "end wired $w MiB >= $SLOT_WIRED_STOP_MB at $(date +%T)" >>"$meta"; pkill -9 -f "$SLOT_BIN"; break
            fi
            if [ $((w + c)) -ge "$SLOT_GUARD_MB" ]; then
                echo "end machine guard: wired $w + compressor $c MiB >= $SLOT_GUARD_MB at $(date +%T)" >>"$meta"
                pkill -9 -f "$SLOT_BIN"; break
            fi
            if grep -qE "$re" "$log" 2>/dev/null; then
                echo "end GPU error at $(date +%T): $(grep -m1 -E "$re" "$log" | cut -c1-200)" >>"$meta"
                pkill -9 -f "$SLOT_BIN"; break
            fi
            now=$(date +%s)
            if [ -z "$world_at" ] && grep -qE '^\[host\] stage t=[0-9.]+s: world' "$log" 2>/dev/null; then
                world_at=$now; echo "world seen by the launcher at $(date +%T) epoch $now" >>"$meta"
            fi
            if [ -n "$world_at" ] && [ "$world_s" -gt 0 ] && [ $((now - world_at)) -ge "$world_s" ]; then
                echo "end watchdog world+${world_s} s at $(date +%T)" >>"$meta"; pkill -9 -f "$SLOT_BIN"; break
            fi
            if [ -z "$world_at" ] && [ "$noworld_s" -gt 0 ] && [ $((now - start)) -ge "$noworld_s" ]; then
                echo "end no world after $noworld_s s at $(date +%T)" >>"$meta"; pkill -9 -f "$SLOT_BIN"; break
            fi
            sleep 0.25
        done
    ) &
    SLOT_GUARD_PID=$!
}

slot_finish() {
    local meta="${1:-}" after rc=0
    [ -n "${SLOT_GUARD_PID:-}" ] && kill "$SLOT_GUARD_PID" 2>/dev/null
    after=$(slot_gpu_events)
    if [ -n "${SLOT_GPU_EVENTS_BEFORE:-}" ] && [ "$after" -gt "$SLOT_GPU_EVENTS_BEFORE" ]; then
        echo "NEW gpuEvent-gta6rt report(s): $SLOT_GPU_EVENTS_BEFORE -> $after; stop queueing runs" >&2
        rc=3
    fi
    [ -n "$meta" ] && echo "end $(date +%T) gpuEvent after $after (before ${SLOT_GPU_EVENTS_BEFORE:-?})" >>"$meta"
    [ -n "${SLOT_COPY_DIR:-}" ] && rm -rf "$SLOT_COPY_DIR"
    SLOT_COPY_DIR=""
    if [ "$(cat "$SLOT_LOCK/pid" 2>/dev/null)" = "$$" ]; then rm -rf "$SLOT_LOCK"; fi
    trap - EXIT
    return $rc
}
