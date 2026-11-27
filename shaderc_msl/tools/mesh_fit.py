#!/usr/bin/env python3
"""Mesh output limits: how many over-limit mesh shaders each step rescues.

Metal limits (measured in spike/s5.2, rule checked 400/400 on real PSOs): mesh size = V x 16 B x ceil(s / 4) <= 32,768
with s the per-vertex scalars of the MSL vertex struct and V the declared vertex count; s <= 124 unique scalars.

Steps, applied to every mesh shader in order (each step only to those still over a limit):
  0 as translated
  1 dead-output elimination by location: a user output no paired pixel shader reads is removed
  2 component trim: each kept output shrinks to the highest component any paired pixel shader reads
     (Metal links a narrower mesh output to a wider fragment input; measured), and the MSL vertex struct members
     are sorted by alignment so float2/float pack (Metal's mesh size is the struct layout end, measured)
  3 meshlet split to 64 vertices / 64 primitives (size limit halves per meshlet; the 124-scalar limit stays)
  4 the rest: flagged for mesh-as-compute
Pairing comes from the title's pipeline library (tools/pipelines.py, pairs.json): a mesh shader paired with no
pixel shader in any record has only position live (depth-only pipelines); a mesh shader in no record at all has
unknown readers, so steps 1 and 2 cannot apply to it.
Writes <out>/mesh_fit.json (summary) and mesh_fit.jsonl (per shader: step, drop list, trimmed widths).
"""
import argparse, collections, concurrent.futures as cf, json, math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spv  # noqa: E402

BI_POSITION, BI_POINTSIZE, BI_CLIP, BI_CULL = 0, 1, 3, 4
PER_PRIM_BUILTINS = {5294, 5295, 5296, 5299, 7, 9, 10, 4432}
OP_COMPOSITE_EXTRACT, OP_VECTOR_SHUFFLE = 81, 79


def mesh_outputs(path):
    m = spv.Module(spv.load(path))
    outs = []
    for d in m.interface():
        if d["sc"] != "out" or d["per_prim"]:
            continue
        if d.get("members"):
            for mm in d["members"]:
                outs.append({"location": None, "builtin": mm["builtin"] if mm["builtin"] is not None else -1, "scalars": mm["scalars"]})
            continue
        outs.append({"location": d["location"], "builtin": d["builtin"], "scalars": d["scalars"]})
    decl = None
    for em in m.exec_modes:
        if em[1] == 26:
            decl = (decl or [None, None])
            decl[0] = em[2]
        elif em[1] == 5270:
            decl = (decl or [None, None])
            decl[1] = em[2]
    return outs, decl


def ps_reads(path):
    """Per input location: highest component read + 1 (4 when the use cannot be resolved)."""
    words = spv.load(path)
    m = spv.Module(words)
    loc_of = {d["id"]: d["location"] for d in m.interface() if d["sc"] == "in" and d["builtin"] is None}
    width = {loc: 0 for loc in loc_of.values()}
    loaded = {}
    chains = {}
    for i, op, n in spv.insts(words):
        if op in (spv.OP_ACCESS_CHAIN, spv.OP_IN_BOUNDS_ACCESS_CHAIN) and words[i + 3] in loc_of:
            idx = words[i + 4] if n > 4 else None
            c = m.consts.get(idx)
            chains[words[i + 2]] = (loc_of[words[i + 3]], c[1][0] if c else None)
        elif op == spv.OP_LOAD:
            src = words[i + 3]
            if src in loc_of:
                loaded[words[i + 2]] = loc_of[src]
            elif src in chains:
                loc, comp = chains[src]
                width[loc] = max(width[loc], 4 if comp is None else comp + 1)
    for i, op, n in spv.insts(words):
        if op == OP_COMPOSITE_EXTRACT:
            v = words[i + 3]
            if v in loaded:
                width[loaded[v]] = max(width[loaded[v]], words[i + 4] + 1)
            continue
        if op == OP_VECTOR_SHUFFLE:
            a, b = words[i + 3], words[i + 4]
            comps = words[i + 5:i + n]
            na = None
            if a in loaded:
                na = 4
                for c in comps:
                    if c != 0xFFFFFFFF and c < na:
                        width[loaded[a]] = max(width[loaded[a]], c + 1)
            if b in loaded:
                for c in comps:
                    if c != 0xFFFFFFFF and c >= 4:
                        width[loaded[b]] = max(width[loaded[b]], c - 4 + 1)
            continue
        if op == spv.OP_LOAD:
            continue
        for k in range(1, n):
            v = words[i + k]
            if v in loaded:
                width[loaded[v]] = 4
    return width


def vertex_scalars(outs, drop=(), widths=None):
    s = 0
    for o in outs:
        if o["builtin"] is None:
            if o["location"] in drop:
                continue
            s += widths.get(o["location"], o["scalars"]) if widths else o["scalars"]
        elif o["builtin"] in PER_PRIM_BUILTINS:
            continue
        elif o["builtin"] == BI_CLIP:
            s += 2 * o["scalars"]
        else:
            s += o["scalars"]
    return s


def fits(s, nv):
    return nv * 16 * math.ceil(s / 4) <= 32768 and s <= 124


def packed_bytes(outs, drop, widths):
    """Per-vertex bytes of the MSL vertex struct with members sorted by alignment (the pack_mesh_struct post-pass):
    float4 and float3 take 16 B, float2 8 B, float and clip-distance arrays 4 B per scalar (measured: Metal sums
    the struct layout without rounding the end)."""
    b = 0
    for o in outs:
        if o["builtin"] is None:
            if o["location"] in drop:
                continue
            w = widths.get(o["location"], o["scalars"])
            b += 16 if w >= 3 else 4 * w
        elif o["builtin"] in PER_PRIM_BUILTINS:
            continue
        elif o["builtin"] == BI_CLIP:
            b += 8 * o["scalars"]
        else:
            b += 16 if o["scalars"] >= 3 else 4 * o["scalars"]
    return b


def fits_packed(b, s, nv):
    return nv * b <= 32768 and s <= 124


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--spirv-dir", required=True)
    ap.add_argument("--pairs", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--s52", default=None, help="spike/s5.2-msl-corpus, to check the scalar model against its MSL")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    index = json.load(open(os.path.join(a.spirv_dir, "index.json")))
    pairs = json.load(open(a.pairs))
    alias = pairs.get("digest_aliases", {})
    group = {}
    for first, rest in alias.items():
        for n in [first] + rest:
            group[n] = [first] + rest
    ms_ps = collections.defaultdict(set)
    ms_records = collections.Counter()
    for g in pairs["graphics"]:
        if not g.get("ms"):
            continue
        for msn in group.get(g["ms"], [g["ms"]]):
            ms_records[msn] += 1
            if g.get("ps"):
                ms_ps[msn].update(group.get(g["ps"], [g["ps"]]))
            else:
                ms_ps[msn].add(None)
    mss = sorted(k for k, v in index.items() if v["stage"] == "ms")
    ps_needed = sorted({p for k in mss for p in ms_ps.get(k, ()) if p})
    sp = lambda n: os.path.join(a.spirv_dir, n + ".spv")
    with cf.ProcessPoolExecutor(a.jobs) as ex:
        reads = dict(zip(ps_needed, ex.map(ps_reads, map(sp, ps_needed), chunksize=32)))
        outs = dict(zip(mss, ex.map(mesh_outputs, map(sp, mss), chunksize=32)))
    rows = []
    step_count = collections.Counter()
    detail = collections.Counter()
    for k in mss:
        o, decl = outs[k]
        nv = decl[0] if decl and decl[0] else index[k].get("max_vertices", 128)
        s0 = vertex_scalars(o)
        row = {"stem": k, "nv": nv, "s0": s0, "records": ms_records.get(k, 0)}
        user = [x["location"] for x in o if x["builtin"] is None]
        if fits(s0, nv):
            row["step"] = 0
        else:
            paired = ms_ps.get(k)
            row["paired_ps"] = sorted(p for p in paired if p) if paired else None
            if paired is not None:
                live = {}
                for p in paired:
                    if p is None:
                        continue
                    for loc, w in reads[p].items():
                        live[loc] = max(live.get(loc, 0), w)
                drop = sorted(l for l in user if live.get(l, 0) == 0)
                s1 = vertex_scalars(o, drop)
                widths = {l: live[l] for l in user if live.get(l, 0) > 0}
                s2 = vertex_scalars(o, drop, widths)
                b2 = packed_bytes(o, drop, widths)
                row.update({"drop": drop, "s1": s1, "widths": widths, "s2": s2, "b2": b2})
                if fits(s1, nv):
                    row["step"] = 1
                elif fits_packed(b2, s2, nv):
                    row["step"] = 2
            if "step" not in row:
                s_best = row.get("s2", s0)
                b_best = row.get("b2", 16 * math.ceil(s0 / 4))
                if fits_packed(b_best, s_best, 64):
                    row["step"] = 3
                else:
                    row["step"] = 4
                    row["why"] = "unique scalars > 124" if s_best > 124 else "size at 64 vertices"
            detail[("paired" if paired is not None else "unpaired", row["step"])] += 1
        step_count[row["step"]] += 1
        rows.append(row)
    check = None
    if a.s52:
        snap = json.load(open(os.path.join(a.s52, "snapshot.json")))
        sid = {s: i for i, s in enumerate(snap["stems"])}
        import mesh_scalars_check
        check = mesh_scalars_check.compare(a.s52, sid, {r["stem"]: r["s0"] for r in rows})
    over0 = sum(1 for r in rows if r["step"] > 0)
    res = {"mesh_shaders": len(mss), "fit_as_translated": step_count[0], "over_limit_as_translated": over0,
           "rescued_by_step": {"1_dead_output_elimination": step_count[1], "2_component_trim": step_count[2],
                               "3_meshlet_split_64": step_count[3]},
           "left_for_mesh_as_compute": step_count[4],
           "remaining_after_step": {"1": over0 - step_count[1], "2": over0 - step_count[1] - step_count[2],
                                    "3": step_count[4]},
           "over_limit_by_pairing": {f"{p} step {s}": n for (p, s), n in sorted(detail.items())},
           "why_left": dict(collections.Counter(r.get("why") for r in rows if r["step"] == 4)),
           "ps_analysed": len(ps_needed), "scalar_model_check": check}
    json.dump(res, open(os.path.join(a.out, "mesh_fit.json"), "w"), indent=1)
    with open(os.path.join(a.out, "mesh_fit.jsonl"), "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    sys.exit(main())
