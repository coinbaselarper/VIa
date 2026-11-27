#!/usr/bin/env python3
"""One shader catalog = a base manifest with overlay rows replacing (or adding) rows by name, as MetalShaders does.

  catalog_merge.py --base M --overlay O [--overlay O ...] --out F

All inputs must share the store's root; F's root line is rewritten to point there (prepare_shaders --catalog F).
"""
import argparse
import os
import sys


def rows(path):
    head, body = [], []
    with open(path) as f:
        for line in f:
            if line.startswith("#") and "\t" not in line:
                head.append(line)
            elif line.strip():
                body.append(line)
    return head, body


def root_of(path, head):
    for h in head:
        if "root=" in h:
            return os.path.realpath(os.path.join(os.path.dirname(path), h.split("root=", 1)[1].split()[0]))
    sys.exit("%s: no root line" % path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--overlay", action="append", default=[])
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    head, body = rows(a.base)
    root = root_of(a.base, head)
    order = [l.split("\t", 1)[0] for l in body]
    by = {l.split("\t", 1)[0]: l for l in body}
    for o in a.overlay:
        oh, ob = rows(o)
        if root_of(o, oh) != root:
            sys.exit("%s: root %s is not the base's %s" % (o, root_of(o, oh), root))
        rep = add = 0
        for l in ob:
            n = l.split("\t", 1)[0]
            if n in by:
                rep += 1
            else:
                add += 1
                order.append(n)
            by[n] = l
        print("%s: %d rows replace, %d added" % (o, rep, add))
    rel = os.path.relpath(root, os.path.dirname(os.path.abspath(a.out)))
    head = [h.replace(h.split("root=", 1)[1].split()[0], rel, 1) if "root=" in h else h for h in head]
    with open(a.out + ".tmp", "w") as f:
        f.writelines(head)
        f.writelines(by[n] for n in order)
    os.rename(a.out + ".tmp", a.out)
    print("%s: %d rows" % (a.out, len(order)))


if __name__ == "__main__":
    main()
