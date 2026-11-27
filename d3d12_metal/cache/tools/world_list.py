#!/usr/bin/env python3
"""The pipelines the world phase uses in its first N seconds, in first-use order, from the recorder's per-run files.

  world_list.py --runs DIR [--runs DIR ...] --manifest M [--manifest M ...] [--seconds 60] [--out PREFIX]

Each pcache-runs/<time>-<pid>.tsv lists first uses (t, phase, key, how, label) and waits (t, "wait", key, ms, from).
A run's world starts at its first first-use in phase "world". A pipeline's time is its earliest first use after the
world began, over all runs that reached it (seconds since that run's world start); runs that ended earlier simply do
not see the later ones, so the list's tail comes from the longest runs. Writes PREFIX.tsv (rel_s, runs, key, label,
how in the run that saw it first) and PREFIX.specs (phase<TAB>text, prepare_shaders --specs input, first-use order).
"""
import argparse
import glob
import os
import sys


def load_manifest(path, texts):
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 5:
                texts.setdefault(parts[0], parts[4])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", action="append", required=True)
    ap.add_argument("--manifest", action="append", required=True)
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--out", default="world-first")
    a = ap.parse_args()
    texts = {}
    for m in a.manifest:
        load_manifest(m, texts)
    best = {}
    seen_runs = {}
    run_spans = []
    for d in a.runs:
        for path in sorted(glob.glob(os.path.join(d, "*.tsv"))):
            rows = []
            with open(path) as f:
                for line in f:
                    p = line.rstrip("\n").split("\t")
                    if len(p) >= 5 and p[1] != "wait":
                        rows.append((float(p[0]), p[1], p[2], p[3], p[4]))
            world = [r for r in rows if r[1] == "world"]
            if not world:
                continue
            t0 = world[0][0]
            run_spans.append((os.path.basename(path), t0, world[-1][0] - t0, len(world)))
            for t, phase, key, how, label in world:
                rel = t - t0
                seen_runs[key] = seen_runs.get(key, 0) + 1
                if key not in best or rel < best[key][0]:
                    best[key] = (rel, label, how)
    order = sorted(((v[0], k, v[1], v[2]) for k, v in best.items() if v[0] <= a.seconds), key=lambda x: x[0])
    missing = [k for _, k, _, _ in order if k not in texts]
    with open(a.out + ".tsv", "w") as f:
        f.write("rel_s\truns\tkey\tlabel\thow\n")
        for rel, k, label, how in order:
            f.write("%.3f\t%d\t%s\t%s\t%s\n" % (rel, seen_runs[k], k, label, how))
    with open(a.out + ".specs", "w") as f:
        f.write("# world phase, first %g s, first-use order (world_list.py)\n" % a.seconds)
        for rel, k, label, how in order:
            if k in texts:
                f.write("world\t%s\n" % texts[k])
    for name, t0, span, n in run_spans:
        print("run %s: world at %.1f s, first uses over %.1f s of it: %d" % (name, t0, span, n))
    buckets = {}
    for rel, *_ in order:
        b = 0 if rel < 1 else 1 if rel < 5 else 2 if rel < 10 else 3 if rel < 30 else 4
        buckets[b] = buckets.get(b, 0) + 1
    names = ["<1 s", "1-5 s", "5-10 s", "10-30 s", "30-%g s" % a.seconds]
    print("world pipelines in the first %g s: %d (%s); %d without a manifest text" % (
        a.seconds, len(order), ", ".join("%s %d" % (names[b], buckets.get(b, 0)) for b in range(5)), len(missing)))
    kinds = {}
    for _, k, _, _ in order:
        t = texts.get(k, "?")[:1]
        kinds[t] = kinds.get(t, 0) + 1
    print("by kind: " + ", ".join("%s %d" % (k, v) for k, v in sorted(kinds.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
