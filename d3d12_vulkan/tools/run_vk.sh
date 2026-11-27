#!/bin/bash
# usage: d3d12_vulkan/tools/run_vk.sh SECONDS LOGNAME [gta6rt args...]
# The title on the Vulkan path through host/tools/run.sh. Refuses while any gta6rt binary runs (run.sh only guards
# build/macos/gta6rt). Defaults, each kept when already set:
#   GTA6RT_BIN                   build/d3d12_vulkan-host-x86_64/gta6rt
#   GTA6RT_VK_DRIVER             MoltenVK 1.4.2 x86_64
#   LEONIDA_SHADERS              build/shaders_vk/apple/spirv (port/shaders_vk)
#   LEONIDA_PIPELINE_CACHE_DIR   run/pipeline_cache/<driver dylib name> (a session file is per driver)
#   LEONIDA_PRESENT_FIRST_LIT    run/present/LOGNAME (the first non-black presented frame as PNG)
#   LEONIDA_XMEM_32BIT           anywhere (x86_64 has no free range below 4 GiB for the title's 32-bit heap)
port="$(cd "$(dirname "$0")/../.." && pwd)"
root="$(cd "$port/.." && pwd)"
if ps -axo comm= | grep -q 'gta6rt'; then
    echo "run_vk.sh: a gta6rt process is running:" >&2
    ps -axo pid=,comm= | grep 'gta6rt' >&2
    exit 125
fi
export GTA6RT_BIN="${GTA6RT_BIN:-$port/build/d3d12_vulkan-host-x86_64/gta6rt}"
export GTA6RT_VK_DRIVER="${GTA6RT_VK_DRIVER:-$root/spike/s1-vulkan-x86/drivers/moltenvk-1.4.2/libMoltenVK.x86_64.dylib}"
export LEONIDA_SHADERS="${LEONIDA_SHADERS:-$port/build/shaders_vk/apple/spirv}"
driver_name="$(basename "$GTA6RT_VK_DRIVER" .dylib)"
export LEONIDA_PIPELINE_CACHE_DIR="${LEONIDA_PIPELINE_CACHE_DIR:-$port/run/pipeline_cache/$driver_name}"
export LEONIDA_PRESENT_FIRST_LIT="${LEONIDA_PRESENT_FIRST_LIT:-$port/run/present/$2}"
export LEONIDA_XMEM_32BIT="${LEONIDA_XMEM_32BIT:-anywhere}"
echo "run_vk.sh: bin=$GTA6RT_BIN driver=$GTA6RT_VK_DRIVER shaders=$LEONIDA_SHADERS cache=$LEONIDA_PIPELINE_CACHE_DIR" \
     "first_lit=$LEONIDA_PRESENT_FIRST_LIT xmem32=$LEONIDA_XMEM_32BIT"
exec "$port/host/tools/run.sh" "$@"
