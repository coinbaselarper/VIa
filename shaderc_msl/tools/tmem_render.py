#!/usr/bin/env python3
"""Render rows for tmem_probe rlist from a tmem_rank json: pixel shaders with a generated vertex function that
writes every user input of the fragment, mesh shaders (split64 with their object stage), vertex shaders.

  tmem_render.py --rank rank.json --catalog CATALOG.tsv --out rlist.tsv --work DIR [--jobs 3]
"""
import argparse, json, os, re, subprocess, sys, threading
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spv  # noqa: E402
import shaderc_msl as sm  # noqa: E402
import tmem_rank  # noqa: E402

FMT = {"float": 55, "half": 25, "uint": 53, "int": 54, "float2": 105, "half2": 65, "uint2": 103, "int2": 104,
       "float4": 125, "half4": 115, "uint4": 123, "int4": 124, "float3": 125, "half3": 115, "uint3": 123, "int3": 124}


def struct(msl, name):
    m = re.search(r"^struct " + name + r"\n\{\n(.*?)^\};", msl, re.S | re.M)
    return m.group(1) if m else ""


def vertex_source(msl):
    fields = []
    for line in struct(msl, "main0_in").splitlines():
        m = re.match(r"\s*(\w+) (\w+) \[\[([^\]]*)\]\];", line)
        if not m:
            continue
        attrs = [a.strip() for a in m.group(3).split(",")]
        keep = [a for a in attrs if a.startswith("user(") or a in ("render_target_array_index", "viewport_array_index")]
        if keep:
            fields.append("    %s %s [[%s]];" % (m.group(1), m.group(2), keep[0]))
    return ("#include <metal_stdlib>\nusing namespace metal;\nstruct VO\n{\n    float4 p [[position]];\n%s\n};\n"
            "vertex VO tmem_v(uint vid [[vertex_id]])\n{\n    VO o = {};\n    o.p = float4(0);\n    return o;\n}\n"
            % "\n".join(fields))


def colors(msl):
    out = {}
    for line in struct(msl, "main0_out").splitlines():
        m = re.match(r"\s*(\w+) \w+ \[\[color\((\d+)\)(, index\(1\))?\]\];", line)
        if m and not m.group(3):
            out[int(m.group(2))] = FMT.get(m.group(1), 115)
    depth = 252 if "[[depth(" in msl.split("struct main0_out", 1)[-1].split("};", 1)[0] else 0
    if "[[stencil]]" in struct(msl, "main0_out"):
        depth = 260
    if not out and not depth:
        out[0] = 115
    return [out.get(i, 115) for i in range(max(out) + 1)] if out else [], depth


def cross(spv_path, stage, work, key):
    base = os.path.join(work, key)
    if not os.path.exists(base + ".metal"):
        tmp = "%s.%d" % (base, threading.get_ident())
        w, _, extra = sm.prepare(spv.load(spv_path), stage, {})
        spv.save(w, tmp + ".spv")
        r = subprocess.run([sm.SPIRV_CROSS, tmp + ".spv", *sm.RECIPE["cross"], "--msl-version",
                            str(sm.RECIPE["msl_version"]), *extra, "--output", tmp + ".metal"], capture_output=True)
        os.remove(tmp + ".spv")
        if r.returncode:
            return None
        os.replace(tmp + ".metal", base + ".metal")
    return open(base + ".metal").read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rank", required=True)
    ap.add_argument("--catalog", required=True)
    ap.add_argument("--specs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--corpus", default=tmem_rank.CORPUS)
    ap.add_argument("--jobs", type=int, default=3)
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    rows, base = tmem_rank.read_catalog(a.catalog)
    mt = {}
    for line in open(a.specs):
        t = line.split()
        if len(t) > 2 and t[1] == "M":
            ms = t[2][3:]
            th = next((x[3:] for x in t if x.startswith("mt=")), "128.1.1").replace(".", ",")
            mt[ms] = th
    rank = [r for r in json.load(open(a.rank)) if r.get("stage") in ("ps", "ms", "vs") and r.get("key")]

    def one(r):
        name, st, key = r["name"], r["stage"], r["key"]
        dec = re.sub(r"~([0-9a-f]{2})", lambda m: chr(int(m.group(1), 16)), name)
        row = next((x for x in rows.get(dec, []) + rows.get(name, []) if x[1] == key), None)
        if not row:
            return None
        lib = os.path.normpath(os.path.join(base, row[4]))
        if st == "vs":
            return "vs\t%s\tmain0\t-\t-\t-\t%s" % (lib, name)
        if st == "ms":
            th = mt.get("%s@%s" % (name, r["kp"]), "128,1,1")
            if row[10] == "split64" and row[11] != "-":
                return "ms\t%s\t%s\t%s\t1,24\t-\t%s" % (os.path.normpath(os.path.join(base, row[11])),
                                                       os.path.normpath(os.path.join(base, row[12])), th, name)
            if row[10] == "mesh_as_compute":
                return None
            return "ms\t%s\t-\t%s\t1,0\t-\t%s" % (lib, th, name)
        msl = cross(os.path.join(a.corpus, dec + ".spv"), "ps", a.work, key)
        if msl is None:
            return None
        vpath = os.path.join(a.work, key + ".v.metal")
        open(vpath, "w").write(vertex_source(msl))
        cl, depth = colors(msl)
        return "ps\t%s\tmain0\t%s\t%s\t%d\t%s" % (lib, vpath, ",".join(map(str, cl)) or "-", depth, name)

    with ThreadPoolExecutor(a.jobs) as ex:
        out = [x for x in ex.map(one, rank) if x]
    with open(a.out, "w") as f:
        f.write("\n".join(out) + "\n")
    print(len(rank), len(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
