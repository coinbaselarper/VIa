#!/bin/bash
# usage: d3d12_metal/tools/rehash_world.sh [WORK]   (default WORK = port/build/rehash-fmix32)
# One command, unattended, resumable (run it again after a stop: finished libraries and archived pipelines are kept):
# every library a world run loads, rebuilt with fnv1a_fmix32 descriptor lookups (rehash_world.py), then the pipelines
# that use them prebuilt into WORK/arch (prepare_shaders), then checked. nice 15, 3 compile jobs; a new library job
# does not start while a gta6rt runs (it waits; running ones finish), and prepare_shaders runs at most
# REHASH_TITLE_JOBS (1) compiles while a gta6rt runs. REHASH_JOBS / REHASH_NICE override. The last lines print the environment a run needs.
set -u
port="$(cd "$(dirname "$0")/../.." && pwd)"
work="${1:-$port/build/rehash-fmix32}"
jobs="${REHASH_JOBS:-3}"
nicelvl="${REHASH_NICE:-15}"
mkdir -p "$work"
log="$work/rehash.log"
lock="$work/.lock"
mkdir "$lock" 2>/dev/null || { echo "rehash_world.sh: $lock exists (another run, or remove it)" >&2; exit 1; }
trap 'rmdir "$lock" 2>/dev/null' EXIT
say() { echo "[$(date +%T)] $*" | tee -a "$log"; }
py() { nice -n "$nicelvl" python3 -I "$port/d3d12_metal/tools/rehash_world.py" "$@" 2>&1 | tee -a "$log"; return "${PIPESTATUS[0]}"; }

say "rehash_world.sh in $work (jobs $jobs, nice $nicelvl)"
[ -f "$work/plan.json" ] || py plan "$work" || exit 1
py count "$work" || exit 1
REHASH_WAIT_FOR_GAME=1 py build "$work" --jobs "$jobs" || say "some libraries failed (see $work/build.jsonl); continuing to the report"
py overlay "$work" || { say "overlay incomplete: $work/report.json"; exit 1; }
py manifest "$work" || exit 1
ps_bin="$port/build/pc7-cache-x86_64/prepare_shaders"
[ -x "$ps_bin" ] || { say "no $ps_bin (build the pc7-cache tree's prepare_shaders target)"; exit 1; }
say "prepare_shaders: $(grep -vc '^#' "$work/prebuild.manifest.txt") pipelines into $work/arch"
nice -n "$nicelvl" "$ps_bin" --manifest "$work/prebuild.manifest.txt" --catalog "$work/catalog.tsv" --archive-dir "$work/arch" \
    --jobs 3 --title-jobs "${REHASH_TITLE_JOBS:-1}" >"$work/prepare.log" 2>&1
rc=$?
tail -4 "$work/prepare.log" | tee -a "$log"
cp -p "$work/prebuild.manifest.txt" "$work/arch/pcache-tier.txt"
[ $rc -eq 0 ] || { say "prepare_shaders exit $rc"; exit 1; }
nice -n "$nicelvl" "$port/d3d12_metal/tools/rehash_check.sh" "$work" 2>&1 | tee -a "$log" || exit 1
say "done. A run with these libraries:"
echo "  GTA6RT_METAL_TABLE_HASH=fnv1a_fmix32 SHADERC_HASH_KIND=fnv1a_fmix32 GTA6RT_METAL_TABLE_CLUSTER_CAP_MB=0 \\" | tee -a "$log"
echo "  GTA6RT_METAL_SHADER_OVERLAY=$work/overlay.tsv:<the default list, shims/d3d12_metal_device.cpp> \\" | tee -a "$log"
echo "  GTA6RT_METAL_ARCHIVE_TIERS=$port/build/pc7/arch-world:$work/arch" | tee -a "$log"
