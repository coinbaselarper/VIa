#!/usr/bin/env python3
"""Compute shaders at Metal limits: compile with the batch recipe and options (shared content-addressed store, so
the batch reuses these builds) and build a compute pipeline, one at a time.

Sets:
  lds    every cs with index.json lds_bytes > 32,768 (spike/s5.2: 28 fail at PSO; 3 at exactly 32,768 pass)
  fp64   every cs whose SPIR-V declares the Float64 capability
  huge   the 7 cs that built no pipeline within 300 s in spike/s5.2 (tools/huge_cs.py HUGE)
Writes <build>/cs_check/cs_check.jsonl (appends; --skip-done skips stems already there) and prints a summary.
"""
import argparse, collections, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import batch as B  # noqa: E402
import huge_cs  # noqa: E402
import probe as P  # noqa: E402
import shaderc_msl as S  # noqa: E402
import spv  # noqa: E402


def sets(index, spirv_dir):
    lds = sorted(k for k, v in index.items() if v["stage"] == "cs" and v.get("lds_bytes", 0) > 32768)
    fp64 = sorted(k for k, v in index.items() if v["stage"] == "cs" and "Float64" in v.get("capabilities", []))
    return {"lds": lds, "fp64": fp64, "huge": list(huge_cs.HUGE)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spirv-dir", required=True)
    ap.add_argument("--build", required=True)
    ap.add_argument("--sets", default="lds,fp64,huge")
    ap.add_argument("--pso-timeout", type=float, default=1200)
    ap.add_argument("--skip-done", action="store_true")
    ap.add_argument("--only", default="", help="comma-separated substrings of stems to run")
    a = ap.parse_args()
    index = json.load(open(os.path.join(a.spirv_dir, "index.json")))
    out = os.path.join(a.build, "cs_check")
    os.makedirs(out, exist_ok=True)
    res_p = os.path.join(out, "cs_check.jsonl")
    done = set()
    if a.skip_done and os.path.exists(res_p):
        done = {json.loads(l)["stem"] for l in open(res_p)}
    dxbc_dir = os.path.join(os.path.dirname(os.path.abspath(a.spirv_dir)), "dxbc")
    tr = S.translator_id(a.spirv_dir)
    cas = os.path.join(a.build, "cas")
    groups = sets(index, a.spirv_dir)
    print({k: len(v) for k, v in groups.items()}, flush=True)
    pr = P.Probe()
    with open(res_p, "a") as fo:
        for g in a.sets.split(","):
            for k in groups[g]:
                if k in done or (a.only and not any(x in k for x in a.only.split(","))):
                    continue
                o = B.shader_opts(index, k, {}, dxbc_dir)
                r = S.compile_one(os.path.join(a.spirv_dir, k + ".spv"), "cs", cas, tr, o, tmp=os.path.join(out, "tmp"))
                row = {"set": g, "stem": k, "lds_bytes": index[k].get("lds_bytes"), "opts": {x: y for x, y in o.items() if x != "vs_route"},
                       "status": r["status"], "error": r.get("error", "")[:300], "key": r["key"],
                       "passes": [p["pass"] for p in r.get("passes", [])], "spv_bytes": r.get("spv_bytes"),
                       "msl_bytes": r.get("msl_bytes"), "cross_s": r.get("cross_s"), "metal_s": r.get("metal_s"),
                       "metal_o0_s": r.get("metal_o0_s"), "opt": r.get("opt"), "metal_rss_mb": r.get("metal_rss_mb"),
                       "cached": bool(r.get("cached"))}
                if r["status"] == "ok":
                    lib = os.path.join(cas, r["key"][:2], r["key"] + ".metallib")
                    t0 = time.monotonic()
                    j = pr.run({"mode": "cs", "lib": lib, "fn": "main0", "reflect": True}, timeout=a.pso_timeout)
                    row.update(pso=j.get("result"), pso_error=j.get("error", "")[:300], pso_wall_s=round(time.monotonic() - t0, 1),
                               simd_width=j.get("threadExecutionWidth"), max_threads=j.get("maxThreads"), static_threadgroup_bytes=j.get("static_tg_mem"))
                    if j.get("result") in ("TIMEOUT", "CRASH"):
                        pr.close()
                        pr = P.Probe()
                fo.write(json.dumps(row) + "\n")
                fo.flush()
                print(json.dumps({x: row.get(x) for x in ("set", "stem", "status", "pso", "pso_wall_s", "metal_s", "opt", "error", "pso_error")}),
                      flush=True)
    pr.close()
    rows = {}
    for l in open(res_p):
        r = json.loads(l)
        rows[r["stem"]] = r
    summ = collections.Counter((r["set"], r["status"] if r["status"] != "ok" else "pso " + str(r.get("pso"))) for r in rows.values())
    print(json.dumps({f"{s} | {k}": n for (s, k), n in sorted(summ.items())}, indent=1))


if __name__ == "__main__":
    sys.exit(main())
