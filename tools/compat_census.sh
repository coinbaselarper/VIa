#!/bin/sh
# Syntax-only compile of every leonida/runtime/{src,shims}/*.cpp against port/compat for both arches.
# Usage: tools/compat_census.sh <out_dir>   (writes <out_dir>/result.tsv and <out_dir>/logs/; leonida is read only)
# Columns: tu, arch, error count (fatal errors stop the TU, so counts are lower bounds), first error.
set -eu
out=${1:?out_dir}
P=$(cd "$(dirname "$0")/.." && pwd)
R=$(cd "$P/.." && pwd)
mkdir -p "$out/logs"
one() {
  tu=$1; arch=$2
  log="$out/logs/$(echo "$tu" | tr / _)-$arch.log"
  xcrun clang++ -arch "$arch" -std=c++20 -fsyntax-only -ferror-limit=0 -w \
    -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DLEONIDA_VULKAN_DEVICE \
    -isystem "$P/compat/include" -isystem "$P/third_party/DirectX-Headers/include" \
    -isystem "$P/third_party/DirectX-Headers/include/directx" -isystem "$P/third_party/DirectX-Headers/include/dxguids" \
    -isystem "$P/third_party/Vulkan-Headers/include" -isystem /opt/homebrew/include -I"$P/include" \
    -I"$R/leonida/runtime/src" -I"$R/leonida/runtime/shims" -I"$R/spike/s1-host-build-scout/generated" \
    -I"$R/leonida/third_party/xtaskqueue/Include" -I"$R/leonida/third_party/xtaskqueue/Source/Common" \
    "$R/$tu" > "$log" 2>&1 || true
  n=$(grep -c ' error: ' "$log" || true)
  first=$(grep -m1 ' error: ' "$log" | sed "s|$R/||" || true)
  printf '%s\t%s\t%s\t%s\n' "$tu" "$arch" "$n" "$first"
}
: > "$out/result.tsv"
for arch in arm64 x86_64; do
  for f in "$R"/leonida/runtime/src/*.cpp "$R"/leonida/runtime/shims/*.cpp; do
    one "${f#$R/}" "$arch" >> "$out/result.tsv"
  done
done
awk -F'\t' '{t[$2]++; c[$2]+=($3==0)} END{for(a in c) print a ": " c[a] " of " t[a] " TUs syntax-clean"}' "$out/result.tsv"
