#!/usr/bin/env python3
"""Rank shaders by thread-private memory: Function/Private variables of aggregate type in the SPIR-V the recipe
feeds SPIRV-Cross. A variable whose access chains all use constant indices is register-promotable ("const");
one indexed by a run-time value ("dyn") has to live in the per-thread stack the driver backs with scratch.

  tmem_rank.py --specs LIST.specs --catalog CATALOG.tsv [--corpus DIR] --out OUT.tsv [--json OUT.json]
  tmem_rank.py --spv X.spv            one shader, details
"""
import argparse, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spv  # noqa: E402

CORPUS = "/Users/a/Desktop/Projects/GTA-VI/spike/s2-make-game/work/shaders/spirv"
OP_TYPE_MATRIX, OP_TYPE_RUNTIME_ARRAY, OP_FUNCTION_END, OP_FUNCTION_CALL = 24, 29, 56, 57
OP_PTR_ACCESS_CHAIN, OP_COPY_MEMORY, OP_CONSTANT_NULL, OP_CONSTANT_COMPOSITE = 67, 63, 46, 44
CHAINS = (spv.OP_ACCESS_CHAIN, spv.OP_IN_BOUNDS_ACCESS_CHAIN, OP_PTR_ACCESS_CHAIN, 70)


def analyze(words):
    types, consts, var_info, names = {}, set(), {}, {}
    fn, fn_of, calls, model = None, {}, {}, None
    chain_base, dyn, uses = {}, set(), {}
    for i, op, n in spv.insts(words):
        w = words[i:i + n]
        if op == spv.OP_ENTRY:
            model = spv.MODELS.get(w[1], str(w[1]))
        elif op == spv.OP_NAME:
            names[w[1]] = spv.lit_str(words, i + 2)
        elif op in (20, 21, 22, 23, 24, 28, 29, 30, 32):
            types[w[1]] = (op, list(w[2:]))
        elif op in (spv.OP_CONSTANT, 41, 42, OP_CONSTANT_NULL, OP_CONSTANT_COMPOSITE, 50):
            consts.add(w[2])
            if op == spv.OP_CONSTANT:
                types.setdefault(("c", w[2]), (None, list(w[3:])))
        elif op == spv.OP_FUNCTION:
            fn = w[2]
            calls.setdefault(fn, [])
        elif op == OP_FUNCTION_END:
            fn = None
        elif op == OP_FUNCTION_CALL:
            calls[fn].append(w[3])
            for a in w[4:]:
                r = chain_base.get(a, a)
                if r in var_info:
                    uses.setdefault(r, set()).add("call")
        elif op == spv.OP_VARIABLE:
            sc = w[3]
            if sc in (spv.SC_PRIVATE, spv.SC_FUNCTION):
                var_info[w[2]] = {"ptype": w[1], "sc": sc, "fn": fn, "init": len(w) > 4}
        elif op in CHAINS:
            base = w[3]
            root = chain_base.get(base, base)
            if root in var_info:
                chain_base[w[2]] = root
                idx = w[4:] if op not in (OP_PTR_ACCESS_CHAIN, 70) else w[5:]
                if any(x not in consts for x in idx):
                    dyn.add(root)
        elif op == OP_COPY_MEMORY:
            for a in w[1:3]:
                r = chain_base.get(a, a)
                if r in var_info:
                    uses.setdefault(r, set()).add("copy")

    def size(t):
        op, a = types[t]
        if op == 20:
            return 4
        if op in (21, 22):
            return a[0] // 8
        if op == 23:
            return size(a[0]) * (4 if a[1] == 3 else a[1])
        if op == OP_TYPE_MATRIX:
            return size(a[0]) * a[1]
        if op == 28:
            return types[("c", a[1])][1][0] * size(a[0])
        if op == 30:
            return sum(size(m) for m in a)
        return 0

    def aggregate(t):
        return types[t][0] in (28, 30, OP_TYPE_MATRIX)

    entry = None
    for i, op, n in spv.insts(words):
        if op == spv.OP_ENTRY:
            entry = words[i + 2]
            break
    reach, stack = set(), [entry]
    while stack:
        f = stack.pop()
        if f in reach:
            continue
        reach.add(f)
        stack.extend(calls.get(f, []))
    ncalls = {}
    for f in reach:
        for c in calls.get(f, []):
            ncalls[c] = ncalls.get(c, 0) + 1
    vs = []
    for v, d in var_info.items():
        t = types[d["ptype"]][1][1]
        if not aggregate(t):
            continue
        if d["fn"] is not None and d["fn"] not in reach:
            continue
        vs.append({"id": v, "name": names.get(v, ""), "sc": "private" if d["sc"] == spv.SC_PRIVATE else "function",
                   "fn": d["fn"], "inlined_copies": 1 if d["fn"] in (None, entry) else ncalls.get(d["fn"], 1),
                   "bytes": size(t), "dyn": v in dyn, "uses": sorted(uses.get(v, ())), "init": d["init"]})
    dyn_b = sum(x["bytes"] * x["inlined_copies"] for x in vs if x["dyn"])
    all_b = sum(x["bytes"] * x["inlined_copies"] for x in vs)
    return {"model": model, "dyn_bytes": dyn_b, "agg_bytes": all_b, "vars": sorted(vs, key=lambda x: -x["bytes"])}


def read_catalog(path):
    rows = {}
    root = None
    for line in open(path):
        if line.startswith("# shaderc_msl manifest") and "root=" in line:
            root = line.split("root=")[1].strip()
        if line.startswith("#"):
            continue
        c = line.rstrip("\n").split("\t")
        rows.setdefault(c[0], []).append(c)
    base = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(path)), root or "."))
    return rows, base


def read_specs(path):
    out = {}
    for line in open(path):
        if line.startswith("#"):
            continue
        for tok in line.split():
            if "=" in tok and tok.split("=")[0] in ("vs", "ps", "cs", "ms", "as", "os") and "@" in tok:
                st, rest = tok.split("=", 1)
                name, kp = rest.rsplit("@", 1)
                out.setdefault((name, kp), st)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--specs")
    ap.add_argument("--catalog")
    ap.add_argument("--corpus", default=CORPUS)
    ap.add_argument("--out")
    ap.add_argument("--json")
    ap.add_argument("--spv")
    a = ap.parse_args()
    if a.spv:
        print(json.dumps(analyze(spv.load(a.spv)), indent=1))
        return 0
    specs = read_specs(a.specs)
    rows, base = read_catalog(a.catalog)
    res = []
    for (name, kp), st in sorted(specs.items()):
        dec = re.sub(r"~([0-9a-f]{2})", lambda m: chr(int(m.group(1), 16)), name)
        cand = rows.get(dec, []) + rows.get(name, [])
        row = next((r for r in cand if r[1].startswith(kp)), cand[-1] if cand else None)
        p = os.path.join(a.corpus, dec + ".spv")
        if not os.path.exists(p):
            res.append({"name": name, "kp": kp, "stage": st, "error": "no spv"})
            continue
        r = analyze(spv.load(p))
        r.update(name=name, kp=kp, stage=st, key=row[1] if row else None,
                 metallib=os.path.normpath(os.path.join(base, row[4])) if row else None,
                 passes=row[8] if row else None)
        res.append(r)
    res.sort(key=lambda r: (-r.get("dyn_bytes", 0), -r.get("agg_bytes", 0)))
    with open(a.out, "w") as f:
        f.write("# name\tstage\tkey\tdyn_bytes\tagg_bytes\tdyn_vars(name:bytes:sc:copies)\tmetallib\n")
        for r in res:
            if "error" in r:
                f.write("%s\t%s\t-\t-\t-\t%s\t-\n" % (r["name"], r["stage"], r["error"]))
                continue
            dv = ",".join("%s:%d:%s:%d" % (x["name"] or x["id"], x["bytes"], x["sc"][0], x["inlined_copies"])
                          for x in r["vars"] if x["dyn"])
            f.write("%s\t%s\t%s\t%d\t%d\t%s\t%s\n" % (r["name"], r["stage"], r["key"] or "-", r["dyn_bytes"],
                                                      r["agg_bytes"], dv or "-", r["metallib"] or "-"))
    if a.json:
        json.dump(res, open(a.json, "w"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
