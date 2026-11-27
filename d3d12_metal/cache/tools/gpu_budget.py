#!/usr/bin/env python3
"""GPU frame budget from GTA6RT_METAL_PASS_TIMING_TSV (+ its .cb file) in budget windows (MODES 'b').

usage: gpu_budget.py PASSES.tsv [--window N ...] [--top 25] [--json OUT]

Every timed encoder stage (render: vertex start..end, end..fragment start, fragment start..end; compute / blit:
start..end) and every command buffer of every queue (GPUStartTime..GPUEndTime) is placed on one clock (counter ticks
are fitted to CACurrentMediaTime with the .cb file's calibration rows). Over the window, from the first timed command
buffer's GPU start to the last one's GPU end, each instant is given to the encoder stages running then (shared
equally when several run at once); an instant with none goes to "in a command buffer, no timed encoder" when some
command buffer is on the GPU, else to "GPU idle" split by whether the next command buffer had already been committed
(kernelStartTime) or not. The rows therefore sum to the window exactly.
"""
import argparse
import bisect
import collections
import json
import re
import sys


def family(label):
    s = label.strip()
    s = re.sub(r"\s*\(.*\)$", "", s)
    s = s.split("/")[-1]
    s = s.lstrip("+_!'()%;$&*,.0123456789-:#@^")
    s = re.sub(r"_[0-9a-f]{8}(_Wrapped)?(~\d+)?$", "", s)
    s = re.sub(r"(_Wrapped)?~\d+$", "", s)
    return s or "(none)"


def load(path):
    rows = []
    with open(path) as f:
        head = f.readline().rstrip("\n").split("\t")
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) < len(head):
                continue
            rows.append(dict(zip(head, p)))
    cbs, cal = [], []
    with open(path + ".cb") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if p[0] == "cal" and len(p) == 5 and p[1] != "media_before":
                cal.append((float(p[1]), int(p[2]), int(p[3]), float(p[4])))
            elif p[0] == "cb" and len(p) >= 10 and p[1] != "window":
                cbs.append(dict(window=int(p[1]), queue=p[2], key=p[3], serial=int(p[4]), ks=float(p[5]),
                                ke=float(p[6]), gs=float(p[7]), ge=float(p[8]), status=int(p[9]),
                                summary=p[10] if len(p) > 10 else "",
                                enc=float(p[11]) if len(p) > 12 else 0.0,
                                held=float(p[12]) / 1e3 if len(p) > 12 else 0.0))
    return rows, cbs, cal


def fit(cal):
    xs = [g for _, _, g, _ in cal]
    ys = [(a + b) / 2 for a, _, _, b in cal]
    n = len(xs)
    if n < 2:
        sys.exit("fewer than 2 calibration rows")
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    a = my - b * mx
    res = max(abs(a + b * x - y) for x, y in zip(xs, ys))
    half = max((d - c) / 2 for c, _, _, d in cal)
    return a, b, res, half


def budget(rows, cbs, a, b, window, top):
    tick = lambda t: a + b * t
    stages = []
    timed_cbs = set()
    enc = collections.Counter()
    for r in rows:
        if int(r["window"]) != window:
            continue
        gs, ge = float(r["cb_gpu_start"]), float(r["cb_gpu_end"])
        if gs > 0 and ge > gs:
            timed_cbs.add((r["queue"], int(r["cb"]), gs, ge))
        t = [int(r[k]) for k in ("t0", "t1", "t2", "t3")]
        kind = r["kind"]
        fam = family(r["first"]) if r["first"] else "(no pipeline)"
        work = int(r["work"])
        enc[kind] += 1
        if kind == "R":
            if work == 0:
                base = "render: clear/load-store only"
            else:
                base = None
            if t[0] and t[1] and t[1] >= t[0]:
                stages.append((tick(t[0]), tick(t[1]), base or "render vertex/mesh: " + fam, r))
            if t[1] and t[2] and t[2] > t[1]:
                stages.append((tick(t[1]), tick(t[2]), base or "render between stages: " + fam, r))
            if t[2] and t[3] and t[3] >= t[2]:
                stages.append((tick(t[2]), tick(t[3]), base or "render fragment: " + fam, r))
        elif kind == "C":
            if t[0] and t[1] and t[1] >= t[0]:
                pre = r["first"].startswith("gta6rt_") or "gta6rt_" in r["first"]
                name = ("compute pre-pass (layer): " if pre else "compute: ") + fam
                if fam != family(r["last"]) and r["last"]:
                    name += " .. " + family(r["last"])
                stages.append((tick(t[0]), tick(t[1]), name, r))
        elif kind == "B":
            if t[0] and t[1] and t[1] >= t[0]:
                stages.append((tick(t[0]), tick(t[1]), "blit (layer copies/fills/resolves)", r))
    if not timed_cbs:
        return None
    t_lo = min(c[2] for c in timed_cbs)
    t_hi = max(c[3] for c in timed_cbs)
    pres = sorted(c["ge"] for c in cbs if c["queue"] == "present" and t_lo <= c["ge"] < t_hi)
    marks = [t for i, t in enumerate(pres) if i == 0 or t - pres[i - 1] > 0.03]
    if len(marks) >= 3:
        t_lo, t_hi = marks[0], marks[-1]
        presents = len(marks) - 1
    else:
        presents = len(marks)
    wcbs = [c for c in cbs if c["gs"] > 0 and c["ge"] > c["gs"] and c["ge"] > t_lo and c["gs"] < t_hi]
    committed = sorted(c["ks"] for c in cbs if c["ks"] > 0)
    ev = []
    for i, (s, e, name, _) in enumerate(stages):
        s, e = max(s, t_lo), min(e, t_hi)
        if e > s:
            ev.append((s, 1, i))
            ev.append((e, -1, i))
    cb_ev = []
    for c in wcbs:
        cb_ev.append((max(c["gs"], t_lo), 1))
        cb_ev.append((min(c["ge"], t_hi), -1))
    pts = sorted(set([t_lo, t_hi] + [x[0] for x in ev] + [x[0] for x in cb_ev]))
    ev.sort()
    cb_ev.sort()
    acc = collections.defaultdict(float)
    excl = collections.defaultdict(float)
    active = set()
    cb_active = 0
    ei = ci = 0
    pending = sorted((c["ks"], c["gs"]) for c in cbs if c["ks"] > 0 and c["gs"] > 0)
    by_start = sorted((c for c in cbs if c["gs"] > 0 and c["ge"] > c["gs"]), key=lambda c: c["gs"])
    starts = [c["gs"] for c in by_start]
    for k in range(len(pts) - 1):
        t0, t1 = pts[k], pts[k + 1]
        while ei < len(ev) and ev[ei][0] <= t0:
            _, d, i = ev[ei]
            if d > 0:
                active.add(i)
            else:
                active.discard(i)
            ei += 1
        while ci < len(cb_ev) and cb_ev[ci][0] <= t0:
            cb_active += cb_ev[ci][1]
            ci += 1
        dt = t1 - t0
        if dt <= 0:
            continue
        if active:
            share = dt / len(active)
            for i in active:
                acc[stages[i][2]] += share
            if len(active) == 1:
                excl[stages[next(iter(active))][2]] += dt
            acc["~overlap (time with >1 stage running)"] += 0
            if len(active) > 1:
                excl["~overlap (time with >1 stage running)"] += dt
        elif cb_active > 0:
            acc["in a command buffer, no timed encoder running"] += dt
        else:
            j = bisect.bisect_left(starts, t1 - 1e-9)
            nxt = by_start[j] if j < len(by_start) else None
            if not nxt or not nxt["enc"]:
                mid = (t0 + t1) / 2
                waiting = any(ks <= mid < gs for ks, gs in pending)
                acc["GPU idle: a committed command buffer not started" if waiting else
                    "GPU idle: nothing committed (no encode times)"] += dt
            else:
                encd, com = nxt["enc"], nxt["enc"] + nxt["held"]
                parts = [(t0, encd, "GPU idle: next command buffer not encoded yet (title / translation CPU)"),
                         (encd, com, "GPU idle: next command buffer held on the CPU by the commit gate (its wait)"),
                         (com, t1, "GPU idle: commit to GPU start (submission latency)")]
                for a0, a1, name in parts:
                    a0, a1 = max(a0, t0), min(a1, t1)
                    if a1 > a0:
                        acc[name] += a1 - a0
    total = t_hi - t_lo
    per_queue = collections.defaultdict(float)
    for q in sorted(set(c["queue"] for c in wcbs)):
        spans = sorted((max(c["gs"], t_lo), min(c["ge"], t_hi)) for c in wcbs if c["queue"] == q)
        end = t_lo
        for s, e in spans:
            s = max(s, end)
            if e > s:
                per_queue[q] += e - s
                end = e
    allspans = sorted((max(c["gs"], t_lo), min(c["ge"], t_hi)) for c in wcbs)
    any_busy, end = 0.0, t_lo
    for s, e in allspans:
        s = max(s, end)
        if e > s:
            any_busy += e - s
            end = e
    return dict(window=window, seconds=total, presents=presents, acc=dict(acc), excl=dict(excl),
                per_queue=dict(per_queue), any_cb_busy=any_busy, encoders=dict(enc), cbs=len(wcbs))


def group(name):
    if name.startswith("render vertex/mesh"):
        return "render vertex/mesh stage"
    if name.startswith("render fragment"):
        return "render fragment stage"
    if name.startswith("render between"):
        return "render, between vertex end and fragment start"
    if name.startswith("render: clear"):
        return "render passes with no draw (clear / load-store only)"
    if name.startswith("compute pre-pass"):
        return "layer pre-passes (indirect / rect compute)"
    if name.startswith("compute"):
        return "compute (title kernels)"
    return name


def shaders(rows, cbs, window):
    """Split windows ('s'): each render segment is one pipeline's draws, each compute encoder one kernel's dispatches."""
    pres = sorted(c["ge"] for c in cbs if c["queue"] == "present")
    lo = hi = None
    sel = [r for r in rows if int(r["window"]) == window]
    if not sel:
        return None
    t_lo = min(float(r["cb_gpu_start"]) for r in sel if float(r["cb_gpu_start"]) > 0)
    t_hi = max(float(r["cb_gpu_end"]) for r in sel)
    marks = [t for i, t in enumerate(pres) if (i == 0 or t - pres[i - 1] > 0.03) and t_lo <= t < t_hi]
    frames = max(1, len(marks) - 1) if len(marks) >= 3 else max(1, len(marks))
    agg = collections.defaultdict(lambda: [0.0, 0.0, 0.0, 0, 0])
    for r in sel:
        ms = float(r["ms"])
        if ms < 0:
            continue
        if r["kind"] == "R" and int(r["work"]) > 0:
            v, f = max(0.0, float(r["vertex_ms"])), max(0.0, float(r["fragment_ms"]))
            a = agg[("R", r["first"])]
            a[0] += v
            a[1] += f
            a[3] += int(r["groups"])
            a[4] += 1
        elif r["kind"] == "C" and int(r["work"]) > 0:
            a = agg[("C", r["first"])]
            a[2] += ms
            a[4] += 1
    return frames, agg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv")
    ap.add_argument("--window", type=int, action="append")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--json")
    ap.add_argument("--shaders", type=int, default=0, help="top N pipelines of the split windows")
    args = ap.parse_args()
    rows, cbs, cal = load(args.tsv)
    a, b, res, half = fit(cal)
    print(f"clock fit: {len(cal)} rows, {b * 1e9:.4f} ns a tick, max residual {res * 1e6:.1f} us "
          f"(sampling half-width {half * 1e6:.1f} us)")
    modes = {}
    for r in rows:
        modes[int(r["window"])] = r["mode"]
    wins = args.window or sorted(w for w, m in modes.items() if m == "b")
    out = []
    for w in wins:
        res_w = budget(rows, cbs, a, b, w, args.top)
        if not res_w:
            continue
        out.append(res_w)
        f = max(1, res_w["presents"])
        print(f"\nwindow {w} (mode {modes.get(w)}): {res_w['seconds'] * 1e3:.0f} ms, {res_w['presents']} presents, "
              f"{res_w['seconds'] * 1e3 / f:.1f} ms a frame; command buffers {res_w['cbs']}; encoders {res_w['encoders']}")
        print(f"  any queue's command buffers on the GPU {res_w['any_cb_busy'] * 1e3 / f:.1f} ms a frame "
              f"({100 * res_w['any_cb_busy'] / res_w['seconds']:.1f}%); by queue: " +
              ", ".join(f"{q} {v * 1e3 / f:.1f}" for q, v in sorted(res_w["per_queue"].items(), key=lambda x: -x[1])))
        groups = collections.defaultdict(float)
        for k, v in res_w["acc"].items():
            groups[group(k)] += v
        tot = sum(groups.values())
        print(f"  sum of rows {tot * 1e3 / f:.1f} ms a frame (window {res_w['seconds'] * 1e3 / f:.1f})")
        for k, v in sorted(groups.items(), key=lambda x: -x[1]):
            print(f"    {v * 1e3 / f:8.1f} ms  {100 * v / tot:5.1f}%  {k}")
        print("  top rows:")
        for k, v in sorted(res_w["acc"].items(), key=lambda x: -x[1])[: args.top]:
            print(f"    {v * 1e3 / f:8.2f} ms  (alone {res_w['excl'].get(k, 0) * 1e3 / f:7.2f})  {k}")
    if len(out) > 1:
        frames = sum(max(1, o["presents"]) for o in out)
        secs = sum(o["seconds"] for o in out)
        groups = collections.defaultdict(float)
        rows_all = collections.defaultdict(float)
        for o in out:
            for k, v in o["acc"].items():
                groups[group(k)] += v
                rows_all[k] += v
        busy = sum(o["any_cb_busy"] for o in out)
        print(f"\nwindows {[o['window'] for o in out]} together: {frames} frames, {secs * 1e3 / frames:.1f} ms a frame; "
              f"command buffers on the GPU {busy * 1e3 / frames:.1f} ms a frame ({100 * busy / secs:.1f}%)")
        for k, v in sorted(groups.items(), key=lambda x: -x[1]):
            print(f"    {v * 1e3 / frames:8.1f} ms  {100 * v / secs:5.1f}%  {k}")
        print("  top rows:")
        for k, v in sorted(rows_all.items(), key=lambda x: -x[1])[: args.top]:
            print(f"    {v * 1e3 / frames:8.2f} ms  {k}")
    if args.shaders:
        tot = collections.defaultdict(lambda: [0.0, 0.0, 0.0, 0, 0])
        nwin = 0
        for w in sorted(w for w, m in modes.items() if m == "s"):
            got = shaders(rows, cbs, w)
            if not got:
                continue
            frames, agg = got
            nwin += 1
            for k, v in agg.items():
                t = tot[k]
                for i in range(3):
                    t[i] += v[i] / frames
                t[3] += v[3] / frames
                t[4] += v[4] / frames
        if nwin:
            print(f"\nsplit windows ({nwin}), ms a frame averaged over them (serialized: each segment waits for all before it)")
            rows_out = []
            for (kind, name), v in tot.items():
                rows_out.append((sum(v[:3]) / nwin, kind, name, v[0] / nwin, v[1] / nwin, v[2] / nwin, v[3] / nwin, v[4] / nwin))
            rows_out.sort(reverse=True)
            print(f"  {'ms':>7} {'vertex':>7} {'frag':>7} {'compute':>7} {'groups':>8} {'segs':>6}  pipeline")
            for t, kind, name, v, f, c, g, n in rows_out[: args.shaders]:
                print(f"  {t:7.2f} {v:7.2f} {f:7.2f} {c:7.2f} {g:8.0f} {n:6.1f}  {kind} {name}")
            print(f"  total {sum(r[0] for r in rows_out):.1f} ms a frame over {len(rows_out)} pipelines")
    if args.json:
        with open(args.json, "w") as fo:
            json.dump(out, fo, indent=1)


if __name__ == "__main__":
    main()
