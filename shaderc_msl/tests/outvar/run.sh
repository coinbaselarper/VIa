#!/bin/bash
# Output-type variant render test: generates the translator-shaped pixel shader (plain and --branch), builds it with
# the recipe and --output-variants u,ui,fu, and draws it into integer targets (outvar_render_test.mm).
#   run.sh <outvar_render_test binary> <work dir>
set -euo pipefail
bin=$1
work=$2
here=$(cd "$(dirname "$0")" && pwd)
tools=$here/../../tools
mkdir -p "$work/cas"
rc=0
for v in plain branch; do
    flag=""
    [ "$v" = branch ] && flag=--branch
    python3 -I "$here/gen_outvar_spv.py" $flag > "$work/outvar-$v.spvasm"
    spirv-as --target-env spv1.5 "$work/outvar-$v.spvasm" -o "$work/outvar-$v.spv"
    spirv-val --target-env vulkan1.2 "$work/outvar-$v.spv"
    python3 -I "$tools/shaderc_msl.py" "$work/outvar-$v.spv" --stage ps --cas "$work/cas" --output-variants u,ui,fu \
        > "$work/outvar-$v.json"
    lib=$(python3 -I -c "import json,sys; r=json.load(open(sys.argv[1])); assert r['status']=='ok', r; \
print(sys.argv[2]+'/cas/'+r['key'][:2]+'/'+r['key']+'.metallib')" "$work/outvar-$v.json" "$work")
    echo "== $v: $lib"
    MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 "$bin" "$lib" || rc=1
done
exit $rc
