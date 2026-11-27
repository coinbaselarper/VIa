#!/bin/bash
# usage: packaging/make_app.sh [--binary PATH] [--out DIR] [--version V] [--min-macos 14.0] [--hardened] [--allow-minos-mismatch]
# Assembles <out>/GTA6RT.app around the x86_64 host binary; ad-hoc codesigned. Contains no game data.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
port="$(cd "$here/.." && pwd)"
bin="$port/build/macos/gta6rt"
out="$port/build/packaging"
version="0.1.0"
min="14.0"
hardened=""
allow=0
mvk="${GTA6RT_PKG_MOLTENVK:-$port/../spike/s1-vulkan-x86/drivers/moltenvk-1.4.2/libMoltenVK.x86_64.dylib}"
kk="${GTA6RT_PKG_KOSMICKRISP:-$port/../spike/s1-vulkan-x86/drivers/kosmickrisp-26.3.1-x86_64/lib/libvulkan_kosmickrisp.dylib}"
while [ $# -gt 0 ]; do
    case "$1" in
        --binary) bin="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --version) version="$2"; shift 2 ;;
        --min-macos) min="$2"; shift 2 ;;
        --hardened) hardened="--options runtime"; shift ;;
        --allow-minos-mismatch) allow=1; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -x "$bin" ] || { echo "missing binary $bin" >&2; exit 1; }
file "$bin" | grep -q x86_64 || { echo "$bin is not x86_64" >&2; exit 1; }

minos="$(vtool -show-build "$bin" | awk '/minos/{print $2; exit}')"
vers_ge() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n 1)" = "$1" ]; }
if vers_ge "$minos" "$min" && [ "$minos" != "$min" ] && [ $allow -eq 0 ]; then
    echo "binary minos $minos is above the pinned minimum $min; rebuild with -DCMAKE_OSX_DEPLOYMENT_TARGET=$min or pass --allow-minos-mismatch" >&2
    exit 3
fi
[ "$minos" != "$min" ] && echo "warning: binary minos=$minos, Info.plist LSMinimumSystemVersion=$min" >&2

app="$out/GTA6RT.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"
sed -e "s|@BUNDLE_ID@|org.gta6rt.host|" -e "s|@VERSION@|$version|" -e "s|@MIN_MACOS@|$min|" \
    "$here/Info.plist.in" >"$app/Contents/Info.plist"
plutil -lint "$app/Contents/Info.plist" >/dev/null
cp "$bin" "$app/Contents/MacOS/gta6rt-host"
install -m 755 "$here/launcher.sh" "$app/Contents/MacOS/GTA6RT"
cp "$here/entitlements.plist" "$app/Contents/Resources/entitlements.plist"

install_driver() {
    local src="$1" name="$2"
    if [ -f "$src" ]; then
        cp "$src" "$app/Contents/Frameworks/$name"
        chmod u+w "$app/Contents/Frameworks/$name"
        install_name_tool -id "@rpath/$name" "$app/Contents/Frameworks/$name"
        echo "bundled $name from $src"
    else
        echo "warning: driver not found: $src" >&2
    fi
}
install_driver "$mvk" libMoltenVK.dylib
install_driver "$kk" libvulkan_kosmickrisp.dylib

install_name_tool -add_rpath @executable_path/../Frameworks "$app/Contents/MacOS/gta6rt-host"

for f in "$app"/Contents/Frameworks/*.dylib; do
    [ -e "$f" ] && codesign --force --sign - "$f"
done
codesign --force --sign - $hardened --entitlements "$here/entitlements.plist" "$app/Contents/MacOS/gta6rt-host"
codesign --force --sign - $hardened "$app"
codesign --verify --deep --strict "$app"
echo "built $app (binary minos $minos, LSMinimumSystemVersion $min)"
