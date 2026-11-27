#!/bin/bash
# usage: audio_out/tools/title_run.sh NAME [SECONDS] [GTA6RT_AUDIO mode]     (defaults: 100, on)
# The S2 run (host/tools/s2_firstrun.sh options: Bink trace, scripted A presses after the first lit frame, every 30th
# present dumped) with the worker host built in build/audio_out-host from a patched copy of port/. Output to
# build/audio_out-host/run/logs/NAME.log, the mixed output to build/audio_out-host/run/NAME.wav (tap before the
# master gain). Playback is quiet: GTA6RT_AUDIO_GAIN defaults to 0.05 (-26 dB).
port="$(cd "$(dirname "$0")/../.." && pwd)"
tree="$port/build/audio_out-host"
name="$1"; secs="${2:-100}"; mode="${3:-on}"
[ -n "$name" ] || { echo "usage: $0 NAME [SECONDS] [MODE]" >&2; exit 2; }
run="$tree/run"
mkdir -p "$run/logs" "$run/pipeline_cache"
if [ ! -d "$run/pipeline_cache/libMoltenVK.x86_64" ] && [ -d "$port/run/pipeline_cache/libMoltenVK.x86_64" ]; then
    cp -R "$port/run/pipeline_cache/libMoltenVK.x86_64" "$run/pipeline_cache/"
fi
present="$run/present/$name"
rm -rf "$present"; mkdir -p "$present"
root="$(cd "$port/.." && pwd)"
export GTA6RT_BIN="$tree/build/gta6rt"
export GTA6RT_AUDIO="$mode"
export GTA6RT_AUDIO_GAIN="${GTA6RT_AUDIO_GAIN:-0.05}"
export GTA6RT_AUDIO_WAV="$run/$name.wav"
export GTA6RT_AUDIO_STATS="${GTA6RT_AUDIO_STATS:-5}"
export LEONIDA_SHADERS="$port/build/shaders_vk/apple/spirv"
export LEONIDA_PRESENT_FIRST_LIT="$present"
cp "$tree/src/host/tools/run.sh" "$tree/run.sh"
sed -i '' "s|^here=.*|here=\"$tree\"|" "$tree/run.sh"
"$tree/run.sh" "$((secs + 10))" "$name" --exe "$root/exe/game_scarlett_master.exe" \
    --package "$root/xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK" --run-dir "$run" \
    --vk-driver "$root/spike/s1-vulkan-x86/drivers/moltenvk-1.4.2/libMoltenVK.x86_64.dylib" \
    --trace=0 --trace-match BinkOpenWithOptions,BinkRegisterFrameBuffers,BinkPause,BinkNextFrame,BinkClose,BinkSetSoundSystem \
    --watchdog "$secs" --memory-report 15 --present-dump "$present,30" \
    --input-script "${S2_INPUT_SCRIPT:-a@40,a@55,a@70,a@85,a@100,a@115}"
