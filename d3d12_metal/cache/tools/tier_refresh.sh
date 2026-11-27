#!/bin/bash
# usage: d3d12_metal/cache/tools/tier_refresh.sh [SECONDS=300] [TIER=build/pc7/arch-world]
# Merges every Metal run's recorder files under port/build (*/pcache-runs next to a pcache-manifest.txt, the tier's
# own and test trees excluded) into build/pc7/lists/world-r8-<SECONDS>.{tsv,specs} and TIER/pcache-tier.txt, and
# writes build/pc7/lists/world-new.specs: the pipelines no earlier tier list had (prepare_shaders --specs input).
port="$(cd "$(dirname "$0")/../../.." && pwd)"
secs="${1:-300}"; tier="${2:-$port/build/pc7/arch-world}"
lists="$port/build/pc7/lists"
args=(); mans=()
while IFS= read -r d; do
    dir="$(dirname "$d")"
    case "$dir" in *test-tmp*|*/build/pcache/runs/*) continue ;; esac
    [ -f "$dir/pcache-manifest.txt" ] || continue
    args+=(--runs "$d"); mans+=(--manifest "$dir/pcache-manifest.txt")
done < <(find "$port/build" -maxdepth 5 -type d -name pcache-runs 2>/dev/null | sort)
old="$lists/world-built.keys"
[ -f "$old" ] || cut -f3 "$lists"/world-*.tsv 2>/dev/null | grep -v '^key$' | sort -u > "$old"
python3 -I "$port/d3d12_metal/cache/tools/world_list.py" "${args[@]}" "${mans[@]}" --seconds "$secs" \
    --out "$lists/world-r8-$secs" | grep -v '^run '
python3 -I "$port/d3d12_metal/cache/tools/tier_manifest.py" "${mans[@]}" --world "$lists/world-r8-$secs.tsv" \
    --out "$tier/pcache-tier.txt"
python3 -I - "$lists/world-r8-$secs.tsv" "$lists/world-r8-$secs.specs" "$old" "$lists/world-new.specs" <<'E'
import sys
tsv, specs, old, out = sys.argv[1:]
keys = [l.split("\t")[2] for l in open(tsv).read().splitlines()[1:]]
lines = [l for l in open(specs).read().splitlines() if not l.startswith("#")]
seen = set(open(old).read().split())
new = [l for k, l in zip(keys, lines) if k not in seen]
with open(out, "w") as f:
    f.write("# world pipelines not in an earlier tier list (tier_refresh.sh)\n")
    f.writelines(l + "\n" for l in new)
with open(old, "a") as f:
    f.writelines(k + "\n" for k in keys if k not in seen)
print("%d world pipelines, %d new since the last tier list -> %s" % (len(keys), len(new), out))
E
