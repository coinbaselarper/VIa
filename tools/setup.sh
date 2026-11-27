#!/bin/bash
# usage: tools/setup.sh --package DIR --exe FILE [options]
# Builds everything the native Metal route needs from your own copy of the game, in dependency order, and writes the
# config tools/play.sh reads (build/setup/config.sh). Resumable: a finished stage is recorded in build/setup/done/
# and skipped next time, and the tools behind the long stages resume where they stopped.
#   --package DIR   the game's Xbox Series package folder (read only; never copied)
#   --exe FILE      the game's executable (read only)
#   --data DIR      put the large outputs (about 32 GiB) on DIR: build/ moves to DIR/build and becomes a link
#   --lists DIR     recipe lists (tools/setup/export_lists.sh): the shaders each overlay rebuilds and the world
#                   pipeline list. Without them the overlays that need names and the pipeline tier are skipped.
#   --tier-from DIR build the pipeline tier from a run's archive directory (its pcache-manifest.txt) instead
#   --jobs N        compile jobs, 1-3 (default 3)
#   --only A,B      run only these stages, even if done; --from A: A and every later stage
#   --redo          run the selected stages even if done
#   --dry-run       print each stage, its state, its measured time and disk cost here, and its commands
#   --sample N      every stage on N shaders (plus one from each list) into this checkout's build/, to prove the
#                   commands; refuses when build/ holds outputs it did not make (use a fresh clone)
#   --stages        list the stages
set -u
here="$(cd "$(dirname "$0")" && pwd)"
port="$(cd "$here/.." && pwd)"
state="$port/build/setup"
T="$port/shaders_vk/translator/tools"
S="$port/shaderc_msl/tools"
D="$port/d3d12_metal/tools"

STAGES="preflight binaries corpus bcpack store-pairs store-fit store-batch store-routes ov-mspeed ov-rectvs ov-mop ov-tmem ov-mcc ov-overrides ov-uintps catalog tier config"

# stage|time measured or inferred on the M4 Pro 24 GB this was written on|disk|basis
COSTS='preflight|under 1 s|0|M
binaries|1-3 min (play binary 63 s at -j4 from clean, pipeline tools about 1 min)|0.5 GiB|M (clean clone, 2026-11-27)
corpus|about 3 h at 3 jobs (carve 8 s, unpack 16 s, disassembly 4.8 min and translation 81 min at 6 jobs, sub-parts 12 min)|22 GiB|M parts, I total at 3 jobs
bcpack|several minutes|60 KiB|I (generator header)
store-pairs|about 1 min|10 MiB|I
store-fit|a few minutes|20 MiB|I
store-batch|about 5 h at 3-4 jobs (70,500 job-seconds measured for the corpus)|5.4 GiB|M job-seconds, I wall time
store-routes|30-60 min (pipeline probes, meshlet split variants)|0.3 GiB|I
ov-mspeed|10 min at 2 jobs (5.1 + 4.9 min measured)|0.2 GiB|M
ov-rectvs|1-2 min|3 MiB|M disk, I time
ov-mop|under 1 min|6 MiB|M disk, I time
ov-tmem|2-5 min|1.4 MiB|M disk, I time
ov-mcc|10-20 min at 2 jobs|0.25 GiB|M disk, I time
ov-overrides|5-10 min|23 MiB|M disk, I time
ov-uintps|18 min at 2 jobs (2,159 job-seconds)|0.25 GiB|M
catalog|seconds|70 MiB|M
tier|1.5-3 h cold at 3 jobs (2,121 world pipelines took 70 min)|3.1 GiB|M part, I total
config|under 1 s|0|M'

usage() { sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-64}"; }

package=""; exe=""; data=""; lists=""; tier_from=""; jobs=3; only=""; from=""; redo=0; dry=0; sample=0
while [ $# -gt 0 ]; do
    case "$1" in
        --package) package="$2"; shift ;;
        --exe) exe="$2"; shift ;;
        --data) data="$2"; shift ;;
        --lists) lists="$2"; shift ;;
        --tier-from) tier_from="$2"; shift ;;
        --jobs) jobs="$2"; shift ;;
        --only) only="$2"; shift ;;
        --from) from="$2"; shift ;;
        --redo) redo=1 ;;
        --dry-run) dry=1 ;;
        --sample) sample="$2"; shift ;;
        --stages) echo "$STAGES" | tr ' ' '\n'; exit 0 ;;
        -h|--help) usage 0 ;;
        *) echo "setup.sh: unknown option $1" >&2; usage ;;
    esac
    shift
done
case "$jobs" in 1|2|3) ;; *) echo "setup.sh: --jobs must be 1, 2 or 3" >&2; exit 64 ;; esac
abspath() { (cd "$(dirname "$1")" 2>/dev/null && echo "$(pwd)/$(basename "$1")"); }
[ -n "$package" ] && package="$(abspath "$package")"
[ -n "$exe" ] && exe="$(abspath "$exe")"
[ -n "$lists" ] && lists="$(abspath "$lists")"
[ -n "$tier_from" ] && tier_from="$(abspath "$tier_from")"
if [ -f "$state/config.sh" ] && { [ -z "$package" ] || [ -z "$exe" ]; }; then
    [ -z "$package" ] && package="$(sed -n 's/^SETUP_PACKAGE=//p' "$state/config.sh" | tr -d "'")"
    [ -z "$exe" ] && exe="$(sed -n 's/^SETUP_EXE=//p' "$state/config.sh" | tr -d "'")"
fi
if [ -z "$data" ]; then
    [ -f "$state/data" ] && data="$(cat "$state/data")" || data="$port/build"
fi
[ "$dry" = 1 ] || mkdir -p "$data" 2>/dev/null
data="$(cd "$data" 2>/dev/null && pwd || echo "$data")"

corpus="$data/corpus"
work="$corpus/work"
spirv="$work/shaders/spirv"
store="$port/build/shaderc_msl"
bin="$port/build/metal-play-x86_64/bin/gta6rt"
prep="$port/build/setup-cache-x86_64/prepare_shaders"
OVERLAYS="mspeed-x86_64 expo-x86_64 gpass-x86_64 tmem-x86_64 mcc-x86_64 d3d12_metal-msl-overrides uintps-x86_64"
LINKED="shaderc_msl $OVERLAYS pc7 setup-tgpack"

find_py() {
    for c in python3.14 python3.13 python3.12 python3.11 python3.10 python3; do
        for d in /opt/homebrew/bin /usr/local/bin ""; do
            p="$d/$c"; [ -z "$d" ] && p="$(command -v "$c" 2>/dev/null)"
            [ -n "$p" ] && [ -x "$p" ] && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null && { echo "$p"; return; }
        done
    done
}
py="${PYTHON:-$(find_py)}"
llvm_mc="${LLVM_MC:-}"
[ -z "$llvm_mc" ] && for c in /opt/homebrew/opt/llvm/bin/llvm-mc /usr/local/opt/llvm/bin/llvm-mc "$(command -v llvm-mc 2>/dev/null)"; do
    [ -n "$c" ] && [ -x "$c" ] && { llvm_mc="$c"; break; }
done
spirv_cross="${SHADERC_SPIRV_CROSS:-$(command -v spirv-cross 2>/dev/null || echo /opt/homebrew/bin/spirv-cross)}"
spirv_bin="$(dirname "$spirv_cross")"
export LLVM_MC="$llvm_mc" SHADERC_SPIRV_CROSS="$spirv_cross"
export SHADERC_SPIRV_AS="${SHADERC_SPIRV_AS:-$spirv_bin/spirv-as}" SHADERC_SPIRV_DIS="${SHADERC_SPIRV_DIS:-$spirv_bin/spirv-dis}"
export SHADERC_SPIRV_VAL="${SHADERC_SPIRV_VAL:-$spirv_bin/spirv-val}"
export GTA6RT_METAL_SPIRV="$spirv" GTA6RT_SHADER_WORK="$work/shaders" GTA6RT_TRANSLATOR_TOOLS="$T" GTA6RT_MAKE_GAME_PYTHON="$py"
nicelvl="${SETUP_NICE:-10}"

say() { echo "$@"; }
die() { echo "setup.sh: $*" >&2; exit 1; }
q() { printf '%q ' "$@"; }
run() {
    if [ "$dry" = 1 ]; then echo "    $(q "$@")"; return 0; fi
    echo "  + $(q "$@")" >>"$state/setup.log"
    nice -n "$nicelvl" "$@" || die "failed ($?): $(q "$@")"
}
note() { [ "$dry" = 1 ] && echo "    # $*" || echo "  $*"; }
list() { [ -n "$lists" ] && [ -s "$lists/$1" ] && echo "$lists/$1"; }
cost() { echo "$COSTS" | awk -F'|' -v s="$1" '$1 == s {print "time " $2 "; disk " $3 " (" $4 ")"}'; }
free_gib() { df -k "$1" | awk 'NR == 2 {print int($4 / 1048576)}'; }

link_out() { :; }

place_build() {
    [ "$data" = "$port/build" ] && return 0
    local real="$data/build"
    if [ -L "$port/build" ]; then
        [ "$(readlink "$port/build")" = "$real" ] || die "$port/build links to $(readlink "$port/build"), not $real"
        return 0
    fi
    mkdir -p "$data"
    if [ -d "$port/build" ]; then
        [ -e "$real" ] && die "both $port/build and $real exist; move one away"
        say "  moving $port/build to $real (build/ becomes a link; relative paths inside it stay valid)"
        mv "$port/build" "$real" || die "could not move $port/build to $real"
    else
        mkdir -p "$real"
    fi
    ln -s "$real" "$port/build" || die "could not link $port/build to $real"
}

sample_names() { echo "$state/sample.names"; }
sample_guard() {
    [ "$sample" -gt 0 ] || return 0
    [ "$dry" = 1 ] && return 0
    [ -f "$state/sample.mode" ] && return 0
    for n in $LINKED corpus; do
        [ -e "$port/build/$n" ] && die "--sample writes into $port/build and $port/build/$n already exists; use a fresh clone"
    done
    [ "$data" != "$port/build" ] && [ -n "$(ls -A "$data" 2>/dev/null)" ] && die "--sample needs an empty --data directory"
    return 0
}

need_game() {
    [ -n "$package" ] && [ -d "$package" ] || die "--package DIR (the game's package folder) is needed for this stage"
    [ -n "$exe" ] && [ -f "$exe" ] || die "--exe FILE (the game's executable) is needed for this stage"
}

# ---- stages ----

st_preflight() {
    local bad=0
    chk() { if eval "$2" >/dev/null 2>&1; then say "  ok    $1"; else say "  MISSING $1${3:+: $3}"; bad=1; fi; }
    chk "macOS on Apple silicon" '[ "$(uname -s)" = Darwin ] && [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ]'
    chk "Rosetta 2" '/usr/bin/arch -x86_64 /usr/bin/true' "softwareupdate --install-rosetta"
    chk "Xcode with the Metal toolchain (xcrun metal, metallib)" 'xcrun -f metal && xcrun -f metallib' "install Xcode, then: xcodebuild -downloadComponent MetalToolchain"
    chk "cmake 3.24+ and ninja" 'command -v cmake && command -v ninja' "brew install cmake ninja"
    chk "Rust with the x86_64-apple-darwin target" '{ command -v cargo || [ -x "$HOME/.cargo/bin/cargo" ]; } && { rustup target list --installed 2>/dev/null || "$HOME/.cargo/bin/rustup" target list --installed; } | grep -q x86_64-apple-darwin' "rustup target add x86_64-apple-darwin"
    chk "Python 3.10+ ($py)" '[ -n "$py" ]' "brew install python"
    chk "llvm-mc ($llvm_mc)" '[ -x "$llvm_mc" ]' "brew install llvm"
    chk "spirv-cross ($spirv_cross)" '[ -x "$spirv_cross" ]' "brew install spirv-cross"
    chk "spirv-as, spirv-dis, spirv-val" '[ -x "$SHADERC_SPIRV_AS" ] && [ -x "$SHADERC_SPIRV_DIS" ] && [ -x "$SHADERC_SPIRV_VAL" ]' "brew install spirv-tools"
    chk "the game package ($package)" '[ -n "$package" ] && [ -f "$package/shaders.rpf" ]'
    chk "the game executable ($exe)" '[ -n "$exe" ] && [ -f "$exe" ]'
    chk "shaderc_msl/tools/soft_f64.metal (needed by the store build; gitignored in older checkouts)" '[ -f "$S/soft_f64.metal" ]'
    local fg; fg=$(free_gib "$data")
    say "  free on $(df -k "$data" | awk 'NR == 2 {print $NF}'): $fg GiB (setup writes about 32 GiB; keep some free besides)"
    [ "$fg" -lt 40 ] && { say "  MISSING disk: under 40 GiB free"; bad=1; }
    if [ -n "$lists" ]; then
        say "  lists: $lists ($(ls "$lists"/*.names 2>/dev/null | wc -l | tr -d ' ') name lists$([ -f "$lists/world-tier.txt" ] && echo ', world tier list'))"
    else
        say "  lists: none (overlays that need names and the pipeline tier will be skipped; README.md, Setup)"
    fi
    [ "$dry" = 1 ] && return 0
    [ "$bad" = 0 ] || die "preflight failed (above)"
}

st_binaries() {
    run env BUILD_PLAY_JOBS="$((jobs * 2))" "$D/build_play.sh"
    run cmake -S "$port/d3d12_metal/cache" -B "$port/build/setup-cache-x86_64" -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
    run ninja -C "$port/build/setup-cache-x86_64" -j "$((jobs * 2))" prepare_shaders
    run cmake -S "$port/shaderc_msl" -B "$port/build/setup-shaderc_msl-arm64" -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA6RT_SHADERC_MSL_TESTS=OFF
    run ninja -C "$port/build/setup-shaderc_msl-arm64" shaderc_msl_probe shaderc_msl_mesh_run
    link_out shaderc_msl
    run mkdir -p "$store/bin2"
    run cp -p "$port/build/setup-shaderc_msl-arm64/shaderc_msl_probe" "$store/bin2/msl_probe"
    run cp -p "$port/build/setup-shaderc_msl-arm64/shaderc_msl_mesh_run" "$store/bin2/mesh_run"
}

pick_sample() {
    local f; f="$(sample_names)"
    [ "$dry" = 1 ] && { note "sample: up to $sample names from the carve index plus one main-part name from each list -> $f"; return 0; }
    "$py" -I - "$work/shaders/dxbc/index.json" "$sample" "$f" ${lists:+"$lists"} <<'EOF'
import json, os, re, sys
idx, n, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
lists = sys.argv[4] if len(sys.argv) > 4 else ""
carved = {e["file"][:-len(".dxbc")]: e["stage"] for e in json.load(open(idx))}
want = []
if lists:
    for f in sorted(os.listdir(lists)):
        p = os.path.join(lists, f)
        if f.endswith(".names"):
            for l in open(p):
                l = l.strip()
                if l in carved:
                    want.append(l)
                    break
    tier = os.path.join(lists, "world-tier.txt")
    if os.path.exists(tier):
        for l in open(tier):
            names = re.findall(r"\b(?:vs|ps|ms|cs)=([^@\s]+)@", l)
            if names and all(x in carved for x in names) and len(names) >= 2:
                want += names
                break
want = list(dict.fromkeys(want))
by = {}
for k, st in sorted(carved.items()):
    by.setdefault(st, []).append(k)
extra, i = [], 0
while len(extra) < max(n, 4) and i < max(len(v) for v in by.values()):
    for st in ("vs", "ps", "ms", "cs"):
        if i < len(by.get(st, [])) and by[st][i] not in want and len(extra) < max(n, 4):
            extra.append(by[st][i])
    i += 1
want += extra
open(out, "w").write("\n".join(want) + "\n")
print("sample: %d shaders -> %s" % (len(want), out))
EOF
}

st_corpus() {
    need_game
    link_out corpus
    run mkdir -p "$corpus/game" "$corpus/out" "$work"
    if [ "$dry" = 1 ]; then
        note "links every file of $package and the executable (as game_scarlett_master.exe) into $corpus/game"
    else
        (cd "$package" && find . -type d) | while read -r d; do mkdir -p "$corpus/game/$d"; done
        (cd "$package" && find . -type f) | while read -r f; do [ -e "$corpus/game/$f" ] || ln -s "$package/$f" "$corpus/game/$f"; done
        [ -e "$corpus/game/game_scarlett_master.exe" ] || ln -s "$exe" "$corpus/game/game_scarlett_master.exe"
    fi
    if [ "$sample" -gt 0 ]; then
        local dx="$work/shaders/dxbc"
        run mkdir -p "$dx"
        [ -f "$dx/index.json" ] || run "$py" -I "$T/shader_carve.py" "$corpus/game/shaders.rpf" "$dx"
        pick_sample
        if [ "$dry" = 0 ]; then
            "$py" -I -c 'import json,sys; n=set(l.strip() for l in open(sys.argv[2])); json.dump([e for e in json.load(open(sys.argv[1])) if e["file"][:-5] in n], open(sys.argv[3],"w"))' \
                "$dx/index.json" "$(sample_names)" "$state/sample-carve.json"
        fi
        run "$py" -I "$T/fssm_unpack.py" "$dx" --carve-index "$state/sample-carve.json"
        run "$py" -I "$T/shader_disasm.py" "$dx/fssm" --unpack-index "$dx/fssm_index.json" --out "$dx/disasm" --mcpu gfx1013 --jobs "$jobs"
        run "$py" -I "$port/shaders_vk/tools/build_corpus.py" --work "$work" --out "$spirv" --parts main --names "$(sample_names)" --no-apple --jobs "$jobs"
        if [ "$dry" = 0 ]; then
            "$py" -I -c 'import json,sys; i=json.load(open(sys.argv[1])); n=[l.strip() for l in open(sys.argv[2]) if l.strip() in i]; open(sys.argv[2],"w").write("\n".join(n)+"\n"); print("sample: %d shaders translated" % len(n))' \
                "$spirv/index.json" "$(sample_names)"
            local part
            for part in library exe fspx bvh; do
                [ -f "$spirv/$part/index.json" ] || { mkdir -p "$spirv/$part"; echo '{}' >"$spirv/$part/index.json"; }
            done
            note "sample: the sub-corpora (library, exe, fspx, bvh) are empty; the full stage translates them with make_game.py"
        fi
        return
    fi
    run env LLVM_MC="$llvm_mc" "$py" -I "$port/shaders_vk/translator/make_game.py" --game "$corpus/game" --out "$corpus/out" \
        --work "$work" --stages check,shaders --jobs "$jobs" --no-tui
}

st_bcpack() {
    need_game
    local tree="$state/bcpack-tree"
    note "the generator reads ../runtime/src/bcpack{,_stream}.h beside its tools folder; a copy with port/portable's headers"
    run rm -rf "$tree"
    run mkdir -p "$tree/runtime/src" "$data/bcpack"
    run cp -R "$T" "$tree/tools"
    run cp "$port/portable/src/bcpack.h" "$port/portable/src/bcpack_stream.h" "$tree/runtime/src/"
    run "$py" -I -B "$tree/tools/build_dstorage_history.py" "$package" --out "$data/bcpack/history.bcph" --if-needed
}

st_store_pairs() {
    link_out shaderc_msl
    run "$py" -I "$S/pipelines.py" --rpf "$corpus/game/shaders.rpf" --dxbc-dir "$work/shaders/dxbc" --out "$store/pipelines"
    if [ "$sample" -gt 0 ]; then
        note "sample: pairs.json keeps only pipelines whose shaders are all in the sample"
        [ "$dry" = 1 ] || "$py" -I - "$store/pipelines/pairs.json" "$(sample_names)" <<'EOF'
import json, sys
p, names = sys.argv[1:]
keep = set(l.strip() for l in open(names))
d = json.load(open(p))
ok = lambda e: all(e[k] in keep for k in ("ps", "vs", "ms", "cs") if e.get(k))
d["graphics"] = [e for e in d["graphics"] if ok(e)]
d["compute"] = [e for e in d["compute"] if ok(e)]
json.dump(d, open(p, "w"))
print("sample pairs: %d graphics, %d compute" % (len(d["graphics"]), len(d["compute"])))
EOF
    fi
}

st_store_fit() {
    run "$py" -I "$S/mesh_fit.py" --spirv-dir "$spirv" --pairs "$store/pipelines/pairs.json" --out "$store/mesh" --jobs "$jobs"
}

extra_dirs() {
    local out="" d
    for d in library exe fspx bvh; do [ -f "$spirv/$d/index.json" ] && out="$out${out:+,}$d"; done
    [ "$dry" = 1 ] && [ "$sample" = 0 ] && out="library,exe,fspx,bvh"
    echo "$out"
}

st_store_batch() {
    local extra; extra="$(extra_dirs)"
    if [ "$sample" -gt 0 ]; then
        run "$py" -I "$S/batch.py" --spirv-dir "$spirv" --build "$store" --jobs "$jobs" --extra-dirs "$extra" --names-file "$(sample_names)"
    else
        run "$py" -I "$S/batch.py" --spirv-dir "$spirv" --build "$store" --jobs "$jobs" --extra-dirs "$extra" --fast-resume
    fi
}

st_store_routes() {
    local extra; extra="$(extra_dirs)"
    run mkdir -p "$store/meshsplit" "$store/tmp/msc"
    run "$py" -I "$S/mesh_pso.py" --spirv-dir "$spirv" --build "$store" --jobs 1
    run "$py" -I "$S/mesh_route.py" build --build "$store" --jobs 2
    if [ "$dry" = 0 ]; then
        "$py" -I -c 'import json,sys; [print(r["stem"]) for r in map(json.loads, open(sys.argv[1])) if r.get("step") == 4]' \
            "$store/mesh/mesh_fit.jsonl" >"$state/msc.stems"
    fi
    if [ "$dry" = 1 ] || [ -s "$state/msc.stems" ]; then
        run "$py" -I "$S/mesh_msc.py" --stems "$state/msc.stems" --work "$store/tmp/msc" --build "$store" --out "$store/meshsplit/msc_setup.jsonl"
    else
        note "no mesh-as-compute shaders in this corpus"
    fi
    run "$py" -I "$S/mesh_route.py" objects --build "$store"
    run "$py" -I "$S/mesh_route.py" decide --build "$store"
    run "$py" -I "$S/batch.py" --spirv-dir "$spirv" --build "$store" --extra-dirs "$extra" --summary-only
}

names_for() {
    local f; f="$(list "$1")" || return 1
    if [ "$sample" -gt 0 ] && [ "$dry" = 0 ]; then
        grep -Fxf "$(sample_names)" "$f" >"$state/sample-$1" || true
        if [ ! -s "$state/sample-$1" ] && [ -n "${2:-}" ]; then
            awk -F'\t' -v st="$2" 'NR == FNR {w[$1] = 1; next} !/^#/ && ($1 in w) && $3 == st {print $1; exit}' \
                "$(sample_names)" "$store/runs/main/manifest.tsv" >"$state/sample-$1"
            [ -s "$state/sample-$1" ] && echo "  sample: no $1 name in the sample; using $(cat "$state/sample-$1") (stage $2)" >&2
        fi
        [ -s "$state/sample-$1" ] || head -1 "$f" >"$state/sample-$1"
        echo "$state/sample-$1"
    else
        echo "$f"
    fi
}

skip() { note "skipped: $*"; }

st_ov_mspeed() {
    local f; f="$(names_for mspeed.names ms)" || { skip "no mspeed.names in --lists"; return 0; }
    link_out mspeed-x86_64
    run mkdir -p "$port/build/mspeed-x86_64/ov"
    run "$py" -I "$S/mspeed_overlay.py" "$port/build/mspeed-x86_64/ov" --names-file "$f" --jobs 2
    run "$py" -I "$here/setup/overlay_root.py" "$port/build/mspeed-x86_64/ov/overlay.tsv" "$port/build/mspeed-x86_64/overlay.tsv"
}

st_ov_rectvs() {
    local f; f="$(names_for rectvs.names vs)" || { skip "no rectvs.names in --lists"; return 0; }
    link_out expo-x86_64
    run mkdir -p "$port/build/expo-x86_64/rectvs"
    run "$py" -I "$D/rect_vs.py" "$port/build/expo-x86_64/rectvs" --names-file "$f"
}

st_ov_mop() {
    local f; f="$(names_for mop.names ms)" || { skip "no mop.names in --lists"; return 0; }
    link_out gpass-x86_64
    run mkdir -p "$port/build/gpass-x86_64/mop"
    run "$py" -I "$S/mesh_out_private.py" "$port/build/gpass-x86_64/mop" --force --list "$f" --jobs 2
}

st_ov_tmem() {
    local m l
    m="$(names_for tmem-merge.names cs)" || m=""
    l="$(names_for tmem-loopcap.names cs)" || l=""
    [ -n "$m$l" ] || { skip "no tmem-*.names in --lists"; return 0; }
    link_out tmem-x86_64
    run mkdir -p "$port/build/tmem-x86_64"
    local mj="" lj=""
    if [ "$dry" = 0 ]; then
        [ -n "$m" ] && mj="$(grep -v '^#' "$m" | paste -sd, -)"
        [ -n "$l" ] && lj="$(grep -v '^#' "$l" | paste -sd, -)"
    else
        mj="<tmem-merge.names, comma-separated>"; lj="<tmem-loopcap.names, comma-separated>"
    fi
    link_out pc7
    run mkdir -p "$port/build/pc7"
    note "tmem_overlay.py copies its catalog's root= line, so the catalog must sit beside build/tmem-x86_64"
    run "$py" -I "$port/d3d12_metal/cache/tools/catalog_merge.py" --base "$store/runs/main/manifest.tsv" --out "$port/build/pc7/catalog-base.tsv"
    run "$py" -I "$S/tmem_overlay.py" --catalog "$port/build/pc7/catalog-base.tsv" --out "$port/build/tmem-x86_64" \
        --corpus "$spirv" --merge "$mj" --loop-cap "$lj" --cap 16384
}

st_ov_mcc() {
    link_out mcc-x86_64
    run mkdir -p "$port/build/mcc-x86_64"
    local f
    if f="$(names_for mcc.names ms)"; then
        run "$py" -I "$S/mesh_out_private.py" "$port/build/mcc-x86_64" --list "$f" --jobs 2
    elif [ "$sample" -gt 0 ]; then
        run "$py" -I "$S/mesh_out_private.py" "$port/build/mcc-x86_64" --scan --jobs 2
    else
        run "$py" -I "$S/mesh_out_private.py" "$port/build/mcc-x86_64" --scan --jobs 2
    fi
}

st_ov_overrides() {
    link_out setup-tgpack
    link_out d3d12_metal-msl-overrides
    local tg="$port/build/setup-tgpack" p s
    run mkdir -p "$tg" "$port/build/d3d12_metal-msl-overrides"
    p="$(names_for overrides-pack.names ms)" || p=""
    s="$(names_for overrides-split.names)" || s=""
    if [ -z "$p$s" ]; then
        run "$py" -I "$D/mesh_tg_pack.py" "$tg" --scan --jobs 2
    else
        if [ -n "$s" ] && [ "$dry" = 0 ]; then
            awk -F'\t' 'NR == FNR {w[$1] = 1; next} !/^#/ && ($1 in w) {print $1 "\t" $11}' "$s" "$store/runs/main/manifest.tsv" >"$state/overrides-split.routes"
            awk -F'\t' '$2 == "split64" {print $1}' "$state/overrides-split.routes" >"$state/overrides-repack.names"
            awk -F'\t' '$2 != "split64" {print $1}' "$state/overrides-split.routes" >"$state/overrides-tosplit.names"
        fi
        local pack_names=() split_names=()
        if [ "$dry" = 0 ]; then
            [ -n "$p" ] && while read -r n; do [ -n "$n" ] && pack_names+=("$n"); done <"$p"
            [ -s "$state/overrides-repack.names" ] && while read -r n; do pack_names+=("$n"); done <"$state/overrides-repack.names"
            [ -s "$state/overrides-tosplit.names" ] && while read -r n; do split_names+=("$n"); done <"$state/overrides-tosplit.names"
        else
            pack_names=("<overrides-pack.names + overrides-split.names already split64 in the store>")
            split_names=("<overrides-split.names that are hw in the store>")
        fi
        [ "${#pack_names[@]}" -gt 0 ] && run "$py" -I "$D/mesh_tg_pack.py" "$tg" --force --jobs 2 "${pack_names[@]}"
        [ "${#split_names[@]}" -gt 0 ] && run "$py" -I "$D/mesh_tg_pack.py" "$tg" --split --jobs 2 "${split_names[@]}"
    fi
    run "$py" -I "$D/msl_overlay.py" "$tg/manifest.tsv" "$port/build/d3d12_metal-msl-overrides"
}

st_ov_uintps() {
    local f a
    f="$(names_for uintps.names ps)" || { skip "no uintps.names in --lists"; return 0; }
    a="$(names_for uintps-also.names cs)" || a=""
    link_out uintps-x86_64
    local u="$port/build/uintps-x86_64" tierlist=()
    run mkdir -p "$u"
    [ -n "$(list world-tier.txt)" ] && tierlist=(--lists "$lists/world-tier.txt")
    run "$py" -I "$S/output_variants.py" scan --spirv-dir "$spirv" --base "$store/runs/main/manifest.tsv" --out "$u" "${tierlist[@]}"
    if [ -n "$a" ]; then
        run "$py" -I "$S/output_variants.py" build --spirv-dir "$spirv" --base "$store/runs/main/manifest.tsv" --out "$u" \
            --names "$f" --also "$a" --jobs 2
    else
        run "$py" -I "$S/output_variants.py" build --spirv-dir "$spirv" --base "$store/runs/main/manifest.tsv" --out "$u" \
            --names "$f" --jobs 2
    fi
}

overlay_files() {
    local o
    for o in uintps-x86_64/overlay.tsv d3d12_metal-msl-overrides/manifest.tsv mcc-x86_64/overlay.tsv tmem-x86_64/overlay.tsv \
             gpass-x86_64/mop/overlay.tsv expo-x86_64/rectvs/manifest.tsv mspeed-x86_64/overlay.tsv; do
        if [ "$dry" = 1 ] || [ -f "$port/build/$o" ]; then echo "$port/build/$o"; fi
    done
}

st_catalog() {
    link_out pc7
    local args=() o
    while read -r o; do [ -n "$o" ] && args+=(--overlay "$o"); done < <(overlay_files)
    run mkdir -p "$port/build/pc7"
    run "$py" -I "$port/d3d12_metal/cache/tools/catalog_merge.py" --base "$store/runs/main/manifest.tsv" "${args[@]}" \
        --out "$port/build/pc7/catalog-setup.tsv"
}

st_tier() {
    link_out pc7
    local src=""
    if [ -n "$tier_from" ]; then
        src="$tier_from/pcache-manifest.txt"
        [ "$dry" = 1 ] || [ -f "$src" ] || die "--tier-from: no $src"
    elif [ -n "$(list world-tier.txt)" ]; then
        src="$lists/world-tier.txt"
    else
        skip "no world-tier.txt in --lists and no --tier-from: the first runs compile the world's pipelines while they play (dark world for a minute or more); afterwards: tools/setup.sh --only tier --tier-from <run dir>/metal-archive"
        return 0
    fi
    local a="$port/build/pc7/arch-world" m="$src"
    run mkdir -p "$a"
    if [ "$sample" -gt 0 ] && [ "$dry" = 0 ]; then
        "$py" -I - "$src" "$(sample_names)" "$state/sample-tier.txt" <<'EOF'
import re, sys
src, names, out = sys.argv[1:]
keep = set(l.strip() for l in open(names))
lines = [l for l in open(src) if l.startswith("#") or
         (re.findall(r"\b(?:vs|ps|ms|cs|as)=([^@\s]+)@", l) and all(n in keep for n in re.findall(r"\b(?:vs|ps|ms|cs|as)=([^@\s]+)@", l)))]
open(out, "w").writelines(lines)
print("sample tier: %d pipelines" % sum(1 for l in lines if not l.startswith("#")))
EOF
        m="$state/sample-tier.txt"
    fi
    run "$prep" --manifest "$m" --catalog "$port/build/pc7/catalog-setup.tsv" --archive-dir "$a" --jobs "$jobs" --title-jobs 1
    run cp "$m" "$a/pcache-tier.txt"
}

st_config() {
    need_game
    run mkdir -p "$state"
    local c="$state/config.sh"
    if [ "$dry" = 1 ]; then note "writes $c (paths below; tools/play.sh reads it)"; fi
    local body
    body="$(cat <<EOF
# written by tools/setup.sh $(date '+%F %T'); read by tools/play.sh
SETUP_PACKAGE=$(printf '%q' "$package")
SETUP_EXE=$(printf '%q' "$exe")
SETUP_BIN=$(printf '%q' "$bin")
SETUP_DATA=$(printf '%q' "$data")
GTA6RT_SOURCE_ROOT=$(printf '%q' "$port")
GTA6RT_METAL_SPIRV=$(printf '%q' "$spirv")
LEONIDA_DSTORAGE_HISTORY=$(printf '%q' "$data/bcpack/history.bcph")
EOF
)"
    if [ "$dry" = 1 ]; then echo "$body" | sed 's/^/    | /'; else echo "$body" >"$c"; echo "$data" >"$state/data"; say "  wrote $c"; fi
}

# ---- driver ----

outputs_kib() {
    du -sk "$data/" 2>/dev/null | awk '{print $1 + 0}'
}

selected() {
    local s="$1"
    if [ -n "$only" ]; then case ",$only," in *",$s,"*) return 0 ;; *) return 1 ;; esac; fi
    if [ -n "$from" ]; then
        local hit=0 x
        for x in $STAGES; do [ "$x" = "$from" ] && hit=1; [ "$x" = "$s" ] && return $((1 - hit)); done
        return 1
    fi
    return 0
}
for x in ${only//,/ } $from; do case " $STAGES " in *" $x "*) ;; *) die "unknown stage $x (--stages lists them)" ;; esac; done

if [ "$dry" = 0 ]; then
    sample_guard
    place_build
    mkdir -p "$state/done"
    [ "$sample" -gt 0 ] && echo "$sample" >"$state/sample.mode"
fi
if [ "$dry" = 1 ]; then
    say "setup.sh --dry-run: $port"
    say "  package $package; executable $exe"
    say "  data $data$([ "$data" != "$port/build" ] && echo " (build/ moves to $data/build and becomes a link)"); lists ${lists:-none}; jobs $jobs$([ "$sample" -gt 0 ] && echo "; sample $sample")"
    say "  measured on an M4 Pro, 24 GB, macOS 27.0.1 (M = measured, I = inferred); total about 10-12 h and 32 GiB"
fi
for s in $STAGES; do
    selected "$s" || continue
    fn="st_${s//-/_}"
    if [ "$dry" = 1 ]; then
        st="todo"; [ -f "$state/done/$s" ] && st="done $(cat "$state/done/$s")"
        say ""
        say "[$s] $st; $(cost "$s")"
        "$fn"
        continue
    fi
    if [ -f "$state/done/$s" ] && [ "$redo" = 0 ] && [ -z "$only" ]; then
        say "[$s] done $(cat "$state/done/$s"); skipped"
        continue
    fi
    say "[$s] $(date '+%T') started; $(cost "$s")"
    echo "[$s] $(date '+%F %T') started" >>"$state/setup.log"
    t0=$(date +%s)
    k0=$(outputs_kib)
    if ! "$fn"; then
        echo "[$s] failed; see above and $state/setup.log" >&2
        exit 1
    fi
    t1=$(date +%s)
    k1=$(outputs_kib)
    echo "$(date '+%F %T') $((t1 - t0)) s, $(((k1 - k0) / 1024)) MiB" >"$state/done/$s"
    say "[$s] done in $((t1 - t0)) s, disk $(((k1 - k0) / 1024)) MiB"
    echo "[$s] $(date '+%F %T') done $((t1 - t0)) s $(((k1 - k0) / 1024)) MiB" >>"$state/setup.log"
done
[ "$dry" = 1 ] && exit 0
say "setup finished. Start with tools/play.sh (tools/play.sh --dry-run shows what it would do)."
