#!/usr/bin/env python3
"""Debug variants of store shaders that count what the GPU did into the backend's debug buffer (Metal buffer index 30,
port/metal set_debug_buffer; the adapter binds it with GTA6RT_METAL_GPU_DEBUG=1 and prints it). Each shader is rebuilt
with the store's own recipe (the passes its CAS record lists, the SPIR-V the store was built from; the raw SPIRV-Cross
output must have the recorded size, else the shader is skipped), one MSL change is made, and the library goes to
OUT/cas; OUT/manifest.tsv is an overlay (only those rows, paths relative to the store root) for
GTA6RT_METAL_SHADER_DEBUG_OVERLAY.

Words of the debug buffer written here (the pre-pass owns 0-127):
  mesh (hw route): 128 groups run, 129 groups with primitives, 130 primitives, 131 vertices, 132 primitives culled by
    the shader's own flag, 133 sample slots taken; samples at 256 + 16 * slot: group id x, vertices, primitives, the
    first unculled primitive, then that primitive's three gl_Position (x, y, z, w floats)
  pixel: 160 invocations, 161 discards, 162 returns from main0

  msl_instrument.py OUT REGEX... [--base OVERLAY] [--jobs N]
    --base: an overlay whose rows replace the store's first (the packed mesh shaders): their packed MSL is instrumented
"""
import argparse
import concurrent.futures as cf
import gzip
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mesh_tg_pack as P  # noqa: E402

R = P.R
VERSION = "msl_instrument 1"


def cas_meta(row, stage):
    lib = row[11] if stage == "ms" and len(row) > 11 and row[10] == "split64" else row[4]
    path = os.path.join(P.STORE_ROOT, lib[: -len(".metallib")] + ".json")
    return json.load(open(path)) if os.path.exists(path) else None


def opts_from(meta):
    o = {}
    for p in meta.get("passes", []):
        if p["pass"] == "drop_outputs":
            o["drop_outputs"] = p["locations"]
        elif p["pass"] == "trim_outputs":
            o["trim_outputs"] = p["widths"]
        elif p["pass"] == "shrink_lds":
            o["shrink_lds"] = p["dwords"]
        elif p["pass"] == "dual_source":
            o["dual_source"] = True
        elif p["pass"] == "x_buf_device":
            o["x_buf_device"] = True
    return o


def regenerate(name, row, stage, tmp):
    meta = cas_meta(row, stage)
    if not meta:
        return None, "no CAS record"
    raw = open(os.path.join(P.SPIRV, name + ".spv"), "rb").read()
    words = list(P.spv.struct.unpack("<%dI" % (len(raw) // 4), raw))
    w, passes, extra = R.prepare(words, stage, opts_from(meta))
    os.makedirs(tmp, exist_ok=True)
    base = os.path.join(tmp, hashlib.sha256(name.encode()).hexdigest()[:16])
    open(base + ".spv", "wb").write(P.spv.to_bytes(w))
    rc, out, err, dt = R.run([R.SPIRV_CROSS, base + ".spv", *R.RECIPE["cross"], "--msl-version",
                              str(R.RECIPE["msl_version"]), *extra, "--output", base + ".metal"], R.RECIPE["cross_timeout"])
    if rc != 0:
        return None, "spirv-cross failed"
    text = open(base + ".metal", encoding="utf-8", errors="replace").read()
    for ext in (".spv", ".metal"):
        try:
            os.remove(base + ext)
        except OSError:
            pass
    if meta.get("msl_bytes") and len(text.encode()) != meta["msl_bytes"]:
        return None, "recipe mismatch: %d bytes of MSL, the store's record says %d" % (len(text.encode()), meta["msl_bytes"])
    fixed, _ = R.msl_fixups(text, passes)
    return (fixed if fixed is not None else text), None


PARAM = "device atomic_uint* gta6rt_dbg [[buffer(30)]], "
ADD = "atomic_fetch_add_explicit(&gta6rt_dbg[%d], %s, memory_order_relaxed);"


def simd_add(word):
    return ("{ uint gta6rt_n%d = simd_sum(1u); if (simd_is_first()) "
            "atomic_fetch_add_explicit(&gta6rt_dbg[%d], gta6rt_n%d, memory_order_relaxed); }" % (word, word, word))


def instrument_mesh(msl):
    head = "[[mesh]] void main0("
    i = msl.find(head)
    if i < 0:
        return None, "no mesh entry"
    msl = msl[: i + len(head)] + PARAM + msl[i + len(head):]
    body = msl[i:]
    anchor = "    threadgroup_barrier(mem_flags::mem_threadgroup);\n    if (spvMeshSizes.y == 0)\n"
    j = body.find(anchor)
    if j < 0:
        return None, "no output-count anchor"
    m = re.search(r"threadgroup spvUnsafeArray<\w+, (\d+)> gl_PrimitiveTriangleIndicesEXT;", body)
    if not m or "gl_Position[" not in body:
        return None, "no staged indices or positions"
    pmax = int(m.group(1))
    culled = "gl_MeshPrimitivesEXT[" in body and "gl_CullPrimitiveEXT" in body
    cull = (lambda k: "gl_MeshPrimitivesEXT[%s].gl_CullPrimitiveEXT" % k) if culled else (lambda k: "false")
    add = [
        "    {",
        "        uint gta6rt_n = simd_sum((gl_LocalInvocationIndex < min(spvMeshSizes.y, %du) && %s) ? 1u : 0u);" % (pmax, cull("gl_LocalInvocationIndex")),
        "        if (simd_is_first() && gta6rt_n != 0u) " + ADD % (132, "gta6rt_n"),
        "    }",
        "    if (gl_LocalInvocationIndex == 0u)",
        "    {",
        "        " + ADD % (128, "1u"),
        "        if (spvMeshSizes.y != 0u)",
        "        {",
        "            " + ADD % (129, "1u"),
        "            " + ADD % (130, "spvMeshSizes.y"),
        "            " + ADD % (131, "spvMeshSizes.x"),
        "            uint gta6rt_p = 0u;",
        "            while (gta6rt_p < min(spvMeshSizes.y, %du) && %s) gta6rt_p++;" % (pmax, cull("gta6rt_p")),
        "            if (gta6rt_p < min(spvMeshSizes.y, %du))" % pmax,
        "            {",
        "                uint gta6rt_s = atomic_fetch_add_explicit(&gta6rt_dbg[133], 1u, memory_order_relaxed);",
        "                if (gta6rt_s < 16u)",
        "                {",
        "                    device atomic_uint* gta6rt_o = gta6rt_dbg + 256u + gta6rt_s * 16u;",
        "                    atomic_store_explicit(&gta6rt_o[0], gl_WorkGroupID.x, memory_order_relaxed);",
        "                    atomic_store_explicit(&gta6rt_o[1], spvMeshSizes.x, memory_order_relaxed);",
        "                    atomic_store_explicit(&gta6rt_o[2], spvMeshSizes.y, memory_order_relaxed);",
        "                    atomic_store_explicit(&gta6rt_o[3], gta6rt_p, memory_order_relaxed);",
        "                    uint gta6rt_v[3] = { uint(gl_PrimitiveTriangleIndicesEXT[gta6rt_p].x), uint(gl_PrimitiveTriangleIndicesEXT[gta6rt_p].y), uint(gl_PrimitiveTriangleIndicesEXT[gta6rt_p].z) };",
        "                    for (uint gta6rt_k = 0u; gta6rt_k < 3u; ++gta6rt_k)",
        "                    {",
        "                        float4 gta6rt_q = gl_Position[min(gta6rt_v[gta6rt_k], 127u)];",
        "                        atomic_store_explicit(&gta6rt_o[4u + gta6rt_k * 4u + 0u], as_type<uint>(gta6rt_q.x), memory_order_relaxed);",
        "                        atomic_store_explicit(&gta6rt_o[4u + gta6rt_k * 4u + 1u], as_type<uint>(gta6rt_q.y), memory_order_relaxed);",
        "                        atomic_store_explicit(&gta6rt_o[4u + gta6rt_k * 4u + 2u], as_type<uint>(gta6rt_q.z), memory_order_relaxed);",
        "                        atomic_store_explicit(&gta6rt_o[4u + gta6rt_k * 4u + 3u], as_type<uint>(gta6rt_q.w), memory_order_relaxed);",
        "                    }",
        "                }",
        "            }",
        "        }",
        "    }",
    ]
    k = i + j + len("    threadgroup_barrier(mem_flags::mem_threadgroup);\n")
    return msl[:k] + "\n".join(add) + "\n" + msl[k:], None


def instrument_pixel(msl):
    m = re.search(r"^(?:\[\[[^\]]*\]\] )?fragment (\w+) main0\(", msl, re.M)
    if not m:
        return None, "no fragment entry"
    i = m.end()
    msl = msl[:i] + PARAM + msl[i:]
    open_brace = msl.find("\n{\n", i)
    if open_brace < 0:
        return None, "no entry body"
    body_start = open_brace + 3
    depth, k = 1, body_start
    while depth and k < len(msl):
        c = msl[k]
        depth += 1 if c == "{" else -1 if c == "}" else 0
        k += 1
    body = msl[body_start:k]
    body = body.replace("discard_fragment();", "{ %s discard_fragment(); }" % simd_add(161))
    body = re.sub(r"(\n\s*)return out;", lambda mm: "%s%s%sreturn out;" % (mm.group(1), simd_add(162), mm.group(1)), body)
    body = "    " + simd_add(160) + "\n" + body
    return msl[:body_start] + body + msl[k:], None


def build(out_dir, name, row, base_row):
    stage = row[2]
    tmp = os.path.join(out_dir, "tmp", str(os.getpid()))
    r = {"name": name, "stage": stage}
    if stage == "ms" and len(row) > 10 and row[10] != "hw":
        r["status"] = "route " + row[10]
        return r
    msl, why = regenerate(name, row, stage, tmp)
    if msl is None:
        r["status"] = "skipped"
        r["error"] = why
        return r
    packed = base_row is not None and "pack_threadgroup_vec3" in base_row[8]
    if packed:
        msl, _ = P.pack(msl, P.tg_bytes(msl)[1])
    msl, why = (instrument_mesh if stage == "ms" else instrument_pixel)(msl)
    if msl is None:
        r["status"] = "skipped"
        r["error"] = why
        return r
    key = hashlib.sha256((VERSION + "\n" + msl).encode()).hexdigest()
    d = os.path.join(out_dir, "cas", key[:2])
    os.makedirs(d, exist_ok=True)
    lib = os.path.join(d, key + ".metallib")
    if not os.path.exists(lib):
        os.makedirs(tmp, exist_ok=True)
        b = os.path.join(tmp, key[:16])
        open(b + ".metal", "w").write(msl)
        with gzip.open(os.path.join(d, key + ".metal.gz"), "wb", compresslevel=1) as g:
            g.write(msl.encode())
        rc, o, e, dt = R.run([R.tool("metal"), "-std=" + R.RECIPE["std"], *R.RECIPE["metal"], b + ".metal", "-o", b + ".air"],
                             R.RECIPE["metal_timeout"])
        if rc != 0:
            r["status"] = "metal_fail"
            r["error"] = (e or b"").decode("utf-8", "replace")[-600:]
            return r
        rc, o, e, dt = R.run([R.tool("metallib"), b + ".air", "-o", lib + ".part"], R.RECIPE["metal_timeout"])
        if rc != 0:
            r["status"] = "metallib_fail"
            return r
        os.replace(lib + ".part", lib)
        for ext in (".metal", ".air"):
            try:
                os.remove(b + ext)
            except OSError:
                pass
    r.update(status="ok", key=key, metallib=lib, packed=packed)
    return r


def overlay_rows(path):
    rows = {}
    if path:
        for line in open(path):
            if line.startswith("#"):
                continue
            f = line.rstrip("\n").split("\t")
            rows[f[0]] = f
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("patterns", nargs="+")
    ap.add_argument("--base")
    ap.add_argument("--jobs", type=int, default=3)
    a = ap.parse_args()
    out_dir = os.path.abspath(a.out)
    head, rows = P.manifest_rows()
    base = overlay_rows(a.base)
    pats = [re.compile(p) for p in a.patterns]
    names = [n for n, f in rows.items() if f[2] in ("ms", "ps") and any(p.search(n) for p in pats)]
    results = []
    with cf.ProcessPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(build, out_dir, n, rows[n], base.get(n)): n for n in names}
        for f in cf.as_completed(futs):
            try:
                r = f.result()
            except Exception as e:  # reported per shader, not fatal
                r = {"name": futs[f], "status": "error", "error": str(e)[:300]}
            results.append(r)
            if r["status"] != "ok":
                print(json.dumps(r), flush=True)
    by = {}
    for r in results:
        by[r["status"]] = by.get(r["status"], 0) + 1
    print("# " + json.dumps(by), file=sys.stderr)
    with open(os.path.join(out_dir, "instrument.jsonl"), "w") as f:
        for r in results:
            f.write(json.dumps(r) + "\n")
    rel_root = os.path.relpath(P.STORE_ROOT, out_dir)
    with open(os.path.join(out_dir, "manifest.tsv.part"), "w") as f:
        for h in head:
            f.write(re.sub(r"root=\S+", "root=" + rel_root, h) if "root=" in h else h)
        for r in sorted(results, key=lambda r: r["name"]):
            if r["status"] != "ok":
                continue
            row = list(base.get(r["name"]) or rows[r["name"]])
            row[1] = r["key"]
            row[4] = os.path.relpath(r["metallib"], P.STORE_ROOT)
            row[8] = (row[8] + "," if row[8] not in ("", "-") else "") + "gta6rt_debug_counters"
            f.write("\t".join(row) + "\n")
    os.replace(os.path.join(out_dir, "manifest.tsv.part"), os.path.join(out_dir, "manifest.tsv"))
    print("# overlay: %d rows -> %s" % (by.get("ok", 0), os.path.join(out_dir, "manifest.tsv")), file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
