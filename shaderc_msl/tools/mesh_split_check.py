#!/usr/bin/env python3
"""Output comparison of the meshlet split against the unsplit shader run as compute.

For each shader: (a) the translator's `--mesh-compute` module (the unsplit program as a compute kernel writing counts,
vertices and primitives to a buffer) is built and run on a patterned heap; (b) the SPIR-V split (capture variant,
chunks = 7, vmax = pmax = 64) is built and run as an object + mesh pipeline on the same heap and push constants, and
mirrors every output store into a buffer. The set of triangles (three corner records of position and every user
parameter location the compute module exports, plus the null-primitive flag) of each group must be equal as
multisets. Triangles that reference a vertex index at or past the vertex count are excluded on both sides (the
original leaves those slots unwritten).

  mesh_split_check.py --stems stems.txt --work DIR [--groups 24] [--pattern rand] [--seed 1] [--heap-mb 1100]
"""
import argparse, collections, json, os, re, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shaderc_msl as S  # noqa: E402
import spv  # noqa: E402
import mesh_split as M  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
CORPUS = os.path.join(ROOT, "spike/s2-make-game/work/shaders")
TRANSLATOR = os.path.join(ROOT, "spike/s2-make-game/tree/tools/rdna2spirv.py")
RUN = os.path.join(ROOT, "port/build/shaderc_msl/bin2/mesh_run")
PY314 = "/opt/homebrew/bin/python3.14"


def gta6rt_running():
    r = subprocess.run(["ps", "-axo", "comm="], capture_output=True, text=True).stdout
    return any(os.path.basename(x.strip()) == "gta6rt" for x in r.splitlines())


def wait_for_gpu_free(poll=0.25, limit=6 * 3600, quiet=0.0):
    """Returns when no gta6rt process exists (and none has for `quiet` seconds)."""
    t0 = time.time()
    free_since = None
    while True:
        if gta6rt_running():
            free_since = None
        else:
            free_since = free_since or time.time()
            if time.time() - free_since >= quiet:
                return
        if time.time() - t0 > limit:
            raise RuntimeError("gta6rt still running")
        time.sleep(poll)


def cross(spv_path, msl_path, extra=()):
    r = subprocess.run([S.SPIRV_CROSS, spv_path, *S.RECIPE["cross"], "--msl-version", str(S.RECIPE["msl_version"]), *extra,
                        "--output", msl_path], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("spirv-cross: " + r.stderr.strip()[:300])
    return open(msl_path).read()


def metallib(msl_path, base):
    r = subprocess.run([S.tool("metal"), "-std=" + S.RECIPE["std"], "-c", "-w", msl_path, "-o", base + ".air"],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("metal: " + (next((l for l in r.stderr.splitlines() if "error" in l), r.stderr))[:300])
    r = subprocess.run([S.tool("metallib"), base + ".air", "-o", base + ".metallib"], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("metallib: " + r.stderr[:300])
    return base + ".metallib"


class Harness:
    def __init__(self):
        self.p = None

    def start(self):
        self.p = subprocess.Popen([RUN], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def job(self, j, timeout=60, gate=True):
        if self.p is None or self.p.poll() is not None:
            self.start()
        if gate:
            wait_for_gpu_free()
        self.p.stdin.write(json.dumps(j) + "\n")
        self.p.stdin.flush()
        import select
        r, _, _ = select.select([self.p.stdout], [], [], timeout)
        if not r:
            self.p.kill()
            self.p = None
            return {"result": "WATCHDOG_TIMEOUT"}
        line = self.p.stdout.readline()
        if not line:
            self.p = None
            return {"result": "PROCESS_DIED"}
        return json.loads(line)


def build_split(stem, work, chunks, capture, vmax=64, pmax=64, opts=None, mutant=False):
    w = spv.load(os.path.join(CORPUS, "spirv", stem + ".spv"))
    passes = []
    if opts:
        w, passes, _ = S.prepare(w, "ms", opts)
    else:
        w, _ = spv.split_bindings(w)
    mesh, task, rep = M.split_mesh(w, chunks, vmax, pmax, capture=capture, mutant=mutant)
    ok, msg = M.validate(mesh)
    if not ok:
        raise RuntimeError("spirv-val: " + msg)
    tag = re.sub(r"[^A-Za-z0-9_.~-]", "_", stem) + (".cap" if capture else "") + (".mut" if mutant else "") + f".k{chunks}"
    base = os.path.join(work, tag)
    spv.save(mesh, base + ".mesh.spv")
    spv.save(task, base + ".task.spv")
    extra = ["--msl-device-argument-buffer", str(M.CAPTURE_SET)] if capture else []
    msl = cross(base + ".mesh.spv", base + ".mesh.metal", extra)
    if passes:
        fixed, _ = S.msl_fixups(msl, passes)
        if fixed is not None:
            msl = fixed
            open(base + ".mesh.metal", "w").write(msl)
    mlib = metallib(base + ".mesh.metal", base + ".mesh")
    tmsl = cross(base + ".task.spv", base + ".task.metal")
    tlib = metallib(base + ".task.metal", base + ".task")
    plan = S.msl_plan(msl)
    return {"mesh": mlib, "task": tlib, "plan": plan, "rep": rep, "msl": msl}


def build_ref(stem, work, ref_dir):
    p = os.path.join(ref_dir, stem + ".spv")
    w = spv.load(p)
    w, _ = spv.split_bindings(w)
    tag = re.sub(r"[^A-Za-z0-9_.~-]", "_", stem) + ".ref"
    base = os.path.join(work, tag)
    spv.save(w, base + ".spv")
    msl = cross(base + ".spv", base + ".metal")
    lib = metallib(base + ".metal", base)
    return {"lib": lib, "plan": S.msl_plan(msl)}


def make_refs(stems, work):
    src = os.path.join(work, "refsrc")
    out = os.path.join(work, "refout")
    os.makedirs(src, exist_ok=True)
    todo = [s for s in stems if not os.path.exists(os.path.join(out, s + ".spv"))]
    for s in todo:
        d = os.path.join(src, s + ".s")
        if not os.path.lexists(d):
            os.symlink(os.path.join(CORPUS, "dxbc/disasm", s + ".s"), d)
    if todo:
        env = dict(os.environ, LLVM_MC="/opt/homebrew/opt/llvm/bin/llvm-mc")
        r = subprocess.run([PY314, TRANSLATOR, src, "--out", out, "--no-asm", "--wave64-emulate", "--f16-interp-fallback",
                            "--mesh-compute"], capture_output=True, text=True, env=env, cwd=work)
        if r.returncode:
            raise RuntimeError("translator: " + r.stderr[-400:])
    return out, json.load(open(os.path.join(out, "index.json")))


def cap_binding(plan):
    if M.CAPTURE_SET in plan["sets"]:
        return {"cap_index": plan["sets"][M.CAPTURE_SET]}
    for ob in plan["other_buffers"]:
        if ob["param"].startswith("device ") and "spvDescriptor" not in ob["param"]:
            return {"cap_index": ob["index"], "cap_direct": True}
    raise RuntimeError("capture buffer not found in the plan")


def decode_ref(buf, lay, groups):
    sb = lay["slot_bytes"]
    res = []
    for g in range(groups):
        base = g * sb
        V, P = struct.unpack_from("<2I", buf, base)
        verts = []
        for i in range(min(V, lay["max_vertices"])):
            o = base + lay["vertices"] + i * lay["vertex_stride"]
            pos = struct.unpack_from("<4I", buf, o)
            prm = {int(loc): struct.unpack_from("<4I", buf, o + off) for loc, off in lay["params"].items()}
            verts.append((pos, prm))
        prims = []
        for p in range(min(P, lay["max_primitives"])):
            o = base + lay["primitives"] + p * lay["primitive_stride"]
            prims.append(struct.unpack_from("<4I", buf, o))
        res.append((V, P, verts, prims))
    return res


def decode_split(buf, rep, groups, chunks):
    rw, vs, vmax, pmax = rep["rec_words"], rep["vs_words"], rep["vmax"], rep["pmax"]
    words = struct.unpack("<%dI" % (len(buf) // 4), buf)
    vmap = rep["vmap"]
    res = []
    for g in range(groups):
        recs = []
        for c in range(chunks):
            b = (g * chunks + c) * rw
            vc, pc, cur, V, P = words[b:b + 5]
            verts = []
            for s in range(min(vc, vmax)):
                o = b + 8 + s * vs
                pos, prm = None, {}
                orig = words[o + vs - 1]
                for m in vmap:
                    d = tuple(words[o + m["offset"]:o + m["offset"] + m["comps"]])
                    if m["builtin"] == "Position":
                        pos = d
                    elif m["location"] is not None:
                        prm[m["location"]] = d
                    elif m["builtin"] is None and m["member"] == 0:
                        pos = d
                verts.append((pos, prm, orig))
            prims = []
            for j in range(min(pc, pmax)):
                o = b + 8 + vmax * vs + j * 8
                prims.append(tuple(words[o:o + 7]))
            recs.append({"vc": vc, "pc": pc, "cur": cur, "V": V, "P": P, "verts": verts, "prims": prims})
        res.append(recs)
    return res


def corner_key(v, locs):
    pos, prm = v[0], v[1]
    return (pos,) + tuple(prm.get(l) for l in locs)


def compare(ref, split, locs):
    stats = collections.Counter()
    bad = []
    for g, (rr, sr) in enumerate(zip(ref, split)):
        V, P, verts, prims = rr
        want = collections.Counter()
        for (i0, i1, i2, null) in prims:
            if max(i0, i1, i2) >= V or max(i0, i1, i2) >= len(verts):
                stats["ref_prims_invalid_index"] += 1
                continue
            want[tuple(corner_key(verts[i], locs) for i in (i0, i1, i2)) + (null,)] += 1
        got = collections.Counter()
        got_idx = collections.Counter()
        want_idx = collections.Counter((i0, i1, i2, null) for (i0, i1, i2, null) in prims if max(i0, i1, i2) < V)
        hdr_ok = True
        wrong = 0
        cur_max = 0
        for rec in sr:
            hdr_ok &= rec["V"] == V and rec["P"] == P
            cur_max = max(cur_max, rec["cur"] - 1)
            for pr in rec["prims"]:
                n0, n1, n2, o0, o1, o2, cull = pr
                if max(o0, o1, o2) >= V:
                    stats["split_prims_invalid_index"] += 1
                    continue
                if max(n0, n1, n2) >= len(rec["verts"]):
                    stats["split_prim_slot_out_of_range"] += 1
                    continue
                if tuple(rec["verts"][i][2] for i in (n0, n1, n2)) != (o0 & 127, o1 & 127, o2 & 127):
                    stats["slot_maps_to_wrong_vertex"] += 1
                    wrong += 1
                    continue
                stats["slot_maps_checked"] += 1
                got[tuple(corner_key(rec["verts"][i], locs) for i in (n0, n1, n2)) + (cull,)] += 1
                got_idx[(o0, o1, o2, cull)] += 1
        stats["groups"] += 1
        stats["header_V_P_equal"] += hdr_ok
        stats["ref_prims"] += sum(want.values())
        stats["chunks_needed_" + str(cur_max + 1 if P else 0)] += 1
        if want == got and want_idx == got_idx and not wrong:
            stats["groups_equal"] += 1
        else:
            stats["groups_differ"] += 1
            if len(bad) < 3:
                bad.append({"group": g, "V": V, "P": P, "want": sum(want.values()), "got": sum(got.values()),
                            "missing": sum((want - got).values()), "extra": sum((got - want).values())})
    return stats, bad


def perf(h, a, stem, ref, lay):
    out = {}
    h.job({"mode": "ref", "lib": ref["lib"], "fn": "main0", "groups": [a.groups, 1, 1], "push_bytes": 152,
           "push_index": ref["plan"]["push"], "pattern": a.pattern, "seed": a.seed, "heap_mb": a.heap_mb,
           "out_bytes": lay["slot_bytes"] * a.groups, "warm": True}, gate=False)
    rr = h.job({"mode": "ref", "lib": ref["lib"], "fn": "main0", "groups": [a.groups, 1, 1], "push_bytes": 152,
                "push_index": ref["plan"]["push"], "pattern": a.pattern, "seed": a.seed, "heap_mb": a.heap_mb,
                "out_bytes": lay["slot_bytes"] * a.groups})
    out["ref_gpu_ms"] = rr.get("gpu_ms")
    for chunks, vmax in ((2, 100), (3, 64)):
        try:
            sp = build_split(stem, a.work, chunks, False, vmax, 128)
        except Exception as e:  # noqa: BLE001
            out[f"k{chunks}"] = {"error": str(e)[:150]}
            continue
        j = {"mode": "split", "lib": sp["mesh"], "fn": "main0", "obj_lib": sp["task"], "obj_fn": "main0",
             "groups": [a.groups, 1, 1], "push_bytes": 144, "push_index": sp["plan"]["push"], "pattern": a.pattern,
             "seed": a.seed, "heap_mb": a.heap_mb}
        w = h.job(dict(j, warm=True), gate=False)
        if w.get("result") != "OK":
            out[f"k{chunks}"] = {"pso": w.get("error", "")[:150]}
            continue
        r = h.job(j)
        out[f"k{chunks}"] = {"vmax": vmax, "gpu_ms": r.get("gpu_ms"), "result": r.get("result")}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stems", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--groups", type=int, default=24)
    ap.add_argument("--pattern", default="rand")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--heap-mb", type=int, default=1100)
    ap.add_argument("--out", default="")
    ap.add_argument("--ref-only", action="store_true")
    ap.add_argument("--perf", action="store_true", help="time the production split (chunks 2 or 3, no capture) against the unsplit compute run")
    ap.add_argument("--mutant", action="store_true", help="negative control: the split writes a wrong index (must DIFFER)")
    ap.add_argument("--patterns", default="", help="ref-only: comma list of patterns run back to back after one gate wait")
    a = ap.parse_args()
    a.work = os.path.abspath(a.work)
    os.makedirs(a.work, exist_ok=True)
    stems = [x.strip() for x in open(a.stems) if x.strip()]
    ref_dir, ref_index = make_refs(stems, a.work)
    h = Harness()
    results = []
    for stem in stems:
        row = {"stem": stem}
        try:
            ref = build_ref(stem, a.work, ref_dir)
            lay = ref_index[stem]["mesh_compute"]
            if a.perf:
                row.update(perf(h, a, stem, ref, lay))
                row["verdict"] = "PERF"
                results.append(row)
                print(json.dumps(row), flush=True)
                if a.out:
                    open(a.out, "a").write(json.dumps(row) + "\n")
                continue
            sp = build_split(stem, a.work, 7, True, mutant=a.mutant)
            locs = sorted(int(k) for k in lay["params"])
            ref_out = os.path.join(a.work, "ref.%d.bin" % os.getpid())
            split_out = os.path.join(a.work, "split.%d.bin" % os.getpid())
            rjob = {"mode": "ref", "lib": ref["lib"], "fn": "main0", "groups": [a.groups, 1, 1], "push_bytes": 152,
                    "push_index": ref["plan"]["push"], "pattern": a.pattern, "seed": a.seed, "heap_mb": a.heap_mb,
                    "out_bytes": lay["slot_bytes"] * a.groups, "out_path": ref_out}
            if a.ref_only and a.patterns:
                wait_for_gpu_free(quiet=0.0)
                res = {}
                for pat in a.patterns.split(","):
                    j2 = dict(rjob, pattern=pat)
                    r_ = h.job(j2, gate=False)
                    if r_.get("result") == "OK":
                        try:
                            d = decode_ref(open(ref_out, "rb").read(), lay, a.groups)
                        except Exception as e_:  # noqa: BLE001
                            res[pat] = {"decode_error": str(e_)[:100], "lay": [lay["slot_bytes"], lay["vertices"], lay["vertex_stride"], lay["primitives"]]}
                            continue
                        res[pat] = {"V_P": [(g[0], g[1]) for g in d][:6],
                                    "valid_prims": sum(1 for g in d for p in g[3] if max(p[:3]) < g[0]), "max_V": max(g[0] for g in d)}
                    else:
                        res[pat] = r_
                row["patterns"] = res
                row["verdict"] = "REF_ONLY"
                print(json.dumps(row), flush=True)
                continue
            h.job(dict(rjob, warm=True), gate=False)
            r1 = h.job(rjob)
            row["ref_run"] = r1.get("result")
            if a.ref_only:
                if r1.get("result") == "OK":
                    d = decode_ref(open(ref_out, "rb").read(), lay, a.groups)
                    row["V_P"] = [(g[0], g[1]) for g in d]
                    row["valid_prims"] = sum(1 for g in d for p in g[3] if max(p[:3]) < g[0])
                row["verdict"] = "REF_ONLY"
                print(json.dumps(row), flush=True)
                continue
            sjob = {"mode": "split", "lib": sp["mesh"], "fn": "main0", "obj_lib": sp["task"], "obj_fn": "main0",
                    "groups": [a.groups, 1, 1], "push_bytes": 144, "push_index": sp["plan"]["push"],
                    **cap_binding(sp["plan"]), "pattern": a.pattern, "seed": a.seed,
                    "heap_mb": a.heap_mb, "out_bytes": sp["rep"]["rec_words"] * 4 * 7 * a.groups, "out_path": split_out}
            h.job(dict(sjob, warm=True), gate=False)
            r2 = h.job(sjob)
            row["split_run"] = r2.get("result")
            row["gpu_ms"] = {"ref": r1.get("gpu_ms"), "split": r2.get("gpu_ms")}
            if r1.get("result") == "OK" and r2.get("result") == "OK":
                ref_d = decode_ref(open(ref_out, "rb").read(), lay, a.groups)
                split_d = decode_split(open(split_out, "rb").read(), sp["rep"], a.groups, 7)
                st, bad = compare(ref_d, split_d, locs)
                row["stats"] = dict(st)
                row["bad"] = bad
                row["verdict"] = "EQUAL" if st["groups_differ"] == 0 and st["groups"] == a.groups else "DIFFER"
                row["V_P_sample"] = [(g[0], g[1]) for g in ref_d[:8]]
            else:
                row["verdict"] = "RUN_FAIL"
                row["errors"] = [r1.get("error"), r2.get("error")]
        except Exception as e:  # noqa: BLE001
            row["verdict"] = "ERROR"
            row["error"] = str(e)[:400]
        results.append(row)
        print(json.dumps(row), flush=True)
        if a.out:
            open(a.out, "a").write(json.dumps(row) + "\n")


if __name__ == "__main__":
    main()
