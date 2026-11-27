#!/bin/bash
# usage: d3d12_metal/tools/metal_run.sh NAME SECONDS [vulkan|metal] [extra gta6rt args...]
# The S2 run of host/tools/s2_firstrun.sh on this module's super-build binary, with the window hidden and frames dumped
# every D3M_DUMP_EVERY presents (default 300) into run/present/NAME: GTA6RT_GPU picks the D3D12 backend (metal by
# default here). S2_INPUT_SCRIPT overrides the presses. Waits up to D3M_WAIT seconds (default 600) for the machine's
# gta6rt slot. The process is killed at SECONDS + 10 by run.sh's timer whatever it does; the Metal path also ends the
# run itself on a GPU timeout or past GTA6RT_METAL_FOOTPRINT_CAP_MB.
port="$(cd "$(dirname "$0")/../.." && pwd)"
name="$1"; secs="${2:-90}"; gpu="${3:-metal}"; shift 3 2>/dev/null
arch="${D3D12_METAL_ARCH:-x86_64}"
present="$port/run/present/$name"
busy() { ps -axo comm= | awk '{ n = $1; sub(/.*\//, "", n); if (n == "gta6rt" || n ~ /^gta6rt_/) f = 1 } END { exit !f }'; }
for ((w = 0; w < ${D3M_WAIT:-600}; w += 5)); do busy || break; sleep 5; done
rm -rf "$present"; mkdir -p "$present"
# A copy of the binary runs, so the tree can be rebuilt while it does (the name stays gta6rt for run.sh's check).
built="${GTA6RT_BIN:-$port/build/d3d12_metal-$arch/gta6rt}"
bin="$port/build/d3d12_metal-$arch/run/gta6rt"
mkdir -p "$(dirname "$bin")" && cp -p "$built" "$bin.tmp" && mv -f "$bin.tmp" "$bin" || exit 2
GTA6RT_BIN="$bin" GTA6RT_GPU="$gpu" LEONIDA_PRESENT_FIRST_LIT="$present" \
    "$port/host/tools/run.sh" "$((secs + 10))" "$name" --trace=0 --hidden-window \
    --trace-match "${D3M_TRACE_MATCH:-BinkOpenWithOptions,BinkRegisterFrameBuffers,BinkClose}" \
    --watchdog "$secs" --memory-report 15 --present-dump "$present,${D3M_DUMP_EVERY:-300}" \
    --input-script "${S2_INPUT_SCRIPT:-a@40,a@55,a@70,a@85,a@100,a@115}" "$@"
