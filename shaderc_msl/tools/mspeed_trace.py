#!/usr/bin/env python3
"""Read a trace written by an mspeed.py trace variant (mspeed_probe out_prefix .trace file).

  mspeed_trace.py TRACE [--groups G] [--words]
"""
import struct
import sys

HEAD = 262144 // 4
KEYS = 1024
CASES = 8192
BITMAP = 16384 * 4


def load(path):
    raw = open(path, "rb").read()
    return struct.unpack("<%dI" % (len(raw) // 4), raw)


def summary(t, groups=1, words=False):
    out = {"oor": t[3], "hash_calls_simd": t[4], "dispatch_simd": t[5], "outer_simd": t[6]}
    out["oor_offsets"] = sorted({(t[64 + 4 * i] | (t[65 + 4 * i] << 32), t[66 + 4 * i]) for i in range(min(t[3], 128))})
    keys = {}
    for k in range(min(t[4], (CASES - KEYS) // 9)):
        w = tuple(t[KEYS + k * 9: KEYS + k * 9 + 8])
        keys[w] = keys.get(w, 0) + 1
    out["keys"] = keys
    cases = []
    for i in range(CASES, CASES + 128):
        v = t[i]
        while v:
            b = (v & -v).bit_length() - 1
            cases.append((i - CASES) * 32 + b)
            v &= v - 1
    out["cases"] = cases
    read = []
    for i in range(BITMAP, len(t)):
        v = t[i]
        while v:
            b = (v & -v).bit_length() - 1
            read.append((i - BITMAP) * 32 + b)
            v &= v - 1
    out["words_read"] = len(read)
    out["seq"] = [t[16384 + i] for i in range(min(t[7], 49152))]
    if words:
        out["read"] = read
    return out


def ranges(words):
    out = []
    for w in words:
        if out and w == out[-1][1]:
            out[-1][1] = w + 1
        else:
            out.append([w, w + 1])
    return out


if __name__ == "__main__":
    t = load(sys.argv[1])
    s = summary(t, words=True)
    print("out of range loads %d %s" % (s["oor"], ["0x%x@site%d" % x for x in s["oor_offsets"]]))
    print("simd groups: dispatch iterations %d, outer iterations %d, hash lookups %d" % (s["dispatch_simd"], s["outer_simd"], s["hash_calls_simd"]))
    print("cases taken %d: %s" % (len(s["cases"]), s["cases"]))
    print("hash keys %d distinct" % len(s["keys"]))
    for k, n in sorted(s["keys"].items(), key=lambda x: -x[1]):
        print("  %s  x%d" % (" ".join("%08x" % w for w in k), n))
    print("words read %d" % s["words_read"])
    for a, b in ranges(s["read"]):
        print("  [0x%x, 0x%x) %d words" % (a * 4, b * 4, b - a))
