#!/usr/bin/env python3
"""Every library a world run loads (the tier's shaders resolved through the default overlay list, as
d3d12_metal/tools/rehash_world.py plans them), rebuilt with wired2_passes (default: hash_noinline), as one overlay
placed first, plus the prebuild manifest + catalog for prepare_shaders.

  wired2_rebuild.py plan WORK [--stages ps,cs] [--top N] [--limit N]   (--top: the N largest libraries)
  wired2_rebuild.py build WORK [--jobs J] [--passes hash_noinline]
  wired2_rebuild.py overlay WORK
  wired2_rebuild.py manifest WORK

Sources: a library's kept MSL (<key>.metal.gz / <key>.msl), else the corpus SPIR-V rebuilt with the options its result
row records (the row's key is recomputed first; a mismatch fails the job loudly), with the passes applied after the
recipe's own MSL fix-ups (pixel output-type variants included). A library the passes do not change keeps its row.
"""
import argparse
import gzip
import hashlib
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(PORT, "d3d12_metal", "tools"))
import rehash_world as RW  # noqa: E402
import wired2_passes as W  # noqa: E402

R = RW.R
_orig_fixups = R.msl_fixups
PASSES = [p for p in os.environ.get("WIRED2_PASSES", "hash_noinline").split(",") if p]


def fixups(text, passes):
    t, info = _orig_fixups(text, passes)
    cur = t if t is not None else text
    changed = t is not None
    for p in PASSES:
        new, i = W.PASSES[p](cur)
        info.setdefault("wired2", {})[p] = i
        if new != cur:
            cur, changed = new, True
    return (cur if changed else None), info


R.msl_fixups = fixups


def applied(info):
    return sum(int(bool(v.get(k))) for v in (info or {}).get("wired2", {}).values() for k in v if k in PASSES)


def one(job, work):
    lib = job["lib"]
    key = hashlib.sha256(lib.encode()).hexdigest()[:24]
    done = os.path.join(work, "done", key + ".json")
    if os.path.exists(done):
        return json.load(open(done))
    t0 = time.monotonic()
    r = {"lib": lib, "names": job["names"], "source": RW.source_kind(lib)}
    tmp = os.path.join(work, "tmp", str(os.getpid()))
    os.makedirs(tmp, exist_ok=True)
    try:
        stem = lib[:-9]
        if r["source"] in ("msl", "metal.gz"):
            text = open(stem + ".msl").read() if r["source"] == "msl" else gzip.open(stem + ".metal.gz", "rt").read()
            new, info = text, {}
            for p in PASSES:
                new, i = W.PASSES[p](new)
                info[p] = i
            r["info"] = info
            r["sites"] = int(new != text)
            if new != text:
                h = hashlib.sha256(new.encode()).hexdigest()
                out = os.path.join(work, "cas", h[:2], h + ".metallib")
                os.makedirs(os.path.dirname(out), exist_ok=True)
                if not os.path.exists(out):
                    r["compile_s"] = round(RW.compile_msl(new, out, tmp), 2)
                with gzip.open(out[:-9] + ".metal.gz", "wt", compresslevel=1) as g:
                    g.write(new)
                if r["source"] == "msl":
                    open(out[:-9] + ".msl", "w").write(new)
                r["new_lib"] = out
        elif r["source"] == "spirv":
            meta = json.load(open(stem + ".json"))
            spv_path = RW.spv_of(meta, job["names"])
            if not os.path.exists(spv_path):
                raise RuntimeError("no corpus SPIR-V " + spv_path)
            opts = RW.opts_of(meta)
            if "output_variants" in opts:
                import out_types
                opts["output_signature"] = out_types.dxbc_targets(R.dxbc_of(spv_path))
            raw = open(spv_path, "rb").read()
            words = list(R.spv.struct.unpack("<%dI" % (len(raw) // 4), raw))
            if RW.FNV_WORD not in words:
                r.update(status="ok", sites=0, wall_s=round(time.monotonic() - t0, 2))
                os.makedirs(os.path.dirname(done), exist_ok=True)
                json.dump(r, open(done + ".tmp", "w"))
                os.replace(done + ".tmp", done)
                return r
            w, passes, extra = R.prepare(words, meta["stage"], opts)
            translator = None
            for d in (RW.SPIRV, os.path.dirname(spv_path)):
                if R.cache_key(raw, R.translator_id(d), passes, extra) == meta["key"]:
                    translator = R.translator_id(d)
                    break
            if translator is None:
                raise RuntimeError("the recorded options do not give the row's key back")
            cas = os.path.join(work, "cas")
            nr = R.compile_one(spv_path, meta["stage"], cas, translator, opts, tmp=tmp, keep_msl=True)
            if nr.get("status") != "ok":
                raise RuntimeError("compile_one: %s %s" % (nr.get("status"), nr.get("error", "")[:300]))
            r["info"] = nr.get("wired2")
            r["sites"] = applied(nr)
            if nr.get("cached"):
                r["cached"] = True
            if r["sites"]:
                r["new_lib"] = os.path.join(cas, nr["key"][:2], nr["key"] + ".metallib")
                r["compile_s"] = round(nr.get("total_s", 0), 2)
        else:
            raise RuntimeError("no MSL and no result row beside the metallib")
        r["status"] = "ok"
    except Exception as e:  # noqa: BLE001
        r["status"] = "fail"
        r["error"] = str(e)[:800]
    r["wall_s"] = round(time.monotonic() - t0, 2)
    os.makedirs(os.path.dirname(done), exist_ok=True)
    if r["status"] == "ok":
        json.dump(r, open(done + ".tmp", "w"))
        os.replace(done + ".tmp", done)
    return r


RW.one = one


def plan(work, stages, limit, top):
    RW.plan(work, 0, 0)
    p = json.load(open(os.path.join(work, "plan.json")))
    if stages:
        keep = set(stages.split(","))
        p["jobs"] = [j for j in p["jobs"] if j["stage"] in keep]
    if top:
        p["jobs"] = sorted(p["jobs"], key=lambda j: -os.path.getsize(j["lib"]))[:top]
    if limit:
        p["jobs"] = p["jobs"][:limit]
    names = {n for j in p["jobs"] for n in j["names"]}
    p["rows"] = {n: w for n, w in p["rows"].items() if n in names}
    json.dump(p, open(os.path.join(work, "plan.json"), "w"))
    print("jobs %d (stages %s, largest %s), %.0f MiB of libraries" %
          (len(p["jobs"]), stages or "all", top or "all", sum(os.path.getsize(j["lib"]) for j in p["jobs"]) / 1048576))


def overlay(work):
    """rehash_world.overlay with this tool's key prefix (a row's key must not equal another rebuild's)."""
    p = json.load(open(os.path.join(work, "plan.json")))
    done = {}
    for f in os.listdir(os.path.join(work, "done")):
        r = json.load(open(os.path.join(work, "done", f)))
        done[r["lib"]] = r
    store = RW.manifest_root(RW.MAIN)
    head = "# shaderc_msl manifest v3 root=%s\n" % os.path.relpath(store, os.path.abspath(work))
    with open(RW.MAIN) as f:
        f.readline()
        cols = f.readline()
    lines, not_done = [], []
    for name, w in p["rows"].items():
        f = list(w["row"])
        changed = False
        for c in RW.LIB_COLS:
            if c < len(f) and f[c] not in ("-", ""):
                lib = os.path.normpath(os.path.join(w["root"], f[c]))
                r = done.get(lib)
                if r is None:
                    not_done.append(lib)
                    continue
                if r.get("new_lib"):
                    f[c] = os.path.relpath(os.path.abspath(r["new_lib"]), store)
                    changed = True
                else:
                    f[c] = os.path.relpath(lib, store)
        if changed:
            f[1] = hashlib.sha256(("wired2 " + ",".join(PASSES) + ":" + f[1]).encode()).hexdigest()
            lines.append("\t".join(f) + "\n")
    with open(os.path.join(work, "overlay.tsv"), "w") as o:
        o.write(head)
        o.write(cols)
        o.writelines(sorted(lines))
    rep = {"rows_changed": len(lines), "rows": len(p["rows"]), "libraries_done": len(done),
           "libraries_not_done": len(set(not_done)), "libraries_changed": sum(1 for r in done.values() if r.get("new_lib"))}
    json.dump(rep, open(os.path.join(work, "report.json"), "w"), indent=1)
    print(json.dumps(rep))
    return rep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["plan", "build", "overlay", "manifest"])
    ap.add_argument("work")
    ap.add_argument("--stages", default="")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--top", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=3)
    a = ap.parse_args()
    if a.cmd == "plan":
        plan(a.work, a.stages, a.limit, a.top)
    elif a.cmd == "build":
        res = RW.build(a.work, a.jobs)
        return 1 if any(r["status"] != "ok" for r in res) else 0
    elif a.cmd == "overlay":
        rep = overlay(a.work)
        return 1 if rep["libraries_not_done"] else 0
    elif a.cmd == "manifest":
        RW.manifest(a.work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
