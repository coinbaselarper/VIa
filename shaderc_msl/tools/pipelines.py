#!/usr/bin/env python3
"""Pipeline pairing from the title's pipeline library (the zlib part of shaders.rpf).

Own reader for the layout measured in leonida/docs/shader_library.md (no leonida code is imported or run):
graphics records name one program per stage slot (0 ps, 1 vs, 2 ms, 6 unknown) and a stage blob whose 16 bytes
after the 0x43 tag word are the HASH digest of the carved DXBC container the program was built from.

Writes <out>/pairs.json:
  graphics: [{hash, ps, vs, ms, slot6, unlinked: [...]}]   container names (index.json keys) per stage
  compute:  [{hash, cs}]
  digest_unmatched: programs whose digest names no carved container
Game-derived data: keep it under port/build.
"""
import argparse, hashlib, json, os, re, struct, sys, time, zlib

MAGIC = b"PS\x00\x09"
LIB_OFFSET = 0x10579000
GFX, CS = 0x70, 24
NONE = 0xFFFFFFFF
SLOTS = {0: "ps", 1: "vs", 2: "ms", 3: "slot3", 4: "slot4", 5: "slot5", 6: "slot6"}
ZHDR = re.compile(rb"\x78[\x01\x5e\x9c\xda]")


def extract(rpf):
    data = open(rpf, "rb").read()
    pos, parts, total, need = LIB_OFFSET, [], 0, None
    while pos < len(data):
        m = ZHDR.search(data, pos)
        if not m:
            break
        d = zlib.decompressobj()
        try:
            out = d.decompress(data[m.start():m.start() + (1 << 20)])
        except zlib.error:
            pos = m.start() + 1
            continue
        if not d.eof:
            pos = m.start() + 1
            continue
        if need is None:
            if out[:4] != MAGIC:
                pos = m.start() + 1
                continue
            g, c, nb, npg, bsize, csize = struct.unpack_from("<6I", out, 0x50)
            need = 0x80 + g * GFX + c * CS + nb * 8 + npg * 8 + bsize + csize
        parts.append(out)
        total += len(out)
        pos = m.start() + (min(len(data) - m.start(), 1 << 20) - len(d.unused_data))
        if total >= need:
            break
    lib = b"".join(parts)
    if need is None or len(lib) != need:
        raise SystemExit(f"library size {len(lib)} != header size {need}")
    return lib


def blob_digest(blob):
    n = len(blob) // 4
    if n < 6:
        return None
    w = struct.unpack_from("<%dI" % n, blob)
    i = {2: 4, 3: 6}.get(w[0] & 0xFF)
    if i is None or (w[i - 1] & 0xFF) != 0x40:
        return None
    while i < n and (w[i] & 0xFF) in (0x41, 0x42):
        i += 8
    if i + 4 < n and (w[i] & 0xFF) == 0x43:
        return blob[4 * i + 4:4 * i + 20].hex()
    return None


def dxbc_hash(path):
    b = open(path, "rb").read()
    if b[:4] != b"DXBC":
        return None
    n = struct.unpack_from("<I", b, 28)[0]
    for k in range(n):
        off = struct.unpack_from("<I", b, 32 + 4 * k)[0]
        if b[off:off + 4] == b"HASH":
            return b[off + 12:off + 28].hex()
    return None


def parse(lib, digest_to_name):
    g, c, nb, npg, bsize, csize = struct.unpack_from("<6I", lib, 0x50)
    o_gfx = 0x80
    o_cs = o_gfx + g * GFX
    o_bt = o_cs + c * CS
    o_pt = o_bt + nb * 8
    o_bd = o_pt + npg * 8
    blobs = [struct.unpack_from("<2I", lib, o_bt + 8 * i) for i in range(nb)]

    def blob(i):
        off, size = blobs[i]
        return lib[o_bd + off:o_bd + off + size]

    unmatched = set()
    graphics = []
    for i in range(g):
        r = o_gfx + i * GFX
        h = struct.unpack_from("<Q", lib, r)[0]
        rec = {"hash": f"{h:016x}"}
        for s in range(7):
            prog, sb = struct.unpack_from("<2I", lib, r + 56 + 8 * s)
            if prog == NONE:
                continue
            name = None
            if sb != NONE and sb < nb:
                d = blob_digest(blob(sb))
                name = digest_to_name.get(d)
            if name is None:
                unmatched.add((SLOTS[s], prog))
                rec.setdefault("unlinked", []).append(f"{SLOTS[s]}:{prog}")
            rec[SLOTS[s]] = name
            rec[SLOTS[s] + "_program"] = prog
        graphics.append(rec)
    compute = []
    for i in range(c):
        r = o_cs + i * CS
        h, st, b1, prog, lb = struct.unpack_from("<Q4I", lib, r)
        d = blob_digest(blob(lb)) if lb < nb else None
        name = digest_to_name.get(d)
        if name is None:
            unmatched.add(("cs", prog))
        compute.append({"hash": f"{h:016x}", "cs": name, "cs_program": prog})
    return {"header": {"graphics": g, "compute": c, "blobs": nb, "programs": npg},
            "graphics": graphics, "compute": compute,
            "digest_unmatched": sorted(f"{s}:{p}" for s, p in unmatched)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpf", required=True)
    ap.add_argument("--dxbc-dir", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    t0 = time.time()
    d2all = {}
    for f in sorted(os.listdir(a.dxbc_dir)):
        if f.endswith(".dxbc"):
            h = dxbc_hash(os.path.join(a.dxbc_dir, f))
            if h:
                d2all.setdefault(h, []).append(f[:-5])
    d2n = {h: v[0] for h, v in d2all.items()}
    dup = sum(len(v) - 1 for v in d2all.values())
    t1 = time.time()
    lib = extract(a.rpf)
    t2 = time.time()
    res = parse(lib, d2n)
    res["library_sha256"] = hashlib.sha256(lib).hexdigest()
    res["library_bytes"] = len(lib)
    res["containers_hashed"] = len(d2n)
    res["duplicate_digests"] = dup
    res["digest_aliases"] = {v[0]: v[1:] for v in d2all.values() if len(v) > 1}
    res["timing_s"] = {"hash_containers": round(t1 - t0, 2), "extract": round(t2 - t1, 2), "parse": round(time.time() - t2, 2)}
    json.dump(res, open(os.path.join(a.out, "pairs.json"), "w"))
    linked = sum(1 for r in res["graphics"] if not r.get("unlinked"))
    print(f"graphics {len(res['graphics'])} (fully linked {linked}), compute {len(res['compute'])}, "
          f"unmatched programs {len(res['digest_unmatched'])}, containers {len(d2n)}, timing {res['timing_s']}")


if __name__ == "__main__":
    sys.exit(main())
