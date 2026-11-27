#!/usr/bin/env python3
"""Where every module of an assembled corpus came from, read off the files themselves (port/shaders_vk).

  corpus_provenance.py CORPUS [--extra DIR ...] [--verify-stripped N]

assemble_corpus.py hard-links a module it takes from a re-translation and writes the stripped bytes of the others as
new files, so a shipped module is a re-translation exactly when it shares its inode with a .spv under one of the
manifest's --retranslated dirs (searched recursively, so a stopped shard's .stage folders count). Per part this prints
re-translated / stripped, checks the totals against the counts shaders_vk.json recorded, and exits 1 on a mismatch.
--extra DIR: .spv files of a re-translation that was not merged; a stripped module with a byte-identical one there is
counted as such. --verify-stripped N: recompute strip() of N random stripped modules (N < 0: all of them) from the
manifest's base and compare bytes with the shipped file.
"""
import argparse
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import strip_float_controls as sfc  # noqa: E402

PARTS = ["", "exe", "fspx", "bvh", "library"]


def spv_files(root):
    for d, _, files in os.walk(root):
        for f in files:
            if f.endswith(".spv"):
                yield os.path.join(d, f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--extra", action="append", default=[])
    ap.add_argument("--verify-stripped", type=int, default=0)
    a = ap.parse_args()
    corpus = os.path.abspath(a.corpus)
    with open(os.path.join(corpus, "shaders_vk.json")) as f:
        manifest = json.load(f)
    corpus_inodes = set()
    for part in PARTS:
        folder = os.path.join(corpus, part) if part else corpus
        for f in os.listdir(folder):
            if f.endswith(".spv"):
                st = os.stat(os.path.join(folder, f))
                corpus_inodes.add((st.st_dev, st.st_ino))
    linked_from = {}
    missing_dirs = []
    for d in manifest.get("retranslated", []):
        if not os.path.isdir(d):
            missing_dirs.append(d)
            continue
        for p in spv_files(d):
            st = os.stat(p)
            key = (st.st_dev, st.st_ino)
            if key in corpus_inodes:
                linked_from.setdefault(key, d)
    extra = {}
    for d in a.extra:
        for p in spv_files(d):
            extra.setdefault(os.path.basename(p)[:-4], p)

    mismatches = 0
    totals = {"modules": 0, "re-translated": 0, "stripped": 0, "stripped, identical re-translation not merged": 0,
              "linked, source dir gone": 0}
    stripped_names = []
    print(f"corpus {corpus}")
    if missing_dirs:
        print(f"  manifest dirs no longer on disk: {missing_dirs}")
    print(f"{'part':8} {'modules':>8} {'VS':>6} {'re-translated':>14} {'stripped':>9} {'(identical not merged)':>23}")
    for part in PARTS:
        folder = os.path.join(corpus, part) if part else corpus
        with open(os.path.join(folder, "index.json")) as f:
            index = json.load(f)
        n = vs = re = stripped = same_extra = gone = 0
        for name, meta in index.items():
            p = os.path.join(folder, name + ".spv")
            st = os.stat(p)
            n += 1
            vs += meta.get("stage") == "vs"
            if (st.st_dev, st.st_ino) in linked_from:
                re += 1
            elif st.st_nlink > 1 and missing_dirs:
                re += 1
                gone += 1
            else:
                stripped += 1
                stripped_names.append((part, name))
                e = extra.get(name)
                if e:
                    with open(e, "rb") as f1, open(p, "rb") as f2:
                        same_extra += f1.read() == f2.read()
        counts = manifest["parts"][part or "main"]["counts"]
        want_stripped = counts.get("stripped", 0) + counts.get("re-translation differs, stripped used", 0)
        want_re = n - want_stripped
        ok = re == want_re and stripped == want_stripped
        mismatches += not ok
        print(f"{part or 'main':8} {n:>8} {vs:>6} {re:>14} {stripped:>9} {same_extra:>23}"
              + ("" if ok else f"   MISMATCH: manifest says {want_re} re-translated, {want_stripped} stripped"))
        totals["modules"] += n
        totals["re-translated"] += re
        totals["stripped"] += stripped
        totals["stripped, identical re-translation not merged"] += same_extra
        totals["linked, source dir gone"] += gone
    print("total: " + ", ".join(f"{v} {k}" for k, v in totals.items()))

    if a.verify_stripped:
        base = manifest["base"]
        sample = stripped_names if a.verify_stripped < 0 else \
            random.Random(1).sample(stripped_names, min(a.verify_stripped, len(stripped_names)))
        bad = 0
        for part, name in sample:
            src = os.path.join(base, part, name + ".spv") if part else os.path.join(base, name + ".spv")
            dst = os.path.join(corpus, part, name + ".spv") if part else os.path.join(corpus, name + ".spv")
            with open(src, "rb") as f1, open(dst, "rb") as f2:
                bad += sfc.strip(f1.read()) != f2.read()
        print(f"verify-stripped: {len(sample)} checked, {bad} differ from strip(base)")
        mismatches += bad != 0
    print("provenance: " + ("matches the manifest" if not mismatches else f"{mismatches} mismatches"))
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
