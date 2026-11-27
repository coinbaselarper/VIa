#!/bin/bash
# usage: tools/play.sh [--vulkan] [--dry-run] [--force] [--run-dir DIR] [--import-saves DIR] [-- extra gta6rt options]
# Starts the game in a window with sound, the overlay and keyboard/controller input. Quit with Cmd+Q.
# Paths come from build/setup/config.sh (written by tools/setup.sh), or PLAY_CONFIG=FILE, and can be overridden with
# PLAY_EXE, PLAY_PACKAGE, PLAY_METAL_BIN (never rebuilt) and PLAY_RUN_DIR.
# Default: the native Metal path, with the guards every Metal test run uses:
#   - a memory footprint cap of 60% of the machine's memory (54% while a compiler or shader build runs);
#   - a GPU command buffer running over 2 s ends the run; the first GPU error ends the run;
#   - the machine's wired memory is checked 4 times a second and the run is ended at 40% of the memory (start refused
#     at 33%), or when wired plus compressed memory passes 62.5%: wired memory cannot be paged out, and running out of
#     it has hung this kind of Mac.
#   These fractions are the values used on a 24 GB Mac (14,848 / 13,312 / 9,728 / 8,192 / 15,360 MiB), scaled.
# --vulkan: the Vulkan path (build/macos/gta6rt from the README build; needs GTA6RT_VK_DRIVER = an x86_64 MoltenVK
#   dylib and LEONIDA_SHADERS = a translated corpus for Apple Vulkan drivers). It does not draw the world.
# --run-dir DIR: writable state (saves, settings, pipeline archive, logs); default run/play in this checkout.
# --import-saves DIR: import a save set first (host/tools/s3_import_saves.py) into the run dir.
# --force: start on a Mac with less than 24 GB of memory (untested; likely to end on the guards).
# PLAY_METAL_POOL=0: the title pool off (no world geometry, about 2 GiB less wired memory).
# Exit codes: 3 GPU error, 4 footprint cap, 5 command buffer over 2 s, 137 stopped by a guard here.
here="$(cd "$(dirname "$0")" && pwd)"
port="$(cd "$here/.." && pwd)"

gpu=metal; dry=0; force=0; run_dir="${PLAY_RUN_DIR:-}"; import=""; pass=()
while [ $# -gt 0 ]; do
    case "$1" in
        --vulkan) gpu=vulkan ;;
        --metal) gpu=metal ;;
        --dry-run) dry=1 ;;
        --force) force=1 ;;
        --run-dir) run_dir="$2"; shift ;;
        --import-saves) import="$2"; shift ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --) shift; pass+=("$@"); break ;;
        *) pass+=("$1") ;;
    esac
    shift
done

fail() { echo "play.sh: $1" >&2; exit "${2:-1}"; }
warn() { echo "play.sh: warning: $1" >&2; }

config="${PLAY_CONFIG:-$port/build/setup/config.sh}"
SETUP_PACKAGE=""; SETUP_EXE=""; SETUP_BIN=""; GTA6RT_SOURCE_ROOT_CFG=""; SPIRV_CFG=""; HISTORY_CFG=""
if [ -f "$config" ]; then
    while IFS='=' read -r k v; do
        case "$k" in ''|\#*) continue ;; esac
        v="$(eval "printf '%s' $v")"
        case "$k" in
            SETUP_PACKAGE) SETUP_PACKAGE="$v" ;;
            SETUP_EXE) SETUP_EXE="$v" ;;
            SETUP_BIN) SETUP_BIN="$v" ;;
            GTA6RT_SOURCE_ROOT) GTA6RT_SOURCE_ROOT_CFG="$v" ;;
            GTA6RT_METAL_SPIRV) SPIRV_CFG="$v" ;;
            LEONIDA_DSTORAGE_HISTORY) HISTORY_CFG="$v" ;;
        esac
    done <"$config"
fi
exe="${PLAY_EXE:-$SETUP_EXE}"
package="${PLAY_PACKAGE:-$SETUP_PACKAGE}"
[ -n "$exe" ] && [ -n "$package" ] || fail "no game paths: run tools/setup.sh --package DIR --exe FILE first, or set PLAY_EXE and PLAY_PACKAGE"
[ -f "$exe" ] || fail "the executable $exe does not exist"
[ -d "$package" ] || fail "the package folder $package does not exist"
source_root="${GTA6RT_SOURCE_ROOT:-${GTA6RT_SOURCE_ROOT_CFG:-$port}}"
run_dir="${run_dir:-$port/run/play}"

if [ "$gpu" = metal ]; then
    bin="${PLAY_METAL_BIN:-${SETUP_BIN:-$port/build/metal-play-x86_64/bin/gta6rt}}"
    log="$run_dir/logs/play-metal.log"
else
    bin="${PLAY_VULKAN_BIN:-$port/build/macos/gta6rt}"
    log="$run_dir/logs/play-vulkan.log"
fi

# ---- preflight ----
[ "$(uname -s)" = Darwin ] || fail "this runs on macOS only"
[ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ] || fail "this Mac does not have Apple silicon; only Apple silicon is supported (Metal mesh shaders, unified memory)"
/usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null || fail "Rosetta 2 is not installed (the game's code runs through it): softwareupdate --install-rosetta"
ram_bytes=$(sysctl -n hw.memsize)
ram_mib=$((ram_bytes / 1048576))
if [ "$ram_mib" -lt 24576 ]; then
    if [ "$force" = 1 ]; then
        warn "this Mac has $((ram_mib / 1024)) GB of memory. Only a 24 GB Mac has run this; the game uses 12-13.5 GB and about 6.7 GB of it wired. Expect the memory guards to end the run, or the machine to stall."
    else
        fail "this Mac has $((ram_mib / 1024)) GB of memory; 24 GB is the least that has been run (the game needs 12-13.5 GB, about 6.7 GB of it wired). --force starts anyway, untested."
    fi
fi
[ -x "$bin" ] || fail "no binary at $bin; build it: $( [ "$gpu" = metal ] && echo "d3d12_metal/tools/build_play.sh" || echo "see README.md, Build")"
minos=$(otool -l "$bin" 2>/dev/null | awk '/LC_BUILD_VERSION/ {b = 1} b && $1 == "minos" {print $2; exit}')
os=$(sw_vers -productVersion)
ver_lt() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | head -1)" = "$1" ] && [ "$1" != "$2" ]; }
if [ -n "$minos" ] && ver_lt "$os" "$minos"; then
    fail "this binary needs macOS $minos or later; this Mac runs $os"
fi
if [ "$gpu" = metal ]; then
    [ -f "$source_root/build/shaderc_msl/runs/main/manifest.tsv" ] || fail "no shader store at $source_root/build/shaderc_msl: run tools/setup.sh"
    [ -d "$source_root/build/pc7/arch-world" ] || warn "no pipeline tier ($source_root/build/pc7/arch-world): the world stays dark while its pipelines compile, a minute or more"
else
    [ -n "${GTA6RT_VK_DRIVER:-}" ] || fail "--vulkan needs GTA6RT_VK_DRIVER (an x86_64 MoltenVK libMoltenVK.dylib)"
    [ -n "${LEONIDA_SHADERS:-}" ] || warn "LEONIDA_SHADERS is not set: every pipeline with a shader fails on the Vulkan path"
fi

set_env=()
setenv() { export "$1=$2"; set_env+=("$1"); }
setenv GTA6RT_SOURCE_ROOT "$source_root"
setenv GTA6RT_AUDIO "${GTA6RT_AUDIO:-on}"
setenv LEONIDA_SAVE_DIR "$run_dir/saves"
[ -n "$SPIRV_CFG" ] && [ -z "${GTA6RT_METAL_SPIRV:-}" ] && setenv GTA6RT_METAL_SPIRV "$SPIRV_CFG"
[ -n "$HISTORY_CFG" ] && [ -f "$HISTORY_CFG" ] && [ -z "${LEONIDA_DSTORAGE_HISTORY:-}" ] && setenv LEONIDA_DSTORAGE_HISTORY "$HISTORY_CFG"
cmd=("$bin" --exe "$exe" --package "$package" --run-dir "$run_dir" --trace=0 --hud "${GTA6RT_HUD:-min}" --memory-report 30 "${pass[@]}")

scale() { echo $((ram_mib * $1 / 24576)); }
wired_mib() { vm_stat | awk '/wired down/ {gsub(/\./,"",$4); print int($4*16384/1048576)}'; }
machine_mib() { vm_stat | awk '/wired down/ {gsub(/\./,"",$4); w=$4} /occupied by compressor/ {gsub(/\./,"",$5); c=$5} END {print int((w+c)*16384/1048576)}'; }
wired_stop=$(scale 9728); wired_start=$(scale 8192); machine_stop=$(scale 15360)
heavy=$(ps -axo comm= | grep -E "prepare_shaders|ninja|clang|swift-frontend|metal-objdump|cc1plus|metal$" | grep -v grep | head -1)
cap=$(scale 14848); [ -n "$heavy" ] && cap=$(scale 13312)
if [ "$gpu" = metal ]; then
    setenv GTA6RT_GPU metal
    setenv GTA6RT_METAL_FOOTPRINT_CAP_MB "${GTA6RT_METAL_FOOTPRINT_CAP_MB:-$cap}"
    setenv GTA6RT_METAL_CB_ABORT_MS "${GTA6RT_METAL_CB_ABORT_MS:-2000}"
    setenv GTA6RT_METAL_ARCHIVE_DIR "${GTA6RT_METAL_ARCHIVE_DIR:-$run_dir/metal-archive}"
    unset GTA6RT_METAL_GPU_ERRORS
    [ "${PLAY_METAL_POOL:-1}" = 0 ] && setenv GTA6RT_METAL_TITLE_POOL 0
fi

if [ "$dry" = 1 ]; then
    echo "path: $gpu; binary $bin (needs macOS ${minos:-?}, this Mac $os, $((ram_mib / 1024)) GB)"
    echo "config: $([ -f "$config" ] && echo "$config" || echo none)"
    echo "run dir: $run_dir; log: $log"
    [ -n "$import" ] && echo "saves: import $import into $run_dir/saves first"
    if [ "$gpu" = metal ]; then
        echo "guards: footprint cap $GTA6RT_METAL_FOOTPRINT_CAP_MB MiB (heavy job now: ${heavy:-none}); command buffer > $GTA6RT_METAL_CB_ABORT_MS ms ends the run;"
        echo "        first GPU error ends the run; wired now $(wired_mib) MiB: start refused at $wired_start, run ended at $wired_stop (4 Hz),"
        echo "        or at wired + compressed $machine_stop MiB (now $(machine_mib))"
    fi
    echo "environment set:"
    for v in "${set_env[@]}"; do printf '  %s=%q\n' "$v" "${!v}"; done
    [ "$gpu" = metal ] && echo "  unset GTA6RT_METAL_GPU_ERRORS"
    printf 'command:'; printf ' %q' "${cmd[@]}"; printf ' >%q 2>&1\n' "$log"
    exit 0
fi

waited=0
while ps -axo comm= | grep -q gta6rt; do
    [ "$waited" = 0 ] && echo "another copy of the game is running; waiting for it to finish..."
    waited=1
    sleep 3
done
mkdir -p "$run_dir/logs" "$run_dir/saves" || fail "cannot create $run_dir"
if [ -n "$import" ]; then
    python3 -I "$port/host/tools/s3_import_saves.py" "$import" --saves "$run_dir/saves" \
        --storage "$run_dir/overlay/.gta6rt-persistent" >"$run_dir/logs/import-saves.log" 2>&1 ||
        fail "the save import failed; see $run_dir/logs/import-saves.log"
fi

if [ "$gpu" = vulkan ]; then
    echo "starting (Vulkan); the window stays black for 15-40 s while it boots. Log: $log"
    exec "${cmd[@]}" >"$log" 2>&1
fi

w=$(wired_mib)
[ "$w" -ge "$wired_start" ] && fail "the machine already has $w MiB of wired memory (the run is ended at $wired_stop MiB); close other heavy apps first"
mkdir -p "$GTA6RT_METAL_ARCHIVE_DIR"
echo "starting on Metal (footprint cap $GTA6RT_METAL_FOOTPRINT_CAP_MB MiB, wired stop $wired_stop MiB). Log: $log"
echo "  The window stays black for 15-40 s while it boots, then the menus."
echo "  In the world: about 2 frames/s; parts of the frame are missing until every pipeline is ready."
echo "  Keyboard: W/A/S/D move, I/J/K/L camera, Space/Return = A, Backspace = B, Escape = pause; GTA6RT_MOUSE=on for the mouse."
"${cmd[@]}" >"$log" 2>&1 &
pid=$!
trap 'kill -9 $pid 2>/dev/null' INT TERM HUP
(
    seen=0
    while kill -0 $pid 2>/dev/null; do
        w=$(wired_mib)
        m=$(machine_mib)
        if [ "$w" -ge "$wired_stop" ] || [ "$m" -gt "$machine_stop" ]; then
            echo "[play] machine wired $w MiB (stop $wired_stop), wired + compressed $m MiB (stop $machine_stop) at $(date +%T): run ended" >>"$log"
            echo "stopped: the machine's wired memory reached $w MiB (wired + compressed $m MiB); this protects the Mac from a hang"
            kill -9 $pid 2>/dev/null
            break
        fi
        size=$(stat -f %z "$log" 2>/dev/null || echo 0)
        if [ "$size" -gt "$seen" ]; then
            if tail -c +$((seen + 1)) "$log" | head -c $((size - seen)) |
                grep -qE "^\[metal\].*(command buffer [0-9]+ failed|FAULTED|kIOGPUCommandBufferCallbackError)"; then
                echo "[play] GPU error at $(date +%T): run ended" >>"$log"
                echo "stopped: the GPU reported an error (see $log)"
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
case $rc in
    0) ;;
    3) echo "stopped: GPU error (the run ends at the first one). Log: $log" ;;
    4) echo "stopped: the game's memory footprint passed $GTA6RT_METAL_FOOTPRINT_CAP_MB MiB. Log: $log" ;;
    5) echo "stopped: a GPU command buffer ran over $GTA6RT_METAL_CB_ABORT_MS ms. Log: $log" ;;
    137) ;;
    *) echo "the game ended with code $rc. Log: $log" ;;
esac
exit $rc
