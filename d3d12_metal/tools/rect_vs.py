#!/usr/bin/env python3
"""Overlay rows for rect-list vertex shaders that carry their MSL source: the store's metallib, copied, with the recipe's
MSL beside it as <key>.msl (d3d12_metal/src/metal_shaders.cpp loads such a library from the text, so port/metal can
build its rect wrapper). The MSL is regenerated with the store's own recipe and must have the recorded size.

  rect_vs.py OUT [--names-file F] [REGEX...]

Moved from the exposure worker's build tree (build/expo-x86_64/tools/rect_vs.py, round 12); the generator of
build/expo-x86_64/rectvs/manifest.tsv. Only change: imports from this directory, and --names-file for exact lists.
"""
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mesh_tg_pack as P  # noqa: E402
import msl_instrument as M  # noqa: E402

R = P.R


def main():
    args = sys.argv[1:]
    exact = set()
    if "--names-file" in args:
        i = args.index("--names-file")
        exact = {l.strip() for l in open(args[i + 1]) if l.strip() and not l.startswith("#")}
        del args[i:i + 2]
    out_dir = os.path.abspath(args[0])
    pats = [re.compile(p) for p in args[1:]]
    head, rows = P.manifest_rows()
    names = sorted(n for n, f in rows.items() if f[2] == "vs" and (n in exact or any(p.search(n) for p in pats)))
    kept, skipped = [], []
    for name in names:
        row = rows[name]
        msl, why = M.regenerate(name, row, "vs", os.path.join(out_dir, "tmp"))
        if msl is None:
            skipped.append((name, why))
            continue
        key = row[1]
        d = os.path.join(out_dir, "cas", key[:2])
        os.makedirs(d, exist_ok=True)
        src_lib = os.path.join(P.STORE_ROOT, row[4])
        lib = os.path.join(d, key + ".metallib")
        shutil.copyfile(src_lib, lib)
        open(os.path.join(d, key + ".msl"), "w").write(msl)
        out = list(row)
        out[4] = os.path.relpath(lib, P.STORE_ROOT)
        kept.append(out)
    rel_root = os.path.relpath(P.STORE_ROOT, out_dir)
    with open(os.path.join(out_dir, "manifest.tsv"), "w") as f:
        for h in head:
            f.write(re.sub(r"root=\S+", "root=" + rel_root, h) if "root=" in h else h)
        for r in kept:
            f.write("\t".join(r) + "\n")
    print("rows %d, skipped %d" % (len(kept), len(skipped)))
    for n, w in skipped:
        print("skipped", n, w)


main()
