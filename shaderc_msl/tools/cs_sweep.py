#!/usr/bin/env python3
"""Compute pipeline build sweep with a per-shader watchdog: finds the shaders whose Metal pipeline build does not finish.

  cs_sweep.py --manifest runs/main/manifest.tsv --out cs_sweep.jsonl [--timeout 300] [--stage cs] [--skip-done]

One msl_probe process serves the jobs; if it does not answer in --timeout seconds it is killed and the shader is
recorded as TIMEOUT (pipeline creation runs on Metal's compiler service, nothing is executed on the GPU). Rows are
sorted by metallib size, smallest first, so a hang shows which size class it belongs to.
"""
import argparse, json, os, select, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
PROBE = os.path.join(ROOT, "port/build/shaderc_msl/bin2/msl_probe")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--stage", default="cs")
    ap.add_argument("--min-mb", type=float, default=0)
    ap.add_argument("--max-mb", type=float, default=1e9)
    ap.add_argument("--skip-done", action="store_true")
    ap.add_argument("--names", default="", help="only the stems listed in this file")
    a = ap.parse_args()
    root = os.path.join(os.path.dirname(os.path.abspath(a.manifest)), "..", "..", "..")
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(a.manifest)), "..", ".."))
    rows = []
    for l in open(a.manifest):
        if l.startswith("# ") and "\t" not in l:
            continue
        f = l.rstrip("\n").split("\t")
        if len(f) < 11 or f[2] != a.stage:
            continue
        lib = os.path.join(root, f[4])
        if os.path.exists(lib):
            rows.append((os.path.getsize(lib), f[0], lib))
    rows.sort()
    only = set(x.strip() for x in open(a.names)) if a.names else None
    done = set()
    if a.skip_done and os.path.exists(a.out):
        for l in open(a.out):
            done.add(json.loads(l)["stem"])
    p = None
    for size, stem, lib in rows:
        if (only is not None and stem not in only) or stem in done or not (a.min_mb * 2**20 <= size <= a.max_mb * 2**20):
            continue
        if p is None or p.poll() is not None:
            p = subprocess.Popen([PROBE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        t0 = time.time()
        p.stdin.write(json.dumps({"mode": a.stage if a.stage in ("cs", "ms") else "cs", "lib": lib, "fn": "main0", "key": stem}) + "\n")
        p.stdin.flush()
        r, _, _ = select.select([p.stdout], [], [], a.timeout)
        if not r:
            p.kill()
            p.wait()
            p = None
            row = {"stem": stem, "metallib_bytes": size, "result": "TIMEOUT", "timeout_s": a.timeout}
        else:
            line = p.stdout.readline()
            if not line:
                row = {"stem": stem, "metallib_bytes": size, "result": "PROBE_DIED"}
                p = None
            else:
                j = json.loads(line)
                row = {"stem": stem, "metallib_bytes": size, "result": j.get("result"), "pso_s": j.get("pso_s"),
                       "error": (j.get("error") or "")[:200] or None, "tg_mem": j.get("static_tg_mem"),
                       "max_threads": j.get("maxThreads")}
        row["wall_s"] = round(time.time() - t0, 2)
        open(a.out, "a").write(json.dumps(row) + "\n")
        if row["result"] != "OK" or row["wall_s"] > 30:
            print(json.dumps(row), flush=True)


if __name__ == "__main__":
    main()
