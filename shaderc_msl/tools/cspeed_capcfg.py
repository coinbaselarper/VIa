#!/usr/bin/env python3
"""Inputs for cspeed_ab from a dispatch captured in the title (GTA6RT_METAL_CS_CAPTURE, d3d12_metal/src/metal_replay.cpp).

The capture holds the user data, the group counts and the title memory the CPU saw at encode: the tables the user-data
pointers name and the ranges of the buffer V#s in them. Those regions are laid out again in one heap buffer (the
contract heap: the kernel reads heap_base + VA), the table pointers in the user data and the V#s in the tables are
rebased onto the new layout (a V# whose range was not captured gets a 64 KiB zero range), and the bindless hash table
gets one entry per T#-looking 8-word window of the tables (slot per T#, texture class from the kernel's own format
tables) and one per 4-word window as an S# key (4 words + 4 x 0xffffffff).

  cspeed_capcfg.py kernel.metal capture.cap out_prefix [--index spirv/library/index.json] [--max-groups 65536]
Writes out_prefix.json (bindings), out_prefix.heap (heap image), out_prefix.table (hash table words).
"""
import argparse, json, os, re, struct, sys

FNV0, FNVP = 2166136261, 16777619
VA0 = 0x10000000
PAD = 64 << 10
CORPUS_INDEX = os.path.join(os.environ.get("GTA6RT_METAL_SPIRV") or os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "spike", "s2-make-game", "work", "shaders", "spirv")),
    "library", "index.json")


def fnv(words):
    h = FNV0
    for w in words:
        h = ((h ^ w) * FNVP) & 0xFFFFFFFF
    return h


def read_cap(path):
    b = open(path, "rb").read()
    assert b[:8] == b"CSCAP001", "not a capture"
    at = 8
    (n,) = struct.unpack_from("<I", b, at); at += 4
    label = b[at:at + n].decode(); at += n
    x, y, z, ind = struct.unpack_from("<4I", b, at); at += 16
    ud = list(struct.unpack_from("<32I", b, at)); at += 128
    (nr,) = struct.unpack_from("<I", b, at); at += 4
    regions = []
    for _ in range(nr):
        va, size, kind = struct.unpack_from("<QQI", b, at); at += 20
        regions.append({"va": va, "data": bytearray(b[at:at + size]), "kind": kind}); at += size
    return {"label": label, "groups": [x, y, z], "indirect": ind, "user_data": ud, "regions": regions}


def valid_swizzle(w3):
    return all(((w3 >> (3 * i)) & 7) not in (2, 3) for i in range(4))


def vsharp(words):
    w0, w1, w2, w3 = words
    if (w3 >> 30) & 3 or not valid_swizzle(w3) or not w2:
        return None
    addr = ((w1 & 0xFFFF) << 32) | w0
    if addr & (1 << 47):
        addr -= 1 << 48
    stride = (w1 >> 16) & 0x3FFF
    size = stride * w2 if stride else w2
    return addr, size


def bindings(msl):
    entry = re.search(r"^kernel void main0\((.*)\)$", msl, re.M).group(1)
    sets = {}
    for m in re.finditer(r"(?:const )?device spvDescriptorSetBuffer(\d+)& spvDescriptorSet\d+ \[\[buffer\((\d+)\)\]\]", entry):
        body = re.search(r"^struct spvDescriptorSetBuffer" + m.group(1) + r"\n\{\n(.*?)^\};", msl, re.S | re.M).group(1)
        first = body.strip().splitlines()[0]
        sets["spvDescriptorSet%s.m" % m.group(1)] = {"index": int(m.group(2)), "decl": first}
    for m in re.finditer(r"device const void\* (spvDescriptorSet\d+Binding\d+) \[\[buffer\((\d+)\)\]\]", entry):
        sets[m.group(1)] = {"index": int(m.group(2)), "decl": ""}
    out = []
    for key, s in sets.items():
        d = s["decl"]
        if "device _" in d and "*" in d:
            out.append({"index": s["index"], "kind": "table"})
            continue
        if "spvDescriptor<sampler>" in d:
            out.append({"index": s["index"], "kind": "sampler"})
            continue
        if key.endswith(".m"):
            tm = re.search(r"spvDescriptor<(\w+)<(\w+)(, access::\w+)?>>", d)
        else:
            tm = re.search(r"spvDescriptorArray<(\w+)<(\w+)(, access::\w+)?>> _\d+ \{" + key + r"\}", msl)
        if not tm:
            raise SystemExit("binding %s: unknown declaration %r" % (key, d))
        out.append({"index": s["index"], "kind": "texture", "type": tm.group(1), "elem": tm.group(2),
                    "write": 1 if tm.group(3) else 0})
    push = re.search(r"constant (_\d+)& (_\d+) \[\[buffer\((\d+)\)\]\]", entry)
    pstruct, pvar = push.group(1), push.group(2)
    body = re.search(r"^struct " + pstruct + r"\n\{\n(.*?)^\};", msl, re.S | re.M).group(1)
    offs, n = {}, 0
    for line in body.splitlines():
        m = re.match(r"\s*uint (_m\d+)(?:\[(\d+)\])?;", line)
        if m:
            offs[m.group(1)] = n
            n += int(m.group(2) or 1)
    hm = re.search(r"\(ulong\(" + pvar + r"\.(_m\d+)\) << 32ul\) \| ulong\(" + pvar + r"\.(_m\d+)\)", msl)
    addr_words = [offs[hm.group(2)], offs[hm.group(1)]] if hm else []
    return out, {"index": int(push.group(3)), "words": n, "addr_words": addr_words}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("msl")
    ap.add_argument("cap")
    ap.add_argument("out")
    ap.add_argument("--index", default=CORPUS_INDEX)
    ap.add_argument("--max-groups", type=int, default=65536)
    a = ap.parse_args()
    msl = open(a.msl).read()
    cap = read_cap(a.cap)
    name = cap["label"].split(" ")[0].split("/")[-1]
    meta = json.load(open(a.index))[name]
    ptr_sgprs = [r["sgpr"] for r in meta["user_sgpr_reads"] if r["kind"] == "ptr32_lo"]
    if any(r["kind"] not in ("ptr32_lo", "const32") for r in meta["user_sgpr_reads"]):
        raise SystemExit("user sgpr kinds not handled: %r" % meta["user_sgpr_reads"])

    regions = cap["regions"]
    layout, at = {}, VA0 + PAD
    for r in regions:
        layout[r["va"]] = at
        at += (len(r["data"]) + PAD + 4095) & ~4095
    heap_bytes = at - VA0
    dummy = VA0

    def rebase(addr, size):
        for r in regions:
            if r["kind"] == 1 and r["va"] == addr and len(r["data"]) >= size:
                return layout[addr]
        return None

    patched = uncaptured = 0
    for r in regions:
        if r["kind"] != 0:
            continue
        d = r["data"]
        for o in range(0, len(d) - 15, 16):
            w = list(struct.unpack_from("<4I", d, o))
            v = vsharp(w)
            if not v or v[0] < 0x10000:
                continue
            new = rebase(v[0], (v[1] + 3) & ~3)
            if new is None:
                if v[1] > (1 << 32):
                    continue
                new = dummy
                stride = (w[1] >> 16) & 0x3FFF
                w[2] = min(w[2], (PAD // stride) if stride else PAD)
                uncaptured += 1
            else:
                patched += 1
            w[0] = new & 0xFFFFFFFF
            w[1] = (w[1] & 0xFFFF0000) | ((new >> 32) & 0xFFFF)
            struct.pack_into("<4I", d, o, *w)

    ud = list(cap["user_data"])
    tables = {r["va"]: r for r in regions if r["kind"] == 0}
    for i in ptr_sgprs:
        t = ud[i] & ~3
        if t in tables:
            ud[i] = layout[t] | (ud[i] & 3)
        elif t >= 0x10000:
            raise SystemExit("user data s%d = %#x names no captured table" % (i, ud[i]))

    heap = bytearray(heap_bytes)
    for r in regions:
        o = layout[r["va"]] - VA0
        heap[o:o + len(r["data"])] = r["data"]

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import tmem_abcfg
    t1, t2, arm_types = tmem_abcfg.format_tables(msl)
    cls_of = {"float": 2, "uint": 1, "int": 0}

    def klass(w1):
        c = (w1 >> 20) & 511
        for t, ty in ((t1, arm_types[0] if arm_types else None), (t2, arm_types[1] if arm_types else None)):
            if t and ty and (t[c >> 5] >> (c & 31)) & 1:
                return cls_of[ty]
        return 2

    keys, slot_class = [], [2]
    for r in tables.values():
        d = r["data"]
        for o in range(0, len(d) - 31, 16):
            w = list(struct.unpack_from("<8I", d, o))
            if ((w[3] >> 28) & 0xF) >= 8 and valid_swizzle(w[3]):
                keys.append((w, len(slot_class)))
                slot_class.append(klass(w[1]))
        for o in range(0, len(d) - 15, 16):
            w = list(struct.unpack_from("<4I", d, o)) + [0xFFFFFFFF] * 4
            keys.append((w, 1 + (o // 16) % 15))
    slots = 1 << (2 * max(1, len(keys))).bit_length()
    table = [0] * (1 + slots * 10 + 10)
    table[0] = slots - 1
    used, lost = set(), 0
    for key, val in keys:
        h = fnv(key)
        for p in range(8):
            s = (h + p) & (slots - 1)
            if s in used and table[s * 10 + 1:s * 10 + 9] == key:
                break
            if s not in used:
                used.add(s)
                table[s * 10 + 1:s * 10 + 9] = key
                table[s * 10 + 9] = val
                break
        else:
            lost += 1

    binds, push = bindings(msl)
    pw = [0] * push["words"]
    pw[:32] = ud[:min(32, push["words"])]
    groups = cap["groups"]
    g = groups[0] * groups[1] * groups[2]
    capped = False
    if g > a.max_groups or g == 0:
        capped = True
        groups = [min(groups[0], a.max_groups) or 1, 1, 1]
    cfg = {"label": cap["label"], "threads": meta["local_size"], "groups": groups, "groups_captured": cap["groups"],
           "indirect": cap["indirect"], "push": {"index": push["index"], "words": pw, "addr_words": push["addr_words"]},
           "heap": {"file": a.out + ".heap", "va0": VA0}, "table": {"file": a.out + ".table"},
           "bindings": binds, "slot_class": slot_class, "samplers": 16}
    open(a.out + ".heap", "wb").write(heap)
    open(a.out + ".table", "wb").write(struct.pack("<%dI" % len(table), *table))
    json.dump(cfg, open(a.out + ".json", "w"))
    print(json.dumps({"label": cap["label"], "groups": cap["groups"], "used_groups": groups, "capped": capped,
                      "regions": len(regions), "tables": len(tables), "heap_mib": round(heap_bytes / 2**20, 1),
                      "vsharps_rebased": patched, "vsharps_uncaptured": uncaptured, "tsharp_slots": len(slot_class) - 1,
                      "keys": len(keys), "keys_lost": lost, "arm_types": arm_types,
                      "bindings": [(b["index"], b["kind"], b.get("type"), b.get("elem"), b.get("write")) for b in binds]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
