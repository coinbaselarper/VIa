#!/usr/bin/env python3
"""Copy an overlay manifest to another directory, rewriting its root= header so it still names the same store.
Row paths are relative to the store root, so only the header changes.
  overlay_root.py SRC DST
"""
import os
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
out = []
for line in open(src):
    m = re.search(r"root=(\S+)", line) if line.startswith("#") else None
    if m:
        store = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(src)), m.group(1)))
        line = line.replace(m.group(0), "root=" + os.path.relpath(store, os.path.dirname(os.path.abspath(dst))))
    out.append(line)
tmp = dst + ".part"
with open(tmp, "w") as f:
    f.writelines(out)
os.replace(tmp, dst)
print("%s: %d rows" % (dst, sum(1 for l in out if not l.startswith("#"))))
