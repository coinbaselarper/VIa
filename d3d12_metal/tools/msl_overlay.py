#!/usr/bin/env python3
"""An overlay manifest for the adapter's shader store (GTA6RT_METAL_SHADER_OVERLAY): the rows of SOURCE (a full manifest
such as mesh_tg_pack.py's OUT/manifest.tsv) that differ from the store's, with every library they name outside the
store copied into DEST/cas and the paths rewritten relative to the store root.

  msl_overlay.py SOURCE_MANIFEST DEST_DIR
"""
import os
import shutil
import sys

PORT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
STORE_ROOT = os.path.join(PORT, "build", "shaderc_msl")
STORE = os.path.join(STORE_ROOT, "runs", "main", "manifest.tsv")
PATH_COLUMNS = (4, 11, 12, 18, 19)


def rows(path):
    head, out = [], {}
    for line in open(path):
        if line.startswith("#") and "\t" not in line:
            head.append(line)
            continue
        f = line.rstrip("\n").split("\t")
        out[f[0]] = f
    return head, out


def main():
    src, dest = sys.argv[1], os.path.abspath(sys.argv[2])
    head, store = rows(STORE)
    _, source = rows(src)
    os.makedirs(os.path.join(dest, "cas"), exist_ok=True)
    changed = [f for n, f in source.items() if store.get(n) != f]
    copied = 0
    for f in changed:
        for c in PATH_COLUMNS:
            if c >= len(f) or not f[c].endswith(".metallib"):
                continue
            path = os.path.normpath(os.path.join(STORE_ROOT, f[c]))
            if path.startswith(STORE_ROOT + os.sep):
                continue
            to = os.path.join(dest, "cas", os.path.basename(os.path.dirname(path)), os.path.basename(path))
            os.makedirs(os.path.dirname(to), exist_ok=True)
            if not os.path.exists(to):
                shutil.copy2(path, to)
                copied += 1
            f[c] = os.path.relpath(to, STORE_ROOT)
    rel_root = os.path.relpath(STORE_ROOT, dest)
    with open(os.path.join(dest, "manifest.tsv.part"), "w") as o:
        for h in head:
            if "root=" in h:
                h = " ".join("root=" + rel_root if w.startswith("root=") else w for w in h.rstrip("\n").split(" ")) + "\n"
            o.write(h)
        for f in sorted(changed, key=lambda f: f[0]):
            o.write("\t".join(f) + "\n")
    os.replace(os.path.join(dest, "manifest.tsv.part"), os.path.join(dest, "manifest.tsv"))
    print("%d rows, %d libraries copied -> %s" % (len(changed), copied, os.path.join(dest, "manifest.tsv")))


if __name__ == "__main__":
    sys.exit(main())
