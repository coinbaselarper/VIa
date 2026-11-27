#!/usr/bin/env python3
"""Boot-path pipeline list from the title's census logs (port/run/logs/r4pso-{a,b,c}.log, written by the census build
spike/r4-pso-census: every graphics PSO creation with its state and shader names, every compute creation, and per
frame the PSO ids drawn / dispatched).

  warmup_list.py --logs 'port/run/logs/r4pso-*.log' --manifest runs/main/manifest.tsv --out warmup/boot_pipelines.json

Tiers, in the order a loader should build them:
  1  used: drawn or dispatched in the boot window of every census run (first-use order = median first-use time)
  2  siblings of tier 1 and the UI / movie / blit families: the first `--tier2` graphics creations of run a that are
     not exe-embedded, in creation order (the title creates families together: blit, debug draw, Bink, default UI,
     Scaleform), duplicates of the same (vs, ps, state) removed
  3  compute programs the layer's own warm-up compiled at boot (pipeline-compile-begin trigger=warmup), in log order
Rows carry the logged state (D3D12 enums as logged) and the catalog entry (key, metallib) when the manifest has it.
"""
import argparse, collections, glob, json, os, re, statistics, sys

GPSO = re.compile(r"\[r4\] gpso #(\d+) (\w+) .*? id=(0x[0-9a-f]+) topo=(\d+) cons=(\d+) fill=(\d+) rts=(\d+) rt0=(\d+) "
                  r"rt1=(\d+) dsv=(\d+) samp=(\d+) ia=(\d+) blend=(\S+) refusal=(.*?) vs=(\S*) ps=(\S*)")
FRAME = re.compile(r"\[r4\] frame (\d+) t=([\d.]+) .*?draws\(id:n/verts\): (.*?) \| dispatches:(.*)")
CPSO = re.compile(r"\[r4\] cpso #(\d+) (\w+) hr=\S+ id=(0x[0-9a-f]+) bytes=(\d+) (\S+)")
WARM = re.compile(r"\[rt\] pipeline-compile-begin trigger=(\w+) .*?variant=(\d+) .*?shaders=(\S+)")


def parse_run(path):
    gp, order, cp, first, cfirst, warm = {}, [], {}, {}, {}, []
    for l in open(path, errors="replace"):
        if l.startswith("[r4] gpso #"):
            m = GPSO.match(l)
            if m:
                g = m.groups()
                gp[g[2]] = {"n": int(g[0]), "result": g[1], "topo": int(g[3]), "cons": int(g[4]), "fill": int(g[5]),
                            "rts": int(g[6]), "rt0": int(g[7]), "rt1": int(g[8]), "dsv": int(g[9]), "samp": int(g[10]),
                            "ia": int(g[11]), "blend": g[12], "refusal": g[13], "vs": g[14], "ps": g[15]}
                order.append(g[2])
        elif l.startswith("[r4] frame"):
            m = FRAME.match(l)
            if m:
                for d in m.group(3).split():
                    first.setdefault("0x" + d.split(":")[0], float(m.group(2)))
                for d in m.group(4).split():
                    cfirst.setdefault("0x" + d.split(":")[0], float(m.group(2)))
        elif l.startswith("[r4] cpso"):
            m = CPSO.match(l)
            if m:
                cp[m.group(3)] = m.group(5)
        elif "pipeline-compile-begin" in l:
            m = WARM.search(l)
            if m and m.group(1) == "warmup":
                warm.append(m.group(3))
    return {"gp": gp, "order": order, "cp": cp, "first": first, "cfirst": cfirst, "warm": warm}


def state_key(g):
    return (g["vs"], g["ps"], g["topo"], g["rts"], g["rt0"], g["rt1"], g["dsv"], g["samp"], g["ia"], g["blend"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tier2", type=int, default=120)
    ap.add_argument("--tier3", type=int, default=268)
    a = ap.parse_args()
    runs = [parse_run(p) for p in sorted(glob.glob(a.logs))]
    if not runs:
        sys.exit("no logs")
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(a.manifest)), "..", ".."))
    cat = {}
    for l in open(a.manifest):
        f = l.rstrip("\n").split("\t")
        if len(f) >= 11 and not (l.startswith("# ") and "\t" not in l):
            cat[f[0]] = {"key": f[1], "stage": f[2], "metallib": f[4]}

    def entry(name):
        e = cat.get(name)
        return {"name": name, "key": e["key"], "metallib": e["metallib"]} if e else {"name": name, "key": None, "metallib": None}

    # tier 1: ids used in every run, keyed by state so ids of different runs line up
    used = collections.defaultdict(list)
    for r in runs:
        for i, t in r["first"].items():
            g = r["gp"].get(i)
            if g:
                used[state_key(g)].append((t, g))
    rows, seen = [], set()
    for k, v in sorted(used.items(), key=lambda kv: statistics.median(t for t, _ in kv[1])):
        g = v[0][1]
        rows.append({"tier": 1, "kind": "graphics", "first_use_s_median": round(statistics.median(t for t, _ in v), 1),
                     "runs_used": len(v), "state": {x: g[x] for x in ("topo", "cons", "fill", "rts", "rt0", "rt1", "dsv", "samp", "ia", "blend")},
                     "vs": entry(g["vs"]), "ps": entry(g["ps"])})
        seen.add(k)
    cused = collections.defaultdict(list)
    for r in runs:
        for i, t in r["cfirst"].items():
            n = r["cp"].get(i)
            if n:
                cused[n].append(t)
    for n, ts in sorted(cused.items(), key=lambda kv: statistics.median(kv[1])):
        rows.append({"tier": 1, "kind": "compute", "first_use_s_median": round(statistics.median(ts), 1), "runs_used": len(ts),
                     "cs": entry(n)})
    # tier 2
    r0 = runs[0]
    n2 = 0
    for i in r0["order"]:
        g = r0["gp"][i]
        if g["result"] != "ok" or g["vs"].startswith("exe/") or state_key(g) in seen:
            continue
        seen.add(state_key(g))
        rows.append({"tier": 2, "kind": "graphics", "creation_index": g["n"],
                     "state": {x: g[x] for x in ("topo", "cons", "fill", "rts", "rt0", "rt1", "dsv", "samp", "ia", "blend")},
                     "vs": entry(g["vs"]), "ps": entry(g["ps"])})
        n2 += 1
        if n2 >= a.tier2:
            break
    # tier 3
    done = set(r["cs"]["name"] for r in rows if r["kind"] == "compute")
    n3 = 0
    for n in r0["warm"]:
        if n in done:
            continue
        done.add(n)
        rows.append({"tier": 3, "kind": "compute", "cs": entry(n)})
        n3 += 1
        if n3 >= a.tier3:
            break
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    out = {"source_logs": sorted(glob.glob(a.logs)), "runs": len(runs),
           "boot_window_s": [min(min(r["first"].values(), default=0) for r in runs), max(max(list(r["first"].values()) + list(r["cfirst"].values()), default=0) for r in runs)],
           "counts": {f"tier{t}_{k}": n for (t, k), n in sorted(collections.Counter((r["tier"], r["kind"]) for r in rows).items())},
           "unresolved": sum(1 for r in rows for k in ("vs", "ps", "cs") if k in r and r[k]["key"] is None),
           "pipelines": rows}
    json.dump(out, open(a.out, "w"), indent=1)
    index = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "spike", "s2-make-game",
                                        "work", "shaders", "spirv", "index.json")))
    lib_index = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "spike",
                                            "s2-make-game", "work", "shaders", "spirv", "library", "index.json")))
    with open(os.path.splitext(a.out)[0] + ".tsv", "w") as f:
        f.write("# G tier vs ps topology_type rts rt0 rt1 dsv samples blend(enable/src/dst D3D12_BLEND) inputs(register:components:type;...)\n"
                "# C tier cs local_size_x local_size_y local_size_z\n")
        for r in rows:
            if r["kind"] == "compute":
                ent = index.get(r["cs"]["name"]) or lib_index.get(r["cs"]["name"].split("/", 1)[-1], {})
                ls = ent.get("local_size") or [0, 0, 0]
                f.write("\t".join(["C", str(r["tier"]), r["cs"]["name"]] + [str(x) for x in ls]) + "\n")
                continue
            st = r["state"]
            sig = index.get(r["vs"]["name"], {}).get("input_signature", [])
            ins = ";".join("%d:%d:%s" % (e["register"], bin(e["mask"]).count("1"), e["component_type"]) for e in sig
                           if e.get("system_value", 0) == 0 and e["mask"]) or "-"
            f.write("\t".join(["G", str(r["tier"]), r["vs"]["name"], r["ps"]["name"], str(st["topo"]), str(st["rts"]),
                               str(st["rt0"]), str(st["rt1"]), str(st["dsv"]), str(st["samp"]), st["blend"], ins]) + "\n")
    print(json.dumps({k: out[k] for k in ("runs", "boot_window_s", "counts", "unresolved")}))


if __name__ == "__main__":
    main()
