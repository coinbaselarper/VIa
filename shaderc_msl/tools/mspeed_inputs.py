#!/usr/bin/env python3
"""Synthetic inputs for mspeed_probe: a heap image (segments), the push words and the bindless hash table.

  mspeed_inputs.py SPEC.json OUTPREFIX [--keys TRACE ...]
SPEC: {"heap_bytes": N, "user": {"16": "0x100000", ...},
       "tables": [{"at": A, "bytes": B, "base": D, "step": S, "stride": 16, "records": R, "w3": "0x11014fac"}],
       "fill": [{"at": A, "bytes": B, "pattern": "unorm"|"snorm"|"rand"|"small"|"word:X", "seed": n}],
       "words": {"0xOFFSET": "0xVALUE"}, "textures": K}
tables: every 16-byte slot k is a V# of a buffer at D + k * S. fill: data patterns; words: single words, last.
--keys: hash-lookup keys from trace files (mspeed_trace) go into the table, mapped to texture indices 1..K-1.
Writes OUTPREFIX.heap, OUTPREFIX.hash, OUTPREFIX.push.json.
"""
import json
import struct
import sys

M32 = 0xFFFFFFFF


def mix(x):
    x &= M32
    x ^= x >> 16
    x = (x * 0x7FEB352D) & M32
    x ^= x >> 15
    x = (x * 0x846CA68B) & M32
    x ^= x >> 16
    return x


def num(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def pattern(p, i, seed):
    h = mix(i * 2654435761 ^ seed)
    if p == "unorm":
        return struct.unpack("<I", struct.pack("<f", (h & 0xFFFF) / 65535.0))[0]
    if p == "snorm":
        return struct.unpack("<I", struct.pack("<f", (h & 0xFFFF) / 32767.5 - 1.0))[0]
    if p == "small":
        return h & 0x3F
    if p == "special":
        odd = (0x7FC00000, 0x7F800000, 0xFF800000, 0x80000000, 0x00000001, 0x807FFFFF, 0x00000000, 0x3F800000)
        if h & 7 == 0:
            return odd[(h >> 3) & 7]
        return (0x3C000000 + (h >> 8) % 0x04000000) | (h & 0x80000000)
    if p.startswith("word:"):
        return num(p[5:])
    return h


def fnv(words):
    h = 2166136261
    for w in words:
        h = ((h ^ w) * 16777619) & M32
    return h


def hash_table(keys, ntex, cap=1024):
    t = [0] * (10 * cap)
    t[0] = cap - 1
    out = {}
    for n, k in enumerate(keys):
        h = fnv(k)
        for probe in range(8):
            e = ((h + probe) & (cap - 1))
            if e == 0 or any(t[10 * e + 1: 10 * e + 9]) or t[10 * e + 9]:
                continue
            t[10 * e + 1: 10 * e + 9] = list(k)
            idx = 1 + n % max(1, ntex - 1)
            t[10 * e + 9] = idx
            out[k] = idx
            break
    return t, out


def main():
    spec = json.load(open(sys.argv[1]))
    prefix = sys.argv[2]
    keys = []
    if "--keys" in sys.argv:
        import mspeed_trace as T
        for path in sys.argv[sys.argv.index("--keys") + 1:]:
            for k in T.summary(T.load(path))["keys"]:
                if k not in keys and any(k):
                    keys.append(k)
    words = {}
    for tb in spec.get("tables", []):
        at, n = num(tb["at"]), num(tb["bytes"])
        for k in range(n // 16):
            base = num(tb["base"]) + k * num(tb["step"])
            w1 = ((base >> 32) & 0xFFFF) | ((num(tb.get("stride", 16)) & 0x3FFF) << 16)
            v = [base & M32, w1, num(tb.get("records", 1024)), num(tb.get("w3", "0x11014fac"))]
            for j in range(4):
                words[at + 16 * k + 4 * j] = v[j]
    for f in spec.get("fill", []):
        at, n = num(f["at"]), num(f["bytes"])
        for i in range(n // 4):
            words[at + 4 * i] = pattern(f["pattern"], i, num(f.get("seed", 1)))
    for k, v in spec.get("words", {}).items():
        words[num(k)] = num(v) & M32
    hb = num(spec["heap_bytes"])
    segs = []
    for off in sorted(words):
        if off + 4 > hb:
            raise SystemExit("word 0x%x past the heap" % off)
        if segs and segs[-1][0] + 4 * len(segs[-1][1]) == off:
            segs[-1][1].append(words[off])
        else:
            segs.append([off, [words[off]]])
    with open(prefix + ".heap", "wb") as f:
        for off, ws in segs:
            f.write(struct.pack("<QI", off, 4 * len(ws)))
            f.write(struct.pack("<%dI" % len(ws), *ws))
    t, mapped = hash_table(keys, num(spec.get("textures", 16)))
    open(prefix + ".hash", "wb").write(struct.pack("<%dI" % len(t), *t))
    push = [0] * 36
    for k, v in spec.get("user", {}).items():
        push[int(k)] = num(v) & M32
    json.dump(push, open(prefix + ".push.json", "w"))
    print("heap words %d in %d segments, hash keys %d (%d placed)" % (len(words), len(segs), len(keys), len(mapped)))


if __name__ == "__main__":
    main()
