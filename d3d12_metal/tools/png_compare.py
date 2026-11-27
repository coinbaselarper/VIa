#!/usr/bin/env python3
"""Compares present dumps of two runs (8-bit RGBA PNGs from present_dump.h).

  png_compare.py A.png B.png [...]     pairs: A against B, one line each

Per pair: both sizes, mean absolute RGB difference (0-255, over B's size; A is sampled nearest-neighbour when the sizes
differ), the share of pixels differing by more than 16 in any channel, and each image's mean RGB and lit share.
"""
import importlib.util
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "png_stats", os.path.join(here, "..", "..", "d3d12_vulkan", "tools", "png_stats.py"))
png_stats = importlib.util.module_from_spec(spec)
spec.loader.exec_module(png_stats)


def load(path):
    w, h, px = png_stats.read_png(path)
    return w, h, [px[y * w * 4:(y + 1) * w * 4] for y in range(h)]


def stats(w, h, rows):
    lit, s = 0, [0, 0, 0]
    for r in rows:
        for x in range(0, w * 4, 4):
            px = r[x:x + 3]
            if px[0] or px[1] or px[2]:
                lit += 1
            s[0] += px[0]
            s[1] += px[1]
            s[2] += px[2]
    n = w * h
    return [c / n for c in s], lit / n


def compare(a_path, b_path):
    aw, ah, ar = load(a_path)
    bw, bh, br = load(b_path)
    total, big = 0, 0
    step = 2
    count = 0
    for y in range(0, bh, step):
        ay = min(ah - 1, y * ah // bh)
        rb, ra = br[y], ar[ay]
        for x in range(0, bw, step):
            ax = min(aw - 1, x * aw // bw) * 4
            bx = x * 4
            d = max(abs(ra[ax] - rb[bx]), abs(ra[ax + 1] - rb[bx + 1]), abs(ra[ax + 2] - rb[bx + 2]))
            total += abs(ra[ax] - rb[bx]) + abs(ra[ax + 1] - rb[bx + 1]) + abs(ra[ax + 2] - rb[bx + 2])
            big += d > 16
            count += 1
    am, alit = stats(aw, ah, ar)
    bm, blit = stats(bw, bh, br)
    print(f"{os.path.basename(a_path)} {aw}x{ah} vs {os.path.basename(b_path)} {bw}x{bh}: "
          f"mean |d| {total / (3 * count):.2f}, {100.0 * big / count:.1f}% pixels off by >16; "
          f"A mean {am[0]:.1f} {am[1]:.1f} {am[2]:.1f} lit {100 * alit:.0f}%; "
          f"B mean {bm[0]:.1f} {bm[1]:.1f} {bm[2]:.1f} lit {100 * blit:.0f}%")


def main(argv):
    if len(argv) < 2 or len(argv) % 2:
        print(__doc__)
        return 2
    for i in range(0, len(argv), 2):
        compare(argv[i], argv[i + 1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
