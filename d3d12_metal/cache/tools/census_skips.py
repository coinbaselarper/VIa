#!/usr/bin/env python3
"""Draws and dispatches skipped because their pipeline is still compiling, per 10 s census window of the adapter
("[rt] d3d12-metal work t=Ts (last N s, P presents; per frame): class C (recorded R, pipeline compiling K, ...)").

  census_skips.py LOG [WORLD_T]

Prints per window: presents, frame rate, commands per frame, recorded per frame, skipped-compiling per frame and in
all (per frame x presents), and the window's time relative to WORLD_T (the recorder's world phase change, read from
the log when not given)."""
import re
import sys


def main():
    log = sys.argv[1]
    text = open(log, errors="replace").read().splitlines()
    world = float(sys.argv[2]) if len(sys.argv) > 2 else None
    if world is None:
        for l in text:
            m = re.search(r"pcache: phase .* -> world at t=([\d.]+)s", l)
            if m:
                world = float(m.group(1))
                break
    print("t_s\tworld_rel\tpresents\tframes/s\tcommands/frame\trecorded/frame\tcompiling/frame\tcompiling_in_window")
    for l in text:
        m = re.match(r"\[rt\] d3d12-metal work t=(\d+)s \(last (\d+) s, (\d+) presents; per frame\): (.*)", l)
        if not m:
            continue
        t, span, presents, body = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
        total = rec = comp = 0.0
        for cls in re.finditer(r"([a-z0-9 -]+?) ([\d.]+) \(([^)]*)\)", body):
            total += float(cls.group(2))
            for part in cls.group(3).split(", "):
                pm = re.match(r"(.*) ([\d.]+)$", part.strip())
                if not pm:
                    continue
                if pm.group(1) == "recorded":
                    rec += float(pm.group(2))
                elif pm.group(1) == "pipeline compiling":
                    comp += float(pm.group(2))
        rel = "%.0f" % (t - world) if world is not None else "-"
        print("%d\t%s\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.0f" % (t, rel, presents, presents / span, total, rec, comp,
                                                         comp * presents))
    return 0


if __name__ == "__main__":
    sys.exit(main())
