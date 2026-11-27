#!/usr/bin/env python3
"""Run bin/vs_lane_test over a whole manifest, restarting after a GPU error (Metal ignores a process's later
submissions once one of its command buffers hung or faulted). Appends every line to <log>."""
import os, subprocess, sys

test, manifest, log = sys.argv[1:4]
n = sum(1 for l in open(manifest) if l.startswith("shader "))
start = 0
with open(log, "w") as f:
    while start < n:
        env = dict(os.environ, SHADERC_VS_LANE_START=str(start))
        p = subprocess.run([test, manifest], capture_output=True, text=True, env=env, timeout=3600)
        f.write(p.stdout)
        stop = [l for l in p.stdout.splitlines() if l.startswith("stop_at ")]
        if p.returncode == 3 and stop:
            start = int(stop[-1].split()[1]) + 1
        else:
            break
