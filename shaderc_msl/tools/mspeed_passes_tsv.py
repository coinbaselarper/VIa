#!/usr/bin/env python3
"""Per mesh shader GPU time from pass-timer TSVs (GTA6RT_METAL_PASS_TIMING_TSV with _SPLIT=1: one render segment per
pipeline): geometry (vertex-stage) ms, mesh groups, us a group, per timed frame.

  mspeed_passes_tsv.py A.tsv [B.tsv] [--frames N] [--top K] [--names FILE]
With B: side by side, A's top K by geometry time.
"""
import argparse
import collections
import csv


def load(path, stage="geometry"):
    agg = collections.defaultdict(lambda: {"seg": 0, "geo": 0.0, "ms": 0.0, "groups": 0})
    windows = set()
    for r in csv.DictReader(open(path), delimiter="\t"):
        if r["kind"] != "R" or r["first"] != r["last"]:
            continue
        if int(r["groups"] or 0) > 0:
            windows.add(r["window"])
        geo = float(r.get("vertex_ms") or -1)
        if geo < 0:
            continue
        name = r["first"].split(",")[0].replace(" (pcache)", "")
        a = agg[name]
        a["seg"] += 1
        a["geo"] += geo
        a["ms"] += float(r["ms"])
        a["groups"] += int(r["groups"] or 0)
    return agg, len(windows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b", nargs="?")
    ap.add_argument("--frames", type=float, default=0)
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--per-window", type=float, default=3.0, help="presented frames a timing window")
    a = ap.parse_args()
    A, wa = load(a.a)
    B, wb = load(a.b) if a.b else ({}, 0)
    fa = a.frames or a.per_window * wa
    fb = a.frames or a.per_window * max(wb, 1)
    us = lambda x: x["geo"] * 1e3 / x["groups"] if x["groups"] else 0.0
    print("A %s: %d windows; B %s: %d windows (per frame: windows x 3)" % (a.a, wa, a.b, wb))
    print("| # | mesh shader | A geometry ms/frame | A groups/frame | A us/group | B geometry ms/frame | B groups/frame | B us/group | B/A per group |")
    print("|---:|---|---:|---:|---:|---:|---:|---:|---:|")
    tot_a = tot_b = 0.0
    for i, (n, x) in enumerate(sorted(A.items(), key=lambda kv: -kv[1]["geo"])[:a.top]):
        y = B.get(n)
        tot_a += x["geo"] / fa
        cells = ["%.1f" % (x["geo"] / fa), "%.0f" % (x["groups"] / fa), "%.2f" % us(x)]
        if y:
            tot_b += y["geo"] / fb
            r = us(y) / us(x) if us(x) else 0
            cells += ["%.1f" % (y["geo"] / fb), "%.0f" % (y["groups"] / fb), "%.2f" % us(y), "%.2f" % r]
        else:
            cells += ["-", "-", "-", "-"]
        print("| %d | `%s` | %s |" % (i + 1, n, " | ".join(cells)))
    all_a = sum(x["geo"] for x in A.values()) / fa
    all_b = sum(x["geo"] for x in B.values()) / fb if B else 0
    print("\nsum of the top %d: A %.1f ms, B %.1f ms a frame; all single-pipeline render segments' geometry: A %.1f, B %.1f"
          % (a.top, tot_a, tot_b, all_a, all_b))


if __name__ == "__main__":
    main()
