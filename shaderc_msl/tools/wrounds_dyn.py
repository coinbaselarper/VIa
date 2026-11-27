#!/usr/bin/env python3
"""Dynamic exchange census of one mesh shader: the fork translation with FLAGS, its exchange segments named by
cause (translator-fork tools/wround_census.py), a trace variant (mspeed.py trace) run for one group on INPUT; every
round the group's two waves start (lanes 0 and 64) is counted by the cause of the exchange that ended the round
before it.
  wrounds_dyn.py WORK NAME INPUT --flags=a,b,..."""
import argparse
import collections
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import wrounds  # noqa: E402,F401  (tree and probe paths)
import sstruct as SS  # noqa: E402
import mspeed  # noqa: E402
import mspeed_trace as MT  # noqa: E402

CENSUS = os.path.join(SS.PORT, "shaders_vk", "translator-fork", "tools", "wround_census.py")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("name")
    ap.add_argument("input")
    ap.add_argument("--flags", default="")
    a = ap.parse_args()
    flags = [f for f in a.flags.split(",") if f]
    os.makedirs(a.work, exist_ok=True)
    stage = os.path.join(a.work, "disasm")
    os.makedirs(stage, exist_ok=True)
    src = os.path.join(SS.DISASM, a.name + ".s")
    dst = os.path.join(stage, a.name + ".s")
    if not os.path.exists(dst):
        os.symlink(src, dst)
    spv = os.path.join(a.work, "spv")
    cj = os.path.join(a.work, "census.jsonl")
    r = subprocess.run([SS.PY, "-I", CENSUS, stage, cj, spv, *SS.BASE_FLAGS, *flags], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stderr[-800:])
    rec = json.loads(open(cj).read().splitlines()[-1])
    segs = {int(k): v for k, v in rec["segs"].items()}
    lib = SS.build_lib(a.work, spv, a.name, SS.FORK_PASSES if flags else None)
    msl = SS.msl_of(lib)
    tr, _ = mspeed.trace(msl)
    path = os.path.join(a.work, "dyn.trace.metal")
    SS.compile_msl(tr, path)
    tlib = path[: -len(".metal")] + ".metallib"
    prefix = os.path.join(a.work, "dyn")
    st = re.search(r"struct spvPerVertex\n\{\n(.*?)\n\};", msl, re.S).group(1)
    job = SS.mesh_job(msl, [tlib], a.input, 1, 1, prefix, 8 + 4 * len(re.findall(r"float4 m_(\d+) \[\[user", st)))
    job.pop("out_index")
    job["out_bytes"] = 0
    json.dump(job, open(prefix + ".job.json", "w"))
    SS.probe(prefix + ".job.json")
    tf = "%s.%s.trace" % (prefix, os.path.basename(tlib))
    t = MT.summary(MT.load(tf))
    os.remove(tf)
    by_lane = collections.defaultdict(list)
    for e in t["seq"]:
        by_lane[(e >> 10) & 63].append((e >> 16, e & 1023))
    out = {"name": a.name, "flags": flags, "input": a.input, "outer_rounds_simd": t["outer_simd"], "lanes": {}}
    for lane, seq in sorted(by_lane.items()):
        rounds = len({x for x, _ in seq})
        causes = collections.Counter(segs.get(pc, "block-start") for _, pc in seq)
        out["lanes"][lane] = {"segments": len(seq), "rounds": rounds, "by_cause": dict(causes.most_common())}
    print(json.dumps(out))


if __name__ == "__main__":
    main()
