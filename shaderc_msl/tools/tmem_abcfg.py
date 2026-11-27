#!/usr/bin/env python3
"""Random-input config for tmem_ab from a translated compute kernel's MSL (contract bindings).

Layout "small" (kernels whose raw loads sit at push-word offsets only): heap words: low 20 bits random small (0..mask), bits 20..28 one of an int / uint / float V# format code taken from the
kernel's own format tables, so every typed-view branch is taken. The bindless table holds an entry for every 8-word
window of the heap's first `windows` words (the kernel's lookups hash 8 V# words), mapping a window to slot 1 + class;
the texture arrays hold a texture of the matching type at that slot (slot 0, the miss, is float).

  tmem_abcfg.py kernel.metal out.json --seed N [--groups 4,4,1] [--threads 8,8,1] [--small 63]
"""
import argparse, json, random, re, sys

FNV0, FNVP = 2166136261, 16777619


def fnv(words):
    h = FNV0
    for w in words:
        h = ((h ^ w) * FNVP) & 0xFFFFFFFF
    return h


def format_tables(msl):
    tabs = {}
    for m in re.finditer(r"constant spvUnsafeArray<uint, 16> (_\d+) = spvUnsafeArray<uint, 16>\(\{ ([^}]*) \}\);", msl):
        tabs[m.group(1)] = [int(x.rstrip("u")) for x in m.group(2).split(", ")]
    m = re.search(r"if \(\(extract_bits\((_\d+)\[_\d+\], _\d+, 1u\) != 0u\) && _\d+\)\n\s*\{\n(?:[^\n]*\n){0,3}?[^\n]*?(_\d+)\[_\d+\]\.(?:read|sample)", msl)
    if not m:
        return None, None, None
    m2 = re.search(r"\n\s*if \(\(extract_bits\((_\d+)\[_\d+\], _\d+, 1u\) != 0u\) && _\d+\)\n\s*\{\n(?:[^\n]*\n){0,3}?[^\n]*?(_\d+)\[_\d+\]\.(?:read|sample)", msl[m.end():])
    if not m2:
        return None, None, None
    views = {v.group(2): v.group(1) for v in re.finditer(r"spvDescriptorArray<texture\w+<(\w+)[^>]*>> (_\d+) \{", msl)}
    return tabs.get(m.group(1)), tabs.get(m2.group(1)), (views.get(m.group(2)), views.get(m2.group(2)))


def codes(t_int, t_uint):
    def bit(t, c):
        return (t[c >> 5] >> (c & 31)) & 1
    ci = next((c for c in range(512) if bit(t_int, c)), None)
    cu = next((c for c in range(512) if bit(t_uint, c) and not bit(t_int, c)), None)
    cf = next(c for c in range(1, 512) if not bit(t_int, c) and not bit(t_uint, c))
    return {k: v for k, v in (("int", ci), ("uint", cu), ("float", cf)) if v is not None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("msl")
    ap.add_argument("out")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--groups", default="4,4,1")
    ap.add_argument("--threads", default="8,8,1")
    ap.add_argument("--small", type=int, default=63)
    ap.add_argument("--windows", type=int, default=512)
    ap.add_argument("--size", type=int, default=64)
    ap.add_argument("--layout", default="small", choices=["small", "safe"],
                    help="safe: every heap and push word has its low 16 bits clear, so any word pair used as a 48-bit "
                         "heap offset stays below 8 MiB (kernels that chase buffer V#s)")
    ap.add_argument("--heap-mib", type=int, default=32)
    a = ap.parse_args()
    msl = open(a.msl).read()
    rng = random.Random(a.seed)
    entry = re.search(r"^kernel void main0\((.*)\)$", msl, re.M).group(1)
    push = re.search(r"constant (_\d+)& (_\d+) \[\[buffer\((\d+)\)\]\]", entry)
    pstruct, pvar, pidx = push.group(1), push.group(2), int(push.group(3))
    body = re.search(r"^struct " + pstruct + r"\n\{\n(.*?)^\};", msl, re.S | re.M).group(1)
    offs, n = {}, 0
    for line in body.splitlines():
        m = re.match(r"\s*uint (_m\d+)(?:\[(\d+)\])?;", line)
        if m:
            offs[m.group(1)] = n
            n += int(m.group(2) or 1)
    hm = re.search(r"\(ulong\(" + pvar + r"\.(_m\d+)\) << 32ul\) \| ulong\(" + pvar + r"\.(_m\d+)\)", msl)
    addr_words = [offs[hm.group(2)], offs[hm.group(1)]] if hm else []
    t_int, t_uint, arm_types = format_tables(msl)
    tfmt = {"int": "rgba32sint", "uint": "rgba32uint", "float": "rgba32float", None: "rgba32float"}
    arm_types = arm_types or (None, None)
    cls = codes(t_int, t_uint) if t_int and t_uint else {"int": 0, "uint": 0, "float": 0}
    names = sorted(cls)
    if a.layout == "safe":
        heap = [(cls[rng.choice(names)] << 20) | (rng.randint(0, 15) << 16) for _ in range(a.windows + 64)]
        pw = [0] * n
    else:
        heap = [(cls[rng.choice(names)] << 20) | rng.randint(0, a.small) for _ in range(max(4096, a.windows + 64))]
        pw = [rng.randint(0, a.small) for _ in range(n)]
    for i in addr_words:
        pw[i] = 0

    def klass(w):
        c = (w >> 20) & 511
        return 0 if c == cls.get("int") else 1 if c == cls.get("uint") else 2

    slots = 1 << (2 * a.windows).bit_length()
    table = [0] * (1 + slots * 10 + 10)
    table[0] = slots - 1
    used = set()
    for w in range(a.windows):
        key = heap[w:w + 8]
        h = fnv(key)
        for p in range(8):
            s = (h + p) & (slots - 1)
            if s not in used:
                used.add(s)
                table[s * 10 + 1:s * 10 + 9] = key
                table[s * 10 + 9] = 1 + klass(key[1])
                break
    cfg = {"seed": a.seed, "threads": [int(x) for x in a.threads.split(",")], "groups": [int(x) for x in a.groups.split(",")],
           "push": {"index": pidx, "words": pw}, "heap": {"words": heap, "addr_words": addr_words, "size_words": max(len(heap), a.heap_mib << 18 if a.layout == "safe" else 0)},
           "buffers": [], "textures": [], "samplers": [], "tex_mask": 0xFFFF if a.layout == "safe" else 0xFFFFFFFF}
    sz = a.size
    for m in re.finditer(r"(?:const )?device spvDescriptorSetBuffer(\d+)& spvDescriptorSet\d+ \[\[buffer\((\d+)\)\]\]", entry):
        sb = re.search(r"^struct spvDescriptorSetBuffer" + m.group(1) + r"\n\{\n(.*?)^\};", msl, re.S | re.M).group(1)
        idx = int(m.group(2))
        first = sb.strip().splitlines()[0]
        if "device _" in first and "*" in first:
            cfg["buffers"].append({"index": idx, "words": table})
        elif "spvDescriptor<sampler>" in first:
            cfg["samplers"].append({"index": idx, "count": 4})
        else:
            tm = re.search(r"spvDescriptor<(texture2d(?:_array)?)<(\w+)(, access::write)?>>", first)
            kind = "2d_array" if tm.group(1).endswith("_array") else "2d"
            fmt = {"int": "rgba32sint", "uint": "rgba32uint", "float": "rgba32float"}[tm.group(2)]
            if tm.group(3):
                sl = [fmt] * 4
            else:
                sl = ["rgba32float", tfmt[arm_types[0]], tfmt[arm_types[1]], "rgba32float"]
            cfg["textures"].append({"index": idx, "type": kind, "write": 1 if tm.group(3) else 0, "slots": sl,
                                    "size": [sz, sz, 4]})
    json.dump(cfg, open(a.out, "w"))
    print(json.dumps({"codes": cls, "arm_types": arm_types, "push_words": n, "addr_words": addr_words, "tables": len(cfg["buffers"]),
                      "textures": [(t["index"], t["write"], t["type"]) for t in cfg["textures"]]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
