#!/bin/bash
# GTA6RT-Play.app launcher: a thin launcher over the workspace with the settings and guards of play.sh. The bundle
# holds no game data, shader binaries or pipeline archives; the binary, shader store, overlays and the pipeline tier
# are the workspace's.
# usage: GTA6RT [--vulkan|--metal] [--dry-run] [newgame] [extra gta6rt options]
# Path: Metal by default; --vulkan or GTA6RT_APP_GPU=vulkan selects the Vulkan path (play.sh's default mode).
# Workspace: GTA6RT_WORKSPACE, else Contents/Resources/workspace (written by make_play_app.sh), else the bundle's
# location when it lies inside a workspace (port/build/packaging/GTA6RT-Play.app).
# --dry-run (or GTA6RT_APP_DRY_RUN=1): prints the decisions, the environment and the exact command; changes nothing.
# GTA6RT_APP_BUILD=0: never rebuild the Metal binary. GTA6RT_APP_DIALOGS=0: no end-of-run dialog.
here="$(cd "$(dirname "$0")" && pwd)"
export PATH="${PATH:+$PATH:}/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

gpu="${GTA6RT_APP_GPU:-metal}"
dry="${GTA6RT_APP_DRY_RUN:-0}"
pass=()
for a in "$@"; do
    case "$a" in
        -psn_*) ;;
        --vulkan) gpu=vulkan ;;
        --metal) gpu=metal ;;
        --dry-run) dry=1 ;;
        *) pass+=("$a") ;;
    esac
done
case "$gpu" in metal|vulkan) ;; *) echo "GTA6RT_APP_GPU must be metal or vulkan, not $gpu" >&2; exit 64 ;; esac

interactive=0; [ -t 1 ] && interactive=1
say() { echo "$@"; }
dialog() {
    [ "$dry" = 1 ] || [ "$interactive" = 1 ] || [ "${GTA6RT_APP_DIALOGS:-1}" = 0 ] && return 0
    osascript -e "display dialog \"$1\" buttons {\"OK\"} default button 1 with title \"GTA6RT\"" >/dev/null 2>&1
}
fail() { say "$1"; dialog "$1"; exit "${2:-1}"; }

ws="${GTA6RT_WORKSPACE:-}"
[ -z "$ws" ] && [ -f "$here/../Resources/workspace" ] && ws="$(head -n 1 "$here/../Resources/workspace")"
if [ -z "$ws" ] || [ ! -d "$ws/port" ]; then
    guess="$(cd "$here/../../../../../.." 2>/dev/null && pwd)"
    [ -n "$guess" ] && [ -d "$guess/port/d3d12_metal" ] && ws="$guess"
fi
[ -n "$ws" ] && [ -f "$ws/port/d3d12_metal/tools/build_play.sh" ] || fail "no workspace found (set GTA6RT_WORKSPACE to the GTA-VI folder)"
root="$(cd "$ws" && pwd)"
port="$root/port"
cd "$root" || exit 1

bin="$port/build/macos/gta6rt"
log="$port/run/logs/app.log"
if [ "$gpu" = metal ]; then
    log="$port/run/logs/app-metal.log"
    if [ -n "${PLAY_METAL_BIN:-}" ]; then
        bin="$PLAY_METAL_BIN"
    else
        builder="$port/d3d12_metal/tools/build_play.sh"
        bin="$port/build/metal-play-x86_64/bin/gta6rt"
        state="$("$builder" --check)"
        st=$?
        if [ "$st" -ne 0 ]; then
            if [ "$dry" = 1 ]; then
                say "build: $state; a real start would run $builder$([ "${GTA6RT_APP_BUILD:-1}" = 0 ] && echo ' (skipped: GTA6RT_APP_BUILD=0)')"
            elif [ "${GTA6RT_APP_BUILD:-1}" = 0 ]; then
                [ "$st" = 2 ] || say "$state; GTA6RT_APP_BUILD=0: using the older build at $bin"
            else
                say "$state. Building the Metal binary from the current sources (1-10 min)..."
                [ "$interactive" = 1 ] || osascript -e 'display notification "Building the Metal binary (1-10 min)" with title "GTA6RT"' >/dev/null 2>&1
                "$builder" >/dev/null || { [ -x "$bin" ] && say "the build failed; using the older build at $bin"; }
            fi
        else
            [ "$dry" = 1 ] && say "build: $state"
        fi
    fi
fi

if [ ! -x "$bin" ]; then
    [ "$gpu" = metal ] && fail "no build at $bin; build one: port/d3d12_metal/tools/build_play.sh"
    fail "no build at $bin"
fi

if [ "$dry" = 1 ]; then
    others=$(ps -axo comm= | grep -c gta6rt)
    [ "$others" -gt 0 ] && say "slot: $others gta6rt running; a real start waits for the slot (3 s polls)"
else
    waited=0
    while ps -axo comm= | grep -q gta6rt; do
        [ "$waited" = 0 ] && say "another copy of the game is running (a worker's test); waiting for it to finish..."
        waited=1
        sleep 3
    done
    "$port/io_bench/scripts/ensure_bcpack_history.sh" >/dev/null 2>&1
fi

set_env=()
setenv() { export "$1=$2"; set_env+=("$1"); }

args=(--exe "$root/exe/game_scarlett_master.exe" --package "$root/xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK")
if [ "${pass[0]:-}" = "newgame" ]; then
    pass=("${pass[@]:1}")
    fresh="$port/run/fresh/play"
    [ "$dry" = 1 ] || mkdir -p "$fresh/saves"
    setenv LEONIDA_SAVE_DIR "$fresh/saves"
    args+=(--run-dir "$fresh")
else
    args+=(--run-dir "$port/run")
    src="$root/saves/manual-save-2026-11-26/extracted/Manual Save/victory/data/7952987A"
    saves="$port/run/play-saves"
    if [ "$dry" = 1 ]; then
        say "saves: import $src into port/run/saves; private copy $saves ($([ -d "$saves/local-user" ] && [ "${PLAY_RESET_SAVES:-0}" != 1 ] && echo exists || echo 'made from port/run/saves'))"
    else
        mkdir -p "$port/run/logs"
        python3 -I "$port/host/tools/s3_import_saves.py" "$src" >"$port/run/logs/play.import.log" 2>&1
        [ "${PLAY_RESET_SAVES:-0}" = 1 ] && rm -rf "$saves"
        if [ ! -d "$saves/local-user" ]; then
            rm -rf "$saves.new"
            cp -R "$port/run/saves" "$saves.new" && rm -rf "$saves" && mv "$saves.new" "$saves" || fail "could not copy the saves to $saves"
        fi
    fi
    setenv LEONIDA_SAVE_DIR "$saves"
fi

setenv GTA6RT_AUDIO "${GTA6RT_AUDIO:-on}"
setenv GTA6RT_SOURCE_ROOT "$port"
cmd=("$bin" --trace=0 --hud "${GTA6RT_HUD:-min}" --memory-report 30 "${args[@]}" "${pass[@]}")

show() {
    say "workspace: $root"
    say "path: $gpu"
    say "log: $log"
    say "environment set by the launcher:"
    for v in "${set_env[@]}"; do printf '  %s=%q\n' "$v" "${!v}"; done
    [ "$gpu" = metal ] && say "  unset GTA6RT_METAL_GPU_ERRORS"
    say "command (cwd $root):"
    printf ' %q' "${cmd[@]}"
    printf ' >%q 2>&1\n' "$log"
}

if [ "$gpu" = vulkan ]; then
    if [ "$dry" = 1 ]; then show; exit 0; fi
    mkdir -p "$port/run/logs"
    say "starting (Vulkan); the window stays black for 15-40 s while it boots. Log: $log"
    exec "${cmd[@]}" >"$log" 2>&1
fi

wired_mib() { vm_stat | awk '/wired down/ {gsub(/\./,"",$4); print int($4*16384/1048576)}'; }
wired_stop=9728
heavy=$(ps -axo comm= | grep -E "prepare_shaders|ninja|clang|swift-frontend|metal-objdump|cc1plus" | grep -v grep | head -1)
cap=14848; [ -n "$heavy" ] && cap=13312
w=$(wired_mib)
archive="$port/run/play-metal-archive"
setenv GTA6RT_GPU metal
setenv GTA6RT_METAL_FOOTPRINT_CAP_MB "${GTA6RT_METAL_FOOTPRINT_CAP_MB:-$cap}"
setenv GTA6RT_METAL_CB_ABORT_MS "${GTA6RT_METAL_CB_ABORT_MS:-2000}"
setenv GTA6RT_METAL_ARCHIVE_DIR "${GTA6RT_METAL_ARCHIVE_DIR:-$archive}"
unset GTA6RT_METAL_GPU_ERRORS
[ "${PLAY_METAL_POOL:-1}" = 0 ] && setenv GTA6RT_METAL_TITLE_POOL 0

if [ "$dry" = 1 ]; then
    say "wired now: $w MiB (start refused at 8192, run ended at $wired_stop, checked 4 times a second)"
    say "heavy job at start: ${heavy:-none} -> footprint cap $cap MiB"
    [ -d "$archive" ] || say "pipeline archive $archive: would be seeded from port/build/d3d12_metal-x86_64/pipeline-archive"
    tier="$port/build/pc7/arch-world"
    say "pipeline tier (read-only, GTA6RT_SOURCE_ROOT): $tier ($([ -d "$tier" ] && ls "$tier" | grep -c '^gta6rt-' || echo 0) archive files)"
    say "guards: GPU error line in the log -> kill; wired >= $wired_stop MiB -> kill; backend exits 3 (GPU error), 4 (footprint), 5 (command buffer > ${GTA6RT_METAL_CB_ABORT_MS} ms)"
    show
    exit 0
fi

[ "$w" -ge 8192 ] && fail "the machine already has $w MiB wired memory (the run is ended at $wired_stop MiB); close other heavy apps first"
if [ ! -d "$archive" ]; then
    mkdir -p "$archive"
    cp -p "$port/build/d3d12_metal-x86_64/pipeline-archive/"* "$archive/" 2>/dev/null
fi
mkdir -p "$port/run/logs"
say "starting on Metal (footprint cap $GTA6RT_METAL_FOOTPRINT_CAP_MB MiB, wired stop $wired_stop MiB$([ "${PLAY_METAL_POOL:-1}" = 0 ] && echo ', title pool off')). Log: $log"
if [ "$interactive" = 1 ]; then
    say "  The window stays black for 15-40 s while it boots, then the menus (about 30 frames/s)."
    say "  Press A / Return through the start pages: the story save then loads (20-25 s, loading screen)."
    say "  In the world: about 30 s until every pipeline is ready and 1-3 frames/s; about 150 frames of a still view"
    say "  with loading tips and no HUD, then a fade to black and the game camera with the HUD: from then on you have control."
    say "  Keyboard: W/A/S/D move, I/J/K/L camera, Space/Return = A, Backspace = B, Escape = pause; GTA6RT_MOUSE=on for the mouse."
    say "  If it stops on wired memory before you get control: PLAY_METAL_POOL=0 (no world geometry)."
fi
"${cmd[@]}" >"$log" 2>&1 &
pid=$!
trap 'kill -9 $pid 2>/dev/null' INT TERM HUP
why="$port/run/logs/app-metal.stop"
rm -f "$why"
(
    seen=0
    while kill -0 $pid 2>/dev/null; do
        w=$(wired_mib)
        if [ "$w" -ge "$wired_stop" ]; then
            echo "[play] machine wired memory $w MiB >= $wired_stop MiB at $(date +%T): run ended" >>"$log"
            echo "stopped: the machine's wired memory reached $w MiB (limit $wired_stop MiB); this protects the Mac from a hang" | tee "$why"
            kill -9 $pid 2>/dev/null
            break
        fi
        size=$(stat -f %z "$log" 2>/dev/null || echo 0)
        if [ "$size" -gt "$seen" ]; then
            if tail -c +$((seen + 1)) "$log" | head -c $((size - seen)) |
                grep -qE "^\[metal\].*(command buffer [0-9]+ failed|FAULTED|kIOGPUCommandBufferCallbackError)"; then
                echo "[play] GPU error at $(date +%T): run ended" >>"$log"
                echo "stopped: the GPU reported an error (see $log)" | tee "$why"
                kill -9 $pid 2>/dev/null
                break
            fi
            seen=$((size > 400 ? size - 400 : 0))
        fi
        sleep 0.25
    done
) &
guard=$!
wait $pid
rc=$?
kill $guard 2>/dev/null
wait $guard 2>/dev/null
msg=""
case $rc in
    0) ;;
    3) msg="stopped: GPU error (the run ends at the first one). Log: $log" ;;
    4) msg="stopped: the game's memory footprint passed $GTA6RT_METAL_FOOTPRINT_CAP_MB MiB. Log: $log" ;;
    5) msg="stopped: a GPU command buffer ran over $GTA6RT_METAL_CB_ABORT_MS ms. Log: $log" ;;
    137) [ -f "$why" ] && msg="$(cat "$why")" ;;
    *) msg="the game ended with code $rc. Log: $log" ;;
esac
if [ -n "$msg" ]; then
    [ "$rc" = 137 ] || say "$msg"
    dialog "$msg"
fi
exit $rc
