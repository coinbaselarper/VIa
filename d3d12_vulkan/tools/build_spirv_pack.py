#!/usr/bin/env python3
"""Pack a translated corpus's .spv modules into one file the layer memory-maps (portable/src/spirv_pack.h).

usage: python3 -I build_spirv_pack.py CORPUS OUT [--stages vs,ps,gs,ms]

CORPUS is the folder LEONIDA_SHADERS names (index.json and its sub-indexes). Only modules of the listed stages are
packed (default: the graphics stages, which PSO creation reads; compute modules are read by pipeline builds). The
layer reads OUT when LEONIDA_SPIRV_PACK names it, or CORPUS/spirv.pack when that exists, and only while the index
files are the ones named in the pack's manifest. A module whose file changed after packing is read from its file.
"""
import argparse
import json
import os
import struct
import sys

INDEX_FILES = ["index.json", "fspx/index.json", "fspx_compute/index.json", "fspx_gfx/index.json", "bvh/index.json",
               "exe/index.json", "library/index.json"]


def manifest(corpus):
    lines = []
    for name in INDEX_FILES:
        try:
            st = os.stat(os.path.join(corpus, name))
        except OSError:
            continue
        lines.append("%s %d %d\n" % (name, st.st_size, st.st_mtime_ns))
    return "".join(lines).encode()


def modules(corpus, stages):
    names = set()
    for index in INDEX_FILES:
        path = os.path.join(corpus, index)
        if not os.path.exists(path):
            continue
        prefix = os.path.dirname(index)
        prefix = prefix + "/" if prefix else ""
        with open(path, "rb") as f:
            data = json.loads(f.read().decode("utf-8-sig"))
        for name, meta in data.items():
            if not isinstance(meta, dict) or meta.get("stage") not in stages:
                continue
            names.add(prefix + name + ".spv")
    return sorted(names, key=lambda n: n.encode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("out")
    ap.add_argument("--stages", default="vs,ps,gs,ms")
    args = ap.parse_args()
    corpus = os.path.abspath(args.corpus)
    stages = set(args.stages.split(","))
    before = manifest(corpus)
    rows = []
    for rel in modules(corpus, stages):
        try:
            st = os.stat(os.path.join(corpus, rel))
        except OSError:
            continue
        rows.append((rel, st.st_size, st.st_mtime_ns))
    names = b"".join(r[0].encode() for r in rows)
    header = 32 + len(before)
    table_offset = (header + 7) & ~7
    names_offset = table_offset + 40 * len(rows)
    data_offset = (names_offset + len(names) + 4095) & ~4095
    tmp = args.out + ".tmp"
    total = 0
    with open(tmp, "wb") as out:
        out.write(b"LSPVPAK1" + struct.pack("<IIQQ", len(rows), len(before), table_offset, names_offset) + before)
        out.write(b"\0" * (table_offset - header))
        offset = data_offset
        name_offset = 0
        table = []
        for rel, size, mtime in rows:
            table.append(struct.pack("<QIIQQq", name_offset, len(rel.encode()), 0, offset, size, mtime))
            name_offset += len(rel.encode())
            offset = (offset + size + 7) & ~7
        out.write(b"".join(table))
        out.write(names)
        out.write(b"\0" * (data_offset - names_offset - len(names)))
        pos = data_offset
        for rel, size, mtime in rows:
            with open(os.path.join(corpus, rel), "rb") as f:
                blob = f.read()
            if len(blob) != size:
                sys.exit("%s changed while packing" % rel)
            out.write(blob)
            pad = ((pos + size + 7) & ~7) - (pos + size)
            out.write(b"\0" * pad)
            pos += size + pad
            total += size
    if manifest(corpus) != before:
        os.unlink(tmp)
        sys.exit("the index files changed while packing")
    os.replace(tmp, args.out)
    print("%s: %d modules, %.0f MiB of SPIR-V (stages %s)" % (args.out, len(rows), total / 1048576.0,
                                                               ",".join(sorted(stages))))


if __name__ == "__main__":
    main()
