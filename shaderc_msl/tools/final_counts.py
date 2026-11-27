#!/usr/bin/env python3
"""Exact per-stage counts for STATUS.md from the run's results, the manifest, the sweeps and the mesh routes.

  final_counts.py --build port/build/shaderc_msl --spirv-dir spike/s2-make-game/work/shaders/spirv [--json out.json]
"""
import argparse, collections, glob, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import batch  # noqa: E402


def jl(path):
    out = []
    if os.path.exists(path):
        for l in open(path):
            try:
                out.append(json.loads(l))
            except ValueError:
                pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--spirv-dir", required=True)
    ap.add_argument("--json", default="")
    a = ap.parse_args()
    index = batch.load_index(a.spirv_dir, "library,exe,fspx,bvh")
    rows = {}
    for r in jl(os.path.join(a.build, "runs", "main", "results.jsonl")):
        rows[r["stem"]] = r
    grp = lambda k: (index[k].get("corpus", "main"), index[k]["stage"])
    tot = collections.Counter(grp(k) for k in index)
    ok = collections.Counter()
    status = collections.defaultdict(collections.Counter)
    missing = collections.Counter()
    for k in index:
        r = rows.get(k)
        g = grp(k)
        if r is None:
            missing[g] += 1
        else:
            status[g][r["status"]] += 1
            ok[g] += r["status"] == "ok"
    res = {"stage_rows": {}}
    for g in sorted(tot):
        res["stage_rows"]["%s:%s" % g] = {"total": tot[g], "ok": ok[g], "not_run": missing[g],
                                          "failed": {s: n for s, n in status[g].items() if s != "ok"}}
    res["total"] = {"shaders": sum(tot.values()), "ok": sum(ok.values()), "not_run": sum(missing.values()),
                    "failed": sum(n for g in status for s, n in status[g].items() if s != "ok")}
    cs = {}
    for f in ("cs_sweep.jsonl", "cs_sweep_sample.jsonl", "cs_sweep_huge.jsonl"):
        for r in jl(os.path.join(a.build, f)):
            cs[r["stem"]] = r
    res["compute_pipeline_sweep"] = {"distinct_stems": len(cs), "results": dict(collections.Counter(r["result"] for r in cs.values())),
                                     "max_wall_s": max((r["wall_s"] for r in cs.values()), default=0)}
    hv = {}
    for f in glob.glob(os.path.join(a.build, "hw_verify*.jsonl")):
        for r in jl(f):
            hv[r["stem"]] = r
    res["hw_verify_mesh_pipeline"] = dict(collections.Counter(r["result"] for r in hv.values()))
    mr_path = os.path.join(a.build, "mesh", "mesh_route.json")
    if os.path.exists(mr_path):
        mr = json.load(open(mr_path))
        res["mesh_routes"] = dict(collections.Counter(v["route"] for v in mr.values()))
        res["mesh_routes_by_step"] = {str(s): dict(collections.Counter(v["route"] for v in mr.values() if v["step"] == s))
                                      for s in range(5)}
        res["split_chunks"] = dict(collections.Counter(v["chunks"] for v in mr.values() if v["route"] == "split64"))
        res["split_vmax_hist"] = dict(sorted(collections.Counter(v["vmax"] for v in mr.values() if v["route"] == "split64").items()))
    cas = batch.cas_bytes(os.path.join(a.build, "cas"))
    res["cas"] = cas
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
