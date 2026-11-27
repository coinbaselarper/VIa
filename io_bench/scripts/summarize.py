"""Median over repeated trials of iobench RESULT lines.

usage: python3 -I summarize.py FILE [FILE...] --rows k1,k2 [--cols k3] --metric mibs[,p50_ms,...] [--where k=v,...] [--fmt 0]
Without --cols the metrics are columns. With --cols one metric is pivoted over the values of the column key.
"""
import re
import statistics
import sys


def parse(paths):
    rows = []
    for p in paths:
        for line in open(p):
            if not line.startswith("RESULT "):
                continue
            d = {}
            for m in re.finditer(r'(\w+)=("[^"]*"|\S+)', line):
                d[m.group(1)] = m.group(2).strip('"')
            rows.append(d)
    return rows


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def main(argv):
    files, opts, i = [], {}, 1
    while i < len(argv):
        if argv[i].startswith("--"):
            opts[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            files.append(argv[i])
            i += 1
    rows = parse(files)
    for cond in filter(None, opts.get("where", "").split(",")):
        k, v = cond.split("=", 1)
        rows = [r for r in rows if r.get(k) == v]
    rk = [k for k in opts.get("rows", "").split(",") if k]
    ck = opts.get("cols")
    metrics = [m for m in opts.get("metric", "mibs").split(",") if m]
    dec = int(opts.get("fmt", "0"))
    groups = {}
    for r in rows:
        key = tuple(r.get(k, "") for k in rk)
        col = r.get(ck, "") if ck else ""
        groups.setdefault((key, col), []).append(r)

    def med(rs, m):
        v = [num(r.get(m)) for r in rs]
        v = [x for x in v if x is not None]
        return statistics.median(v) if v else float("nan")

    def sort_key(t):
        return [(0, num(x)) if num(x) is not None else (1, x) for x in t]

    keys = sorted({k for k, _ in groups}, key=sort_key)
    if ck:
        cols = sorted({c for _, c in groups}, key=lambda c: (0, num(c)) if num(c) is not None else (1, c))
        print("| " + " | ".join(rk) + " | " + " | ".join(f"{ck}={c}" for c in cols) + " |")
        print("|" + "---|" * (len(rk) + len(cols)))
        for k in keys:
            cells = []
            for c in cols:
                rs = groups.get((k, c))
                cells.append("" if not rs else f"{med(rs, metrics[0]):.{dec}f}")
            print("| " + " | ".join(k) + " | " + " | ".join(cells) + " |")
    else:
        print("| " + " | ".join(rk) + " | " + " | ".join(metrics) + " | trials |")
        print("|" + "---|" * (len(rk) + len(metrics) + 1))
        for k in keys:
            rs = groups[(k, "")]
            print("| " + " | ".join(k) + " | " + " | ".join(f"{med(rs, m):.{dec}f}" for m in metrics) + f" | {len(rs)} |")


main(sys.argv)
