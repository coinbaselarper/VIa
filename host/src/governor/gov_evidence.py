#!/usr/bin/env python3
"""Governor evidence for a title run log: prints "[s3] gov ..." lines (append them to the log like s3_evidence.py).

usage: gov_evidence.py LOG [--append]

Reads the governor's "[gov] ..." lines: the per-category table at each stage change (the [s3] evidence points), the
one-line memory accounting at each interval, level changes, actions with the bytes they recovered, warnings and the
governor's own CPU time.
"""
import re
import sys

LINE = re.compile(r"^\[gov\] memory \+(\d+)s \((.*?)\): footprint (\d+) MiB \(budget (\d+), (\d+)%\), resident (\d+), "
                  r"compressed (\d+).*?pool (\d+) committed, (\d+) touched.*?gpu metal (\d+) of (\d+) working set, "
                  r"graphics ledger (\d+).*?all processes (\d+) allocated.*?layer vk (\d+) in (\d+) \(title copies (\d+), "
                  r"render targets (\d+).*?evictable (\d+), failed allocations (\d+)\).*?malloc (\d+) in use.*?"
                  r"machine: pressure (\w+), available (\d+)%, swap (\d+) of (\d+), free\+speculative (\d+), wired (\d+).*?"
                  r"load ([\d.]+); level (\d)")
TABLE_HEAD = re.compile(r"^\[gov\] table \+(\d+)s at (.*?): footprint")
TABLE_ROW = re.compile(r"^\[gov\] table   (.*)$")
ACTION = re.compile(r"^\[gov\] action (\S+ \S+) \(level (\d)\): (.*)$")
OVERHEAD = re.compile(r"^\[gov\] overhead over (.*)$")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    out = []
    lines = open(path, errors="replace").read().splitlines()
    mems = []
    for l in lines:
        m = LINE.match(l)
        if m:
            g = m.groups()
            mems.append(g)
    current = None
    for l in lines:
        h = TABLE_HEAD.match(l)
        if h:
            current = "[s3] gov table +%ss at %s:" % (h.group(1), h.group(2))
            out.append(current + " " + l.split(": ", 1)[1])
            continue
        r = TABLE_ROW.match(l)
        if r and current:
            out.append("[s3] gov table   " + r.group(1))
            continue
        a = ACTION.match(l)
        if a:
            out.append("[s3] gov action %s (level %s): %s" % a.groups())
            continue
        if l.startswith("[gov] WARNING") or l.startswith("[gov] level ") or l.startswith("[gov] OS memory pressure event"):
            out.append("[s3] gov " + l[6:])
            continue
        o = OVERHEAD.match(l)
        if o:
            out.append("[s3] gov overhead over " + o.group(1))
    if mems:
        peak = max(mems, key=lambda g: int(g[2]))
        out.append("[s3] gov peak footprint %s MiB at +%ss (budget %s MiB); pool %s committed / %s touched; gpu metal %s of %s "
                   "working set; layer vk %s MiB (title copies %s, render targets %s, evictable %s, failed allocations %s); "
                   "machine pressure %s, available %s%%, swap %s of %s MiB, load %s"
                   % (peak[2], peak[0], peak[3], peak[7], peak[8], peak[9], peak[10], peak[13], peak[15], peak[16], peak[17],
                      peak[18], peak[20], peak[21], peak[22], peak[23], peak[26]))
        for g in mems:
            out.append("[s3] gov memory +%ss: footprint %s (%s%% of %s), pool touched %s, gpu metal %s, layer vk %s, "
                       "malloc %s; pressure %s, swap %s, free+spec %s, wired %s, load %s; level %s"
                       % (g[0], g[2], g[4], g[3], g[8], g[9], g[13], g[19], g[20], g[22], g[24], g[25], g[26], g[27]))
    text = "\n".join(out)
    if "--append" in sys.argv:
        with open(path, "a") as f:
            f.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
