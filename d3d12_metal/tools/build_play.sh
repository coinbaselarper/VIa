#!/bin/bash
# usage: d3d12_metal/tools/build_play.sh [--check] [--quiet]
# Builds the canonical playable Metal binary from the live sources into port/build/metal-play-x86_64 (attach.cmake
# super-build, x86_64, RelWithDebInfo), incrementally, at low priority, and publishes it with an atomic rename to
# port/build/metal-play-x86_64/bin/gta6rt (a running copy keeps its inode). The last line printed is that path.
# Other workers' trees are untouched; two invocations of this script do not build at once (the second waits).
# Records what it built from in port/build/metal-play-x86_64/bin/source-stamp (mtime = when the build started, so an
# edit made during the build counts as newer) and source-stamp.txt (newest source file, its time, the build times).
# --check: no build; exit 0 when the published binary is current, 1 when sources are newer (or the tree is out of
# date), 2 when there is no published binary. Prints one line saying which.
# BUILD_PLAY_JOBS (default 6) and BUILD_PLAY_NICE (default 10) set ninja's parallelism and the nice level.
set -u
port="$(cd "$(dirname "$0")/../.." && pwd)"
tree="$port/build/metal-play-x86_64"
pub="$tree/bin"
log="$port/build/metal-play-x86_64-build.log"
jobs="${BUILD_PLAY_JOBS:-6}"
nicelvl="${BUILD_PLAY_NICE:-10}"
check=0; quiet=0
for a in "$@"; do
    case "$a" in
        --check) check=1 ;;
        --quiet) quiet=1 ;;
        *) echo "build_play.sh: unknown option $a" >&2; exit 64 ;;
    esac
done
say() { [ "$quiet" = 1 ] || echo "$@" >&2; }

newest_source() {
    find "$port" \( -path "$port/build" -o -path "$port/run" -o -path "$port/.git" -o -name repo -o -name tests \) -prune -o \
        -type f \( -name '*.cpp' -o -name '*.mm' -o -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.m' \
        -o -name '*.inc' -o -name 'CMakeLists.txt' -o -name '*.cmake' -o -name '*.def' -o -name '*.metal' -o -name '*.S' \) \
        -exec stat -f '%m %N' {} + 2>/dev/null | sort -n | tail -1
}

newer_than_stamp() {
    find "$port" \( -path "$port/build" -o -path "$port/run" -o -path "$port/.git" -o -name repo -o -name tests \) -prune -o \
        -type f \( -name '*.cpp' -o -name '*.mm' -o -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.m' \
        -o -name '*.inc' -o -name 'CMakeLists.txt' -o -name '*.cmake' -o -name '*.def' -o -name '*.metal' -o -name '*.S' \) \
        -newer "$pub/source-stamp" -print 2>/dev/null | head -1
}

if [ "$check" = 1 ]; then
    if [ ! -x "$pub/gta6rt" ] || [ ! -f "$pub/source-stamp" ]; then echo "no published build at $pub/gta6rt"; exit 2; fi
    n=$(newer_than_stamp)
    if [ -n "$n" ]; then echo "sources newer than the build: ${n#$port/}"; exit 1; fi
    echo "build current ($(sed -n 's/^built_from_start=//p' "$pub/source-stamp.txt" 2>/dev/null))"
    exit 0
fi

lock="$tree.lock"
mkdir -p "$port/build"
waited=0
until mkdir "$lock" 2>/dev/null; do
    holder=$(cat "$lock/pid" 2>/dev/null)
    if [ -n "$holder" ] && ! kill -0 "$holder" 2>/dev/null; then rm -rf "$lock"; continue; fi
    [ "$waited" = 0 ] && say "another build_play.sh (pid ${holder:-?}) is building; waiting"
    waited=1
    sleep 2
done
echo $$ >"$lock/pid"
trap 'rm -rf "$lock"' EXIT

mkdir -p "$pub"
start_epoch=$(date +%s)
start_file="$tree/.build-start"
touch "$start_file"
src=$(newest_source)

echo "build_play.sh $(date '+%F %T') pid $$" >"$log"
if [ ! -f "$tree/build.ninja" ]; then
    say "configuring $tree"
    nice -n "$nicelvl" cmake -S "$port" -B "$tree" -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES="$port/d3d12_metal/superbuild/attach.cmake" >>"$log" 2>&1 ||
        { echo "build_play.sh: configure failed; see $log" >&2; exit 1; }
fi

say "building gta6rt in $tree (nice $nicelvl, $jobs jobs; log $log)"
if ! nice -n "$nicelvl" ninja -C "$tree" -j "$jobs" gta6rt >>"$log" 2>&1; then
    grep -E "error:|FAILED" "$log" | tail -5 >&2
    echo "build_play.sh: build failed; the published binary is unchanged; see $log" >&2
    exit 1
fi

if [ ! -x "$pub/gta6rt" ] || ! cmp -s "$tree/gta6rt" "$pub/gta6rt"; then
    cp -p "$tree/gta6rt" "$pub/.gta6rt.new" && mv -f "$pub/.gta6rt.new" "$pub/gta6rt" ||
        { echo "build_play.sh: could not publish $pub/gta6rt" >&2; exit 1; }
fi
mv -f "$start_file" "$pub/source-stamp"
{
    echo "built_from_start=$(date -r "$start_epoch" '+%Y-%m-%d %H:%M:%S')"
    echo "built_end=$(date '+%Y-%m-%d %H:%M:%S')"
    echo "newest_source=$(date -r "${src%% *}" '+%Y-%m-%d %H:%M:%S') ${src#* }"
    echo "binary_sha256=$(shasum -a 256 "$pub/gta6rt" | cut -d' ' -f1)"
} >"$pub/source-stamp.txt"
say "$(sed -n 's/^newest_source=/newest source: /p' "$pub/source-stamp.txt")"
echo "$pub/gta6rt"
