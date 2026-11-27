#!/usr/bin/env python3
"""Speckle count on a scene-probe readback (GTA6RT_METAL_SCENE_RAW=1 .raw beside the PNG; RGBA16F, DXGI 10):
non-finite texels, exact-zero texels, and isolated dark texels (luminance under RATIO x the median of the 8
neighbours while the second-darkest neighbour is not dark: a single-texel dot), over the frame or a box; exposure
independent. Optional PNG of a crop, tone mapped at the frame's median luminance.

  speckle_dots.py RAW WIDTH HEIGHT [--box x0,y0,x1,y1] [--ratio 0.25] [--png OUT --crop x,y,w,h --scale 3]
"""
import argparse
import math
import os
import struct
import sys
import zlib


def read_rgba16f(path, w, h):
    d = open(path, "rb").read()
    pitch = len(d) // h
    vals = []
    for y in range(h):
        row = struct.unpack_from("<%de" % (w * 4), d, y * pitch)
        vals.append(row)
    return vals


def lum(row, x):
    r, g, b = row[4 * x], row[4 * x + 1], row[4 * x + 2]
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def analyze(rows, w, h, box, ratio):
    x0, y0, x1, y1 = box
    L = [[lum(rows[y], x) for x in range(w)] for y in range(h)]
    nonfinite = zero = dots = n = 0
    for y in range(max(1, y0), min(h - 1, y1)):
        for x in range(max(1, x0), min(w - 1, x1)):
            v = L[y][x]
            if not math.isfinite(v):
                nonfinite += 1
                continue
            n += 1
            if v == 0.0:
                zero += 1
            nb = sorted(L[yy][xx] if math.isfinite(L[yy][xx]) else 0.0
                        for yy, xx in ((y - 1, x - 1), (y - 1, x), (y - 1, x + 1), (y, x - 1), (y, x + 1),
                                       (y + 1, x - 1), (y + 1, x), (y + 1, x + 1)))
            med = 0.5 * (nb[3] + nb[4])
            if med > 1e-4 and v < ratio * med and nb[1] >= 0.5 * med:
                dots += 1
    return n, nonfinite, zero, dots, L


def png(path, rows, L, crop, scale):
    cx, cy, cw, ch = crop
    finite = sorted(v for r in L for v in r if math.isfinite(v) and v > 0)
    key = finite[len(finite) // 2] if finite else 1.0
    out = bytearray()
    for y in range(ch * scale):
        out.append(0)
        sy = cy + y // scale
        for x in range(cw * scale):
            sx = cx + x // scale
            for c in range(3):
                v = rows[sy][4 * sx + c]
                if not math.isfinite(v):
                    out.append(255 if c == 0 else 0)
                    continue
                t = max(v, 0.0) * 0.18 / key
                out.append(int(round(255 * (t / (1 + t)) ** (1 / 2.2))))
    W, H = cw * scale, ch * scale

    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(bytes(out), 6)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("w", type=int)
    ap.add_argument("h", type=int)
    ap.add_argument("--box")
    ap.add_argument("--ratio", type=float, default=0.25)
    ap.add_argument("--png")
    ap.add_argument("--crop", default="0,0,1280,720")
    ap.add_argument("--scale", type=int, default=1)
    a = ap.parse_args()
    rows = read_rgba16f(a.raw, a.w, a.h)
    box = tuple(int(v) for v in a.box.split(",")) if a.box else (0, 0, a.w, a.h)
    n, nf, z, d, L = analyze(rows, a.w, a.h, box, a.ratio)
    print("%s: %d texels in box, non-finite %d, zero %d (%.2f%%), isolated dark %d (%.3f%%)" %
          (os.path.basename(a.raw), n, nf, z, 100.0 * z / max(n, 1), d, 100.0 * d / max(n, 1)))
    if a.png:
        png(a.png, rows, L, tuple(int(v) for v in a.crop.split(",")), a.scale)


if __name__ == "__main__":
    main()
