#!/usr/bin/env python3
"""Overlay of mesh shaders rebuilt with the mesh-speed passes (tools/mspeed_passes.py): each from its store recipe,
private mesh outputs when they apply (tools/mesh_out_private.py), then the passes; route hw rows only.

  mspeed_overlay.py OUT [--names FILE] [--passes a,b,...] [--jobs N] [NAME...]
OUT/cas holds the libraries (+ .metal.gz), OUT/overlay.tsv the rows (paths relative to the store root),
OUT/mspeed.jsonl one result per shader. A shader the passes cannot change keeps no row.
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
import mesh_out_private as MOP  # noqa: E402
import mspeed_passes as MP  # noqa: E402
import shaderc_msl as R  # noqa: E402

VERSION = "mspeed 1"
DEFAULT_PASSES = ["chain_segments", "round_counts_simd", "barrier_tg_only", "simd_votes", "fp_inline", "hash_memo"]


def build(out_dir, name, row, passes):
    r = {"name": name}
    tmp = os.path.join(out_dir, "tmp", str(os.getpid()))
    if row[2] == "ps":
        import msl_instrument as MI
        meta = MI.cas_meta(row, "ps")
        if not meta or any(p["pass"] in ("output_variants", "dual_source") for p in meta.get("passes", [])) or meta.get("output_variants"):
            r["status"] = "ps_variants"
            return r
        msl, err = MI.regenerate(name, row, "ps", tmp)
        if msl is None:
            r.update(status="cross_fail", error=err)
            return r
        os.makedirs(tmp, exist_ok=True)
        base = os.path.join(tmp, hashlib.sha256(name.encode()).hexdigest()[:16])
        names = [p["pass"] for p in meta["passes"]]
    else:
        msl, rpasses, base = P.cross(name, row, tmp)
        if msl is None:
            r["status"] = "cross_fail"
            return r
        names = [p["pass"] for p in rpasses]
        if len(row) > 10 and row[10] == "split64":
            tg, arrays = P.tg_bytes(msl)
            if tg is not None and P.over(tg, P.mesh_bytes(msl)):
                msl, npk = P.pack(msl, arrays)
                names.append("pack_threadgroup_vec3")
            r["split"] = True
        else:
            new, arrays, why = MOP.privatize(msl)
            if new is not None:
                msl = new
                names.append(MOP.PASS)
    try:
        out, info = MP.apply(msl, passes)
    except ValueError as e:
        r.update(status="not_applied", error=str(e)[:200])
        return r
    r["info"] = info
    if out == msl:
        r["status"] = "unchanged"
        return r
    names += [p for p in passes if p in info and any(v for k, v in info[p].items() if k != "why" and isinstance(v, int))]
    key = hashlib.sha256((VERSION + "\n" + ",".join(passes) + "\n" + out).encode()).hexdigest()
    d = os.path.join(out_dir, "cas", key[:2])
    os.makedirs(d, exist_ok=True)
    lib = os.path.join(d, key + ".metallib")
    if not os.path.exists(lib):
        open(base + ".metal", "w").write(out)
        with gzip.open(os.path.join(d, key + ".metal.gz"), "wb", compresslevel=1) as g:
            g.write(out.encode())
        air = base + ".air"
        rc, o, e, dt = R.run([R.tool("metal"), "-std=" + R.RECIPE["std"], *R.RECIPE["metal"], base + ".metal", "-o", air],
                             R.RECIPE["metal_timeout"])
        r["metal_s"] = round(dt, 1)
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
    tg = P.tg_bytes(out)[0] if row[2] == "ms" else None
    r.update(status="ok", key=key, metallib=lib, passes=names, tg_bytes=tg)
    return r


def overlay_row(row, r):
    f = list(row) + ["-"] * max(0, 28 - len(row))
    f[1] = r["key"]
    f[4] = os.path.relpath(r["metallib"], P.STORE_ROOT)
    f[8] = ",".join(r["passes"])
    if row[2] == "ms" and r.get("split"):
        f[11] = os.path.relpath(r["metallib"], P.STORE_ROOT)
        f[4] = row[4]
    elif row[2] == "ms":
        f[10] = "hw"
        for c in range(11, 18):
            f[c] = "-"
    return f[: len(row)] if len(row) >= 18 else f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("names", nargs="*")
    ap.add_argument("--names-file")
    ap.add_argument("--passes", default=",".join(DEFAULT_PASSES))
    ap.add_argument("--jobs", type=int, default=2)
    a = ap.parse_args()
    if a.names_file:
        a.names += [l.strip() for l in open(a.names_file) if l.strip()]
    passes = [p for p in a.passes.split(",") if p]
    out_dir = os.path.abspath(a.out)
    head, rows = P.manifest_rows()
    results = []
    todo = []
    for n in dict.fromkeys(a.names):
        row = rows.get(n)
        if row is None:
            results.append({"name": n, "status": "not_in_store"})
        elif row[2] not in ("ms", "ps") or (row[2] == "ms" and len(row) > 10 and row[10] not in ("hw", "-", "split64")):
            results.append({"name": n, "status": "route " + (row[10] if len(row) > 10 else row[2])})
        else:
            todo.append(n)
    with cf.ProcessPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(build, out_dir, n, rows[n], passes): n for n in todo}
        for f in cf.as_completed(futs):
            try:
                r = f.result()
            except Exception as e:
                r = {"name": futs[f], "status": "error", "error": str(e)[:300]}
            results.append(r)
            print(json.dumps({k: v for k, v in r.items() if k not in ("metallib",)}), flush=True)
    log = os.path.join(out_dir, "mspeed.jsonl")
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
    done = sorted(n for n, r in old.items() if r["status"] == "ok" and n in rows)
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
    return 1 if any(r["status"] in ("error", "metal_fail", "metallib_fail") for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
