#!/usr/bin/env python3
"""Hw-route mesh shaders over Metal's 32 KiB of threadgroup memory: SPIRV-Cross stages every mesh output (per vertex
and per primitive) in a 128-entry threadgroup array that main0 copies into the mesh object after the barrier. In the
translated title shaders each staged output is written only at the writing thread's own index and read back by the
same thread (X[gl_LocalInvocationIndex] in the body, X[spvVI] / X[spvPI] in the copy), so no thread reads another's
element. This pass makes each such array one thread variable; only the shared state (mesh sizes, the LDS emulation)
stays in threadgroup memory. A shader with any other use of a staged array is left alone and reported.

  mesh_out_private.py OUT NAME...       these shaders (store rows), rebuilt with the store's recipe plus this pass
  mesh_out_private.py OUT --scan        every hw-route mesh shader of the store over the limits
  options: --list FILE (names, one a line), --jobs N, --force (also shaders that already fit)

OUT/cas holds the libraries (and .metal.gz), OUT/overlay.tsv the rows (paths relative to the store root) for
GTA6RT_METAL_SHADER_OVERLAY, OUT/mesh_out_private.jsonl one result per shader.
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
PORT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(PORT, "d3d12_metal", "tools"))
import mesh_tg_pack as P  # noqa: E402
import shaderc_msl as R  # noqa: E402

VERSION = "mesh_out_private 1"
PASS = "private_mesh_outputs"
OWN_INDEX = ("gl_LocalInvocationIndex", "spvVI", "spvPI")


def staged_arrays(msl):
    _, body = P.entry_body(msl)
    copy = body[body.find("spvMesh.set_primitive_count"):] if "spvMesh.set_primitive_count" in body else ""
    out = []
    for m in P.DECL.finditer(body):
        t, n, name = m.group(1), m.group(2), m.group(4)
        if not t or not n:
            continue
        if re.search(r"\b" + re.escape(name) + r"\[(?:spvVI|spvPI)\]", copy):
            out.append((name, t, int(n)))
    return out


def privatize(msl):
    """(new MSL, arrays made private, reason when nothing was done)."""
    if re.search(r"\bgl_LocalInvocationIndex\s*(?:[-+*/|&^]?=(?!=)|\+\+|--)", msl):
        return None, [], "gl_LocalInvocationIndex is written"
    for v in ("spvVI", "spvPI"):
        if msl.count("const uint %s = gl_LocalInvocationIndex;" % v) != 1:
            return None, [], v + " is not the thread index"
    arrays = staged_arrays(msl)
    if not arrays:
        return None, [], "no staged outputs"
    names = {n for n, _, _ in arrays}
    for m in re.finditer(r"threadgroup spvUnsafeArray<\w+, \d+>& (\w+)", msl):
        if m.group(1) not in names and re.search(r"\b" + re.escape(m.group(1)) + r"\[", msl):
            for n, t, k in arrays:
                if m.group(0).startswith("threadgroup spvUnsafeArray<%s, %d>&" % (t, k)):
                    return None, [], "array parameter %s may alias a staged output" % m.group(1)
    out = msl
    for name, t, k in arrays:
        e = re.escape(name)
        decl = "threadgroup spvUnsafeArray<%s, %d> %s;" % (t, k, name)
        param = "threadgroup spvUnsafeArray<%s, %d>& %s" % (t, k, name)
        uses = re.findall(r"(?<![.\w])" + e + r"\b", out)
        n_decl = out.count(decl)
        n_param = len(re.findall(re.escape(param) + r"(?=[,)])", out))
        n_sub = len(re.findall(r"(?<![.\w])" + e + r"\[(?:%s)\]" % "|".join(OWN_INDEX), out))
        n_arg = len(re.findall(r"(?<=[(,] )" + e + r"(?=[,)])|(?<=\()" + e + r"(?=[,)])", out))
        member = len(re.findall(r"^\s*\w+ " + e + r"(?: \[\[[^\]]*\]\])?;", out, re.M))
        if n_decl != 1 or n_decl + n_param + n_sub + n_arg + member != len(uses):
            return None, [], "%s: %d uses, %d explained" % (name, len(uses), n_decl + n_param + n_sub + n_arg + member)
        out = out.replace(decl, "%s %s = {};" % (t, name))
        out = re.sub(re.escape(param) + r"(?=[,)])", "thread %s& %s" % (t, name), out)
        out = re.sub(r"(?<![.\w])" + e + r"\[(?:%s)\]" % "|".join(OWN_INDEX), name, out)
    return out, [n for n, _, _ in arrays], None


def build(out_dir, name, row, force):
    tmp = os.path.join(out_dir, "tmp", str(os.getpid()))
    msl, passes, base = P.cross(name, row, tmp)
    r = {"name": name}
    if msl is None:
        r["status"] = "cross_fail"
        return r
    before, _ = P.tg_bytes(msl)
    mesh = P.mesh_bytes(msl)
    r.update(before=before, mesh_output=mesh)
    if before is None:
        r["status"] = "unsized"
        return r
    if not P.over(before, mesh) and not force:
        r["status"] = "fits"
        return r
    new, arrays, why = privatize(msl)
    if new is None:
        r.update(status="not_private", error=why)
        return r
    after, _ = P.tg_bytes(new)
    r.update(after=after, private_arrays=len(arrays))
    if after is None or P.over(after, mesh):
        r["status"] = "still_over"
        return r
    key = hashlib.sha256((VERSION + "\n" + new).encode()).hexdigest()
    d = os.path.join(out_dir, "cas", key[:2])
    os.makedirs(d, exist_ok=True)
    lib = os.path.join(d, key + ".metallib")
    if not os.path.exists(lib):
        open(base + ".metal", "w").write(new)
        with gzip.open(os.path.join(d, key + ".metal.gz"), "wb", compresslevel=1) as g:
            g.write(new.encode())
        air = base + ".air"
        rc, o, e, dt = R.run([R.tool("metal"), "-std=" + R.RECIPE["std"], *R.RECIPE["metal"], base + ".metal", "-o", air],
                             R.RECIPE["metal_timeout"])
        if rc != 0:
            r.update(status="metal_fail", error=(e or b"").decode("utf-8", "replace")[-600:])
            return r
        part = "%s.%d.part" % (lib, os.getpid())
        rc, o, e, dt = R.run([R.tool("metallib"), air, "-o", part], R.RECIPE["metal_timeout"])
        if rc != 0:
            r["status"] = "metallib_fail"
            return r
        os.replace(part, lib)
        for ext in (".metal", ".air"):
            try:
                os.remove(base + ext)
            except OSError:
                pass
    r.update(status="private", key=key, metallib=lib, passes=[p["pass"] for p in passes] + [PASS])
    return r


def overlay_row(row, r):
    f = list(row) + ["-"] * max(0, 28 - len(row))
    f[1] = r["key"]
    f[4] = os.path.relpath(r["metallib"], P.STORE_ROOT)
    f[8] = (f[8] + "," if f[8] not in ("", "-") else "") + PASS
    f[10] = "hw"
    for c in range(11, 18):
        f[c] = "-"
    return f[: len(row)] if len(row) >= 18 else f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("names", nargs="*")
    ap.add_argument("--scan", action="store_true")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--list", help="a file of names, one a line")
    a = ap.parse_args()
    if a.list:
        a.names += [l.rstrip("\n") for l in open(a.list) if l.strip()]
    if not a.scan and not a.names:
        ap.error("names, --list or --scan")
    out_dir = os.path.abspath(a.out)
    head, rows = P.manifest_rows()
    names = a.names or [n for n, f in rows.items() if f[2] == "ms" and len(f) > 10 and f[10] in ("hw", "-")]
    missing = [n for n in names if n not in rows]
    for n in missing:
        print(json.dumps({"name": n, "status": "not_in_store"}), flush=True)
    results = []
    with cf.ProcessPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(build, out_dir, n, rows[n], a.force): n for n in names if n in rows}
        for f in cf.as_completed(futs):
            try:
                r = f.result()
            except Exception as e:
                r = {"name": futs[f], "status": "error", "error": str(e)[:300]}
            results.append(r)
            if r["status"] != "fits":
                print(json.dumps({k: v for k, v in r.items() if k not in ("metallib", "passes")}), flush=True)
    log = os.path.join(out_dir, "mesh_out_private.jsonl")
    old = {}
    if os.path.exists(log):
        for line in open(log):
            x = json.loads(line)
            old[x["name"]] = x
    for r in results:
        old[r["name"]] = r
    with open(log + ".part", "w") as f:
        for r in old.values():
            f.write(json.dumps(r) + "\n")
    os.replace(log + ".part", log)
    rel_root = os.path.relpath(P.STORE_ROOT, out_dir)
    done = sorted((n for n, r in old.items() if r["status"] == "private" and n in rows))
    with open(os.path.join(out_dir, "overlay.tsv.part"), "w") as f:
        for h in head:
            f.write(re.sub(r"root=\S+", "root=" + rel_root, h) if "root=" in h else h)
        for n in done:
            f.write("\t".join(overlay_row(rows[n], old[n])) + "\n")
    os.replace(os.path.join(out_dir, "overlay.tsv.part"), os.path.join(out_dir, "overlay.tsv"))
    by = {}
    for r in results:
        by[r["status"]] = by.get(r["status"], 0) + 1
    print("# %s; overlay %d rows -> %s" % (json.dumps(by), len(done), os.path.join(out_dir, "overlay.tsv")), file=sys.stderr)
    return 1 if any(r["status"] in ("error", "metal_fail", "metallib_fail", "cross_fail") for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
