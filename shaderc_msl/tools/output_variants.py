#!/usr/bin/env python3
"""Pixel-shader output-type variants over the corpus and the title's pipeline lists (tools/out_types.py).

  scan    which pixel shaders need a variant: every one whose DXBC signature declares an integer SV_Target (the
          declared code), plus the codes the pipeline lists pair them with (pcache specs / manifests: rt formats);
          writes <out>/scan.json and the persistent list warmup/output_variants.tsv
  build   rebuilds those shaders' libraries with their variant entries into <out>/cas and writes <out>/manifest.tsv
          (the base manifest with those rows replaced; root = the base's) and <out>/overlay.tsv (the replaced rows
          only); --names limits the set, --first-lists builds the list-paired shaders first
          --also FILE adds shaders of any stage rebuilt with batch.py's options (e.g. the library compute shaders
          over 32 KiB of threadgroup memory that take shrink_lds since the sub-corpus DXBC lookup)
  specs   writes <out>/int-targets.specs: the list pipelines with an integer colour target (for prepare_shaders)
"""
import argparse, json, os, re, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import out_types  # noqa: E402
import shaderc_msl as S  # noqa: E402
import spv  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
MODULE = os.path.dirname(HERE)
UINT = {3, 7, 12, 17, 25, 30, 36, 42, 50, 57, 62}
SINT = {4, 8, 14, 18, 32, 38, 44, 52, 59, 63}


def decode(name):
    return re.sub(r"~([0-9a-f]{2})", lambda m: chr(int(m.group(1), 16)), name)


def list_pipelines(paths):
    """(kind, ps name, code, spec line) for every R/M pipeline with a pixel shader in the lists."""
    out = []
    for p in paths:
        for line in open(p):
            m = re.search(r"(?:^|\t)([RM]) (.*)$", line.rstrip("\n"))
            if not m:
                continue
            body = m.group(2)
            ps = re.search(r"\bps=(\S+)", body)
            rt = re.search(r"\brt=(\d+):([\d.]+)", body)
            if not ps or ps.group(1) == "-" or not rt:
                continue
            fm = [int(x) for x in rt.group(2).split(".")][:int(rt.group(1))]
            code = "".join("u" if f in UINT else "i" if f in SINT else "f" for f in fm).rstrip("f")
            out.append((m.group(1), decode(ps.group(1).split("@")[0]), code, m.group(1) + " " + body))
    return out


def declared_code(sig):
    if not sig:
        return ""
    n = max(sig) + 1
    return "".join({"uint": "u", "int": "i"}.get(sig.get(loc), "f") for loc in range(n)).rstrip("f")


def scan(a):
    import batch
    idx = batch.load_index(a.spirv_dir, "library,exe,fspx,bvh")
    pairs = list_pipelines(a.lists)
    listed = {}
    for kind, ps, code, _ in pairs:
        listed.setdefault(ps, set()).add(code)
    rows = {}
    for k, v in sorted(idx.items()):
        if v.get("stage") != "ps":
            continue
        sig = out_types.dxbc_targets(S.dxbc_of(os.path.join(a.spirv_dir, k + ".spv")))
        dc = declared_code(sig)
        lc = {c for c in listed.get(k, ()) if c}
        if not dc and not lc:
            continue
        w = spv.load(os.path.join(a.spirv_dir, k + ".spv"))
        info = out_types.analyze(w, sig)
        codes = sorted({c for c in lc | {dc} if c and out_types.needs_variant(info, c)})
        mism = sorted(c for c in lc if c and c != dc)
        rows[k] = {"declared": dc, "listed": sorted(lc), "codes": codes, "listed_other_than_declared": mism,
                   "info": {str(l): {kk: vv for kk, vv in r.items()} for l, r in info.items()},
                   "in_lists": k in listed}
    lossy = {k: r for k, r in rows.items()
             if any(r["info"].get(str(l), {}).get("lossy") for l, c in enumerate(r["declared"]) if c != "f")}
    numeric = {k: r for k, r in rows.items() if r["listed_other_than_declared"]}
    summary = {
        "pixel_shaders": sum(1 for v in idx.values() if v.get("stage") == "ps"),
        "with_integer_declared_target": sum(1 for r in rows.values() if r["declared"]),
        "need_variant": sum(1 for r in rows.values() if r["codes"]),
        "codes": {}, "list_pipelines_with_int_target": sum(1 for _k, _p, c, _ in pairs if c),
        "list_shaders_with_int_target": sum(1 for r in rows.values() if r["listed"]),
        "list_shaders_needing_variant": sum(1 for r in rows.values() if r["listed"] and r["codes"]),
        "integer_location_with_lossy_export": len(lossy),
        "listed_code_differs_from_declared": len(numeric),
    }
    for r in rows.values():
        for c in r["codes"]:
            summary["codes"][c] = summary["codes"].get(c, 0) + 1
    os.makedirs(a.out, exist_ok=True)
    json.dump({"summary": summary, "rows": rows}, open(os.path.join(a.out, "scan.json"), "w"), indent=1, sort_keys=True)
    with open(os.path.join(MODULE, "warmup", "output_variants.tsv"), "w") as f:
        f.write("# pixel shaders whose library needs output-type entries main0_o<code> (tools/output_variants.py scan)\n")
        f.write("# name\tcodes\tdeclared\tlisted\n")
        for k, r in sorted(rows.items()):
            if r["codes"]:
                f.write("%s\t%s\t%s\t%s\n" % (k, ",".join(r["codes"]), r["declared"] or "-", ",".join(r["listed"]) or "-"))
    print(json.dumps(summary, indent=1))
    for k, r in sorted(numeric.items()):
        print("listed code differs from the signature: %s declared %r listed %s" % (k, r["declared"], r["listed"]))
    for k in sorted(lossy)[:20]:
        print("lossy integer export: %s %s" % (k, json.dumps(lossy[k]["info"])))


def read_manifest(path):
    head, rows = [], {}
    for line in open(path):
        if line.startswith("#") and "\t" not in line:
            head.append(line)
            continue
        c = line.rstrip("\n").split("\t")
        rows[c[0]] = c
    return head, rows


def build(a):
    sc = json.load(open(os.path.join(a.out, "scan.json")))["rows"]
    names = [k for k, r in sc.items() if r["codes"]]
    if a.names:
        want = {l.strip() for l in open(a.names) if l.strip()}
        names = [k for k in names if k in want]
    if a.first_lists:
        names.sort(key=lambda k: (not sc[k]["in_lists"], k))
    if a.limit:
        names = names[:a.limit]
    extra = []
    if a.also:
        import batch
        index = batch.load_index(a.spirv_dir, "library,exe,fspx,bvh")
        extra = [l.strip() for l in open(a.also) if l.strip() and not l.startswith("#")]
    base_head, base_rows = read_manifest(a.base)
    base_dir = os.path.dirname(os.path.abspath(a.base))
    m = re.search(r"root=(\S+)", base_head[0])
    root = os.path.normpath(os.path.join(base_dir, m.group(1) if m else "."))
    cas = os.path.join(a.out, "cas")
    os.makedirs(cas, exist_ok=True)
    tr = S.translator_id(a.spirv_dir)
    dxbc = os.path.join(os.path.dirname(os.path.abspath(a.spirv_dir)), "dxbc")
    results_path = os.path.join(a.out, "build.jsonl")
    done = {}
    if os.path.exists(results_path):
        for line in open(results_path):
            r = json.loads(line)
            done[r["name"]] = r
    names = extra + [k for k in names if k not in extra]
    todo = [k for k in names if k not in done or done[k]["status"] != "ok"]
    print("output variants: %d shaders, %d to build" % (len(names), len(todo)), flush=True)

    def one(k):
        if k in sc:
            stage = "ps"
            opts = {"vs_route": "lane", "output_variants": sc[k]["codes"],
                    "output_signature": out_types.dxbc_targets(S.dxbc_of(os.path.join(a.spirv_dir, k + ".spv")))}
        else:
            import batch
            stage = index[k]["stage"]
            opts = batch.shader_opts(index, k, {}, dxbc)
        t0 = time.monotonic()
        r = S.compile_one(os.path.join(a.spirv_dir, k + ".spv"), stage, cas, tr, opts,
                          tmp=os.path.join(a.out, "tmp", str(os.getpid()) + "_" + str(abs(hash(k)) % 10**8)))
        return k, r, time.monotonic() - t0

    with open(results_path, "a") as log, ThreadPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(one, k) for k in todo]
        for n, f in enumerate(as_completed(futs), 1):
            k, r, dt = f.result()
            row = {"name": k, "status": r.get("status"), "key": r.get("key"), "error": r.get("error"),
                   "codes": sc[k]["codes"] if k in sc else [], "seconds": round(dt, 1),
                   "passes": [p["pass"] for p in r.get("passes", [])], "plan": r.get("plan"),
                   "variants": {c: {kk: v.get(kk) for kk in ("status", "report", "colors", "error")}
                                for c, v in (r.get("output_variants") or {}).items()}}
            done[k] = row
            log.write(json.dumps(row) + "\n")
            log.flush()
            print("[%d/%d] %s %s %.1f s %s" % (n, len(todo), row["status"], k, dt, row["error"] or ""), flush=True)
    rel_cas = os.path.relpath(cas, root)
    new_rows = {}
    for k, r in done.items():
        if r["status"] != "ok" or k not in base_rows:
            continue
        c = list(base_rows[k])
        c[1] = r["key"]
        c[4] = "%s/%s/%s.metallib" % (rel_cas, r["key"][:2], r["key"])
        plan = r.get("plan") or {}
        if plan.get("entry"):
            import batch
            sets = plan.get("sets") or {}
            c[5] = str(plan["push"]) if plan.get("push") is not None else "-"
            c[6] = ",".join("%d:%s" % (int(s) - 1, i) for s, i in sorted(sets.items(), key=lambda kv: int(kv[0]))) or "-"
            c[8] = ",".join(r["passes"]) or "-"
            c[9] = ",".join("%d:%s" % (int(kk) - 1, v) for kk, v in sorted(batch.discrete_of(plan).items(),
                                                                         key=lambda kv: int(kv[0]))) or "-"
        new_rows[k] = c
    hdr = re.sub(r"root=\S+", "root=" + os.path.relpath(root, os.path.abspath(a.out)), base_head[0])
    for k in a.rows or []:
        c = open(k).read().rstrip("\n").split("\t")
        new_rows[c[0]] = c
    mname = a.manifest_name
    with open(os.path.join(a.out, mname + ".part"), "w") as f:
        f.write(hdr)
        f.writelines(base_head[1:])
        for k, c in base_rows.items():
            f.write("\t".join(new_rows.get(k, c)) + "\n")
        for k in sorted(set(new_rows) - set(base_rows)):
            f.write("\t".join(new_rows[k]) + "\n")
    os.replace(os.path.join(a.out, mname + ".part"), os.path.join(a.out, mname))
    with open(os.path.join(a.out, "overlay.tsv"), "w") as f:
        f.write(hdr)
        f.writelines(base_head[1:])
        for k in sorted(new_rows):
            f.write("\t".join(new_rows[k]) + "\n")
    bad = {k: r for k, r in done.items() if r["status"] != "ok"}
    print("manifest: %d rows replaced; %d failed%s" % (len(new_rows), len(bad),
                                                       "".join("\n  %s: %s" % (k, r["error"]) for k, r in sorted(bad.items())[:20])))


def specs(a):
    with open(os.path.join(a.out, "int-targets.specs"), "w") as f:
        f.write("# list pipelines with an integer colour target (tools/output_variants.py specs)\n")
        seen = set()
        for kind, ps, code, line in list_pipelines(a.lists):
            if code and line not in seen:
                seen.add(line)
                f.write("world\t%s\n" % line)
    print("%d specs" % len(seen))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["scan", "build", "specs"])
    ap.add_argument("--spirv-dir", default=os.path.join(MODULE, "..", "..", "spike/s2-make-game/work/shaders/spirv"))
    ap.add_argument("--lists", nargs="*", default=[])
    ap.add_argument("--out", required=True)
    ap.add_argument("--base", default=os.path.join(MODULE, "..", "build/shaderc_msl/runs/main/manifest.tsv"))
    ap.add_argument("--manifest-name", default="manifest.tsv")
    ap.add_argument("--rows", nargs="*", help="files holding one ready manifest row each (shaders the base lacks)")
    ap.add_argument("--names")
    ap.add_argument("--also", help="file of further shader names (any stage) rebuilt with the batch's options, "
                                   "e.g. library compute shaders that now take shrink_lds")
    ap.add_argument("--first-lists", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=2)
    a = ap.parse_args()
    a.spirv_dir = os.path.abspath(a.spirv_dir)
    a.out = os.path.abspath(a.out)
    {"scan": scan, "build": build, "specs": specs}[a.cmd](a)


if __name__ == "__main__":
    main()
