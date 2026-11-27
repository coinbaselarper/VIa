#!/usr/bin/env python3
"""One summary block per world-phase run of the Metal path (s7_world_run.sh logs): when the world phase started, how
the run ended and how long it stayed in the world, GPU and host errors, frames/s and ms per submission in the world,
the footprint by category, the last work report (draw census), the scene probe's lines and the budget monitor's.

  s7_evidence.py RUN... > run/logs/s4-worldframe-metal.log
"""
import os
import re
import sys

LOGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "run", "logs")


def num(s):
    return float(s) if s is not None else None


def summarise(name):
    path = os.path.join(LOGS, name + ".log")
    if not os.path.exists(path):
        return ["== %s: no log" % name]
    lines = open(path, errors="replace").read().splitlines()
    out = ["=" * 100, "== %s  (%s)" % (name, os.path.abspath(path))]
    world = end = None
    end_line = ""
    start = next((l for l in lines if l.startswith("run.sh: start")), "")
    out.append(start)
    for l in lines:
        m = re.match(r"\[host\] stage t=([\d.]+)s: world", l)
        if m and world is None:
            world = float(m.group(1))
        m = re.match(r"run\.sh: (?:rc=(\d+) after (\d+) s|killed after (\d+)s)", l)
        if m:
            end = float(m.group(2) or m.group(3))
            end_line = l
    env = [l for l in lines if l.startswith("[rt] d3d12-metal scene probe:") or "GTA6RT_" in l and l.startswith("[s7] env")]
    out += env
    fatal = [l for l in lines if re.search(r"fatal signal|\[seh\] FATAL|Metal stopped running|device was lost|"
                                           r"footprint guard|std::terminate called|SIGSEGV on|SIGABRT on thread", l)]
    timeouts = [l for l in lines if l.startswith("[metal] command buffer") or l.startswith("[metal]   encoder")]
    in_world = (end - world) if (world is not None and end is not None) else None
    out.append("world phase at %s s; ended %s; in the world %s s" % (
        "%.1f" % world if world is not None else "never", end_line or "(no run.sh end line)",
        "%.0f" % in_world if in_world is not None else "-"))
    out += ["  " + l[:300] for l in fatal[:8]]
    out += ["  " + l[:400] for l in timeouts[:12]]
    phases = []
    for l in lines:
        m = re.match(r"\[rt\] submit phases t=(\d+)s: (\d+) submissions in ([\d.]+) s \(([\d.]+)/s.*?\), ([\d.]+) frames/s; "
                     r"ms per submission \(max\): (.*?); (\d+)% of the wall time", l)
        if m and world is not None and int(m.group(1)) > world:
            phases.append(l)
    if phases:
        out.append("submit phases in the world (%d windows of 5 s):" % len(phases))
        shown = phases if len(phases) <= 6 else phases[:3] + phases[-3:]
        out += ["  " + l[len("[rt] submit phases "):] for l in shown]
        fps = [float(re.search(r"([\d.]+) frames/s", l).group(1)) for l in phases]
        out.append("  frames/s in the world: min %.1f, median %.1f, max %.1f" % (min(fps), sorted(fps)[len(fps) // 2], max(fps)))
    mem = [l for l in lines if l.startswith("[rt] gpu-memory t=")]
    if mem:
        out.append("memory:")
        out += ["  " + l[5:900] for l in mem[-2:]]
    gov = [l for l in lines if l.startswith("[gov] memory +")]
    if gov:
        out.append("  " + gov[-1][:420])
    work = [l for l in lines if l.startswith("[rt] d3d12-metal work t=")]
    if work:
        out.append("work report (per frame), first and last in the world:")
        ww = [l for l in work if world is not None and int(re.search(r"t=(\d+)s", l).group(1)) > world + 5]
        for l in (ww[:1] + ww[-1:]) if ww else work[-1:]:
            out.append("  " + l[5:2600])
    scene = [l for l in lines if l.startswith("[rt] d3d12-metal scene") and "probe:" not in l]
    if scene:
        out.append("scene probe:")
        out += ["  " + l[5:600] for l in scene[:160]]
    misc = [l for l in lines if re.search(r"lands in a malloc region|set aside|no proxy hooks|proxy hooks removed|"
                                          r"list cannot hold its scratch|list value write to .* is dropped", l)]
    if misc:
        out.append("guards:")
        out += ["  " + l[:300] for l in misc[:12]]
    out.append("  malloc-region list value writes: %d lines; blocks set aside: %d lines" % (
        sum("lands in a malloc region" in l for l in lines), sum("set aside (" in l for l in lines)))
    pages = [l for l in lines if l.startswith("[s3] page") or l.startswith("[s3] stage")]
    out += pages[:20]
    return out


def main():
    for name in sys.argv[1:]:
        print("\n".join(summarise(name)))


if __name__ == "__main__":
    sys.exit(main())
