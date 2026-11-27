"""Summarise a LEONIDA_DSTORAGE_SHAPES trace (written by port/shims_gdk/src/dstorage_shapes.h).

usage: python3 -I dstorage_shapes.py TRACE [--game DIR] [--replay-tsv OUT] [--json OUT]

Prints the queues the title created, what it asked of each (codec mix, sizes, destination kinds, files), how deep its queues were and
how long requests took from EnqueueRequest to publication. --replay-tsv writes the zlib and raw file requests as
`path offset src dst queue` lines, the input format of tests/dstorage_replay_bench.cpp (the queue column is optional there).
"""
import argparse
import collections
import json
import os
import sys

BC_NAMES = {0: "-", 1: "bc1", 2: "bc2", 3: "bc3", 4: "bc4", 5: "bc5", 6: "bc6h", 7: "bc7"}
DEFAULT_GAME = "/Users/a/Desktop/Projects/GTA-VI/xbox/GTA6.Xbox.v1.0.2.1-CYBERLEEK"


def pct(values, p):
    if not values:
        return 0
    v = sorted(values)
    return v[min(len(v) - 1, int(len(v) * p / 100))]


def codec(options):
    parts = []
    if options & 1:
        parts.append("zlib")
    bc = (options >> 1) & 0xF
    if bc:
        parts.append(BC_NAMES.get(bc, f"bc{bc}"))
    if (options >> 5) & 0xFFFF:
        parts.append(f"swz{(options >> 5) & 0xFFFF:x}")
    return "+".join(parts) or "raw"


def load(path):
    queues, files, reqs, subs, done, cancels, closes = {}, {}, {}, [], [], [], {}
    pids = []
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("[rt] ds-shape "):
                line = line[len("[rt] ds-shape "):]
            if line.startswith("# ds-shapes"):
                pids.append(line)
                continue
            c = line.split("\t")
            k = c[0]
            try:
                if k == "Q":
                    queues[c[2]] = dict(t=int(c[1]), source=c[3], capacity=int(c[4]), priority=int(c[5]), auto_off=int(c[6]), name=c[7], tid=int(c[8]))
                elif k == "F":
                    files[int(c[1])] = c[2]
                elif k == "R":
                    reqs[(c[2], int(c[3]))] = dict(t=int(c[1]), q=c[2], seq=int(c[3]), tid=int(c[4]), options=int(c[5], 16), source=c[6], file=int(c[7]),
                                                   off=int(c[8]), src=int(c[9]), mid=int(c[10]), dst=int(c[11]), dest=c[12], tag=int(c[13], 16), depth=int(c[14]))
                elif k == "S":
                    subs.append(dict(t=int(c[1]), q=c[2], requests=int(c[3]), entries=int(c[4]), src=int(c[5]), dst=int(c[6])))
                elif k == "C":
                    done.append(dict(t=int(c[1]), q=c[2], seq=int(c[3]), hr=c[4], latency=int(c[5]), read=int(c[6]), decode=int(c[7]), via=c[8], window=int(c[9]), cancelled=int(c[10])))
                elif k == "X":
                    cancels.append(dict(t=int(c[1]), q=c[2], mask=c[3], value=c[4], matched=int(c[5])))
                elif k == "Z":
                    closes[c[2]] = dict(t=int(c[1]), requests=int(c[3]), src=int(c[4]), max_window=int(c[5]))
            except (IndexError, ValueError):
                print(f"skipped malformed line: {line[:120]}", file=sys.stderr)
    return pids, queues, files, reqs, subs, done, cancels, closes


def size_line(values):
    if not values:
        return "n=0"
    return f"n={len(values)} p50={pct(values, 50)} p90={pct(values, 90)} p99={pct(values, 99)} max={max(values)} total_MiB={sum(values) / 1048576:.1f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--game", default=os.environ.get("IOB_GAME", DEFAULT_GAME))
    ap.add_argument("--replay-tsv")
    ap.add_argument("--json")
    a = ap.parse_args()
    pids, queues, files, reqs, subs, done, cancels, closes = load(a.trace)
    print(f"{len(pids)} process header(s), {len(queues)} queues, {len(reqs)} requests, {len(done)} completions, {len(subs)} submits, {len(cancels)} cancels, {len(files)} files")
    summary = {"queues": {}}
    by_q = collections.defaultdict(list)
    for r in reqs.values():
        by_q[r["q"]].append(r)
    lat_by = collections.defaultdict(list)
    done_by = collections.defaultdict(list)
    for d in done:
        lat_by[d["q"]].append(d["latency"])
        done_by[d["q"]].append(d)
    subs_by = collections.defaultdict(list)
    for s in subs:
        subs_by[s["q"]].append(s)
    for q, info in queues.items():
        rs = by_q.get(q, [])
        print(f"\nqueue {q} name={info['name']} source={info['source']} capacity={info['capacity']} priority={info['priority']} auto_submit_off={info['auto_off']} created_by_tid={info['tid']} at +{info['t'] / 1e6:.2f}s")
        z = closes.get(q)
        if z:
            print(f"  closed at +{z['t'] / 1e6:.2f}s: {z['requests']} requests, {z['src'] / 1048576:.1f} MiB source, max window {z['max_window']}")
        if not rs:
            continue
        span = (max(r["t"] for r in rs) - min(r["t"] for r in rs)) / 1e6
        print(f"  requests {len(rs)} over {span:.2f}s enqueue span; enqueuing tids {dict(collections.Counter(r['tid'] for r in rs))}")
        mix = collections.Counter(codec(r["options"]) for r in rs)
        print("  codec mix:", ", ".join(f"{k} {v}" for k, v in mix.most_common()))
        print("  destination:", dict(collections.Counter(r["dest"] for r in rs)), " source:", dict(collections.Counter(r["source"] for r in rs)))
        print("  src bytes:", size_line([r["src"] for r in rs]))
        print("  dst bytes:", size_line([r["dst"] for r in rs]))
        print("  requests <= 16 KiB:", sum(r["src"] <= 16384 for r in rs), " bytes in requests >= 256 KiB:", f"{100 * sum(r['src'] for r in rs if r['src'] >= 262144) / max(1, sum(r['src'] for r in rs)):.0f}%")
        fc = collections.Counter(files.get(r["file"], "-") for r in rs)
        print("  files:", "; ".join(f"{k} x{v}" for k, v in fc.most_common(6)))
        depth = [r["depth"] for r in rs]
        print(f"  outstanding at enqueue: p50={pct(depth, 50)} p90={pct(depth, 90)} max={max(depth)}")
        ss = subs_by.get(q, [])
        if ss:
            print(f"  submits {len(ss)}: requests per submit p50={pct([s['requests'] for s in ss], 50)} p90={pct([s['requests'] for s in ss], 90)} max={max(s['requests'] for s in ss)}")
        ls = lat_by.get(q, [])
        if ls:
            print(f"  enqueue->publish latency us: p50={pct(ls, 50)} p90={pct(ls, 90)} p99={pct(ls, 99)} max={max(ls)}")
            ds = done_by[q]
            print(f"  read us p50={pct([d['read'] for d in ds], 50)} decode us p50={pct([d['decode'] for d in ds], 50)}; via {dict(collections.Counter(d['via'] for d in ds))}; window p50={pct([d['window'] for d in ds], 50)} max={max(d['window'] for d in ds)}")
            bad = collections.Counter(d["hr"] for d in ds if d["hr"] != "0x00000000")
            print("  failures:", dict(bad) if bad else "none")
        summary["queues"][q] = dict(info, requests=len(rs), codec=dict(mix), src_p50=pct([r["src"] for r in rs], 50), src_p90=pct([r["src"] for r in rs], 90),
                                    depth_p90=pct(depth, 90), depth_max=max(depth), latency_p50_us=pct(ls, 50), latency_p99_us=pct(ls, 99))
    if cancels:
        print(f"\ncancels: {len(cancels)} calls, {sum(c['matched'] for c in cancels)} requests matched")
    allr = list(reqs.values())
    if allr:
        print("\nall queues:", size_line([r["src"] for r in allr]), "| codec", dict(collections.Counter(codec(r["options"]) for r in allr)))
        span = max(1, max(r["t"] for r in allr) - min(r["t"] for r in allr)) / 1e6
        print(f"request rate {len(allr) / span:.0f}/s, source {sum(r['src'] for r in allr) / 1048576 / span:.1f} MiB/s over {span:.1f}s of enqueueing")
    if a.replay_tsv:
        n = 0
        order = {q: i for i, q in enumerate(sorted(queues, key=lambda q: queues[q]["t"]))}
        with open(a.replay_tsv, "w") as out:
            for r in sorted(allr, key=lambda r: r["t"]):
                if r["source"] != "file" or r["options"] & ~(1 | (1 << 21)) & 0xFFFFFF:
                    continue
                path = files.get(r["file"], "")
                low = path.replace("\\", "/")
                head = low[:5].upper()
                if head == "GAME:":
                    low = low[5:]
                elif low[1:2] == ":":
                    low = low[2:]
                rel = low.lstrip("/")
                while rel.startswith("./"):
                    rel = rel[2:].lstrip("/")
                real = os.path.join(a.game, rel)
                dst = r["dst"] if r["options"] & 1 else r["src"]
                out.write(f"{real}\t{r['off']}\t{r['src']}\t{dst}\t{order.get(r['q'], 0)}\n")
                n += 1
        print(f"\nwrote {n} zlib/raw file requests (path offset src dst queue) to {a.replay_tsv}")
    cpu = sum(d["read"] + d["decode"] for d in done)
    if done:
        print(f"read+decode time summed over requests: {cpu / 1e6:.1f} s (includes file IO waits)")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(summary, f, indent=1)


if __name__ == "__main__":
    main()
