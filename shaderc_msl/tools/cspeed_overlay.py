#!/usr/bin/env python3
"""Library compute kernels rebuilt with the lossless MSL passes (tools/mspeed_passes.py: merge_typed_reads from
tools/tmem.py, chain_segments, round_counts_simd, barrier_tg_only, simd_votes, fp_inline, hash_memo), each from its
store recipe (the store row's passes are kept).

  cspeed_overlay.py build OUT TAG --passes a,b,... NAME...   libraries in OUT/cas, results in OUT/var/TAG.jsonl
  cspeed_overlay.py rows OUT PICK                             OUT/overlay.tsv from PICK (lines "NAME TAG")
A pass that does not apply to a kernel (no PC switch, no hash function, ...) is skipped and noted; a kernel that no
pass changes gets status "unchanged".
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
import msl_instrument as MI  # noqa: E402
import mspeed_passes as MP  # noqa: E402
import shaderc_msl as R  # noqa: E402

VERSION = "cspeed 1"


def build(out_dir, name, row, passes):
    r = {"name": name}
    tmp = os.path.join(out_dir, "tmp", str(os.getpid()))
    msl, err = MI.regenerate(name, row, "cs", tmp)
    if msl is None:
        r.update(status="cross_fail", error=err)
        return r
    os.makedirs(tmp, exist_ok=True)
    base = os.path.join(tmp, hashlib.sha256(name.encode()).hexdigest()[:16])
    meta = MI.cas_meta(row, "cs")
    names = [p["pass"] for p in meta["passes"]]
    out, info, applied = msl, {}, []
    for p in passes:
        try:
            new, i = MP.PASSES[p](out)
        except (ValueError, AttributeError, KeyError, IndexError) as e:
            info[p] = {"why": "not applicable: %s" % str(e)[:120]}
            continue
        info[p] = i
        if new != out:
            out = new
            applied.append(p)
    r["info"] = info
    if out == msl:
        r["status"] = "unchanged"
        return r
    names += applied
    key = hashlib.sha256((VERSION + "\n" + ",".join(applied) + "\n" + out).encode()).hexdigest()
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
    r.update(status="ok", key=key, metallib=lib, passes=names, applied=applied)
    return r


def cmd_build(a):
    out_dir = os.path.abspath(a.out)
    passes = [p for p in a.passes.split(",") if p]
    head, rows = P.manifest_rows()
    results, todo = [], []
    for n in dict.fromkeys(a.names):
        row = rows.get(n)
        if row is None or row[2] != "cs":
            results.append({"name": n, "status": "not a store compute row"})
        else:
            todo.append(n)
    with cf.ProcessPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(build, out_dir, n, rows[n], passes): n for n in todo}
        for f in cf.as_completed(futs):
            try:
                r = f.result()
            except Exception as e:
                r = {"name": futs[f], "status": "error", "error": str(e)[:300]}
            r["tag"] = a.tag
            results.append(r)
            print(json.dumps({k: v for k, v in r.items() if k != "metallib"}), flush=True)
    os.makedirs(os.path.join(out_dir, "var"), exist_ok=True)
    log = os.path.join(out_dir, "var", a.tag + ".jsonl")
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
    return 1 if any(r["status"] in ("error", "metal_fail", "metallib_fail") for r in results) else 0


def cmd_rows(a):
    out_dir = os.path.abspath(a.out)
    head, rows = P.manifest_rows()
    picked = []
    for line in open(a.pick):
        line = line.split("#")[0].strip()
        if not line:
            continue
        name, tag = line.split()
        res = {}
        for l in open(os.path.join(out_dir, "var", tag + ".jsonl")):
            x = json.loads(l)
            res[x["name"]] = x
        r = res.get(name)
        if not r or r["status"] != "ok":
            raise SystemExit("%s: no ok build in variant %s" % (name, tag))
        f = list(rows[name])
        f[1] = r["key"]
        f[4] = os.path.relpath(r["metallib"], P.STORE_ROOT)
        f[8] = ",".join(r["passes"])
        picked.append(f)
    rel_root = os.path.relpath(P.STORE_ROOT, out_dir)
    with open(os.path.join(out_dir, "overlay.tsv.part"), "w") as f:
        for h in head:
            f.write(re.sub(r"root=\S+", "root=" + rel_root, h) if "root=" in h else h)
        for row in picked:
            f.write("\t".join(row) + "\n")
    os.replace(os.path.join(out_dir, "overlay.tsv.part"), os.path.join(out_dir, "overlay.tsv"))
    print("overlay %d rows -> %s" % (len(picked), os.path.join(out_dir, "overlay.tsv")))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("out")
    b.add_argument("tag")
    b.add_argument("names", nargs="+")
    b.add_argument("--passes", required=True)
    b.add_argument("--jobs", type=int, default=2)
    r = sub.add_parser("rows")
    r.add_argument("out")
    r.add_argument("pick")
    a = ap.parse_args()
    return cmd_build(a) if a.cmd == "build" else cmd_rows(a)


if __name__ == "__main__":
    sys.exit(main())
