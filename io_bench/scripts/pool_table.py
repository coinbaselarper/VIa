"""Median table for e16_pool_vs_title.sh output: python3 -I pool_table.py RESULTS.txt"""
import collections
import statistics
import sys

rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    if not line.startswith("RESULT"):
        continue
    d = dict(kv.split("=", 1) for kv in line.split()[1:])
    rows[(int(d["burn"]), d["cfg"])].append((float(d["out_mibs"]), float(d["burn_vs_solo"]), float(d["cpu_s_per_gib_out"])))
for burn in sorted({k[0] for k in rows}):
    print(f"burn threads {burn}")
    for cfg in [k[1] for k in rows if k[0] == burn]:
        v = rows[(burn, cfg)]
        print(f"  {cfg:16s} out {statistics.median(x[0] for x in v):6.0f} MiB/s  busy threads kept {statistics.median(x[1] for x in v):5.2f}  cpu/GiB {statistics.median(x[2] for x in v):4.1f}  n={len(v)}")
