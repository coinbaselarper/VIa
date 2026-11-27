#!/usr/bin/env python3
"""Binding fix check: stratified sample, compiled with the recipe (split pre-pass) and with the unmodified S0.4
recipe, both reflected by Metal (pipeline reflection), compared with index.json bindings.

Sample: every distinct (stage, bindings) signature gets one shader, then each stage is filled at random to its
quota (seeded). gs is included: it has no Metal stage, so it is reported as a cross failure.
Output: <out>/verify_bindings.jsonl (one row per shader) and summary.json.
"""
import argparse, collections, concurrent.futures as cf, hashlib, json, os, random, re, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe as P  # noqa: E402
import spv  # noqa: E402
import shaderc_msl as S  # noqa: E402

QUOTA = {"vs": 130, "ps": 170, "cs": 130, "ms": 150, "gs": 20}
SEED = 7101
BASE_FLAGS = ["--msl", "--msl-version", "30100", "--msl-argument-buffers", "--msl-argument-buffer-tier", "1",
              "--msl-device-argument-buffer", "0"]


def sample(index):
    rng = random.Random(SEED)
    by_sig = collections.defaultdict(list)
    for k in sorted(index):
        v = index[k]
        by_sig[(v["stage"], tuple(v.get("bindings", [])))].append(k)
    pick = {}
    for sig in sorted(by_sig):
        pick[rng.choice(by_sig[sig])] = "signature"
    for st, q in QUOTA.items():
        pool = sorted(k for k in index if index[k]["stage"] == st and k not in pick)
        have = sum(1 for k in pick if index[k]["stage"] == st)
        for k in rng.sample(pool, max(0, min(len(pool), q - have))):
            pick[k] = "random"
    return pick


def base_compile(spv_path, out_dir, key):
    msl = os.path.join(out_dir, key + ".base.metal")
    rc, _, err, _ = S.run([S.SPIRV_CROSS, spv_path, *BASE_FLAGS, "--output", msl], 300)
    if rc != 0:
        return None, None, err.decode(errors="replace")[:300]
    air = msl[:-6] + ".air"
    lib = msl[:-6] + ".metallib"
    rc, _, err, _ = S.run([S.tool("metal"), "-std=metal3.1", "-c", "-w", msl, "-o", air], 600)
    if rc != 0:
        return None, None, err.decode(errors="replace")[:300]
    rc, _, err, _ = S.run([S.tool("metallib"), air, "-o", lib], 600)
    os.remove(air)
    text = open(msl, encoding="utf-8", errors="replace").read()
    return lib, S.msl_plan(text), None


def arg_buffers(bindings):
    out = {}
    for b in bindings or []:
        m = re.match(r"spvDescriptorSet(\d+)$", b["name"])
        if m and b["type"] == 0:
            out[int(m.group(1))] = b
    return out


def discrete_buffers(bindings):
    out = {}
    for b in bindings or []:
        m = re.match(r"spvDescriptorSet(\d+)Binding0$", b["name"])
        if m and b["type"] == 0:
            out[int(m.group(1))] = b
    return out


def push_binding(bindings, plan):
    for b in bindings or []:
        if b["type"] == 0 and plan.get("push") is not None and b["index"] == plan["push"]:
            return b
    return None


def check_split(meta, row, refl):
    exp = sorted(meta.get("bindings", []))
    abufs = arg_buffers(refl.get("bindings"))
    disc = discrete_buffers(refl.get("bindings"))
    got = sorted(s - 1 for s in set(abufs) | set(disc))
    problems = []
    import batch as B
    pd = B.discrete_of(row["plan"])
    for s_, b in disc.items():
        if pd.get(s_) != b["index"]:
            problems.append(f"descriptor array set {s_} at buffer({b['index']}), plan has {pd.get(s_)}")
    if not set(got) <= set(exp):
        problems.append(f"bindings reflected {got} not within index {exp}")
    plan_sets = sorted(int(s) - 1 for s in list(row["plan"]["sets"]) + list(pd))
    if plan_sets != got:
        problems.append(f"MSL plan bindings {plan_sets} != reflected {got}")
    for s, b in abufs.items():
        if b["index"] != s:
            problems.append(f"set {s} at buffer({b['index']}), plan wants {s}")
        offs = sorted({m["offset"] for m in b.get("members", [])})
        if offs and offs != [0]:
            problems.append(f"set {s} members at offsets {offs}")
        if row["plan"]["sets"].get(str(s), row["plan"]["sets"].get(s)) != b["index"]:
            problems.append(f"set {s}: MSL plan {row['plan']['sets']} disagrees with reflection")
    pc = meta.get("push_constant_bytes", 0)
    pb = push_binding(refl.get("bindings"), row["plan"])
    if pc and (pb is None or pb.get("size") != pc):
        problems.append(f"push block size {pb and pb.get('size')} != {pc}")
    return got, problems


def check_base(meta, refl, plan):
    abufs = arg_buffers(refl.get("bindings"))
    exp = sorted(meta.get("bindings", []))
    if not exp:
        return {"sets": sorted(abufs), "overlap": False}
    s0 = abufs.get(0)
    members = s0.get("members", []) if s0 else []
    offsets = sorted({m["offset"] for m in members})
    return {"sets": sorted(abufs), "members": len(members), "distinct_offsets": len(offsets),
            "overlap": len(offsets) < len(exp)}


POINTER_OPERAND = {61: (3,), 62: (1,), 63: (1, 2), 65: (3,), 66: (3,), 67: (3,), 60: (3,), 68: (3,), 83: (3,),
                   **{op: (3,) for op in range(227, 243)}}


def binding_refs(spv_path):
    """Set-0 bindings whose variables are used through a pointer operand (load, store, copy, access chain, image texel
    pointer, array length, copy object, atomics) or passed to a function call."""
    words = spv.load(spv_path)
    sets, binds = {}, {}
    for i, op, n in spv.insts(words):
        if op == spv.OP_DECORATE and n >= 4:
            if words[i + 2] == spv.DEC_SET:
                sets[words[i + 1]] = words[i + 3]
            elif words[i + 2] == spv.DEC_BINDING:
                binds[words[i + 1]] = words[i + 3]
    var_b = {v: b for v, b in binds.items() if sets.get(v, 0) == 0}
    used = set()
    for i, op, n in spv.insts(words):
        if op in POINTER_OPERAND:
            ws = [words[i + k] for k in POINTER_OPERAND[op] if k < n]
        elif op == 57:
            ws = words[i + 4:i + n]
        else:
            continue
        for w in ws:
            if w in var_b:
                used.add(var_b[w])
    return sorted(set(var_b.values())), sorted(used)


def subset_problems(row):
    exp = sorted(row["index_bindings"])
    got = row.get("reflected_bindings", [])
    out = [p for p in row.get("problems", []) if not p.startswith("bindings reflected")]
    if not set(got) <= set(exp):
        out.append(f"bindings reflected {got} not within index {exp}")
    return out


def recheck(a, index):
    """Re-run the rows of a previous verify_bindings.jsonl that did not reach reflection, with the current recipe and
    the mesh_fit options the batch uses for mesh shaders; re-judge every row (reflected set must be a subset of
    index.json, and every declared binding Metal did not reflect must be unreferenced in the SPIR-V)."""
    import batch as B
    out = os.path.join(a.build, "verify_bindings")
    prev = [json.loads(l) for l in open(os.path.join(out, "verify_bindings.jsonl"))]
    mo = B.mesh_opts(os.path.join(a.build, "mesh", "mesh_fit.jsonl"))
    cas = os.path.join(a.build, "cas")
    tr = S.translator_id(a.spirv_dir)
    tmp = os.path.join(out, "tmp")
    pr = P.Probe()
    t0 = time.monotonic()
    rows, summ = [], collections.Counter()
    for row in prev:
        stem, st = row["stem"], row["stage"]
        meta = index[stem]
        spv_path = os.path.join(a.spirv_dir, stem + ".spv")
        rerun = st != "gs" and (row["status"] != "ok" or row.get("pso") != "OK" or a.recheck_all)
        if rerun:
            o = {"vs_route": "lane", **(mo.get(stem, {}) if st == "ms" else {})}
            r = S.compile_one(spv_path, st, cas, tr, o, tmp=os.path.join(tmp, "recheck"))
            new = {"stem": stem, "stage": st, "why": row["why"], "index_bindings": row["index_bindings"],
                   "status": r["status"], "key": r["key"], "rechecked": True,
                   "passes": [p["pass"] for p in r.get("passes", [])]}
            for k in ("base", "base_error", "base_pso"):
                if k in row:
                    new[k] = row[k]
            if r["status"] != "ok":
                new["error"] = r.get("error", "")[:300]
            else:
                lib = os.path.join(cas, r["key"][:2], r["key"] + ".metallib")
                refl = pr.run(P.job_for(r, lib, tmp), timeout=300)
                new["pso"] = refl.get("result")
                if refl.get("result") != "OK":
                    new["pso_error"] = refl.get("error", "")[:300]
                else:
                    got, problems = check_split(meta, r, refl)
                    new["discrete_sets"] = sorted(s_ - 1 for s_ in discrete_buffers(refl.get("bindings")))
                    new["reflected_bindings"] = got
                    new["declared_unused"] = sorted(set(meta.get("bindings", [])) - set(got))
                    new["problems"] = problems
                if "base" not in new and r["status"] == "ok":
                    blib, bplan, berr = base_compile(spv_path, tmp, hashlib.sha256(stem.encode()).hexdigest())
                    if blib:
                        brefl = pr.run(P.job_for({**r, "plan": bplan}, blib, tmp), timeout=300)
                        if brefl.get("result") == "OK":
                            new["base"] = check_base(meta, brefl, bplan)
                        else:
                            new["base_pso"] = brefl.get("result")
                    elif berr:
                        new["base_error"] = berr
            row = new
        if row.get("status") == "ok" and row.get("pso") == "OK":
            row["problems"] = subset_problems(row)
            declared, used = binding_refs(spv_path)
            row["spirv_declared"], row["spirv_referenced"] = declared, used
            unrefl = set(row["index_bindings"]) - set(row["reflected_bindings"])
            row["declared_unused"] = sorted(unrefl)
            bad = sorted(unrefl & set(used))
            if bad:
                row["problems"].append(f"bindings {bad} referenced in SPIR-V but not reflected")
            row["referenced_not_reflected"] = bad
            row["split_ok"] = not row["problems"]
            summ[(st, "split_ok" if row["split_ok"] else "split_mismatch")] += 1
            if unrefl and not bad:
                summ[(st, "rows_with_unreferenced_unreflected_bindings")] += 1
            if row.get("discrete_sets"):
                summ[(st, "rows_with_descriptor_array_buffers")] += 1
        elif row.get("status") != "ok":
            summ[(st, "compile_fail")] += 1
        else:
            summ[(st, "pso_fail")] += 1
        if "base" in row:
            summ[(st, "base_overlap" if row["base"]["overlap"] else "base_no_overlap")] += 1
        rows.append(row)
    pr.close()
    os.replace(os.path.join(out, "verify_bindings.jsonl"), os.path.join(out, "verify_bindings.v1.jsonl"))
    with open(os.path.join(out, "verify_bindings.jsonl"), "w") as f:
        for row in rows:
            f.write(json.dumps(row) + "\n")
    res = {"n": len(rows), "rechecked": sum(1 for r in rows if r.get("rechecked")), "by_stage": {},
           "recheck_s": round(time.monotonic() - t0, 1)}
    for (st, k), n in sorted(summ.items()):
        res["by_stage"].setdefault(st, {})[k] = n
    res["by_stage_n"] = dict(collections.Counter(r["stage"] for r in rows))
    json.dump(res, open(os.path.join(out, "summary.json"), "w"), indent=1)
    print(json.dumps(res, indent=1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spirv-dir", required=True)
    ap.add_argument("--build", required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--recheck", action="store_true")
    ap.add_argument("--recheck-all", action="store_true", help="with --recheck: rebuild and reflect every row")
    a = ap.parse_args()
    if a.recheck:
        return recheck(a, json.load(open(os.path.join(a.spirv_dir, "index.json"))))
    out = os.path.join(a.build, "verify_bindings")
    os.makedirs(out, exist_ok=True)
    cas = os.path.join(a.build, "cas")
    index = json.load(open(os.path.join(a.spirv_dir, "index.json")))
    pick = sample(index)
    tr = S.translator_id(a.spirv_dir)
    stems = sorted(pick)
    json.dump({"seed": SEED, "quota": QUOTA, "stems": {k: pick[k] for k in stems}}, open(os.path.join(out, "sample.json"), "w"), indent=0)
    print(f"sample {len(stems)}: " + str(collections.Counter(index[k]["stage"] for k in stems)), flush=True)
    tmp = os.path.join(out, "tmp")
    os.makedirs(tmp, exist_ok=True)
    t0 = time.monotonic()

    def comp(stem):
        spv_path = os.path.join(a.spirv_dir, stem + ".spv")
        st = index[stem]["stage"]
        r = S.compile_one(spv_path, st, cas, tr, {"vs_route": "lane"}, tmp=os.path.join(tmp, "w%d" % threading.get_ident()))
        blib, bplan, berr = (None, None, None)
        if st != "gs":
            blib, bplan, berr = base_compile(spv_path, tmp, hashlib.sha256(stem.encode()).hexdigest())
        return stem, r, blib, bplan, berr

    with cf.ThreadPoolExecutor(a.jobs) as ex:
        comps = list(ex.map(comp, stems))
    t1 = time.monotonic()
    print(f"compiled in {t1 - t0:.0f} s", flush=True)
    pr = P.Probe()
    rows = []
    summ = collections.Counter()
    for stem, r, blib, bplan, berr in comps:
        meta = index[stem]
        st = meta["stage"]
        row = {"stem": stem, "stage": st, "why": pick[stem], "index_bindings": meta.get("bindings", []),
               "status": r["status"], "key": r["key"]}
        if r["status"] != "ok":
            row["error"] = r.get("error", "")[:300]
            summ[(st, "compile_fail")] += 1
            rows.append(row)
            continue
        lib = os.path.join(cas, r["key"][:2], r["key"] + ".metallib")
        refl = pr.run(P.job_for(r, lib, tmp), timeout=300)
        row["pso"] = refl.get("result")
        if refl.get("result") != "OK":
            row["pso_error"] = refl.get("error", "")[:300]
            summ[(st, "pso_fail")] += 1
        else:
            got, problems = check_split(meta, r, refl)
            row["reflected_bindings"] = got
            row["declared_unused"] = sorted(set(meta.get("bindings", [])) - set(got))
            row["problems"] = problems
            row["split_ok"] = not problems
            summ[(st, "split_ok" if not problems else "split_mismatch")] += 1
        if blib:
            bj = P.job_for({**r, "plan": bplan}, blib, tmp)
            brefl = pr.run(bj, timeout=300)
            if brefl.get("result") == "OK":
                row["base"] = check_base(meta, brefl, bplan)
                summ[(st, "base_overlap" if row["base"]["overlap"] else "base_no_overlap")] += 1
            else:
                row["base_pso"] = brefl.get("result")
        elif berr:
            row["base_error"] = berr
        rows.append(row)
    pr.close()
    with open(os.path.join(out, "verify_bindings.jsonl"), "w") as f:
        for row in rows:
            f.write(json.dumps(row) + "\n")
    res = {"n": len(rows), "by_stage": {}, "compile_s": round(t1 - t0, 1), "probe_s": round(time.monotonic() - t1, 1)}
    for (st, k), n in sorted(summ.items()):
        res["by_stage"].setdefault(st, {})[k] = n
    res["by_stage_n"] = dict(collections.Counter(index[k]["stage"] for k in stems))
    json.dump(res, open(os.path.join(out, "summary.json"), "w"), indent=1)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    sys.exit(main())
