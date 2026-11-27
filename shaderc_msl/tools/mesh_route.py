#!/usr/bin/env python3
"""Mesh route decision and the split / mesh-as-compute variants for every mesh shader.

  mesh_route.py build  --build port/build/shaderc_msl [--jobs 2]      build the split variants named by the sweep
  mesh_route.py decide --build port/build/shaderc_msl                  write mesh/mesh_route.json

Routes (manifest column mesh_route):
  hw               the shader's own library builds a Metal mesh pipeline (as translated, or after dead-output
                   elimination / trim): measured in mesh_pso.jsonl and hw_verify.jsonl, else by the size model
  split64          the unsplit library exceeds Metal's mesh size; the split variant (object + mesh stage, SPIR-V
                   transform of tools/mesh_split.py) built a pipeline in meshsplit/sweep_split*.jsonl
  mesh_as_compute  neither builds (over 124 unique scalars at any size): the compute kernel + vertex shader of
                   tools/mesh_msc.py (meshsplit/msc_*.jsonl)
Inputs and outputs are files under --build; nothing here talks to the GPU.
"""
import argparse, concurrent.futures as cf, glob, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shaderc_msl as S  # noqa: E402
import batch  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
CORPUS = os.environ.get("GTA6RT_METAL_SPIRV") or os.path.join(ROOT, "spike/s2-make-game/work/shaders/spirv")


def jsonl(path):
    out = []
    if os.path.exists(path):
        for l in open(path):
            try:
                out.append(json.loads(l))
            except ValueError:
                pass
    return out


def sweep_rows(build):
    best = {}
    for f in sorted(glob.glob(os.path.join(build, "meshsplit", "sweep_split*.jsonl"))):
        for r in jsonl(f):
            if r.get("result") == "OK" or r["stem"] not in best:
                best[r["stem"]] = r
    return best


PROBE = os.path.join(HERE, "..", "..", "build", "shaderc_msl", "bin2", "msl_probe")


class Probe:
    def __init__(self):
        self.p = None

    def ms(self, lib, obj):
        import subprocess
        if self.p is None or self.p.poll() is not None:
            self.p = subprocess.Popen([PROBE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.p.stdin.write(json.dumps({"mode": "ms", "lib": lib, "fn": "main0", "obj_lib": obj, "obj_fn": "main0",
                                       "mesh_threads": 128}) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            self.p = None
            return {"result": "PROBE_DIED"}
        return json.loads(line)


def pick_chunks(vmax):
    return 1 if vmax >= 128 else 2 if vmax >= 90 else 3


_PROBE = Probe()


def build_one(args):
    """Compile the split variant into the store and create its Metal mesh pipeline; on a size refusal Metal's message
    gives the total mesh size, from which the largest vmax that fits is derived (retry, down to 64 / pmax 64)."""
    import re
    stem, hint, build, mo, fit = args
    opts0 = mo.get(stem, {"vs_route": "lane"})
    tr = S.translator_id(CORPUS)
    S.tool_versions()
    cas = os.path.join(build, "cas")
    tmp = os.path.join(build, "tmp", "split%d" % os.getpid())
    if hint:
        v, p = hint["vmax"], hint["pmax"]
    else:
        sc = fit.get("s2") or fit.get("s1") or fit.get("s0") or 124
        b = 16 * -(-sc // 4)
        v, p = max(64, min(127, int((32768 - 20 * 128) // b))), 128
    tried, row = [], {}
    for attempt in range(6):
        split = {"chunks": pick_chunks(v), "vmax": v, "pmax": p}
        r = S.compile_one(os.path.join(CORPUS, stem + ".spv"), "ms", cas, tr, dict(opts0, split_mesh=split), tmp=tmp)
        if r["status"] != "ok":
            row = {"status": "build_" + r["status"], "error": r.get("error")}
            break
        lib = lambda k: os.path.join(cas, k[:2], k + ".metallib")
        pr = _PROBE.ms(lib(r["key"]), lib(r["object_key"]))
        tried.append([v, p, pr.get("result"), (pr.get("error") or "")[:100]])
        if pr.get("result") == "OK":
            row = {"status": "ok", "key": r["key"], "object_key": r["object_key"], "chunks": split["chunks"], "vmax": v,
                   "pmax": p, "plan": r.get("plan"), "passes": r.get("passes"), "pso_s": pr.get("pso_s")}
            break
        m = re.search(r"Total mesh size \((\d+)\)", pr.get("error") or "")
        if not m:
            row = {"status": "pipeline_" + str(pr.get("result")), "error": (pr.get("error") or "")[:200]}
            break
        total = int(m.group(1))
        per_prim = 19.75
        b = (total - per_prim * p) / v
        nv = int((32768 - per_prim * p) // b)
        if nv >= v:
            nv = v - 1
        if nv < 64:
            if p > 64:
                p, nv = 64, int((32768 - per_prim * 64) // b)
            if nv < 64:
                row = {"status": "pipeline_FAIL", "error": (pr.get("error") or "")[:200]}
                break
        v = min(127, nv)
    else:
        row = {"status": "pipeline_FAIL", "error": "no vmax fit after retries"}
    row["tried"] = tried
    return stem, row


def cmd_build(a):
    sw = sweep_rows(a.build)
    mo = batch.mesh_opts(os.path.join(a.build, "mesh", "mesh_fit.jsonl"))
    fit = {r["stem"]: r for r in jsonl(os.path.join(a.build, "mesh", "mesh_fit.jsonl"))}
    out = os.path.join(a.build, "meshsplit", "split_rows.jsonl")
    done = {r["stem"] for r in jsonl(out)}
    names = set()
    if a.names:
        names = {x.strip() for x in open(a.names) if x.strip()}
    pso = {r["stem"]: r for r in jsonl(os.path.join(a.build, "mesh", "mesh_pso.jsonl"))}
    hv = {}
    for f in glob.glob(os.path.join(a.build, "hw_verify*.jsonl")):
        for r in jsonl(f):
            hv[r["stem"]] = r

    def hw_ok(stem):
        if stem in hv:
            return hv[stem]["result"] == "OK"
        if stem in pso and pso[stem].get("pso") is not None:
            return pso[stem]["pso"] == "OK"
        return None

    todo = []
    for stem, r in sorted(fit.items()):
        if r["step"] == 4 or stem in done:
            continue
        if names:
            if stem not in names:
                continue
        elif r["step"] == 3 or (r["step"] == 2 and hw_ok(stem) is not True):
            pass
        else:
            continue
        hint = None
        if stem in sw and sw[stem].get("result") == "OK":
            hint = {"vmax": sw[stem]["vmax"], "pmax": sw[stem]["pmax"]}
        elif stem in sw and r["step"] == 3:
            hint = None
        todo.append((stem, hint, a.build, mo, r))
    print(f"split variants to build: {len(todo)} (already {len(done)})", flush=True)
    with cf.ProcessPoolExecutor(a.jobs) as ex, open(out, "a") as fo:
        for stem, row in ex.map(build_one, todo, chunksize=1):
            row["stem"] = stem
            fo.write(json.dumps(row) + "\n")
            fo.flush()
            if row["status"] != "ok":
                print("FAIL", stem, row.get("error"), flush=True)


def cmd_objects(a):
    import mesh_split
    tr = S.translator_id(CORPUS)
    S.tool_versions()
    tmp = os.path.join(a.build, "tmp", "objects")
    os.makedirs(tmp, exist_ok=True)
    lines = ["# split object-stage libraries: chunks (mesh groups launched per original group) -> metallib; the mesh "
             "library does not depend on it, so a dispatch may pick any chunks >= the manifest's split_chunks"]
    for k in range(1, 8):
        key = S.compile_task(mesh_split.assemble(mesh_split.task_module(k)), os.path.join(a.build, "cas"), tmp)
        lines.append(f"{k}\tcas/{key[:2]}/{key}.metallib")
    open(os.path.join(a.build, "runs", "main", "split_objects.tsv"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


def cmd_decide(a):
    steps = {}
    for r in jsonl(os.path.join(a.build, "mesh", "mesh_fit.jsonl")):
        steps[r["stem"]] = r["step"]
    pso = {r["stem"]: r for r in jsonl(os.path.join(a.build, "mesh", "mesh_pso.jsonl"))}
    hv = {}
    for f in glob.glob(os.path.join(a.build, "hw_verify*.jsonl")):
        for r in jsonl(f):
            hv[r["stem"]] = r
    sw = sweep_rows(a.build)
    split_rows = {r["stem"]: r for r in jsonl(os.path.join(a.build, "meshsplit", "split_rows.jsonl"))}
    msc = {}
    for f in glob.glob(os.path.join(a.build, "meshsplit", "msc_*.jsonl")):
        for r in jsonl(f):
            msc[r["stem"]] = r
    route, why = {}, {}
    for stem, st in steps.items():
        hw_ok = None
        if stem in hv:
            hw_ok = hv[stem]["result"] == "OK"
            basis = "measured (hw_verify)"
        elif stem in pso and pso[stem].get("pso") is not None:
            hw_ok = pso[stem]["pso"] == "OK"
            basis = "measured (mesh_pso)"
        if hw_ok is None and st in (0, 1, 2):
            hw_ok, basis = True, "model"
        if hw_ok:
            route[stem] = {"route": "hw", "basis": basis, "step": st}
        elif stem in split_rows and split_rows[stem]["status"] == "ok":
            r = split_rows[stem]
            route[stem] = {"route": "split64", "step": st, "basis": "pipeline built (sweep_split)", "key": r["key"],
                           "object_key": r["object_key"], "chunks": r["chunks"], "vmax": r["vmax"], "pmax": r["pmax"],
                           "plan": r["plan"]}
        elif stem in msc and msc[stem]["status"] == "ok":
            m = msc[stem]
            route[stem] = {"route": "mesh_as_compute", "step": st, "basis": "split refused: " + (sw.get(stem, {}).get("error") or "n/a")[:120],
                           "cs_key": m["cs_key"], "vs_key": m["vs_key"], "layout": m["layout"], "local_size": m["local_size"],
                           "cs_push": m["cs_push"], "vs_push": m["vs_push"], "cs_sets": m["cs_sets"], "vs_sets": m["vs_sets"]}
        else:
            route[stem] = {"route": "unresolved", "step": st}
    cnt = {}
    for r in route.values():
        cnt[r["route"]] = cnt.get(r["route"], 0) + 1
    json.dump(route, open(os.path.join(a.build, "mesh", "mesh_route.json"), "w"))
    print(json.dumps(cnt))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["build", "decide", "objects"])
    ap.add_argument("--build", required=True)
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--names", default="", help="build: only these stems (any step)")
    a = ap.parse_args()
    {"build": cmd_build, "decide": cmd_decide, "objects": cmd_objects}[a.cmd](a)


if __name__ == "__main__":
    main()
