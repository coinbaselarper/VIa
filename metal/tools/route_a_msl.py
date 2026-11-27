#!/usr/bin/env python3
"""Route A (S0.4 / s5.2 recipe) MSL for the shader pairs the metal backend test builds pipelines from.

base:  spirv-cross --msl --msl-version 30000 --msl-argument-buffers --msl-argument-buffer-tier 1
       --msl-device-argument-buffer 0                      (the recipe exactly as S0.4 and s5.2 ran it)
split: the same after moving set-0 binding b to descriptor set 1 + b in the SPIR-V decorations
       (one argument buffer per contract binding, DESIGN.md section 6)

Reads SPIR-V and index.json from --spirv-dir (read only), writes everything under --out, and a line manifest
the C++ test reads. Nothing derived from game data is written into the source tree.
"""
import argparse, gzip, json, os, re, struct, subprocess, sys

PAIRS = [
    ("blit", "VS_Blit_fad9fd10_Wrapped", "PS_BlitMod_fad9fd10_Wrapped"),
    ("ui_text", "VInstancedText_27eac676_Wrapped", "FTextMul_27eac676_Wrapped"),
    ("ui_gradient", "_VInstancedPosition3dVertexCxformGradientMask_15356630_Wrapped",
     ".FVertexCxformMulGradientMask_15356630_Wrapped"),
    ("bink", "VS_Bink_e9aa19fd_Wrapped", "PS_BinkNoAlpha_e9aa19fd_Wrapped"),
]
# ids in spike/s5.2-msl-corpus/snapshot.json, to check the MSL is byte-identical to that run's step 1 output
S52_IDS = {
    "VS_Blit_fad9fd10_Wrapped": 22467, "PS_BlitMod_fad9fd10_Wrapped": 21042,
    "VInstancedText_27eac676_Wrapped": 22396, "FTextMul_27eac676_Wrapped": 20337,
    "_VInstancedPosition3dVertexCxformGradientMask_15356630_Wrapped": 25531,
    ".FVertexCxformMulGradientMask_15356630_Wrapped": 10125,
    "VS_Bink_e9aa19fd_Wrapped": 22428, "PS_BinkNoAlpha_e9aa19fd_Wrapped": 21032,
}
BASE = ["--msl", "--msl-version", "30000", "--msl-argument-buffers", "--msl-argument-buffer-tier", "1",
        "--msl-device-argument-buffer", "0"]
# Corpus compute shaders: two within Metal's 32 KiB threadgroup memory, two above it (translator lds_bytes).
COMPUTE = [
    ".OceanWaveSim_CS_InverseFFT_Y_6f3df6ff_Wrapped",
    ".WaterSimMgr_CS_ClearVelocity_805e733f_Wrapped",
    "Ivfx_proj_decal_trail_CS_UpdateProjDecalTrailMeshIndices_3e94584f_Wrapped",
    "Lvfx_proj_decal_hit_detection_CS_ComputeHitDetectionResults_7b0d06e8_Wrapped",
]
# Corpus mesh shaders across the S5.2 shapes: within the limits, over the mesh size, over size and scalars.
MESH = [
    "#MS_VS_WaterHeight_414eb82d_Wrapped",
    "!MS_VS_Transform_344d5e15_Wrapped",
    "!MS_VS_Transform_a1eba70b_Wrapped~1",
    "!MS_VS_Transform_eb34a03f_Wrapped~121",
    "!MS_VS_Transform_0c91b99b_Wrapped",
    "2MS_VS_MirrorReflectionTransformD_3d30f6df_Wrapped~36",
    "$MS_VS_TransformLit_1af1d70b_Wrapped",
    "$MS_VS_TransformLit_1af1d70b_Wrapped~4",
]


def split_sets(words):
    out = list(words)
    sets, binds = {}, {}
    i = 5
    while i < len(out):
        op, n = out[i] & 0xFFFF, out[i] >> 16
        if n == 0:
            raise ValueError("bad SPIR-V word count")
        if op == 71 and n >= 4:
            if out[i + 2] == 34:
                sets[out[i + 1]] = i + 3
            elif out[i + 2] == 33:
                binds[out[i + 1]] = i + 3
        i += n
    moved = []
    for var, pos in sets.items():
        if out[pos] == 0 and var in binds:
            b = out[binds[var]]
            out[pos] = 1 + b
            out[binds[var]] = 0
            moved.append(b)
    return out, sorted(set(moved))


def cross(tool, spv, flags):
    p = subprocess.run([tool, spv] + flags, capture_output=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError(f"spirv-cross {spv}: {p.stderr.decode(errors='replace')[:400]}")
    return p.stdout


def make_split_msl(tool, spv_path, out_dir, stem):
    raw = open(spv_path, "rb").read()
    words = struct.unpack("<%dI" % (len(raw) // 4), raw)
    split_words, moved = split_sets(words)
    split_spv = os.path.join(out_dir, stem + ".spv")
    open(split_spv, "wb").write(struct.pack("<%dI" % len(split_words), *split_words))
    flags = ["--msl", "--msl-version", "30000", "--msl-argument-buffers", "--msl-argument-buffer-tier", "1"]
    for s in range(1, 30):
        flags += ["--msl-device-argument-buffer", str(s)]
    path = os.path.join(out_dir, stem + ".metal")
    open(path, "wb").write(cross(tool, split_spv, flags))
    return path, moved


def mesh_verdicts(s52_dir):
    try:
        stems = json.load(open(os.path.join(s52_dir, "snapshot.json")))["stems"]
        shapes = json.load(open(os.path.join(s52_dir, "mesh-shapes", "shapes.json")))
    except OSError:
        return {}
    out = {}
    for sh in shapes:
        for i in sh["ids"]:
            out[stems[i]] = (sh["result"], sh.get("mesh_bytes_reported"), sh.get("unique_scalars_reported"),
                             sh["vertex_scalars"])
    return out


def struct_body(msl, name):
    m = re.search(r"^struct " + name + r"\n\{\n(.*?)^\};", msl, re.S | re.M)
    return m.group(1) if m else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spirv-dir", required=True)
    ap.add_argument("--spirv-cross", default="/opt/homebrew/bin/spirv-cross")
    ap.add_argument("--out", required=True)
    ap.add_argument("--metallib", action="store_true", help="also build split/<stem>.metallib with xcrun metal")
    ap.add_argument("--s52-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                       "../../../spike/s5.2-msl-corpus"))
    a = ap.parse_args()
    os.makedirs(os.path.join(a.out, "base"), exist_ok=True)
    os.makedirs(os.path.join(a.out, "split"), exist_ok=True)
    index = json.load(open(os.path.join(a.spirv_dir, "index.json")))
    lines = []
    for label, vs, ps in PAIRS:
        lines.append(f"pair {label} {vs} {ps}")
        for stem in (vs, ps):
            spv = os.path.join(a.spirv_dir, stem + ".spv")
            meta = index.get(stem, {})
            base_msl = cross(a.spirv_cross, spv, BASE)
            base_path = os.path.join(a.out, "base", stem + ".metal")
            open(base_path, "wb").write(base_msl)
            raw = open(spv, "rb").read()
            words = struct.unpack("<%dI" % (len(raw) // 4), raw)
            split_words, moved = split_sets(words)
            split_spv = os.path.join(a.out, "split", stem + ".spv")
            open(split_spv, "wb").write(struct.pack("<%dI" % len(split_words), *split_words))
            flags = ["--msl", "--msl-version", "30000", "--msl-argument-buffers", "--msl-argument-buffer-tier", "1"]
            for s in range(1, 30):
                flags += ["--msl-device-argument-buffer", str(s)]
            split_msl = cross(a.spirv_cross, split_spv, flags)
            split_path = os.path.join(a.out, "split", stem + ".metal")
            open(split_path, "wb").write(split_msl)
            lib_path = "-"
            if a.metallib:
                air = os.path.join(a.out, "split", stem + ".air")
                lib_path = os.path.join(a.out, "split", stem + ".metallib")
                subprocess.run(["xcrun", "metal", "-std=metal3.0", "-w", "-c", split_path, "-o", air], check=True, timeout=600)
                subprocess.run(["xcrun", "metallib", air, "-o", lib_path], check=True, timeout=600)
            same = "n/a"
            sid = S52_IDS.get(stem)
            gz = os.path.join(a.s52_dir, "msl", f"{sid:05d}.metal.gz") if sid is not None else ""
            if gz and os.path.exists(gz):
                same = "yes" if gzip.open(gz).read() == base_msl else "no"
            stage = meta.get("stage", "?")
            bindings = ",".join(str(b) for b in meta.get("bindings", [])) or "-"
            lines.append(f"shader {stem} {stage} {base_path} {split_path} {meta.get('push_constant_bytes', 0)} "
                         f"{bindings} s52_identical={same} moved={','.join(map(str, moved)) or '-'} {lib_path}")
            if stage == "vs":
                for e in meta.get("input_signature", []):
                    if e.get("system_value", 0):
                        continue
                    n = bin(e.get("mask", 0)).count("1")
                    lines.append(f"input {stem} {e['register']} {e.get('component_type', 'float')} {n} {e.get('semantic', '?')}")
            else:
                text = base_msl.decode("utf-8", "replace")
                for m in re.finditer(r"(\w+) \w+ \[\[color\((\d+)\)\]\]", struct_body(text, "main0_out")):
                    lines.append(f"color {stem} {m.group(2)} {m.group(1)}")
                if "[[depth(" in struct_body(text, "main0_out"):
                    lines.append(f"depth {stem}")
    os.makedirs(os.path.join(a.out, "compute"), exist_ok=True)
    os.makedirs(os.path.join(a.out, "mesh"), exist_ok=True)
    for stem in COMPUTE:
        spv = os.path.join(a.spirv_dir, stem + ".spv")
        if not os.path.exists(spv):
            continue
        meta = index.get(stem, {})
        path, _ = make_split_msl(a.spirv_cross, spv, os.path.join(a.out, "compute"), stem)
        ls = meta.get("local_size", [0, 0, 0])
        bindings = ",".join(str(b) for b in meta.get("bindings", [])) or "-"
        lines.append(f"compute {stem} {path} {meta.get('lds_bytes', 0)} {bindings} {ls[0]},{ls[1]},{ls[2]}")
    verdicts = mesh_verdicts(a.s52_dir)
    for stem in MESH:
        spv = os.path.join(a.spirv_dir, stem + ".spv")
        if not os.path.exists(spv):
            continue
        path, _ = make_split_msl(a.spirv_cross, spv, os.path.join(a.out, "mesh"), stem)
        v = verdicts.get(stem, ("?", None, None, 0))
        lines.append(f"mesh {stem} {path} {v[0]} {v[1] or '-'} {v[2] or '-'} {v[3]}")
    open(os.path.join(a.out, "manifest.txt"), "w").write("\n".join(lines) + "\n")
    print(f"route_a_msl: {len(PAIRS)} pairs -> {a.out}/manifest.txt")


if __name__ == "__main__":
    sys.exit(main())
