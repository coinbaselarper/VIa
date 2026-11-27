#!/bin/bash
# usage: packaging/make_play_app.sh [--out DIR] [--workspace DIR] [--version V] [--min-macos 14.0]
# Assembles <out>/GTA6RT-Play.app: a thin launcher (play_launcher.sh) over the workspace, ad-hoc codesigned. No game
# data, binaries, shader libraries or pipeline archives are copied; Contents/Resources/workspace names the workspace.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
port="$(cd "$here/.." && pwd)"
out="$port/build/packaging"
ws="$(cd "$port/.." && pwd)"
version="0.2.0"
min="14.0"
while [ $# -gt 0 ]; do
    case "$1" in
        --out) out="$2"; shift 2 ;;
        --workspace) ws="$(cd "$2" && pwd)"; shift 2 ;;
        --version) version="$2"; shift 2 ;;
        --min-macos) min="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -f "$ws/port/d3d12_metal/tools/build_play.sh" ] || { echo "$ws is not a workspace (no port/d3d12_metal/tools/build_play.sh)" >&2; exit 1; }

app="$out/GTA6RT-Play.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
sed -e "s|@BUNDLE_ID@|org.gta6rt.play|" -e "s|@VERSION@|$version|" -e "s|@MIN_MACOS@|$min|" \
    "$here/Info.plist.in" >"$app/Contents/Info.plist"
plutil -lint "$app/Contents/Info.plist" >/dev/null
install -m 755 "$here/play_launcher.sh" "$app/Contents/MacOS/GTA6RT"
echo "$ws" >"$app/Contents/Resources/workspace"
codesign --force --sign - "$app"
codesign --verify --deep --strict "$app"
echo "built $app (workspace $ws)"
