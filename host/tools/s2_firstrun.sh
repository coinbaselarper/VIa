#!/bin/bash
# usage: host/tools/s2_firstrun.sh [NAME] [SECONDS]      (defaults: s2-firstrun, 150)
# The S2 run: the title with a visible window on the default driver (MoltenVK x86_64) and the translated shaders,
# scripted A presses, every 30th presented frame dumped as PNG into run/present/NAME. Afterwards the evidence is
# appended to run/logs/NAME.log by s2_evidence.py: video frames (the launch movie), the text on each still page
# (macOS Vision), and "[s2] first-run page reached: ..." lines. Exit: run.sh's code if the run did not reach its
# watchdog (124 = killed by run.sh's timer too), else 0 when a first-run page was read, 1 when not.
port="$(cd "$(dirname "$0")/../.." && pwd)"
name="${1:-s2-firstrun}"
secs="${2:-150}"
present="$port/run/present/$name"
tool="$port/build/macos/tools/screen_text"
if [ ! -x "$tool" ] || [ "$port/host/tools/screen_text.swift" -nt "$tool" ]; then
    mkdir -p "$(dirname "$tool")"
    swiftc -O "$port/host/tools/screen_text.swift" -o "$tool" || exit 2
fi
rm -rf "$present"
mkdir -p "$present"
start=$(date +%s)
LEONIDA_PRESENT_FIRST_LIT="$present" "$port/host/tools/run.sh" "$((secs + 10))" "$name" --trace=0 \
    --trace-match BinkOpenWithOptions,BinkRegisterFrameBuffers,BinkPause,BinkNextFrame,BinkClose \
    --watchdog "$secs" --memory-report 15 --present-dump "$present,30" \
    --input-script "${S2_INPUT_SCRIPT:-a@40,a@55,a@70,a@85,a@100,a@115}"
rc=$?
log="$port/run/logs/$name.log"
[ -f "$log" ] || exit 2
nf=$(grep -c '^\[trace\].*bink2scarlett.dll!BinkNextFrame' "$log")
echo "[s2] Bink: $nf BinkNextFrame calls traced (the title decodes one movie frame per call)" | tee -a "$log"
python3 -I "$port/host/tools/s2_evidence.py" "$log" "$present" "$start" "$tool"
[ "$rc" -eq 124 ] || [ "$rc" -eq 0 ] || exit "$rc"
grep -q '^\[s2\] first-run page reached: ' "$log"
