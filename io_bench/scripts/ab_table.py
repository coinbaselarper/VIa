"""Median table for e13_ab_confirm.sh output: python3 -I ab_table.py RESULTS.txt"""
import collections
import statistics
import sys

rows = collections.defaultdict(list)
for line in open(sys.argv[1]):
    if not line.startswith("RESULT"):
        continue
    d = dict(kv.split("=", 1) for kv in line.split()[1:])
    rows[(d["req"], d["variant"])].append((float(d["out_mibs"]), float(d["cpu_s_per_gib_out"])))
for req in sorted({k[0] for k in rows}):
    a = [v[0] for v in rows[(req, "a")]]
    b = [v[0] for v in rows[(req, "b_fd")]]
    ca = statistics.median(v[1] for v in rows[(req, "a")])
    cb = statistics.median(v[1] for v in rows[(req, "b_fd")])
    print(f"{req}: baseline {statistics.median(a):.0f} MiB/s, live {statistics.median(b):.0f} MiB/s, x{statistics.median(b) / statistics.median(a):.2f}, "
          f"cpu/GiB {ca:.1f} -> {cb:.1f} s, n={len(a)}/{len(b)}, worst pairing x{min(b) / max(a):.2f}")
