#!/usr/bin/env python3
"""Render scratch census rows for tmem_probe rlist, with every shader resolved through an overlay list (first wins)
over the store manifest, as a world run loads it.

  wired2_rlist.py --base RLIST.tsv --out OUT.tsv [--overlays a.tsv:b.tsv:...] [--kinds ps,ms,vs] [--only NAME,...]

RLIST.tsv is a tmem_render.py list (its vertex functions and formats are kept); only the libraries change. A name no
manifest lists keeps its row. One line on stderr says how many rows each manifest won.
"""
import argparse
import os
import re
import sys

PORT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAIN = os.path.join(PORT, "build", "shaderc_msl", "runs", "main", "manifest.tsv")
DEFAULT = ["build/mspeed-x86_64/overlay.tsv", "build/expo-x86_64/rectvs/manifest.tsv", "build/gpass-x86_64/mop/overlay.tsv",
           "build/tmem-x86_64/overlay.tsv", "build/mcc-x86_64/overlay.tsv", "build/d3d12_metal-msl-overrides/manifest.tsv",
           "build/uintps-x86_64/overlay.tsv"]


def dec(t):
    return re.sub(r"~([0-9a-f]{2})", lambda m: chr(int(m.group(1), 16)), t)


def manifest_root(path):
    with open(path) as f:
        head = f.readline()
    m = re.search(r"root=(\S+)", head)
    return os.path.normpath(os.path.join(os.path.dirname(path), m.group(1) if m else "."))


def resolve(paths):
    won = {}
    for ov in paths:
        if not os.path.exists(ov):
            print("missing manifest %s" % ov, file=sys.stderr)
            continue
        root = manifest_root(ov)
        for line in open(ov):
            if (line.startswith("#") and "\t" not in line) or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) < 5 or f[0] in won:
                continue
            won[f[0]] = (ov, root, f)
    return won


def lib(root, p):
    return p if p in ("", "-") or os.path.isabs(p) else os.path.normpath(os.path.join(root, p))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--overlays", default="")
    ap.add_argument("--kinds", default="ps,ms,vs")
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    ovs = [os.path.join(PORT, o) for o in DEFAULT] if not a.overlays else a.overlays.split(":")
    won = resolve(ovs + [MAIN])
    kinds = set(a.kinds.split(","))
    only = set(a.only.split(",")) if a.only else None
    by, out = {}, []
    for line in open(a.base):
        f = line.rstrip("\n").split("\t")
        if f[0] not in kinds:
            continue
        name = dec(f[-1])
        if only is not None and name not in only:
            continue
        w = won.get(name)
        if w:
            ov, root, r = w
            route = r[10] if len(r) > 10 else "-"
            if f[0] == "ms" and route == "split64" and len(r) > 12 and r[11] not in ("", "-"):
                f[1], f[2] = lib(root, r[11]), lib(root, r[12])
            elif f[0] == "ms":
                f[1], f[2] = lib(root, r[4]), "-"
            else:
                f[1] = lib(root, r[4])
            by[ov] = by.get(ov, 0) + 1
        else:
            by["(none)"] = by.get("(none)", 0) + 1
        out.append("\t".join(f))
    with open(a.out, "w") as o:
        o.write("\n".join(out) + "\n")
    for k, v in sorted(by.items(), key=lambda kv: -kv[1]):
        print("%6d %s" % (v, os.path.relpath(k, PORT) if k != "(none)" else k), file=sys.stderr)


if __name__ == "__main__":
    main()
