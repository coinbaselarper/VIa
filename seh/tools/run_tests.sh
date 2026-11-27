#!/bin/bash
# Builds and runs the seh tests. x86_64 binaries run one at a time under a timeout (Rosetta rule in port/README.md).
# usage: seh/tools/run_tests.sh [full]     (from port/; "full" adds the 3.1M-vector run over every exe function)
set -u
cd "$(dirname "$0")/../.."
ROOT=$(cd .. && pwd)
PY="${SEH_PYTHON:-$ROOT/spike/s1-seh/venv/bin/python} -I"
PKG="$ROOT/xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK"
EXE="$ROOT/exe/game_scarlett_master.exe"
OUT=build/seh-vectors
T=seh/tools/run_timeout.sh
mkdir -p "$OUT"

for a in arm64 x86_64; do
    [ -f build/seh-$a/build.ninja ] || cmake -S . -B build/seh-$a -G Ninja -DCMAKE_OSX_ARCHITECTURES=$a \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA6RT_MODULES="os;isa;seh" > build/seh-configure-$a.log 2>&1
    ninja -C build/seh-$a -j3 > build/seh-build-$a.log 2>&1 || { echo "build $a failed (build/seh-build-$a.log)"; exit 1; }
done

build/seh-arm64/seh/tests/seh_unit_test > "$OUT/unit_arm64.out" 2>&1; echo "unit arm64: rc=$?  $(tail -1 "$OUT/unit_arm64.out")"
$T 120 build/seh-x86_64/seh/tests/seh_unit_test > "$OUT/unit_x86_64.out" 2>&1; echo "unit x86_64: rc=$?  $(tail -1 "$OUT/unit_x86_64.out")"

stride=10
[ "${1:-}" = full ] && stride=1
$PY seh/tools/gen_vectors.py "$EXE" "$OUT/exe_s$stride.txt" $stride > "$OUT/gen_exe_s$stride.out"
$T 1800 build/seh-x86_64/seh/tests/seh_pdata_exec_test "$EXE" "$OUT/exe_s$stride.txt" > "$OUT/exe_s$stride.out" 2>&1
echo "pdata exe stride $stride: rc=$?  $(tail -1 "$OUT/exe_s$stride.out")"
for f in "$PKG"/*.dll; do
    n=$(basename "$f" .dll)
    $PY seh/tools/gen_vectors.py "$f" "$OUT/dll_$n.txt" 1 > "$OUT/gen_dll_$n.out"
    $T 300 build/seh-x86_64/seh/tests/seh_pdata_exec_test "$f" "$OUT/dll_$n.txt" > "$OUT/dll_$n.out" 2>&1
    echo "pdata $n: rc=$?  $(tail -1 "$OUT/dll_$n.out")"
done

$T 300 build/seh-x86_64/seh/tests/seh_dispatch_test "$PKG" build/seh-x86_64/seh/tests/guest_eh.obj "$EXE" \
    > "$OUT/dispatch.out" 2>&1
echo "dispatch: rc=$?  $(tail -1 "$OUT/dispatch.out")"
