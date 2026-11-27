#!/usr/bin/env python3
"""sstruct.py's mesh A/B (A = corpus row, B0..Bn = fork variants; capture + safe variants; every output word compared;
warm-up and late copies of A) split into a CPU phase and one GPU phase, so a batch of shaders takes the gta6rt slot once.

  wrounds_batch.py prep WORK LIST.tsv [--inputs zero,fol_a] [--groups 2048] [--reps 5]
        LIST.tsv: NAME <tab> FLAGS0;FLAGS1;...   (translate, build, write job files: no GPU)
  wrounds_batch.py run WORK                      (all jobs under ONE tmem_slot.sh session; then compare -> WORK/ab.jsonl)
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import wrounds  # noqa: E402,F401  (tree, probe paths, capture fallback)
import sstruct as SS  # noqa: E402
import mspeed  # noqa: E402
import mspeed_cmp  # noqa: E402
import hashlib  # noqa: E402


def prep_one(work, name, variants, inputs, groups, reps):
    tag = hashlib.sha256(name.encode()).hexdigest()[:10]
    res = {"name": name, "variants": [",".join(v) for v in variants], "tag": tag}
    builds = [("a", SS.CORPUS, None)]
    for k, flags in enumerate(variants):
        fork_dir = os.path.join(work, "spv-%s-%d" % (tag, k))
        SS.translate(fork_dir, [name], flags)
        builds.append(("b%d" % k, fork_dir, flags))
    out = {}
    for k, spv_dir, flags in builds:
        r = SS.build_lib(work, spv_dir, name, None if k == "a" else SS.FORK_PASSES)
        if r.get("status") != "ok":
            raise RuntimeError("%s build %s: %s" % (k, r.get("status"), r.get("error", "")))
        msl = SS.msl_of(r)
        res["cases_" + k] = len(re.findall(r"\n\s*case \d+u:", msl))
        safe, _ = mspeed.safe(msl)
        cap, info = mspeed.capture(safe)
        path = os.path.join(work, "%s.%s.cap.metal" % (tag, k))
        SS.compile_msl(cap, path)
        out[k] = (path[: -len(".metal")] + ".metallib", info, msl)
    rec = out["a"][1]["record_words"]
    if any(v[1]["record_words"] != rec for v in out.values()):
        raise RuntimeError("output layout differs")
    keys = [b[0] for b in builds]
    for k2 in ("warm", "a_late"):
        cp = out["a"][0][: -len(".metallib")] + "." + k2 + ".metallib"
        shutil.copyfile(out["a"][0], cp)
        out[k2] = (cp, out["a"][1], out["a"][2])
    order = ["warm"] + keys + ["a_late"]
    res["jobs"] = {}
    for inp in inputs:
        prefix = os.path.join(work, "%s.%s" % (tag, inp))
        job = SS.mesh_job(out["a"][2], [out[k][0] for k in order], inp, groups, reps, prefix, rec)
        jp = prefix + ".job.json"
        json.dump(job, open(jp, "w"))
        res["jobs"][inp] = jp
    res["libs"] = {k: out[k][0] for k in order}
    res["record_words"] = rec
    res["keys"] = keys
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("work")
    ap.add_argument("list", nargs="?")
    ap.add_argument("--inputs", default="zero,fol_a")
    ap.add_argument("--groups", type=int, default=2048)
    ap.add_argument("--reps", type=int, default=5)
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    plan = os.path.join(a.work, "plan.jsonl")
    if a.cmd == "prep":
        done = set()
        if os.path.exists(plan):
            done = {json.loads(l)["name"] for l in open(plan)}
        for line in open(a.list):
            if not line.strip():
                continue
            name, flags = line.rstrip("\n").split("\t")
            if name in done:
                continue
            variants = [[f for f in v.split(",") if f] for v in flags.split(";")]
            try:
                r = prep_one(a.work, name, variants, a.inputs.split(","), a.groups, a.reps)
            except Exception as e:
                r = {"name": name, "error": str(e)[:300]}
            with open(plan, "a") as f:
                f.write(json.dumps(r) + "\n")
            print(name, "ok" if "error" not in r else r["error"], flush=True)
        return
    if a.cmd == "run":
        recs = [json.loads(l) for l in open(plan)]
        jobs = [jp for r in recs if "jobs" in r for jp in r["jobs"].values()]
        script = os.path.join(a.work, "run_all.sh")
        with open(script, "w") as f:
            f.write("#!/bin/bash\n")
            for jp in jobs:
                f.write('"%s" "%s" > "%s.out.txt" 2>&1\n' % (SS.PROBE, jp, jp))
        os.chmod(script, 0o755)
        subprocess.run([SS.SLOT, script])
        with open(os.path.join(a.work, "ab.jsonl"), "w") as fo:
            for r in recs:
                if "jobs" not in r:
                    fo.write(json.dumps(r) + "\n")
                    continue
                res = {k: r[k] for k in ("name", "variants") if k in r}
                res.update({k: v for k, v in r.items() if k.startswith("cases_")})
                res["inputs"] = {}
                for inp, jp in r["jobs"].items():
                    times = {}
                    for line in open(jp + ".out.txt"):
                        fld = line.split("\t")
                        if fld[0] == "TIME":
                            m = re.search(r"([\d.]+) us a group \(best\)\t([\d.]+) \(median\)", line)
                            times[fld[1]] = (float(m.group(1)), float(m.group(2)))
                        elif fld[0] == "FAIL":
                            times["FAIL"] = line.strip()
                    prefix = jp[: -len(".job.json")]
                    base = lambda k: os.path.basename(r["libs"][k])
                    ri = {"us_group_a": times.get(base("a")), "us_group_a_late": times.get(base("a_late")),
                          "us_group_warm": times.get(base("warm")), "fail": times.get("FAIL")}
                    fa = "%s.%s.out" % (prefix, base("a"))
                    for k in r["keys"][1:]:
                        fb = "%s.%s.out" % (prefix, base(k))
                        ri["us_group_" + k] = times.get(base(k))
                        if os.path.exists(fa) and os.path.exists(fb):
                            st, first = mspeed_cmp.compare(r["record_words"], fa, fb)
                            ri["cmp_" + k] = st
                            ri["first_" + k] = first[:3]
                            os.remove(fb)
                    for k2 in ("warm", "a_late", "a"):
                        try:
                            os.remove("%s.%s.out" % (prefix, base(k2)))
                        except OSError:
                            pass
                    res["inputs"][inp] = ri
                fo.write(json.dumps(res) + "\n")
        return
    raise SystemExit("cmd: prep | run")


if __name__ == "__main__":
    main()
