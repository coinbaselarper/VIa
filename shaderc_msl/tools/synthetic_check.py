#!/usr/bin/env python3
"""The meshlet split on a mesh shader whose inputs are controlled: tests/synthetic_mesh/synth_mesh.glsl (same epilogue
shape as the translator's mesh shaders: counts, per-lane vertex outputs, per-lane packed triangle) with meshlets
written into the heap by this script (strips, fans, random, shuffled, repeated indices, ...). The expected set of
output triangles (three corners of position and two user outputs, plus the cull flag) is computed here on the CPU and
compared with what the split mesh stage emitted (capture variant, chunks = 7). Also records how many chunks each
meshlet needs (the walk's final chunk number) for vmax 64 / pmax 64.

  synthetic_check.py --work DIR [--mutant] [--groups 16]
"""
import argparse, collections, json, os, random, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shaderc_msl as S  # noqa: E402
import spv  # noqa: E402
import mesh_split as M  # noqa: E402
import mesh_split_check as C  # noqa: E402

SRC = os.path.join(HERE, "..", "tests", "synthetic_mesh", "synth_mesh.glsl")
RUN = os.path.join(C.ROOT, "port/build/shaderc_msl/bin2/mesh_run_syn")
BLOCK = 65536
OFF = 0x1000000


def meshlet(kind, rnd):
    if kind == "strip":
        V, tris = 128, [(p, p + 1, p + 2) for p in range(126)]
    elif kind == "fan":
        V, tris = 128, [(0, p + 1, p + 2) for p in range(126)]
    elif kind == "random":
        V, tris = 128, [tuple(rnd.randrange(128) for _ in range(3)) for _ in range(128)]
    elif kind == "shuffled_strip":
        perm = list(range(128))
        rnd.shuffle(perm)
        V, tris = 128, [(perm[p], perm[p + 1], perm[p + 2]) for p in range(126)]
    elif kind == "small":
        V, tris = 10, [(p % 10, (p + 1) % 10, (p + 3) % 10) for p in range(8)]
    elif kind == "v64_random":
        V, tris = 64, [tuple(rnd.randrange(64) for _ in range(3)) for _ in range(128)]
    elif kind == "repeats":
        V = 100
        tris = []
        for p in range(100):
            i, j = rnd.randrange(V), rnd.randrange(V)
            tris.append(rnd.choice([(i, i, i), (i, j, i), (i, i, j), (j, i, i), (i, j, j)]))
    elif kind == "spread":
        V, tris = 128, [(p, (p + 43) % 128, (p + 86) % 128) for p in range(128)]
    elif kind == "clustered":
        V = 128
        tris = []
        for p in range(128):
            c = (p // 16) * 16
            tris.append(tuple(c + rnd.randrange(16) for _ in range(3)))
    elif kind == "cull_mix":
        V, tris = 128, [(p, p + 1, p + 2) for p in range(126)]
    else:
        raise ValueError(kind)
    cull = [1 if (kind == "cull_mix" and p % 3 == 0) else 0 for p in range(len(tris))]
    return V, tris, cull


KINDS = ["strip", "fan", "random", "shuffled_strip", "small", "v64_random", "repeats", "spread", "clustered", "cull_mix"]


def f32(x):
    return struct.unpack("<I", struct.pack("<f", float(x)))[0]


def build_heap_blob(groups, rnd):
    blob = bytearray(groups * BLOCK)
    meta = []
    for g in range(groups):
        kind = KINDS[g % len(KINDS)]
        V, tris, cull = meshlet(kind, rnd)
        words = [0] * (BLOCK // 4)
        words[0], words[1] = V, len(tris)
        for p, (a, b, c) in enumerate(tris):
            words[128 + p] = a | (b << 8) | (c << 16) | (cull[p] << 24)
        verts = []
        for lane in range(V):
            x, y, z = rnd.randrange(1, 1000), rnd.randrange(1, 1000), rnd.randrange(1, 1000)
            verts.append((x, y, z))
            words[512 + lane * 4], words[513 + lane * 4], words[514 + lane * 4] = f32(x), f32(y), f32(z)
        struct.pack_into("<%dI" % len(words), blob, g * BLOCK, *words)
        meta.append((kind, V, tris, cull, verts))
    return bytes(blob), meta


def expected(g, V, tris, cull, verts):
    def corner(i):
        x, y, z = verts[i]
        return (f32(x), f32(y), f32(z), f32(1.0), f32(x * 2.0), f32(y * 2.0), f32(z * 2.0), f32(2.0),
                f32(i), f32(g), f32(x + y), f32(7.0))
    out = collections.Counter()
    for (a, b, c), cl in zip(tris, cull):
        if max(a, b, c) < V:
            out[(corner(a), corner(b), corner(c), cl)] += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--groups", type=int, default=20)
    ap.add_argument("--mutant", action="store_true")
    ap.add_argument("--seed", type=int, default=9)
    ap.add_argument("--kinds", default="", help="comma list of meshlet kinds (default all), cycled over the groups")
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    a.work = os.path.abspath(a.work)
    os.makedirs(a.work, exist_ok=True)
    rnd = random.Random(a.seed)
    global KINDS
    if a.kinds:
        KINDS = a.kinds.split(",")
    base = os.path.join(a.work, "synth")
    subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.3", "-S", "mesh", SRC, "-o", base + ".spv"], check=True,
                   capture_output=True)
    subprocess.run(["spirv-opt", "-O", base + ".spv", "-o", base + ".opt.spv"], check=True)
    w = spv.load(base + ".opt.spv")
    w, _ = spv.split_bindings(w)
    mesh, task, rep = M.split_mesh(w, 7, 64, 64, capture=True, mutant=a.mutant)
    spv.save(mesh, base + ".mesh.spv")
    spv.save(task, base + ".task.spv")
    msl = C.cross(base + ".mesh.spv", base + ".mesh.metal")
    mlib = C.metallib(base + ".mesh.metal", base + ".mesh")
    C.cross(base + ".task.spv", base + ".task.metal")
    tlib = C.metallib(base + ".task.metal", base + ".task")
    plan = S.msl_plan(msl)
    cap = C.cap_binding(plan)
    blob, meta = build_heap_blob(a.groups, rnd)
    open(base + ".heap.bin", "wb").write(blob)
    h = C.Harness()
    h.start = lambda: setattr(h, "p", subprocess.Popen([RUN], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1))
    out_path = base + ".cap.bin"
    job = {"mode": "split", "lib": mlib, "fn": "main0", "obj_lib": tlib, "obj_fn": "main0", "groups": [a.groups, 1, 1],
           "push_bytes": 144, "push_index": plan["push"], **cap, "pattern": "const:0", "seed": 1, "heap_mb": 64,
           "out_bytes": rep["rec_words"] * 4 * 7 * a.groups, "out_path": out_path, "inject_file": base + ".heap.bin",
           "inject_off": OFF}
    h.job(dict(job, warm=True), gate=False)
    r = h.job(job)
    row = {"mutant": a.mutant, "run": r.get("result"), "error": r.get("error")}
    if r.get("result") == "OK":
        dec = C.decode_split(open(out_path, "rb").read(), rep, a.groups, 7)
        res = []
        for g, recs in enumerate(dec):
            kind, V, tris, cull, verts = meta[g]
            want = expected(g, V, tris, cull, verts)
            got = collections.Counter()
            wrong = 0
            need = 0
            for rec in recs:
                need = max(need, rec["cur"] if rec["P"] else 0)
                for pr in rec["prims"]:
                    n0, n1, n2, o0, o1, o2, cl = pr
                    if max(o0, o1, o2) >= V:
                        continue
                    if n0 >= len(rec["verts"]) or n1 >= len(rec["verts"]) or n2 >= len(rec["verts"]):
                        wrong += 1
                        continue
                    if tuple(rec["verts"][i][2] for i in (n0, n1, n2)) != (o0, o1, o2):
                        wrong += 1
                        continue
                    key = []
                    for i in (n0, n1, n2):
                        pos, prm = rec["verts"][i][0], rec["verts"][i][1]
                        key.append(tuple(pos) + tuple(prm.get(0, ())) + tuple(prm.get(1, ())))
                    got[(key[0], key[1], key[2], cl)] += 1
            ok = got == want and not wrong
            res.append({"group": g, "kind": kind, "V": V, "P": len(tris), "chunks_needed": need, "triangles": sum(want.values()),
                        "equal": ok, "wrong_slot_maps": wrong, "missing": sum((want - got).values()), "extra": sum((got - want).values())})
        row["groups"] = res
        row["all_equal"] = all(x["equal"] for x in res)
        row["chunks_needed_by_kind"] = {k: max((x["chunks_needed"] for x in res if x["kind"] == k), default=0) for k in KINDS}
    print(json.dumps(row))
    if a.out:
        open(a.out, "a").write(json.dumps(row) + "\n")


if __name__ == "__main__":
    main()
