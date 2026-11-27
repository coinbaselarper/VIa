#!/bin/bash
# usage: d3d12_metal/tools/rehash_check.sh [WORK]
# Checks of a rehash_world.sh result, no GPU: (1) shaderc_msl's spvDescriptorFmix32 helper, compiled as C++, equals
# portable/src/descriptor_table.h's hash_descriptor_key(fnv1a_fmix32) on 4 M random keys, the wrapped FNV expression
# included; (2) WORK/report.json has no failed and no missing library; (3) prepare_shaders refused nothing.
set -u
port="$(cd "$(dirname "$0")/../.." && pwd)"
work="${1:-}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
python3 -I - "$port" "$tmp/helper.inc" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1] + "/shaderc_msl/tools")
import shaderc_msl as R
expr = "(" * 15 + "2166136261u ^ k[0]) * 16777619u)"
for i in range(1, 8):
    expr += " ^ k[%d]) * 16777619u" % i + (")" if i < 7 else "")
text = "using namespace metal;\nuint lookup_hash(const uint* k) { return " + expr + "; }\n"
new, sites = R.hash_fmix32(text)
assert sites == 1, sites
open(sys.argv[2], "w").write(new.replace("using namespace metal;\n", "").replace("__attribute__((always_inline))", ""))
EOF
cat >"$tmp/t.cpp" <<EOF
#include "descriptor_table.h"
#include <cstdio>
#include <random>
using uint = std::uint32_t;
#include "$tmp/helper.inc"
int main() {
    std::mt19937 rng(15);
    std::uint64_t bad = 0;
    for (int n = 0; n < 4000000; ++n) {
        std::uint32_t k[8];
        for (auto& w : k) w = rng();
        if (n & 1) k[4] = k[5] = k[6] = k[7] = 0xffffffffu;
        if (lookup_hash(k) != leonida::hash_descriptor_key(k, leonida::DescriptorHashKind::fnv1a_fmix32)) ++bad;
    }
    std::printf("fmix32 helper vs descriptor_table.h: %llu of 4000000 keys differ\n", (unsigned long long)bad);
    return bad != 0;
}
EOF
clang++ -std=c++20 -O1 -I"$port/portable/src" "$tmp/t.cpp" -o "$tmp/t" || exit 1
"$tmp/t" || exit 1
[ -z "$work" ] && exit 0
python3 -I - "$work" <<'EOF' || exit 1
import json, sys
r = json.load(open(sys.argv[1] + "/report.json"))
print("report: %d rows changed of %d, %d libraries done, %d not done, %d failed, %d FNV sites in %d libraries, %d tier names unresolved"
      % (r["rows_changed"], r["rows"], r["libraries_done"], r["libraries_not_done"], len(r["failed"]), r["sites"],
         r["libraries_with_sites"], r["unresolved"]))
sys.exit(1 if r["failed"] or r["libraries_not_done"] else 0)
EOF
if [ -f "$work/prepare.log" ]; then
    grep -E "^built [0-9]+ pipelines" "$work/prepare.log" || { echo "prepare.log has no summary line"; exit 1; }
    grep -qE " 0 failed" "$work/prepare.log" || { echo "prepare_shaders refused pipelines (see $work/prepare.log)"; exit 1; }
fi
echo "rehash check: ok"
