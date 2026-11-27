#!/usr/bin/env python3
"""Frame rate per compile-load step: run/logs/NAME.load.tsv (compile_load.sh) against the "submit phases" lines.

  load_fps.py NAME

The log's first line ("run.sh: start HH:MM:SS") gives the process start; a "submit phases t=Ts" line covers the 5 s
before T. A window is counted in a step when it lies inside the step (the first 5 s after a step change are skipped,
so a window never mixes two steps)."""
import datetime
import os
import re
import sys

PORT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def main():
    name = sys.argv[1]
    log = os.path.join(PORT, "run", "logs", name + ".log")
    steps = os.path.join(PORT, "run", "logs", name + ".load.tsv")
    lines = open(log, errors="replace").read().splitlines()
    m = re.match(r"run\.sh: start (\d+):(\d+):(\d+)", lines[0])
    mtime = datetime.datetime.fromtimestamp(os.path.getmtime(log))
    start = mtime.replace(hour=int(m.group(1)), minute=int(m.group(2)), second=int(m.group(3)), microsecond=0)
    if start > mtime:
        start -= datetime.timedelta(days=1)
    t0 = start.timestamp()
    windows = []
    for l in lines:
        m = re.match(r"\[rt\] submit phases t=(\d+)s: \d+ submissions in ([\d.]+) s .*?, ([\d.]+) frames/s", l)
        if m:
            end = t0 + float(m.group(1))
            windows.append((end - float(m.group(2)), end, float(m.group(3))))
    print("step\tjobs\tqos\twindows\tframes/s (mean)\tmin\tmax")
    for row in open(steps).read().splitlines()[1:]:
        step, jobs, qos, a, b = row.split("\t")
        a, b = float(a), float(b)
        inside = [w[2] for w in windows if w[0] >= a + 1 and w[1] <= b + 1]
        if inside:
            print("%s\t%s\t%s\t%d\t%.2f\t%.2f\t%.2f" % (step, jobs, qos, len(inside), sum(inside) / len(inside),
                                                    min(inside), max(inside)))
        else:
            print("%s\t%s\t%s\t0\t-\t-\t-" % (step, jobs, qos))
    return 0


if __name__ == "__main__":
    sys.exit(main())
