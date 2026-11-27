#!/usr/bin/env python3
"""Overlay of rebuilt shaders (round 9 thread-memory work): each named shader is rebuilt from the corpus with the
options of its current catalog row plus --merge-typed-reads and/or --loop-cap, into <out>/cas, and written as an
overlay row (format of build/uintps-x86_64/overlay.tsv: the catalog row with the new key, metallib and passes).

  tmem_overlay.py --catalog build/pc7/catalog-r8.tsv --out build/tmem-x86_64 --merge NAMES --loop-cap NAMES [--cap 16384]
  NAMES: comma-separated catalog names (library/lib_cs_00968,...)
"""
import argparse, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shaderc_msl as S  # noqa: E402
import tmem_rank  # noqa: E402


def row_opts(meta):
    o = {}
    for p in meta.get("passes", []):
        if p["pass"] == "shrink_lds":
            o["shrink_lds"] = p["dwords"]
        elif p["pass"] == "x_buf_device":
            o["x_buf_device"] = True
        elif p["pass"] == "dual_source":
            o["dual_source"] = True
        elif p["pass"] in ("drop_outputs", "trim_outputs", "split_mesh", "output_variants"):
            raise SystemExit("%s: pass %s not reproduced by this tool" % (meta.get("spv"), p["pass"]))
    return o


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--catalog", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--corpus", default=tmem_rank.CORPUS)
    ap.add_argument("--merge", default="")
    ap.add_argument("--loop-cap", default="")
    ap.add_argument("--cap", type=int, default=1 << 14)
    ap.add_argument("--check-key", action="store_true", help="also rebuild without the new passes; the key must equal the row's")
    a = ap.parse_args()
    rows, base = tmem_rank.read_catalog(a.catalog)
    merge = [x for x in a.merge.split(",") if x]
    cap = [x for x in a.loop_cap.split(",") if x]
    cas = os.path.join(os.path.abspath(a.out), "cas")
    tr = S.translator_id(a.corpus)
    out_rows, report = [], []
    for name in dict.fromkeys(merge + cap):
        row = rows[name][-1]
        meta = json.load(open(os.path.normpath(os.path.join(base, row[4]))[:-len(".metallib")] + ".json"))
        opts = row_opts(meta)
        spv_path = os.path.join(a.corpus, name + ".spv")
        if a.check_key:
            r0 = S.compile_one(spv_path, row[2], cas, tr, dict(opts))
            if r0["key"] != row[1]:
                raise SystemExit("%s: rebuilt key %s != catalog %s" % (name, r0["key"], row[1]))
        if name in merge:
            opts["merge_typed_reads"] = True
        if name in cap:
            opts["loop_cap"] = a.cap
        r = S.compile_one(spv_path, row[2], cas, tr, opts)
        if r.get("status") != "ok":
            raise SystemExit("%s: %s %s" % (name, r.get("status"), r.get("error", "")))
        new = list(row)
        new[1] = r["key"]
        new[4] = os.path.relpath(os.path.join(cas, r["key"][:2], r["key"] + ".metallib"), base)
        new[8] = ",".join(p["pass"] for p in r["passes"] if p.get("level") != "msl" or p["pass"] in ("merge_typed_reads", "loop_cap"))
        out_rows.append("\t".join(new))
        report.append({"name": name, "old_key": row[1], "key": r["key"], "typed_ops_merged": r.get("typed_ops_merged"),
                       "loops_capped": r.get("loops_capped"), "metal_s": r.get("metal_s")})
        print(json.dumps(report[-1]), flush=True)
    head = [l for l in open(a.catalog) if l.startswith("#")][:2]
    with open(os.path.join(a.out, "overlay.tsv"), "w") as f:
        f.writelines(head)
        f.write("\n".join(out_rows) + "\n")
    json.dump(report, open(os.path.join(a.out, "overlay.json"), "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
