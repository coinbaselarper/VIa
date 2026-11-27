#!/usr/bin/env python3
"""Pixel statistics of present dumps (8-bit RGBA PNGs as present_dump.h writes them; any filter type).

  png_stats.py FILE.png [...]   one line per file: size, lit pixels (any RGB channel > 0), mean RGB, distinct colours
"""
import struct
import sys
import zlib


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w = 8, [], None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", body[:10])
            if depth != 8 or colour != 6:
                raise ValueError("only 8-bit RGBA")
        elif kind == b"IDAT":
            idat.append(body)
        pos += 12 + n
    raw = zlib.decompress(b"".join(idat))
    stride = w * 4
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if ft == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft != 0:
            raise ValueError(f"filter {ft} not handled")
        rows.append(bytes(line))
        prev = line
    return w, h, b"".join(rows)


def stats(path):
    w, h, px = read_png(path)
    lit, sums, colours = 0, [0, 0, 0], set()
    for i in range(0, len(px), 4):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if r or g or b:
            lit += 1
            sums[0] += r
            sums[1] += g
            sums[2] += b
        if len(colours) < 65536:
            colours.add((r, g, b))
    n = w * h
    mean = [s / n for s in sums]
    return f"{path}: {w}x{h} lit {lit}/{n} ({100.0 * lit / n:.1f}%) mean rgb {mean[0]:.1f} {mean[1]:.1f} {mean[2]:.1f} colours {len(colours)}{'+' if len(colours) >= 65536 else ''}"


if __name__ == "__main__":
    for p in sys.argv[1:]:
        print(stats(p), flush=True)
