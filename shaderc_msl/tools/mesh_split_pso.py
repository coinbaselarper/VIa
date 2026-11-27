#!/usr/bin/env python3
"""Pipeline creation for the split variants: build (object + mesh) for each stem and create the Metal mesh pipeline.

  mesh_split_pso.py --stems FILE --work DIR [--chunks 3] [--vmax 64] [--pmax 64] [--out results.jsonl]

Result rows: stem, step (from mesh_fit), result (OK or Metal's error text), build seconds, pipeline seconds. One
msl_probe process serves all jobs (pipeline creation does not run anything on the GPU).
"""
import argparse, json, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mesh_split_check as C  # noqa: E402
import shaderc_msl as S  # noqa: E402

PROBE = os.path.join(C.ROOT, "port/build/shaderc_msl/bin2/msl_probe")


class Probe:
    def __init__(self):
        self.p = None

    def job(self, j):
        if self.p is None or self.p.poll() is not None:
            self.p = subprocess.Popen([PROBE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.p.stdin.write(json.dumps(j) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            self.p = None
            return {"result": "PROBE_DIED"}
        return json.loads(line)


def pick_chunks(vmax):
    return 1 if vmax >= 128 else 2 if vmax >= 90 else 3


def try_one(pr, C, stem, work, chunks, vmax, pmax, opts):
    sp = C.build_split(stem, work, chunks, False, vmax, pmax, opts)
    r = pr.job({"mode": "ms", "lib": sp["mesh"], "fn": "main0", "obj_lib": sp["task"], "obj_fn": "main0", "mesh_threads": 128})
    return sp, r


def auto(pr, C, stem, work, opts, pmax=128):
    out, sp = auto1(pr, C, stem, work, opts, pmax)
    if sp is None and pmax > 64 and "Total mesh size" in (out.get("error") or ""):
        out2, sp2 = auto1(pr, C, stem, work, opts, 64)
        out2["probes"] = out["probes"] + out2["probes"]
        return out2, sp2
    return out, sp


def auto1(pr, C, stem, work, opts, pmax=128):
    """Largest vmax that builds: probe at 127 (Metal's error names the total mesh size, so bytes per vertex), then at
    floor(32768 / bytes per vertex), down to 64. Returns (row fields, sp)."""
    out = {"probes": []}
    sp, r = try_one(pr, C, stem, work, 2, 127, pmax, opts)
    out["probes"].append([127, r.get("result"), (r.get("error") or "")[:120]])
    v = 127
    if r.get("result") != "OK":
        m = re.search(r"Total mesh size \((\d+)\)", r.get("error") or "")
        if not m:
            out["result"], out["error"] = r.get("result"), (r.get("error") or "")[:200]
            return out, None
        b = int(m.group(1)) / 127.0
        v = int(32768 // b)
        tries = 0
        while v >= 64 and tries < 8:
            sp, r = try_one(pr, C, stem, work, 2, v, pmax, opts)
            out["probes"].append([v, r.get("result"), (r.get("error") or "")[:120]])
            if r.get("result") == "OK":
                break
            v -= 1 if tries < 4 else 4
            tries += 1
        else:
            out["result"], out["error"] = "FAIL", out["probes"][-1][2]
            return out, None
    out.update(result="OK", vmax=v, pmax=pmax, chunks=pick_chunks(v), pso_s=r.get("pso_s"))
    return out, sp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stems", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--chunks", type=int, default=3)
    ap.add_argument("--vmax", type=int, default=64)
    ap.add_argument("--pmax", type=int, default=64)
    ap.add_argument("--out", default="")
    ap.add_argument("--auto", action="store_true")
    ap.add_argument("--skip-ok", default="", help="glob of result files; stems with an OK row there are skipped")
    a = ap.parse_args()
    a.work = os.path.abspath(a.work)
    os.makedirs(a.work, exist_ok=True)
    steps = {}
    for l in open(os.path.join(C.ROOT, "port/build/shaderc_msl/mesh/mesh_fit.jsonl")):
        r = json.loads(l)
        steps[r["stem"]] = r
    import batch
    mo = batch.mesh_opts(os.path.join(C.ROOT, "port/build/shaderc_msl/mesh/mesh_fit.jsonl"))
    pr = Probe()
    skip = set()
    if a.skip_ok:
        import glob
        for f in glob.glob(a.skip_ok):
            for l in open(f):
                try:
                    r = json.loads(l)
                except ValueError:
                    continue
                if r.get("result") == "OK":
                    skip.add(r["stem"])
    for stem in [x.strip() for x in open(a.stems) if x.strip() and x.strip() not in skip]:
        row = {"stem": stem, "step": steps.get(stem, {}).get("step"), "vmax": a.vmax, "pmax": a.pmax}
        t0 = time.time()
        try:
            if a.auto:
                res, _ = auto(pr, C, stem, a.work, mo.get(stem, {"vs_route": "lane"}), a.pmax)
                row.update(res)
                row["build_s"] = round(time.time() - t0, 2)
                print(json.dumps(row), flush=True)
                if a.out:
                    open(a.out, "a").write(json.dumps(row) + "\n")
                continue
            sp = C.build_split(stem, a.work, a.chunks, False, a.vmax, a.pmax, mo.get(stem, {"vs_route": "lane"}))
            row["build_s"] = round(time.time() - t0, 2)
            mm = re.search(r"mesh<(\w+), (\w+), (\d+), (\d+), topology::(\w+)>", sp["msl"])
            row["mesh_decl"] = [int(mm.group(3)), int(mm.group(4))] if mm else None
            r = pr.job({"mode": "ms", "lib": sp["mesh"], "fn": "main0", "obj_lib": sp["task"], "obj_fn": "main0",
                        "mesh_threads": 128})
            row["result"] = r.get("result")
            row["error"] = (r.get("error") or "")[:200] or None
            row["pso_s"] = r.get("pso_s")
        except Exception as e:  # noqa: BLE001
            row["result"] = "BUILD_ERROR"
            row["error"] = str(e)[:300]
        print(json.dumps(row), flush=True)
        if a.out:
            open(a.out, "a").write(json.dumps(row) + "\n")


if __name__ == "__main__":
    main()
