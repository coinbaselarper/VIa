#!/usr/bin/env python3
"""Watchdog runs of tests/vs_lane_test over the 3 vertex shaders that hang the GPU on synthetic data, with the heap
words set to a chosen integer (the loop limit the shaders read from memory) -- never while a gta6rt process exists.

  vs_hang_run.py --manifest vs_hang/manifest.txt --test bin2/vs_lane_test --out vs_hang.jsonl
"""
import argparse, json, os, subprocess, sys, time


def gta6rt_running():
    r = subprocess.run(["ps", "-axo", "comm="], capture_output=True, text=True).stdout
    return any(os.path.basename(x.strip()) == "gta6rt" for x in r.splitlines())


def wait_free():
    while gta6rt_running():
        time.sleep(0.25)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--n", type=int, default=64)
    ap.add_argument("--heaps", default="1,2,4,8,16,32,64,128,256,1024,4096,pattern")
    a = ap.parse_args()
    stems = [l.split()[1] for l in open(a.manifest) if l.startswith("shader ")]
    for hv in a.heaps.split(","):
        for i, stem in enumerate(stems):
            env = dict(os.environ, SHADERC_VS_LANE_N=str(a.n), SHADERC_VS_LANE_NOBIG="1", SHADERC_VS_LANE_LIMIT="1",
                       SHADERC_VS_LANE_START=str(i))
            if hv != "pattern":
                env["SHADERC_VS_LANE_HEAP_INT"] = hv
            wait_free()
            t0 = time.time()
            try:
                p = subprocess.run([a.test, a.manifest], capture_output=True, text=True, env=env, timeout=a.timeout)
                out, rc, to = p.stdout, p.returncode, False
            except subprocess.TimeoutExpired as e:
                out, rc, to = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or ""), None, True
            row = {"stem": stem, "heap": hv, "n": a.n, "wall_s": round(time.time() - t0, 2), "rc": rc, "watchdog_timeout": to,
                   "lines": [l[:300] for l in out.splitlines() if l.startswith("shader ")][:3],
                   "stdout_tail": out[-1500:]}
            print(json.dumps(row), flush=True)
            open(a.out, "a").write(json.dumps(row) + "\n")


if __name__ == "__main__":
    main()
