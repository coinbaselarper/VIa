#!/bin/bash
# usage: tools/setup/export_lists.sh [--port DIR] OUT
# Writes the recipe lists that tools/setup.sh --lists reads, taken from a workspace whose build/ already holds the
# overlays and the world tier (default: this checkout). The lists are shader names and pipeline descriptions recorded
# from runs of the game: game-derived, so they are written outside the repository and never committed by this script.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
port="$(cd "$here/../.." && pwd)"
if [ "${1:-}" = "--port" ]; then port="$(cd "$2" && pwd)"; shift 2; fi
out="${1:?usage: export_lists.sh [--port DIR] OUT}"
b="$port/build"
mkdir -p "$out"

rows() { grep -v '^#' "$1" | cut -f1; }
need() { [ -f "$1" ] || { echo "export_lists: missing $1" >&2; exit 1; }; }

need "$b/mspeed-x86_64/overlay.tsv"
rows "$b/mspeed-x86_64/overlay.tsv" >"$out/mspeed.names"
need "$b/expo-x86_64/rectvs/manifest.tsv"
rows "$b/expo-x86_64/rectvs/manifest.tsv" >"$out/rectvs.names"
need "$b/gpass-x86_64/mop/overlay.tsv"
rows "$b/gpass-x86_64/mop/overlay.tsv" >"$out/mop.names"
need "$b/tmem-x86_64/overlay.tsv"
grep -v '^#' "$b/tmem-x86_64/overlay.tsv" | awk -F'\t' '$9 ~ /merge_typed_reads/ {print $1}' >"$out/tmem-merge.names"
grep -v '^#' "$b/tmem-x86_64/overlay.tsv" | awk -F'\t' '$9 ~ /loop_cap/ {print $1}' >"$out/tmem-loopcap.names"
need "$b/mcc-x86_64/overlay.tsv"
rows "$b/mcc-x86_64/overlay.tsv" >"$out/mcc.names"
need "$b/d3d12_metal-msl-overrides/manifest.tsv"
grep -v '^#' "$b/d3d12_metal-msl-overrides/manifest.tsv" | awk -F'\t' '$11 == "split64" {print $1}' >"$out/overrides-split.names"
grep -v '^#' "$b/d3d12_metal-msl-overrides/manifest.tsv" | awk -F'\t' '$11 != "split64" {print $1}' >"$out/overrides-pack.names"
need "$b/uintps-x86_64/overlay.tsv"
grep -v '^#' "$b/uintps-x86_64/overlay.tsv" | awk -F'\t' '$3 == "ps" {print $1}' >"$out/uintps.names"
grep -v '^#' "$b/uintps-x86_64/overlay.tsv" | awk -F'\t' '$3 != "ps" && $1 !~ /^fspx\// {print $1}' >"$out/uintps-also.names"
grep -v '^#' "$b/uintps-x86_64/overlay.tsv" | awk -F'\t' '$1 ~ /^fspx\// {print $1}' >"$out/uintps-fspx.names"
need "$b/pc7/arch-world/pcache-tier.txt"
cp "$b/pc7/arch-world/pcache-tier.txt" "$out/world-tier.txt"

cat >"$out/README.txt" <<'EOF'
Recipe lists for tools/setup.sh --lists (game-derived: shader names and pipeline descriptions; do not publish
without deciding to).
  mspeed.names           mesh + pixel shaders rebuilt with the mesh-speed passes (shaderc_msl/tools/mspeed_overlay.py)
  rectvs.names           rect-list vertex shaders given their MSL (d3d12_metal/tools/rect_vs.py)
  mop.names              hot G-buffer mesh rows rebuilt with private outputs (mesh_out_private.py --force)
  tmem-merge.names       compute kernels rebuilt with merge_typed_reads (tmem_overlay.py --merge)
  tmem-loopcap.names     compute kernels rebuilt with loop_cap 16384 (tmem_overlay.py --loop-cap)
  mcc.names              hw-route mesh rows with private outputs (mesh_out_private.py --list; --scan finds the same set)
  overrides-pack.names   mesh rows packed under the threadgroup limit (mesh_tg_pack.py)
  overrides-split.names  mesh rows rebuilt through the meshlet split (mesh_tg_pack.py --split)
  uintps.names           pixel shaders with integer-output variants (output_variants.py build --names)
  uintps-also.names      compute kernels rebuilt with batch options in the same overlay (--also)
  uintps-fspx.names      FSR4 programs the store lacks (rows only when the corpus has them)
  world-tier.txt         the world pipeline list (prepare_shaders --manifest) for the pipeline tier
EOF
wc -l "$out"/*.names "$out/world-tier.txt" | sed 's/^/  /'
echo "lists written to $out"
