#!/bin/bash
# usage: d3d12_metal/tools/metal_run.sh NAME SECONDS [vulkan|metal] [extra gta6rt args...]
# The S2 run of host/tools/s2_firstrun.sh on this module's super-build binary, with the window hidden and frames dumped
# every D3M_DUMP_EVERY presents (default 300) into run/present/NAME: GTA6RT_GPU picks the D3D12 backend (metal by
# default here). S2_INPUT_SCRIPT overrides the presses. The slot, the per-run binary copy and every guard are
# slot_lib.sh's; the process is also killed at SECONDS + 10 by run.sh's timer whatever it does.
set -u
. "$(dirname "$0")/slot_lib.sh"
port="$SLOT_PORT"
name="$1"; secs="${2:-90}"; gpu="${3:-metal}"; shift 3 2>/dev/null
arch="${D3D12_METAL_ARCH:-x86_64}"
present="$port/run/present/$name"
meta="$port/run/logs/$name.meta"
built="${GTA6RT_BIN:-$port/build/d3d12_metal-$arch/gta6rt}"
mkdir -p "$port/run/logs"
: >"$meta"
slot_take "$name"
slot_copy "$built" || exit 2
rm -rf "$present"; mkdir -p "$present"
echo "$SLOT_START_LINE bin $built copy $SLOT_BIN" >>"$meta"
env $(slot_env) GTA6RT_BIN="$SLOT_BIN" GTA6RT_GPU="$gpu" LEONIDA_PRESENT_FIRST_LIT="$present" \
    "$port/host/tools/run.sh" "$((secs + 10))" "$name" --trace=0 --hidden-window \
    --trace-match "${D3M_TRACE_MATCH:-BinkOpenWithOptions,BinkRegisterFrameBuffers,BinkClose}" \
    --watchdog "$secs" --memory-report 15 --present-dump "$present,${D3M_DUMP_EVERY:-300}" \
    --input-script "${S2_INPUT_SCRIPT:-a@40,a@55,a@70,a@85,a@100,a@115}" "$@" &
rpid=$!
sleep 3
slot_collided 2>>"$meta"
slot_guard $rpid "$port/run/logs/$name.log" "$meta"
wait $rpid; rc=$?
slot_finish "$meta" || rc=3
exit $rc
