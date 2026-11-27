#!/usr/bin/env python3
"""The proposed verify.sh S4 checks for the native Metal path, on one finished s7_world_run.sh run.

  s4_check.py RUN [SCENE_DIR]

S4a (stability): the world phase is reached and the process is still up 180 s later (the watchdog ends it: run.sh rc
124 or "killed"), with no fatal signal or SEH abort, no GPU error ("Metal stopped running this process's work", a
failed command buffer of its own), no device-lost exit (3) and no footprint-cap exit (4).
S4b (first world frame): the scene probe's colour dump (GTA6RT_METAL_SCENE_DUMP) of a frame in the world exists and has
at least 5 % non-zero texels, and the G-buffer dump has at least one target with 5 % non-zero texels.
Prints PASS/FAIL lines and the run's frames/s and footprint; exit 0 only when both pass.
"""
import os
import re
import sys

LOGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "run", "logs")


def main():
    run = sys.argv[1]
    lines = open(os.path.join(LOGS, run + ".log"), errors="replace").read().splitlines()
    world = end = rc = None
    for l in lines:
        m = re.match(r"\[host\] stage t=([\d.]+)s: world", l)
        if m and world is None:
            world = float(m.group(1))
        m = re.match(r"run\.sh: rc=(\d+) after (\d+) s", l)
        if m:
            rc, end = int(m.group(1)), float(m.group(2))
        m = re.match(r"run\.sh: killed after (\d+)s", l)
        if m:
            rc, end = 124, float(m.group(1))
    bad = [l for l in lines if re.search(r"fatal signal|\[seh\] FATAL|Metal stopped running|failed \(own error\)|"
                                         r"FOOTPRINT_CAP_MB|the Metal device was lost", l)]
    stayed = (end - world) if world is not None and end is not None else 0
    ok_a = world is not None and rc in (0, 124) and stayed >= 180 and not bad
    print("%s S4a world phase stays up 180 s on Metal: world at %s s, %.0f s in it, rc %s%s" % (
        "PASS" if ok_a else "FAIL", "%.1f" % world if world is not None else "-", stayed, rc,
        "; " + bad[0][:160] if bad else ""))
    colour = [l for l in lines if re.match(r"\[rt\] d3d12-metal scene colour \(", l)]
    gbuf = [l for l in lines if re.match(r"\[rt\] d3d12-metal scene gbuffer\d \(", l)]
    lit = lambda l: float(re.search(r": ([\d.]+)% of texels non-zero", l).group(1))
    best_colour = max((lit(l) for l in colour), default=0.0)
    best_gbuf = max((lit(l) for l in gbuf), default=0.0)
    ok_b = best_colour >= 5.0 and best_gbuf >= 5.0
    print("%s S4b world frame: scene colour %.1f%% non-zero texels (%d dumps), G-buffer best %.1f%% (%d targets)" % (
        "PASS" if ok_b else "FAIL", best_colour, len(colour), best_gbuf, len(gbuf)))
    fps = []
    for l in lines:
        m = re.search(r"submit phases t=(\d+)s: .*?([\d.]+) frames/s", l)
        if m and world is not None and int(m.group(1)) > world + 10:
            fps.append(float(m.group(2)))
    mem = [l for l in lines if l.startswith("[rt] gpu-memory t=")]
    if fps:
        print("info: frames/s in the world: median %.1f (min %.1f, max %.1f)" % (sorted(fps)[len(fps) // 2], min(fps), max(fps)))
    if mem:
        m = re.search(r"process footprint (\d+) MiB", mem[-1])
        print("info: last footprint %s MiB" % (m.group(1) if m else "?"))
    return 0 if ok_a and ok_b else 1


if __name__ == "__main__":
    sys.exit(main())
