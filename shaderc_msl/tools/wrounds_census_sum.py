#!/usr/bin/env python3
"""Exchange points of the world's translated shaders by cause, weighted by measured frame cost.

  wrounds_census_sum.py COST.tsv|PASSES.tsv CENSUS.jsonl [CENSUS2.jsonl ...] [--cs]

COST: a pass-timer TSV (GTA6RT_METAL_PASS_TIMING_TSV, mesh geometry ms a frame per shader, mspeed_passes_tsv.load)
or a two-column table NAME<tab>ms a frame (compute: build/cspeed-x86_64/notes/cs-b-table.md as cs-cost.tsv).
CENSUS: translator-fork tools/wround_census.py output (one line per shader, "points": {cause: n}).
Per census: static points summed over all shaders, and frame ms attributed to each cause (each shader's ms split by
its share of points: every exchange point of a shader counted as one round).
"""
import argparse
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

GROUPS = (
    ("v_readfirstlane", ("rfl",)),
    ("v_readlane", ("readlane",)),
    ("exec/vcc branch votes", ("branchvote",)),
    ("SCC/compare votes, masks as data", ("sccvote", "scmpvote", "maskdata", "wqm")),
    ("ds_swizzle", ("ds_lane:ds_swizzle_b32",)),
    ("ds_bpermute / ds_permute", ("ds_lane:ds_bpermute_b32", "ds_permute")),
    ("DPP (row/quad/DPP8; reduction and scan steps)", ("dpp",)),
    ("v_permlane(x)16", ("permlane",)),
    ("memory-order rounds (stores, LDS, atomics)", ("memorder",)),
    ("s_barrier", ("s_barrier",)),
    ("ds_append / other", ("ds_append", "other", "desc_lookup")),
)


def group_of(cause):
    for g, keys in GROUPS:
        for k in keys:
            if cause == k or cause.startswith(k + ":") or (":" in k and cause == k):
                return g
    return "ds_append / other"


def load_cost(path):
    if path.endswith(".tsv") and open(path).readline().startswith("window\t"):
        import mspeed_passes_tsv as MP
        agg, windows = MP.load(path)
        frames = windows * 3.0
        return {n: a["geo"] / frames for n, a in agg.items() if n}
    out = {}
    for line in open(path):
        f = line.rstrip("\n").split("\t")
        if len(f) >= 2 and f[0]:
            try:
                out[f[0].replace("library/", "")] = float(f[1])
            except ValueError:
                pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cost")
    ap.add_argument("census", nargs="+")
    a = ap.parse_args()
    cost = load_cost(a.cost)
    print("| census | shaders | weighted ms a frame (shaders with a cost) | " +
          " | ".join(g for g, _ in GROUPS) + " | all |")
    print("|---|---:|---:|" + "---:|" * (len(GROUPS) + 1))
    for path in a.census:
        pts = collections.Counter()
        ms = collections.Counter()
        n = 0
        wsum = 0.0
        for line in open(path):
            r = json.loads(line)
            n += 1
            name = r["name"].replace("library/", "")
            p = collections.Counter()
            for k, v in r.get("points", {}).items():
                p[group_of(k)] += v
            tot = sum(p.values())
            pts.update(p)
            w = cost.get(name, 0.0)
            if w and tot:
                wsum += w
                for g, v in p.items():
                    ms[g] += w * v / tot
        cells = []
        for g, _ in GROUPS:
            cells.append("%d / %.1f" % (pts[g], ms[g]) if pts[g] else "-")
        cells.append("%d / %.1f" % (sum(pts.values()), sum(ms.values())))
        print("| %s | %d | %.1f | %s |" % (os.path.basename(path), n, wsum, " | ".join(cells)))
    print("\ncells: static exchange points over all shaders / frame ms attributed (a shader's measured ms split by its "
          "points)")


if __name__ == "__main__":
    main()
