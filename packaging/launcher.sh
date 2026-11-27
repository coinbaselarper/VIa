#!/bin/bash
# GTA6RT.app launcher. No game data lives in the bundle: the package and exe are located by config or a first-run picker.
here="$(cd "$(dirname "$0")" && pwd)"
support="$HOME/Library/Application Support/GTA6RT"
conf="$support/config"
logdir="$HOME/Library/Logs/GTA6RT"
mkdir -p "$support" "$logdir"

dialog() { osascript -e "display dialog \"$1\" buttons {\"OK\"} default button 1 with title \"GTA6RT\"" >/dev/null 2>&1 || echo "GTA6RT: $1" >&2; }
pick_folder() { osascript -e "POSIX path of (choose folder with prompt \"$1\")" 2>/dev/null; }
pick_file() { osascript -e "POSIX path of (choose file with prompt \"$1\")" 2>/dev/null; }
conf_get() { [ -f "$conf" ] && sed -n "s/^$1=//p" "$conf" | tail -n 1; }
conf_set() {
    local tmp="$conf.tmp"
    { [ -f "$conf" ] && grep -v "^$1=" "$conf"; echo "$1=$2"; } >"$tmp" 2>/dev/null
    mv "$tmp" "$conf"
}

pass=()
for a in "$@"; do
    case "$a" in
        -psn_*) ;;
        --reset-config) rm -f "$conf" ;;
        --print-config) echo "config: $conf"; [ -f "$conf" ] && cat "$conf"; exit 0 ;;
        *) pass+=("$a") ;;
    esac
done

package="${GTA6RT_PACKAGE:-$(conf_get PACKAGE)}"
exe="${GTA6RT_EXE:-$(conf_get EXE)}"
driver="${GTA6RT_VK_DRIVER:-$(conf_get VK_DRIVER)}"
driver="${driver:-moltenvk}"

if [ -z "$package" ] || [ ! -d "$package" ]; then
    package="$(pick_folder "Select the GTA6 Xbox package folder (read-only, never copied or modified)")"
    package="${package%/}"
    if [ -z "$package" ] || [ ! -d "$package" ]; then dialog "No package folder selected."; exit 1; fi
    conf_set PACKAGE "$package"
fi

if [ -z "$exe" ] || [ ! -f "$exe" ]; then
    exe="$(find "$package" -maxdepth 2 -name game_scarlett_master.exe 2>/dev/null | head -n 1)"
    if [ -z "$exe" ]; then
        exe="$(pick_file "Select game_scarlett_master.exe")"
    fi
    if [ -z "$exe" ] || [ ! -f "$exe" ]; then dialog "No title executable selected."; exit 1; fi
    conf_set EXE "$exe"
fi

case "$driver" in
    moltenvk) driver="$here/../Frameworks/libMoltenVK.dylib" ;;
    kosmickrisp) driver="$here/../Frameworks/libvulkan_kosmickrisp.dylib" ;;
esac

args=(--exe "$exe" --package "$package" --run-dir "$support/run" --vk-driver "$driver")
if [ ! -t 1 ]; then
    exec >>"$logdir/gta6rt.log" 2>&1
fi
exec "$here/gta6rt-host" "${args[@]}" "${pass[@]}"
