#!/usr/bin/env python3
"""Submission phases of a title run, from its `[rt] submit phases t=Ns` lines.

usage: phase_summary.py LOG [NAME=FROM:TO ...]

Prints every 5 s line (presented frames/s, ms per submission by phase, share of the wall time) with the run's events
(first lit frame, pad presses, BinkRegisterFrameBuffers, the movie's frame buffers), then for each NAME=FROM:TO window
(seconds since start, inclusive of the lines whose t falls in it) the submission-weighted mean of every phase and the
mean presented frames/s.
"""
import re
import sys

PHASE = re.compile(r"\[rt\] submit phases t=(\d+)s: (\d+) submissions in ([\d.]+) s .*?, ([\d.]+) frames/s; ms per "
                   r"submission \(max\): wait ([\d.]+) \(\d+\) heaps ([\d.]+) \((\d+)\) shadows ([\d.]+) \((\d+)\) "
                   r"geometry ([\d.]+) \(\d+\) textures ([\d.]+) \((\d+)\) replay ([\d.]+) \((\d+)\); (\d+)% of the wall")
EVENTS = [
    (re.compile(r"first non-black frame (\d+) at t=([\d.]+)s"), lambda m: (float(m.group(2)), f"first lit frame {m.group(1)}")),
    (re.compile(r"gameinput: press (\w+)@([\d.]+)"), lambda m: (float(m.group(2)), f"press {m.group(1)}")),
    (re.compile(r"device was lost"), lambda m: (None, "DEVICE LOST")),
]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    rows, events = [], []
    last_t = 0
    with open(sys.argv[1], errors="replace") as f:
        for line in f:
            m = PHASE.search(line)
            if m:
                g = m.groups()
                last_t = int(g[0])
                rows.append(dict(t=int(g[0]), n=int(g[1]), fps=float(g[3]), heaps=float(g[5]), heaps_max=int(g[6]),
                                 shadows=float(g[7]), textures=float(g[10]), replay=float(g[12]),
                                 replay_max=int(g[13]), wall=int(g[14])))
                continue
            if "BinkRegisterFrameBuffers(" in line:
                events.append((last_t, "BinkRegisterFrameBuffers"))
            if "movie frame buffers at" in line:
                events.append((last_t, line.split("[rt] d3d12: ")[-1].strip()[:90]))
            for pattern, make in EVENTS:
                e = pattern.search(line)
                if e:
                    t, what = make(e)
                    events.append((t if t is not None else last_t, what))
    print(f"{'t':>4} {'subm':>5} {'fps':>6} {'heaps':>6} {'shadows':>7} {'textures':>8} {'replay':>7} {'wall%':>5}")
    for r in rows:
        print(f"{r['t']:>4} {r['n']:>5} {r['fps']:>6.1f} {r['heaps']:>6.2f} {r['shadows']:>7.2f} {r['textures']:>8.2f} "
              f"{r['replay']:>7.2f} {r['wall']:>5}")
    for t, what in events:
        print(f"  event ~{t:.1f}s: {what}")
    for spec in sys.argv[2:]:
        name, span = spec.split("=")
        lo, hi = (float(x) for x in span.split(":"))
        sel = [r for r in rows if lo <= r["t"] <= hi]
        if not sel:
            print(f"{name}: no lines in {lo}-{hi} s")
            continue
        n = sum(r["n"] for r in sel)
        mean = {k: sum(r[k] * r["n"] for r in sel) / n for k in ("heaps", "shadows", "textures", "replay")}
        fps = sum(r["fps"] for r in sel) / len(sel)
        print(f"{name} ({lo:.0f}-{hi:.0f} s, {len(sel)} lines, {n} submissions): {fps:.1f} frames/s; ms per submission "
              f"heaps {mean['heaps']:.2f} shadows {mean['shadows']:.2f} textures {mean['textures']:.2f} "
              f"replay {mean['replay']:.2f} (sum {sum(mean.values()):.2f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
