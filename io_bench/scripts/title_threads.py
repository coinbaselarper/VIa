"""Per-thread CPU of the title from capture_title_shapes.sh's census: python3 -I title_threads.py THREADS.txt [FROM_S TO_S]

Prints, for the window between two samples, the threads (by their order in `ps -M`) that used CPU and what share of a core each used,
and a summary: how many threads were busy over 10%, 50%, and the total cores used.
"""
import re
import sys


def secs(t):
    m, s = t.split(":")
    return int(m) * 60 + float(s)


def load(path):
    samples = []
    cur = None
    for line in open(path):
        if line.startswith("## t="):
            cur = {"t": int(line.split()[1][2:]), "threads": []}
            samples.append(cur)
        elif cur is not None and re.match(r"^\s+\d+\s", line) or (cur is not None and re.match(r"^\S+\s+\d+\s+\?\?", line)):
            f = line.split()
            times = [x for x in f if re.fullmatch(r"\d+:\d+\.\d+", x)]
            if len(times) >= 2:
                cur["threads"].append(secs(times[0]) + secs(times[1]))
    return samples


def main():
    samples = load(sys.argv[1])
    if len(samples) < 2:
        sys.exit("fewer than two samples")
    t0 = samples[0]["t"]
    lo = float(sys.argv[2]) if len(sys.argv) > 2 else 0
    hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
    sel = [s for s in samples if lo <= s["t"] - t0 <= hi]
    if len(sel) < 2:
        sys.exit("window holds fewer than two samples")
    a, b = sel[0], sel[-1]
    dt = b["t"] - a["t"]
    n = min(len(a["threads"]), len(b["threads"]))
    use = [(b["threads"][i] - a["threads"][i]) / dt for i in range(n)]
    print(f"window +{a['t'] - t0}s..+{b['t'] - t0}s ({dt}s), threads at start {len(a['threads'])}, at end {len(b['threads'])}")
    busy = sorted((u, i) for i, u in enumerate(use) if u > 0.01)
    for u, i in reversed(busy[:40]):
        print(f"  thread #{i}: {u * 100:.0f}% of a core")
    print(f"threads over 10%: {sum(u > 0.10 for u in use)}, over 50%: {sum(u > 0.50 for u in use)}, total {sum(use):.2f} cores")
    for s in sel[1:]:
        k = min(len(s["threads"]), len(sel[0]["threads"]))
        print(f"  +{s['t'] - t0}s: threads={len(s['threads'])} cpu_total_s={sum(s['threads']):.1f}")


main()
