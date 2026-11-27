#!/bin/bash
# usage: d3d12_metal/cache/tools/fault_stop.sh LOG BIN
# Kills BIN's process at the first GPU page fault, command-buffer error or GPU timeout written to LOG (lead's rule 3).
log="$1"; bin="$2"
while [ ! -f "$log" ]; do sleep 0.5; done
while pgrep -f "$bin" >/dev/null || [ ! -s "$log" ]; do
    if grep -qE "^\[metal\].*(command buffer [0-9]+ failed|FAULTED|kIOGPUCommandBufferCallbackError)" "$log"; then
        echo "[fault_stop] $(date +%T): $(grep -m1 -E "^\[metal\].*(command buffer [0-9]+ failed|FAULTED|kIOGPUCommandBufferCallbackError)" "$log" | cut -c1-200)" >> "$log.meta"
        pkill -9 -f "$bin"; exit 1
    fi
    sleep 0.5
done
