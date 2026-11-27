#!/bin/bash
# Checks that src/ is FEX at the pinned commit plus patches/*.diff, minus the pruned directories in README.md.
# usage: verify_vendor.sh [FEX git clone, default spike/x6-fexcore/src/fex]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
clone="${1:-$here/../../../spike/x6-fexcore/src/fex}"
pin=3ee25399232a6431c77c6314c437ea523ec29d0c
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
git -C "$clone" archive "$pin" | tar -x -C "$tmp"
for sub in fmt xxhash range-v3 unordered_dense; do
    rev=$(git -C "$clone" ls-tree "$pin" "External/$sub" | awk '{print $3}')
    git -C "$clone/External/$sub" archive "$rev" | tar -x -C "$tmp/External/$sub"
done
for p in "$here"/patches/*.diff; do git -C "$tmp" apply "$p"; done
diff -rq -x diff.txt "$tmp" "$here/src" \
    | grep -v -E "^Only in $tmp(/External)?(/Data)?: (Source|ThunkLibs|docs|unittests|vixl|zydis|rpmalloc|Catch2|Vulkan-Headers|drm-headers|fex-[a-z-]*|jemalloc_glibc|tracy|code-format-helper|AppConfig|binfmts|nix|CI|Dockerfile|AGENTS.md|CLAUDE.md|CODE_OF_CONDUCT.md|CONTRIBUTING.md|default.nix|flake.lock|flake.nix|\.git.*)$" \
    | grep -v -E "^Only in $tmp/External/(fmt|range-v3|unordered_dense|xxhash): (test|doc|example|perf|data|tests|cli)$" \
    | grep -v -E ": (\.git[a-z]*|\.github|diff\.txt)$" \
    > "$tmp/diff.txt" || true
if [ -s "$tmp/diff.txt" ]; then
    cat "$tmp/diff.txt"
    echo "verify_vendor: FAIL"
    exit 1
fi
echo "verify_vendor: PASS (src = $pin + $(ls "$here"/patches/*.diff | wc -l | tr -d ' ') patches, pruned dirs excluded)"
