#!/usr/bin/env python3
"""Bounded reproduction of a mesh shader GPU hang (hang_probe.mm) and a loop census of the same program.

  hang_probe.py prep WORK NAME [--inputs zero,fol_a] [--groups 1,16] [--census-cap 1048576] [--cap 16384]
  hang_probe.py run WORK            (every job through shaderc_msl/tools/tmem_slot.sh, one at a time, smallest first;
                                     stops at the first FAIL/ABORT; prints the gpuEvent-gta6rt count before and after)

Libraries per shader, all built like the wave-rounds batch's "a" row (corpus translation + the store's mesh passes,
mspeed.safe, capture of every output):
  ship    the row as the batch ran it (no change)
  census  ship + a trip counter per `for (;;)` of main0, counted over re-entries, capped at --census-cap; each
          thread's counts go to record 0 (words 3..) of its group by atomic max
  capped  ship + shaderc_msl's loop_cap pass (--cap, the store option `--loop-cap`)
  noout   census + no mesh output at all (returns after the capture: the program runs, nothing is rasterized)
  clamp   census + every primitive index clamped below the vertex count (the fork's --mesh-index-clamp, in MSL)
Order per input: noout, census, clamp, capped, then ship last (the unbounded one).

`run` executes only variants with the loop cap compiled in (ship is refused), and refuses the shader/input pairs in
LOCKUPS: 2MS_VS_TransformTerrainNormalTint_e74ae427~11 on fol_a locked the GPU (firmware-detected lockup) three times
on 2026-11-27 with every loop capped at 16,384 trips and with no mesh output, so a cap does not make it safe.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(PORT, "shaderc_msl", "tools"))
import wrounds  # noqa: E402  (capture fallback for mesh programs)
import sstruct as SS  # noqa: E402
import mspeed  # noqa: E402
import tmem  # noqa: E402

SLOT = os.path.join(PORT, "shaderc_msl", "tools", "tmem_slot.sh")
FOR = re.compile(r"^(\s*)for \(;;\)$")
ORDER = ("noout", "census", "clamp", "capped", "ship")
LOCKUPS = {("2MS_VS_TransformTerrainNormalTint_e74ae427_Wrapped~11", "fol_a")}


def census(msl, cap):
    """Trip counters for every `for (;;)` of the one function that holds the program's loops (main0 or the body
    function it calls; the bounded hash-probe loops of the *_mspeed helpers are left out), kept in a thread array of
    main0 that the capture block stores by atomic max into record 0, words 3.., of the group."""
    fns = mspeed.functions(msl)
    holders = [f for f, (s0, lp, rp, bs, be) in fns.items()
               if not f.endswith("_mspeed") and any(FOR.match(l) for l in msl[bs:be].split("\n"))]
    if len(holders) != 1:
        raise ValueError("census: loops in %s" % holders)
    fn = holders[0]
    s0, lp, rp, bs, be = fns[fn]
    lines = msl[bs:be].split("\n")
    out, n, i = [], 0, 0
    arr = "spvCensus" if fn != "main0" else "spvCensusArr"
    while i < len(lines):
        m = FOR.match(lines[i])
        if m and i + 1 < len(lines) and lines[i + 1] == m.group(1) + "{":
            ind = m.group(1)
            out += [lines[i], lines[i + 1], "%s    if (++%s[%d] > %du)" % (ind, arr, n, cap), "%s    {" % ind,
                    "%s        break;" % ind, "%s    }" % ind]
            n += 1
            i += 2
            continue
        out.append(lines[i])
        i += 1
    msl = msl[:bs] + "\n".join(out) + msl[be:]
    if fn != "main0":
        msl = msl[:rp - 1] + ", thread uint* spvCensus" + msl[rp - 1:]
        s0, lp, rp, bs, be = mspeed.functions(msl)["main0"]
        body = msl[bs:be]
        calls = [c.start() for c in re.finditer(r"\b%s\(" % re.escape(fn), body)]
        if not calls:
            raise ValueError("census: main0 does not call " + fn)
        for c in reversed(calls):
            close = mspeed.match_paren(body, c + len(fn))
            body = body[:close - 1] + ", spvCensusArr" + body[close - 1:]
        msl = msl[:bs] + body + msl[be:]
    s0, lp, rp, bs, be = mspeed.functions(msl)["main0"]
    body = msl[bs:be]
    anchor = "        if (gl_LocalInvocationIndex == 0) { mo[0] = spvMeshSizes.x; mo[1] = spvMeshSizes.y; mo[2] = 0x5eed5eedu; }"
    if anchor not in body:
        raise ValueError("census: no capture block")
    rec = int(re.search(r"\(gl_WorkGroupID\.x \* 129u\) \* (\d+)u", body).group(1))
    if n + 3 > rec:
        raise ValueError("census: %d loops do not fit a record of %d words" % (n, rec))
    stores = ["        atomic_fetch_max_explicit((device atomic_uint*)&mo[%du], spvCensusArr[%d], memory_order_relaxed);" % (3 + k, k)
              for k in range(n)]
    body = body.replace(anchor, anchor + "\n" + "\n".join(stores), 1)
    body = body[:2] + "    uint spvCensusArr[%d] = {};\n" % max(n, 1) + body[2:]
    return msl[:bs] + body + msl[be:], n


def prep(a):
    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    r = SS.build_lib(work, SS.CORPUS, a.name)
    if r.get("status") != "ok":
        raise SystemExit("build: %s %s" % (r.get("status"), r.get("error", "")))
    msl = SS.msl_of(r)
    safe, _ = mspeed.safe(msl)
    cap_msl, info = wrounds.capture(safe)
    rec = info["record_words"]
    variants = {"ship": cap_msl}
    variants["census"], loops = census(cap_msl, a.census_cap)
    variants["capped"], ncap = tmem.loop_cap(cap_msl, a.cap)
    gate = "    if (spvMeshSizes.y == 0)\n    {\n        return;\n    }"
    if gate not in variants["census"]:
        raise SystemExit("no mesh output gate")
    variants["noout"] = variants["census"].replace(gate, "    return;\n" + gate, 1)
    clamp = variants["census"]
    for c in "xyz":
        old = "gl_PrimitiveTriangleIndicesEXT.%s);" % c
        clamp = clamp.replace("spvMesh.set_index(spvPI * 3u + %du, %s" % ("xyz".index(c), old),
                              "spvMesh.set_index(spvPI * 3u + %du, min(gl_PrimitiveTriangleIndicesEXT.%s, spvMeshSizes.x - 1u));" % ("xyz".index(c), c))
    if clamp.count("spvMeshSizes.x - 1u") != 3:
        raise SystemExit("index clamp: set_index lines not found")
    variants["clamp"] = clamp
    libs = {}
    for k, text in variants.items():
        path = os.path.join(work, "%s.metal" % k)
        res = SS.compile_msl(text, path)
        if res is not None and isinstance(res, dict) and res.get("status") not in (None, "ok"):
            raise SystemExit("compile %s: %s" % (k, res))
        libs[k] = path[: -len(".metal")] + ".metallib"
        if not os.path.exists(libs[k]):
            raise SystemExit("compile %s: no metallib" % k)
    jobs = []
    for inp in a.inputs.split(","):
        for g in [int(x) for x in a.groups.split(",")]:
            for k in ORDER:
                prefix = os.path.join(work, "%s.g%d.%s" % (inp, g, k))
                j = SS.mesh_job(msl, [libs[k]], inp, g, 1, prefix, rec)
                j["record_words"] = rec
                j["loops"] = loops if k in ("census", "noout", "clamp") else 0
                jp = prefix + ".job.json"
                json.dump(j, open(jp, "w"))
                jobs.append({"input": inp, "groups": g, "variant": k, "job": jp})
    json.dump({"name": a.name, "loops": loops, "capped_loops": ncap, "record_words": rec, "jobs": jobs},
              open(os.path.join(work, "plan.json"), "w"), indent=1)
    print("prep: %s, %d loops in main0 (census), %d capped, record %d words, %d jobs" % (a.name, loops, ncap, rec, len(jobs)))


def gpu_events():
    try:
        return sum(1 for f in os.listdir("/Library/Logs/DiagnosticReports") if f.startswith("gpuEvent-gta6rt"))
    except OSError:
        return -1


def run(a):
    work = os.path.abspath(a.work)
    plan = json.load(open(os.path.join(work, "plan.json")))
    probe = os.path.join(work, "gta6rt_stab_hang_probe")
    before = gpu_events()
    print("gpuEvent-gta6rt before: %d" % before)
    for j in sorted(plan["jobs"], key=lambda j: (j["groups"], j["input"] != "zero", ORDER.index(j["variant"]))):
        if a.only and j["variant"] not in a.only.split(","):
            continue
        if j["variant"] == "ship" or (plan["name"], j["input"]) in LOCKUPS:
            print("refused: %s on %s (%s)" % (j["variant"], j["input"], "no loop cap" if j["variant"] == "ship" else "known GPU lockup"))
            continue
        r = subprocess.run([SLOT, probe, j["job"]], capture_output=True, text=True, timeout=7200)
        lines = [l for l in r.stdout.splitlines() if l.split("\t")[0] in ("TIME", "LOOPS", "FAIL", "ABORT")]
        print("%s g%d %s rc %d: %s" % (j["input"], j["groups"], j["variant"], r.returncode, " | ".join(lines) or r.stderr[-300:]))
        sys.stdout.flush()
        if r.returncode != 0 or any(l.startswith(("FAIL", "ABORT")) for l in lines):
            print("stopped at the first failure")
            break
        if gpu_events() != before:
            print("new gpuEvent report; stopped")
            break
    print("gpuEvent-gta6rt after: %d" % gpu_events())


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prep")
    p.add_argument("work")
    p.add_argument("name")
    p.add_argument("--inputs", default="zero,fol_a")
    p.add_argument("--groups", default="1,16")
    p.add_argument("--census-cap", type=int, default=1 << 20)
    p.add_argument("--cap", type=int, default=16384)
    r = sub.add_parser("run")
    r.add_argument("work")
    r.add_argument("--only", default="")
    a = ap.parse_args()
    prep(a) if a.cmd == "prep" else run(a)


if __name__ == "__main__":
    main()
