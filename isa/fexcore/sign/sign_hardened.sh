#!/bin/bash
# usage: sign_hardened.sh BINARY [ENTITLEMENTS]: ad-hoc signature with the hardened runtime and the JIT entitlement
# FEX needs (MAP_JIT code buffers). Development builds do not need this: the linker's ad-hoc signature has no hardened
# runtime, and MAP_JIT works there without an entitlement.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
ent="${2:-$here/jit.entitlements}"
codesign -s - -f -o runtime --entitlements "$ent" "$1"
codesign -d --entitlements - "$1" 2>/dev/null | grep -q allow-jit
echo "signed $1 (hardened runtime, $(basename "$ent"))"
