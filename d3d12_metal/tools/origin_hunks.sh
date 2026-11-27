#!/bin/bash
# usage: d3d12_metal/tools/origin_hunks.sh   (from anywhere)
# The changed line ranges of each copied file against the upstream snapshot it was copied from (origin/upstream/),
# written to origin/hunks.txt: "upstream-file a,b -> copy c,d" per hunk (diff -U0 numbering).
m="$(cd "$(dirname "$0")/.." && pwd)"
out="$m/origin/hunks.txt"
: >"$out"
pair() {
    local up="$m/origin/upstream/$1" copy="$m/$2"
    local n
    n=$(diff -U0 "$up" "$copy" | grep -c '^@@')
    printf '== %s -> %s: %s hunks, %s upstream lines removed, %s lines added\n' "$1" "$2" "$n" \
        "$(diff "$up" "$copy" | grep -c '^<')" "$(diff "$up" "$copy" | grep -c '^>')" >>"$out"
    diff -U0 "$up" "$copy" | grep '^@@' | sed -E 's/^@@ -([0-9,]+) \+([0-9,]+) @@.*/  upstream \1 -> copy \2/' >>"$out"
}
pair d3d12_com.h shims/d3d12_com.h
pair d3d12_com.cpp shims/d3d12_com.cpp
pair dxgi_com.cpp shims/dxgi_com.cpp
pair dxgi_com.h shims/dxgi_com.h
pair d3d12_vulkan_device.cpp shims/d3d12_metal_device.cpp
pair vulkan_present.h src/metal_present_planes.h
head -n 1 "$out"; grep '^==' "$out"
