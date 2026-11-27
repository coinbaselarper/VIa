#!/usr/bin/env python3
"""One line per Metal title run for STATUS-cache.md: load, stages, first lit frame, the pipeline cache's last report,
the archive line and the footprint over time.

  pcache_summary.py LOG...
"""
import re, sys

STAGE = re.compile(r"\[host\] stage t=([\d.]+)s: (.*?) \(threads")
LIT = re.compile(r"first non-black frame (\d+) at t=([\d.]+)s")
FOOT = re.compile(r"\[gov\] memory \+(\d+)s .*?footprint (\d+) MiB")
LOAD = re.compile(r"load \{ ([\d.]+) ([\d.]+) ([\d.]+) \}")
REPORT = re.compile(r"\[rt\] pcache: (t=.*)")
ARCH = re.compile(r"\[rt\] d3d12-metal: pipeline archive (/\S+): (\w+), (loaded|new) \((\d+) bytes, ([\d.]+) s\)")


def summary(path):
    out = {"log": path, "stages": [], "foot": []}
    loads = []
    for line in open(path, errors="replace"):
        m = STAGE.search(line)
        if m:
            out["stages"].append((float(m.group(1)), m.group(2)))
        m = LIT.search(line)
        if m and "lit" not in out:
            out["lit"] = (int(m.group(1)), float(m.group(2)))
        m = FOOT.search(line)
        if m:
            out["foot"].append((int(m.group(1)), int(m.group(2))))
        if line.startswith("run.sh:"):
            m = LOAD.search(line)
            if m:
                loads.append(float(m.group(1)))
            if "rc=" in line or "killed" in line:
                out["end"] = line.strip()
        m = REPORT.search(line)
        if m and "first uses" in line:
            out["report"] = m.group(1)
        m = ARCH.search(line)
        if m:
            out["archive"] = f"{m.group(3)} {int(m.group(4)) / 1048576:.1f} MiB in {m.group(5)} s"
    out["load"] = loads
    return out


def main():
    for p in sys.argv[1:]:
        s = summary(p)
        landing = next((t for t, n in s["stages"] if n.startswith("landing")), None)
        lit = s.get("lit")
        print(f"== {p}")
        print(f"   load start/end {s['load']}; archive {s.get('archive', '-')}; landing stage {landing} s; "
              f"first lit {'frame %d at %.1f s' % lit if lit else '-'}; end: {s.get('end', '-')}")
        print("   stages: " + ", ".join(f"{n} {t:.1f}" for t, n in s["stages"]))
        if s["foot"]:
            print("   footprint MiB: " + ", ".join(f"+{t}s {f}" for t, f in s["foot"][::2]))
        print(f"   pcache: {s.get('report', '-')}")


if __name__ == "__main__":
    main()
