#!/bin/sh
# usage: ensure_bcpack_history.sh [--force] [--game DIR] [--out FILE]
# Writes the BCPack predictor profile the DirectStorage shim loads from <shader folder parent>/bcpack/history.bcph when that file is missing
# (--force: regenerate). The shader folder is port/build/shaders_vk/apple/spirv, so the default output is
# port/build/shaders_vk/apple/bcpack/history.bcph. The profile is fitted from the player's own TTPF pools by leonida/tools/build_dstorage_history.py
# (reads the game folder only; needs python 3.10+ and a C++ compiler for its trainer; several minutes).
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=$(cd "$HERE/../.." && pwd)
ROOT=$(cd "$PORT/.." && pwd)
GAME=${IOB_GAME:-$ROOT/xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK}
OUT=$PORT/build/shaders_vk/apple/bcpack/history.bcph
FORCE=0
while [ $# -gt 0 ]; do
  case $1 in
    --force) FORCE=1 ;;
    --game) GAME=$2; shift ;;
    --out) OUT=$2; shift ;;
    *) echo "usage: $0 [--force] [--game DIR] [--out FILE]" >&2; exit 2 ;;
  esac
  shift
done
PY=${PYTHON:-}
if [ -z "$PY" ]; then
  for c in python3.14 python3.13 python3.12 python3.11 python3.10 python3; do
    for d in /opt/homebrew/bin /usr/local/bin ""; do
      p=$d/$c
      [ -z "$d" ] && p=$(command -v "$c" 2>/dev/null)
      [ -n "$p" ] && [ -x "$p" ] && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null && { PY=$p; break 2; }
    done
  done
fi
[ -n "$PY" ] || { echo "ensure_bcpack_history: needs python 3.10 or newer (the generator uses X | None annotations); set PYTHON=" >&2; exit 1; }
valid() {
  [ -f "$1" ] && [ "$(wc -c < "$1" | tr -d ' ')" = 60936 ] && [ "$(head -c 8 "$1")" = BCPHIST1 ]
}
if [ "$FORCE" = 0 ] && valid "$OUT"; then
  echo "bcpack history present: $OUT"
  exit 0
fi
[ -d "$GAME" ] || { echo "ensure_bcpack_history: game folder $GAME not found (IOB_GAME or --game)" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")" || exit 1
echo "bcpack history: generating $OUT from $GAME"
if [ "$FORCE" = 1 ]; then rm -f "$OUT" "${OUT%.bcph}.bcph.stamp"; fi
nice "$PY" -I -B "$ROOT/leonida/tools/build_dstorage_history.py" "$GAME" --out "$OUT" --if-needed || exit 1
valid "$OUT" || { echo "ensure_bcpack_history: $OUT is not a valid profile after generation" >&2; exit 1; }
echo "bcpack history ready: $OUT"
