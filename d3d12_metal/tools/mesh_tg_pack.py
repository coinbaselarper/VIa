#!/usr/bin/env python3
"""Mesh shaders over Metal's 32 KiB of threadgroup memory: SPIRV-Cross stages every mesh output in a threadgroup
array of 128 entries, and an MSL float3 / uint3 takes 16 bytes there. This rebuilds such shaders with the store's own
recipe (port/shaderc_msl/tools/shaderc_msl.py, the passes recorded for the store's library) and one more MSL pass:
the float3 / uint3 output staging arrays of the mesh entry become packed_float3 / packed_uint3 (12 bytes; whole-
element reads and writes only, so the values are unchanged). Libraries go to OUT/cas; OUT/manifest.tsv is the store's
manifest with those rows pointing at them (GTA6RT_METAL_SHADER_MANIFEST=OUT/manifest.tsv).

  mesh_tg_pack.py OUT --scan [--jobs N]     every hw-route mesh shader of the store, sized; packed where over
  mesh_tg_pack.py OUT NAME...               these only
  mesh_tg_pack.py OUT --split NAME...       these through the store's meshlet split instead (route split64: object
                                            stage + 64-vertex mesh groups, 7 chunks), for shaders whose staged
                                            outputs alone pass Metal's 61,440-byte threadgroup + mesh output limit
"""
import argparse
import concurrent.futures as cf
import gzip
import hashlib
import json
import os
import re
import sys

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "shaderc_msl", "tools")
sys.path.insert(0, os.path.abspath(TOOLS))
import shaderc_msl as R  # noqa: E402
import spv  # noqa: E402

PORT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
STORE = os.path.join(PORT, "build", "shaderc_msl", "runs", "main")
STORE_ROOT = os.path.join(PORT, "build", "shaderc_msl")
SPIRV = os.path.abspath(os.path.join(PORT, "..", "spike", "s2-make-game", "work", "shaders", "spirv"))
LIMIT = 32768
VERSION = "mesh_tg_pack 2"

SIZES = {"float": 4, "float2": 8, "float3": 16, "float4": 16, "half": 2, "half2": 4, "half3": 8, "half4": 8,
         "int": 4, "int2": 8, "int3": 16, "int4": 16, "uint": 4, "uint2": 8, "uint3": 16, "uint4": 16, "bool": 1,
         "packed_float3": 12, "packed_uint3": 12, "packed_int3": 12, "uchar4": 4, "gl_MeshPerPrimitiveEXT": 1, "short": 2,
         "ushort": 2}
DECL = re.compile(r"^\s*threadgroup (?:spvUnsafeArray<(\w+), (\d+)>|(\w+)) (\w+);\s*$", re.M)


def manifest_rows():
    head, rows = [], {}
    for line in open(os.path.join(STORE, "manifest.tsv")):
        if line.startswith("#") and "\t" not in line:
            head.append(line)
            continue
        f = line.rstrip("\n").split("\t")
        rows[f[0]] = f
    return head, rows


def opts_of(row):
    lib = row[11] if len(row) > 11 and row[10] == "split64" else row[4]
    meta = json.load(open(os.path.join(STORE_ROOT, lib[: -len(".metallib")] + ".json")))
    o = {}
    for p in meta["passes"]:
        if p["pass"] == "drop_outputs":
            o["drop_outputs"] = p["locations"]
        elif p["pass"] == "trim_outputs":
            o["trim_outputs"] = p["widths"]
    return o


def entry_body(msl):
    i = msl.find("[[mesh]] void main0(")
    return (i, msl[i:]) if i >= 0 else (-1, "")


def tg_bytes(msl):
    _, body = entry_body(msl)
    total, arrays = 0, []
    for m in DECL.finditer(body):
        t, n, single, name = m.group(1) or m.group(3), int(m.group(2) or 1), m.group(3), m.group(4)
        if t not in SIZES:
            return None, []
        total += SIZES[t] * n
        arrays.append((name, t, n))
    return total, arrays


IDX = "gl_PrimitiveTriangleIndicesEXT"
IDX_WRITE = re.compile(r"^(\s*)" + IDX + r"\[([^\n;]+)\] = ([^\n;]+);$", re.M)


def pack_indices(msl):
    """The staged triangle indices (uint3, 16 bytes) as uchar4: Metal's mesh indices are uchar. Only when every use is
    the declaration, a parameter, a bare argument, a whole-element write or a .x/.y/.z read."""
    uses = len(re.findall(r"\b" + IDX + r"\b", msl))
    decl = msl.count("threadgroup spvUnsafeArray<uint3, 128> " + IDX)
    param = msl.count("threadgroup spvUnsafeArray<uint3, 128>& " + IDX)
    writes = len(IDX_WRITE.findall(msl))
    reads = len(re.findall(r"\b" + IDX + r"\[\w+\]\.[xyz]\b", msl))
    args = len(re.findall(r"[(,] ?" + IDX + r"\s*[,)]", msl))
    if not decl or decl + param + writes + reads + args != uses:
        return msl, False
    out = msl.replace("threadgroup spvUnsafeArray<uint3, 128> " + IDX, "threadgroup spvUnsafeArray<uchar4, 128> " + IDX)
    out = out.replace("threadgroup spvUnsafeArray<uint3, 128>& " + IDX, "threadgroup spvUnsafeArray<uchar4, 128>& " + IDX)
    out = IDX_WRITE.sub(lambda m: "%s%s[%s] = uchar4(uchar3(%s), 0);" % (m.group(1), IDX, m.group(2), m.group(3)), out)
    return out, True


def pack(msl, arrays):
    out, idx = pack_indices(msl)
    count = int(idx)
    for t in ("float3", "uint3", "int3"):
        for k in sorted({k for n, tt, k in arrays if tt == t and k > 1}):
            a = "threadgroup spvUnsafeArray<%s, %d>" % (t, k)
            b = "threadgroup spvUnsafeArray<packed_%s, %d>" % (t, k)
            count += out.count(a)
            out = out.replace(a, b)
    return out, count


def cross(name, row, tmp):
    raw = open(os.path.join(SPIRV, name + ".spv"), "rb").read()
    words = list(spv.struct.unpack("<%dI" % (len(raw) // 4), raw))
    opts = opts_of(row)
    w, passes, extra = R.prepare(words, "ms", opts)
    if len(row) > 15 and row[10] == "split64":
        opts["split_mesh"] = {"chunks": int(row[13]), "vmax": int(row[14]), "pmax": int(row[15])}
        w, _task = R.apply_split(w, passes, opts)
    os.makedirs(tmp, exist_ok=True)
    base = os.path.join(tmp, hashlib.sha256(name.encode()).hexdigest()[:16])
    open(base + ".spv", "wb").write(spv.to_bytes(w))
    rc, out, err, dt = R.run([R.SPIRV_CROSS, base + ".spv", *R.RECIPE["cross"], "--msl-version",
                              str(R.RECIPE["msl_version"]), *extra, "--output", base + ".metal"], R.RECIPE["cross_timeout"])
    if rc != 0:
        return None, passes, base
    text = open(base + ".metal", encoding="utf-8", errors="replace").read()
    fixed, _ = R.msl_fixups(text, passes)
    return (fixed if fixed is not None else text), passes, base


def build(out_dir, name, row, force):
    tmp = os.path.join(out_dir, "tmp", str(os.getpid()))
    msl, passes, base = cross(name, row, tmp)
    r = {"name": name}
    if msl is None:
        r["status"] = "cross_fail"
        return r
    before, arrays = tg_bytes(msl)
    r["before"] = before
    if before is None:
        r["status"] = "unsized"
        return r
    if before <= LIMIT and not force:
        r["status"] = "fits"
        return r
    packed, n = pack(msl, arrays)
    after, _ = tg_bytes(packed)
    r.update(after=after, packed_arrays=n)
    if after is None or after > LIMIT:
        r["status"] = "still_over"
        return r
    key = hashlib.sha256((VERSION + "\n" + packed).encode()).hexdigest()
    d = os.path.join(out_dir, "cas", key[:2])
    os.makedirs(d, exist_ok=True)
    lib = os.path.join(d, key + ".metallib")
    r["metallib"] = lib
    if not os.path.exists(lib):
        open(base + ".metal", "w").write(packed)
        with gzip.open(os.path.join(d, key + ".metal.gz"), "wb", compresslevel=1) as g:
            g.write(packed.encode())
        air = base + ".air"
        rc, o, e, dt = R.run([R.tool("metal"), "-std=" + R.RECIPE["std"], *R.RECIPE["metal"], base + ".metal", "-o", air],
                             R.RECIPE["metal_timeout"])
        if rc != 0:
            r["status"] = "metal_fail"
            r["error"] = (e or b"").decode("utf-8", "replace")[-600:]
            return r
        rc, o, e, dt = R.run([R.tool("metallib"), air, "-o", lib + ".part"], R.RECIPE["metal_timeout"])
        if rc != 0:
            r["status"] = "metallib_fail"
            return r
        os.replace(lib + ".part", lib)
        for ext in (".spv", ".metal", ".air"):
            try:
                os.remove(base + ext)
            except OSError:
                pass
    r["status"] = "packed"
    r["split_route"] = len(row) > 10 and row[10] == "split64"
    r["key"] = key
    r["passes"] = [p["pass"] for p in passes] + ["pack_threadgroup_vec3"]
    return r


def build_split(out_dir, name, row, chunks=7, vmax=64, pmax=64):
    tmp = os.path.join(out_dir, "tmp", str(os.getpid()))
    os.makedirs(tmp, exist_ok=True)
    o = opts_of(row)
    o["split_mesh"] = {"chunks": chunks, "vmax": vmax, "pmax": pmax}
    tr = R.translator_id(SPIRV)
    r = R.compile_one(os.path.join(SPIRV, name + ".spv"), "ms", os.path.join(out_dir, "cas"), tr, o, tmp=tmp)
    out = {"name": name, "status": "split" if r.get("status") == "ok" and r.get("object_key") else "split_" + str(r.get("status")),
           "error": r.get("error")}
    if out["status"] == "split":
        plan = r.get("plan") or {}
        sets = {k: v for k, v in (plan.get("sets") or {}).items() if int(k) < 30}
        out.update(key=r["key"], object_key=r["object_key"], chunks=chunks, vmax=vmax, pmax=pmax,
                   push=plan.get("push"), tables=",".join("%d:%d" % (int(k) - 1, v) for k, v in
                                                          sorted(sets.items(), key=lambda kv: int(kv[0]))) or "-",
                   mesh_lib=os.path.join(out_dir, "cas", r["key"][:2], r["key"] + ".metallib"),
                   object_lib=os.path.join(out_dir, "cas", r["object_key"][:2], r["object_key"] + ".metallib"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("names", nargs="*")
    ap.add_argument("--scan", action="store_true")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--split", action="store_true")
    a = ap.parse_args()
    out_dir = os.path.abspath(a.out)
    head, rows = manifest_rows()
    names = a.names or [n for n, f in rows.items() if f[2] == "ms" and len(f) > 10 and f[10] in ("hw", "split64")]
    if not a.scan and not a.names:
        ap.error("names or --scan")
    results = []
    with cf.ProcessPoolExecutor(max_workers=a.jobs) as ex:
        futs = {(ex.submit(build_split, out_dir, n, rows[n]) if a.split else ex.submit(build, out_dir, n, rows[n], a.force)): n
                for n in names if n in rows}
        for i, f in enumerate(cf.as_completed(futs)):
            try:
                r = f.result()
            except Exception as e:  # a shader the recipe cannot rebuild is reported, not fatal
                r = {"name": futs[f], "status": "error", "error": str(e)[:300]}
            results.append(r)
            if r["status"] not in ("fits",):
                print(json.dumps(r), flush=True)
            if i % 500 == 499:
                print("# %d of %d" % (i + 1, len(futs)), file=sys.stderr, flush=True)
    by = {}
    for r in results:
        by[r["status"]] = by.get(r["status"], 0) + 1
    print("# " + json.dumps(by), file=sys.stderr)
    log_path = os.path.join(out_dir, "mesh_tg_pack.jsonl")
    old = {}
    if os.path.exists(log_path):
        for line in open(log_path):
            r = json.loads(line)
            old[r["name"]] = r
    for r in results:
        old[r["name"]] = r
    with open(log_path, "w") as f:
        for r in old.values():
            f.write(json.dumps(r) + "\n")
    packed = {n: r for n, r in old.items() if r["status"] == "packed"}
    split = {n: r for n, r in old.items() if r["status"] == "split"}
    rel_root = os.path.relpath(STORE_ROOT, out_dir)
    with open(os.path.join(out_dir, "manifest.tsv.part"), "w") as f:
        for h in head:
            f.write(re.sub(r"root=\S+", "root=" + rel_root, h) if "root=" in h else h)
        for n, row in rows.items():
            if n in packed and packed[n].get("split_route"):
                row = list(row)
                row[11] = os.path.relpath(packed[n]["metallib"], STORE_ROOT)
                row[8] = (row[8] + "," if row[8] not in ("", "-") else "") + "pack_threadgroup_vec3"
            elif n in packed:
                row = list(row)
                row[1] = packed[n]["key"]
                row[4] = os.path.relpath(packed[n]["metallib"], STORE_ROOT)
                row[8] = (row[8] + "," if row[8] not in ("", "-") else "") + "pack_threadgroup_vec3"
            elif n in split:
                r = split[n]
                row = list(row) + ["-"] * max(0, 28 - len(row))
                row[10] = "split64"
                row[11] = os.path.relpath(r["mesh_lib"], STORE_ROOT)
                row[12] = os.path.relpath(r["object_lib"], STORE_ROOT)
                row[13:18] = [str(r["chunks"]), str(r["vmax"]), str(r["pmax"]),
                              str(r["push"]) if r["push"] is not None else "-", r["tables"]]
            f.write("\t".join(row) + "\n")
    os.replace(os.path.join(out_dir, "manifest.tsv.part"), os.path.join(out_dir, "manifest.tsv"))
    print("# manifest: %d rows, %d packed, %d split -> %s" % (len(rows), len(packed), len(split), os.path.join(out_dir, "manifest.tsv")),
          file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
