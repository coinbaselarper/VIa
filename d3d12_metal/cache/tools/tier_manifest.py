#!/usr/bin/env python3
"""A tier directory's pcache-tier.txt: what every Metal run that looks the tier up prebuilds (pcache.cpp start()).

  tier_manifest.py --manifest M [--manifest M ...] --world LIST.tsv --out DIR/pcache-tier.txt

Entries before the world: the union of the manifests (earliest phase, then earliest time). World entries: the
world_list.py order, t = seconds after the world began (pcache uses t within a phase as the expected first use, for
prebuild order and for releasing pipelines that were never used). A world key the list lacks keeps its manifest time
offset by the list's end.
"""
import argparse
import os
import sys

PHASES = ["boot", "landing", "movie", "first-run", "story-load", "world"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", action="append", required=True)
    ap.add_argument("--world", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    best = {}
    for m in a.manifest:
        with open(m) as f:
            for line in f:
                if line.startswith("#"):
                    continue
                p = line.rstrip("\n").split("\t")
                if len(p) < 5 or p[1] not in PHASES:
                    continue
                key, phase, t, runs, text = p[0], PHASES.index(p[1]), float(p[2]), int(p[3]), p[4]
                cur = best.get(key)
                if cur is None or (phase, t) < (cur[0], cur[1]):
                    best[key] = [phase, t, runs + (cur[2] if cur else 0), text]
                else:
                    cur[2] += runs
    world = PHASES.index("world")
    rel = {}
    with open(a.world) as f:
        next(f)
        for line in f:
            p = line.rstrip("\n").split("\t")
            rel[p[2]] = float(p[0])
    end = max(rel.values()) if rel else 0
    t0 = min((v[1] for v in best.values() if v[0] == world), default=0)
    rows = []
    missing = 0
    for key, (phase, t, runs, text) in best.items():
        if key in rel:
            phase, t = world, rel[key]
        elif phase == world:
            t = end + (t - t0)
        rows.append((phase, t, key, runs, text))
    for key in rel:
        if key not in best:
            missing += 1
    rows.sort()
    tmp = a.out + ".tmp"
    with open(tmp, "w") as f:
        f.write("# gta6rt pcache manifest 1\n# key\tphase\tt_s\truns\tpipeline\n")
        for phase, t, key, runs, text in rows:
            f.write("%s\t%s\t%.3f\t%d\t%s\n" % (key, PHASES[phase], t, runs, text))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, a.out)
    by = {}
    for phase, *_ in rows:
        by[PHASES[phase]] = by.get(PHASES[phase], 0) + 1
    print("%s: %d pipelines (%s); %d world-list keys without a manifest text" % (
        a.out, len(rows), ", ".join("%s %d" % (p, by[p]) for p in PHASES if p in by), missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
