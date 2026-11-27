#!/usr/bin/env python3
"""Thread-private scratch of compute variants (driver scratch, compute pool): for library/NAME build the store recipe
from the corpus SPIR-V (or the translator fork with FLAGS) with the given MSL passes, then probe each library alone on
a fresh device (tmem_probe lib, indirect zero-group dispatch) through tmem_slot.sh.

  wired2_cs.py WORK library/NAME TX,TY,TZ VARIANT...      VARIANT = label=forkflags|-:pass,pass|-
  e.g. wired2_cs.py build/wired2-x86_64/cs library/lib_cs_00977 8,16,1 store=-:- merged=-:merge_typed_reads \
           spill=--spill-lanes:merge_typed_reads
Prints one line per variant: label, graphics step (MiB), metallib.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sstruct as SS  # noqa: E402

PORT = SS.PORT
PROBE = os.path.join(PORT, "build", "tmem-x86_64", "gta6rt_tmem_probe")


def probe(lib, threads):
    r = subprocess.run([SS.SLOT, PROBE, "lib", lib, "main0", threads, "indirect0"], capture_output=True, text=True)
    step = None
    for l in r.stdout.splitlines():
        m = re.match(r"RESULT graphics_delta_mib ([\d.-]+)", l)
        if m:
            step = float(m.group(1))
    return step, r.stdout


def main():
    work, name, threads = sys.argv[1:4]
    os.makedirs(work, exist_ok=True)
    head, rows = SS.P.manifest_rows()
    row = rows[name]
    out = {}
    for v in sys.argv[4:]:
        label, spec = v.split("=", 1)
        flags, passes = spec.split(":", 1)
        flags = [] if flags == "-" else flags.split(",")
        passes = [] if passes == "-" else passes.split(",")
        spv_root = SS.CORPUS
        if flags:
            fork_dir = os.path.join(work, "spv-" + label)
            SS.translate(fork_dir, [name], flags)
            spv_root = fork_dir
        r = SS.build_cs(work, spv_root, name, row, passes)
        if r.get("status") != "ok":
            print(label, "build", r.get("status"))
            continue
        step, raw = probe(r["metallib"], threads)
        out[label] = {"graphics": step, "metallib": r["metallib"], "passes": r.get("passes"), "info": r.get("info")}
        print(label, step, r["metallib"], flush=True)
        if step is None:
            print(raw[-800:])
    json.dump(out, open(os.path.join(work, "res-%s.json" % name.replace("/", "_")), "w"), indent=1)


if __name__ == "__main__":
    main()
