#!/usr/bin/env python3
"""Compare mesh outputs captured by mspeed.py capture variants (mspeed_probe .out files).

  mspeed_cmp.py RECORD_WORDS A.out B.out...
Per group: mesh sizes; per vertex t < V: position and attributes; per primitive t < P: indices and cull flag.
Prints groups, vertices, primitives compared and the words that differ (bit-exact).
"""
import struct
import sys


def groups(path, rec):
    raw = open(path, "rb").read()
    w = struct.unpack("<%dI" % (len(raw) // 4), raw)
    per = 129 * rec
    for g in range(len(w) // per):
        yield w[g * per:(g + 1) * per]


def compare(rec, a, b):
    attrs = rec - 8
    stats = {"groups": 0, "vertices": 0, "primitives": 0, "differ": 0, "size_differ": 0, "nonzero_vertex_words": 0}
    first = []
    for ga, gb in zip(groups(a, rec), groups(b, rec)):
        if ga[2] != 0x5EED5EED:
            continue
        stats["groups"] += 1
        if ga[0:2] != gb[0:2]:
            stats["size_differ"] += 1
            continue
        nv, np_ = min(ga[0], 128), min(ga[1], 128)
        for t in range(128):
            ra = ga[(1 + t) * rec:(2 + t) * rec]
            rb = gb[(1 + t) * rec:(2 + t) * rec]
            if t < nv:
                stats["vertices"] += 1
                va, vb = ra[:4 + attrs], rb[:4 + attrs]
                stats["nonzero_vertex_words"] += sum(1 for x in va if x)
                d = sum(1 for x, y in zip(va, vb) if x != y)
                stats["differ"] += d
                if d and len(first) < 8:
                    first.append(("v", t, [(i, hex(x), hex(y)) for i, (x, y) in enumerate(zip(va, vb)) if x != y][:4]))
            if t < np_:
                stats["primitives"] += 1
                pa, pb = ra[4 + attrs:], rb[4 + attrs:]
                d = sum(1 for x, y in zip(pa, pb) if x != y)
                stats["differ"] += d
                if d and len(first) < 8:
                    first.append(("p", t, list(zip(pa, pb))))
    return stats, first


if __name__ == "__main__":
    rec = int(sys.argv[1])
    for b in sys.argv[3:]:
        s, f = compare(rec, sys.argv[2], b)
        print(b, s, f[:3] if f else "")
