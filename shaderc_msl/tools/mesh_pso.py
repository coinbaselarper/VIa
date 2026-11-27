#!/usr/bin/env python3
"""Measure the mesh steps with real pipelines (mesh_fit.py only predicts).

  step 0  a seeded sample of the shaders that fit as translated: PSO from the batch metallib
  step 1/2  every shader the batch built with drop_outputs / trim_outputs: PSO from the batch metallib
  step 3  every shader predicted to need a meshlet split: MSL regenerated with the same pre-passes, declaration
          rewritten to 64 vertices / 64 primitives, compiled and built. This only measures whether Metal accepts the
          split shape; the shader body is not split, so such a library must not be used.
Writes <build>/mesh/mesh_pso.jsonl and mesh_pso.json.
"""
import argparse, collections, concurrent.futures as cf, json, os, random, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe as P  # noqa: E402
import shaderc_msl as S  # noqa: E402
import spv  # noqa: E402
import verify_bindings as V  # noqa: E402


def split64(spirv_dir, tmp, row, opts):
    words = spv.load(os.path.join(spirv_dir, row["stem"] + ".spv"))
    w, passes, extra = S.prepare(words, "ms", opts)
    base = os.path.join(tmp, re.sub(r"[^A-Za-z0-9]", "_", row["stem"]))
    spv.save(w, base + ".spv")
    rc, out, err, _ = S.run([S.SPIRV_CROSS, base + ".spv", *S.RECIPE["cross"], "--msl-version", str(S.RECIPE["msl_version"])], 300)
    if rc != 0:
        return None, "cross: " + err.decode(errors="replace")[:200]
    msl = out.decode()
    if opts.get("trim_outputs"):
        msl, _ = S.pack_mesh_struct(msl)
    msl2, n = re.subn(r"\bmesh<(\w+), (\w+), 128, 128, topology::triangle>", r"mesh<\1, \2, 64, 64, topology::triangle>", msl)
    if n == 0:
        return None, "no 128/128 declaration"
    open(base + ".metal", "w").write(msl2)
    rc, _, err, _ = S.run([S.tool("metal"), "-std=" + S.RECIPE["std"], "-c", "-w", base + ".metal", "-o", base + ".air"], 600)
    if rc != 0:
        return None, "metal: " + err.decode(errors="replace")[:200]
    rc, _, err, _ = S.run([S.tool("metallib"), base + ".air", "-o", base + ".metallib"], 600)
    for e in (".spv", ".metal", ".air"):
        os.remove(base + e)
    return (base + ".metallib", None) if rc == 0 else (None, "metallib")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spirv-dir", required=True)
    ap.add_argument("--build", required=True)
    ap.add_argument("--run", default="main")
    ap.add_argument("--sample0", type=int, default=300)
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--split-only", action="store_true", help="only the step 3/4 split64 check, into mesh_split64.*")
    ap.add_argument("--split-sample", type=int, default=0, help="with --split-only: seeded sample of that many")
    a = ap.parse_args()
    fit = [json.loads(l) for l in open(os.path.join(a.build, "mesh", "mesh_fit.jsonl"))]
    index = json.load(open(os.path.join(a.spirv_dir, "index.json")))
    res = {}
    for line in open(os.path.join(a.build, "runs", a.run, "results.jsonl")):
        r = json.loads(line)
        res[r["stem"]] = r
    rng = random.Random(9282)
    s0 = [r for r in fit if r["step"] == 0]
    s0 = rng.sample(s0, min(a.sample0, len(s0)))
    s12 = [r for r in fit if r["step"] in (1, 2)]
    s3 = [r for r in fit if r["step"] in (3, 4)]
    if a.split_only:
        s0, s12 = [], []
        if a.split_sample:
            s3 = random.Random(6464).sample(s3, min(a.split_sample, len(s3)))
    tmp = os.path.join(a.build, "mesh", "tmp_split" if a.split_only else "tmp")
    os.makedirs(tmp, exist_ok=True)
    import batch as B
    mo = B.mesh_opts(os.path.join(a.build, "mesh", "mesh_fit.jsonl"))
    tr = S.translator_id(a.spirv_dir)
    cas = os.path.join(a.build, "cas")
    need = [r for r in s0 + s12 if res.get(r["stem"], {}).get("status") != "ok"]
    name = "mesh_split64" if a.split_only else "mesh_pso"

    def comp(r):
        o = B.shader_opts(index, r["stem"], mo, None)
        return r["stem"], S.compile_one(os.path.join(a.spirv_dir, r["stem"] + ".spv"), "ms", cas, tr, o,
                                        tmp=os.path.join(tmp, "c%d" % os.getpid()))

    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for stem, r in ex.map(comp, need):
            res[stem] = r
    print(f"compiled {len(need)} mesh shaders the batch had not built yet", flush=True)
    s0 = [r for r in s0 if res[r["stem"]].get("status") == "ok"]
    s12 = [r for r in s12 if res[r["stem"]].get("status") == "ok"]
    pr = P.Probe()
    out = open(os.path.join(a.build, "mesh", name + ".jsonl"), "w", buffering=1)
    summ = collections.Counter()
    t0 = time.monotonic()
    for r in s0 + s12:
        b = res[r["stem"]]
        lib = os.path.join(a.build, "cas", b["key"][:2], b["key"] + ".metallib")
        j = pr.run({"mode": "ms", "lib": lib, "fn": "main0", "reflect": True}, timeout=300)
        row = {"stem": r["stem"], "step": r["step"], "pso": j.get("result"), "error": j.get("error", "")[:200],
               "pso_s": j.get("pso_s")}
        if j.get("result") == "OK":
            got, problems = V.check_split(index[r["stem"]], b, j)
            row["binding_problems"] = problems
            summ[("bindings", "ok" if not problems else "mismatch")] += 1
        m = re.search(r"Total mesh size \((\d+)\)", row["error"])
        if m:
            row["mesh_bytes"] = int(m.group(1))
        summ[(r["step"], row["pso"])] += 1
        out.write(json.dumps(row) + "\n")
    t1 = time.monotonic()

    def build(r):
        o = {}
        if r.get("drop") is not None:
            o["drop_outputs"] = r["drop"]
            if r.get("widths"):
                o["trim_outputs"] = r["widths"]
        return r, split64(a.spirv_dir, tmp, r, o)

    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for r, (lib, err) in ex.map(build, s3):
            row = {"stem": r["stem"], "step": r["step"], "split64": True}
            if lib is None:
                row.update(pso="BUILD_FAIL", error=err)
            else:
                j = pr.run({"mode": "ms", "lib": lib, "fn": "main0"}, timeout=300)
                row.update(pso=j.get("result"), error=j.get("error", "")[:200])
                os.remove(lib)
            summ[(r["step"], "split64 " + row["pso"])] += 1
            out.write(json.dumps(row) + "\n")
    pr.close()
    out.close()
    js = {"by_step_and_result": {f"step {k[0]} {k[1]}": v for k, v in sorted(summ.items(), key=lambda kv: str(kv[0]))},
          "sampled_step0": len(s0), "step12_built": len(s12), "step34_checked": len(s3),
          "seconds": {"pso_steps012": round(t1 - t0, 1), "split64": round(time.monotonic() - t1, 1)}}
    json.dump(js, open(os.path.join(a.build, "mesh", name + ".json"), "w"), indent=1)
    print(json.dumps(js, indent=1))


if __name__ == "__main__":
    sys.exit(main())
