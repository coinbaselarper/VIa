#!/usr/bin/env python3
"""Tier spec lists for prepare_shaders: the boot-set specs (boot_set.py) whose pipeline names the bootstrap file puts
in a phase (census "still building" names of S3 runs), one file per phase, plus the exact manifest entries.

  tiers.py --specs census.specs --bootstrap pcache-bootstrap.txt --out DIR
writes DIR/tier-<phase>.specs and prints the counts.
"""
import argparse, collections, os, re, sys

PHASES = ["boot", "landing", "movie", "first-run", "story-load", "world"]
FIELD = re.compile(r" (vs|ps|ms|cs)=(\S+)")


def unesc(s):
    return re.sub(r"~([0-9a-f]{2})", lambda m: chr(int(m.group(1), 16)), s)


def label(text):
    names = {}
    for k, v in FIELD.findall(text):
        names[k] = "" if v == "-" else unesc(v.rsplit("@", 1)[0])
    first = names.get("vs") or names.get("ms") or names.get("cs") or ""
    return first + ("," + names["ps"] if names.get("ps") else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--specs", action="append", required=True)
    ap.add_argument("--bootstrap", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    phase_of = {}
    for line in open(a.bootstrap):
        p = line.rstrip("\n").split("\t")
        if len(p) == 3 and p[0] == "L":
            phase_of[p[2]] = p[1]
    tiers = collections.defaultdict(list)
    seen_labels = collections.defaultdict(set)
    for path in a.specs:
        for line in open(path):
            if line.startswith("#") or "\t" not in line:
                continue
            text = line.rstrip("\n").split("\t", 1)[1]
            lab = label(text)
            ph = phase_of.get(lab)
            if ph:
                tiers[ph].append(text)
                seen_labels[ph].add(lab)
    os.makedirs(a.out, exist_ok=True)
    want = collections.Counter(phase_of.values())
    for ph in PHASES:
        if not tiers.get(ph):
            continue
        with open(os.path.join(a.out, f"tier-{ph}.specs"), "w") as f:
            for t in tiers[ph]:
                f.write(f"{ph}\t{t}\n")
        print(f"tier {ph}: {len(tiers[ph])} specs for {len(seen_labels[ph])} of {want[ph]} bootstrap names")


if __name__ == "__main__":
    sys.exit(main())
